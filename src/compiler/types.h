#pragma once

#include <string>
#include <vector>

namespace petrichor::compiler {

// Types the checker works with.
enum class TypeKind {
	Error, // Already reported; suppresses follow-up errors.
	Void,
	Bool,
	Int,
	Float,
	String,
	Builtin, // Another built-in value type of the engine, named by `name`.
	Object, // An engine class named by `name`.
	Option, // T? of a value type T, which is `args[0]` (spec §3.4); objects carry `nullable` instead.
	Function, // A function value (spec §3.11): `args` holds the return type, then the parameter types.
	Signal, // A signal of an object: `args` holds the types of its arguments.
	Variant,
	Unsupported, // An engine type this compiler cannot handle yet.
};

struct Type {
	TypeKind kind = TypeKind::Error;
	std::string name; // Builtin: the type, such as Vector3; Object: the class; Unsupported: the engine's name.
	bool nullable = false; // Object: T? rather than T (spec §3.3).
	std::vector<Type> args; // Option: the value type inside; Array: the element type, if typed.

	static Type of(TypeKind p_kind) { return Type{ p_kind, std::string(), false, {} }; }
	static Type builtin(const std::string &p_name) { return Type{ TypeKind::Builtin, p_name, false, {} }; }
	static Type object(const std::string &p_class, bool p_nullable = false) { return Type{ TypeKind::Object, p_class, p_nullable, {} }; }
	static Type option(const Type &p_inner) { return Type{ TypeKind::Option, std::string(), false, { p_inner } }; }
	static Type array(const Type &p_element) { return Type{ TypeKind::Builtin, "Array", false, { p_element } }; }
	static Type function(const Type &p_return, const std::vector<Type> &p_params) {
		Type type{ TypeKind::Function, std::string(), false, { p_return } };
		type.args.insert(type.args.end(), p_params.begin(), p_params.end());
		return type;
	}
	static Type signal(const std::vector<Type> &p_params) { return Type{ TypeKind::Signal, std::string(), false, p_params }; }

	bool operator==(const Type &p_other) const { return kind == p_other.kind && name == p_other.name && nullable == p_other.nullable && args == p_other.args; }
	bool operator!=(const Type &p_other) const { return !(*this == p_other); }

	bool is_error() const { return kind == TypeKind::Error; }
	bool is_numeric() const { return kind == TypeKind::Int || kind == TypeKind::Float; }
	// Bool, int, float and String have instructions of their own; the other
	// value types go through the engine's functions for each type.
	bool is_primitive() const { return kind == TypeKind::Bool || is_numeric() || kind == TypeKind::String; }
	bool is_storable() const { return kind != TypeKind::Error && kind != TypeKind::Void && kind != TypeKind::Unsupported; }
	// May be None: T? of an object or of a value type.
	bool is_optional() const { return kind == TypeKind::Option || (kind == TypeKind::Object && nullable); }
	// The type inside T?, which unwrap() gives.
	Type unwrapped() const;
	// The name of the type in the engine's built-in table: "int", "Vector3"…;
	// empty for objects, options and Variant.
	std::string builtin_name() const;
	std::string to_string() const;
};

// A type named as godot-cpp's extension API names it: "float", "Vector3",
// "Variant", "enum::Vector3.Axis", a class… Objects the engine returns are
// nullable until audited (spec §10.2), so `p_object_nullable` says how to read
// a class name.
Type api_type(const std::string &p_name, bool p_object_nullable);

} // namespace petrichor::compiler
