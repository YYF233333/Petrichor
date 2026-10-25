#pragma once

#include "bytecode/module.h"
#include "bytecode/opcodes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace petrichor::compiler::ir {

// Explicit basic blocks over typed virtual registers, not SSA (architecture
// §2.1). Straight-line instructions use bytecode opcodes with registers in place
// of frame offsets; control flow lives in block terminators.

constexpr uint32_t NO_REG = 0xFFFFFFFF;

struct Instr {
	bytecode::Op op = bytecode::Op::LINE;
	uint32_t dst = NO_REG;
	uint32_t a = NO_REG;
	uint32_t b = NO_REG;
	// LINE: line. CONST_*: constant index or bool value. *_MEMBER_*: member
	// index. CALL*: function, engine call or utility index.
	uint32_t imm = 0;
	uint32_t base = NO_REG; // CALL*: the value the method is called on.
	std::vector<uint32_t> args;
};

enum class Terminator {
	None, // Still open. Lowering closes every block, so none reaches emit.
	Jump,
	Branch, // To `target` if `value` is true, else to `else_target`.
	Return,
	ReturnVoid,
};

struct Block {
	std::vector<Instr> instrs;
	Terminator terminator = Terminator::None;
	uint32_t target = 0;
	uint32_t else_target = 0;
	uint32_t value = NO_REG;
};

struct Function {
	std::string name;
	std::vector<bytecode::Slot> regs; // Parameters, then locals, then temporaries.
	uint32_t param_count = 0;
	bytecode::ValueType return_type = bytecode::ValueType::Void;
	bool is_pub = false;
	bool is_override = false;
	uint32_t line = 0;
	std::vector<Block> blocks;
};

// Lays out blocks and replaces registers with frame offsets.
bytecode::Function emit(const Function &p_function, const bytecode::Module &p_module);

} // namespace petrichor::compiler::ir
