#include "compiler/checker.h"

#include <unordered_map>
#include <unordered_set>

namespace petrichor::compiler {

std::string Type::to_string() const {
	switch (kind) {
		case TypeKind::Error:
			return "<error>";
		case TypeKind::Void:
			return "void";
		case TypeKind::Bool:
			return "bool";
		case TypeKind::Int:
			return "int";
		case TypeKind::Float:
			return "float";
		case TypeKind::String:
			return "String";
		case TypeKind::Variant:
			return "Variant";
		case TypeKind::Object:
		case TypeKind::Unsupported:
			return name;
	}
	return "?";
}

bool is_assignable(const Type &p_from, const Type &p_to) {
	if (p_from.is_error() || p_to.is_error()) {
		return true;
	}
	if (p_from == p_to) {
		return true;
	}
	if (p_from.kind == TypeKind::Int && p_to.kind == TypeKind::Float) {
		return true;
	}
	return p_to.kind == TypeKind::Variant && p_from.is_storable();
}

namespace {

class Checker {
	ClassDecl &cls;
	const EngineApi &engine;
	std::vector<Diagnostic> &errors;

	FuncDecl *function = nullptr;
	std::vector<std::unordered_map<std::string, int>> scopes;
	int loop_depth = 0;

	std::unordered_map<std::string, int> member_index;
	std::unordered_map<std::string, int> function_index;

	void error(int p_line, int p_column, const std::string &p_message) {
		errors.push_back({ p_line, p_column, p_message });
	}

	Type resolve(const TypeRef &p_ref, bool p_allow_void) {
		if (p_ref.name == "int") {
			return Type::of(TypeKind::Int);
		}
		if (p_ref.name == "float") {
			return Type::of(TypeKind::Float);
		}
		if (p_ref.name == "bool") {
			return Type::of(TypeKind::Bool);
		}
		if (p_ref.name == "String") {
			return Type::of(TypeKind::String);
		}
		if (p_ref.name == "void") {
			if (p_allow_void) {
				return Type::of(TypeKind::Void);
			}
			error(p_ref.line, p_ref.column, "void is only valid as a return type.");
			return Type::of(TypeKind::Error);
		}
		if (engine.class_exists(p_ref.name)) {
			error(p_ref.line, p_ref.column, "Object types such as " + p_ref.name + " are not supported yet.");
			return Type::of(TypeKind::Error);
		}
		error(p_ref.line, p_ref.column, "Unknown type \"" + p_ref.name + "\".");
		return Type::of(TypeKind::Error);
	}

	// The type a value of an engine signature can have in this compiler.
	bool engine_type_usable(const Type &p_type) const {
		return p_type.is_storable() || p_type.kind == TypeKind::Void || p_type.kind == TypeKind::Variant;
	}

	/* Expressions */

	Type set(Expr &r_expr, const Type &p_type) {
		r_expr.type = p_type;
		return p_type;
	}

	int find_local(const std::string &p_name) const {
		for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
			const auto found = scope->find(p_name);
			if (found != scope->end()) {
				return found->second;
			}
		}
		return -1;
	}

	Type check_name(Expr &r_expr) {
		const int local = find_local(r_expr.text);
		if (local >= 0) {
			r_expr.target = Target::Local;
			r_expr.index = local;
			return set(r_expr, function->locals[local].type);
		}
		const auto member = member_index.find(r_expr.text);
		if (member != member_index.end()) {
			r_expr.target = Target::Member;
			r_expr.index = member->second;
			return set(r_expr, cls.members[member->second].type);
		}
		if (function_index.count(r_expr.text)) {
			error(r_expr.line, r_expr.column, "Functions cannot be used as values yet; call " + r_expr.text + "().");
		} else {
			error(r_expr.line, r_expr.column, "Identifier \"" + r_expr.text + "\" is not declared in the current scope.");
		}
		return set(r_expr, Type::of(TypeKind::Error));
	}

