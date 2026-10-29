#include "runtime/vm.h"

#include "bytecode/opcodes.h"
#include "runtime/debugger.h"
#include "runtime/method_callable.h"
#include "runtime/text.h"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/godot.hpp>

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>

using namespace godot;

namespace petrichor {

using bytecode::NO_SLOT;
using bytecode::Op;
using bytecode::ValueType;

InstanceStorage::InstanceStorage(const std::shared_ptr<const ClassImage> &p_image) :
		image(p_image), members(static_cast<uint8_t *>(::operator new(p_image->members_layout.size))) {
	image->members_layout.construct(members);
}

InstanceStorage::~InstanceStorage() {
	image->members_layout.destroy(members);
	::operator delete(members);
}

namespace {

// What a static function, which has no instance, reads as its instance's state.
const std::atomic<bool> NO_INSTANCE_FREED{ false };

// GDScript's default limit (debug/settings/gdscript/max_call_stack,
// modules/gdscript/gdscript.cpp:2870); deeper recursion is a program error
// (spec §8.1) instead of a crash.
constexpr size_t MAX_CALL_DEPTH = 1024;

// The frames of this language on this thread, innermost last, for error lines
// and the debugger. It never grows past its first reservation, so that a
// function's record stays put while it runs.
thread_local std::vector<StackFrame> call_stack;

// The memory of the frames of this language on this thread, taken and given
// back last in first out, from blocks that do not move.
class FrameStack {
	static constexpr size_t BLOCK_SIZE = 64 * 1024;
	struct Block {
		std::unique_ptr<uint8_t[]> data;
		size_t size = 0;
		size_t used = 0;
	};
	std::vector<Block> blocks;
	size_t current = 0;

	static size_t aligned(uint32_t p_size) { return (size_t(p_size) + 15) & ~size_t(15); }

public:
	uint8_t *push(uint32_t p_size) {
		const size_t size = aligned(p_size);
		while (current < blocks.size() && blocks[current].size - blocks[current].used < size) {
			current++;
		}
		if (current == blocks.size()) {
			const size_t block_size = std::max(BLOCK_SIZE, size);
			blocks.push_back(Block{ std::make_unique<uint8_t[]>(block_size), block_size, 0 });
		}
		Block &block = blocks[current];
		uint8_t *data = block.data.get() + block.used;
		block.used += size;
		return data;
	}

	void pop(uint32_t p_size) {
		blocks[current].used -= aligned(p_size);
		while (current > 0 && blocks[current].used == 0) {
			current--;
		}
	}
};

thread_local FrameStack frame_stack;

// A frame of a function, constructed and destroyed by its layout.
class Frame {
	const SlotLayout &layout;

public:
	uint8_t *const data;

	explicit Frame(const SlotLayout &p_layout) :
			layout(p_layout), data(frame_stack.push(p_layout.size)) {
		layout.construct(data);
	}
	~Frame() {
		layout.destroy(data);
		frame_stack.pop(layout.size);
	}
	Frame(const Frame &) = delete;
	Frame &operator=(const Frame &) = delete;
};

// Where an engine function reads a right operand that a unary operator does not have.
alignas(16) const uint8_t NOTHING[64] = {};

// The arguments of a call to the engine, from the instruction's (type, offset)
// pairs: each in its slot's own layout, or as a Variant where the function
// takes a Variant, for extra vararg arguments, or for every argument of a call
// by name. Kept on the C++ stack up to a few arguments.
class Arguments {
	static constexpr uint32_t INLINE = 8;
	const void *inline_pointers[INLINE];
	alignas(Variant) uint8_t inline_variants[INLINE * sizeof(Variant)];
	uint32_t variant_count = 0;
	std::vector<const void *> heap_pointers;
	std::vector<Variant> heap_variants;

public:
	const void **pointers;

