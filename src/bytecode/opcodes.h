#pragma once

#include <cstdint>

namespace petrichor::bytecode {

// Every instruction is an opcode word followed by operand words. Operands are
// byte offsets into the frame (or into the instance, for members), indices
// into the module's tables, value types, or immediate values. The list is the
// single definition from which the compiler emits and the virtual machine
// dispatches (architecture §2.2).
//
// X(name, operand count); a count of -1 marks a call: `target, dst, base,
// argc` followed by argc (type, offset) pairs. dst and base are NO_SLOT when
// there is none; a base of NO_SLOT on CALL_ENGINE means self.
#define PETRICHOR_OPCODES(X)                                         \
	X(LINE, 1)                                                       \
	X(CONST_INT, 2)                                                  \
	X(CONST_FLOAT, 2)                                                \
	X(CONST_BOOL, 2)                                                 \
	X(CONST_STRING, 2)                                               \
	X(CONST_VALUE, 3) /* type, dst, constant */                      \
	X(MOVE_8, 2)                                                     \
	X(MOVE_STRING, 2)                                                \
	X(MOVE_VALUE, 3) /* type, dst, src */                            \
	X(LOAD_MEMBER_8, 2)                                              \
	X(LOAD_MEMBER_STRING, 2)                                         \
	X(LOAD_MEMBER_VALUE, 3) /* type, dst, member */                  \
	X(STORE_MEMBER_8, 2)                                             \
	X(STORE_MEMBER_STRING, 2)                                        \
	X(STORE_MEMBER_VALUE, 3) /* type, member, src */                 \
	X(INT_TO_FLOAT, 2)                                               \
	X(FLOAT_TO_INT, 2)                                               \
	X(ADD_INT, 3)                                                    \
	X(SUB_INT, 3)                                                    \
	X(MUL_INT, 3)                                                    \
	X(DIV_INT, 3)                                                    \
	X(MOD_INT, 3)                                                    \
	X(NEG_INT, 2)                                                    \
	X(ADD_FLOAT, 3)                                                  \
	X(SUB_FLOAT, 3)                                                  \
	X(MUL_FLOAT, 3)                                                  \
	X(DIV_FLOAT, 3)                                                  \
	X(NEG_FLOAT, 2)                                                  \
	X(ADD_STRING, 3)                                                 \
	X(EQ_INT, 3)                                                     \
	X(NE_INT, 3)                                                     \
	X(LT_INT, 3)                                                     \
	X(LE_INT, 3)                                                     \
	X(GT_INT, 3)                                                     \
	X(GE_INT, 3)                                                     \
	X(EQ_FLOAT, 3)                                                   \
	X(NE_FLOAT, 3)                                                   \
	X(LT_FLOAT, 3)                                                   \
	X(LE_FLOAT, 3)                                                   \
	X(GT_FLOAT, 3)                                                   \
	X(GE_FLOAT, 3)                                                   \
	X(EQ_BOOL, 3)                                                    \
	X(NE_BOOL, 3)                                                    \
	X(EQ_STRING, 3)                                                  \
	X(NE_STRING, 3)                                                  \
	X(NOT, 2)                                                        \
	X(EVALUATE, 4) /* operator, dst, left, right (NO_SLOT: unary) */ \
	X(GET_FIELD, 3) /* field, dst, base */                           \
	X(SET_FIELD, 3) /* field, base, value */                         \
	X(GET_PROPERTY, 3) /* property, dst, object (NO_SLOT: self) */   \
	X(SET_PROPERTY, 3) /* property, object (NO_SLOT: self), value */ \
	X(NEW_OBJECT, 2) /* class, dst */                                \
	X(JUMP, 1)                                                       \
	X(JUMP_IF_FALSE, 2)                                              \
	X(JUMP_IF_TRUE, 2)                                               \
	X(CALL, -1)                                                      \
	X(CALL_ENGINE, -1)                                               \
	X(CALL_UTILITY, -1)                                              \
	X(CALL_BUILTIN, -1)                                              \
	X(CONSTRUCT, -1)                                                 \
	X(RETURN, 1)                                                     \
	X(RETURN_VOID, 0)

enum class Op : uint32_t {
#define PETRICHOR_OPCODE_ENUM(m_name, m_count) m_name,
	PETRICHOR_OPCODES(PETRICHOR_OPCODE_ENUM)
#undef PETRICHOR_OPCODE_ENUM
	COUNT
};

constexpr uint32_t NO_SLOT = 0xFFFFFFFF;

// Number of operand words of a fixed-size instruction, or -1 for a call.
int operand_count(Op p_op);
const char *op_name(Op p_op);

} // namespace petrichor::bytecode
