#include "runtime/vm.h"

#include "bytecode/opcodes.h"
#include "runtime/debugger.h"
#include "runtime/method_callable.h"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/godot.hpp>

#include <cstring>
#include <new>

using namespace godot;

namespace petrichor {

using bytecode::NO_SLOT;
using bytecode::Op;
using bytecode::ValueType;

namespace {

thread_local std::vector<StackFrame> call_stack;

// Frame memory: small frames on the C++ stack, larger ones on the heap.
class FrameMemory {
	alignas(16) uint8_t inline_buffer[256];
	uint8_t *heap = nullptr;

public:
	uint8_t *data;

	explicit FrameMemory(uint32_t p_size) {
		data = p_size <= sizeof(inline_buffer) ? inline_buffer : (heap = static_cast<uint8_t *>(::operator new(p_size)));
	}
	~FrameMemory() {
		::operator delete(heap);
	}
	FrameMemory(const FrameMemory &) = delete;
	FrameMemory &operator=(const FrameMemory &) = delete;
};

// A single slot that holds a value of any type: return values, and results
// the engine writes that nothing reads.
class ValueSlot {
	alignas(16) uint8_t storage[64];
	ValueType type;

public:
	explicit ValueSlot(ValueType p_type) :
			type(p_type) {
		construct_value(type, storage);
	}
	~ValueSlot() {
		destroy_value(type, storage);
	}
	ValueSlot(const ValueSlot &) = delete;
	ValueSlot &operator=(const ValueSlot &) = delete;
	uint8_t *data() { return storage; }
};

// Where an engine function reads a right operand that a unary operator does not have.
alignas(16) const uint8_t NOTHING[64] = {};

// The arguments of a call to the engine, from the instruction's (type, offset)
// pairs: each in its slot's own layout, or as a Variant where the function
// takes a Variant, for extra vararg arguments, or for every argument of a call
// by name.
class Arguments {
	std::vector<Variant> variants;

public:
	std::vector<const void *> pointers;

	Arguments(uint8_t *p_frame, const uint32_t *p_pairs, uint32_t p_count, const std::vector<ValueType> *p_params) {
		variants.reserve(p_count); // Pointers into it stay valid.
		pointers.resize(p_count);
		for (uint32_t i = 0; i < p_count; i++) {
			const ValueType type = ValueType(p_pairs[2 * i]);
			uint8_t *slot = p_frame + p_pairs[2 * i + 1];
			if (!p_params || i >= p_params->size() || (*p_params)[i] == ValueType::Variant) {
				variants.push_back(to_variant(type, slot));
				pointers[i] = &variants.back();
			} else {
				pointers[i] = slot;
			}
		}
	}

	const Variant **as_variants() { return reinterpret_cast<const Variant **>(pointers.data()); }
};

void report_error(const ClassImage &p_image, const bytecode::Function &p_function, int p_line, const String &p_message) {
	// The debugger breaks while the stack is still intact (spec §12.5).
	debugger::error(p_message);
	const CharString description = p_message.utf8();
	const CharString file = p_image.path.utf8();
	godot::gdextension_interface::print_script_error(description.get_data(), p_function.name.c_str(), file.get_data(), p_line, true);
}

class Execution {
	const ClassImage &image;
	const InstanceContext &context;

public:
	Execution(const ClassImage &p_image, const InstanceContext &p_context) :
			image(p_image), context(p_context) {}