	Arguments(uint8_t *p_frame, const uint32_t *p_pairs, uint32_t p_count, const std::vector<ValueType> *p_params) {
		if (p_count > INLINE) {
			heap_pointers.resize(p_count);
			heap_variants.reserve(p_count); // Pointers into it stay valid.
		}
		pointers = p_count > INLINE ? heap_pointers.data() : inline_pointers;
		for (uint32_t i = 0; i < p_count; i++) {
			const ValueType type = ValueType(p_pairs[2 * i]);
			uint8_t *slot = p_frame + p_pairs[2 * i + 1];
			if (p_params && i < p_params->size() && (*p_params)[i] != ValueType::Variant) {
				pointers[i] = slot;
			} else if (type == ValueType::Variant || type == ValueType::Object) {
				pointers[i] = slot; // Already a Variant.
			} else if (p_count > INLINE) {
				heap_variants.push_back(to_variant(type, slot));
				pointers[i] = &heap_variants.back();
			} else {
				pointers[i] = new (inline_variants + variant_count++ * sizeof(Variant)) Variant(to_variant(type, slot));
			}
		}
	}
	~Arguments() {
		for (uint32_t i = 0; i < variant_count; i++) {
			reinterpret_cast<Variant *>(inline_variants)[i].~Variant();
		}
	}
	Arguments(const Arguments &) = delete;
	Arguments &operator=(const Arguments &) = delete;

	const Variant **as_variants() { return reinterpret_cast<const Variant **>(pointers); }
};

void report_error(const ClassImage &p_image, const bytecode::Function &p_function, int p_line, const String &p_message) {
	vm::report_error(p_image, p_function.name.c_str(), p_line, p_message);
}

class Execution {
	const ClassImage &image;
	const InstanceContext &context;
	// Whether the engine's debugger is on, which does not change while the
	// engine runs (core/debugger/engine_debugger.h:106); asked once per call
	// from the engine, since asking is a call into the engine.
	const bool debugging;
	// self as a Variant, made when an instruction first needs it.
	alignas(Variant) uint8_t self_storage[sizeof(Variant)] = {};
	bool has_self = false;

	Variant &self() {
		if (!has_self) {
			new (self_storage) Variant(context.owner);
			has_self = true;
		}
		return variant_at(self_storage);
	}

public:
	Execution(const ClassImage &p_image, const InstanceContext &p_context, bool p_debugging) :
			image(p_image), context(p_context), debugging(p_debugging) {}
	~Execution() {
		if (has_self) {
			variant_at(self_storage).~Variant();
		}
	}
	Execution(const Execution &) = delete;
	Execution &operator=(const Execution &) = delete;

