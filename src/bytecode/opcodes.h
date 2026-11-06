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
// X(name, emit_schema, verify_schema); an emit_schema of "*" marks a call:
// `target, dst, base, argc` followed by argc (type, offset) pairs. dst and
// base are NO_SLOT when there is none; a base of NO_SLOT on CALL_ENGINE means
// self, unless the call is of a static method. CALL_METHOD calls a method of a
// class of this language on the object in base (spec §8.1), or a static one
// with no base, with the arguments and the result in their slots' own layout.
// CALL_VALUE calls the function value in base with the signature of a method
// call, whose method is "call", or "callv" with the Array of the arguments as
// its one argument; a program error in the function aborts the caller (spec
// §8.1). MAKE_LAMBDA has this form too: the lambda's function, the Callable it
// makes, no base, and the values it captures, each of the type of its capture
// parameter (spec §3.11).
//
// In emit_schema, each character encodes one operand word from ir::Instr:
// D=dst, A=a, B=b, I=imm, T=type, F=field, M=member_offset, R=regs[dst].type,
// S=regs[a].type. In verify_schema, a leading '@' marks an instruction that
// always uses the instance, and each following character checks the operand at
// that position: i/f/b/s/o/v/c/g (slot of Int/Float/Bool/String/Object/Variant/
// Callable/Signal), S/O (slot_or_none of String/Object), n (String member),
// t (type_operand), l/m/k (slot_like/member_like/constant_like of o[0]),
// C/M/F/P/E/R (index in classes/members/functions/properties/fields/operators),
// j (jump target), or '.' (checked by the instruction's own rule).
#define PETRICHOR_OPCODES(X) \
	X(LINE, "I", "") \
	X(CONST_INT, "DI", "i.") \
	X(CONST_FLOAT, "DI", "f.") \
	X(CONST_BOOL, "DI", "b.") \
	X(CONST_STRING, "DI", "s.") \
	X(CONST_VALUE, "TDI", "tlk") /* type, dst, constant */ \
	X(MOVE_8, "DA", "") \
	X(MOVE_STRING, "DA", "ss") \
	X(MOVE_VALUE, "TDA", "tll") /* type, dst, src */ \
	X(LOAD_MEMBER_8, "DM", "@") \
	X(LOAD_MEMBER_STRING, "DM", "@sn") \
	X(LOAD_MEMBER_VALUE, "TDM", "@tlm") /* type, dst, member */ \
	X(STORE_MEMBER_8, "MA", "@") \
	X(STORE_MEMBER_STRING, "MA", "@ns") \
	X(STORE_MEMBER_VALUE, "TMA", "@tml") /* type, member, src */ \
	X(LOAD_STATIC, "TDFI", "tl") /* type, dst, class (NO_SLOT: this one), static variable: the class is initialized first, and a variable not yet initialized is a program error (spec §4.8) */ \
	X(STORE_STATIC, "TFIA", "t..l") /* type, class (NO_SLOT: this one), static variable, src: as LOAD_STATIC */ \
	X(INIT_STATIC, "TIA", "t.l") /* type, static variable, src: in the static initializer, which counts the variable as initialized then */ \
	X(INT_TO_FLOAT, "DA", "fi") \
	X(FLOAT_TO_INT, "DA", "if") \
	X(ADD_INT, "DAB", "iii") \
	X(SUB_INT, "DAB", "iii") \
	X(MUL_INT, "DAB", "iii") \
	X(DIV_INT, "DAB", "iii") \
	X(MOD_INT, "DAB", "iii") \
	X(NEG_INT, "DA", "ii") \
	X(AND_INT, "DAB", "iii") \
	X(OR_INT, "DAB", "iii") \
	X(XOR_INT, "DAB", "iii") \
	X(NOT_INT, "DA", "ii") \
	X(SHL_INT, "DAB", "iii") /* a negative operand or an amount of 64 or more is a program error (spec §5.1) */ \
	X(SHR_INT, "DAB", "iii") \
	X(POW_INT, "DAB", "iii") /* int_power(); 0 to a negative power is a program error (spec §5.1) */ \
	X(ADD_FLOAT, "DAB", "fff") \
	X(SUB_FLOAT, "DAB", "fff") \
	X(MUL_FLOAT, "DAB", "fff") \
	X(DIV_FLOAT, "DAB", "fff") \
	X(POW_FLOAT, "DAB", "fff") \
	X(NEG_FLOAT, "DA", "ff") \
	X(ADD_STRING, "DAB", "sss") \
	X(FORMAT, "DAB", "ssv") /* dst (String), format (String), values (Variant): the engine's `%`, a format error a program error (spec §5.1) */ \
	X(EQ_INT, "DAB", "bii") \
	X(NE_INT, "DAB", "bii") \
	X(LT_INT, "DAB", "bii") \
	X(LE_INT, "DAB", "bii") \
	X(GT_INT, "DAB", "bii") \
	X(GE_INT, "DAB", "bii") \
	X(EQ_FLOAT, "DAB", "bff") \
	X(NE_FLOAT, "DAB", "bff") \
	X(LT_FLOAT, "DAB", "bff") \
	X(LE_FLOAT, "DAB", "bff") \
	X(GT_FLOAT, "DAB", "bff") \
	X(GE_FLOAT, "DAB", "bff") \
	X(EQ_BOOL, "DAB", "bbb") \
	X(NE_BOOL, "DAB", "bbb") \
	X(EQ_STRING, "DAB", "bss") \
	X(NE_STRING, "DAB", "bss") \
	X(NOT, "DA", "bb") \
	X(EVALUATE, "IDAB", "R") /* operator, dst, left, right (NO_SLOT: unary) */ \
	X(GET_FIELD, "IDA", "E") /* field, dst, base */ \
	X(SET_FIELD, "IAB", "E") /* field, base, value */ \
	X(GET_PROPERTY, "IDA", "P") /* property, dst, object (NO_SLOT: self) */ \
	X(HAS_MEMBER, "DAB", "bos") /* dst (bool), object, name (String): whether the object's get() finds it (spec §5.1) */ \
	X(CHECK_ENUM, "IA", "ti") /* type, src: a program error unless the int in src is one of the enum's values (spec §3.7) */ \
	X(SET_PROPERTY, "IAB", "P") /* property, object (NO_SLOT: self), value */ \
	X(NEW_OBJECT, "ID", "Co") /* class, dst: of a class of this language, the engine class with the script */ \
	X(NEW_WITHOUT_INIT, "ID", "Co") /* class (of this language), dst: as NEW_OBJECT, but _init() is left to the CALL_METHOD on dst that follows, skipped when the initializers fail (spec §6.1, §8.1) */ \
	X(GET_SCRIPT, "ID", "Co") /* class, dst: the script of a class of this language, also of an inner class (spec §4.1) */ \
	X(GET_SINGLETON, "ID", "Co") /* class, dst */ \
	X(GET_AUTOLOAD, "ID", ".o") /* name constant, dst */ \
	X(METHOD_VALUE, "ID", "@Fc") /* function, dst (Callable) */ \
	X(GET_SIGNAL, "IDA", ".gO") /* name constant, dst, object (NO_SLOT: self) */ \
	X(CHECK_OBJECT, "IA", "Co") /* class, slot: not None, of the class */ \
	X(CHECK_LIVE, "A", "o") /* slot: an object the engine takes is not freed */ \
	X(CHECK_ASSIGNED, "IA", "@Mo") /* member, slot: an export is assigned */ \
	X(CHECK_RANGE, "IA", ".i") /* IntRange, slot (int): fits the engine's */ \
	X(DEBUG_BUILD, "D", "b") /* dst (bool): whether this is a debug build, the only one where assert runs (spec §8.3) */ \
	X(ASSERT_FAIL, "A", "S") /* message (String, NO_SLOT: none): the program error of a failed assert */ \
	X(CAST_OBJECT, "IDA", "Coo") /* class, dst, src: `as`, None if it fails */ \
	X(IS_INSTANCE, "IDA", "Cbo") /* class, dst (bool), src: `is` */ \
	X(IS_NONE, "TDA", "tbl") /* type of src, dst (bool), src (an object or T?) */ \
	X(SOME, "TDA", ".l") /* type of dst (T?), dst, src (T): Some(src) (spec §3.4) */ \
	X(EQUAL, "TDAB", "tbll") /* type, dst (bool), a, b: a == b, a struct field by field (spec §3.6) */ \
	X(LESS, "TDAB", ".bll") /* type, dst (bool), a, b: a < b of a struct, field by field in order, of a T?, None first, or of a container that keeps boxes, element by element (spec §5.2) */ \
	X(TO_VARIANT, "TDA", "tvl") /* type of src, dst (Variant), src */ \
	X(FROM_VARIANT, "TDA", "tlv") /* type of dst, dst, src (Variant): a value that is of the type, as a narrowed local is (spec §5.4) */ \
	X(VARIANT_OBJECT, "DA", "ov") /* dst (Object), src (Variant): the object in it, None for any other value (spec §5.3) */ \
	X(IS_TYPE, "TDA", ".bv") /* type, dst (bool), src (Variant): `is` of a built-in type, a container only of exactly that type (spec §3.8, §5.4) */ \
	X(CONSTRUCT_FROM, "TDA", ".lv") /* type of dst, dst, src (Variant): `T(v)`, the engine's construction, a value it cannot convert a program error (spec §3.13) */ \
	X(SAME_OBJECT, "IDAB", ".boo") /* T? sides (bit 0: a, bit 1: b), dst (bool), a, b (Object): the same object by ObjectID, a freed one on a T? side being None (spec §3.3, §5.2) */ \
	X(VARIANT_EQUAL, "DAB", "bvv") /* dst (bool), a, b (Variant): `==` by the engine's operator table, a pair not in it a program error (spec §5.2) */ \
	X(MATCHES, "DAB", "bvv") /* dst (bool), value, pattern (Variant): of the pattern's type, String and StringName alike, and equal to it (spec §5.6) */ \
	X(BOX, "TDAI", ".vl") /* type, dst (Variant), src, key (0 or 1): src in a box, as a container keeps a boxed type (module.h); a key's box hashes the value */ \
	X(ENCODE, "TDA", "..l") /* type of src, dst, src: a container that keeps boxes as the untyped container of its kind, a copy with every key and element encoded (spec §3.8, §10.1) */ \
	X(UNWRAP, "TDAB", "tl.S") /* type of dst, dst, src (T?), message (NO_SLOT: default) */ \
	X(NEW_CONTAINER, "TD", "t") /* type, dst: an empty Array or Dictionary, typed as the type says, a boxed type as Callable */ \
	X(GET_INDEX, "SRDAB", "") /* type of base, type of dst, dst, base, index (an int, or a Dictionary's key, a Variant when boxed): a box is opened, a container read is checked against its type (spec §3.8) */ \
	X(SET_INDEX, "RDAB", "") /* type of base, base, index, src (a Variant when boxed): a read-only base, an index out of bounds is a program error; a new Dictionary key is stored as a key (spec §3.8) */ \
	X(JUMP, "I", "j") \
	X(JUMP_IF_FALSE, "AI", "bj") \
	X(JUMP_IF_TRUE, "AI", "bj") \
	X(CALL, "*", "") \
	X(CALL_SUPER, "*", "") \
	X(AWAIT_TASK, "DAI", ".o") /* dst, task, owned (0 or 1) */ \
	X(AWAIT_SIGNAL, "DA", ".g") /* dst, signal */ \
	X(TASK_CANCEL, "A", "o") /* task */ \
	X(GET_OWNER, "ID", "@Co") /* class, dst: the owner that @owner requires (spec §7.3) */ \
	X(GET_SELF, "D", "@o") /* dst */ \
	X(CALL_AWAIT_ENGINE, "*", "") \
	X(CALL_AWAIT_BUILTIN, "*", "") \
	X(CALL_ENGINE, "*", "") \
	X(CALL_METHOD, "*", "") \
	X(CALL_UTILITY, "*", "") \
	X(CALL_BUILTIN, "*", "") \
	X(CONSTRUCT, "*", "") \
	X(MAKE_LAMBDA, "*", "") \
	X(CALL_VALUE, "*", "") \
	X(RETURN, "A", "") \
	X(RETURN_VOID, "", "")