	Type check_unary(Expr &r_expr) {
		const Type operand = check_expr(*r_expr.left);
		if (operand.is_error()) {
			return set(r_expr, operand);
		}
		if (r_expr.op == TokenKind::Minus) {
			if (operand.is_numeric()) {
				return set(r_expr, operand);
			}
			error(r_expr.line, r_expr.column, "Cannot negate a value of type " + operand.to_string() + ".");
		} else {
			if (operand.kind == TypeKind::Bool) {
				return set(r_expr, operand);
			}
			error(r_expr.line, r_expr.column, "\"not\" requires bool, found " + operand.to_string() + ". Compare explicitly, for example x == 0.");
		}
		return set(r_expr, Type::of(TypeKind::Error));
	}

	// The result of `p_left op p_right` (spec §5.1), or Error after reporting.
	Type binary_type(TokenKind p_op, const Type &p_left, const Type &p_right, int p_line, int p_column) {
		if (p_left.is_error() || p_right.is_error()) {
			return Type::of(TypeKind::Error);
		}
		const bool numeric = p_left.is_numeric() && p_right.is_numeric();
		const bool both_int = p_left.kind == TypeKind::Int && p_right.kind == TypeKind::Int;
		const Type number = Type::of(both_int ? TypeKind::Int : TypeKind::Float);
		switch (p_op) {
			case TokenKind::Plus:
				if (p_left.kind == TypeKind::String && p_right.kind == TypeKind::String) {
					return p_left;
				}
				[[fallthrough]];
			case TokenKind::Minus:
			case TokenKind::Star:
			case TokenKind::Slash:
				if (numeric) {
					return number;
				}
				break;
			case TokenKind::Percent:
				if (both_int) {
					return number;
				}
				if (numeric) {
					error(p_line, p_column, "\"%\" requires int operands; use fmod() for float.");
					return Type::of(TypeKind::Error);
				}
				break;
			case TokenKind::Less:
			case TokenKind::LessEqual:
			case TokenKind::Greater:
			case TokenKind::GreaterEqual:
				if (numeric) {
					return Type::of(TypeKind::Bool);
				}
				break;
			case TokenKind::EqualEqual:
			case TokenKind::BangEqual:
				if (numeric || (p_left == p_right && (p_left.kind == TypeKind::Bool || p_left.kind == TypeKind::String))) {
					return Type::of(TypeKind::Bool);
				}
				error(p_line, p_column, "Cannot compare " + p_left.to_string() + " with " + p_right.to_string() + ".");
				return Type::of(TypeKind::Error);
			case TokenKind::And:
			case TokenKind::Or:
				if (p_left.kind == TypeKind::Bool && p_right.kind == TypeKind::Bool) {
					return p_left;
				}
				error(p_line, p_column, std::string(token_name(p_op)) + " requires bool operands, found " + p_left.to_string() + " and " + p_right.to_string() + ".");
				return Type::of(TypeKind::Error);
			default:
				break;
		}
		error(p_line, p_column, std::string("Invalid operands ") + p_left.to_string() + " and " + p_right.to_string() + " for " + token_name(p_op) + ".");
		return Type::of(TypeKind::Error);
	}

	Type check_binary(Expr &r_expr) {
		const Type left = check_expr(*r_expr.left);
		const Type right = check_expr(*r_expr.right);
		return set(r_expr, binary_type(r_expr.op, left, right, r_expr.line, r_expr.column));
	}

	// Checks the arguments of a call against parameter types.
	void check_arguments(Expr &r_call, const std::vector<Type> &p_params, int p_required, bool p_vararg) {
		const int count = int(r_call.args.size());
		const int max = int(p_params.size());
		if (count < p_required || (!p_vararg && count > max)) {
			std::string expected = std::to_string(p_required);
			if (p_vararg) {
				expected = "at least " + expected;
			} else if (p_required != max) {
				expected += " to " + std::to_string(max);
			}
			error(r_call.line, r_call.column, "Function \"" + r_call.text + "()\" takes " + expected + " argument(s), " + std::to_string(count) + " given.");
		}
		for (int i = 0; i < count; i++) {
			Expr &arg = *r_call.args[i];
			const Type arg_type = check_expr(arg);
			const Type param = i < max ? p_params[i] : Type::of(TypeKind::Variant);
			r_call.param_types.push_back(param);
			if (arg_type.kind == TypeKind::Void) {
				error(arg.line, arg.column, "A call that returns void cannot be an argument.");
			} else if (!is_assignable(arg_type, param)) {
				std::string message = "Cannot pass a value of type " + arg_type.to_string() + " as " + param.to_string() + ".";
				if (arg_type.kind == TypeKind::Float && param.kind == TypeKind::Int) {
					message += " Convert explicitly with int(x).";
				}
				error(arg.line, arg.column, message);
			}
		}
	}

