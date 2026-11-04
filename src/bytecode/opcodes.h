#pragma once

#include <cstddef>
#include <cstdint>

namespace petrichor::bytecode {

// Every instruction is an opcode word followed by operand words. Operands are
// byte offsets into the frame (or into the instance, for members), indices
// into the module's tables, or immediate values; an operand named `type` is an
// index into the type table. A field of a struct in a slot is a slot of its
// own at the struct's offset plus the field's (module.h). The list is the
// single definition from which the compiler emits and the virtual machine
// dispatches (architecture §2.2).
//
// X(name, operand count); a count of -1 marks a call: `target, dst, base,
// argc` followed by argc (type, offset) pairs. dst and base are NO_SLOT when
// there is none; a base of NO_SLOT on CALL_ENGINE means self, unless the call
// is of a static method. CALL_METHOD calls a method of a class of this language
// on the object in base (spec §8.1), or a static one with no base, with the
// arguments and the result in their slots' own layout. CALL_VALUE calls the
// function value in base with the signature of a method call, whose method is
// "call", or "callv" with the Array of the arguments as its one argument; a
// program error in the function aborts the caller (spec §8.1). MAKE_LAMBDA has
// this form too: the lambda's function, the Callable it makes, no base, and the
// values it captures, each of the type of its capture parameter (spec §3.11).
#define PETRICHOR_OPCODES(X) \
	X(LINE, 1) \
	X(CONST_INT, 2) \
	X(CONST_FLOAT, 2) \
	X(CONST_BOOL, 2) \
	X(CONST_STRING, 2) \
	X(CONST_VALUE, 3) /* type, dst, constant */ \
	X(MOVE_8, 2) \
	X(MOVE_STRING, 2) \
	X(MOVE_VALUE, 3) /* type, dst, src */ \
	X(LOAD_MEMBER_8, 2) \
	X(LOAD_MEMBER_STRING, 2) \
	X(LOAD_MEMBER_VALUE, 3) /* type, dst, member */ \
	X(STORE_MEMBER_8, 2) \
	X(STORE_MEMBER_STRING, 2) \
	X(STORE_MEMBER_VALUE, 3) /* type, member, src */ \
	X(LOAD_STATIC, 4) /* type, dst, class (NO_SLOT: this one), static variable: the class is initialized first, and a variable not yet initialized is a program error (spec §4.8) */ \
	X(STORE_STATIC, 4) /* type, class (NO_SLOT: this one), static variable, src: as LOAD_STATIC */ \
	X(INIT_STATIC, 3) /* type, static variable, src: in the static initializer, which counts the variable as initialized then */ \
	X(INT_TO_FLOAT, 2) \
	X(FLOAT_TO_INT, 2) \
	X(ADD_INT, 3) \
	X(SUB_INT, 3) \
	X(MUL_INT, 3) \
	X(DIV_INT, 3) \
	X(MOD_INT, 3) \
	X(NEG_INT, 2) \
	X(AND_INT, 3) \
	X(OR_INT, 3) \
	X(XOR_INT, 3) \
	X(NOT_INT, 2) \
	X(SHL_INT, 3) /* a negative operand or an amount of 64 or more is a program error (spec §5.1) */ \
	X(SHR_INT, 3) \
	X(POW_INT, 3) /* int_power(); 0 to a negative power is a program error (spec §5.1) */ \
	X(ADD_FLOAT, 3) \
	X(SUB_FLOAT, 3) \
	X(MUL_FLOAT, 3) \
	X(DIV_FLOAT, 3) \
	X(POW_FLOAT, 3) \
	X(NEG_FLOAT, 2) \
	X(ADD_STRING, 3) \
	X(FORMAT, 3) /* dst (String), format (String), values (Variant): the engine's `%`, a format error a program error (spec §5.1) */ \
	X(EQ_INT, 3) \
	X(NE_INT, 3) \
	X(LT_INT, 3) \
	X(LE_INT, 3) \
	X(GT_INT, 3) \
	X(GE_INT, 3) \
	X(EQ_FLOAT, 3) \
	X(NE_FLOAT, 3) \
	X(LT_FLOAT, 3) \
	X(LE_FLOAT, 3) \
	X(GT_FLOAT, 3) \
	X(GE_FLOAT, 3) \
	X(EQ_BOOL, 3) \
	X(NE_BOOL, 3) \
	X(EQ_STRING, 3) \
	X(NE_STRING, 3) \
	X(NOT, 2) \
	X(EVALUATE, 4) /* operator, dst, left, right (NO_SLOT: unary) */ \
	X(GET_FIELD, 3) /* field, dst, base */ \
	X(SET_FIELD, 3) /* field, base, value */ \
	X(GET_PROPERTY, 3) /* property, dst, object (NO_SLOT: self) */ \
	X(HAS_MEMBER, 3) /* dst (bool), object, name (String): whether the object's get() finds it (spec §5.1) */ \
	X(CHECK_ENUM, 2) /* type, src: a program error unless the int in src is one of the enum's values (spec §3.7) */ \
	X(SET_PROPERTY, 3) /* property, object (NO_SLOT: self), value */ \
	X(NEW_OBJECT, 2) /* class, dst: of a class of this language, the engine class with the script */ \
	X(GET_SCRIPT, 2) /* class, dst: the script of a class of this language, also of an inner class (spec §4.1) */ \
	X(GET_SINGLETON, 2) /* class, dst */ \
	X(GET_AUTOLOAD, 2) /* name constant, dst */ \
	X(METHOD_VALUE, 2) /* function, dst (Callable) */ \
	X(GET_SIGNAL, 3) /* name constant, dst, object (NO_SLOT: self) */ \
	X(CHECK_OBJECT, 2) /* class, slot: not None, of the class */ \
	X(CHECK_LIVE, 1) /* slot: an object the engine takes is not freed */ \
	X(CHECK_ASSIGNED, 2) /* member, slot: an export is assigned */ \
	X(CHECK_RANGE, 2) /* IntRange, slot (int): fits the engine's */ \
	X(DEBUG_BUILD, 1) /* dst (bool): whether this is a debug build, the only one where assert runs (spec §8.3) */ \
	X(ASSERT_FAIL, 1) /* message (String, NO_SLOT: none): the program error of a failed assert */ \
	X(CAST_OBJECT, 3) /* class, dst, src: `as`, None if it fails */ \
	X(IS_INSTANCE, 3) /* class, dst (bool), src: `is` */ \
	X(IS_NONE, 3) /* type of src, dst (bool), src (an object or T?) */ \
	X(SOME, 3) /* type of dst (T?), dst, src (T): Some(src) (spec §3.4) */ \
	X(EQUAL, 4) /* type, dst (bool), a, b: a == b, a struct field by field (spec §3.6) */ \
	X(LESS, 4) /* type, dst (bool), a, b: a < b of a struct, field by field in order, of a T?, None first, or of a container that keeps boxes, element by element (spec §5.2) */ \
	X(TO_VARIANT, 3) /* type of src, dst (Variant), src */ \
	X(BOX, 4) /* type, dst (Variant), src, key (0 or 1): src in a box, as a container keeps a boxed type (module.h); a key's box hashes the value */ \
	X(ENCODE, 3) /* type of src, dst, src: a container that keeps boxes as the untyped container of its kind, a copy with every key and element encoded (spec §3.8, §10.1) */ \
	X(UNWRAP, 4) /* type of dst, dst, src (T?), message (NO_SLOT: default) */ \
	X(NEW_CONTAINER, 2) /* type, dst: an empty Array or Dictionary, typed as the type says, a boxed type as Callable */ \
	X(GET_INDEX, 5) /* type of base, type of dst, dst, base, index (an int, or a Dictionary's key, a Variant when boxed): a box is opened, a container read is checked against its type (spec §3.8) */ \
	X(SET_INDEX, 4) /* type of base, base, index, src (a Variant when boxed): a read-only base, an index out of bounds is a program error; a new Dictionary key is stored as a key (spec §3.8) */ \
	X(JUMP, 1) \
	X(JUMP_IF_FALSE, 2) \
	X(JUMP_IF_TRUE, 2) \
	X(CALL, -1) \
	X(CALL_SUPER, -1) \
	X(AWAIT_TASK, 3) /* dst, task, owned (0 or 1) */ \
	X(AWAIT_SIGNAL, 2) /* dst, signal */ \
	X(TASK_CANCEL, 1) /* task */ \
	X(GET_OWNER, 2) /* class, dst: the owner that @owner requires (spec §7.3) */ \
	X(GET_SELF, 1) /* dst */ \
	X(CALL_AWAIT_ENGINE, -1) \
	X(CALL_AWAIT_BUILTIN, -1) \
	X(CALL_ENGINE, -1) \
	X(CALL_METHOD, -1) \
	X(CALL_UTILITY, -1) \
	X(CALL_BUILTIN, -1) \
	X(CONSTRUCT, -1) \
	X(MAKE_LAMBDA, -1) \
	X(CALL_VALUE, -1) \
	X(RETURN, 1) \
	X(RETURN_VOID, 0)

enum class Op : uint32_t {
#define PETRICHOR_OPCODE_ENUM(m_name, m_count) m_name,
	PETRICHOR_OPCODES(PETRICHOR_OPCODE_ENUM)
#undef PETRICHOR_OPCODE_ENUM
			COUNT
};

constexpr uint32_t NO_SLOT = 0xFFFFFFFF;

// An engine integer narrower than int64 or unsigned, which the engine would
// truncate or wrap (spec §10.6): the first operand of CHECK_RANGE.
enum class IntRange : uint32_t {
	INT8,
	INT16,
	INT32,
	UINT8,
	UINT16,
	UINT32,
	UINT64,
	CHAR32, // A char32_t, unsigned 32 bits.
	COUNT,
};

inline bool int_in_range(IntRange p_range, int64_t p_value) {
	switch (p_range) {
		case IntRange::INT8:
			return p_value >= INT8_MIN && p_value <= INT8_MAX;
		case IntRange::INT16:
			return p_value >= INT16_MIN && p_value <= INT16_MAX;
		case IntRange::INT32:
			return p_value >= INT32_MIN && p_value <= INT32_MAX;
		case IntRange::UINT8:
			return p_value >= 0 && p_value <= UINT8_MAX;
		case IntRange::UINT16:
			return p_value >= 0 && p_value <= UINT16_MAX;
		case IntRange::UINT32:
		case IntRange::CHAR32:
			return p_value >= 0 && p_value <= int64_t(UINT32_MAX);
		case IntRange::UINT64:
			return p_value >= 0;
		default:
			return false;
	}
}

// `a ** n` of two ints (spec §5.1), as POW_INT computes it and the compiler
// in constants: exact, wrapping as `*` does; for a negative n, 1 divided by
// a ** n truncated toward zero. False for 0 to a negative power.
inline bool int_power(int64_t p_base, int64_t p_exponent, int64_t &r_result) {
	if (p_exponent < 0) {
		if (p_base == 0) {
			return false;
		}
		r_result = p_base == 1 ? 1 : (p_base == -1 ? ((p_exponent & 1) ? -1 : 1) : 0);
		return true;
	}
	uint64_t result = 1;
	uint64_t base = uint64_t(p_base);
	for (uint64_t exponent = uint64_t(p_exponent); exponent != 0; exponent >>= 1) {
		if (exponent & 1) {
			result *= base;
		}
		base *= base;
	}
	r_result = int64_t(result);
	return true;
}

// As the engine API names it: "int8", "uint32", "char32"…
const char *int_range_name(IntRange p_range);

// Number of operand words of a fixed-size instruction, or -1 for a call.
int operand_count(Op p_op);
const char *op_name(Op p_op);

// The words of the instruction at `p_code`, opcode included. A call's length
// depends on its argument count, p_code[4], which must be there.
inline size_t instruction_length(const uint32_t *p_code) {
	const int count = operand_count(Op(p_code[0]));
	return count < 0 ? 5 + 2 * size_t(p_code[4]) : 1 + size_t(count);
}

} // namespace petrichor::bytecode
