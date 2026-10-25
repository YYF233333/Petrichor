#include "compiler/checker.h"

#include "compiler/builtin_api.h"

#include <unordered_map>
#include <unordered_set>

namespace petrichor::compiler {

static bool is_string_like(const Type &p_type) {
	return p_type.kind == TypeKind::String || (p_type.kind == TypeKind::Builtin && (p_type.name == "StringName" || p_type.name == "NodePath"));
}

bool is_assignable(const Type &p_from, const Type &p_to, const EngineApi &p_engine) {
	if (p_from.is_error() || p_to.is_error()) {
		return true;
	}
	if (p_from == p_to) {
		return true;
	}
	if (p_from.kind == TypeKind::Int && p_to.kind == TypeKind::Float) {
		return true;
	}
	// Converted where needed, as in GDScript (spec §10.7).
	if (is_string_like(p_from) && is_string_like(p_to) && (p_from.kind == TypeKind::String || p_to.kind == TypeKind::String)) {
		return true;
	}
	if (p_from.kind == TypeKind::Object && p_to.kind == TypeKind::Object) {
		return (p_to.nullable || !p_from.nullable) && p_engine.inherits(p_from.name, p_to.name);
	}
	return p_to.kind == TypeKind::Variant && p_from.is_storable();
}

namespace {

const char *operator_name(TokenKind p_op) {
	switch (p_op) {
		case TokenKind::Plus:
			return "+";
		case TokenKind::Minus:
			return "-";
		case TokenKind::Star:
			return "*";
		case TokenKind::Slash:
			return "/";
		case TokenKind::Percent:
			return "%";
		case TokenKind::EqualEqual:
			return "==";
		case TokenKind::BangEqual:
			return "!=";
		case TokenKind::Less:
			return "<";
		case TokenKind::LessEqual:
			return "<=";
		case TokenKind::Greater:
			return ">";
		case TokenKind::GreaterEqual:
			return ">=";
		default:
			return "";
	}
}

// Utility functions that the engine binds with Variant and that are typed by
// their arguments instead (spec §10.3).
enum class ArgumentTyped {
	None,
	Same, // Every argument has the result's type, such as clamp and min.
	Lerp, // from and to have the result's type; the weight is a float.
};

ArgumentTyped argument_typed(const std::string &p_name) {
	static const std::unordered_set<std::string> same = { "floor", "ceil", "round", "abs", "sign", "snapped", "wrap", "max", "min", "clamp" };
	if (same.count(p_name)) {
		return ArgumentTyped::Same;
	}
	return p_name == "lerp" ? ArgumentTyped::Lerp : ArgumentTyped::None;
}

// The types these utilities accept in the engine (core/variant/variant_utility.cpp).
bool is_numeric_or_vector(const Type &p_type) {
	if (p_type.is_numeric()) {
		return true;
	}
	static const std::unordered_set<std::string> vectors = { "Vector2", "Vector2i", "Vector3", "Vector3i", "Vector4", "Vector4i" };
	return p_type.kind == TypeKind::Builtin && vectors.count(p_type.name);
}

bool is_interpolable(const Type &p_type) {
	static const std::unordered_set<std::string> types = { "Vector2", "Vector3", "Vector4", "Color", "Quaternion", "Basis" };
	return p_type.kind == TypeKind::Float || (p_type.kind == TypeKind::Builtin && types.count(p_type.name));
}

// Values that refer to shared engine storage: changing one through a method
// changes it for everyone who holds it, so it is not written back.
bool is_shared(const Type &p_type) {
	return p_type.kind == TypeKind::Builtin && (p_type.name == "Array" || p_type.name == "Dictionary");
}

bool is_literal(const Expr &p_expr) {
	switch (p_expr.kind) {
		case ExprKind::IntLiteral:
		case ExprKind::FloatLiteral:
		case ExprKind::StringLiteral:
		case ExprKind::BoolLiteral:
			return true;
		case ExprKind::Unary:
			return p_expr.op == TokenKind::Minus && (p_expr.left->kind == ExprKind::IntLiteral || p_expr.left->kind == ExprKind::FloatLiteral);
		default:
			return false;
	}
}

class Checker {
	ClassDecl &cls;
	const EngineApi &engine;
	const CompileOptions &options;
	std::vector<Diagnostic> &errors;
	std::vector<Scene> scenes; // The scenes that use this script.

	FuncDecl *function = nullptr;
	std::vector<std::unordered_map<std::string, int>> scopes;
	int loop_depth = 0;
	// The member whose initializer is being checked: it and the members after
	// it are not initialized yet (spec §6.1).
	int initializing = -1;

	std::unordered_map<std::string, int> member_index;
	std::unordered_map<std::string, int> function_index;