	// Runs a function on an initialized frame. Returns false after a program
	// error, which unwinds every frame of this language up to the engine.
	bool run(uint32_t p_function, uint8_t *frame, uint8_t *p_result) {
		const bytecode::Function &fn = image.module.functions[p_function];
		const bytecode::Module &module = image.module;
		const uint32_t *code = fn.code.data();
		uint8_t *members = context.members;
		const size_t depth = call_stack.size();
		call_stack.push_back({ &image, &fn, frame, members, int(fn.line) });
		debugger::enter_function();
		struct Pop {
			~Pop() {
				debugger::exit_function();
				call_stack.pop_back();
			}
		} pop;

#define I64(m_offset) (*reinterpret_cast<int64_t *>(frame + (m_offset)))
#define U64(m_offset) (*reinterpret_cast<uint64_t *>(frame + (m_offset)))
#define F64(m_offset) (*reinterpret_cast<double *>(frame + (m_offset)))
#define STR(m_offset) (*reinterpret_cast<String *>(frame + (m_offset)))
#define FAIL(m_message)                                                   \
	do {                                                                  \
		report_error(image, fn, call_stack[depth].line, (m_message));     \
		return false;                                                     \
	} while (false)
// Wrapping arithmetic on int, as in GDScript; through unsigned to stay defined.
#define WRAP(m_op) int64_t(uint64_t(I64(code[pc + 2])) m_op uint64_t(I64(code[pc + 3])))
// The object an instruction works on: self, or a live object in a slot (spec §3.3).
#define OBJECT(m_name, m_offset)                                                     \
	Variant m_name;                                                                  \
	if ((m_offset) == NO_SLOT) {                                                     \
		m_name = Variant(context.owner);                                             \
	} else if (is_live_object(frame + (m_offset))) {                                 \
		m_name = variant_at(frame + (m_offset));                                     \
	} else {                                                                         \
		FAIL(is_none(variant_at(frame + (m_offset))) ? String("The value is None.") : String("The object was freed.")); \
	}

		size_t pc = 0;
		while (true) {
			switch (Op(code[pc])) {
				case Op::LINE:
					call_stack[depth].line = int(code[pc + 1]);
					if (debugger::is_active()) {
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
					std::memcpy(frame + code[pc + 1], members + code[pc + 2], 8);
					pc += 3;
					break;
				case Op::LOAD_MEMBER_STRING:
					STR(code[pc + 1]) = *reinterpret_cast<String *>(members + code[pc + 2]);
					pc += 3;
					break;
				case Op::LOAD_MEMBER_VALUE:
					copy_value(ValueType(code[pc + 1]), frame + code[pc + 2], members + code[pc + 3]);
					pc += 4;
					break;
				case Op::STORE_MEMBER_8:
					std::memcpy(members + code[pc + 1], frame + code[pc + 2], 8);
					pc += 3;
					break;
				case Op::STORE_MEMBER_STRING:
					*reinterpret_cast<String *>(members + code[pc + 1]) = STR(code[pc + 2]);
					pc += 3;
					break;
				case Op::STORE_MEMBER_VALUE:
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
#define COMPARE(m_op, m_access, m_cmp)                                                 \
	case Op::m_op:                                                                     \
		U64(code[pc + 1]) = (m_access(code[pc + 2]) m_cmp m_access(code[pc + 3])) ? 1 : 0; \
		pc += 4;                                                                       \
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
				case Op::METHOD_VALUE:
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
					// Spec §7.5: the scene or the resource did not assign the export.
					if (is_none(variant_at(frame + code[pc + 2]))) {
						FAIL(vformat("\"%s\" has not been assigned.", image.member_names[code[pc + 1]]));
					}
					pc += 3;
					break;
				case Op::CHECK_OBJECT: {
					const StringName &expected = image.classes[code[pc + 1]];
					const Variant &value = variant_at(frame + code[pc + 2]);
					if (is_none(value)) {
						FAIL(vformat("Expected %s, found None.", expected));
					}
					const Object *object = value.get_validated_object();
					if (object && !object->is_class(expected)) {
						FAIL(vformat("Expected %s, found %s.", expected, object->get_class()));
					}
					pc += 3;
					break;
				}
				case Op::CAST_OBJECT: {
					// A freed object is None for every type (spec §5.3).
					const uint8_t *src = frame + code[pc + 3];
					const Object *object = is_live_object(src) ? variant_at(src).get_validated_object() : nullptr;
					const bool fits = object && object->is_class(image.classes[code[pc + 1]]);
					variant_at(frame + code[pc + 2]) = fits ? variant_at(src) : Variant();
					pc += 4;
					break;
				}
				case Op::NEW_ARRAY: {
					Array array;
					const ValueType element = ValueType(code[pc + 1]);
					if (element != ValueType::Void) {
						array.set_typed(uint32_t(variant_type(element)), code[pc + 2] == NO_SLOT ? StringName() : image.classes[code[pc + 2]], Variant());
					}
					*reinterpret_cast<Array *>(frame + code[pc + 3]) = array;
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
					U64(code[pc + 2]) = object && object->is_class(image.classes[code[pc + 1]]) ? 1 : 0;
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
					FrameMemory callee_frame(callee.frame_size);
					construct_slots(callee_frame.data, callee.frame_size, callee.slots);
					for (uint32_t i = 0; i < argc; i++) {
						copy_value(callee.params[i].type, callee_frame.data + callee.params[i].offset, frame + code[pc + 6 + 2 * i]);
					}
					const bool ok = run(target, callee_frame.data, dst == NO_SLOT ? nullptr : frame + dst);
					destroy_slots(callee_frame.data, callee.slots);
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
					// is checked on the way (spec §10.3).
					Variant variant_result;
					ValueSlot unread(dst == NO_SLOT && returns != ValueType::Variant ? returns : ValueType::Void);
					void *result = returns == ValueType::Variant || returns == ValueType::Void ? static_cast<void *>(&variant_result) : (dst == NO_SLOT ? unread.data() : frame + dst);
					if (utility) {
						image.utilities[target](result, args.pointers.data(), int(argc));
					} else {
						image.builtin_methods[target](frame + code[pc + 3], args.pointers.data(), result, int(argc));
					}
					if (returns == ValueType::Variant && dst != NO_SLOT && !from_variant(result_type, variant_result, frame + dst)) {
						FAIL(vformat("A call returned an unexpected %s.", Variant::get_type_name(variant_result.get_type())));
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
					image.constructors[target](frame + dst, args.pointers.data());
					pc += 5 + 2 * argc;
					break;
				}
				case Op::RETURN:
					if (p_result) {
						copy_value(fn.return_type, p_result, frame + code[pc + 1]);
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
#undef OBJECT
	}
};

} // namespace

namespace vm {

Variant call(const std::shared_ptr<const ClassImage> &p_image, int p_function, const InstanceContext &p_context, const Variant **p_args, int64_t p_argc, GDExtensionCallError &r_error) {
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

	FrameMemory frame(fn.frame_size);
	construct_slots(frame.data, fn.frame_size, fn.slots);
	for (size_t i = 0; i < fn.params.size(); i++) {
		const bytecode::Slot &param = fn.params[i];
		const Variant value = i < size_t(p_argc) ? *p_args[i] : literal_value(fn.defaults[i - required]);
		if (!from_variant(param.type, value, frame.data + param.offset, ValueCheck(param))) {
			destroy_slots(frame.data, fn.slots);
			// The caller reports this with its own call site, as for engine methods.
			r_error.error = GDEXTENSION_CALL_ERROR_INVALID_ARGUMENT;
			r_error.argument = int32_t(i);
			r_error.expected = GDExtensionVariantType(variant_type(param.type));
			return Variant();
		}
	}

	ValueSlot result(fn.return_type);
	const bool ok = Execution(*image, p_context).run(uint32_t(p_function), frame.data, result.data());
	Variant value = ok ? to_variant(fn.return_type, result.data()) : default_variant(fn.return_type);
	destroy_slots(frame.data, fn.slots);
	return value;
}

const std::vector<StackFrame> &stack() {
	return call_stack;
}

} // namespace vm

} // namespace petrichor
