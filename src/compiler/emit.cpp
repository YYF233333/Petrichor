#include "compiler/ir.h"

namespace petrichor::compiler::ir {

using bytecode::Op;

bytecode::Function emit(const Function &p_function, const bytecode::Module &p_module) {
	bytecode::Function out;
	out.name = p_function.name;
	out.result = p_function.result;
	out.is_pub = p_function.is_pub;
	out.is_override = p_function.is_override;
	out.is_exposed = p_function.is_exposed;
	out.line = p_function.line;

	// Registers in order, each at an 8-byte aligned offset of its own size.
	std::vector<uint32_t> offsets;
	uint32_t frame_size = 0;
	for (uint32_t i = 0; i < p_function.regs.size(); i++) {
		bytecode::Slot slot = p_function.regs[i];
		slot.offset = frame_size;
		offsets.push_back(frame_size);
		frame_size += bytecode::slot_size(slot.type);
		out.slots.push_back(slot);
		if (i < p_function.param_count) {
			out.params.push_back(slot);
		}
	}
	out.frame_size = frame_size;
	auto offset_of = [&](uint32_t p_reg) {
		return p_reg == NO_REG ? bytecode::NO_SLOT : offsets[p_reg];
	};
	auto member_offset = [&](uint32_t p_member) {
		return p_module.members[p_member].offset;
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

	for (uint32_t b = 0; b < p_function.blocks.size(); b++) {
		const Block &block = p_function.blocks[b];
		block_start[b] = uint32_t(code.size());
		for (const Instr &instr : block.instrs) {
			put(uint32_t(instr.op));
			if (bytecode::operand_count(instr.op) < 0) {
				put(instr.imm);
				put(offset_of(instr.dst));
				put(offset_of(instr.base));
				put(uint32_t(instr.args.size()));
				for (uint32_t arg : instr.args) {
					put(uint32_t(p_function.regs[arg].type));
					put(offset_of(arg));
				}
				continue;
			}
			switch (instr.op) {
				case Op::LINE:
					put(instr.imm);
					break;
				case Op::CONST_INT:
				case Op::CONST_FLOAT:
				case Op::CONST_BOOL:
				case Op::CONST_STRING:
					put(offset_of(instr.dst));
					put(instr.imm);
					break;
				case Op::CONST_VALUE:
					put(uint32_t(instr.type));
					put(offset_of(instr.dst));
					put(instr.imm);
					break;
				case Op::MOVE_VALUE:
					put(uint32_t(instr.type));
					put(offset_of(instr.dst));
					put(offset_of(instr.a));
					break;
				case Op::LOAD_MEMBER_8:
				case Op::LOAD_MEMBER_STRING:
					put(offset_of(instr.dst));
					put(member_offset(instr.imm));
					break;
				case Op::LOAD_MEMBER_VALUE:
					put(uint32_t(instr.type));
					put(offset_of(instr.dst));
					put(member_offset(instr.imm));
					break;
				case Op::STORE_MEMBER_8:
				case Op::STORE_MEMBER_STRING:
					put(member_offset(instr.imm));
					put(offset_of(instr.a));
					break;
				case Op::STORE_MEMBER_VALUE:
					put(uint32_t(instr.type));
					put(member_offset(instr.imm));
					put(offset_of(instr.a));
					break;
				case Op::EVALUATE:
					put(instr.imm);
					put(offset_of(instr.dst));
					put(offset_of(instr.a));
					put(offset_of(instr.b));
					break;
				case Op::GET_FIELD:
				case Op::GET_PROPERTY:
					put(instr.imm);
					put(offset_of(instr.dst));
					put(offset_of(instr.a));
					break;
				case Op::SET_FIELD:
				case Op::SET_PROPERTY:
					put(instr.imm);
					put(offset_of(instr.a));
					put(offset_of(instr.b));
					break;
				case Op::NEW_OBJECT:
				case Op::GET_SINGLETON:
				case Op::GET_AUTOLOAD:
				case Op::METHOD_VALUE:
					put(instr.imm);
					put(offset_of(instr.dst));
					break;
				case Op::CHECK_OBJECT:
				case Op::CHECK_ASSIGNED:
					put(instr.imm);
					put(offset_of(instr.a));
					break;
				case Op::CAST_OBJECT:
				case Op::IS_INSTANCE:
				case Op::GET_SIGNAL:
					put(instr.imm);
					put(offset_of(instr.dst));
					put(offset_of(instr.a));
					break;
				case Op::NEW_ARRAY:
					put(uint32_t(instr.type));
					put(instr.imm);
					put(offset_of(instr.dst));
					break;
				case Op::GET_INDEX:
					put(uint32_t(p_function.regs[instr.a].type));
					put(uint32_t(p_function.regs[instr.dst].type));
					put(offset_of(instr.dst));
					put(offset_of(instr.a));
					put(offset_of(instr.b));
					break;
				case Op::TO_VARIANT:
					put(uint32_t(instr.type));
					put(offset_of(instr.dst));
					put(offset_of(instr.a));
					break;
				case Op::UNWRAP:
					put(uint32_t(instr.type));
					put(offset_of(instr.dst));
					put(offset_of(instr.a));
					put(offset_of(instr.b));
					break;
				default:
					// dst, a and, for binary instructions, b.
					put(offset_of(instr.dst));
					put(offset_of(instr.a));
					if (bytecode::operand_count(instr.op) == 3) {
						put(offset_of(instr.b));
					}
					break;
			}
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