	void error(int p_line, int p_column, const std::string &p_message) {
		errors.push_back({ p_line, p_column, p_message });
	}

	bool assignable(const Type &p_from, const Type &p_to) const {
		return is_assignable(p_from, p_to, engine);
	}

	bool is_type_name(const std::string &p_name) const {
		return p_name == "int" || p_name == "float" || p_name == "bool" || builtin::find_class(p_name) || engine.class_exists(p_name);
	}

	Type resolve(const TypeRef &p_ref, bool p_allow_void) {
		Type type = resolve_name(p_ref, p_allow_void);
		if (p_ref.nullable && !type.is_error()) {
			if (type.kind != TypeKind::Object) {
				error(p_ref.line, p_ref.column, "Optional values of type " + type.to_string() + " are not supported yet.");
				return Type::of(TypeKind::Error);
			}
			type.nullable = true;
		}
		return type;
	}

	Type resolve_name(const TypeRef &p_ref, bool p_allow_void) {
		if (p_ref.name == "void") {
			if (p_allow_void) {
				return Type::of(TypeKind::Void);
			}
			error(p_ref.line, p_ref.column, "void is only valid as a return type.");
			return Type::of(TypeKind::Error);
		}
		if (p_ref.name == "Variant" || p_ref.name == "Nil") {
			error(p_ref.line, p_ref.column, "The type " + p_ref.name + " is not supported yet.");
			return Type::of(TypeKind::Error);
		}
		if (builtin::find_class(p_ref.name)) {
			return api_type(p_ref.name, false);
		}
		if (engine.class_exists(p_ref.name)) {
			return Type::object(p_ref.name);
		}
		error(p_ref.line, p_ref.column, "Unknown type \"" + p_ref.name + "\".");
		return Type::of(TypeKind::Error);
	}

	/* Expressions */

	Type set(Expr &r_expr, const Type &p_type) {
		r_expr.type = p_type;
		return p_type;
	}

	Type fail(Expr &r_expr, const std::string &p_message) {
		error(r_expr.line, r_expr.column, p_message);
		return set(r_expr, Type::of(TypeKind::Error));
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

	// The type a name refers to when it is the base of `Name.x` or
	// `Name.f()`: not when a local, a member or a property has that name.
	bool names_type(const Expr &p_expr) const {
		return p_expr.kind == ExprKind::Name && find_local(p_expr.text) < 0 && !member_index.count(p_expr.text) &&
				!engine.find_property(cls.extends, p_expr.text) && is_type_name(p_expr.text);
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
			if (initializing >= 0 && member->second >= initializing) {
				return fail(r_expr, "\"" + r_expr.text + "\" is not initialized yet: initializers run in declaration order (spec §6.1).");
			}
			r_expr.target = Target::Member;
			r_expr.index = member->second;
			return set(r_expr, cls.members[member->second].type);
		}
		if (const auto property = engine.find_property(cls.extends, r_expr.text)) {
			if (!property->type.is_storable()) {
				return fail(r_expr, "The property \"" + r_expr.text + "\" has type " + property->type.to_string() + ", which is not supported yet.");
			}
			r_expr.target = Target::SelfProperty;
			return set(r_expr, property->type);
		}
		if (function_index.count(r_expr.text)) {
			return fail(r_expr, "Functions cannot be used as values yet; call " + r_expr.text + "().");
		}
		if (is_type_name(r_expr.text)) {
			return fail(r_expr, "\"" + r_expr.text + "\" is a type, not a value.");
		}
		return fail(r_expr, "Identifier \"" + r_expr.text + "\" is not declared in the current scope.");
	}

