#include "compiler/checker_internal.h"

namespace petrichor::compiler::checking {

const char *operator_name(TokenKind p_op) {
	switch (p_op) {
		case TokenKind::Plus:
			return "+";
		case TokenKind::Minus:
			return "-";
		case TokenKind::Star:
			return "*";
		case TokenKind::StarStar:
			return "**";
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
		case TokenKind::ShiftLeft:
			return "<<";
		case TokenKind::ShiftRight:
			return ">>";
		case TokenKind::Ampersand:
			return "&";
		case TokenKind::Pipe:
			return "|";
		case TokenKind::Caret:
			return "^";
		default:
			return "";
	}
}

/* Operators (spec §5.1) */

Type Checker::check_unary(Expr &r_expr) {
	const Type operand = as_int(check_value(*r_expr.left));
	if (operand.is_error()) {
		return set(r_expr, operand);
	}
	if (r_expr.op != TokenKind::Minus && r_expr.op != TokenKind::Tilde) {
		if (operand.kind == TypeKind::Bool) {
			return set(r_expr, operand);
		}
		return fail(r_expr, "\"not\" requires bool, found " + operand.to_string() + "." + compare_hint(operand, true));
	}
	if (operand.kind == TypeKind::Variant) {
		return fail(r_expr, std::string("Cannot use ") + token_name(r_expr.op) + " on a Variant." + variant_hint());
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
	return operand.kind == TypeKind::Int ? set(r_expr, operand) : fail(r_expr, "\"~\" requires int, found " + operand.to_string() + ".");
}

// How to write what GDScript's truth test of a value means: `x != 0`, or
// for `not x` `x == 0` (spec §5.4); empty for a type it has none for.
std::string Checker::compare_hint(const Type &p_type, bool p_negated) {
	const std::string op = p_negated ? " == " : " != ";
	if (p_type.is_numeric()) {
		return " Compare explicitly, for example x" + op + "0.";
	}
	if (p_type.kind == TypeKind::String) {
		return " Compare explicitly, for example x" + op + "\"\".";
	}
	if (p_type.is_optional() || p_type.kind == TypeKind::Variant) {
		return " Compare explicitly, for example x" + op + "None.";
	}
	if (p_type.kind == TypeKind::Builtin && (p_type.name == "Array" || p_type.name == "Dictionary" || p_type.name.rfind("Packed", 0) == 0)) {
		return std::string(" Test it explicitly, for example ") + (p_negated ? "x.is_empty()." : "not x.is_empty().");
	}
	return std::string();
}

// The result of `p_left op p_right` (spec §5.1), or Error after reporting;
// with `r_compared`, the type both sides of == are compared as, where they
// convert to it: Variant, or the deeper T? (§5.2).
Type Checker::binary_type(TokenKind p_op, const Type &p_left, const Type &p_right, const TokenRange &p_at, Type *r_compared) {
	if (p_left.is_error() || p_right.is_error()) {
		return Type::of(TypeKind::Error);
	}
	// Formats a value of any type, which enters the engine as anywhere else
	// (spec §5.1, §10.1).
	if (p_op == TokenKind::Percent && is_format_string(p_left)) {
		if (!assignable(p_right, Type::of(TypeKind::Variant))) {
			error(p_at, "Cannot format a value of type " + p_right.to_string() + "." + variant_refusal(p_right));
			return Type::of(TypeKind::Error);
		}
		return Type::of(TypeKind::String);
	}
	const bool ordering = p_op == TokenKind::Less || p_op == TokenKind::LessEqual || p_op == TokenKind::Greater || p_op == TokenKind::GreaterEqual;
	const bool equality = p_op == TokenKind::EqualEqual || p_op == TokenKind::BangEqual;
	// Ordered by LESS (spec §3.6, §5.2), the types of both sides being one:
	// `<` does not wrap a T into T?.
	if (ordering && p_left == p_right && (p_left.kind == TypeKind::Struct || p_left.kind == TypeKind::PayloadEnum || p_left.kind == TypeKind::Option || (is_typed_container(p_left) && p_left.name == "Array"))) {
		if (!has_less(p_left)) {
			error(p_at, "Values of type " + p_left.to_string() + " are not ordered: a struct or an enum with payloads is when all its fields are, T? and Array[T] when T is.");
			return Type::of(TypeKind::Error);
		}
		return Type::of(TypeKind::Bool);
	}
	// The side with fewer levels of T? is wrapped (spec §5.2).
	if (equality && (p_left.kind == TypeKind::Option || p_right.kind == TypeKind::Option)) {
		const bool left_deeper = option_depth(p_left) >= option_depth(p_right);
		if (wraps_to(left_deeper ? p_right : p_left, left_deeper ? p_left : p_right)) {
			if (r_compared) {
				*r_compared = left_deeper ? p_left : p_right;
			}
			return Type::of(TypeKind::Bool);
		}
	}
	if (p_left.kind == TypeKind::Option || p_right.kind == TypeKind::Option) {
		const Type &optional = p_left.kind == TypeKind::Option ? p_left : p_right;
		error(p_at, "The value may be None (" + optional.to_string() + "); unwrap it first.");
		return Type::of(TypeKind::Error);
	}
	if (p_op == TokenKind::And || p_op == TokenKind::Or) {
		if (p_left.kind == TypeKind::Bool && p_right.kind == TypeKind::Bool) {
			return p_left;
		}
		error(p_at, std::string(token_name(p_op)) + " requires bool operands, found " + p_left.to_string() + " and " + p_right.to_string() + ".");
		return Type::of(TypeKind::Error);
	}
	// Only == and != take a Variant, by the engine's operator table when
	// the program runs (spec §3.13, §5.2).
	if (p_left.kind == TypeKind::Variant || p_right.kind == TypeKind::Variant) {
		const Type &other = p_left.kind == TypeKind::Variant ? p_right : p_left;
		if (equality && assignable(other, Type::of(TypeKind::Variant))) {
			if (r_compared) {
				*r_compared = Type::of(TypeKind::Variant);
			}
			return Type::of(TypeKind::Bool);
		}
		error(p_at, equality ? "Cannot compare a Variant with " + other.to_string() + "." : std::string("Cannot use ") + token_name(p_op) + " on a Variant." + variant_hint());
		return Type::of(TypeKind::Error);
	}
	// Objects and tasks by identity (spec §5.2), when one could be the other:
	// two classes that do not extend one another never share an object.
	if (equality && p_left.kind == p_right.kind && (p_left.kind == TypeKind::Object || p_left.kind == TypeKind::Task)) {
		Type left = p_left;
		Type right = p_right;
		left.nullable = right.nullable = false;
		if (left.is_interface || right.is_interface || assignable(left, right) || assignable(right, left)) {
			return Type::of(TypeKind::Bool);
		}
		const bool object = left.kind == TypeKind::Object;
		error(p_at, left.to_string() + " and " + right.to_string() + (object ? " are never the same object, since neither class extends the other." : " are never the same task, since neither result type is the other."));
		return Type::of(TypeKind::Error);
	}
	if (p_left.kind == TypeKind::Enum || p_right.kind == TypeKind::Enum) {
		return binary_type(p_op, as_int(p_left), as_int(p_right), p_at);
	}
	if (p_left.is_primitive() && p_right.is_primitive()) {
		return primitive_binary_type(p_op, p_left, p_right, p_at);
	}
	// Structs compare field by field (spec §3.6), enums with payloads by
	// variant and then so (§3.7).
	if ((p_left.kind == TypeKind::Struct || p_left.kind == TypeKind::PayloadEnum) && equality && p_left == p_right) {
		return Type::of(TypeKind::Bool);
	}
	// Other value types follow the engine's operator table.
	const std::string left = p_left.builtin_name();
	const std::string right = p_right.builtin_name();
	// The sum of two arrays typed alike is typed so too, in the engine and in
	// GDScript's static checks (core/variant/variant_op.h:665-667,
	// modules/gdscript/gdscript_analyzer.cpp:6301-6307).
	if (p_op == TokenKind::Plus && left == "Array" && right == "Array" && !p_left.args.empty() && p_left.args == p_right.args) {
		return p_left;
	}
	if (equality && left == "Array" && right == "Array" && !p_left.args.empty() && !p_right.args.empty() && p_left.args[0] != p_right.args[0]) {
		// The engine compares elements only of the same type (spec §5.2).
		error(p_at, p_left.to_string() + " and " + p_right.to_string() + " are equal only when both are empty, so comparing them is an error.");
		return Type::of(TypeKind::Error);
	}
	const builtin::Class *type = left.empty() ? nullptr : builtin::find_class(left);
	const std::string name = operator_name(p_op);
	for (uint32_t i = 0; type && !right.empty() && i < type->operator_count; i++) {
		const builtin::Operator &op = type->operators[i];
		if (name == op.name && right == op.right_type) {
			return api_type(op.return_type, false);
		}
	}
	error(p_at, std::string("Invalid operands ") + p_left.to_string() + " and " + p_right.to_string() + " for " + token_name(p_op) + ".");
	return Type::of(TypeKind::Error);
}

Type Checker::primitive_binary_type(TokenKind p_op, const Type &p_left, const Type &p_right, const TokenRange &p_at) {
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
		case TokenKind::StarStar:
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
				error(p_at, "\"%\" requires int operands; use fmod() for float.");
				return Type::of(TypeKind::Error);
			}
			break;
		case TokenKind::ShiftLeft:
		case TokenKind::ShiftRight:
		case TokenKind::Ampersand:
		case TokenKind::Pipe:
		case TokenKind::Caret:
			if (both_int) {
				return number;
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
			error(p_at, "Cannot compare " + p_left.to_string() + " with " + p_right.to_string() + ".");
			return Type::of(TypeKind::Error);
		default:
			break;
	}
	error(p_at, std::string("Invalid operands ") + p_left.to_string() + " and " + p_right.to_string() + " for " + token_name(p_op) + ".");
	return Type::of(TypeKind::Error);
}

Type Checker::check_binary(Expr &r_expr) {
	const bool equality = r_expr.op == TokenKind::EqualEqual || r_expr.op == TokenKind::BangEqual;
	if (equality && (r_expr.left->kind == ExprKind::NoneLiteral || r_expr.right->kind == ExprKind::NoneLiteral)) {
		return check_none_comparison(r_expr);
	}
	if (r_expr.op == TokenKind::In) {
		return check_in(r_expr);
	}
	const Type left = check_value(*r_expr.left);
	Type right;
	if (r_expr.op == TokenKind::And || r_expr.op == TokenKind::Or) {
		// The right side runs only when the left is true (and) or false
		// (or), and narrows accordingly (spec §5.4).
		const Facts facts = facts_of(*r_expr.left);
		narrowed.push_back(r_expr.op == TokenKind::And ? facts.when_true : facts.when_false);
		right = check_value(*r_expr.right);
		narrowed.pop_back();
	} else if (r_expr.op == TokenKind::Percent && is_format_string(left)) {
		// The values to format, of any types (spec §5.1).
		right = check_value(*r_expr.right, Type::builtin("Array"));
	} else {
		// Ok(v) and Err(e) take their type from the Result they are compared
		// with (spec §3.5).
		right = check_value(*r_expr.right, left.is_result() ? left : Type::of(TypeKind::Error));
	}
	Type compared = Type::of(TypeKind::Error);
	const Type type = binary_type(r_expr.op, left, right, r_expr.range, &compared);
	const bool variant = compared.kind == TypeKind::Variant;
	r_expr.target = variant ? Target::VariantEqual : Target::None;
	r_expr.param_types = variant || compared.is_error() ? std::vector<Type>() : std::vector<Type>{ compared, compared };
	return set(r_expr, type);
}

// `x == None` and `x != None` (spec §5.2), which narrow x (§5.4).
Type Checker::check_none_comparison(Expr &r_expr) {
	Expr &value = r_expr.left->kind == ExprKind::NoneLiteral ? *r_expr.right : *r_expr.left;
	const Type type = check_value(value);
	set(r_expr.left->kind == ExprKind::NoneLiteral ? *r_expr.left : *r_expr.right, type);
	if (type.is_error()) {
		return set(r_expr, type);
	}
	if (!type.is_optional() && type.kind != TypeKind::Variant) {
		const std::string hint = type.kind == TypeKind::Object ? " Check whether it is alive with is_instance_valid(x), or make its type " + type.to_string() + "?." : "";
		return fail(r_expr, "A value of type " + type.to_string() + " is never None." + hint);
	}
	r_expr.target = Target::NoneComparison;
	return set(r_expr, Type::of(TypeKind::Bool));
}

// `x in c` (spec §5.1): c.has(x) of an Array, a Dictionary or a packed
// array, c.contains(x) of a String or StringName, so x takes the type
// those take; or, on an object, whether the engine finds a member named x.
Type Checker::check_in(Expr &r_expr) {
	const Type container = check_value(*r_expr.right);
	if (container.is_error()) {
		check_value(*r_expr.left);
		return set(r_expr, Type::of(TypeKind::Error));
	}
	if (container.kind == TypeKind::Variant) {
		check_value(*r_expr.left);
		return fail(r_expr, "Cannot look for a value in a Variant." + variant_hint());
	}
	if (container.kind == TypeKind::Object) {
		const Type name = check_value(*r_expr.left);
		if (container.nullable) {
			return fail(r_expr, "The value may be None (" + container.to_string() + "); unwrap it first.");
		}
		if (!name.is_error() && name.kind != TypeKind::String && name != Type::builtin("StringName")) {
			error(r_expr.left->range, "\"in\" on an object looks for a member by name, a String or StringName, not " + name.to_string() + ".");
			return set(r_expr, Type::of(TypeKind::Error));
		}
		r_expr.target = Target::HasMember;
		return set(r_expr, Type::of(TypeKind::Bool));
	}
	const std::string base = container.kind == TypeKind::String ? std::string("String") : container.builtin_name();
	const bool text = base == "String" || base == "StringName";
	const bool listed = base == "Array" || base == "Dictionary" || base.rfind("Packed", 0) == 0;
	const builtin::Class *type = text || listed ? builtin::find_class(base) : nullptr;
	const builtin::Method *method = type ? builtin::find_method(*type, text ? "contains" : "has") : nullptr;
	if (!method || method->argument_count != 1) {
		const Type value = check_value(*r_expr.left);
		if (container.kind == TypeKind::Option) {
			return fail(r_expr, "The value may be None (" + container.to_string() + "); unwrap it first.");
		}
		return value.is_error() ? set(r_expr, value) : fail(r_expr, "Invalid operands " + value.to_string() + " and " + container.to_string() + " for \"in\".");
	}
	r_expr.engine_params = { api_type(method->arguments[0].type, false) };
	Type element = r_expr.engine_params[0];
	if ((base == "Array" || base == "Dictionary") && !container.args.empty()) {
		element = container.args[0]; // The element or key type (spec §3.8).
	}
	const Type value = check_value(*r_expr.left, element);
	if (!value.is_error() && !assignable(value, element)) {
		std::string message = "\"in\" looks for a value of type " + element.to_string() + " in " + container.to_string() + ", not " + value.to_string() + ".";
		if (value.kind == TypeKind::Float && element.kind == TypeKind::Int) {
			message += " Convert explicitly with int(x).";
		}
		error(r_expr.left->range, message);
		return set(r_expr, Type::of(TypeKind::Error));
	}
	r_expr.target = Target::Contains;
	r_expr.owner = base;
	r_expr.text = method->name;
	r_expr.param_types = { element };
	// Through an untyped view, checked as the program runs (spec §3.8).
	r_expr.checks_elements = (base == "Array" || base == "Dictionary") && container.args.empty();
	return set(r_expr, Type::of(TypeKind::Bool));
}

// `x is T` (spec §5.4).
Type Checker::check_type_test(Expr &r_expr) {
	const Type value = check_value(*r_expr.left);
	const Type tested = resolve(r_expr.cast_type, false);
	if (value.is_error() || tested.is_error()) {
		return set(r_expr, Type::of(TypeKind::Error));
	}
	// What a Variant holds, or of an untyped container, whether it is typed
	// so (spec §3.8, §3.13).
	const bool untyped = value.kind == TypeKind::Builtin && value.args.empty() && is_typed_container(tested) && tested.name == value.name;
	if (value.kind == TypeKind::Variant || untyped) {
		const bool object = tested.kind == TypeKind::Object && !tested.nullable;
		if (!object && !testable(tested)) {
			return fail(r_expr, "is tests classes and built-in types, not " + tested.to_string() + ".");
		}
		r_expr.tested = tested;
		return set(r_expr, Type::of(TypeKind::Bool));
	}
	if (tested.kind != TypeKind::Object || tested.nullable) {
		return fail(r_expr, "is tests classes, not " + tested.to_string() + ".");
	}
	if (value.kind != TypeKind::Object) {
		return fail(r_expr, "is tests objects, not " + value.to_string() + ".");
	}
	if (!related(value, tested)) {
		return fail(r_expr, value.name + " and " + tested.name + " are unrelated classes; is would always be false.");
	}
	r_expr.tested = tested;
	return set(r_expr, Type::of(TypeKind::Bool));
}

// A built-in type whose values a Variant tells by their engine type, a
// container by the types the engine keeps for it (spec §3.8).
bool Checker::testable(const Type &p_type) {
	if (p_type.is_primitive()) {
		return true;
	}
	if (p_type.kind != TypeKind::Builtin || is_boxed(p_type)) {
		return false;
	}
	return std::none_of(p_type.args.begin(), p_type.args.end(), [](const Type &p_arg) { return p_arg.is_interface || is_typed_container(p_arg); });
}

/* Narrowing (spec §5.4) */

Checker::Facts Checker::facts_of(const Expr &p_condition) const {
	Facts facts;
	if (p_condition.type.is_error()) {
		return facts;
	}
	switch (p_condition.kind) {
		case ExprKind::Binary:
			if (p_condition.target == Target::NoneComparison) {
				const Expr &value = p_condition.left->kind == ExprKind::NoneLiteral ? *p_condition.right : *p_condition.left;
				const int local = tested_local(value);
				if (local >= 0) {
					(p_condition.op == TokenKind::BangEqual ? facts.when_true : facts.when_false)[local] = value.type.unwrapped();
				}
			} else if (p_condition.op == TokenKind::And) {
				facts.when_true = facts_of(*p_condition.left).when_true;
				for (const auto &fact : facts_of(*p_condition.right).when_true) {
					facts.when_true[fact.first] = fact.second;
				}
			} else if (p_condition.op == TokenKind::Or) {
				facts.when_false = facts_of(*p_condition.left).when_false;
				for (const auto &fact : facts_of(*p_condition.right).when_false) {
					facts.when_false[fact.first] = fact.second;
				}
			}
			break;
		case ExprKind::Unary:
			if (p_condition.op == TokenKind::Not) {
				const Facts inner = facts_of(*p_condition.left);
				facts.when_true = inner.when_false;
				facts.when_false = inner.when_true;
			}
			break;
		case ExprKind::TypeTest: {
			const int local = tested_local(*p_condition.left);
			if (local >= 0) {
				(p_condition.bool_value ? facts.when_false : facts.when_true)[local] = p_condition.tested;
			}
			break;
		}
		default:
			break;
	}
	return facts;
}

// The narrowed type of a local, if any; the innermost fact wins.
const Type *Checker::narrowed_in(const std::vector<std::unordered_map<int, Type>> &p_narrowed, int p_local) {
	for (auto map = p_narrowed.rbegin(); map != p_narrowed.rend(); ++map) {
		const auto found = map->find(p_local);
		if (found != map->end()) {
			return &found->second;
		}
	}
	return nullptr;
}

// An assignment ends what was known about the local, also in the
// enclosing blocks.
void Checker::forget(int p_local) {
	for (auto &map : narrowed) {
		map.erase(p_local);
	}
}

// A Callable that a function value goes into is of that function's type.
void Checker::narrow_by_value(int p_local, const Type &p_value) {
	if (p_value.kind == TypeKind::Function && function->locals[p_local].type == Type::builtin("Callable")) {
		narrowed.back()[p_local] = p_value;
	}
}

void Checker::add_facts(const std::unordered_map<int, Type> &p_facts) {
	for (const auto &fact : p_facts) {
		narrowed.back()[fact.first] = fact.second;
	}
}

// Before a loop: what its body assigns is not known on the next iteration.
void Checker::forget_assigned(const Block &p_block) {
	for (const auto &stmt : p_block) {
		if (stmt->kind == StmtKind::Assign && stmt->target->kind == ExprKind::Name) {
			const int local = find_local(stmt->target->text);
			if (local >= 0) {
				forget(local);
			}
		}
		forget_assigned(stmt->body);
		forget_assigned(stmt->else_body);
		for (const MatchBranch &branch : stmt->branches) {
			forget_assigned(branch.body);
		}
	}
}

// Ends on every path with return, or, when `p_or_jump`, also with break or
// continue, so that it leaves the block.
bool Checker::always_ends(const Block &p_block, bool p_or_jump) {
	if (p_block.empty()) {
		return false;
	}
	const Stmt &last = *p_block.back();
	if (last.kind == StmtKind::Return || (p_or_jump && (last.kind == StmtKind::Break || last.kind == StmtKind::Continue))) {
		return true;
	}
	if (last.kind == StmtKind::Match) {
		// Some branch takes every value, and every branch leaves.
		for (const MatchBranch &branch : last.branches) {
			if (!always_ends(branch.body, p_or_jump)) {
				return false;
			}
		}
		return last.exhaustive;
	}
	return last.kind == StmtKind::If && always_ends(last.body, p_or_jump) && always_ends(last.else_body, p_or_jump);
}

/* Lambdas (spec §3.11) */

// `func(x): body` (spec §3.11): code of the function around it, checked
// as a function of its own. Its parameters take the types of the function
// type it goes to, unless written, and its return type is what it returns
// (§4.6). Checked once, also when a call checks its arguments again.
Type Checker::check_lambda(Expr &r_expr, const Type &p_expected) {
	FuncDecl &lambda = *r_expr.function;
	if (lambda.checked) {
		return r_expr.type;
	}
	lambda.checked = true;
	if (in_constant) {
		return fail(r_expr, "A lambda is not a constant expression.");
	}
	lambda.is_static = function ? function->is_static : static_initializing >= 0;
	// A function type has no rest parameter (spec §4.6).
	const bool typed_by_expected = p_expected.kind == TypeKind::Function && p_expected.args.size() == lambda.params.size() + 1 && !lambda.has_rest();
	for (size_t i = 0; i < lambda.params.size(); i++) {
		ParamDecl &param = lambda.params[i];
		if (param.default_value) {
			error(param.default_value->range, "Lambda parameters have no default values.");
		}
		if (param.declared_type) {
			param.type = resolve(*param.declared_type, false);
		} else if (typed_by_expected) {
			param.type = p_expected.args[i + 1];
		} else {
			untyped(param, ": nothing the lambda goes to gives one");
		}
	}
	check_rest(lambda);
	Returns inferred;
	if (typed_by_expected) {
		inferred.expected = p_expected.args[0];
	}
	Returns *const outer_returns = returns;
	if (lambda.return_type_ref) {
		lambda.return_type = resolve(*lambda.return_type_ref, true);
		returns = nullptr;
	} else {
		returns = &inferred;
	}

	enclosing.push_back({ function, std::move(scopes), std::move(narrowed), &r_expr, loop_depth, in_predelete, initializing, initializing_onready, static_initializing });
	function = &lambda;
	scopes = { {} };
	narrowed = { {} };
	loop_depth = 0;
	in_predelete = false;
	const bool was_assert = in_assert;
	in_assert = false;
	initializing = -1;
	static_initializing = -1;
	declare_params(lambda);
	check_block(lambda.body);
	in_assert = was_assert;
	if (!lambda.return_type_ref) {
		lambda.return_type = inferred.type.value_or(Type::of(TypeKind::Void));
		if (inferred.type && inferred.bare) {
			error(r_expr.range, function_label() + " returns a value on some paths and none on others.");
		}
		if (partial(lambda.return_type)) {
			const Type &left = lambda.return_type;
			const std::string missing = !left.is_result() ? "which T? None is" : (left.args[0].is_error() ? "what Ok(...) holds" : "what Err(...) holds");
			error(r_expr.range, function_label() + " needs a return type: what it returns does not say " + missing + ".");
			lambda.return_type = Type::of(TypeKind::Error);
		}
		for (Expr *value : inferred.values) {
			if (partial(value->type)) {
				set(*value, lambda.return_type);
			}
			expect_scene_fits(*value, lambda.return_type);
		}
	}
	if (lambda.return_type.kind != TypeKind::Void && !lambda.return_type.is_error() && !always_ends(lambda.body, false)) {
		error(r_expr.range, "Not all paths of " + (lambda.name.empty() ? std::string("the lambda") : "the lambda \"" + lambda.name + "\"") + " return a value.");
	}
	// Its task keeps the result as the engine does (§9.8).
	const std::string loss = lambda.is_async ? encoding_loss(lambda.return_type, "the engine", false) : std::string();
	if (!loss.empty()) {
		error(r_expr.range, function_label() + " returns " + lambda.return_type.to_string() + ", but " + loss);
	}
	Enclosing &outer = enclosing.back();
	function = outer.function;
	scopes = std::move(outer.scopes);
	narrowed = std::move(outer.narrowed);
	loop_depth = outer.loop_depth;
	in_predelete = outer.in_predelete;
	initializing = outer.initializing;
	initializing_onready = outer.initializing_onready;
	static_initializing = outer.static_initializing;
	enclosing.pop_back();
	returns = outer_returns;

	return set(r_expr, function_type(lambda));
}

// The lambda or method value with a rest parameter that the value is, a
// Callable (spec §4.6).
const FuncDecl *Checker::rest_function(const Expr &p_value) {
	const bool method = p_value.target == Target::MethodValue || p_value.target == Target::StaticValue || p_value.target == Target::StructMethodValue || p_value.target == Target::NewValue;
	const FuncDecl *declared = p_value.kind == ExprKind::Lambda ? p_value.function.get() : (method ? p_value.callee : nullptr);
	return declared && declared->has_rest() ? declared : nullptr;
}

// A function connected to a signal takes what the signal passes (spec §4.7).
void Checker::check_handler(const Expr &p_handler, const Type &p_signal) {
	const Type &handler = p_handler.type;
	if (handler.is_error()) {
		return;
	}
	// A Callable, checked by its declaration.
	if (const FuncDecl *declared = rest_function(p_handler)) {
		if (!takes(*declared, p_signal.args.size())) {
			error(p_handler.range, p_signal.to_string() + " passes " + std::to_string(p_signal.args.size()) + " argument(s), but " + signature_text(*declared) + " takes " + argument_count(*declared) + ".");
			return;
		}
		for (size_t i = 0; i < p_signal.args.size(); i++) {
			if (!assignable(p_signal.args[i], *argument_type(*declared, i))) {
				error(p_handler.range, p_signal.to_string() + " passes " + p_signal.args[i].to_string() + " as argument " + std::to_string(i + 1) + ", which " + signature_text(*declared) + " does not take.");
				return;
			}
		}
		return;
	}
	if (handler.kind != TypeKind::Function) {
		error(p_handler.range, "A signal is connected to a function value, such as a method name, whose parameters it can check; found " + handler.to_string() + ".");
		return;
	}
	if (handler.args.size() - 1 != p_signal.args.size()) {
		error(p_handler.range, p_signal.to_string() + " passes " + std::to_string(p_signal.args.size()) + " argument(s), but " + handler.to_string() + " takes " + std::to_string(handler.args.size() - 1) + ".");
		return;
	}
	for (size_t i = 0; i < p_signal.args.size(); i++) {
		if (!assignable(p_signal.args[i], handler.args[i + 1])) {
			error(p_handler.range, p_signal.to_string() + " passes " + p_signal.args[i].to_string() + " as argument " + std::to_string(i + 1) + ", which " + handler.to_string() + " does not take.");
			return;
		}
	}
}

/* Option and Result (spec §3.4, §3.5) */

// A signature of LANGUAGE_FUNCTIONS, with T and E, and T? and E?, spelled as
// the types the receiver holds; E stays without `p_e`.
static std::string spelled(const char *p_text, const Type &p_t, const Type *p_e) {
	std::string text;
	for (const char *c = p_text; *c; c++) {
		const Type *named = *c == 'T' ? &p_t : (*c == 'E' ? p_e : nullptr);
		if (!named || (c > p_text && std::isalnum(static_cast<unsigned char>(c[-1]))) || std::isalnum(static_cast<unsigned char>(c[1]))) {
			text += *c;
			continue;
		}
		const bool optional = c[1] == '?';
		text += optional ? optional_of(*named).to_string() : named->to_string();
		c += optional;
	}
	return text;
}

// A method of T? or Result[T, E] (spec §3.4, §3.5), as Rust's, typed by what
// it gives on each side. One that takes a function calls it only on the side
// it is for. ok_or() takes the error type from the Result expected, if any.
Type Checker::check_language_method(Expr &r_expr, const Type &p_base, const Type &p_expected) {
	const std::string &name = r_expr.text;
	const bool of_result = p_base.is_result();
	const LanguageFunction *method = language_function(of_result ? LanguageFunction::Of::Result : LanguageFunction::Of::Option, name);
	if (!method) {
		return fail_call(r_expr, of_result ? p_base.to_string() + " has no method \"" + name + "()\"." : "The value may be None (" + p_base.to_string() + "); check it before calling \"" + name + "()\".");
	}
	refer(r_expr.name_token, language_id(of_result ? "Result" : "Option", name));
	r_expr.target = Target::LanguageMethod;
	r_expr.language = method;
	// What each side holds: T, then E or nothing.
	const Type held[2] = { of_result ? p_base.args[0] : p_base.unwrapped(), of_result ? p_base.args[1] : Type::of(TypeKind::Void) };
	const Type &value = held[0];
	const std::string takes = spelled(method->takes, value, of_result ? &held[1] : nullptr);
	hint(r_expr, [&](int p_current) { return signature(name, *method->takes ? std::vector<std::string>{ takes } : std::vector<std::string>(), false, p_current, spelled(method->returns, value, of_result ? &held[1] : nullptr)); });
	// The side that calls the function passed, if one does.
	const int side = method->sides[0] == Gives::Called || method->sides[0] == Gives::WrappedCalled || method->sides[0] == Gives::Filtered ? 0 : 1;
	const Gives gives = method->sides[side];
	const Gives other = method->sides[1 - side];
	if (method->sides[1] == Gives::WrappedArgument) {
		// ok_or()
		const Type wanted = p_expected.is_result() ? p_expected.args[1] : Type::of(TypeKind::Error);
		if (r_expr.args.size() != 1) {
			return fail_call(r_expr, "Function \"ok_or()\" takes 1 argument(s), " + std::to_string(r_expr.args.size()) + " given.");
		}
		Expr &arg = *r_expr.args[0];
		Type failure = check_value(arg, wanted);
		if (!wanted.is_error()) {
			expect_assignable(arg, failure, wanted, "pass");
			failure = wanted;
		}
		if (failure.kind == TypeKind::None) {
			return fail(r_expr, "ok_or(None) needs the Result type it makes, from where it goes.");
		}
		r_expr.param_types.push_back(failure);
		return set(r_expr, failure.is_error() ? failure : Type::result(value, failure));
	}
	if (gives != Gives::Called && gives != Gives::WrappedCalled && gives != Gives::Filtered) {
		// What a side holds, as a T? for ok() and err(), or the receiver;
		// unwrap_or() and or() take one more of it, expect() a message.
		const bool to_option = of_result && (method->sides[0] == Gives::Default || method->sides[1] == Gives::Default);
		const Type result = method->sides[0] == Gives::Self ? p_base : (to_option ? optional_of(held[method->sides[0] == Gives::Default]) : value);
		const std::vector<Type> params = method->sides[1] == Gives::Argument ? std::vector<Type>{ result } : (*method->takes ? std::vector<Type>{ Type::of(TypeKind::String) } : std::vector<Type>());
		check_arguments(r_expr, params, int(params.size()), false);
		if (!of_result && method->sides[1] == Gives::Default && !value.is_optional()) {
			return fail_call(r_expr, p_base.to_string() + " has no method \"flatten()\", which makes a T?? into a T?.");
		}
		return set(r_expr, result);
	}
	// The function, called on the side it is for, of what the side holds, or
	// of nothing on None. What it returns when the method says: the type the
	// other side gives, some U? as and_then() of T? returns, or a Result that
	// keeps the other side's type; which its returns say.
	const std::vector<Type> params = of_result || side == 0 ? std::vector<Type>{ held[side] } : std::vector<Type>();
	Type wanted = Type::of(TypeKind::Error);
	if (gives == Gives::Filtered) {
		wanted = Type::of(TypeKind::Bool);
	} else if (gives == Gives::Called) {
		const Type open = Type::of(TypeKind::Error);
		wanted = other == Gives::Inner ? value : (other == Gives::Self ? p_base : (other == Gives::Default ? Type::option(open) : (side == 0 ? Type::result(open, held[1]) : Type::result(value, open))));
	}
	const std::optional<Type> returned = check_function_argument(r_expr, params, wanted);
	if (!returned || returned->is_error()) {
		return set(r_expr, Type::of(TypeKind::Error));
	}
	const size_t kept = wanted.is_result() && wanted.args[0].is_error() ? 1 : 0;
	const bool fits = wanted.is_error() ? returned->kind != TypeKind::Void : (!partial(wanted) ? assignable(*returned, wanted) : (wanted.is_result() ? returned->is_result() && returned->args[kept] == wanted.args[kept] : returned->is_optional()));
	if (!fits) {
		error(r_expr.args[0]->range, "The function passed to " + name + "() returns " + returned->to_string() + ", not " + takes.substr(takes.find("-> ") + 3) + ".");
		return set(r_expr, Type::of(TypeKind::Error));
	}
	if (gives == Gives::Filtered) {
		return set(r_expr, p_base);
	}
	if (gives == Gives::WrappedCalled) {
		return set(r_expr, !of_result && other == Gives::Default ? optional_of(*returned) : (side == 0 ? Type::result(*returned, held[1]) : Type::result(value, *returned)));
	}
	return set(r_expr, partial(wanted) ? *returned : wanted);
}

// `Some(v)` (spec §3.4): of the T? it goes to, which says what v is, or of
// v's type.
Type Checker::check_some(Expr &r_expr, const Type &p_expected) {
	const Type inner = p_expected.is_optional() ? p_expected.unwrapped() : Type::of(TypeKind::Error);
	hint(r_expr, [&](int p_current) { return signature("Some", { "value: " + (inner.is_error() ? std::string("T") : inner.to_string()) }, false, p_current, inner.is_error() ? std::string("T?") : p_expected.to_string()); });
	if (!r_expr.arg_names.empty() || r_expr.args.size() != 1) {
		for (auto &arg : r_expr.args) {
			check_value(*arg);
		}
		return fail_call(r_expr, "Some(...) takes one value by position, as in Some(x).");
	}
	refer(r_expr.name_token, language_id("Option", "Some"));
	Expr &arg = *r_expr.args[0];
	const Type given = check_value(arg, inner);
	if (given.is_error()) {
		return set(r_expr, given);
	}
	if (!inner.is_error()) {
		expect_assignable(arg, given, inner, "pass");
	} else if (given.kind == TypeKind::None || given.kind == TypeKind::Void) {
		return fail_call(r_expr, std::string("Some(") + (given.kind == TypeKind::None ? "None" : "...") + ") does not say which type it holds; declare the type it goes to.");
	}
	const Type value = inner.is_error() ? given : inner;
	r_expr.target = Target::Some;
	r_expr.param_types = { value };
	return set(r_expr, optional_of(value));
}

// `Ok(v)` and `Err(e)` (spec §3.5): of the Result they go to, which says
// what v or e is.
Type Checker::check_result_construction(Expr &r_expr, const Type &p_expected) {
	const Type result = p_expected.kind == TypeKind::Option ? p_expected.args[0] : p_expected;
	if (!result.is_result() || !r_expr.arg_names.empty()) {
		if (!r_expr.arg_names.empty()) {
			return fail_call(r_expr, r_expr.text + "(...) takes its value by position, as in " + r_expr.text + "(x).");
		}
		if (!result.is_error()) {
			return fail_call(r_expr, r_expr.text + "(...) makes a Result, but a value of type " + result.to_string() + " goes here.");
		}
		return fail_call(r_expr, r_expr.text + "(...) takes its type from where it goes, as in `var r: Result[int, String] = " + r_expr.text + "(...)`; nothing here gives one.");
	}
	const int variant = r_expr.text == "Ok" ? 0 : 1;
	refer(r_expr.name_token, language_id("Result", r_expr.text));
	r_expr.target = Target::VariantConstructor;
	r_expr.index = variant;
	// Returned by a lambda going to and_then() or or_else(), whose Result
	// leaves this side to what the lambda returns (spec §4.6).
	if (result.args[variant].is_error() && r_expr.args.size() == 1) {
		const Type given = check_value(*r_expr.args[0]);
		if (given.kind == TypeKind::None || given.kind == TypeKind::Void) {
			return fail_call(r_expr, r_expr.text + "(" + (given.kind == TypeKind::None ? "None" : "...") + ") does not say which type it holds; declare the lambda's return type.");
		}
		r_expr.param_types.push_back(given);
		Type made = result;
		made.args[variant] = given;
		return given.is_error() ? set(r_expr, given) : set(r_expr, Type::result(made.args[0], made.args[1]));
	}
	hint(r_expr, [&](int p_current) { return signature(r_expr.text, { (variant == 0 ? "value: " : "error: ") + result.args[variant].to_string() }, false, p_current, result.to_string()); });
	check_arguments(r_expr, { result.args[variant] }, 1, false);
	return set(r_expr, result);
}

// The one argument of map() and the like: a function whose parameters take
// `p_params`, none or one, which a lambda's parameters then have unless
// written, as its returned values take `p_returns` when known; what it
// returns, or nullopt after reporting.
std::optional<Type> Checker::check_function_argument(Expr &r_call, const std::vector<Type> &p_params, const Type &p_returns) {
	if (r_call.args.size() != 1) {
		for (auto &arg : r_call.args) {
			check_value(*arg);
		}
		error(r_call.range, "Function \"" + r_call.text + "()\" takes 1 argument(s), " + std::to_string(r_call.args.size()) + " given.");
		return std::nullopt;
	}
	Expr &arg = *r_call.args[0];
	const Type function = check_value(arg, Type::function(p_returns, p_params));
	if (function.is_error()) {
		return std::nullopt;
	}
	if (function.kind != TypeKind::Function || function.is_async || function.args.size() != p_params.size() + 1 || (!p_params.empty() && !assignable(p_params[0], function.args[1]))) {
		const std::string wanted = p_params.empty() ? "without parameters" : "of one parameter, which takes " + p_params[0].to_string();
		error(arg.range, "\"" + r_call.text + "()\" takes a function " + wanted + "; found " + function.to_string() + ".");
		return std::nullopt;
	}
	r_call.param_types.push_back(function);
	return function.args[0];
}

/* Conditional expressions, subscripts, literals, casts and super */

// The type both branches fit: one of them, or for objects the nearest
// class both extend; Error if there is none.
Type Checker::common_type(const Type &p_a, const Type &p_b) const {
	if (assignable(p_a, p_b)) {
		return p_b;
	}
	if (assignable(p_b, p_a)) {
		return p_a;
	}
	if (p_a.kind == TypeKind::Object && p_b.kind == TypeKind::Object && p_a.args.empty() && p_b.args.empty() && !p_a.is_interface && !p_b.is_interface) {
		for (std::string path = p_a.script; is_script_path(path);) {
			const ClassDecl *header = script_header(path);
			if (!header) {
				break;
			}
			if (extends_script(p_b.script, path)) {
				Type result = script_class_type(*header, path);
				result.nullable = p_a.nullable || p_b.nullable;
				return result;
			}
			path = classes.base_path(*header, path);
		}
		for (std::string cls = p_a.engine_class(); !cls.empty(); cls = engine.parent_class(cls)) {
			if (engine.inherits(p_b.engine_class(), cls)) {
				return Type::object(cls, p_a.nullable || p_b.nullable);
			}
		}
	}
	return Type::of(TypeKind::Error);
}

// `a if c else b` (spec §5.5).
Type Checker::check_conditional(Expr &r_expr, const Type &p_expected) {
	expect_condition(*r_expr.base);
	const Facts facts = facts_of(*r_expr.base);
	narrowed.push_back(facts.when_true);
	Type a = check_value(*r_expr.left, p_expected);
	narrowed.back() = facts.when_false;
	Type b = check_value(*r_expr.right, p_expected);
	narrowed.pop_back();
	if (a.is_error() || b.is_error()) {
		return set(r_expr, Type::of(TypeKind::Error));
	}
	// As Ok(x) if c else Err(e), returned by a lambda going to and_then().
	if (a.is_result() && partial(a) && !partial(b)) {
		a = set(*r_expr.left, completed(a, b));
	} else if (b.is_result() && partial(b) && !partial(a)) {
		b = set(*r_expr.right, completed(b, a));
	}
	if (!p_expected.is_error() && !partial(p_expected) && assignable(a, p_expected) && assignable(b, p_expected)) {
		expect_implemented(*r_expr.left, a, p_expected);
		expect_implemented(*r_expr.right, b, p_expected);
		return set(r_expr, p_expected);
	}
	Type result = common_type(a, b);
	if (result.is_error() && (a.kind == TypeKind::None) != (b.kind == TypeKind::None)) {
		result = optional_of(a.kind == TypeKind::None ? b : a);
	}
	if (result.is_error()) {
		return fail(r_expr, "The branches have types " + a.to_string() + " and " + b.to_string() + ", which have no common type; declare the type.");
	}
	return set(r_expr, result);
}

// `a[i]`, or `d[k]` (spec §3.8); `p_written` when it is only assigned to.
Type Checker::check_subscript(Expr &r_expr) {
	const Type base = check_value(*r_expr.base);
	const std::optional<std::pair<Type, Type>> entries = base.is_error() ? std::nullopt : entries_of(base);
	const Type index = check_value(*r_expr.right, entries ? entries->first : Type::of(TypeKind::Error));
	if (base.is_error() || index.is_error()) {
		return set(r_expr, Type::of(TypeKind::Error));
	}
	if (base.is_optional()) {
		return fail(r_expr, "The value may be None (" + base.to_string() + "); check it before indexing it.");
	}
	if (base.kind == TypeKind::Variant) {
		return fail(r_expr, "Cannot index a Variant." + variant_hint());
	}
	if (entries) {
		if (!assignable(index, entries->first)) {
			return fail(*r_expr.right, "The keys of " + base.to_string() + " are " + entries->first.to_string() + ", not " + index.to_string() + ".");
		}
		r_expr.param_types = { entries->first };
		return set(r_expr, entries->second);
	}
	// A String's elements are its characters, each a String.
	const std::optional<Type> element = base.kind == TypeKind::String ? Type::of(TypeKind::String) : element_of(base);
	if (!element) {
		return fail(r_expr, "Values of type " + base.to_string() + " cannot be indexed.");
	}
	if (index.kind != TypeKind::Int) {
		return fail(*r_expr.right, "An index must be int, found " + index.to_string() + ".");
	}
	return set(r_expr, *element);
}

// `[a, b]` takes its type from where it goes (spec §4.3).
Type Checker::check_array_literal(Expr &r_expr, const Type &p_expected) {
	// Into a Variant, as in an untyped Array, it is an untyped Array.
	const Type wanted = p_expected.kind == TypeKind::Option ? p_expected.args[0] : p_expected;
	const Type expected = wanted.kind == TypeKind::Variant ? Type::builtin("Array") : wanted;
	if (expected.kind != TypeKind::Builtin || expected.name != "Array") {
		if (r_expr.args.empty()) {
			return fail(r_expr, "An empty array needs a declared type, for example var a: Array[int] = [].");
		}
		// Otherwise its elements' common type gives it (spec §4.3).
		const std::optional<Type> element = common_element(r_expr.args, 0, 1, "elements");
		if (!element) {
			return set(r_expr, Type::of(TypeKind::Error));
		}
		r_expr.param_types.assign(r_expr.args.size(), *element);
		return set(r_expr, Type::array(*element));
	}
	const Type element = expected.args.empty() ? Type::of(TypeKind::Variant) : expected.args[0];
	for (auto &arg : r_expr.args) {
		const Type type = check_value(*arg, element);
		r_expr.param_types.push_back(element);
		if (!assignable(type, element)) {
			error(arg->range, "Cannot put a value of type " + type.to_string() + " in " + expected.to_string() + ".");
		} else {
			expect_implemented(*arg, type, element);
			expect_scene_fits(*arg, element);
		}
	}
	return set(r_expr, expected);
}

// The common type of the values at `p_first`, `p_first + p_step`… of a
// literal, which have no type to go to; nullopt after reporting.
std::optional<Type> Checker::common_element(std::vector<std::unique_ptr<Expr>> &r_values, size_t p_first, size_t p_step, const std::string &p_what) {
	std::optional<Type> common;
	bool ok = true;
	for (size_t i = p_first; i < r_values.size(); i += p_step) {
		const Type type = check_value(*r_values[i]);
		if (type.is_error() || !ok) {
			ok = false;
			continue;
		}
		if (type.kind == TypeKind::None) {
			error(r_values[i]->range, "None does not say which T? the " + p_what + " are; declare the type.");
			ok = false;
			continue;
		}
		const Type next = common ? common_type(*common, type) : type;
		if (next.is_error()) {
			error(r_values[i]->range, "The " + p_what + " have types " + common->to_string() + " and " + type.to_string() + ", which have no common type; declare the type.");
			ok = false;
			continue;
		}
		common = next;
	}
	if (!ok) {
		return std::nullopt;
	}
	for (size_t i = p_first; i < r_values.size(); i += p_step) {
		expect_scene_fits(*r_values[i], *common);
	}
	return common;
}

// `{k: v}` takes its type from where it goes, as `[a, b]` does, or from
// its keys and values. A key written as a name is a StringName, as in
// GDScript (modules/gdscript/gdscript_parser.cpp:3330-3336).
Type Checker::check_dictionary_literal(Expr &r_expr, const Type &p_expected) {
	const Type wanted = p_expected.kind == TypeKind::Option ? p_expected.args[0] : p_expected;
	const Type expected = wanted.kind == TypeKind::Variant ? Type::builtin("Dictionary") : wanted;
	const std::optional<std::pair<Type, Type>> entries = entries_of(expected);
	if (!entries && r_expr.args.empty()) {
		return fail_call(r_expr, "An empty dictionary needs a declared type, for example var d: Dictionary[String, int] = {}.");
	}
	if (!entries) {
		const std::optional<Type> key = r_expr.bool_value ? std::optional<Type>(Type::builtin("StringName")) : common_element(r_expr.args, 0, 2, "keys");
		if (r_expr.bool_value) {
			for (size_t i = 0; i < r_expr.args.size(); i += 2) {
				check_value(*r_expr.args[i], *key);
			}
		}
		const std::optional<Type> value = common_element(r_expr.args, 1, 2, "values");
		if (!key || !value) {
			return set(r_expr, Type::of(TypeKind::Error));
		}
		for (size_t i = 0; i + 1 < r_expr.args.size(); i += 2) {
			r_expr.param_types.push_back(*key);
			r_expr.param_types.push_back(*value);
		}
		return set(r_expr, Type::dictionary(*key, *value));
	}
	const Type name_key = entries->first.kind == TypeKind::Variant ? Type::builtin("StringName") : entries->first;
	for (size_t i = 0; i + 1 < r_expr.args.size(); i += 2) {
		const Type &want_key = r_expr.bool_value ? name_key : entries->first;
		const Type key = check_value(*r_expr.args[i], want_key);
		const Type value = check_value(*r_expr.args[i + 1], entries->second);
		r_expr.param_types.push_back(want_key);
		r_expr.param_types.push_back(entries->second);
		if (!assignable(key, want_key)) {
			error(r_expr.args[i]->range, "The keys of " + expected.to_string() + " are " + entries->first.to_string() + ", not " + key.to_string() + ".");
		} else {
			expect_implemented(*r_expr.args[i], key, want_key);
			expect_scene_fits(*r_expr.args[i], want_key);
		}
		if (!assignable(value, entries->second)) {
			error(r_expr.args[i + 1]->range, "Cannot put a value of type " + value.to_string() + " in " + expected.to_string() + ".");
		} else {
			expect_implemented(*r_expr.args[i + 1], value, entries->second);
			expect_scene_fits(*r_expr.args[i + 1], entries->second);
		}
	}
	return set(r_expr, expected);
}

// `x as T` (spec §5.3).
Type Checker::check_cast(Expr &r_expr) {
	const Type from = check_value(*r_expr.left);
	const Type to = resolve(r_expr.cast_type, false);
	if (from.is_error() || to.is_error()) {
		return set(r_expr, Type::of(TypeKind::Error));
	}
	if (to.kind != TypeKind::Object) {
		const std::string name = to.unwrapped().to_string();
		return fail(r_expr, "as only converts objects; convert to " + name + " with " + name + "(x) or " + name + ".try_from(x).");
	}
	if (from.kind == TypeKind::Variant) {
		// None also when it holds no object (spec §5.3).
		r_expr.tested = to;
		Type result = to;
		result.nullable = true;
		return set(r_expr, result);
	}
	if (from.kind != TypeKind::Object) {
		return fail(r_expr, "as only converts objects, not " + from.to_string() + ".");
	}
	if (!related(from, to)) {
		return fail(r_expr, from.name + " and " + to.name + " are unrelated classes; as would always give None.");
	}
	r_expr.tested = to;
	// Already a T: nothing to convert.
	Type result = to;
	result.nullable = from.nullable || !is_a(from, to);
	return set(r_expr, result);
}

Type Checker::check_super(Expr &r_expr) {
	if (!function) {
		return fail_call(r_expr, "super is only valid inside a function.");
	}
	if (!r_expr.base) {
		// `super()` names no function.
		r_expr.text = function->name;
		r_expr.name_token = NO_TOKEN;
	}
	const FuncDecl *callee = base_function(r_expr.text);
	if (callee && callee->is_abstract) {
		return fail_call(r_expr, "\"" + callee->name + "()\" is abstract in the base class, which has no body to call.");
	}
	if (callee) {
		const ClassDecl &owner = declaring(*cls.base, *callee, &ClassDecl::functions);
		refer(r_expr.name_token, script_id(owner.path, callee->name));
		if (!callee->is_static && refuse_in_static(r_expr, "super." + r_expr.text)) {
			return fail_call(r_expr);
		}
		const Type result = call_declared(r_expr, *callee, *owner.source, callee->return_type);
		r_expr.target = Target::SuperMethod;
		return result;
	}
	if (r_expr.text == "_init") {
		if (refuse_in_static(r_expr, "super._init")) {
			return fail_call(r_expr);
		}
		check_arguments(r_expr, {}, 0, false);
		r_expr.target = Target::Noop;
		refer(r_expr.name_token, engine_id("Object", "_init"));
		return set(r_expr, Type::of(TypeKind::Void));
	}
	const auto method = engine.find_method(cls.native_base, r_expr.text);
	if (!method || method->is_virtual) {
		return fail_call(r_expr, "No base implementation of \"" + r_expr.text + "()\".");
	}
	if (!method->is_static && refuse_in_static(r_expr, r_expr.text)) {
		return fail_call(r_expr);
	}
	r_expr.base.reset();
	return check_engine_call(r_expr, *method);
}

/* await, tasks and ? (spec §8.2, §9) */

// Why await cannot be written here; empty where it can.
std::string Checker::no_await() const {
	if (in_assert) {
		return "await cannot appear in an assert: it would vanish with the assertion in release builds.";
	}
	if (in_predelete) {
		return "await is not allowed in the PREDELETE branch.";
	}
	if (!function || !function->is_async || initializing >= 0) {
		return "await requires an async function and cannot appear in an initializer.";
	}
	return std::string();
}

// `Task.of(signal)`, `Task.delay(seconds)`, `Task.any([...])` and
// `Task.all([...])` (spec §9.5): tasks the runtime makes, which an await
// owns when it starts them, as a call. A signal gives what an await of it
// gives as a value (§9.6).
Type Checker::check_task_function(Expr &r_expr) {
	refer(r_expr.base->name_token, language_id("Task", std::string()));
	const std::string &name = r_expr.text;
	if (name != "of" && name != "delay" && name != "any" && name != "all") {
		return fail_call(r_expr, "Task has no function \"" + name + "()\".");
	}
	refer(r_expr.name_token, language_id("Task", name));
	r_expr.is_async = true;
	if (name == "any" || name == "all") {
		return check_task_join(r_expr);
	}
	if (name == "delay") {
		check_arguments(r_expr, { Type::of(TypeKind::Float) }, 1, false);
		r_expr.target = Target::TaskDelay;
		return set(r_expr, Type::task(Type::of(TypeKind::Void)));
	}
	if (r_expr.args.size() != 1) {
		return fail_call(r_expr, "Task.of() takes one signal.");
	}
	const Type signal = check_value(*r_expr.args[0]);
	std::optional<Type> result;
	if (signal.kind == TypeKind::Signal) {
		result = signal.args.size() == 1 ? signal.args[0] : Type::of(TypeKind::Void);
	} else if (signal.builtin_name() == "Signal") {
		result = Type::of(TypeKind::Variant);
	} else if (!signal.is_error()) {
		error(r_expr.args[0]->range, "Task.of() takes a signal, not " + signal.to_string() + ".");
	}
	if (!result) {
		return set(r_expr, Type::of(TypeKind::Error));
	}
	r_expr.param_types.push_back(signal);
	r_expr.target = Target::TaskOf;
	return set(r_expr, Type::task(*result));
}

// The tasks of Task.any() or Task.all(), listed in brackets, where a task
// started is the join's own, or in an Array[Task[T]] (spec §9.5). any gives
// the index of the first to finish; all gives nothing, as tuples could give
// the results (BACKLOG).
Type Checker::check_task_join(Expr &r_expr) {
	const bool any = r_expr.text == "any";
	if (r_expr.args.size() != 1) {
		return fail_call(r_expr, "Task." + r_expr.text + "() takes one list of tasks, as Task." + r_expr.text + "([a, f()]).");
	}
	Expr &list = *r_expr.args[0];
	if (list.kind == ExprKind::ArrayLiteral) {
		if (any && list.args.empty()) {
			error(list.range, "Task.any() needs a task to wait for.");
		}
		for (auto &member : list.args) {
			const Type type = check_value(*member);
			if (!type.is_error() && (type.kind != TypeKind::Task || type.nullable)) {
				error(member->range, "Task." + r_expr.text + "() takes tasks, not " + type.to_string() + ".");
			}
		}
		set(list, Type::builtin("Array"));
	} else {
		const Type tasks = check_value(list);
		if (!tasks.is_error() && (!is_typed_container(tasks) || tasks.name != "Array" || tasks.args[0].kind != TypeKind::Task || tasks.args[0].nullable)) {
			error(list.range, "Task." + r_expr.text + "() takes tasks in brackets or an Array[Task[T]], not " + tasks.to_string() + ".");
		}
	}
	r_expr.target = any ? Target::TaskAny : Target::TaskAll;
	return set(r_expr, Type::task(Type::of(any ? TypeKind::Int : TypeKind::Void)));
}

Type Checker::check_await(Expr &r_expr) {
	const std::string refused = no_await();
	if (!refused.empty()) {
		return fail(r_expr, refused);
	}
	Expr &operand = *r_expr.left;
	operand.awaited = true;
	const Type type = check_expr(operand);
	if (type.is_error()) {
		return set(r_expr, type);
	}
	if (type.kind == TypeKind::Task) {
		if (type.nullable) {
			return fail(r_expr, "Unwrap the optional Task before awaiting it.");
		}
		return set(r_expr, type.args[0]);
	}
	if (type.kind == TypeKind::Signal) {
		return set(r_expr, type.args.size() == 1 ? type.args[0] : Type::of(TypeKind::Void));
	}
	if (type.builtin_name() == "Signal") {
		return set(r_expr, Type::of(TypeKind::Variant));
	}
	if (operand.kind == ExprKind::Call && (operand.is_foreign || (operand.base && operand.base->type == Type::builtin("Callable") && (operand.text == "call" || operand.text == "callv")))) {
		return set(r_expr, type);
	}
	return fail(r_expr, "await expects a Task, a signal or a call to GDScript.");
}

// A partial type that a lambda returns, completed by another it returns.
Type Checker::completed(const Type &p_partial, const Type &p_other) {
	if (p_partial.is_result()) {
		return p_other.is_result() ? Type::result(p_partial.args[0].is_error() ? p_other.args[0] : p_partial.args[0], p_partial.args[1].is_error() ? p_other.args[1] : p_partial.args[1]) : p_partial;
	}
	if (p_other.kind == TypeKind::None || (p_other.kind == TypeKind::Option && p_other.args[0].is_error())) {
		return p_partial;
	}
	return p_other.is_optional() ? p_other : optional_of(p_other);
}

// A type that a lambda without a return type returns, whose return type is
// the type all of them fit (spec §4.6).
void Checker::add_returned(Type p_value, const TokenRange &p_at) {
	if (!returns->type || p_value.is_error() || returns->type->is_error()) {
		returns->type = returns->type ? Type::of(TypeKind::Error) : p_value;
		return;
	}
	Type known = *returns->type;
	if (partial(known)) {
		known = completed(known, p_value);
	}
	if (partial(p_value)) {
		p_value = completed(p_value, known);
	}
	const Type common = common_type(known, p_value);
	if (common.is_error()) {
		error(p_at, function_label() + " returns " + returned_text(*returns->type) + " and " + returned_text(p_value) + "; declare its return type.");
	}
	returns->type = common;
}

// A type a lambda returns, as a diagnostic shows it, even if partial.
std::string Checker::returned_text(const Type &p_type) {
	if (p_type.kind == TypeKind::None || p_type.kind == TypeKind::Option) {
		return p_type.kind == TypeKind::None || p_type.args[0].is_error() ? std::string("None") : p_type.to_string();
	}
	return p_type.is_result() && partial(p_type) ? (p_type.args[0].is_error() ? "Err(" + p_type.args[1].to_string() + ")" : "Ok(" + p_type.args[0].to_string() + ")") : p_type.to_string();
}

// `x?` (spec §8.2): the value inside x, or the function returns the Err or
// the None x holds. So it returns a Result with the same error type, or a
// T?; between the two, ok() and ok_or() convert.
Type Checker::check_try(Expr &r_expr) {
	const Type type = check_value(*r_expr.left);
	if (type.is_error()) {
		return set(r_expr, type);
	}
	const bool result = type.is_result();
	if (!result && !type.is_optional()) {
		return fail(r_expr, "? takes apart a Result or a T?, not " + type.to_string() + ".");
	}
	const std::string found = result ? "the Err(...) of " + type.to_string() : "the None of " + type.to_string();
	if (in_assert) {
		return fail(r_expr, "? cannot appear in an assert: it would vanish with the assertion in release builds.");
	}
	if (!function || initializing >= 0 || static_initializing >= 0) {
		return fail(r_expr, "? returns " + found + " from the function, and cannot appear in an initializer.");
	}
	if (returns && returns->expected.is_error()) {
		return fail(r_expr, "? returns " + found + " from the lambda, whose return type is then needed; write it, as in `func(x) -> " + (result ? "Result[int, " + type.args[1].to_string() + "]" : std::string("int?")) + ":`.");
	}
	// A lambda returns what the function type it goes to does.
	const Type to = returns ? returns->expected : function->return_type;
	if (to.is_error()) {
		return set(r_expr, Type::of(TypeKind::Error));
	}
	if (result && to.is_result() && to.args[1] != type.args[1]) {
		return fail(r_expr, "? returns " + found + ", but " + function_label() + " returns " + to.to_string() + "; convert the error with map_err().");
	}
	if (result ? !to.is_result() : !to.is_optional()) {
		const std::string convert = to.is_optional() ? "; convert it with ok()" : (to.is_result() ? "; convert it with ok_or()" : "");
		return fail(r_expr, "? returns " + found + ", but " + function_label() + " returns " + to.to_string() + convert + ".");
	}
	if (returns) {
		add_returned(to, r_expr.range);
	}
	return set(r_expr, result ? type.args[0] : type.unwrapped());
}

/* Values, places and assignability (spec §3.14) */

// Types an expression and computes its value when it is a constant
// expression (spec §4.5).
Type Checker::check_expr(Expr &r_expr, const Type &p_expected) {
	const Type type = type_expr(r_expr, p_expected);
	if (type.is_error()) {
		return type;
	}
	if (!r_expr.constant) {
		r_expr.constant = fold(r_expr);
	}
	return type;
}

void Checker::report_constant_error(const Expr &p_expr) {
	if (p_expr.constant_error_at) {
		error(p_expr.constant_error_at->range, p_expr.constant_error);
	} else if (p_expr.kind == ExprKind::Binary && p_expr.op_token != NO_TOKEN) {
		error(p_expr.op_token, p_expr.constant_error);
	} else {
		error(p_expr.range, p_expr.constant_error);
	}
}

// The part of an expression that keeps it from being a constant expression.
const Expr &Checker::not_constant_part(const Expr &p_expr) {
	for (const Expr *child : { p_expr.base.get(), p_expr.left.get(), p_expr.right.get() }) {
		if (child && !child->constant && !child->type.is_error()) {
			return not_constant_part(*child);
		}
	}
	for (const auto &arg : p_expr.args) {
		if (!arg->constant && !arg->type.is_error()) {
			return not_constant_part(*arg);
		}
	}
	return p_expr;
}

// Reports why an expression whose value must be a constant is not one.
void Checker::require_constant(const Expr &p_expr) {
	const Expr &part = not_constant_part(p_expr);
	if (!part.constant_error.empty()) {
		report_constant_error(part);
		return;
	}
	std::string what = "This is not a constant expression";
	switch (part.kind) {
		case ExprKind::Call:
			what = "\"" + part.text + "()\" is not a constant expression: of calls, only constructors of built-in types and math functions are";
			break;
		case ExprKind::Name:
		case ExprKind::Member:
			if (part.target == Target::Local || part.target == Target::Member || part.target == Target::StaticVar || part.target == Target::SelfProperty || part.target == Target::ObjectProperty) {
				what = "\"" + part.text + "\" is a variable, not a constant";
			}
			break;
		case ExprKind::Self:
			what = "self is not a constant expression";
			break;
		case ExprKind::Cast:
		case ExprKind::TypeTest:
			what = "as and is are not supported in constant expressions";
			break;
		default:
			break;
	}
	error(part.range, what + ".");
}

// The string type a string literal is built as: the one it goes to, when
// it converts to that (spec §10.7), otherwise its own.
Type Checker::literal_type(const Expr &p_literal, const Type &p_to) const {
	const Type own = p_literal.op == TokenKind::StringName ? Type::builtin("StringName") : (p_literal.op == TokenKind::NodePath ? Type::builtin("NodePath") : Type::of(TypeKind::String));
	const Type to = p_to.kind == TypeKind::Option ? p_to.args[0] : p_to;
	const bool text = to.kind == TypeKind::String || to.builtin_name() == "StringName" || to.builtin_name() == "NodePath";
	return text && assignable(own, to) ? to : own;
}

// `p_expected` is the type the value goes to, if known; literals of arrays,
// dictionaries, strings and None take their type from it.
Type Checker::type_expr(Expr &r_expr, const Type &p_expected) {
	if (completion && (r_expr.kind == ExprKind::Name || r_expr.kind == ExprKind::Member || r_expr.kind == ExprKind::Call || r_expr.kind == ExprKind::GetNode) && at_cursor(r_expr.text)) {
		r_expr.kind == ExprKind::GetNode ? complete_node_path(r_expr) : complete(r_expr, p_expected);
		return set(r_expr, Type::of(TypeKind::Error));
	}
	switch (r_expr.kind) {
		case ExprKind::IntLiteral:
			if (r_expr.int_overflows) {
				return fail(r_expr, "Integer literal is too large for int.");
			}
			return set(r_expr, Type::of(TypeKind::Int));
		case ExprKind::FloatLiteral:
			return set(r_expr, Type::of(TypeKind::Float));
		case ExprKind::StringLiteral:
			return set(r_expr, literal_type(r_expr, p_expected));
		case ExprKind::BoolLiteral:
			return set(r_expr, Type::of(TypeKind::Bool));
		case ExprKind::Name:
			return check_name(r_expr);
		case ExprKind::Await:
			return check_await(r_expr);
		case ExprKind::Self:
			return check_self(r_expr);
		case ExprKind::Unary:
			return check_unary(r_expr);
		case ExprKind::Binary:
			return check_binary(r_expr);
		case ExprKind::Call:
			return check_call(r_expr, p_expected);
		case ExprKind::Member:
			return check_member_access(r_expr);
		case ExprKind::GetNode:
			return check_get_node(r_expr);
		case ExprKind::Cast:
			return check_cast(r_expr);
		case ExprKind::ArrayLiteral:
			return check_array_literal(r_expr, p_expected);
		case ExprKind::DictionaryLiteral:
			return check_dictionary_literal(r_expr, p_expected);
		case ExprKind::Subscript:
			return check_subscript(r_expr);
		case ExprKind::Conditional:
			return check_conditional(r_expr, p_expected);
		case ExprKind::NoneLiteral:
			// The T? it goes to, where that is known.
			if ((p_expected.is_optional() && !partial(p_expected)) || p_expected.kind == TypeKind::Variant) {
				return set(r_expr, p_expected);
			}
			return set(r_expr, Type::of(TypeKind::None));
		case ExprKind::TypeTest:
			return check_type_test(r_expr);
		case ExprKind::Lambda:
			return check_lambda(r_expr, p_expected);
		case ExprKind::Try:
			return check_try(r_expr);
		case ExprKind::Error:
			break;
	}
	return set(r_expr, Type::of(TypeKind::Error));
}

// Checks an expression whose value is stored, passed or returned.
Type Checker::check_value(Expr &r_expr, const Type &p_expected) {
	const Type type = check_expr(r_expr, p_expected);
	if (type.kind == TypeKind::Void) {
		error(r_expr.range, "This call returns void and has no value.");
		return Type::of(TypeKind::Error);
	}
	if (type.kind == TypeKind::Unsupported) {
		error(r_expr.range, "Values of type " + type.to_string() + " are not supported yet.");
		return Type::of(TypeKind::Error);
	}
	return type;
}

// What a Variant needs before it is used as a value of a type (spec §3.14).
std::string Checker::variant_hint(const Type &p_to) {
	if (p_to.kind == TypeKind::Object) {
		return " Convert it first with v as " + p_to.name + ", or narrow it with `if v is " + p_to.name + ":`.";
	}
	if (is_typed_container(p_to)) {
		return " Narrow it first with `if v is " + p_to.to_string() + ":`.";
	}
	const std::string name = p_to.kind == TypeKind::Builtin || p_to.is_primitive() ? p_to.to_string() : "T";
	return " Convert it first with " + name + "(v), or narrow it with `if v is " + name + ":`.";
}

// Whether the expression is a place that can be written: a local, a
// member, a property, or a member of a built-in value in such a place,
// which is then written back (architecture §2.1).
bool Checker::check_place(const Expr &p_target) {
	const std::string constant = constant_root(p_target);
	if (!constant.empty()) {
		const bool whole = p_target.kind != ExprKind::Subscript && p_target.target != Target::Field && p_target.target != Target::StructField;
		error(p_target.range, whole ? "Cannot assign to the constant \"" + constant + "\"." : "\"" + constant + "\" is a constant, whose containers are read-only.");
		return false;
	}
	if (p_target.kind == ExprKind::Subscript) {
		if (p_target.base->type.kind == TypeKind::String) {
			error(p_target.range, "A String's characters cannot be assigned. Remove one with s = s.erase(i), or replace it with s = s.erase(i).insert(i, c).");
			return false;
		}
		// An Array or a Dictionary changes in place; a packed array is a value,
		// written back along the chain.
		return is_shared(p_target.base->type) || check_written_back(p_target);
	}
	switch (p_target.target) {
		case Target::Local:
			if (p_target.kind == ExprKind::Self) {
				error(p_target.range, "self is read-only in a struct's methods; return a changed copy instead.");
				return false;
			}
			if (is_capture(p_target.index)) {
				// It would change neither the variable outside nor the next call (spec §3.11).
				error(p_target.range, "Cannot assign to \"" + p_target.text + "\", which the lambda captures as a copy; keep what it shares in a member or a container.");
				return false;
			}
			return true;
		case Target::Member:
		case Target::StaticVar:
		case Target::Owner:
			return true;
		case Target::SelfProperty:
		case Target::ObjectProperty: {
			if (p_target.is_static) {
				error(p_target.range, "Cannot assign to the constant \"" + p_target.text + "\".");
				return false;
			}
			const std::string &owner = p_target.target == Target::SelfProperty ? cls.native_base : p_target.owner;
			const auto property = engine.find_property(owner, p_target.text);
			if (property && !property->writable) {
				error(p_target.range, "The property \"" + p_target.text + "\" is read-only.");
				return false;
			}
			const InterfaceProperty *required = p_target.base && p_target.base->type.is_interface ? interface_property(p_target.base->type, p_target.text) : nullptr;
			if (required && required->is_readonly) {
				error(p_target.range, "The property \"" + p_target.text + "\" of " + p_target.base->type.to_string() + " is @readonly.");
				return false;
			}
			return true;
		}
		case Target::Field:
		case Target::StructField:
			return check_written_back(p_target);
		default:
			if (!p_target.type.is_error()) {
				error(p_target.range, "Cannot assign to this expression.");
			}
			return false;
	}
}

// The constant an expression is or is a part of, as `C`, `C[i]` and `C.x`
// are; empty for anything else.
std::string Checker::constant_root(const Expr &p_expr) const {
	const bool name = p_expr.kind == ExprKind::Name || p_expr.kind == ExprKind::Member;
	if (name && (p_expr.target == Target::Constant || (p_expr.target == Target::Local && function && function->locals[p_expr.index].constant))) {
		return p_expr.text;
	}
	const bool part = p_expr.kind == ExprKind::Subscript || (p_expr.kind == ExprKind::Member && (p_expr.target == Target::Field || p_expr.target == Target::StructField));
	return part && p_expr.base ? constant_root(*p_expr.base) : std::string();
}

// A part of a value, changed in a copy written back along the chain (spec §3.6).
bool Checker::check_written_back(const Expr &p_target) {
	const ExprKind base = p_target.base->kind;
	if (base == ExprKind::Name || base == ExprKind::Member || base == ExprKind::Subscript || base == ExprKind::Self) {
		return check_place(*p_target.base);
	}
	error(p_target.range, p_target.kind == ExprKind::Subscript ? "Cannot assign to an element of a temporary value." : "Cannot assign to a member of a temporary value.");
	return false;
}

// A scene statically known going to a PackedScene[T] that is not, whose
// instantiate(...) asks only for the members of T that must be assigned
// and are not exported (spec §7.4). The scene may leave no other member
// to it. A conditional not folded converts each branch.
void Checker::expect_scene_fits(const Expr &p_value, const Type &p_to) {
	if (!is_typed_scene(p_to)) {
		return;
	}
	if (p_value.kind == ExprKind::Conditional && !p_value.constant) {
		expect_scene_fits(*p_value.left, p_to);
		expect_scene_fits(*p_value.right, p_to);
		return;
	}
	const std::optional<Scene> scene = known_scene(p_value);
	const std::string unfit = scene ? unfit_scene(*scene, p_to) : std::string();
	if (!unfit.empty()) {
		error(p_value.range, scene->path + " " + unfit);
	}
}

// What a scene leaves to instantiate(...) that a PackedScene[T] does not
// ask for, as the error says it after the scene; "" for nothing. It does
// not ask for what the _init of T assigns.
std::string Checker::unfit_scene(const Scene &p_scene, const Type &p_to) {
	const SceneNode *root = p_scene.find_node(".");
	const std::string script = !root ? std::string() : root->script.empty() ? root->instance_script
																			: root->script;
	const ClassDecl *made = is_script_path(script) ? script_decl(script) : nullptr;
	const Type &taken = p_to.args[0];
	const ClassDecl *asks = taken.kind == TypeKind::Object && !taken.is_interface && !taken.script.empty() ? script_decl(taken.script) : nullptr;
	for (const VarDecl *member : made ? made->all_members : std::vector<const VarDecl *>()) {
		const bool asked = !member->is_export && asks && !init_assigns(*asks, member->name) && find_named(asks->all_members, member->name);
		if (member->must_assign && !init_assigns(*made, member->name) && !asked && (!member->is_export || !stored_value(p_scene, ".", member->name).value_or(false))) {
			return "leaves \"" + member->name + "\" to instantiate(), but a " + p_to.to_string() + " does not ask for it.";
		}
	}
	return std::string();
}

// Why the engine would lose a None of a value of the type (spec §10.1), as
// a sentence whose subject is `p_who`; empty when it would not. With
// `p_fix`, a T?? or Variant? itself is given the call that makes it fit.
// `p_what` names the value, as "its result", when it is not the subject's.
std::string Checker::encoding_loss(const Type &p_type, const std::string &p_who, bool p_fix, bool p_copied, const std::string &p_what) const {
	const std::optional<Type> lost = lost_in_encoding(p_type, *this, p_copied);
	if (!lost) {
		return std::string();
	}
	const bool variant = lost->args[0].kind == TypeKind::Variant;
	const bool whole = *lost == p_type;
	const std::string none = whole ? (p_what.empty() ? std::string("its None") : "the None of " + p_what) : "the None of the " + lost->to_string() + " inside" + (p_what.empty() ? std::string() : " " + p_what);
	std::string text = p_who + " cannot tell " + none + " from " + (variant ? "null" : "Some(None)");
	if (whole && p_fix) {
		text += variant ? "; pass x.unwrap_or(null)" : "; pass x.flatten()";
	}
	return text + ".";
}

// Why a value of the type does not go to a Variant or an untyped container
// as it is (spec §3.14), after the error that says so.
std::string Checker::variant_refusal(const Type &p_type) const {
	const std::string loss = encoding_loss(p_type, "A Variant", true);
	if (!loss.empty()) {
		return " " + loss;
	}
	if (is_typed_container(p_type) && is_boxed(p_type)) {
		return " Its elements exist only in this language; convert it explicitly with " + p_type.name + "(x), a copy with the elements encoded.";
	}
	return std::string();
}

// Why a function value does not go to a Callable or a Variant, whose
// calls encode as those of a pub function do (spec §10.1).
std::string Checker::callable_refusal(const Type &p_function) const {
	for (size_t i = 1; i <= p_function.args.size(); i++) {
		const size_t part = i % p_function.args.size(); // The result last.
		const std::string loss = encoding_loss(p_function.args[part], "The engine", false, false, part == 0 ? std::string("its result") : "its parameter " + std::to_string(part));
		if (!loss.empty()) {
			return " " + loss;
		}
	}
	return std::string();
}

void Checker::expect_assignable(const Expr &p_value, const Type &p_from, const Type &p_to, const char *p_action) {
	if (assignable(p_from, p_to)) {
		expect_implemented(p_value, p_from, p_to);
		expect_scene_fits(p_value, p_to);
		return;
	}
	std::string message = std::string("Cannot ") + p_action + " a value of type " + p_from.to_string() + " as " + p_to.to_string() + ".";
	if (p_from.kind == TypeKind::Float && p_to.kind == TypeKind::Int) {
		message += " Convert explicitly with int(x).";
	}
	if (p_to.unwrapped().kind == TypeKind::Enum && (p_from.kind == TypeKind::Int || p_from.kind == TypeKind::Enum)) {
		const std::string name = enum_spelling(p_to.unwrapped());
		message += " Convert explicitly with " + name + "(x), which checks that it is one of the values.";
	}
	const bool untyped_view = p_to.kind == TypeKind::Builtin && p_to.args.empty() && p_to.name == p_from.name;
	if (p_to.kind == TypeKind::Variant || untyped_view) {
		message += variant_refusal(p_from);
	}
	if (p_from.kind == TypeKind::Function && (p_to.kind == TypeKind::Variant || p_to == Type::builtin("Callable"))) {
		message += callable_refusal(p_from);
	}
	if (p_from.kind == TypeKind::Variant) {
		message += variant_hint(p_to.unwrapped());
	}
	if (is_typed_container(p_to) && p_from.kind == TypeKind::Builtin && p_from.args.empty() && p_from.name == p_to.name) {
		message += " Its elements may be of any type; narrow it with `if x is " + p_to.to_string() + ":`, or copy them in with assign().";
	}
	error(p_value.range, message);
}

void Checker::expect_condition(Expr &r_condition) {
	const Type type = check_value(r_condition);
	if (type.is_error() || type.kind == TypeKind::Bool) {
		return;
	}
	error(r_condition.range, "A condition must be bool, found " + type.to_string() + "." + compare_hint(type, false));
}

} // namespace petrichor::compiler::checking
