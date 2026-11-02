#pragma once

#include "compiler/lexer.h"
#include "compiler/types.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace petrichor::compiler {

struct Expr;

// Variant::Operator (core/variant/variant.h:564-597).
enum EngineOperator : uint8_t {
	OP_EQUAL,
	OP_NOT_EQUAL,
	OP_LESS,
	OP_LESS_EQUAL,
	OP_GREATER,
	OP_GREATER_EQUAL,
	OP_ADD,
	OP_SUBTRACT,
	OP_MULTIPLY,
	OP_DIVIDE,
	OP_NEGATE,
	OP_POSITIVE,
	OP_MODULE,
	OP_POWER,
	OP_SHIFT_LEFT,
	OP_SHIFT_RIGHT,
	OP_BIT_AND,
	OP_BIT_OR,
	OP_BIT_XOR,
	OP_BIT_NEGATE,
	OP_AND,
	OP_OR,
	OP_XOR,
	OP_NOT,
	OP_IN,
	OP_UNKNOWN = 0xFF, // Not one of the engine's.
};

// The engine's operator for a binary operator token.
uint8_t engine_operator(TokenKind p_op);

// The value of a constant expression (spec §4.5). The compiler computes
// scalars and the structure of containers; what the engine computes when the
// class loads is kept as the computation, with known parts in it.
struct ConstValue {
	enum class Kind {
		// Known to the compiler.
		Int,
		Float,
		Bool,
		String, // Also a StringName or a NodePath, as `type` says.
		None,
		// Containers, whose elements may be either.
		Array, // `items` are the elements.
		Dictionary, // `items` are each key and its value, one after another.
		// Computed by the engine when the class loads.
		Builtin, // The constant `text` of the built-in type, such as UP of Vector3.
		Construct, // The constructor `index` of the type, in the engine's list, of `items`.
		Operator, // The engine's operator `index` (Variant::Operator) on `items`, one or two.
		Utility, // The math utility function `text`, with `hash`, of `items`.
		Field, // The member `text` of the built-in value `items[0]`.
		// The resource at the path `text`, loaded where it is used, by `source`.
		Preload,
	};
	Kind kind = Kind::None;
	Type type;
	int64_t i = 0;
	double f = 0.0;
	bool b = false;
	std::string text;
	int index = -1;
	uint32_t hash = 0;
	std::vector<std::shared_ptr<const ConstValue>> items;
	// Construct and Utility: the types `items` go in as, Variant for what the
	// engine takes as a Variant; Utility: what the engine declares it returns.
	std::vector<Type> params;
	Type returns;
	const Expr *source = nullptr;
	// Preload of a script: its file, and the class it names (spec §2.1),
	// which is Error when it cannot be known while only the declarations are
	// checked.
	std::string script;
	Type script_class;

	bool is_known() const { return kind <= Kind::None; }
	bool is_container() const { return kind == Kind::Array || kind == Kind::Dictionary; }
};

using ConstPtr = std::shared_ptr<const ConstValue>;

// The value of an expression the checker has typed, from the values of its
// parts (spec §4.5); null when it is not a constant expression, with the
// reason in `constant_error` when computing it would fail or is not allowed.
ConstPtr fold(Expr &r_expr);

// The value as stored where a `p_to` is expected: int as float (spec §3.2),
// a String as a StringName or a NodePath (§10.7), the elements of a container
// as its element types; for a T?, the value inside Some, or None.
ConstPtr converted(const ConstPtr &p_value, const Type &p_to);

// Whether a preload is in the value, which no table of constants can hold.
bool loads(const ConstValue &p_value);

// A value of an enum (spec §3.7), an int typed as the enum, and its name as a
// value: a Dictionary of the names and the values.
ConstPtr enum_constant(int64_t p_value, const Type &p_enum);
ConstPtr enum_dictionary(const EnumValues &p_values, const Type &p_enum);

} // namespace petrichor::compiler