	Type check_member(Expr &r_expr) {
		Expr &base = *r_expr.base;
		if (names_type(base)) {
			base.target = Target::TypeName;
			if (const builtin::Class *type = builtin::find_class(base.text)) {
				if (const builtin::Constant *constant = builtin::find_constant(*type, r_expr.text)) {
					r_expr.target = Target::BuiltinConstant;
					r_expr.owner = base.text;
					return set(r_expr, api_type(constant->type, false));
				}
				return fail(r_expr, base.text + " has no constant \"" + r_expr.text + "\".");
			}
			return fail(r_expr, "Constants and enums of engine classes, such as " + base.text + "." + r_expr.text + ", are not supported yet.");
		}

		const Type base_type = check_value(base);
		if (base_type.is_error()) {
			return set(r_expr, base_type);
		}
		const std::string builtin_name = base_type.builtin_name();
		if (!builtin_name.empty()) {
			const builtin::Class *type = builtin::find_class(builtin_name);
			const builtin::Member *member = type ? builtin::find_member(*type, r_expr.text) : nullptr;
			if (!member) {
				return fail(r_expr, base_type.to_string() + " has no member \"" + r_expr.text + "\".");
			}
			r_expr.target = Target::Field;
			r_expr.owner = builtin_name;
			return set(r_expr, api_type(member->type, false));
		}
		if (base_type.kind == TypeKind::Object) {
			if (base_type.nullable) {
				return fail(r_expr, "The value may be None (" + base_type.to_string() + "); check it before reading \"" + r_expr.text + "\".");
			}
			const auto property = engine.find_property(base_type.name, r_expr.text);
			if (!property) {
				return fail(r_expr, base_type.name + " has no property \"" + r_expr.text + "\".");
			}
			if (!property->type.is_storable()) {
				return fail(r_expr, "The property \"" + r_expr.text + "\" has type " + property->type.to_string() + ", which is not supported yet.");
			}
			r_expr.target = Target::ObjectProperty;
			r_expr.owner = base_type.name;
			return set(r_expr, property->type);
		}
		return fail(r_expr, "Values of type " + base_type.to_string() + " have no members.");
	}

	Type check_unary(Expr &r_expr) {
		const Type operand = check_value(*r_expr.left);
		if (operand.is_error()) {
			return set(r_expr, operand);
		}
		if (r_expr.op == TokenKind::Minus) {
			if (operand.is_numeric()) {
				return set(r_expr, operand);
			}
			if (operand.kind == TypeKind::Builtin) {
				const builtin::Class *type = builtin::find_class(operand.name);
				for (uint32_t i = 0; type && i < type->operator_count; i++) {
					if (std::string(type->operators[i].name) == "unary-") {
						return set(r_expr, api_type(type->operators[i].return_type, false));
					}
				}
			}
			return fail(r_expr, "Cannot negate a value of type " + operand.to_string() + ".");
		}
		if (operand.kind == TypeKind::Bool) {
			return set(r_expr, operand);
		}
		return fail(r_expr, "\"not\" requires bool, found " + operand.to_string() + ". Compare explicitly, for example x == 0.");
	}

	// The result of `p_left op p_right` (spec §5.1), or Error after reporting.
	Type binary_type(TokenKind p_op, const Type &p_left, const Type &p_right, int p_line, int p_column) {
		if (p_left.is_error() || p_right.is_error()) {
			return Type::of(TypeKind::Error);
		}
		if (p_op == TokenKind::And || p_op == TokenKind::Or) {
			if (p_left.kind == TypeKind::Bool && p_right.kind == TypeKind::Bool) {
				return p_left;
			}
			error(p_line, p_column, std::string(token_name(p_op)) + " requires bool operands, found " + p_left.to_string() + " and " + p_right.to_string() + ".");
			return Type::of(TypeKind::Error);
		}
		if (p_left.is_primitive() && p_right.is_primitive()) {
			return primitive_binary_type(p_op, p_left, p_right, p_line, p_column);
		}
		// Other value types follow the engine's operator table.
		const std::string left = p_left.builtin_name();
		const std::string right = p_right.builtin_name();
		const builtin::Class *type = left.empty() ? nullptr : builtin::find_class(left);
		const std::string name = operator_name(p_op);
		for (uint32_t i = 0; type && !right.empty() && i < type->operator_count; i++) {
			const builtin::Operator &op = type->operators[i];
			if (name == op.name && right == op.right_type) {
				return api_type(op.return_type, false);
			}
		}
		error(p_line, p_column, std::string("Invalid operands ") + p_left.to_string() + " and " + p_right.to_string() + " for " + token_name(p_op) + ".");
		return Type::of(TypeKind::Error);
	}

	Type primitive_binary_type(TokenKind p_op, const Type &p_left, const Type &p_right, int p_line, int p_column) {
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
			default:
				break;
		}
		error(p_line, p_column, std::string("Invalid operands ") + p_left.to_string() + " and " + p_right.to_string() + " for " + token_name(p_op) + ".");
		return Type::of(TypeKind::Error);
	}

	Type check_binary(Expr &r_expr) {
		const Type left = check_value(*r_expr.left);
		const Type right = check_value(*r_expr.right);
		return set(r_expr, binary_type(r_expr.op, left, right, r_expr.line, r_expr.column));
	}

	void report_argument(const Expr &p_arg, const Type &p_arg_type, const Type &p_param) {
		std::string message = "Cannot pass a value of type " + p_arg_type.to_string() + " as " + p_param.to_string() + ".";
		if (p_arg_type.kind == TypeKind::Float && p_param.kind == TypeKind::Int) {
			message += " Convert explicitly with int(x).";
		}
		error(p_arg.line, p_arg.column, message);
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
			const Type arg_type = check_value(arg);
			const Type param = i < max ? p_params[i] : Type::of(TypeKind::Variant);
			r_call.param_types.push_back(param);
			if (!assignable(arg_type, param)) {
				report_argument(arg, arg_type, param);
			}
		}
	}

