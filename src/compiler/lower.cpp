#include "compiler/lower.h"

#include "compiler/ir.h"

namespace petrichor::compiler {

using bytecode::Op;
using bytecode::ValueType;
using ir::NO_REG;

namespace {

ValueType value_type(const Type &p_type) {
	switch (p_type.kind) {
		case TypeKind::Bool:
			return ValueType::Bool;
		case TypeKind::Int:
			return ValueType::Int;
		case TypeKind::Float:
			return ValueType::Float;
		case TypeKind::String:
			return ValueType::String;
		default:
			return ValueType::Void;
	}
}

bytecode::Constant constant_of(const Expr &p_literal, const Type &p_target) {
	bytecode::Constant constant;
	constant.type = value_type(p_target);
	const Expr *literal = &p_literal;
	bool negate = false;
	if (literal->kind == ExprKind::Unary) {
		negate = true;
		literal = literal->left.get();
	}
	switch (literal->kind) {
		case ExprKind::IntLiteral:
			constant.i = negate ? -literal->int_value : literal->int_value;
			constant.f = double(constant.i);
			break;
		case ExprKind::FloatLiteral:
			constant.f = negate ? -literal->float_value : literal->float_value;
			break;
		case ExprKind::StringLiteral:
			constant.s = literal->text;
			break;
		case ExprKind::BoolLiteral:
			constant.b = literal->bool_value;
			break;
		default:
			break;
	}
	return constant;
}

class Lowerer {
	const ClassDecl &cls;
	bytecode::Module &module;

	ir::Function fn;
	uint32_t current = 0;

	struct Loop {
		uint32_t continue_block;
		uint32_t break_block;
	};
	std::vector<Loop> loops;

	/* Blocks and registers */

	uint32_t new_block() {
		fn.blocks.emplace_back();
		return uint32_t(fn.blocks.size() - 1);
	}

	bool terminated() const { return fn.blocks[current].terminator != ir::Terminator::None; }

	void add(const ir::Instr &p_instr) {
		if (terminated()) {
			current = new_block(); // Unreachable code after a return or jump.
		}
		fn.blocks[current].instrs.push_back(p_instr);
	}

	void jump(uint32_t p_target) {
		if (!terminated()) {
			fn.blocks[current].terminator = ir::Terminator::Jump;
			fn.blocks[current].target = p_target;
		}
	}

	void branch(uint32_t p_condition, uint32_t p_then, uint32_t p_else) {
		if (terminated()) {
			current = new_block();
		}
		ir::Block &block = fn.blocks[current];
		block.terminator = ir::Terminator::Branch;
		block.value = p_condition;
		block.target = p_then;
		block.else_target = p_else;
	}

	uint32_t temp(ValueType p_type) {
		fn.regs.push_back({ std::string(), p_type, 0 });
		return uint32_t(fn.regs.size() - 1);
	}

	ValueType reg_type(uint32_t p_reg) const { return fn.regs[p_reg].type; }

	void instr(Op p_op, uint32_t p_dst, uint32_t p_a = NO_REG, uint32_t p_b = NO_REG, uint32_t p_imm = 0) {
		ir::Instr i;
		i.op = p_op;
		i.dst = p_dst;
		i.a = p_a;
		i.b = p_b;
		i.imm = p_imm;
		add(i);
	}

	void move(uint32_t p_dst, uint32_t p_src) {
		if (p_dst != p_src) {
			instr(reg_type(p_dst) == ValueType::String ? Op::MOVE_STRING : Op::MOVE_8, p_dst, p_src);
		}
	}

	/* Tables */

	uint32_t add_constant(const bytecode::Constant &p_constant) {
		module.constants.push_back(p_constant);
		return uint32_t(module.constants.size() - 1);
	}

	uint32_t engine_call(const std::string &p_method, ValueType p_return) {
		for (uint32_t i = 0; i < module.engine_calls.size(); i++) {
			if (module.engine_calls[i].method == p_method && module.engine_calls[i].return_type == p_return) {
				return i;
			}
		}
		module.engine_calls.push_back({ p_method, p_return });
		return uint32_t(module.engine_calls.size() - 1);
	}

	uint32_t utility_call(const std::string &p_function, ValueType p_return) {
		for (uint32_t i = 0; i < module.utility_calls.size(); i++) {
			if (module.utility_calls[i].function == p_function && module.utility_calls[i].return_type == p_return) {
				return i;
			}
		}
		module.utility_calls.push_back({ p_function, p_return });
		return uint32_t(module.utility_calls.size() - 1);
	}

