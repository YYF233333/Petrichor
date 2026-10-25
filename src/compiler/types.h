#pragma once

#include <string>

namespace petrichor::compiler {

// Types the checker works with. The subset of the first compilation chain
// (architecture §5 step 3) stores bool, int, float and String; Variant only
// appears as the parameter type of engine functions such as print.
enum class TypeKind {
	Error, // Already reported; suppresses follow-up errors.
	Void,
	Bool,
	Int,
	Float,
	String,
	Variant,
	Object,
	Unsupported, // An engine type this compiler cannot handle yet.
};

struct Type {
	TypeKind kind = TypeKind::Error;
	std::string name; // Class name for Object; engine type name for Unsupported.

	static Type of(TypeKind p_kind) { return Type{ p_kind, std::string() }; }

	bool operator==(const Type &p_other) const { return kind == p_other.kind && name == p_other.name; }
	bool operator!=(const Type &p_other) const { return !(*this == p_other); }

	bool is_error() const { return kind == TypeKind::Error; }
	bool is_numeric() const { return kind == TypeKind::Int || kind == TypeKind::Float; }
	bool is_storable() const {
		return kind == TypeKind::Bool || kind == TypeKind::Int || kind == TypeKind::Float || kind == TypeKind::String;
	}
	std::string to_string() const;
};

} // namespace petrichor::compiler
