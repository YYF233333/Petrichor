#include "runtime/vm.h"

#include "bytecode/opcodes.h"

#include <godot_cpp/godot.hpp>

#include <cstring>
#include <new>

using namespace godot;

namespace petrichor {

using bytecode::Op;
using bytecode::ValueType;

namespace {

constexpr uint32_t NO_SLOT = 0xFFFFFFFF;

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

// A single slot that holds a value of any type, for return values.
class ValueSlot {
	alignas(8) uint8_t storage[8];
	ValueType type;

public:
	explicit ValueSlot(ValueType p_type) :
			type(p_type) {
		std::memset(storage, 0, sizeof(storage));
		if (type == ValueType::String) {
			new (storage) String();
		}
	}
	~ValueSlot() {
		if (type == ValueType::String) {
			reinterpret_cast<String *>(storage)->~String();
		}
	}
	uint8_t *data() { return storage; }
};

// The variadic utility functions all share one signature hash.
constexpr GDExtensionInt VARARG_UTILITY_HASH = 2648703342;

GDExtensionPtrUtilityFunction utility_pointer(const char *p_name) {
	const StringName name = p_name;
	return godot::gdextension_interface::variant_get_ptr_utility_function(name._native_ptr(), VARARG_UTILITY_HASH);
}

void call_utility(Utility p_utility, const Variant **p_args, GDExtensionInt p_count) {
	static const GDExtensionPtrUtilityFunction pointers[] = {
		utility_pointer("print"),
		utility_pointer("prints"),
		utility_pointer("printerr"),
		utility_pointer("push_error"),
		utility_pointer("push_warning"),
	};
	Variant result;
	pointers[int(p_utility)](&result, reinterpret_cast<GDExtensionConstVariantPtr *>(p_args), int(p_count));
}

void report_error(const ClassImage &p_image, const bytecode::Function &p_function, int p_line, const String &p_message) {
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
		const uint32_t *code = fn.code.data();
		uint8_t *members = context.members;
		const size_t depth = call_stack.size();
		call_stack.push_back({ &image, &fn, frame, members, int(fn.line) });
		struct Pop {
			~Pop() { call_stack.pop_back(); }
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

		size_t pc = 0;
		while (true) {
			switch (Op(code[pc])) {
				case Op::LINE:
					call_stack[depth].line = int(code[pc + 1]);
					pc += 2;
					break;
				case Op::CONST_INT:
					I64(code[pc + 1]) = image.module.constants[code[pc + 2]].i;
					pc += 3;
					break;
				case Op::CONST_FLOAT:
					F64(code[pc + 1]) = image.module.constants[code[pc + 2]].f;
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
				case Op::MOVE_8:
					U64(code[pc + 1]) = U64(code[pc + 2]);
					pc += 3;
					break;
				case Op::MOVE_STRING:
					STR(code[pc + 1]) = STR(code[pc + 2]);
					pc += 3;
					break;
				case Op::LOAD_MEMBER_8:
					std::memcpy(frame + code[pc + 1], members + code[pc + 2], 8);
					pc += 3;
					break;
				case Op::LOAD_MEMBER_STRING:
					STR(code[pc + 1]) = *reinterpret_cast<String *>(members + code[pc + 2]);
					pc += 3;
					break;
				case Op::STORE_MEMBER_8:
					std::memcpy(members + code[pc + 1], frame + code[pc + 2], 8);
					pc += 3;
					break;
				case Op::STORE_MEMBER_STRING:
					*reinterpret_cast<String *>(members + code[pc + 1]) = STR(code[pc + 2]);
					pc += 3;
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
				case Op::JUMP:
					pc = code[pc + 1];
					break;
				case Op::JUMP_IF_FALSE:
					pc = U64(code[pc + 1]) ? pc + 3 : code[pc + 2];
					break;
				case Op::JUMP_IF_TRUE:
					pc = U64(code[pc + 1]) ? code[pc + 2] : pc + 3;
					break;
				case Op::CALL: {
					const uint32_t target = code[pc + 1];
					const uint32_t dst = code[pc + 2];
					const uint32_t argc = code[pc + 3];
					const bytecode::Function &callee = image.module.functions[target];
					FrameMemory callee_frame(callee.frame_size);
					construct_slots(callee_frame.data, callee.frame_size, callee.slots);
					for (uint32_t i = 0; i < argc; i++) {
						copy_value(callee.params[i].type, callee_frame.data + callee.params[i].offset, frame + code[pc + 5 + 2 * i]);
					}
					const bool ok = run(target, callee_frame.data, dst == NO_SLOT ? nullptr : frame + dst);
					destroy_slots(callee_frame.data, callee.slots);
					if (!ok) {
						return false;
					}
					pc += 4 + 2 * argc;
					break;
				}
				case Op::CALL_ENGINE:
				case Op::CALL_UTILITY: {
					const bool engine = Op(code[pc]) == Op::CALL_ENGINE;
					const uint32_t target = code[pc + 1];
					const uint32_t dst = code[pc + 2];
					const uint32_t argc = code[pc + 3];
					std::vector<Variant> args(argc);
					std::vector<const Variant *> arg_pointers(argc);
					for (uint32_t i = 0; i < argc; i++) {
						args[i] = to_variant(ValueType(code[pc + 4 + 2 * i]), frame + code[pc + 5 + 2 * i]);
						arg_pointers[i] = &args[i];
					}
					if (engine) {
						// Dynamic for now; calls through MethodBind pointers come
						// once the compiler knows method hashes (architecture §2.2).
						Variant self(context.owner);
						Variant result;
						GDExtensionCallError error;
						self.callp(image.engine_methods[target], arg_pointers.data(), int(argc), result, error);
						if (error.error != GDEXTENSION_CALL_OK) {
							FAIL(vformat("Calling %s() on %s failed.", image.engine_methods[target], context.owner->get_class()));
						}
						if (dst != NO_SLOT && !from_variant(image.module.engine_calls[target].return_type, result, frame + dst)) {
							FAIL(vformat("%s() returned an unexpected %s.", image.engine_methods[target], Variant::get_type_name(result.get_type())));
						}
					} else {
						call_utility(image.utilities[target], arg_pointers.data(), GDExtensionInt(argc));
					}					pc += 4 + 2 * argc;
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
	if (size_t(p_argc) != fn.params.size()) {
		r_error.error = size_t(p_argc) > fn.params.size() ? GDEXTENSION_CALL_ERROR_TOO_MANY_ARGUMENTS : GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS;
		r_error.expected = int32_t(fn.params.size());
		return Variant();
	}

	FrameMemory frame(fn.frame_size);
	construct_slots(frame.data, fn.frame_size, fn.slots);
	for (size_t i = 0; i < fn.params.size(); i++) {
		if (!from_variant(fn.params[i].type, *p_args[i], frame.data + fn.params[i].offset)) {
			destroy_slots(frame.data, fn.slots);
			// The caller reports this with its own call site, as for engine methods.
			r_error.error = GDEXTENSION_CALL_ERROR_INVALID_ARGUMENT;
			r_error.argument = int32_t(i);
			r_error.expected = GDExtensionVariantType(variant_type(fn.params[i].type));
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