	Type check_call(Expr &r_expr) {
		const std::string &name = r_expr.text;

		const auto script = function_index.find(name);
		if (script != function_index.end()) {
			const FuncDecl &callee = cls.functions[script->second];
			std::vector<Type> params;
			for (const ParamDecl &param : callee.params) {
				params.push_back(param.type);
			}
			check_arguments(r_expr, params, int(params.size()), false);
			r_expr.target = Target::ScriptFunction;
			r_expr.index = script->second;
			return set(r_expr, callee.return_type);
		}

		if (const auto method = engine.find_method(cls.extends, name)) {
			if (method->is_static) {
				error(r_expr.line, r_expr.column, "Static engine methods are not supported yet.");
				return set(r_expr, Type::of(TypeKind::Error));
			}
			if (method->is_virtual) {
				error(r_expr.line, r_expr.column, "\"" + name + "()\" is a virtual engine method that this class does not define.");
				return set(r_expr, Type::of(TypeKind::Error));
			}
			std::vector<Type> params;
			for (const EngineParam &param : method->params) {
				if (!engine_type_usable(param.type)) {
					error(r_expr.line, r_expr.column, "\"" + name + "()\" takes a parameter of type " + param.type.to_string() + ", which is not supported yet.");
					return set(r_expr, Type::of(TypeKind::Error));
				}
				params.push_back(param.type);
			}
			check_arguments(r_expr, params, int(params.size()) - method->default_count, method->is_vararg);
			r_expr.target = Target::EngineMethod;
			return set(r_expr, method->return_type);
		}

		if (!r_expr.base) {
			if (const auto utility = engine.find_utility(name)) {
				std::vector<Type> params;
				for (const EngineParam &param : utility->params) {
					params.push_back(param.type);
				}
				check_arguments(r_expr, params, int(params.size()) - utility->default_count, utility->is_vararg);
				r_expr.target = Target::Utility;
				return set(r_expr, utility->return_type);
			}
			if (name == "int" || name == "float") {
				const TypeKind result = name == "int" ? TypeKind::Int : TypeKind::Float;
				r_expr.target = name == "int" ? Target::ConvertToInt : Target::ConvertToFloat;
				if (r_expr.args.size() != 1) {
					error(r_expr.line, r_expr.column, name + "() takes one argument.");
					return set(r_expr, Type::of(TypeKind::Error));
				}
				const Type arg = check_expr(*r_expr.args[0]);
				if (!arg.is_error() && !arg.is_numeric()) {
					error(r_expr.args[0]->line, r_expr.args[0]->column, name + "() of " + arg.to_string() + " is not supported yet.");
					return set(r_expr, Type::of(TypeKind::Error));
				}
				r_expr.param_types.push_back(arg);
				return set(r_expr, Type::of(result));
			}
		}

		error(r_expr.line, r_expr.column, "Function \"" + name + "()\" not found in the class or its base " + cls.extends + ".");
		for (auto &arg : r_expr.args) {
			check_expr(*arg);
		}
		return set(r_expr, Type::of(TypeKind::Error));
	}

	Type check_expr(Expr &r_expr) {
		switch (r_expr.kind) {
			case ExprKind::IntLiteral:
				return set(r_expr, Type::of(TypeKind::Int));
			case ExprKind::FloatLiteral:
				return set(r_expr, Type::of(TypeKind::Float));
			case ExprKind::StringLiteral:
				return set(r_expr, Type::of(TypeKind::String));
			case ExprKind::BoolLiteral:
				return set(r_expr, Type::of(TypeKind::Bool));
			case ExprKind::Name:
				return check_name(r_expr);
			case ExprKind::Self:
				error(r_expr.line, r_expr.column, "self as a value is not supported yet.");
				return set(r_expr, Type::of(TypeKind::Error));
			case ExprKind::Unary:
				return check_unary(r_expr);
			case ExprKind::Binary:
				return check_binary(r_expr);
			case ExprKind::Call:
				return check_call(r_expr);
		}
		return set(r_expr, Type::of(TypeKind::Error));
	}

