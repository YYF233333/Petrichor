#include "compiler/ir.h"

namespace petrichor::compiler::ir {

using bytecode::Op;

bytecode::Function emit(const Function &p_function, const bytecode::Module &p_module) {
	bytecode::Function out;
	out.name = p_function.name;
	out.result = p_function.result;
	out.is_async = p_function.is_async;
	out.is_pub = p_function.is_pub;
	out.is_override = p_function.is_override;
	out.line = p_function.line;

	// Registers in order, each at an 8-byte aligned offset of its own size,
	// and views inside their roots.
	std::vector<uint32_t> offsets;
	uint32_t frame_size = 0;
	for (uint32_t i = 0; i < p_function.regs.size(); i++) {
		const auto view = p_function.views.find(i);
		if (view != p_function.views.end()) {
			offsets.push_back(offsets[view->second.root] + view->second.offset);
			continue;
		}
		bytecode::Slot slot = p_function.regs[i];
		slot.offset = frame_size;
		offsets.push_back(frame_size);
		frame_size += bytecode::type_size(p_module.types, slot.type);
		out.slots.push_back(slot);
		if (i < p_function.param_count) {
			out.params.push_back(slot);
		}
	}
	out.frame_size = frame_size;
	auto offset_of = [&](uint32_t p_reg) {
		return p_reg == NO_REG ? bytecode::NO_SLOT : offsets[p_reg];
	};
	for (const auto &[reg, bits] : p_function.presets) {
		out.presets.push_back({ offsets[reg], bits });
	}
	auto member_offset = [&](const Instr &p_instr) {
		return p_module.members[p_instr.imm].offset + p_instr.field;
	};

	std::vector<uint32_t> &code = out.code;
	std::vector<uint32_t> block_start(p_function.blocks.size(), 0);
	// Jump operands to patch once every block's position is known.
	std::vector<std::pair<size_t, uint32_t>> fixups;

	auto put = [&](uint32_t p_word) { code.push_back(p_word); };
	auto jump = [&](Op p_op, uint32_t p_block) {
		put(uint32_t(p_op));
		fixups.push_back({ code.size(), p_block });
		put(0);
	};

	// `p_jump`: the block that a J operand jumps to.
	auto put_instr = [&](const Instr &p_instr, uint32_t p_jump) {
		put(uint32_t(p_instr.op));
		if (bytecode::operand_count(p_instr.op) < 0) {
			put(p_instr.imm);
			put(offset_of(p_instr.dst));
			put(offset_of(p_instr.base));
			put(uint32_t(p_instr.args.size()));
			for (uint32_t arg : p_instr.args) {
				put(p_function.regs[arg].type);
				put(offset_of(arg));
			}
			return;
		}
		for (const char *s = bytecode::op_emit_schema(p_instr.op); *s; s++) {
			switch (*s) {
				case 'D':
					put(offset_of(p_instr.dst));
					break;
				case 'A':
					put(offset_of(p_instr.a));
					break;
				case 'B':
					put(offset_of(p_instr.b));
					break;
				case 'I':
					put(p_instr.imm);
					break;
				case 'T':
					put(p_instr.type);
					break;
				case 'F':
					put(p_instr.field);
					break;
				case 'M':
					put(member_offset(p_instr));
					break;
				case 'R':
					put(p_function.regs[p_instr.dst].type);
					break;
				case 'S':
					put(p_function.regs[p_instr.a].type);
					break;
				case 'J':
					fixups.push_back({ code.size(), p_jump });
					put(0);
					break;
			}
		}
	};

	for (uint32_t b = 0; b < p_function.blocks.size(); b++) {
		const Block &block = p_function.blocks[b];
		block_start[b] = uint32_t(code.size());
		for (const Instr &instr : block.instrs) {
			put_instr(instr, 0);
		}

		const uint32_t next = b + 1;
		switch (block.terminator) {
			case Terminator::None:
				// Never falls through into the next block: blocks are laid out in
				// creation order, which is not the order of control flow.
				put(uint32_t(Op::RETURN_VOID));
				break;
			case Terminator::Jump:
				if (block.target != next) {
					jump(Op::JUMP, block.target);
				}
				break;
			case Terminator::Branch:
				if (block.target == next) {
					put(uint32_t(Op::JUMP_IF_FALSE));
					put(offset_of(block.value));
					fixups.push_back({ code.size(), block.else_target });
					put(0);
				} else {
					put(uint32_t(Op::JUMP_IF_TRUE));
					put(offset_of(block.value));
					fixups.push_back({ code.size(), block.target });
					put(0);
					if (block.else_target != next) {
						jump(Op::JUMP, block.else_target);
					}
				}
				break;
			case Terminator::Iterate:
				put_instr(block.step, block.else_target);
				if (block.target != next) {
					jump(Op::JUMP, block.target);
				}
				break;
			case Terminator::Return:
				put(uint32_t(Op::RETURN));
				put(offset_of(block.value));
				break;
			case Terminator::ReturnVoid:
				put(uint32_t(Op::RETURN_VOID));
				break;
		}
	}

	for (const auto &fixup : fixups) {
		code[fixup.first] = block_start[fixup.second];
	}
	return out;
}

} // namespace petrichor::compiler::ir
