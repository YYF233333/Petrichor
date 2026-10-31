#pragma once

#include "bytecode/module.h"
#include "bytecode/opcodes.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace petrichor::compiler::ir {

// Explicit basic blocks over typed virtual registers, not SSA (architecture
// §2.1). Straight-line instructions use bytecode opcodes with registers in place
// of frame offsets; control flow lives in block terminators.

constexpr uint32_t NO_REG = 0xFFFFFFFF;
static_assert(NO_REG == bytecode::NO_SLOT, "a missing register is emitted as a missing slot");

// Operands in bytecode order (opcodes.h): `type` for instructions that take a
// type, `imm` for a table index, a constant, a member or a line, then `dst`,
// `a` and `b` as the instruction uses them.
struct Instr {
	bytecode::Op op = bytecode::Op::LINE;
	uint32_t type = 0; // In the module's type table.
	uint32_t imm = 0;
	uint32_t dst = NO_REG;
	uint32_t a = NO_REG;
	uint32_t b = NO_REG;
	uint32_t base = NO_REG; // CALL*: the value the method is called on.
	std::vector<uint32_t> args;
	uint32_t field = 0; // Members: where a field of a struct starts in the member.
};

// A register that is a field of a struct in another register: it has no
// slot of its own but the bytes of `root` from `offset` (architecture §2.2).
// Writing it writes part of the root, so whatever reasons about registers
// takes it as part of its root.
struct View {
	uint32_t root = NO_REG;
	uint32_t offset = 0;
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
	std::unordered_map<uint32_t, View> views; // By register; a root comes before its views.
	uint32_t param_count = 0;
	bytecode::Slot result; // As bytecode::Function::result.
	bool is_async = false;
	bool is_pub = false;
	bool is_override = false;
	bool is_exposed = false;
	uint32_t line = 0;
	std::vector<Block> blocks;
};

// Lays out blocks and replaces registers with frame offsets.
bytecode::Function emit(const Function &p_function, const bytecode::Module &p_module);

} // namespace petrichor::compiler::ir