	std::vector<Type> engine_params(const EngineMethod &p_method, Expr &r_call, bool &r_ok) {
		std::vector<Type> params;
		r_ok = true;
		for (const EngineParam &param : p_method.params) {
			if (!param.type.is_storable()) {
				fail(r_call, "\"" + r_call.text + "()\" takes a parameter of type " + param.type.to_string() + ", which is not supported yet.");
				r_ok = false;
				return params;
			}
			params.push_back(param.type);
		}
		return params;
	}

	Type check_engine_call(Expr &r_expr, const EngineMethod &p_method) {
		if (p_method.is_static) {
			return fail(r_expr, "Static engine methods are not supported yet.");
		}
		if (p_method.is_virtual) {
			return fail(r_expr, "\"" + r_expr.text + "()\" is a virtual engine method.");
		}
		if (p_method.return_type.kind == TypeKind::Unsupported) {
			return fail(r_expr, "\"" + r_expr.text + "()\" returns " + p_method.return_type.to_string() + ", which is not supported yet.");
		}
		bool ok = false;
		const std::vector<Type> params = engine_params(p_method, r_expr, ok);
		if (!ok) {
			return Type::of(TypeKind::Error);
		}
		check_arguments(r_expr, params, int(params.size()) - p_method.default_count, p_method.is_vararg);
		r_expr.target = Target::EngineMethod;
		return set(r_expr, p_method.return_type);
	}

	Type check_utility(Expr &r_expr, const builtin::Utility &p_utility) {
		r_expr.target = Target::Utility;
		r_expr.hash = p_utility.hash;
		r_expr.is_vararg = p_utility.is_vararg;
		r_expr.engine_return = api_type(p_utility.return_type, true);
		const ArgumentTyped rule = argument_typed(r_expr.text);
		if (rule == ArgumentTyped::None) {
			std::vector<Type> params;
			for (uint32_t i = 0; i < p_utility.argument_count; i++) {
				params.push_back(api_type(p_utility.arguments[i].type, true));
			}
			for (const Type &type : params) {
				if (type.kind == TypeKind::Object || !type.is_storable()) {
					return fail(r_expr, "\"" + r_expr.text + "()\" takes " + type.to_string() + ", which is not supported yet.");
				}
			}
			if (r_expr.engine_return.kind == TypeKind::Object || r_expr.engine_return.kind == TypeKind::Unsupported) {
				return fail(r_expr, "\"" + r_expr.text + "()\" returns " + r_expr.engine_return.to_string() + ", which is not supported yet.");
			}
			int required = 0;
			for (uint32_t i = 0; i < p_utility.argument_count; i++) {
				required += p_utility.arguments[i].has_default ? 0 : 1;
			}
			check_arguments(r_expr, params, required, p_utility.is_vararg);
			return set(r_expr, r_expr.engine_return);
		}

		// Typed by the arguments (spec §10.3); the engine takes them as Variant.
		const size_t expected = rule == ArgumentTyped::Lerp ? 3 : p_utility.argument_count;
		if (r_expr.args.size() < expected || (!p_utility.is_vararg && r_expr.args.size() > expected)) {
			for (auto &arg : r_expr.args) {
				check_value(*arg);
			}
			return fail(r_expr, "Function \"" + r_expr.text + "()\" takes " + (p_utility.is_vararg ? "at least " : "") + std::to_string(expected) + " argument(s).");
		}
		std::vector<Type> types;
		for (auto &arg : r_expr.args) {
			types.push_back(check_value(*arg));
			r_expr.param_types.push_back(Type::of(TypeKind::Variant));
		}
		for (const Type &type : types) {
			if (type.is_error()) {
				return set(r_expr, type);
			}
		}
		const Type result = types[0];
		const size_t same_count = rule == ArgumentTyped::Lerp ? 2 : types.size();
		for (size_t i = 1; i < same_count; i++) {
			if (types[i] != result) {
				std::string message = "The arguments of " + r_expr.text + "() must have the same type, found " + result.to_string() + " and " + types[i].to_string() + ".";
				if (types[i].is_numeric() && result.is_numeric()) {
					message += " Write the int argument as a float, for example 1.0.";
				}
				return fail(*r_expr.args[i], message);
			}
		}
		if (rule == ArgumentTyped::Lerp) {
			if (!is_interpolable(result)) {
				return fail(r_expr, "lerp() interpolates float, vectors, Color, Quaternion and Basis, not " + result.to_string() + ".");
			}
			if (!types[2].is_numeric()) {
				return fail(*r_expr.args[2], "The weight of lerp() must be a number.");
			}
			// An int weight converts to float (spec §3.2) before it goes to the engine.
			r_expr.param_types[2] = Type::of(TypeKind::Float);
		} else if (!is_numeric_or_vector(result)) {
			return fail(r_expr, r_expr.text + "() takes numbers or vectors, not " + result.to_string() + ".");
		}
		return set(r_expr, result);
	}