	/* Expressions */

	// Converts int to float where a float is expected (spec §3.2).
	uint32_t convert(uint32_t p_reg, const Type &p_to) {
		if (p_reg != NO_REG && reg_type(p_reg) == ValueType::Int && p_to.kind == TypeKind::Float) {
			const uint32_t result = temp(ValueType::Float);
			instr(Op::INT_TO_FLOAT, result, p_reg);
			return result;
		}
		return p_reg;
	}

	uint32_t default_value(uint32_t p_dst) {
		switch (reg_type(p_dst)) {
			case ValueType::Int:
				instr(Op::CONST_INT, p_dst, NO_REG, NO_REG, add_constant(bytecode::Constant{ ValueType::Int }));
				break;
			case ValueType::Float:
				instr(Op::CONST_FLOAT, p_dst, NO_REG, NO_REG, add_constant(bytecode::Constant{ ValueType::Float }));
				break;
			case ValueType::Bool:
				instr(Op::CONST_BOOL, p_dst, NO_REG, NO_REG, 0);
				break;
			case ValueType::String:
				instr(Op::CONST_STRING, p_dst, NO_REG, NO_REG, add_constant(bytecode::Constant{ ValueType::String }));
				break;
			case ValueType::Void:
				break;
		}
		return p_dst;
	}

	uint32_t binary(TokenKind p_op, uint32_t p_left, uint32_t p_right, const Type &p_result) {
		ValueType operand = reg_type(p_left);
		if (reg_type(p_left) == ValueType::Float || reg_type(p_right) == ValueType::Float) {
			operand = ValueType::Float;
			p_left = convert(p_left, Type::of(TypeKind::Float));
			p_right = convert(p_right, Type::of(TypeKind::Float));
		}
		const bool f = operand == ValueType::Float;
		Op op = Op::ADD_INT;
		switch (p_op) {
			case TokenKind::Plus:
				op = operand == ValueType::String ? Op::ADD_STRING : (f ? Op::ADD_FLOAT : Op::ADD_INT);
				break;
			case TokenKind::Minus:
				op = f ? Op::SUB_FLOAT : Op::SUB_INT;
				break;
			case TokenKind::Star:
				op = f ? Op::MUL_FLOAT : Op::MUL_INT;
				break;
			case TokenKind::Slash:
				op = f ? Op::DIV_FLOAT : Op::DIV_INT;
				break;
			case TokenKind::Percent:
				op = Op::MOD_INT;
				break;
			case TokenKind::EqualEqual:
				op = operand == ValueType::String ? Op::EQ_STRING : operand == ValueType::Bool ? Op::EQ_BOOL : (f ? Op::EQ_FLOAT : Op::EQ_INT);
				break;
			case TokenKind::BangEqual:
				op = operand == ValueType::String ? Op::NE_STRING : operand == ValueType::Bool ? Op::NE_BOOL : (f ? Op::NE_FLOAT : Op::NE_INT);
				break;
			case TokenKind::Less:
				op = f ? Op::LT_FLOAT : Op::LT_INT;
				break;
			case TokenKind::LessEqual:
				op = f ? Op::LE_FLOAT : Op::LE_INT;
				break;
			case TokenKind::Greater:
				op = f ? Op::GT_FLOAT : Op::GT_INT;
				break;
			case TokenKind::GreaterEqual:
				op = f ? Op::GE_FLOAT : Op::GE_INT;
				break;
			default:
				break;
		}
		const uint32_t result = temp(value_type(p_result));
		instr(op, result, p_left, p_right);
		return result;
	}

	uint32_t short_circuit(const Expr &p_expr) {
		const bool is_and = p_expr.op == TokenKind::And;
		const uint32_t result = temp(ValueType::Bool);
		move(result, lower_expr(*p_expr.left));
		const uint32_t right_block = new_block();
		const uint32_t end_block = new_block();
		if (is_and) {
			branch(result, right_block, end_block);
		} else {
			branch(result, end_block, right_block);
		}
		current = right_block;
		move(result, lower_expr(*p_expr.right));
		jump(end_block);
		current = end_block;
		return result;
	}

	std::vector<uint32_t> arguments(const Expr &p_call) {
		std::vector<uint32_t> args;
		for (size_t i = 0; i < p_call.args.size(); i++) {
			args.push_back(convert(lower_expr(*p_call.args[i]), p_call.param_types[i]));
		}
		return args;
	}

