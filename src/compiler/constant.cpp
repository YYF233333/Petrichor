#include "compiler/constant.h"

#include "compiler/ast.h"
#include "compiler/builtin_api.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <locale>
#include <optional>
#include <sstream>
#include <unordered_set>

namespace petrichor::compiler {

uint8_t engine_operator(TokenKind p_op) {
	switch (p_op) {
		case TokenKind::EqualEqual:
			return OP_EQUAL;
		case TokenKind::BangEqual:
			return OP_NOT_EQUAL;
		case TokenKind::Less:
			return OP_LESS;
		case TokenKind::LessEqual:
			return OP_LESS_EQUAL;
		case TokenKind::Greater:
			return OP_GREATER;
		case TokenKind::GreaterEqual:
			return OP_GREATER_EQUAL;
		case TokenKind::Plus:
			return OP_ADD;
		case TokenKind::Minus:
			return OP_SUBTRACT;
		case TokenKind::Star:
			return OP_MULTIPLY;
		case TokenKind::Slash:
			return OP_DIVIDE;
		case TokenKind::Percent:
			return OP_MODULE;
		case TokenKind::ShiftLeft:
			return OP_SHIFT_LEFT;
		case TokenKind::ShiftRight:
			return OP_SHIFT_RIGHT;
		case TokenKind::Ampersand:
			return OP_BIT_AND;
		case TokenKind::Pipe:
			return OP_BIT_OR;
		case TokenKind::Caret:
			return OP_BIT_XOR;
		case TokenKind::And:
			return OP_AND;
		case TokenKind::Or:
			return OP_OR;
		default:
			return OP_UNKNOWN;
	}
}

namespace {

using Kind = ConstValue::Kind;

std::shared_ptr<ConstValue> make(Kind p_kind, const Type &p_type) {
	auto value = std::make_shared<ConstValue>();
	value->kind = p_kind;
	value->type = p_type;
	return value;
}

ConstPtr int_value(int64_t p_value) {
	auto value = make(Kind::Int, Type::of(TypeKind::Int));
	value->i = p_value;
	return value;
}

ConstPtr float_value(double p_value) {
	auto value = make(Kind::Float, Type::of(TypeKind::Float));
	value->f = p_value;
	return value;
}

ConstPtr bool_value(bool p_value) {
	auto value = make(Kind::Bool, Type::of(TypeKind::Bool));
	value->b = p_value;
	return value;
}

// As the virtual machine computes them: int arithmetic wraps.
int64_t wrapping(uint64_t p_value) {
	return int64_t(p_value);
}

double number(const ConstValue &p_value) {
	return p_value.kind == Kind::Int ? double(p_value.i) : p_value.f;
}

// float to int, as FLOAT_TO_INT checks it at run time.
bool fits_int(double p_value) {
	return p_value >= -9223372036854775808.0 && p_value < 9223372036854775808.0;
}

std::string number_text(double p_value) {
	std::ostringstream stream;
	stream.imbue(std::locale::classic());
	stream << std::setprecision(17) << p_value;
	return stream.str();
}

ConstPtr fail(Expr &r_expr, const std::string &p_message, const Expr *p_at = nullptr) {
	r_expr.constant_error = p_message;
	r_expr.constant_error_at = p_at;
	return nullptr;
}

// The constructor of `p_to` that takes one value of `p_from`, as built-in
// types convert (spec §3.2, §10.7); -1 if there is none.
int converting_constructor(const Type &p_from, const Type &p_to) {
	const builtin::Class *type = builtin::find_class(p_to.builtin_name());
	const std::string from = p_from.builtin_name();
	for (uint32_t i = 0; type && i < type->constructor_count; i++) {
		if (type->constructors[i].argument_count == 1 && from == type->constructors[i].arguments[0].type) {
			return int(i);
		}
	}
	return -1;
}

bool is_text(const Type &p_type) {
	return p_type.kind == TypeKind::String || p_type.builtin_name() == "StringName" || p_type.builtin_name() == "NodePath";
}

bool same_key(const ConstValue &p_a, const ConstValue &p_b) {
	if (p_a.kind != p_b.kind) {
		return false;
	}
	switch (p_a.kind) {
		case Kind::Int:
			return p_a.i == p_b.i;
		case Kind::Float:
			return p_a.f == p_b.f || (std::isnan(p_a.f) && std::isnan(p_b.f));
		case Kind::Bool:
			return p_a.b == p_b.b;
		case Kind::String:
			return p_a.text == p_b.text && p_a.type == p_b.type;
		case Kind::None:
			return true;
		default:
			return false;
	}
}

std::string key_text(const ConstValue &p_key) {
	switch (p_key.kind) {
		case Kind::Int:
			return std::to_string(p_key.i);
		case Kind::Float:
			return number_text(p_key.f);
		case Kind::Bool:
			return p_key.b ? "true" : "false";
		case Kind::String:
			return "\"" + p_key.text + "\"";
		default:
			return "None";
	}
}

bool is_integer_vector(const Type &p_type) {
	const std::string name = p_type.builtin_name();
	return name == "Vector2i" || name == "Vector3i" || name == "Vector4i";
}

// Whether a divisor is known not to be zero, every component of a vector.
bool known_nonzero(const ConstValue &p_divisor) {
	if (p_divisor.kind == Kind::Int) {
		return p_divisor.i != 0;
	}
	if (p_divisor.kind == Kind::Construct && is_integer_vector(p_divisor.type) && !p_divisor.items.empty()) {
		for (const ConstPtr &component : p_divisor.items) {
			if (component->kind != Kind::Int || component->i == 0) {
				return false;
			}
		}
		return true;
	}
	return false;
}

ConstPtr fold_int_binary(Expr &r_expr, int64_t p_a, int64_t p_b) {
	switch (r_expr.op) {
		case TokenKind::Plus:
			return int_value(wrapping(uint64_t(p_a) + uint64_t(p_b)));
		case TokenKind::Minus:
			return int_value(wrapping(uint64_t(p_a) - uint64_t(p_b)));
		case TokenKind::Star:
			return int_value(wrapping(uint64_t(p_a) * uint64_t(p_b)));
		case TokenKind::Slash:
		case TokenKind::Percent:
			if (p_b == 0) {
				return fail(r_expr, r_expr.op == TokenKind::Slash ? "Division by zero." : "Modulo by zero.");
			}
			// The minimum int divided by -1 wraps, as at run time.
			if (p_b == -1) {
				return int_value(r_expr.op == TokenKind::Slash ? wrapping(0ull - uint64_t(p_a)) : 0);
			}
			return int_value(r_expr.op == TokenKind::Slash ? p_a / p_b : p_a % p_b);
		case TokenKind::ShiftLeft:
		case TokenKind::ShiftRight:
			if (p_a < 0 || p_b < 0) {
				return fail(r_expr, "A shift takes operands that are not negative (spec §4.5).");
			}
			if (p_b >= 64) {
				return fail(r_expr, "A shift by 64 or more bits is undefined (spec §4.5).");
			}
			return int_value(r_expr.op == TokenKind::ShiftLeft ? wrapping(uint64_t(p_a) << p_b) : p_a >> p_b);
		case TokenKind::Ampersand:
			return int_value(p_a & p_b);
		case TokenKind::Pipe:
			return int_value(p_a | p_b);
		case TokenKind::Caret:
			return int_value(p_a ^ p_b);
		case TokenKind::EqualEqual:
			return bool_value(p_a == p_b);
		case TokenKind::BangEqual:
			return bool_value(p_a != p_b);
		case TokenKind::Less:
			return bool_value(p_a < p_b);
		case TokenKind::LessEqual:
			return bool_value(p_a <= p_b);
		case TokenKind::Greater:
			return bool_value(p_a > p_b);
		case TokenKind::GreaterEqual:
			return bool_value(p_a >= p_b);
		default:
			return nullptr;
	}
}

ConstPtr fold_float_binary(TokenKind p_op, double p_a, double p_b) {
	switch (p_op) {
		case TokenKind::Plus:
			return float_value(p_a + p_b);
		case TokenKind::Minus:
			return float_value(p_a - p_b);
		case TokenKind::Star:
			return float_value(p_a * p_b);
		case TokenKind::Slash:
			return float_value(p_a / p_b);
		case TokenKind::EqualEqual:
			return bool_value(p_a == p_b);
		case TokenKind::BangEqual:
			return bool_value(p_a != p_b);
		case TokenKind::Less:
			return bool_value(p_a < p_b);
		case TokenKind::LessEqual:
			return bool_value(p_a <= p_b);
		case TokenKind::Greater:
			return bool_value(p_a > p_b);
		case TokenKind::GreaterEqual:
			return bool_value(p_a >= p_b);
		default:
			return nullptr;
	}
}

ConstPtr fold_known_binary(Expr &r_expr, const ConstValue &p_left, const ConstValue &p_right) {
	const TokenKind op = r_expr.op;
	if (p_left.kind == Kind::Int && p_right.kind == Kind::Int) {
		return fold_int_binary(r_expr, p_left.i, p_right.i);
	}
	const bool numeric = (p_left.kind == Kind::Int || p_left.kind == Kind::Float) && (p_right.kind == Kind::Int || p_right.kind == Kind::Float);
	if (numeric) {
		return fold_float_binary(op, number(p_left), number(p_right));
	}
	if (p_left.kind == Kind::Bool && p_right.kind == Kind::Bool) {
		switch (op) {
			case TokenKind::And:
				return bool_value(p_left.b && p_right.b);
			case TokenKind::Or:
				return bool_value(p_left.b || p_right.b);
			case TokenKind::EqualEqual:
				return bool_value(p_left.b == p_right.b);
			case TokenKind::BangEqual:
				return bool_value(p_left.b != p_right.b);
			default:
				return nullptr;
		}
	}
	if (p_left.kind == Kind::String && p_right.kind == Kind::String) {
		// By content, StringName and String alike (spec §10.7).
		if (op == TokenKind::EqualEqual || op == TokenKind::BangEqual) {
			return bool_value((p_left.text == p_right.text) == (op == TokenKind::EqualEqual));
		}
		if (op == TokenKind::Plus && r_expr.type.kind == TypeKind::String) {
			auto value = make(Kind::String, r_expr.type);
			value->text = p_left.text + p_right.text;
			return value;
		}
	}
	return nullptr;
}

ConstPtr fold_binary(Expr &r_expr) {
	if (r_expr.target == Target::NoneComparison) {
		return nullptr;
	}
	const ConstPtr &left = r_expr.left->constant;
	const ConstPtr &right = r_expr.right->constant;
	if (!left || !right) {
		return nullptr;
	}
	if (left->is_known() && right->is_known()) {
		if (ConstPtr value = fold_known_binary(r_expr, *left, *right)) {
			return value;
		}
		if (!r_expr.constant_error.empty()) {
			return nullptr;
		}
	}
	if (left->kind == Kind::Preload || right->kind == Kind::Preload) {
		return nullptr;
	}
	if (r_expr.op == TokenKind::Percent && left->type.kind == TypeKind::String) {
		return fail(r_expr, "Formatting with % is not supported in constant expressions (spec §4.5).");
	}
	if (r_expr.op == TokenKind::Plus && left->kind == Kind::Array && right->kind == Kind::Array) {
		auto value = make(Kind::Array, r_expr.type);
		value->items = left->items;
		value->items.insert(value->items.end(), right->items.begin(), right->items.end());
		return value;
	}
	const std::string left_name = left->type.builtin_name();
	const std::string right_name = right->type.builtin_name();
	if (r_expr.op == TokenKind::Star && ((left_name == "Quaternion" && right_name == "Vector3") || (left_name == "Vector3" && right_name == "Quaternion"))) {
		return fail(r_expr, "Quaternion times a vector checks the value in debug builds, so constant expressions cannot use it (spec §4.5).");
	}
	if ((r_expr.op == TokenKind::Slash || r_expr.op == TokenKind::Percent) && is_integer_vector(left->type) && !known_nonzero(*right)) {
		return fail(r_expr, "The divisor of an integer vector must be known at compile time and not zero (spec §4.5).");
	}
	if ((r_expr.op == TokenKind::Slash || r_expr.op == TokenKind::Percent) && left->type.kind == TypeKind::Int && right->type.kind == TypeKind::Int && !known_nonzero(*right)) {
		return fail(r_expr, "The divisor of an int must be known at compile time (spec §4.5).");
	}
	if ((r_expr.op == TokenKind::ShiftLeft || r_expr.op == TokenKind::ShiftRight)) {
		return fail(r_expr, "Both sides of a shift must be known at compile time (spec §4.5).");
	}
	const uint8_t op = engine_operator(r_expr.op);
	if (op == OP_UNKNOWN) {
		return nullptr;
	}
	auto value = make(Kind::Operator, r_expr.type);
	value->index = op;
	value->items = { left, right };
	return value;
}

ConstPtr fold_unary(Expr &r_expr) {
	const ConstPtr &operand = r_expr.left->constant;
	if (!operand || operand->kind == Kind::Preload) {
		return nullptr;
	}
	switch (r_expr.op) {
		case TokenKind::Minus:
			if (operand->kind == Kind::Int) {
				return int_value(wrapping(0ull - uint64_t(operand->i)));
			}
			if (operand->kind == Kind::Float) {
				return float_value(-operand->f);
			}
			break;
		case TokenKind::Not:
			if (operand->kind == Kind::Bool) {
				return bool_value(!operand->b);
			}
			break;
		case TokenKind::Tilde:
			if (operand->kind == Kind::Int) {
				return int_value(~operand->i);
			}
			break;
		default:
			return nullptr;
	}
	auto value = make(Kind::Operator, r_expr.type);
	value->index = r_expr.op == TokenKind::Minus ? OP_NEGATE : (r_expr.op == TokenKind::Not ? OP_NOT : OP_BIT_NEGATE);
	value->items = { operand };
	return value;
}

ConstPtr fold_conditional(Expr &r_expr) {
	const ConstPtr &condition = r_expr.base->constant;
	if (!condition || !r_expr.left->constant || !r_expr.right->constant) {
		return nullptr;
	}
	if (condition->kind != Kind::Bool) {
		return fail(r_expr, "The condition must be known at compile time (spec §4.5).");
	}
	return converted(condition->b ? r_expr.left->constant : r_expr.right->constant, r_expr.type);
}

ConstPtr fold_name(Expr &r_expr) {
	switch (r_expr.target) {
		case Target::EngineConstant:
			return int_value(r_expr.int_value);
		case Target::BuiltinConstant: {
			auto value = make(Kind::Builtin, r_expr.type);
			value->text = r_expr.text;
			return value;
		}
		case Target::Field: {
			const ConstPtr &base = r_expr.base ? r_expr.base->constant : nullptr;
			if (!base || base->kind == Kind::Preload) {
				return nullptr;
			}
			auto value = make(Kind::Field, r_expr.type);
			value->text = r_expr.text;
			value->items = { base };
			return value;
		}
		default:
			return nullptr;
	}
}

// Color(s) of a color name looks the name up (core/math/color.h:274-280),
// which constants do not do (spec §4.5); the names are those of Color's
// constants, written with spaces, hyphens and the like as the engine
// normalizes them (core/math/color.cpp:412-432).
ConstPtr check_color_string(Expr &r_expr, const ConstValue &p_string) {
	if (p_string.kind != Kind::String) {
		return fail(r_expr, "The string of Color(s) must be known at compile time in a constant (spec §4.5).");
	}
	const std::string &text = p_string.text;
	const size_t start = !text.empty() && text[0] == '#' ? 1 : 0;
	const size_t digits = text.size() - start;
	bool hex = digits == 3 || digits == 4 || digits == 6 || digits == 8;
	for (size_t i = start; hex && i < text.size(); i++) {
		hex = std::isxdigit(static_cast<unsigned char>(text[i])) != 0;
	}
	if (hex) {
		return nullptr;
	}
	const auto normalized = [](const std::string &p_name) {
		std::string result;
		for (const char c : p_name) {
			if (c != ' ' && c != '-' && c != '_' && c != '\'' && c != '.') {
				result += char(std::toupper(static_cast<unsigned char>(c)));
			}
		}
		return result;
	};
	const builtin::Class *color = builtin::find_class("Color");
	for (uint32_t i = 0; color && i < color->constant_count; i++) {
		if (normalized(color->constants[i].name) == normalized(text)) {
			return fail(r_expr, "A color name is looked up when the class loads, so constants write the constant instead: Color." + std::string(color->constants[i].name) + " (spec §4.5).");
		}
	}
	return fail(r_expr, "\"" + text + "\" is neither a hexadecimal color nor the name of one (spec §4.5).");
}

ConstPtr fold_constructor(Expr &r_expr) {
	for (const auto &arg : r_expr.args) {
		if (!arg->constant || arg->constant->kind == Kind::Preload) {
			return nullptr;
		}
	}
	const std::string &type = r_expr.owner;
	std::vector<std::string> params;
	for (const Type &param : r_expr.param_types) {
		params.push_back(param.builtin_name());
	}
	if (type == "Color" && !params.empty() && params[0] == "String") {
		check_color_string(r_expr, *r_expr.args[0]->constant);
		if (!r_expr.constant_error.empty()) {
			return nullptr;
		}
	}
	const bool checks_value = (type == "Basis" && params == std::vector<std::string>{ "Vector3", "float" }) ||
			(type == "Quaternion" && (params == std::vector<std::string>{ "Vector3", "float" } || params == std::vector<std::string>{ "Basis" } || params == std::vector<std::string>{ "Vector3", "Vector3" }));
	if (checks_value) {
		return fail(r_expr, "This constructor of " + type + " checks its arguments in debug builds, so constant expressions cannot use it (spec §4.5).");
	}
	if (is_text(r_expr.type) && params.size() == 1 && is_text(r_expr.param_types[0]) && r_expr.args[0]->constant->kind == Kind::String) {
		auto value = make(Kind::String, r_expr.type);
		value->text = r_expr.args[0]->constant->text;
		return value;
	}
	auto value = make(Kind::Construct, r_expr.type);
	value->index = r_expr.index;
	value->params = r_expr.param_types;
	for (size_t i = 0; i < r_expr.args.size(); i++) {
		value->items.push_back(converted(r_expr.args[i]->constant, r_expr.param_types[i]));
	}
	return value;
}

ConstPtr fold_conversion(Expr &r_expr) {
	const ConstPtr &arg = r_expr.args.empty() ? nullptr : r_expr.args[0]->constant;
	if (!arg || arg->kind == Kind::Preload) {
		return nullptr;
	}
	if (r_expr.target == Target::ConvertToFloat) {
		return converted(arg, Type::of(TypeKind::Float));
	}
	if (arg->kind == Kind::Int) {
		return arg;
	}
	if (arg->kind != Kind::Float) {
		return fail(r_expr, "The float converted to int must be known at compile time (spec §4.5).");
	}
	if (!fits_int(arg->f)) {
		return fail(r_expr, "Cannot convert " + number_text(arg->f) + " to int: out of range.");
	}
	return int_value(int64_t(arg->f));
}

// float to int inside a math function, checked as int(x) is.
std::optional<int64_t> rounded(Expr &r_expr, double p_value) {
	if (!fits_int(p_value)) {
		fail(r_expr, "Cannot convert " + number_text(p_value) + " to int: out of range.");
		return std::nullopt;
	}
	return int64_t(p_value);
}

// Math functions whose result is an int or a bool, of known arguments, as the
// engine computes them (core/variant/variant_utility.cpp, core/math/math_funcs.h).
ConstPtr fold_known_math(Expr &r_expr, const std::vector<ConstPtr> &p_args) {
	const std::string &name = r_expr.text;
	const auto i = [&](size_t p_index) { return p_args[p_index]->i; };
	const auto f = [&](size_t p_index) { return number(*p_args[p_index]); };
	const bool ints = std::all_of(p_args.begin(), p_args.end(), [](const ConstPtr &p_arg) { return p_arg->kind == Kind::Int; });
	if (r_expr.type.kind == TypeKind::Bool) {
		if (name == "is_equal_approx") {
			if (f(0) == f(1)) {
				return bool_value(true);
			}
			const double tolerance = std::max(0.00001 * std::abs(f(0)), 0.00001);
			return bool_value(std::abs(f(0) - f(1)) < tolerance);
		}
		if (name == "is_zero_approx") {
			return bool_value(std::abs(f(0)) < 0.00001);
		}
		if (name == "is_finite") {
			return bool_value(std::isfinite(f(0)));
		}
		if (name == "is_inf") {
			return bool_value(std::isinf(f(0)));
		}
		if (name == "is_nan") {
			return bool_value(std::isnan(f(0)));
		}
		return nullptr;
	}
	if (r_expr.type.kind != TypeKind::Int) {
		return nullptr;
	}
	if ((name == "absi" || name == "abs") && ints) {
		return int_value(i(0) < 0 ? wrapping(0ull - uint64_t(i(0))) : i(0));
	}
	if ((name == "signi" || name == "sign") && ints) {
		return int_value(i(0) == 0 ? 0 : (i(0) < 0 ? -1 : 1));
	}
	if ((name == "floor" || name == "ceil" || name == "round") && ints) {
		return p_args[0];
	}
	if ((name == "mini" || name == "min" || name == "maxi" || name == "max") && ints) {
		const bool least = name == "mini" || name == "min";
		int64_t best = i(0);
		for (size_t a = 1; a < p_args.size(); a++) {
			if (least ? best > i(a) : best < i(a)) {
				best = i(a);
			}
		}
		return int_value(best);
	}
	if (name == "clampi" && ints) {
		return int_value(i(0) < i(1) ? i(1) : (i(0) > i(2) ? i(2) : i(0)));
	}
	if (name == "clamp" && ints) {
		// Against the minimum, then the result against the maximum.
		int64_t value = i(0) < i(1) ? i(1) : i(0);
		return int_value(value > i(2) ? i(2) : value);
	}
	if ((name == "wrapi" || name == "wrap") && ints) {
		const int64_t range = wrapping(uint64_t(i(2)) - uint64_t(i(1)));
		if (range == 0) {
			return int_value(i(1));
		}
		const int64_t offset = wrapping(uint64_t(i(0)) - uint64_t(i(1)));
		return int_value(wrapping(uint64_t(i(1)) + uint64_t(((offset % range) + range) % range)));
	}
	if (name == "posmod" && ints) {
		if (i(1) == 0) {
			return fail(r_expr, "Modulo by zero.");
		}
		if (i(1) == -1) {
			return int_value(0);
		}
		int64_t value = i(0) % i(1);
		if ((value < 0 && i(1) > 0) || (value > 0 && i(1) < 0)) {
			value += i(1);
		}
		return int_value(value);
	}
	if (name == "nearest_po2" && ints) {
		uint64_t value = uint64_t(i(0)) - 1;
		for (int shift = 1; shift < 64; shift <<= 1) {
			value |= value >> shift;
		}
		return int_value(wrapping(value + 1));
	}
	if (name == "floori" || name == "ceili" || name == "roundi") {
		const double value = name == "floori" ? std::floor(f(0)) : (name == "ceili" ? std::ceil(f(0)) : std::round(f(0)));
		const std::optional<int64_t> result = rounded(r_expr, value);
		return result ? int_value(*result) : nullptr;
	}
	if (name == "snappedi" || (name == "snapped" && ints)) {
		const double step = double(i(1));
		const double value = step != 0 ? std::floor(f(0) / step + 0.5) * step : f(0);
		const std::optional<int64_t> result = rounded(r_expr, value);
		return result ? int_value(*result) : nullptr;
	}
	if (name == "step_decimals") {
		const double magnitude = std::abs(f(0));
		if (!(magnitude < 2147483648.0)) {
			return fail(r_expr, "Cannot convert " + number_text(f(0)) + " to int: out of range.");
		}
		static const double steps[] = { 0.9999, 0.09999, 0.009999, 0.0009999, 0.00009999, 0.000009999, 0.0000009999, 0.00000009999, 0.000000009999, 0.0000000009999 };
		const double decimals = magnitude - int(magnitude);
		for (int s = 0; s < 10; s++) {
			if (decimals >= steps[s]) {
				return int_value(s);
			}
		}
		return int_value(0);
	}
	return nullptr;
}

// Math functions that round a float to an int.
bool rounds_float(const std::string &p_name) {
	static const std::unordered_set<std::string> names = { "floori", "ceili", "roundi", "snappedi", "step_decimals" };
	return names.count(p_name) > 0;
}

ConstPtr fold_utility(Expr &r_expr) {
	const builtin::Utility *utility = builtin::find_utility(r_expr.text);
	if (!utility || !utility->is_math) {
		return nullptr;
	}
	std::vector<ConstPtr> args;
	bool known = true;
	for (const auto &arg : r_expr.args) {
		if (!arg->constant || arg->constant->kind == Kind::Preload) {
			return nullptr;
		}
		args.push_back(arg->constant);
		known = known && arg->constant->is_known();
	}
	if (rounds_float(r_expr.text) && !args.empty() && !args[0]->is_known()) {
		return fail(r_expr, "The float that " + r_expr.text + "() rounds to an int must be known at compile time (spec §4.5).");
	}
	if (r_expr.text == "posmod" && args.size() == 2 && !args[1]->is_known()) {
		return fail(r_expr, "The divisor of posmod() must be known at compile time (spec §4.5).");
	}
	if (known) {
		if (ConstPtr value = fold_known_math(r_expr, args)) {
			return value;
		}
		if (!r_expr.constant_error.empty()) {
			return nullptr;
		}
	}
	auto value = make(Kind::Utility, r_expr.type);
	value->text = r_expr.text;
	value->hash = r_expr.hash;
	value->params = r_expr.param_types;
	value->returns = r_expr.engine_return;
	for (size_t a = 0; a < args.size(); a++) {
		value->items.push_back(a < r_expr.param_types.size() ? converted(args[a], r_expr.param_types[a]) : args[a]);
	}
	return value;
}

ConstPtr fold_array(Expr &r_expr) {
	const std::optional<Type> element = element_of(r_expr.type);
	if (!r_expr.args.empty() && element && is_boxed(*element)) {
		return nullptr; // Boxes are made as the program runs.
	}
	auto value = make(Kind::Array, r_expr.type);
	for (size_t i = 0; i < r_expr.args.size(); i++) {
		const ConstPtr &item = r_expr.args[i]->constant;
		if (!item) {
			return nullptr;
		}
		if (loads(*item)) {
			return fail(r_expr, "preload() in a container in a constant is not supported yet.");
		}
		value->items.push_back(converted(item, stored_element_type(r_expr, i)));
	}
	return value;
}

ConstPtr fold_dictionary(Expr &r_expr) {
	const std::optional<std::pair<Type, Type>> entries = entries_of(r_expr.type);
	if (!r_expr.args.empty() && entries && (is_boxed(entries->first) || is_boxed(entries->second))) {
		return nullptr;
	}
	auto value = make(Kind::Dictionary, r_expr.type);
	for (size_t i = 0; i + 1 < r_expr.args.size(); i += 2) {
		const ConstPtr &key = r_expr.args[i]->constant;
		const ConstPtr &item = r_expr.args[i + 1]->constant;
		if (!key || !item) {
			return nullptr;
		}
		if (!key->is_known()) {
			return fail(r_expr, "The keys of a dictionary in a constant must be known at compile time (spec §4.5).", r_expr.args[i].get());
		}
		if (loads(*item)) {
			return fail(r_expr, "preload() in a container in a constant is not supported yet.");
		}
		const ConstPtr stored = converted(key, entries ? entries->first : key->type);
		for (size_t k = 0; k < value->items.size(); k += 2) {
			if (same_key(*value->items[k], *stored)) {
				return fail(r_expr, "The key " + key_text(*key) + " is already in this dictionary.", r_expr.args[i].get());
			}
		}
		value->items.push_back(stored);
		value->items.push_back(converted(item, entries ? entries->second : item->type));
	}
	return value;
}

ConstPtr fold_subscript(Expr &r_expr) {
	const ConstPtr &base = r_expr.base->constant;
	const ConstPtr &index = r_expr.right->constant;
	if (!base || !index || !base->is_container()) {
		return nullptr;
	}
	if (!index->is_known()) {
		return fail(r_expr, "The index of a constant must be known at compile time (spec §4.5).");
	}
	if (base->kind == Kind::Array) {
		const int64_t size = int64_t(base->items.size());
		// A negative index counts from the end, as at run time.
		const int64_t at = index->i < 0 ? index->i + size : index->i;
		if (index->kind != Kind::Int || at < 0 || at >= size) {
			return fail(r_expr, "Index " + std::to_string(index->i) + " is out of bounds (size " + std::to_string(size) + ").");
		}
		return base->items[size_t(at)];
	}
	const std::optional<std::pair<Type, Type>> entries = entries_of(base->type);
	const ConstPtr key = converted(index, entries ? entries->first : index->type);
	for (size_t k = 0; k + 1 < base->items.size(); k += 2) {
		if (same_key(*base->items[k], *key)) {
			return base->items[k + 1];
		}
	}
	return fail(r_expr, "The dictionary has no key " + key_text(*index) + ".");
}

} // namespace

ConstPtr converted(const ConstPtr &p_value, const Type &p_to) {
	if (!p_value || p_to.is_error() || p_to.kind == TypeKind::Variant || p_value->type == p_to) {
		return p_value;
	}
	if (p_to.kind == TypeKind::Option && p_value->kind != Kind::None) {
		return converted(p_value, p_to.args[0]); // Some(value), which lowering wraps.
	}
	if (p_to.kind == TypeKind::Float && p_value->type.kind == TypeKind::Int) {
		if (p_value->kind == Kind::Int) {
			return float_value(double(p_value->i));
		}
	}
	if (p_value->kind == Kind::String && is_text(p_to)) {
		auto value = std::make_shared<ConstValue>(*p_value);
		value->type = p_to;
		return value;
	}
	if (!p_value->is_known() && p_value->type.kind != TypeKind::Object && p_value->type.builtin_name() != p_to.builtin_name()) {
		// Converted by the engine too, as the type's constructor converts.
		const int constructor = converting_constructor(p_value->type, p_to);
		if (constructor >= 0) {
			auto value = make(Kind::Construct, p_to);
			value->index = constructor;
			value->params = { p_value->type };
			value->items = { p_value };
			return value;
		}
	}
	auto value = std::make_shared<ConstValue>(*p_value);
	value->type = p_to;
	if (p_value->kind == Kind::Array) {
		const std::optional<Type> element = element_of(p_to);
		for (ConstPtr &item : value->items) {
			item = converted(item, element ? *element : item->type);
		}
	} else if (p_value->kind == Kind::Dictionary) {
		const std::optional<std::pair<Type, Type>> entries = entries_of(p_to);
		for (size_t k = 0; k + 1 < value->items.size(); k += 2) {
			value->items[k] = converted(value->items[k], entries ? entries->first : value->items[k]->type);
			value->items[k + 1] = converted(value->items[k + 1], entries ? entries->second : value->items[k + 1]->type);
		}
	}
	return value;
}

bool loads(const ConstValue &p_value) {
	if (p_value.kind == Kind::Preload) {
		return true;
	}
	for (const ConstPtr &item : p_value.items) {
		if (loads(*item)) {
			return true;
		}
	}
	return false;
}

ConstPtr fold(Expr &r_expr) {
	if (r_expr.type.is_error()) {
		return nullptr;
	}
	switch (r_expr.kind) {
		case ExprKind::IntLiteral:
			return r_expr.int_overflows ? nullptr : int_value(r_expr.int_value);
		case ExprKind::FloatLiteral:
			return float_value(r_expr.float_value);
		case ExprKind::StringLiteral: {
			auto value = make(Kind::String, r_expr.type);
			value->text = r_expr.text;
			return value;
		}
		case ExprKind::BoolLiteral:
			return bool_value(r_expr.bool_value);
		case ExprKind::NoneLiteral:
			return make(Kind::None, r_expr.type);
		case ExprKind::Name:
		case ExprKind::Member:
			return fold_name(r_expr);
		case ExprKind::Unary:
			return fold_unary(r_expr);
		case ExprKind::Binary:
			return fold_binary(r_expr);
		case ExprKind::Conditional:
			return fold_conditional(r_expr);
		case ExprKind::Call:
			switch (r_expr.target) {
				case Target::BuiltinConstructor:
					return fold_constructor(r_expr);
				case Target::ConvertToInt:
				case Target::ConvertToFloat:
					return fold_conversion(r_expr);
				case Target::Utility:
					return fold_utility(r_expr);
				default:
					return nullptr;
			}
		case ExprKind::ArrayLiteral:
			return fold_array(r_expr);
		case ExprKind::DictionaryLiteral:
			return fold_dictionary(r_expr);
		case ExprKind::Subscript:
			return fold_subscript(r_expr);
		default:
			return nullptr;
	}
}

} // namespace petrichor::compiler