	Type check_constructor(Expr &r_expr, const builtin::Class &p_type) {
		std::vector<Type> args;
		for (auto &arg : r_expr.args) {
			args.push_back(check_value(*arg));
		}
		for (const Type &type : args) {
			if (type.is_error()) {
				return set(r_expr, type);
			}
		}
		// The first constructor that takes these types exactly; otherwise the
		// first that takes them with conversions.
		int chosen = -1;
		for (uint32_t i = 0; i < p_type.constructor_count; i++) {
			const builtin::Constructor &constructor = p_type.constructors[i];
			if (constructor.argument_count != args.size()) {
				continue;
			}
			bool viable = true;
			bool exact = true;
			for (uint32_t a = 0; a < constructor.argument_count; a++) {
				const Type param = api_type(constructor.arguments[a].type, false);
				viable = viable && param.kind != TypeKind::Object && assignable(args[a], param);
				exact = exact && param == args[a];
			}
			if (viable && exact) {
				chosen = int(i);
				break;
			}
			if (viable && chosen < 0) {
				chosen = int(i);
			}
		}
		if (chosen < 0) {
			std::string types;
			for (const Type &type : args) {
				types += (types.empty() ? "" : ", ") + type.to_string();
			}
			return fail(r_expr, "No constructor of " + std::string(p_type.name) + " takes (" + types + ").");
		}
		const builtin::Constructor &constructor = p_type.constructors[chosen];
		for (uint32_t a = 0; a < constructor.argument_count; a++) {
			r_expr.param_types.push_back(api_type(constructor.arguments[a].type, false));
		}
		r_expr.target = Target::BuiltinConstructor;
		r_expr.index = chosen;
		r_expr.owner = p_type.name;
		return set(r_expr, api_type(p_type.name, false));
	}

	Type check_builtin_method(Expr &r_expr, const Type &p_base) {
		const std::string base_name = p_base.builtin_name();
		const builtin::Class *type = builtin::find_class(base_name);
		const builtin::Method *method = type ? builtin::find_method(*type, r_expr.text) : nullptr;
		if (!method) {
			return fail(r_expr, p_base.to_string() + " has no method \"" + r_expr.text + "()\".");
		}
		if (method->is_vararg || method->is_static) {
			return fail(r_expr, "\"" + r_expr.text + "()\" of " + base_name + " is not supported yet.");
		}
		std::vector<Type> params;
		int required = 0;
		for (uint32_t i = 0; i < method->argument_count; i++) {
			const Type param = api_type(method->arguments[i].type, true);
			if (param.kind == TypeKind::Object || !param.is_storable()) {
				return fail(r_expr, "\"" + r_expr.text + "()\" takes " + param.to_string() + ", which is not supported yet.");
			}
			params.push_back(param);
			required += method->arguments[i].has_default ? 0 : 1;
		}
		if (int(r_expr.args.size()) != int(params.size())) {
			// Default values of built-in methods are not passed yet.
			if (int(r_expr.args.size()) >= required && int(r_expr.args.size()) < int(params.size())) {
				return fail(r_expr, "\"" + r_expr.text + "()\" needs all of its " + std::to_string(params.size()) + " arguments so far.");
			}
		}
		const Type result = api_type(method->return_type, true);
		if (result.kind == TypeKind::Object || result.kind == TypeKind::Unsupported) {
			return fail(r_expr, "\"" + r_expr.text + "()\" returns " + result.to_string() + ", which is not supported yet.");
		}
		// A method that changes a value changes it where it came from, which
		// must be a place that can be written; Array and Dictionary are shared
		// and change in place.
		if (!method->is_const && !is_shared(p_base)) {
			const Expr &base = *r_expr.base;
			const bool place = base.target == Target::Local || base.target == Target::Member || base.target == Target::SelfProperty ||
					base.target == Target::ObjectProperty || base.target == Target::Field;
			if (!place) {
				return fail(r_expr, "\"" + r_expr.text + "()\" changes the " + base_name + " it is called on, which is a temporary value here.");
			}
			if (!check_place(base)) {
				return set(r_expr, Type::of(TypeKind::Error));
			}
		}
		check_arguments(r_expr, params, int(params.size()), false);
		r_expr.target = Target::BuiltinMethod;
		r_expr.owner = base_name;
		r_expr.hash = method->hash;
		r_expr.is_const = method->is_const;
		r_expr.engine_return = result;
		return set(r_expr, result);
	}