	// Checks an expression whose value is stored, passed or returned.
	Type check_value(Expr &r_expr) {
		const Type type = check_expr(r_expr);
		if (type.kind == TypeKind::Void) {
			error(r_expr.line, r_expr.column, "This call returns void and has no value.");
			return Type::of(TypeKind::Error);
		}
		if (!type.is_error() && !type.is_storable()) {
			error(r_expr.line, r_expr.column, "Values of type " + type.to_string() + " are not supported yet.");
			return Type::of(TypeKind::Error);
		}
		return type;
	}

	void expect_assignable(const Expr &p_value, const Type &p_from, const Type &p_to, const char *p_action) {
		if (is_assignable(p_from, p_to)) {
			return;
		}
		std::string message = std::string("Cannot ") + p_action + " a value of type " + p_from.to_string() + " as " + p_to.to_string() + ".";
		if (p_from.kind == TypeKind::Float && p_to.kind == TypeKind::Int) {
			message += " Convert explicitly with int(x).";
		}
		error(p_value.line, p_value.column, message);
	}

	void expect_condition(Expr &r_condition) {
		const Type type = check_value(r_condition);
		if (type.is_error() || type.kind == TypeKind::Bool) {
			return;
		}
		std::string hint;
		if (type.is_numeric()) {
			hint = " Compare explicitly, for example x != 0.";
		} else if (type.kind == TypeKind::String) {
			hint = " Compare explicitly, for example x != \"\".";
		}
		error(r_condition.line, r_condition.column, "A condition must be bool, found " + type.to_string() + "." + hint);
	}

	/* Statements */

	void check_block(Block &r_block) {
		scopes.emplace_back();
		for (auto &stmt : r_block) {
			check_stmt(*stmt);
		}
		scopes.pop_back();
	}

	void declare_local(Stmt &r_stmt, const Type &p_type) {
		if (scopes.back().count(r_stmt.name)) {
			error(r_stmt.line, r_stmt.column, "\"" + r_stmt.name + "\" is already declared in this block.");
		}
		r_stmt.local = int(function->locals.size());
		function->locals.push_back({ r_stmt.name, p_type });
		scopes.back()[r_stmt.name] = r_stmt.local;
	}