enum class Op : uint32_t {
#define PETRICHOR_OPCODE_ENUM(m_name, m_emit, m_verify) m_name,
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

// wrapi(), as the compiler folds it and the runtime calls it: the value moved
// into [min, max) or (max, min], computed unsigned so that no span overflows.
inline int64_t int_wrapi(int64_t p_value, int64_t p_min, int64_t p_max) {
	if (p_min == p_max) {
		return p_min;
	}
	const bool up = p_min < p_max;
	const uint64_t span = up ? uint64_t(p_max) - uint64_t(p_min) : uint64_t(p_min) - uint64_t(p_max);
	const uint64_t rem = (p_value >= p_min ? uint64_t(p_value) - uint64_t(p_min) : uint64_t(p_min) - uint64_t(p_value)) % span;
	const uint64_t step = (up == (p_value >= p_min) || rem == 0) ? rem : span - rem;
	return int64_t(up ? uint64_t(p_min) + step : uint64_t(p_min) - step);
}

// As the engine API names it: "int8", "uint32", "char32"…
const char *int_range_name(IntRange p_range);

// Number of operand words of a fixed-size instruction, or -1 for a call.
int operand_count(Op p_op);
const char *op_name(Op p_op);
const char *op_emit_schema(Op p_op);
const char *op_verify_schema(Op p_op);

// The words of the instruction at `p_code`, opcode included. A call's length
// depends on its argument count, p_code[4], which must be there.
inline size_t instruction_length(const uint32_t *p_code) {
	const int count = operand_count(Op(p_code[0]));
	return count < 0 ? 5 + 2 * size_t(p_code[4]) : 1 + size_t(count);
}

} // namespace petrichor::bytecode