	uint32_t call(const Expr &p_call) {
		const ValueType result_type = value_type(p_call.type);
		switch (p_call.target) {
			case Target::ConvertToInt: {
				const uint32_t arg = lower_expr(*p_call.args[0]);
				if (reg_type(arg) == ValueType::Int) {
					return arg;
				}
				const uint32_t result = temp(ValueType::Int);
				instr(Op::FLOAT_TO_INT, result, arg);
				return result;
			}
			case Target::ConvertToFloat:
				return convert(lower_expr(*p_call.args[0]), Type::of(TypeKind::Float));
			default:
				break;
		}

		ir::Instr i;
		i.args = arguments(p_call);
		i.dst = result_type == ValueType::Void ? NO_REG : temp(result_type);
		switch (p_call.target) {
			case Target::ScriptFunction:
				i.op = Op::CALL;
				i.imm = uint32_t(p_call.index);
				break;
			case Target::EngineMethod:
				i.op = Op::CALL_ENGINE;
				i.imm = engine_call(p_call.text, result_type);
				break;
			case Target::Utility:
				i.op = Op::CALL_UTILITY;
				i.imm = utility_call(p_call.text, result_type);
				break;
			default:
				break;
		}
		add(i);
		return i.dst;
	}

	uint32_t lower_expr(const Expr &p_expr) {
		switch (p_expr.kind) {
			case ExprKind::IntLiteral:
			case ExprKind::FloatLiteral:
			case ExprKind::StringLiteral: {
				const ValueType type = value_type(p_expr.type);
				const uint32_t result = temp(type);
				const Op op = type == ValueType::Int ? Op::CONST_INT : (type == ValueType::Float ? Op::CONST_FLOAT : Op::CONST_STRING);
				instr(op, result, NO_REG, NO_REG, add_constant(constant_of(p_expr, p_expr.type)));
				return result;
			}
			case ExprKind::BoolLiteral: {
				const uint32_t result = temp(ValueType::Bool);
				instr(Op::CONST_BOOL, result, NO_REG, NO_REG, p_expr.bool_value ? 1 : 0);
				return result;
			}
			case ExprKind::Name: {
				if (p_expr.target == Target::Local) {
					return uint32_t(p_expr.index);
				}
				const ValueType type = value_type(p_expr.type);
				const uint32_t result = temp(type);
				instr(type == ValueType::String ? Op::LOAD_MEMBER_STRING : Op::LOAD_MEMBER_8, result, NO_REG, NO_REG, uint32_t(p_expr.index));
				return result;
			}
			case ExprKind::Unary: {
				const uint32_t operand = lower_expr(*p_expr.left);
				const uint32_t result = temp(reg_type(operand));
				if (p_expr.op == TokenKind::Not) {
					instr(Op::NOT, result, operand);
				} else {
					instr(reg_type(operand) == ValueType::Float ? Op::NEG_FLOAT : Op::NEG_INT, result, operand);
				}
				return result;
			}
			case ExprKind::Binary:
				if (p_expr.op == TokenKind::And || p_expr.op == TokenKind::Or) {
					return short_circuit(p_expr);
				} else {
					const uint32_t left = lower_expr(*p_expr.left);
					const uint32_t right = lower_expr(*p_expr.right);
					return binary(p_expr.op, left, right, p_expr.type);
				}
			case ExprKind::Call:
				return call(p_expr);
			case ExprKind::Self:
				break;
		}
		return NO_REG;
	}

	/* Statements */

	void store(const Expr &p_target, uint32_t p_value) {
		if (p_target.target == Target::Local) {
			move(uint32_t(p_target.index), p_value);
		} else {
			instr(reg_type(p_value) == ValueType::String ? Op::STORE_MEMBER_STRING : Op::STORE_MEMBER_8, NO_REG, p_value, NO_REG, uint32_t(p_target.index));
		}
	}

	static TokenKind compound_operator(TokenKind p_assign) {
		switch (p_assign) {
			case TokenKind::MinusEqual:
				return TokenKind::Minus;
			case TokenKind::StarEqual:
				return TokenKind::Star;
			case TokenKind::SlashEqual:
				return TokenKind::Slash;
			case TokenKind::PercentEqual:
				return TokenKind::Percent;
			default:
				return TokenKind::Plus;
		}
	}

	void lower_block(const Block &p_block) {
		for (const auto &stmt : p_block) {
			lower_stmt(*stmt);
		}
	}