	Type check_call(Expr &r_expr) {
		const std::string &name = r_expr.text;

		if (r_expr.base && r_expr.base->kind != ExprKind::Self) {
			Expr &base = *r_expr.base;
			if (names_type(base)) {
				base.target = Target::TypeName;
				if (name == "new" && engine.class_exists(base.text) && !builtin::find_class(base.text)) {
					if (!engine.can_instantiate(base.text)) {
						return fail(r_expr, base.text + " cannot be instantiated.");
					}
					if (!r_expr.args.empty()) {
						return fail(r_expr, "new() of an engine class takes no arguments.");
					}
					r_expr.target = Target::NewObject;
					r_expr.owner = base.text;
					return set(r_expr, Type::object(base.text));
				}
				return fail(r_expr, "Static methods such as " + base.text + "." + name + "() are not supported yet.");
			}
			const Type base_type = check_value(base);
			if (base_type.is_error()) {
				for (auto &arg : r_expr.args) {
					check_value(*arg);
				}
				return set(r_expr, base_type);
			}
			if (!base_type.builtin_name().empty()) {
				return check_builtin_method(r_expr, base_type);
			}
			if (base_type.kind == TypeKind::Object) {
				if (base_type.nullable) {
					return fail(r_expr, "The value may be None (" + base_type.to_string() + "); check it before calling \"" + name + "()\".");
				}
				if (const auto method = engine.find_method(base_type.name, name)) {
					return check_engine_call(r_expr, *method);
				}
				return fail(r_expr, base_type.name + " has no method \"" + name + "()\".");
			}
			return fail(r_expr, "Values of type " + base_type.to_string() + " have no methods.");
		}

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
			if (method->is_virtual) {
				return fail(r_expr, "\"" + name + "()\" is a virtual engine method that this class does not define.");
			}
			r_expr.base = nullptr; // `self.f()` is `f()`.
			return check_engine_call(r_expr, *method);
		}

		if (!r_expr.base) {
			if (name == "int" || name == "float") {
				const TypeKind result = name == "int" ? TypeKind::Int : TypeKind::Float;
				r_expr.target = name == "int" ? Target::ConvertToInt : Target::ConvertToFloat;
				if (r_expr.args.size() != 1) {
					return fail(r_expr, name + "() takes one argument.");
				}
				const Type arg = check_value(*r_expr.args[0]);
				if (!arg.is_error() && !arg.is_numeric()) {
					return fail(*r_expr.args[0], name + "() of " + arg.to_string() + " is not supported yet.");
				}
				r_expr.param_types.push_back(arg);
				return set(r_expr, Type::of(result));
			}
			if (const builtin::Utility *utility = builtin::find_utility(name)) {
				return check_utility(r_expr, *utility);
			}
			if (name != "bool" && name != "String") {
				if (const builtin::Class *type = builtin::find_class(name)) {
					return check_constructor(r_expr, *type);
				}
			}
		}