	void check_stmt(Stmt &r_stmt) {
		switch (r_stmt.kind) {
			case StmtKind::Var: {
				Type type = Type::of(TypeKind::Error);
				if (r_stmt.declared_type) {
					type = resolve(*r_stmt.declared_type, false);
				}
				if (r_stmt.value) {
					const Type value = check_value(*r_stmt.value);
					if (r_stmt.declared_type) {
						expect_assignable(*r_stmt.value, value, type, "assign");
					} else {
						type = value;
					}
				} else if (!r_stmt.declared_type) {
					error(r_stmt.line, r_stmt.column, "\"" + r_stmt.name + "\" needs a type or an initial value.");
				}
				declare_local(r_stmt, type);
				return;
			}
			case StmtKind::Assign: {
				Expr &target = *r_stmt.target;
				if (target.kind != ExprKind::Name) {
					check_expr(target);
					error(target.line, target.column, "Cannot assign to this expression.");
					check_value(*r_stmt.value);
					return;
				}
				const Type target_type = check_name(target);
				Type value = check_value(*r_stmt.value);
				if (r_stmt.assign_op != TokenKind::Equal) {
					TokenKind op = TokenKind::Plus;
					switch (r_stmt.assign_op) {
						case TokenKind::MinusEqual:
							op = TokenKind::Minus;
							break;
						case TokenKind::StarEqual:
							op = TokenKind::Star;
							break;
						case TokenKind::SlashEqual:
							op = TokenKind::Slash;
							break;
						case TokenKind::PercentEqual:
							op = TokenKind::Percent;
							break;
						default:
							break;
					}
					value = binary_type(op, target_type, value, r_stmt.line, r_stmt.column);
				}
				expect_assignable(*r_stmt.value, value, target_type, "assign");
				return;
			}
			case StmtKind::If:
				expect_condition(*r_stmt.condition);
				check_block(r_stmt.body);
				check_block(r_stmt.else_body);
				return;
			case StmtKind::While:
				expect_condition(*r_stmt.condition);
				loop_depth++;
				check_block(r_stmt.body);
				loop_depth--;
				return;
			case StmtKind::Return: {
				const Type expected = function->return_type;
				if (!r_stmt.value) {
					if (expected.kind != TypeKind::Void && !expected.is_error()) {
						error(r_stmt.line, r_stmt.column, "\"" + function->name + "()\" must return a value of type " + expected.to_string() + ".");
					}
					return;
				}
				const Type value = check_value(*r_stmt.value);
				if (expected.kind == TypeKind::Void) {
					error(r_stmt.value->line, r_stmt.value->column, "\"" + function->name + "()\" returns void. Declare its return type, for example -> " + (value.is_error() ? std::string("int") : value.to_string()) + ".");
					return;
				}
				expect_assignable(*r_stmt.value, value, expected, "return");
				return;
			}
			case StmtKind::Pass:
				return;
			case StmtKind::Break:
			case StmtKind::Continue:
				if (loop_depth == 0) {
					error(r_stmt.line, r_stmt.column, std::string(r_stmt.kind == StmtKind::Break ? "break" : "continue") + " is only valid in a loop.");
				}
				return;
			case StmtKind::Expression:
				if (r_stmt.value->kind != ExprKind::Call) {
					check_expr(*r_stmt.value);
					error(r_stmt.value->line, r_stmt.value->column, "This expression has no effect.");
					return;
				}
				check_expr(*r_stmt.value);
				return;
		}
	}

	static bool always_returns(const Block &p_block) {
		if (p_block.empty()) {
			return false;
		}
		const Stmt &last = *p_block.back();
		if (last.kind == StmtKind::Return) {
			return true;
		}
		if (last.kind == StmtKind::If) {
			return always_returns(last.body) && always_returns(last.else_body);
		}
		return false;
	}

	/* Declarations */

	void check_members() {
		for (size_t i = 0; i < cls.members.size(); i++) {
			VarDecl &member = cls.members[i];
			if (member_index.count(member.name)) {
				error(member.line, member.column, "The member \"" + member.name + "\" is already declared.");
				continue;
			}
			member_index[member.name] = int(i);

			Type type = Type::of(TypeKind::Error);
			if (member.declared_type) {
				type = resolve(*member.declared_type, false);
			}
			if (member.initializer) {
				const Expr &init = *member.initializer;
				const bool literal = init.kind == ExprKind::IntLiteral || init.kind == ExprKind::FloatLiteral ||
						init.kind == ExprKind::StringLiteral || init.kind == ExprKind::BoolLiteral ||
						(init.kind == ExprKind::Unary && init.op == TokenKind::Minus &&
								(init.left->kind == ExprKind::IntLiteral || init.left->kind == ExprKind::FloatLiteral));
				if (!literal) {
					error(init.line, init.column, "Member initial values must be literals so far.");
				}
				// No scope: names in the initializer are reported as undeclared.
				const Type value = check_expr(*member.initializer);
				if (member.declared_type) {
					expect_assignable(init, value, type, "assign");
				} else {
					type = value;
				}
			} else if (!member.declared_type) {
				error(member.line, member.column, "\"" + member.name + "\" needs a type or an initial value.");
			}
			member.type = type;
		}
	}