	void lower_stmt(const Stmt &p_stmt) {
		instr(Op::LINE, NO_REG, NO_REG, NO_REG, uint32_t(p_stmt.line));
		switch (p_stmt.kind) {
			case StmtKind::Var: {
				const uint32_t local = uint32_t(p_stmt.local);
				if (p_stmt.value) {
					const Type local_type = Type::of(fn.regs[local].type == ValueType::Float ? TypeKind::Float : TypeKind::Error);
					move(local, convert(lower_expr(*p_stmt.value), local_type));
				} else {
					default_value(local);
				}
				return;
			}
			case StmtKind::Assign: {
				const Expr &target = *p_stmt.target;
				uint32_t value = lower_expr(*p_stmt.value);
				if (p_stmt.assign_op != TokenKind::Equal) {
					const uint32_t current_value = lower_expr(target);
					const TokenKind op = compound_operator(p_stmt.assign_op);
					Type result = target.type;
					if (reg_type(current_value) == ValueType::Float || reg_type(value) == ValueType::Float) {
						result = Type::of(TypeKind::Float);
					}
					value = binary(op, current_value, value, result);
				}
				store(target, convert(value, target.type));
				return;
			}
			case StmtKind::If: {
				const uint32_t condition = lower_expr(*p_stmt.condition);
				const uint32_t then_block = new_block();
				const uint32_t end_block = new_block();
				const uint32_t else_block = p_stmt.else_body.empty() ? end_block : new_block();
				branch(condition, then_block, else_block);
				current = then_block;
				lower_block(p_stmt.body);
				jump(end_block);
				if (else_block != end_block) {
					current = else_block;
					lower_block(p_stmt.else_body);
					jump(end_block);
				}
				current = end_block;
				return;
			}
			case StmtKind::While: {
				const uint32_t condition_block = new_block();
				const uint32_t body_block = new_block();
				const uint32_t end_block = new_block();
				jump(condition_block);
				current = condition_block;
				branch(lower_expr(*p_stmt.condition), body_block, end_block);
				current = body_block;
				loops.push_back({ condition_block, end_block });
				lower_block(p_stmt.body);
				loops.pop_back();
				jump(condition_block);
				current = end_block;
				return;
			}
			case StmtKind::Return: {
				if (terminated()) {
					current = new_block();
				}
				if (p_stmt.value) {
					const Type return_type = Type::of(fn.return_type == ValueType::Float ? TypeKind::Float : TypeKind::Error);
					const uint32_t value = convert(lower_expr(*p_stmt.value), return_type);
					if (terminated()) {
						current = new_block();
					}
					fn.blocks[current].terminator = ir::Terminator::Return;
					fn.blocks[current].value = value;
				} else {
					fn.blocks[current].terminator = ir::Terminator::ReturnVoid;
				}
				return;
			}
			case StmtKind::Break:
				jump(loops.back().break_block);
				return;
			case StmtKind::Continue:
				jump(loops.back().continue_block);
				return;
			case StmtKind::Pass:
				return;
			case StmtKind::Expression:
				lower_expr(*p_stmt.value);
				return;
		}
	}

public:
	Lowerer(const ClassDecl &p_class, bytecode::Module &r_module) :
			cls(p_class), module(r_module) {}

	void run() {
		module.global_name = cls.class_name;
		module.base_class = cls.extends;
		for (size_t i = 0; i < cls.members.size(); i++) {
			const VarDecl &decl = cls.members[i];
			bytecode::Member member;
			member.name = decl.name;
			member.type = value_type(decl.type);
			member.offset = uint32_t(i * 8);
			member.is_pub = decl.is_pub;
			member.is_export = decl.is_export;
			member.initial = decl.initializer ? constant_of(*decl.initializer, decl.type) : bytecode::Constant{ member.type };
			module.members.push_back(member);
		}
		module.instance_size = uint32_t(cls.members.size() * 8);

		for (const FuncDecl &decl : cls.functions) {
			fn = ir::Function();
			fn.name = decl.name;
			fn.param_count = uint32_t(decl.params.size());
			fn.return_type = value_type(decl.return_type);
			fn.is_pub = decl.is_pub;
			fn.is_override = decl.is_override;
			fn.line = uint32_t(decl.line);
			for (const LocalInfo &local : decl.locals) {
				fn.regs.push_back({ local.name, value_type(local.type), 0 });
			}
			loops.clear();
			current = new_block();
			lower_block(decl.body);
			module.functions.push_back(ir::emit(fn, module));
		}
	}
};

} // namespace

bytecode::Module lower(const ClassDecl &p_class) {
	bytecode::Module module;
	Lowerer(p_class, module).run();
	return module;
}

} // namespace petrichor::compiler