	// Runs a function on an initialized frame. Returns false after a program
	// error, which unwinds every frame of this language up to the engine.
	bool run(uint32_t p_function, uint8_t *frame, uint8_t *p_result) {
		const bytecode::Function &fn = image.module.functions[p_function];
		const bytecode::Module &module = image.module;
		const uint32_t *code = fn.code.data();
		uint8_t *members = context.storage ? context.storage->members : nullptr;
		const std::atomic<bool> &freed = context.storage ? context.storage->freed : NO_INSTANCE_FREED;
		if (call_stack.capacity() < MAX_CALL_DEPTH) {
			call_stack.reserve(MAX_CALL_DEPTH);
		}
		if (call_stack.size() == MAX_CALL_DEPTH) {
			const StackFrame &caller = call_stack.back();
			report_error(*caller.image, *caller.function, caller.line, vformat("Stack overflow: more than %d calls deep. Check for infinite recursion.", int64_t(MAX_CALL_DEPTH)));
			return false;
		}
		call_stack.push_back({ &image, &fn, frame, members, int(fn.line) });
		StackFrame &record = call_stack.back();
		if (debugging) {
			debugger::enter_function();
		}
		struct Pop {
			bool debugging;
			~Pop() {
				if (debugging) {
					debugger::exit_function();
				}
				call_stack.pop_back();
			}
		} pop{ debugging };

#define I64(m_offset) (*reinterpret_cast<int64_t *>(frame + (m_offset)))
#define U64(m_offset) (*reinterpret_cast<uint64_t *>(frame + (m_offset)))
#define F64(m_offset) (*reinterpret_cast<double *>(frame + (m_offset)))
#define STR(m_offset) (*reinterpret_cast<String *>(frame + (m_offset)))
#define FAIL(m_message) \
	do { \
		report_error(image, fn, record.line, (m_message)); \
		return false; \
	} while (false)
// Wrapping arithmetic on int, as in GDScript; through unsigned to stay defined.
#define WRAP(m_op) int64_t(uint64_t(I64(code[pc + 2])) m_op uint64_t(I64(code[pc + 3])))
// self and the members, unless the engine deleted the instance while this
// function ran (spec §3.3).
#define SELF_ALIVE() \
	if (freed.load(std::memory_order_relaxed)) { \
		FAIL("self was freed, or lost its script, while this method ran."); \
	}
// The object an instruction works on: self, or a live object in a slot (spec §3.3).
#define OBJECT(m_name, m_offset) \
	if ((m_offset) == NO_SLOT) { \
		SELF_ALIVE(); \
	} else if (!is_live_object(frame + (m_offset))) { \
		FAIL(is_none(variant_at(frame + (m_offset))) ? String("The value is None.") : String("The object was freed.")); \
	} \
	Variant &m_name = (m_offset) == NO_SLOT ? self() : variant_at(frame + (m_offset));

		size_t pc = 0;
		while (true) {
			switch (Op(code[pc])) {
				case Op::LINE:
					record.line = int(code[pc + 1]);
					if (debugging) {
						debugger::line(image.source, int(code[pc + 1]));
					}
					pc += 2;
					break;
				case Op::CONST_INT:
					I64(code[pc + 1]) = module.constants[code[pc + 2]].i;
					pc += 3;
					break;
				case Op::CONST_FLOAT:
					F64(code[pc + 1]) = module.constants[code[pc + 2]].f;
					pc += 3;
					break;
				case Op::CONST_BOOL:
					U64(code[pc + 1]) = code[pc + 2];
					pc += 3;
					break;
				case Op::CONST_STRING:
					STR(code[pc + 1]) = image.strings[code[pc + 2]];
					pc += 3;
					break;
				case Op::CONST_VALUE:
					from_variant(ValueType(code[pc + 1]), image.values[code[pc + 3]], frame + code[pc + 2]);
					pc += 4;
					break;
				case Op::MOVE_8:
					U64(code[pc + 1]) = U64(code[pc + 2]);
					pc += 3;
					break;
				case Op::MOVE_STRING:
					STR(code[pc + 1]) = STR(code[pc + 2]);
					pc += 3;
					break;
				case Op::MOVE_VALUE:
					copy_value(ValueType(code[pc + 1]), frame + code[pc + 2], frame + code[pc + 3]);
					pc += 4;
					break;
				case Op::LOAD_MEMBER_8:
					SELF_ALIVE();
					std::memcpy(frame + code[pc + 1], members + code[pc + 2], 8);
					pc += 3;
					break;
				case Op::LOAD_MEMBER_STRING:
					SELF_ALIVE();
					STR(code[pc + 1]) = *reinterpret_cast<String *>(members + code[pc + 2]);
					pc += 3;
					break;
				case Op::LOAD_MEMBER_VALUE:
					SELF_ALIVE();
					copy_value(ValueType(code[pc + 1]), frame + code[pc + 2], members + code[pc + 3]);
					pc += 4;
					break;
				case Op::STORE_MEMBER_8:
					SELF_ALIVE();
					std::memcpy(members + code[pc + 1], frame + code[pc + 2], 8);
					pc += 3;
					break;
				case Op::STORE_MEMBER_STRING:
					SELF_ALIVE();
					*reinterpret_cast<String *>(members + code[pc + 1]) = STR(code[pc + 2]);
					pc += 3;
					break;
				case Op::STORE_MEMBER_VALUE:
					SELF_ALIVE();
					copy_value(ValueType(code[pc + 1]), members + code[pc + 2], frame + code[pc + 3]);
					pc += 4;
					break;
				case Op::INT_TO_FLOAT:
					F64(code[pc + 1]) = double(I64(code[pc + 2]));
					pc += 3;
					break;
				case Op::FLOAT_TO_INT: {
					const double value = F64(code[pc + 2]);
					if (!(value >= -9223372036854775808.0 && value < 9223372036854775808.0)) {
						FAIL(vformat("Cannot convert %s to int: out of range.", value));
					}
					I64(code[pc + 1]) = int64_t(value);
					pc += 3;
					break;
				}
				case Op::ADD_INT:
					I64(code[pc + 1]) = WRAP(+);
					pc += 4;
					break;
				case Op::SUB_INT:
					I64(code[pc + 1]) = WRAP(-);
					pc += 4;
					break;
				case Op::MUL_INT:
					I64(code[pc + 1]) = WRAP(*);
					pc += 4;
					break;
				case Op::DIV_INT:
				case Op::MOD_INT: {
					const int64_t a = I64(code[pc + 2]);
					const int64_t b = I64(code[pc + 3]);
					if (b == 0) {
						FAIL("Division by zero.");
					}
					const bool div = Op(code[pc]) == Op::DIV_INT;
					// The minimum int divided by -1 wraps instead of trapping.
					if (b == -1) {
						I64(code[pc + 1]) = div ? int64_t(0ull - uint64_t(a)) : 0;
					} else {
						I64(code[pc + 1]) = div ? a / b : a % b;
					}
					pc += 4;
					break;
				}
				case Op::NEG_INT:
					I64(code[pc + 1]) = int64_t(0ull - uint64_t(I64(code[pc + 2])));
					pc += 3;
					break;
				case Op::ADD_FLOAT:
					F64(code[pc + 1]) = F64(code[pc + 2]) + F64(code[pc + 3]);
					pc += 4;
					break;
				case Op::SUB_FLOAT:
					F64(code[pc + 1]) = F64(code[pc + 2]) - F64(code[pc + 3]);
					pc += 4;
					break;
				case Op::MUL_FLOAT:
					F64(code[pc + 1]) = F64(code[pc + 2]) * F64(code[pc + 3]);
					pc += 4;
					break;
				case Op::DIV_FLOAT:
					F64(code[pc + 1]) = F64(code[pc + 2]) / F64(code[pc + 3]);
					pc += 4;
					break;
				case Op::NEG_FLOAT:
					F64(code[pc + 1]) = -F64(code[pc + 2]);
					pc += 3;
					break;
				case Op::ADD_STRING:
					STR(code[pc + 1]) = STR(code[pc + 2]) + STR(code[pc + 3]);
					pc += 4;
					break;
#define COMPARE(m_op, m_access, m_cmp) \
	case Op::m_op: \
		U64(code[pc + 1]) = (m_access(code[pc + 2]) m_cmp m_access(code[pc + 3])) ? 1 : 0; \
		pc += 4; \
		break;
					COMPARE(EQ_INT, I64, ==)
					COMPARE(NE_INT, I64, !=)
					COMPARE(LT_INT, I64, <)
					COMPARE(LE_INT, I64, <=)
					COMPARE(GT_INT, I64, >)
					COMPARE(GE_INT, I64, >=)
					COMPARE(EQ_FLOAT, F64, ==)
					COMPARE(NE_FLOAT, F64, !=)
					COMPARE(LT_FLOAT, F64, <)
					COMPARE(LE_FLOAT, F64, <=)
					COMPARE(GT_FLOAT, F64, >)
					COMPARE(GE_FLOAT, F64, >=)
					COMPARE(EQ_BOOL, U64, ==)
					COMPARE(NE_BOOL, U64, !=)
					COMPARE(EQ_STRING, STR, ==)
					COMPARE(NE_STRING, STR, !=)
#undef COMPARE
				case Op::NOT:
					U64(code[pc + 1]) = U64(code[pc + 2]) ? 0 : 1;
					pc += 3;
					break;
				case Op::EVALUATE: {
					const uint32_t right = code[pc + 4];
					image.evaluators[code[pc + 1]](frame + code[pc + 3], right == NO_SLOT ? NOTHING : frame + right, frame + code[pc + 2]);
					pc += 5;
					break;
				}
				case Op::GET_FIELD:
					image.getters[code[pc + 1]](frame + code[pc + 3], frame + code[pc + 2]);
					pc += 4;
					break;
				case Op::SET_FIELD:
					image.setters[code[pc + 1]](frame + code[pc + 2], frame + code[pc + 3]);
					pc += 4;
					break;
				case Op::GET_PROPERTY: {
					const uint32_t index = code[pc + 1];
					OBJECT(object, code[pc + 3]);
					bool valid = false;
					const Variant value = object.get_named(image.properties[index], valid);
					if (!valid) {
						FAIL(vformat("Cannot read the property %s.", image.properties[index]));
					}
					if (!from_variant(module.properties[index].type, value, frame + code[pc + 2])) {
						FAIL(vformat("The property %s holds an unexpected %s.", image.properties[index], Variant::get_type_name(value.get_type())));
					}
					pc += 4;
					break;
				}
				case Op::SET_PROPERTY: {
					const uint32_t index = code[pc + 1];
					OBJECT(object, code[pc + 2]);
					bool valid = false;
					object.set_named(image.properties[index], to_variant(module.properties[index].type, frame + code[pc + 3]), valid);
					if (!valid) {
						FAIL(vformat("Cannot write the property %s.", image.properties[index]));
					}
					pc += 4;
					break;
				}
				case Op::NEW_OBJECT: {
					const Variant object = ClassDBSingleton::get_singleton()->instantiate(image.classes[code[pc + 1]]);
					if (object.get_type() != Variant::OBJECT || !object.booleanize()) {
						FAIL(vformat("Cannot construct %s.", image.classes[code[pc + 1]]));
					}
					variant_at(frame + code[pc + 2]) = object;
					pc += 3;
					break;
				}
				case Op::GET_SINGLETON: {
					Object *singleton = Engine::get_singleton()->get_singleton(image.classes[code[pc + 1]]);
					if (!singleton) {
						FAIL(vformat("The engine has no singleton %s.", image.classes[code[pc + 1]]));
					}
					variant_at(frame + code[pc + 2]) = Variant(singleton);
					pc += 3;
					break;
				}
				case Op::GET_AUTOLOAD: {
					// An autoload is a child of the root by its name (spec §7.3).
					const StringName name = image.values[code[pc + 1]];
					SceneTree *tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop());
					Window *root = tree ? tree->get_root() : nullptr;
					Node *node = root ? root->get_node_or_null(NodePath(String(name))) : nullptr;
					if (!node) {
						FAIL(vformat("The autoload %s is not in the scene tree.", name));
					}
					variant_at(frame + code[pc + 2]) = Variant(node);
					pc += 3;
					break;
				}
				case Op::METHOD_VALUE:
					SELF_ALIVE();
					*reinterpret_cast<Callable *>(frame + code[pc + 2]) = Callable(memnew(MethodCallable(context.owner, image.function_names[code[pc + 1]])));
					pc += 3;
					break;
				case Op::GET_SIGNAL: {
					OBJECT(object, code[pc + 3]);
					*reinterpret_cast<Signal *>(frame + code[pc + 2]) = Signal(static_cast<Object *>(object), StringName(image.values[code[pc + 1]]));
					pc += 4;
					break;
				}
				case Op::CHECK_ASSIGNED:
					if (is_none(variant_at(frame + code[pc + 2]))) {
						const uint32_t member = code[pc + 1];
						// Spec §6.3; §7.5: the scene or the resource did not assign the export.
						FAIL(vformat(module.members[member].is_onready ? "\"%s\" is read before the node is ready." : "\"%s\" has not been assigned.", image.member_names[member]));
					}
					pc += 3;
					break;
				case Op::CHECK_RANGE: {
					// Spec §10.6: the engine would truncate or wrap it.
					const bytecode::IntRange range = bytecode::IntRange(code[pc + 1]);
					if (!bytecode::int_in_range(range, I64(code[pc + 2]))) {
						FAIL(vformat("%d is out of range for the engine's %s.", I64(code[pc + 2]), bytecode::int_range_name(range)));
					}
					pc += 3;
					break;
				}
				case Op::CHECK_OBJECT: {
					const StringName &expected = image.classes[code[pc + 1]];
					const Variant &value = variant_at(frame + code[pc + 2]);
					if (is_none(value)) {
						FAIL(vformat("Expected %s, found None.", expected));
					}
					const Object *object = value.get_validated_object();
					if (object && !image.fits_class(object, code[pc + 1])) {
						FAIL(vformat("Expected %s, found %s.", expected, object->get_class()));
					}
					pc += 3;
					break;
				}
				case Op::CHECK_LIVE:
					if (!is_live_object(frame + code[pc + 1])) {
						FAIL("The object given to the engine was freed.");
					}
					pc += 2;
					break;
				case Op::CAST_OBJECT: {
					// A freed object is None for every type (spec §5.3).
					const uint8_t *src = frame + code[pc + 3];
					const Object *object = is_live_object(src) ? variant_at(src).get_validated_object() : nullptr;
					const bool fits = object && image.fits_class(object, code[pc + 1]);
					variant_at(frame + code[pc + 2]) = fits ? variant_at(src) : Variant();
					pc += 4;
					break;
				}
				case Op::NEW_ARRAY: {
					const uint32_t cls = code[pc + 2];
					*reinterpret_cast<Array *>(frame + code[pc + 3]) = typed_array(ValueType(code[pc + 1]), cls == NO_SLOT ? StringName() : image.classes[cls], cls == NO_SLOT ? String() : image.class_scripts[cls]);
					pc += 4;
					break;
				}
				case Op::GET_INDEX: {
					// A negative index counts from the end, as in GDScript.
					Variant container = to_variant(ValueType(code[pc + 1]), frame + code[pc + 4]);
					const int64_t index = I64(code[pc + 5]);
					bool valid = false;
					bool out_of_bounds = false;
					const Variant value = container.get_indexed(index, valid, out_of_bounds);
					if (!valid || out_of_bounds) {
						FAIL(vformat("Index %d is out of bounds (size %d).", index, int64_t(container.call("size"))));
					}
					const ValueType element = ValueType(code[pc + 2]);
					if (!from_variant(element, value, frame + code[pc + 3])) {
						FAIL(vformat("The element is %s, not %s.", Variant::get_type_name(value.get_type()), bytecode::value_type_name(element)));
					}
					pc += 6;
					break;
				}
				case Op::IS_INSTANCE: {
					// A freed object is no class's instance (spec §3.3).
					const uint8_t *src = frame + code[pc + 3];
					const Object *object = is_live_object(src) ? variant_at(src).get_validated_object() : nullptr;
					U64(code[pc + 2]) = object && image.fits_class(object, code[pc + 1]) ? 1 : 0;
					pc += 4;
					break;
				}
				case Op::IS_NONE: {
					// A freed object reads as None in T? (spec §3.3).
					const Variant &value = variant_at(frame + code[pc + 2]);
					U64(code[pc + 1]) = is_none(value) || (value.get_type() == Variant::OBJECT && !value.booleanize()) ? 1 : 0;
					pc += 3;
					break;
				}
				case Op::TO_VARIANT:
					variant_at(frame + code[pc + 2]) = to_variant(ValueType(code[pc + 1]), frame + code[pc + 3]);
					pc += 4;
					break;
				case Op::UNWRAP: {
					// A freed object reads as None in T? (spec §3.3).
					const ValueType type = ValueType(code[pc + 1]);
					const uint8_t *src = frame + code[pc + 3];
					const bool present = type == ValueType::Object ? is_live_object(src) : !is_none(variant_at(src));
					if (!present || !from_variant(type, variant_at(src), frame + code[pc + 2])) {
						FAIL(code[pc + 4] == NO_SLOT ? String("unwrap() found None.") : STR(code[pc + 4]));
					}
					pc += 5;
					break;
				}
				case Op::JUMP:
					pc = code[pc + 1];
					break;
				case Op::JUMP_IF_FALSE:
					pc = U64(code[pc + 1]) ? pc + 3 : code[pc + 2];
					break;
				case Op::JUMP_IF_TRUE:
					pc = U64(code[pc + 1]) ? code[pc + 2] : pc + 3;
					break;
				// Calls: target, dst, base, argc, then (type, offset) per argument.
				case Op::CALL: {
					const uint32_t target = code[pc + 1];
					const uint32_t dst = code[pc + 2];
					const uint32_t argc = code[pc + 4];
					const bytecode::Function &callee = module.functions[target];
					bool ok;
					{
						Frame callee_frame(image.frames[target]);
						for (uint32_t i = 0; i < argc; i++) {
							copy_value(callee.params[i].type, callee_frame.data + callee.params[i].offset, frame + code[pc + 6 + 2 * i]);
						}
						ok = run(target, callee_frame.data, dst == NO_SLOT ? nullptr : frame + dst);
					}
					if (!ok) {
						return false;
					}
					pc += 5 + 2 * argc;
					break;
				}
				case Op::CALL_ENGINE: {
					const uint32_t target = code[pc + 1];
					const uint32_t dst = code[pc + 2];
					const uint32_t argc = code[pc + 4];
					// By name for now; through MethodBind pointers once the
					// compiler knows method hashes (architecture §2.2).
					OBJECT(object, code[pc + 3]);
					Arguments args(frame, code + pc + 5, argc, nullptr);
					Variant result;
					GDExtensionCallError error;
					object.callp(image.engine_methods[target], args.as_variants(), int(argc), result, error);
					if (error.error != GDEXTENSION_CALL_OK) {
						FAIL(vformat("Calling %s() failed.", image.engine_methods[target]));
					}
					if (dst != NO_SLOT && !from_variant(module.engine_calls[target].return_type, result, frame + dst)) {
						FAIL(vformat("%s() returned an unexpected %s.", image.engine_methods[target], Variant::get_type_name(result.get_type())));
					}
					pc += 5 + 2 * argc;
					break;
				}
				case Op::CALL_METHOD: {
					// A method of a class of this language, called directly: a
					// program error in it ends this function too (spec §8.1).
					const uint32_t target = code[pc + 1];
					const uint32_t dst = code[pc + 2];
					const uint32_t argc = code[pc + 4];
					OBJECT(receiver, code[pc + 3]);
					// Within a run every script is compiled against the others
					// (spec §12.4); only bytecode that was not built together
					// can differ, and then the arguments would not fit.
					ScriptedObject callee;
					const int index = scripted_object(static_cast<Object *>(receiver), callee) ? callee.image->find_function(image.method_names[target]) : -1;
					if (index < 0 || !callee.image->module.functions[index].is_pub || callee.image->function_signatures[index] != image.call_signatures[target]) {
						FAIL(vformat("%s() is not the method this script was compiled against. Build the project again.", image.method_names[target]));
					}
					const bytecode::Function &method = callee.image->module.functions[index];
					const size_t required = method.params.size() - method.defaults.size();
					bool ok;
					{
						Frame callee_frame(callee.image->frames[index]);
						for (size_t i = 0; i < method.params.size(); i++) {
							uint8_t *param = callee_frame.data + method.params[i].offset;
							if (i < argc) {
								copy_value(method.params[i].type, param, frame + code[pc + 6 + 2 * i]);
							} else {
								from_variant(method.params[i].type, callee.image->param_defaults[index][i - required], param, &callee.image->param_checks[index][i]);
							}
						}
						ok = Execution(*callee.image, callee.context, debugging).run(uint32_t(index), callee_frame.data, dst == NO_SLOT ? nullptr : frame + dst);
					}
					if (!ok) {
						return false;
					}
					pc += 5 + 2 * argc;
					break;
				}
				case Op::CALL_UTILITY:
				case Op::CALL_BUILTIN: {
					const bool utility = Op(code[pc]) == Op::CALL_UTILITY;
					const uint32_t target = code[pc + 1];
					const uint32_t dst = code[pc + 2];
					const uint32_t argc = code[pc + 4];
					const std::vector<ValueType> &params = utility ? module.utility_calls[target].params : module.builtin_methods[target].params;
					const ValueType returns = utility ? module.utility_calls[target].return_type : module.builtin_methods[target].return_type;
					const ValueType result_type = utility ? module.utility_calls[target].result_type : module.builtin_methods[target].result_type;
					Arguments args(frame, code + pc + 5, argc, &params);
					// A native result goes straight into its slot; a Variant one
					// is checked on the way (spec §10.3). The scratch slot takes a
					// Variant result, or one that nothing reads.
					const bool via_variant = returns == ValueType::Variant || returns == ValueType::Void;
					const bool unread = !via_variant && dst == NO_SLOT;
					alignas(16) uint8_t scratch[64];
					construct_value(via_variant ? ValueType::Variant : (unread ? returns : ValueType::Void), scratch);
					void *result = via_variant || unread ? scratch : frame + dst;
					if (utility) {
						image.utilities[target](result, args.pointers, int(argc));
					} else {
						image.builtin_methods[target](frame + code[pc + 3], args.pointers, result, int(argc));
					}
					const bool fits = returns != ValueType::Variant || dst == NO_SLOT || from_variant(result_type, variant_at(scratch), frame + dst);
					const Variant::Type got = via_variant ? type_of(variant_at(scratch)) : Variant::NIL;
					destroy_value(via_variant ? ValueType::Variant : (unread ? returns : ValueType::Void), scratch);
					if (!fits) {
						FAIL(vformat("A call returned an unexpected %s.", Variant::get_type_name(got)));
					}
					pc += 5 + 2 * argc;
					break;
				}
				case Op::CONSTRUCT: {
					const uint32_t target = code[pc + 1];
					const uint32_t dst = code[pc + 2];
					const uint32_t argc = code[pc + 4];
					const bytecode::ConstructorCall &constructor = module.constructors[target];
					Arguments args(frame, code + pc + 5, argc, &constructor.params);
					// The engine constructs into memory that holds no value.
					destroy_value(constructor.type, frame + dst);
					image.constructors[target](frame + dst, args.pointers);
					pc += 5 + 2 * argc;
					break;
				}
				case Op::RETURN:
					if (p_result) {
						copy_value(fn.result.type, p_result, frame + code[pc + 1]);
					}
					return true;
				case Op::RETURN_VOID:
					return true;
				default:
					FAIL(vformat("Invalid instruction %d.", code[pc]));
			}
		}
#undef I64
#undef U64
#undef F64
#undef STR
#undef FAIL
#undef WRAP
#undef SELF_ALIVE
#undef OBJECT
	}
};

} // namespace