	void check_signature(FuncDecl &r_function) {
		const auto engine_method = engine.find_method(cls.extends, r_function.name);
		if (engine_method && !engine_method->is_virtual) {
			error(r_function.line, r_function.column, "\"" + r_function.name + "()\" is a method of " + cls.extends + " that cannot be overridden.");
		}
		r_function.is_override = engine_method && engine_method->is_virtual;

		if (r_function.is_override) {
			if (r_function.params.size() != engine_method->params.size()) {
				error(r_function.line, r_function.column, "\"" + r_function.name + "()\" overrides an engine method with " + std::to_string(engine_method->params.size()) + " parameter(s).");
			}
		}
		for (size_t i = 0; i < r_function.params.size(); i++) {
			ParamDecl &param = r_function.params[i];
			Type engine_type = Type::of(TypeKind::Error);
			if (r_function.is_override && i < engine_method->params.size()) {
				engine_type = engine_method->params[i].type;
				if (!engine_type.is_storable()) {
					error(param.line, param.column, "Parameters of type " + engine_type.to_string() + " are not supported yet.");
					engine_type = Type::of(TypeKind::Error);
				}
			}
			if (param.declared_type) {
				param.type = resolve(*param.declared_type, false);
				if (r_function.is_override && !engine_type.is_error() && param.type != engine_type) {
					error(param.declared_type->line, param.declared_type->column, "The overridden method declares this parameter as " + engine_type.to_string() + ".");
				}
			} else if (r_function.is_override) {
				// The type comes from the overridden declaration (spec §4.6).
				param.type = engine_type;
			} else {
				error(param.line, param.column, "Parameter \"" + param.name + "\" needs a type.");
				param.type = Type::of(TypeKind::Error);
			}
		}

		if (r_function.return_type_ref) {
			r_function.return_type = resolve(*r_function.return_type_ref, true);
			if (r_function.is_override && !engine_method->return_type.is_error() && r_function.return_type != engine_method->return_type) {
				error(r_function.return_type_ref->line, r_function.return_type_ref->column, "The overridden method returns " + engine_method->return_type.to_string() + ".");
			}
		} else if (r_function.is_override) {
			r_function.return_type = engine_method->return_type;
			if (!engine_type_usable(r_function.return_type) || r_function.return_type.kind == TypeKind::Variant) {
				error(r_function.line, r_function.column, "Return values of type " + r_function.return_type.to_string() + " are not supported yet.");
				r_function.return_type = Type::of(TypeKind::Error);
			}
		} else {
			r_function.return_type = Type::of(TypeKind::Void);
		}
	}

	void check_body(FuncDecl &r_function) {
		function = &r_function;
		scopes.clear();
		scopes.emplace_back();
		for (const ParamDecl &param : r_function.params) {
			if (scopes.back().count(param.name)) {
				error(param.line, param.column, "Parameter \"" + param.name + "\" is declared twice.");
			}
			scopes.back()[param.name] = int(r_function.locals.size());
			r_function.locals.push_back({ param.name, param.type });
		}
		check_block(r_function.body);
		if (r_function.return_type.kind != TypeKind::Void && !r_function.return_type.is_error() && !always_returns(r_function.body)) {
			error(r_function.line, r_function.column, "Not all paths of \"" + r_function.name + "()\" return a value.");
		}
		function = nullptr;
	}

public:
	Checker(ClassDecl &r_class, const EngineApi &p_engine, std::vector<Diagnostic> &r_errors) :
			cls(r_class), engine(p_engine), errors(r_errors) {}

	void run() {
		if (!engine.class_exists(cls.extends)) {
			error(cls.extends_line, cls.extends_column, "Unknown base class \"" + cls.extends + "\".");
			return;
		}
		check_members();
		for (size_t i = 0; i < cls.functions.size(); i++) {
			FuncDecl &function_decl = cls.functions[i];
			if (function_index.count(function_decl.name)) {
				error(function_decl.line, function_decl.column, "The function \"" + function_decl.name + "()\" is already declared.");
				continue;
			}
			if (member_index.count(function_decl.name)) {
				error(function_decl.line, function_decl.column, "\"" + function_decl.name + "\" is already declared as a member.");
				continue;
			}
			function_index[function_decl.name] = int(i);
			check_signature(function_decl);
		}
		// In declaration order, so errors come out in a stable order.
		for (size_t i = 0; i < cls.functions.size(); i++) {
			const auto registered = function_index.find(cls.functions[i].name);
			if (registered != function_index.end() && registered->second == int(i)) {
				check_body(cls.functions[i]);
			}
		}
	}
};

} // namespace

void check(ClassDecl &r_class, const EngineApi &p_engine, std::vector<Diagnostic> &r_errors) {
	Checker(r_class, p_engine, r_errors).run();
}

} // namespace petrichor::compiler
