#include "compiler/ir.h"

namespace petrichor::compiler::ir {

using bytecode::Op;

namespace {

uint32_t offset_of(uint32_t p_reg) {
	return p_reg == NO_REG ? NO_REG : p_reg * 8;
}

bool is_member_op(Op p_op) {
	return p_op == Op::LOAD_MEMBER_8 || p_op == Op::LOAD_MEMBER_STRING || p_op == Op::STORE_MEMBER_8 || p_op == Op::STORE_MEMBER_STRING;
}

} // namespace

bytecode::Function emit(const Function &p_function, const bytecode::Module &p_module) {
	bytecode::Function out;
	out.name = p_function.name;
	out.return_type = p_function.return_type;
	out.is_pub = p_function.is_pub;
	out.is_override = p_function.is_override;
	out.line = p_function.line;
	out.frame_size = uint32_t(p_function.regs.size() * 8);
	for (uint32_t i = 0; i < p_function.regs.size(); i++) {
		bytecode::Slot slot = p_function.regs[i];
		slot.offset = offset_of(i);
		out.slots.push_back(slot);
		if (i < p_function.param_count) {
			out.params.push_back(slot);
		}
	}

	std::vector<uint32_t> &code = out.code;
	std::vector<uint32_t> block_start(p_function.blocks.size(), 0);
	// Jump operands to patch once every block's position is known.
	std::vector<std::pair<size_t, uint32_t>> fixups;

	auto jump = [&](Op p_op, uint32_t p_block) {
		code.push_back(uint32_t(p_op));
		fixups.push_back({ code.size(), p_block });
		code.push_back(0);
	};

	for (uint32_t b = 0; b < p_function.blocks.size(); b++) {
		const Block &block = p_function.blocks[b];
		block_start[b] = uint32_t(code.size());
		for (const Instr &instr : block.instrs) {
			code.push_back(uint32_t(instr.op));
			const int count = bytecode::operand_count(instr.op);
			if (count < 0) {
				code.push_back(instr.imm);
				code.push_back(offset_of(instr.dst));
				code.push_back(uint32_t(instr.args.size()));
				for (uint32_t arg : instr.args) {
					code.push_back(uint32_t(p_function.regs[arg].type));
					code.push_back(offset_of(arg));
				}
				continue;
			}
			switch (instr.op) {
				case Op::LINE:
					code.push_back(instr.imm);
					break;
				case Op::CONST_INT:
				case Op::CONST_FLOAT:
				case Op::CONST_BOOL:
				case Op::CONST_STRING:
					code.push_back(offset_of(instr.dst));
					code.push_back(instr.imm);
					break;
				default:
					if (is_member_op(instr.op)) {
						const uint32_t member = p_module.members[instr.imm].offset;
						if (instr.op == Op::LOAD_MEMBER_8 || instr.op == Op::LOAD_MEMBER_STRING) {
							code.push_back(offset_of(instr.dst));
							code.push_back(member);
						} else {
							code.push_back(member);
							code.push_back(offset_of(instr.a));
						}
					} else {
						code.push_back(offset_of(instr.dst));
						code.push_back(offset_of(instr.a));
						if (count == 3) {
							code.push_back(offset_of(instr.b));
						}
					}
					break;
			}
		}

		const uint32_t next = b + 1;
		switch (block.terminator) {
			case Terminator::None:
				if (next == p_function.blocks.size()) {
					code.push_back(uint32_t(Op::RETURN_VOID));
				}
				break;
			case Terminator::Jump:
				if (block.target != next) {
					jump(Op::JUMP, block.target);
				}
				break;
			case Terminator::Branch:
				if (block.target == next) {
					code.push_back(uint32_t(Op::JUMP_IF_FALSE));
					code.push_back(offset_of(block.value));
					fixups.push_back({ code.size(), block.else_target });
					code.push_back(0);
				} else {
					code.push_back(uint32_t(Op::JUMP_IF_TRUE));
					code.push_back(offset_of(block.value));
					fixups.push_back({ code.size(), block.target });
					code.push_back(0);
					if (block.else_target != next) {
						jump(Op::JUMP, block.else_target);
					}
				}
				break;
			case Terminator::Return:
				code.push_back(uint32_t(Op::RETURN));
				code.push_back(offset_of(block.value));
				break;
			case Terminator::ReturnVoid:
				code.push_back(uint32_t(Op::RETURN_VOID));
				break;
		}
	}

	for (const auto &fixup : fixups) {
		code[fixup.first] = block_start[fixup.second];
	}
	return out;
}

} // namespace petrichor::compiler::ir
