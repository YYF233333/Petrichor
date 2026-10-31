#include "runtime/vm.h"

#include "bytecode/opcodes.h"
#include "runtime/debugger.h"
#include "runtime/method_callable.h"
#include "runtime/object_watch.h"
#include "runtime/task.h"
#include "runtime/text.h"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/callable_custom.hpp>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
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

struct CoroutineFrame : std::enable_shared_from_this<CoroutineFrame> {
	std::shared_ptr<const ClassImage> image;
	InstanceContext context;
	CoroutineTask *task;
	uint32_t function;
	uint8_t *data;
	size_t pc = 0;
	int line = 0;
	uint32_t destination = NO_SLOT;
	uint64_t generation = 0;
	// The awaited object was freed: tasks::frame() fails the wait once the
	// frame's deletions are done, unless the waiter's own object went too (spec §9.7).
	bool awaited_freed = false;
	uint64_t lost_object = 0;
	Signal waiting;
	Callable callback;
	Signal script_changed;
	Callable changed_callback;
	object_watch::Subscription watch;
	Variant held_state;
	Ref<CoroutineTask> waited_task;
	Ref<CoroutineTask> child;

	CoroutineFrame(const std::shared_ptr<const ClassImage> &p_image, uint32_t p_function, const InstanceContext &p_context, CoroutineTask *p_task) :
			image(p_image), context(p_context), task(p_task), function(p_function), data(static_cast<uint8_t *>(::operator new(p_image->frames[p_function].size))) {
		image->frames[function].construct(data);
		line = int(image->module.functions[function].line);
	}
	~CoroutineFrame() {
		disconnect();
		if (data) {
			image->frames[function].destroy(data);
		}
		::operator delete(data);
	}
	void disconnect();
	bool receive(const Variant &p_value);
	bool await_task(const Ref<CoroutineTask> &p_task);
	void report_error(const String &p_message);
	void resume();
	void complete_wait(uint64_t p_generation, const Variant &p_value);
	bool suspend(const Signal &p_signal, uint32_t p_dst, uint64_t p_watch, bool p_script = false);
};

namespace {
std::mutex tasks_mutex;
std::atomic<uint64_t> task_frame{ 0 };
std::map<uint64_t, Ref<CoroutineTask>> active_tasks;

std::vector<Ref<CoroutineTask>> task_snapshot() {
	std::lock_guard<std::mutex> lock(tasks_mutex);
	std::vector<Ref<CoroutineTask>> tasks;
	for (const auto &entry : active_tasks) {
		tasks.push_back(entry.second);
	}
	return tasks;
}

class ResumeCallable : public CallableCustom {
	std::function<void(const Variant &)> callback;
	static bool equal(const CallableCustom *a, const CallableCustom *b) { return a == b; }
	static bool less(const CallableCustom *a, const CallableCustom *b) { return std::less<const CallableCustom *>()(a, b); }

public:
	explicit ResumeCallable(std::function<void(const Variant &)> p_callback) : callback(std::move(p_callback)) {}
	uint32_t hash() const override { return uint32_t(reinterpret_cast<uintptr_t>(this)); }
	String get_as_text() const override { return "Petrichor await"; }
	CompareEqualFunc get_compare_equal_func() const override { return equal; }
	CompareLessFunc get_compare_less_func() const override { return less; }
	ObjectID get_object() const override { return ObjectID(); }
	bool is_valid() const override { return true; }
	int get_argument_count(bool &r_valid) const override {
		r_valid = true;
		return 0;
	}
	void call(const Variant **p_args, int p_count, Variant &r_result, GDExtensionCallError &r_error) const override {
		r_error.error = GDEXTENSION_CALL_OK;
		Variant value;
		if (p_count == 1) {
			value = *p_args[0];
		} else if (p_count > 1) {
			Array args;
			for (int i = 0; i < p_count; ++i) {
				args.push_back(*p_args[i]);
			}
			value = args;
		}
		callback(value);
	}
};

// `p_result_key` is the result as the caller sees it, which an override may
// give as another type (spec §4.6).
Ref<CoroutineTask> start_task(const std::shared_ptr<const ClassImage> &p_image, uint32_t p_function, const InstanceContext &p_context, uint8_t *p_frame, const std::string *p_result_key = nullptr) {
	const bytecode::Function &fn = p_image->module.functions[p_function];
	Ref<CoroutineTask> task;
	task.instantiate();
	task->owner_id = p_context.owner ? uint64_t(p_context.owner->get_instance_id()) : 0;
	task->result_signature = p_result_key ? *p_result_key : p_image->types[fn.result.type].result_key;
	task->continuation = std::make_shared<CoroutineFrame>(p_image, p_function, p_context, task.ptr());
	for (const auto &param : fn.params) {
		copy_value(p_image->types[param.type], task->continuation->data + param.offset, p_frame + param.offset);
	}
	{
		std::lock_guard<std::mutex> lock(tasks_mutex);
		active_tasks.emplace(uint64_t(task->get_instance_id()), task);
	}
	if (p_context.storage && p_context.storage->coroutines_cancelled) {
		task->cancel_requested = true;
		task->owner_freed = true;
		task->owner_freed_frame = task_frame.load();
	}
	task->continuation->resume();
	return task;
}

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

// A value of a type outside any frame, such as a result on its way, made and
// destroyed by its type; on the C++ stack when it is small.
class ValueBuffer {
	const TypeImage &type;
	alignas(16) uint8_t small[80];
	std::unique_ptr<uint64_t[]> large;

public:
	uint8_t *const data;