		error(r_expr.line, r_expr.column, "Function \"" + name + "()\" not found in the class or its base " + cls.extends + ".");
		for (auto &arg : r_expr.args) {
			check_value(*arg);
		}
		return set(r_expr, Type::of(TypeKind::Error));
	}

	Type check_expr(Expr &r_expr) {
		switch (r_expr.kind) {
			case ExprKind::IntLiteral:
				if (r_expr.int_overflows) {
					return fail(r_expr, "Integer literal is too large for int.");
				}
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
				return fail(r_expr, "self as a value is not supported yet.");
			case ExprKind::Unary:
				return check_unary(r_expr);
			case ExprKind::Binary:
				return check_binary(r_expr);
			case ExprKind::Call:
				return check_call(r_expr);
			case ExprKind::Member:
				return check_member(r_expr);
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
		if (type.kind == TypeKind::Unsupported || type.kind == TypeKind::Variant) {
			error(r_expr.line, r_expr.column, "Values of type " + type.to_string() + " are not supported yet.");
			return Type::of(TypeKind::Error);
		}
		return type;
	}

	// Whether the expression is a place that can be written: a local, a
	// member, a property, or a member of a built-in value in such a place,
	// which is then written back (architecture §2.1).
	bool check_place(const Expr &p_target) {
		switch (p_target.target) {
			case Target::Local:
			case Target::Member:
				return true;
			case Target::SelfProperty:
			case Target::ObjectProperty: {
				const std::string &owner = p_target.target == Target::SelfProperty ? cls.extends : p_target.owner;
				const auto property = engine.find_property(owner, p_target.text);
				if (property && !property->writable) {
					error(p_target.line, p_target.column, "The property \"" + p_target.text + "\" is read-only.");
					return false;
				}
				return true;
			}
			case Target::Field:
				if (p_target.base->kind == ExprKind::Name || p_target.base->kind == ExprKind::Member) {
					return check_place(*p_target.base);
				}
				error(p_target.line, p_target.column, "Cannot assign to a member of a temporary value.");
				return false;
			default:
				if (!p_target.type.is_error()) {
					error(p_target.line, p_target.column, "Cannot assign to this expression.");
				}
				return false;
		}
	}

	void expect_assignable(const Expr &p_value, const Type &p_from, const Type &p_to, const char *p_action) {
		if (assignable(p_from, p_to)) {
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
				} else if (type.kind == TypeKind::Object && !type.nullable) {
					error(r_stmt.line, r_stmt.column, "\"" + r_stmt.name + "\" of type " + type.to_string() + " needs an initial value.");
				}
				declare_local(r_stmt, type);
				return;
			}
			case StmtKind::Assign: {
				Expr &target = *r_stmt.target;
				const Type target_type = target.kind == ExprKind::Name || target.kind == ExprKind::Member ? check_expr(target) : Type::of(TypeKind::Error);
				if (target.kind != ExprKind::Name && target.kind != ExprKind::Member) {
					check_expr(target);
					error(target.line, target.column, "Cannot assign to this expression.");
					check_value(*r_stmt.value);
					return;
				}
				Type value = check_value(*r_stmt.value);
				if (target_type.is_error() || !check_place(target)) {
					return;
				}
				if (r_stmt.assign_op != TokenKind::Equal) {
					value = binary_type(compound_operator(r_stmt.assign_op), target_type, value, r_stmt.line, r_stmt.column);
					r_stmt.compound_result = value;
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

	void declare_members() {
		for (size_t i = 0; i < cls.members.size(); i++) {
			VarDecl &member = cls.members[i];
			if (member_index.count(member.name)) {
				error(member.line, member.column, "The member \"" + member.name + "\" is already declared.");
				continue;
			}
			member_index[member.name] = int(i);
		}
	}

	// After the signatures, since an initializer can call this class's functions.
	void check_members() {
		for (size_t i = 0; i < cls.members.size(); i++) {
			VarDecl &member = cls.members[i];
			const auto registered = member_index.find(member.name);
			if (registered == member_index.end() || registered->second != int(i)) {
				continue;
			}
			Type type = Type::of(TypeKind::Error);
			if (member.declared_type) {
				type = resolve(*member.declared_type, false);
			}
			if (member.initializer) {
				initializing = int(i);
				const Type value = check_value(*member.initializer);
				initializing = -1;
				if (member.declared_type) {
					expect_assignable(*member.initializer, value, type, "assign");
				} else {
					type = value;
				}
				member.literal_initializer = is_literal(*member.initializer) && type.is_primitive();
			} else if (!member.declared_type) {
				error(member.line, member.column, "\"" + member.name + "\" needs a type or an initial value.");
			} else if (type.kind == TypeKind::Object && !type.nullable) {
				// Spec §6.2.
				error(member.line, member.column, "Members that must be assigned, such as \"" + member.name + "\" of type " + type.to_string() + ", are not supported yet; give it an initial value.");
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
				// The engine passes its own objects to virtual methods.
				engine_type.nullable = false;
				if (!engine_type.is_storable() || engine_type.kind == TypeKind::Variant) {
					error(param.line, param.column, "Parameters of type " + engine_type.to_string() + " are not supported yet.");
					engine_type = Type::of(TypeKind::Error);
				}
			}
			if (param.declared_type) {
				param.type = resolve(*param.declared_type, false);
				// A parameter may accept more than the overridden one does, such as
				// T? for T (spec §4.6).
				if (r_function.is_override && !engine_type.is_error() && !param.type.is_error() && !assignable(engine_type, param.type)) {
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

		// The engine constructs Node and Resource scripts with no arguments
		// (spec §6.2); RefCounted constructors with arguments come with new(...).
		if (r_function.name == "_init" && !r_function.params.empty()) {
			const bool engine_made = engine.inherits(cls.extends, "Node") || engine.inherits(cls.extends, "Resource");
			error(r_function.line, r_function.column, engine_made ? "_init() of a " + cls.extends + " script cannot take parameters: the engine constructs it with none. Use members that must be assigned instead (spec §6.2)." : "_init() with parameters is not supported yet.");
		}
		if (r_function.name == "_init" && r_function.return_type_ref && r_function.return_type_ref->name != "void") {
			error(r_function.return_type_ref->line, r_function.return_type_ref->column, "_init() returns nothing.");
		}

		if (r_function.return_type_ref) {
			r_function.return_type = resolve(*r_function.return_type_ref, true);
			if (r_function.is_override && !engine_method->return_type.is_error() && r_function.return_type != engine_method->return_type) {
				error(r_function.return_type_ref->line, r_function.return_type_ref->column, "The overridden method returns " + engine_method->return_type.to_string() + ".");
			}
		} else if (r_function.is_override) {
			r_function.return_type = engine_method->return_type;
			if (r_function.return_type.kind != TypeKind::Void && (!r_function.return_type.is_storable() || r_function.return_type.kind == TypeKind::Variant)) {
				error(r_function.line, r_function.column, "Return values of type " + r_function.return_type.to_string() + " are not supported yet.");
				r_function.return_type = Type::of(TypeKind::Error);
			}
		} else {
			r_function.return_type = Type::of(TypeKind::Void);
		}
	}

	void declare_params(FuncDecl &r_function) {
		for (const ParamDecl &param : r_function.params) {
			if (scopes.back().count(param.name)) {
				error(param.line, param.column, "Parameter \"" + param.name + "\" is declared twice.");
			}
			scopes.back()[param.name] = int(r_function.locals.size());
			r_function.locals.push_back({ param.name, param.type });
		}
	}

	// Connections saved in scenes to methods of this class (spec §7.7): the
	// method must exist and take what the signal passes; the engine finds it
	// by name (§10.5).
	void check_connections() {
		for (const Scene &scene : scenes) {
			for (const SceneConnection &connection : scene.connections) {
				const SceneNode *target = scene.find_node(connection.target);
				if (!target || target->script != options.path) {
					continue;
				}
				const std::string where = scene.path + ": the signal \"" + connection.signal + "\" of " + connection.source + " is connected to " + connection.method + "()";
				const auto found = function_index.find(connection.method);
				if (found == function_index.end()) {
					error(cls.extends_line, cls.extends_column, where + ", which this class does not declare.");
					continue;
				}
				FuncDecl &method = cls.functions[found->second];
				method.is_exposed = true;
				const SceneNode *source = scene.find_node(connection.source);
				const auto signal = source ? engine.find_signal(source->type, connection.signal) : std::nullopt;
				if (!signal) {
					continue; // A signal declared by a script; checked once script signals are typed.
				}
				const int passed = int(signal->params.size()) - connection.unbinds + connection.binds;
				if (int(method.params.size()) != passed) {
					error(method.line, method.column, where + ", which passes " + std::to_string(passed) + " argument(s); " + method.name + "() takes " + std::to_string(method.params.size()) + ".");
					continue;
				}
				for (int i = 0; i < int(signal->params.size()) - connection.unbinds; i++) {
					Type argument = signal->params[i].type;
					argument.nullable = false; // The engine's own signal arguments.
					if (!assignable(argument, method.params[i].type)) {
						error(method.params[i].line, method.params[i].column, where + ", which passes " + argument.to_string() + " here.");
					}
				}
			}
		}
	}

	void check_body(FuncDecl &r_function) {
		function = &r_function;
		scopes.clear();
		scopes.emplace_back();
		declare_params(r_function);
		if (options.declarations_only) {
			function = nullptr;
			scopes.clear();
			return;
		}
		check_block(r_function.body);
		if (r_function.return_type.kind != TypeKind::Void && !r_function.return_type.is_error() && !always_returns(r_function.body)) {
			error(r_function.line, r_function.column, "Not all paths of \"" + r_function.name + "()\" return a value.");
		}
		function = nullptr;
		scopes.clear();
	}

public:
	Checker(ClassDecl &r_class, const EngineApi &p_engine, const CompileOptions &p_options, std::vector<Diagnostic> &r_errors) :
			cls(r_class), engine(p_engine), options(p_options), errors(r_errors) {}

	void run() {
		if (!engine.class_exists(cls.extends)) {
			error(cls.extends_line, cls.extends_column, "Unknown base class \"" + cls.extends + "\".");
			return;
		}
		if (options.project && !options.declarations_only && !options.path.empty()) {
			scenes = options.project->scenes_with_script(options.path);
		}
		declare_members();
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
		check_members();
		check_connections();
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

void check(ClassDecl &r_class, const EngineApi &p_engine, const CompileOptions &p_options, std::vector<Diagnostic> &r_errors) {
	Checker(r_class, p_engine, p_options, r_errors).run();
}

} // namespace petrichor::compiler