namespace vm {

Variant call(const std::shared_ptr<const ClassImage> &p_image, int p_function, const InstanceContext &p_context, const Variant **p_args, int64_t p_argc, GDExtensionCallError &r_error, bool *r_failed) {
	// Old code stays alive until this call returns, even if a hot reload
	// replaces the image meanwhile (spec §12.4).
	const std::shared_ptr<const ClassImage> image = p_image;
	const bytecode::Function &fn = image->module.functions[p_function];
	r_error.error = GDEXTENSION_CALL_OK;
	if (image->module.declarations_only) {
		ERR_PRINT(vformat("%s was only described for the editor; it runs once the project is built.", image->path));
		r_error.error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
		return Variant();
	}
	// The last parameters may be left out when they have default values.
	const size_t required = fn.params.size() - fn.defaults.size();
	if (size_t(p_argc) < required || size_t(p_argc) > fn.params.size()) {
		r_error.error = size_t(p_argc) > fn.params.size() ? GDEXTENSION_CALL_ERROR_TOO_MANY_ARGUMENTS : GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS;
		r_error.expected = int32_t(size_t(p_argc) > fn.params.size() ? fn.params.size() : required);
		return Variant();
	}

	Frame frame(image->frames[p_function]);
	const std::vector<ValueCheck> &checks = image->param_checks[p_function];
	for (size_t i = 0; i < fn.params.size(); i++) {
		const bytecode::Slot &param = fn.params[i];
		const Variant &value = i < size_t(p_argc) ? *p_args[i] : image->param_defaults[p_function][i - required];
		if (!from_variant(param.type, value, frame.data + param.offset, &checks[i])) {
			// A program error at the entry, and the engine gets the default value
			// (spec §8.1, §10.1); the engine's backtrace of the caller goes with it.
			report_error(*image, fn.name.c_str(), int(fn.line), vformat("Argument \"%s\" of %s(): %s.", to_godot(param.name), to_godot(fn.name), entry_mismatch(param.type, value, checks[i])));
			if (r_failed) {
				*r_failed = true;
			}
			return default_variant(fn.result.type);
		}
	}

	alignas(16) uint8_t result[64];
	construct_value(fn.result.type, result);
	const bool ok = Execution(*image, p_context, debugger::is_active()).run(uint32_t(p_function), frame.data, result);
	if (r_failed) {
		*r_failed = !ok;
	}
	Variant value = ok ? outward_variant(fn.result, result) : default_variant(fn.result.type);
	destroy_value(fn.result.type, result);
	return value;
}

const std::vector<StackFrame> &stack() {
	return call_stack;
}

void report_error(const ClassImage &p_image, const char *p_function, int p_line, const String &p_message) {
	// The debugger breaks while the stack is still intact (spec §12.5).
	debugger::error(p_message);
	const CharString description = p_message.utf8();
	const CharString file = p_image.path.utf8();
	godot::gdextension_interface::print_script_error(description.get_data(), p_function, file.get_data(), p_line, true);
}

} // namespace vm

} // namespace petrichor