	explicit ValueBuffer(const TypeImage &p_type) :
			type(p_type), large(p_type.size > sizeof(small) ? new uint64_t[p_type.size / 8 + 1] : nullptr), data(large ? reinterpret_cast<uint8_t *>(large.get()) : small) {
		construct_value(type, data);
	}
	~ValueBuffer() { destroy_value(type, data); }
	ValueBuffer(const ValueBuffer &) = delete;
	ValueBuffer &operator=(const ValueBuffer &) = delete;
};

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

	Arguments(const std::vector<TypeImage> &p_types, uint8_t *p_frame, const uint32_t *p_pairs, uint32_t p_count, const std::vector<ValueType> *p_params) {
		if (p_count > INLINE) {
			heap_pointers.resize(p_count);
			heap_variants.reserve(p_count); // Pointers into it stay valid.
		}
		pointers = p_count > INLINE ? heap_pointers.data() : inline_pointers;
		for (uint32_t i = 0; i < p_count; i++) {
			const TypeImage &type = p_types[p_pairs[2 * i]];
			uint8_t *slot = p_frame + p_pairs[2 * i + 1];
			if (p_params && i < p_params->size() && (*p_params)[i] != ValueType::Variant) {
				pointers[i] = slot;
			} else if (type.storage == ValueType::Variant || type.storage == ValueType::Object) {
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

// What an object must be to fit the class table's entry `p_index`.
ValueCheck class_check(const ClassImage &p_image, uint32_t p_index) {
	ValueCheck check;
	check.class_name = p_image.classes[p_index];
	check.script = p_image.class_scripts[p_index];
	check.interface = p_image.class_interfaces[p_index];
	check.nullable = false;
	return check;
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

	// Calls a function of a class of this language for the call instruction at
	// `p_call`: a program error in it ends the caller too (spec §8.1). An
	// override or an inherited function may take and give other types than the
	// call was compiled against (§4.6), which the arguments and result convert to.
	bool call_function(const ClassImage &p_callee, uint32_t p_index, const InstanceContext &p_context, const uint32_t *p_call, uint8_t *frame, const bytecode::Slot &p_result, const bytecode::Function &p_caller, int p_line) {
		const uint32_t dst = p_call[2], argc = p_call[4];
		const bytecode::Function &method = p_callee.module.functions[p_index];
		const auto fail = [&](const String &p_message) {
			report_error(image, p_caller, p_line, p_message);
			return false;
		};
		if (!method.is_static && !p_context.storage) {
			return fail("An instance method needs a receiver. Build the project again.");
		}
		const size_t required = method.params.size() - method.defaults.size();
		if (argc < required || argc > method.params.size()) {
			return fail("Override argument count changed. Rebuild the project.");
		}
		Frame callee_frame(p_callee.frames[p_index]);
		for (size_t i = 0; i < method.params.size(); i++) {
			uint8_t *param = callee_frame.data + method.params[i].offset;
			const TypeImage &param_type = p_callee.types[method.params[i].type];
			const TypeImage *source = i < argc ? &image.types[p_call[5 + 2 * i]] : nullptr;
			if (source && source->layout == param_type.layout) {
				copy_value(param_type, param, frame + p_call[6 + 2 * i]);
				continue;
			}
			Variant value;
			String error;
			if (source) {
				value = to_variant(*source, frame + p_call[6 + 2 * i]);
			} else if (!constant_value(p_callee.types, method.defaults[i - required], value, error)) {
				return fail(error);
			}
			if (!from_variant(param_type, value, param, true)) {
				return fail(vformat("Argument of %s(): %s.", p_callee.function_names[p_index], entry_mismatch(param_type, value)));
			}
		}
		const TypeImage &result_type = image.types[p_result.type];
		if (method.is_async) {
			const Ref<CoroutineTask> task = start_task(p_callee.shared_from_this(), p_index, p_context, callee_frame.data, &result_type.result_key);
			if (dst != NO_SLOT) {
				variant_at(frame + dst) = task;
			}
			return true;
		}
		const bool here = &p_callee == &image && &p_context == &context;
		const auto run_callee = [&](uint8_t *p_returned) {
			return here ? run(p_index, callee_frame.data, p_returned) : Execution(p_callee, p_context, debugging).run(p_index, callee_frame.data, p_returned);
		};
		const TypeImage &returns = p_callee.types[method.result.type];
		if (dst == NO_SLOT || returns.layout == result_type.layout) {
			return run_callee(dst == NO_SLOT ? nullptr : frame + dst);
		}
		ValueBuffer returned(returns);
		return run_callee(returned.data) && from_variant(result_type, outward_variant(returns, returned.data), frame + dst, true);
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
	bool run(uint32_t p_function, uint8_t *frame, uint8_t *p_result, CoroutineFrame *p_async = nullptr) {
		const bytecode::Function &fn = image.module.functions[p_function];
		const bytecode::Module &module = image.module;
		const uint32_t *code = fn.code.data();
		uint8_t *members = context.storage ? context.storage->members : nullptr;
		const std::atomic<bool> &freed = context.storage ? context.storage->freed : NO_INSTANCE_FREED;
		if (call_stack.capacity() < MAX_CALL_DEPTH + 1) {
			call_stack.reserve(MAX_CALL_DEPTH + 1);
		}
		if (call_stack.size() == MAX_CALL_DEPTH) {
			const StackFrame &caller = call_stack.back();
			report_error(*caller.image, *caller.function, caller.line, vformat("Stack overflow: more than %d calls deep. Check for infinite recursion.", int64_t(MAX_CALL_DEPTH)));
			return false;
		}
		call_stack.push_back({ &image, context.self_image.get(), &fn, frame, members, p_async ? p_async->line : int(fn.line) });
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

		size_t pc = p_async ? p_async->pc : 0;
		while (true) {
			switch (Op(code[pc])) {
				case Op::LINE:
					record.line = int(code[pc + 1]);
					if (p_async) {
						p_async->line = record.line;
					}
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
					from_variant(image.types[code[pc + 1]], image.values[code[pc + 3]], frame + code[pc + 2], false);
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
					copy_value(image.types[code[pc + 1]], frame + code[pc + 2], frame + code[pc + 3]);
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
					copy_value(image.types[code[pc + 1]], frame + code[pc + 2], members + code[pc + 3]);
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
					copy_value(image.types[code[pc + 1]], members + code[pc + 2], frame + code[pc + 3]);
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
					if (!from_variant(image.types[module.properties[index].type], value, frame + code[pc + 2], false)) {
						FAIL(vformat("The property %s holds an unexpected %s.", image.properties[index], Variant::get_type_name(value.get_type())));
					}
					pc += 4;
					break;
				}
				case Op::SET_PROPERTY: {
					const uint32_t index = code[pc + 1];
					OBJECT(object, code[pc + 2]);
					bool valid = false;
					object.set_named(image.properties[index], to_variant(image.types[module.properties[index].type], frame + code[pc + 3]), valid);
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
				case Op::GET_OWNER: {
					// Checked at every read, since code and the engine change the
					// owner (spec §7.3).
					SELF_ALIVE();
					const Node *node = Object::cast_to<Node>(context.owner);
					Node *found = node ? node->get_owner() : nullptr;
					const uint32_t index = code[pc + 1];
					if (!found || !image.fits_class(found, index)) {
						FAIL(vformat("owner: %s (@owner).", entry_mismatch(ValueType::Object, Variant(found), class_check(image, index))));
					}
					variant_at(frame + code[pc + 2]) = Variant(found);
					pc += 3;
					break;
				}
				case Op::GET_SELF:
					SELF_ALIVE();
					variant_at(frame + code[pc + 1]) = self();
					pc += 2;
					break;
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
					const Variant &value = variant_at(frame + code[pc + 2]);
					const Object *object = value.get_validated_object();
					if (is_none(value) || (object && !image.fits_class(object, code[pc + 1]))) {
						const String mismatch = entry_mismatch(ValueType::Object, value, class_check(image, code[pc + 1]));
						FAIL(mismatch.substr(0, 1).to_upper() + mismatch.substr(1) + ".");
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
					const ValueCheck &typed = image.types[code[pc + 1]].check;
					*reinterpret_cast<Array *>(frame + code[pc + 2]) = typed_array(typed.element, typed.class_name, typed.script);
					pc += 3;
					break;
				}
				case Op::GET_INDEX: {
					// A negative index counts from the end, as in GDScript.
					Variant container = to_variant(image.types[code[pc + 1]].storage, frame + code[pc + 4]);
					const int64_t index = I64(code[pc + 5]);
					bool valid = false;
					bool out_of_bounds = false;
					const Variant value = container.get_indexed(index, valid, out_of_bounds);
					if (!valid || out_of_bounds) {
						FAIL(vformat("Index %d is out of bounds (size %d).", index, int64_t(container.call("size"))));
					}
					const TypeImage &element = image.types[code[pc + 2]];
					if (!from_variant(element, value, frame + code[pc + 3], false)) {
						FAIL(vformat("The element is %s, not %s.", Variant::get_type_name(value.get_type()), type_description(element)));
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
					const uint8_t *src = frame + code[pc + 3];
					if (image.types[code[pc + 1]].kind == bytecode::TypeKind::Option) {
						U64(code[pc + 2]) = U64(code[pc + 3]) ? 0 : 1;
					} else {
						// A freed object reads as None in T? (spec §3.3).
						const Variant &value = variant_at(src);
						U64(code[pc + 2]) = is_none(value) || (value.get_type() == Variant::OBJECT && !value.booleanize()) ? 1 : 0;
					}
					pc += 4;
					break;
				}
				case Op::SOME: {
					const TypeImage &option = image.types[code[pc + 1]];
					copy_value(*option.element, frame + code[pc + 2] + 8, frame + code[pc + 3]);
					U64(code[pc + 2]) = 1;
					pc += 4;
					break;
				}
				case Op::TO_VARIANT:
					variant_at(frame + code[pc + 2]) = to_variant(image.types[code[pc + 1]], frame + code[pc + 3]);
					pc += 4;
					break;
				case Op::UNWRAP: {
					const TypeImage &type = image.types[code[pc + 1]];
					const uint8_t *src = frame + code[pc + 3];
					// An object's T? holds the object as T does, and a freed one reads
					// as None (spec §3.3); T? of another type holds a tag first.
					const bool object = type.kind == bytecode::TypeKind::Object;
					if (object ? !is_live_object(src) : U64(code[pc + 3]) == 0) {
						FAIL(code[pc + 4] == NO_SLOT ? String("unwrap() found None.") : STR(code[pc + 4]));
					}
					copy_value(type, frame + code[pc + 2], object ? src : src + 8);
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
					const ClassImage *owner = module.functions[target].is_static || !context.self_image ? &image : context.self_image.get();
					const ClassImage::FunctionOrigin &origin = owner->function_origins[target];
					if (!call_function(origin.image ? *origin.image : *owner, origin.image ? uint32_t(origin.index) : target, context, code + pc, frame, module.functions[target].result, fn, record.line)) {
						return false;
					}
					pc += 5 + 2 * code[pc + 4];
					break;
				}
				case Op::CALL_SUPER:
				case Op::CALL_METHOD: {
					const uint32_t target = code[pc + 1];
					const StringName &name = image.method_names[target];
					ScriptedObject callee;
					if (Op(code[pc]) == Op::CALL_SUPER) {
						callee = { image.base, context };
					} else {
						OBJECT(receiver, code[pc + 3]);
						if (!scripted_object(static_cast<Object *>(receiver), callee)) {
							FAIL("The receiver is not a Petrichor instance.");
						}
						if (callee.context.storage == context.storage && context.self_image) {
							callee.image = context.self_image;
						}
						callee.context.self_image = callee.image;
					}
					// Within a run every script is compiled against the others
					// (spec §12.4); only bytecode that was not built together
					// can differ, and then the arguments would not fit.
					int index = callee.image ? callee.image->find_function(name) : -1;
					if (index < 0 || !callee.image->has_signature(name, image.call_signatures[target]) || (Op(code[pc]) == Op::CALL_METHOD && !callee.image->module.functions[index].is_pub && !callee.image->module.functions[index].is_override)) {
						FAIL(vformat("%s() is not the method this script was compiled against. Build the project again.", name));
					}
					ClassImage::resolve_function(callee.image, index);
					if (!call_function(*callee.image, uint32_t(index), callee.context, code + pc, frame, module.method_calls[target].result, fn, record.line)) {
						return false;
					}
					pc += 5 + 2 * code[pc + 4];
					break;
				}
				case Op::CALL_AWAIT_ENGINE:
				case Op::CALL_ENGINE: {
					const uint32_t target = code[pc + 1];
					const uint32_t dst = code[pc + 2];
					const uint32_t argc = code[pc + 4];
					// By name for now; through MethodBind pointers once the
					// compiler knows method hashes (architecture §2.2).
					OBJECT(object, code[pc + 3]);
					Arguments args(image.types, frame, code + pc + 5, argc, nullptr);
					Variant result;
					GDExtensionCallError error;
					object.callp(image.engine_methods[target], args.as_variants(), int(argc), result, error);
					if (error.error != GDEXTENSION_CALL_OK) {
						FAIL(vformat("Calling %s() failed.", image.engine_methods[target]));
					}
					Object *state = result.get_type() == Variant::OBJECT ? static_cast<Object *>(result) : nullptr;
					if (state && state->is_class("GDScriptFunctionState")) {
						if (Op(code[pc]) == Op::CALL_AWAIT_ENGINE) {
							if (p_async->task->cancel_requested) {
								return true;
							}
							p_async->held_state = result;
							p_async->pc = pc + 5 + 2 * argc;
							if (!p_async->suspend(Signal(state, "completed"), dst, object_id_of(object), true)) {
								FAIL("Cannot await GDScript completion.");
							}
							return true;
						}
						if (dst != NO_SLOT) {
							FAIL("The method is a GDScript coroutine and needs await.");
						}
					} else if (dst != NO_SLOT && !from_variant(image.types[module.engine_calls[target].result_type], result, frame + dst, false)) {
						FAIL(vformat("%s() returned an unexpected %s.", image.engine_methods[target], Variant::get_type_name(result.get_type())));
					}
					pc += 5 + 2 * argc;
					break;
				}

				case Op::CALL_AWAIT_BUILTIN: {
					const uint32_t dst = code[pc + 2];
					const uint32_t argc = code[pc + 4];
					const Callable &callable = *reinterpret_cast<Callable *>(frame + code[pc + 3]);
					Arguments args(image.types, frame, code + pc + 5, argc, nullptr);
					Variant base = callable;
					Variant result;
					GDExtensionCallError error;
					base.callp(to_godot(module.builtin_methods[code[pc + 1]].method), args.as_variants(), int(argc), result, error);
					if (error.error != GDEXTENSION_CALL_OK) {
						FAIL("Calling the awaited Callable failed.");
					}
					Object *state = result.get_type() == Variant::OBJECT ? static_cast<Object *>(result) : nullptr;
					p_async->destination = dst;
					if (state && state->is_class("GDScriptFunctionState")) {
						if (p_async->task->cancel_requested) {
							return true;
						}
						p_async->held_state = result;
						p_async->pc = pc + 5 + 2 * argc;
						if (!p_async->suspend(Signal(state, "completed"), dst, uint64_t(callable.get_object_id()), true)) {
							FAIL("Cannot await GDScript completion.");
						}
						return true;
					}
					if (result.get_type() == Variant::SIGNAL) {
						if (p_async->task->cancel_requested) {
							return true;
						}
						const Signal signal = result;
						Ref<CoroutineTask> task = Object::cast_to<CoroutineTask>(signal.get_object());
						p_async->pc = pc + 5 + 2 * argc;
						if (task.is_valid() && signal.get_name() == StringName("completed")) {
							if (!p_async->await_task(task)) {
								return false;
							}
						} else if (!p_async->suspend(signal, dst, uint64_t(signal.get_object_id()))) {
							FAIL("Cannot await this signal.");
						}
						if (p_async->task->state == CoroutineTask::State::Suspended) {
							return true;
						}
					} else if (!p_async->receive(result)) {
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
					const TypeImage &result_type = image.types[utility ? module.utility_calls[target].result_type : module.builtin_methods[target].result_type];
					Arguments args(image.types, frame, code + pc + 5, argc, &params);
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
					const bool fits = returns != ValueType::Variant || dst == NO_SLOT || from_variant(result_type, variant_at(scratch), frame + dst, false);
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
					Arguments args(image.types, frame, code + pc + 5, argc, &constructor.params);
					// The engine constructs into memory that holds no value.
					destroy_value(constructor.type, frame + dst);
					image.constructors[target](frame + dst, args.pointers);
					pc += 5 + 2 * argc;
					break;
				}
				case Op::TASK_CANCEL: {
					CoroutineTask *task = Object::cast_to<CoroutineTask>(static_cast<Object *>(variant_at(frame + code[pc + 1])));
					if (!task) {
						FAIL("Expected a Task handle.");
					}
					task->cancel();
					pc += 2;
					break;
				}
				case Op::AWAIT_TASK: {
					Ref<CoroutineTask> task = variant_at(frame + code[pc + 2]);
					if (task.is_null()) {
						FAIL("Expected a Task handle.");
					}
					if (code[pc + 3]) {
						p_async->child = task;
					}
					if (p_async->task->cancel_requested) {
						return true;
					}
					p_async->destination = code[pc + 1];
					p_async->pc = pc + 4;
					if (!p_async->await_task(task)) {
						return false;
					}
					if (p_async->task->state == CoroutineTask::State::Suspended) {
						return true;
					}
					pc += 4;
					break;
				}
				case Op::AWAIT_SIGNAL: {
					if (p_async->task->cancel_requested) {
						return true;
					}
					const Signal signal = *reinterpret_cast<Signal *>(frame + code[pc + 2]);
					p_async->pc = pc + 3;
					if (!p_async->suspend(signal, code[pc + 1], uint64_t(signal.get_object_id()))) {
						FAIL("Cannot await this signal.");
					}
					return true;
				}
				case Op::RETURN:
					if (p_result) {
						copy_value(image.types[fn.result.type], p_result, frame + code[pc + 1]);
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

void CoroutineTask::_bind_methods() {
	ClassDB::bind_method(D_METHOD("cancel"), &CoroutineTask::cancel);
	ADD_SIGNAL(MethodInfo("completed", PropertyInfo(Variant::NIL, "result", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NIL_IS_VARIANT)));
}

void CoroutineTask::finish(State p_state, const Variant &p_result) {
	if (ended()) {
		return;
	}
	Ref<CoroutineTask> keep(this);
	state = p_state;
	result = p_state == State::Finished ? p_result : Variant();
	if (continuation) {
		const auto frame = std::move(continuation);
		frame->disconnect();
		Ref<CoroutineTask> child = frame->child;
		frame->child.unref();
		if (p_state == State::Cancelled && child.is_valid()) {
			child->cancel();
		}
		if (frame->data) {
			uint8_t *data = frame->data;
			frame->data = nullptr;
			frame->image->frames[frame->function].destroy(data);
			::operator delete(data);
		}
	}
	{
		std::lock_guard<std::mutex> lock(tasks_mutex);
		active_tasks.erase(uint64_t(get_instance_id()));
	}
	emit_signal("completed", result);
}

void CoroutineTask::cancel() {
	if (ended()) {
		return;
	}
	cancel_requested = true;
	if (state == State::Suspended) {
		finish(State::Cancelled);
	}
}
void CoroutineTask::cancel_owner() {
	if (ended()) {
		return;
	}
	owner_freed = true;
	owner_freed_frame = task_frame.load();
	cancel();
}

void CoroutineFrame::disconnect() {
	++generation;
	if (!waiting.is_null() && waiting.get_object() && waiting.is_connected(callback)) {
		waiting.disconnect(callback);
	}
	if (!script_changed.is_null() && script_changed.get_object() && script_changed.is_connected(changed_callback)) {
		script_changed.disconnect(changed_callback);
	}
	object_watch::unwatch(watch);
	watch = {};
	waiting = Signal();
	callback = Callable();
	script_changed = Signal();
	changed_callback = Callable();
	held_state = Variant();
	waited_task.unref();
	lost_object = 0;
}

void CoroutineFrame::report_error(const String &p_message) {
	const auto &fn = image->module.functions[function];
	const bool already_running = !call_stack.empty() && call_stack.back().frame == data;
	if (!already_running) {
		if (call_stack.capacity() < MAX_CALL_DEPTH + 1) {
			call_stack.reserve(MAX_CALL_DEPTH + 1);
		}
		uint8_t *members = context.storage && !context.storage->freed.load() ? context.storage->members : nullptr;
		call_stack.push_back({ image.get(), context.self_image.get(), &fn, data, members, line });
	}
	vm::report_error(*image, fn.name.c_str(), line, p_message);
	if (!already_running) {
		call_stack.pop_back();
	}
}

bool CoroutineFrame::await_task(const Ref<CoroutineTask> &p_task) {
	if (p_task->state == CoroutineTask::State::Finished) {
		child.unref();
		return receive(p_task->result);
	}
	if (p_task->state == CoroutineTask::State::Failed) {
		return false;
	}
	if (p_task->owner_freed) {
		const uint64_t id = p_task->owner_id;
		if (p_task->owner_freed_frame != task_frame.load()) {
			report_error(vformat("The awaited object %d was freed or lost its script.", int64_t(id)));
			return false;
		}
		child.unref();
		awaited_freed = true;
		lost_object = id;
		task->state = CoroutineTask::State::Suspended;
		return true;
	}
	if (p_task->state == CoroutineTask::State::Cancelled) {
		report_error("The awaited task was cancelled.");
		return false;
	}
	waited_task = p_task;
	if (!suspend(Signal(p_task.ptr(), "completed"), destination, 0)) {
		report_error("Cannot await task completion.");
		return false;
	}
	return true;
}

bool CoroutineFrame::receive(const Variant &p_value) {
	if (destination == NO_SLOT) {
		return true;
	}
	for (const auto &slot : image->module.functions[function].slots) {
		if (slot.offset != destination) {
			continue;
		}
		const TypeImage &type = image->types[slot.type];
		if (from_variant(type, p_value, data + destination, true)) {
			return true;
		}
		report_error("Await result: " + entry_mismatch(type, p_value));
		return false;
	}
	return false;
}

bool CoroutineFrame::suspend(const Signal &p_signal, uint32_t p_dst, uint64_t p_watch, bool p_script) {
	destination = p_dst;
	waiting = p_signal;
	task->state = CoroutineTask::State::Suspended;
	const uint64_t ticket = ++generation;
	const std::weak_ptr<CoroutineFrame> weak = shared_from_this();
	const std::function<void(const Variant &)> resume = [weak, ticket](const Variant &value) {
		if (const auto frame = weak.lock()) {
			frame->complete_wait(ticket, value);
		}
	};
	callback = Callable(memnew(ResumeCallable(resume)));
	if (!waiting.get_object()) {
		awaited_freed = true;
		lost_object = p_watch;
		return true;
	}
	if (waiting.connect(callback) != OK) {
		return false;
	}
	if (p_watch) {
		Object *object = ObjectDB::get_instance(ObjectID(p_watch));
		if (!object) {
			awaited_freed = true;
			lost_object = p_watch;
		} else {
			watch = object_watch::watch(object->_owner, [weak, ticket](uint64_t id) {
				if (const auto frame = weak.lock(); frame && frame->generation == ticket) {
					frame->awaited_freed = true;
					frame->lost_object = id;
				}
			});
			if (p_script) {
				script_changed = Signal(object, "script_changed");
				const std::function<void(const Variant &)> changed = [weak, ticket, p_watch](const Variant &) {
					if (const auto frame = weak.lock(); frame && frame->generation == ticket) {
						frame->awaited_freed = true;
						frame->lost_object = p_watch;
					}
				};
				changed_callback = Callable(memnew(ResumeCallable(changed)));
				script_changed.connect(changed_callback);
			}
		}
	}
	return true;
}

void CoroutineFrame::complete_wait(uint64_t p_generation, const Variant &p_value) {
	if (p_generation != generation || task->ended() || awaited_freed) {
		return;
	}
	Ref<CoroutineTask> keep(task);
	const Ref<CoroutineTask> completed = waited_task;
	disconnect();
	child.unref();
	if (completed.is_valid()) {
		if (!await_task(completed)) {
			task->finish(CoroutineTask::State::Failed);
			return;
		}
		if (awaited_freed) {
			return;
		}
		resume();
		return;
	}
	if (!receive(p_value)) {
		task->finish(CoroutineTask::State::Failed);
		return;
	}
	resume();
}

void CoroutineFrame::resume() {
	const auto keep_frame = shared_from_this();
	Ref<CoroutineTask> keep_task(task);
	if (task->cancel_requested && pc != 0) {
		task->finish(CoroutineTask::State::Cancelled);
		return;
	}
	task->state = CoroutineTask::State::Running;
	const TypeImage &result_type = image->types[image->module.functions[function].result.type];
	ValueBuffer result(result_type);
	const bool ok = Execution(*image, context, debugger::is_active()).run(function, data, result.data, this);
	if (task->cancel_requested) {
		task->finish(CoroutineTask::State::Cancelled);
	} else if (!ok) {
		task->finish(CoroutineTask::State::Failed);
	} else if (task->state == CoroutineTask::State::Running) {
		task->finish(CoroutineTask::State::Finished, outward_variant(result_type, result.data));
	}
}

namespace tasks {
void cancel_owner(const std::shared_ptr<InstanceStorage> &p_storage) {
	p_storage->coroutines_cancelled = true;
	for (const auto &task : task_snapshot()) {
		if (task->continuation && task->continuation->context.storage == p_storage) {
			task->cancel_owner();
		}
	}
}
void frame() {
	++task_frame;
	for (const auto &task : task_snapshot()) {
		const auto frame = task->continuation;
		if (!frame || !frame->awaited_freed || task->ended()) {
			continue;
		}
		if (frame->context.storage && frame->context.storage->coroutines_cancelled) {
			task->cancel_owner();
		} else {
			frame->report_error(vformat("The awaited object %d was freed or lost its script.", int64_t(frame->lost_object)));
			task->finish(CoroutineTask::State::Failed);
		}
	}
}
void clear() {
	for (const auto &task : task_snapshot()) {
		task->cancel();
	}
}
} // namespace tasks

namespace vm {

Variant call(const std::shared_ptr<const ClassImage> &p_image, int p_function, const InstanceContext &p_context, const Variant **p_args, int64_t p_argc, GDExtensionCallError &r_error, bool *r_failed) {
	// Old code stays alive until this call returns, even if a hot reload
	// replaces the image meanwhile (spec §12.4).
	std::shared_ptr<const ClassImage> image = p_image;
	InstanceContext context = p_context;
	if (!context.self_image) {
		context.self_image = p_image;
	}
	ClassImage::resolve_function(image, p_function);
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
	const TypeImage &result_type = image->types[fn.result.type];
	for (size_t i = 0; i < fn.params.size(); i++) {
		const bytecode::Slot &param = fn.params[i];
		const TypeImage &type = image->types[param.type];
		// Like member initializers, defaults are built late, after script types load.
		Variant value = i < size_t(p_argc) ? *p_args[i] : Variant();
		String error;
		bool valid = i < size_t(p_argc) || constant_value(image->types, fn.defaults[i - required], value, error);
		if (valid && !from_variant(type, value, frame.data + param.offset, true)) {
			error = entry_mismatch(type, value);
			valid = false;
		}
		if (!valid) {
			// A program error at the entry, and the engine gets the default value
			// (spec §8.1, §10.1); the engine's backtrace of the caller goes with it.
			report_error(*image, fn.name.c_str(), int(fn.line), vformat("Argument \"%s\" of %s(): %s.", to_godot(param.name), to_godot(fn.name), error));
			if (r_failed) {
				*r_failed = true;
			}
			return default_variant(result_type);
		}
	}

	if (fn.is_async) {
		const Ref<CoroutineTask> task = start_task(image, uint32_t(p_function), context, frame.data);
		if (r_failed) {
			*r_failed = task->state == CoroutineTask::State::Failed;
		}
		return task->ended() ? task->result : Variant(Signal(task.ptr(), "completed"));
	}
	ValueBuffer result(result_type);
	const bool ok = Execution(*image, context, debugger::is_active()).run(uint32_t(p_function), frame.data, result.data);
	if (r_failed) {
		*r_failed = !ok;
	}
	return ok ? outward_variant(result_type, result.data) : default_variant(result_type);
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
