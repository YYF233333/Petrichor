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
	out.is_exposed = p_function.is_exposed;
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
					put(p_function.regs[arg].type);
					put(offset_of(arg));
				}
				continue;
			}
			switch (instr.op) {
				case Op::TASK_CANCEL:
					put(offset_of(instr.a));
					break;
				case Op::AWAIT_TASK:
				case Op::AWAIT_SIGNAL:
					put(offset_of(instr.dst));
					put(offset_of(instr.a));
					if (instr.op == Op::AWAIT_TASK) {
						put(instr.imm);
					}
					break;
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
					put(member_offset(instr));
					break;
				case Op::LOAD_MEMBER_VALUE:
					put(uint32_t(instr.type));
					put(offset_of(instr.dst));
					put(member_offset(instr));
					break;
				case Op::STORE_MEMBER_8:
				case Op::STORE_MEMBER_STRING:
					put(member_offset(instr));
					put(offset_of(instr.a));
					break;
				case Op::STORE_MEMBER_VALUE:
					put(uint32_t(instr.type));
					put(member_offset(instr));
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
				case Op::GET_OWNER:
				case Op::GET_SINGLETON:
				case Op::GET_AUTOLOAD:
				case Op::METHOD_VALUE:
					put(instr.imm);
					put(offset_of(instr.dst));
					break;
				case Op::CHECK_OBJECT:
				case Op::CHECK_ASSIGNED:
				case Op::CHECK_RANGE:
					put(instr.imm);
					put(offset_of(instr.a));
					break;
				case Op::CHECK_LIVE:
					put(offset_of(instr.a));
					break;
				case Op::GET_SELF:
					put(offset_of(instr.dst));
					break;
				case Op::CAST_OBJECT:
				case Op::IS_INSTANCE:
				case Op::GET_SIGNAL:
					put(instr.imm);
					put(offset_of(instr.dst));
					put(offset_of(instr.a));
					break;
				case Op::NEW_CONTAINER:
					put(instr.type);
					put(offset_of(instr.dst));
					break;
				case Op::SET_INDEX:
					put(p_function.regs[instr.dst].type);
					put(offset_of(instr.dst));
					put(offset_of(instr.a));
					put(offset_of(instr.b));
					break;
				case Op::GET_INDEX:
					put(p_function.regs[instr.a].type);
					put(p_function.regs[instr.dst].type);
					put(offset_of(instr.dst));
					put(offset_of(instr.a));
					put(offset_of(instr.b));
					break;
				case Op::TO_VARIANT:
				case Op::ENCODE:
				case Op::IS_NONE:
				case Op::SOME:
					put(instr.type);
					put(offset_of(instr.dst));
					put(offset_of(instr.a));
					break;
				case Op::BOX:
					put(instr.type);
					put(offset_of(instr.dst));
					put(offset_of(instr.a));
					put(instr.imm);
					break;
				case Op::UNWRAP:
				case Op::EQUAL:
				case Op::LESS:
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
