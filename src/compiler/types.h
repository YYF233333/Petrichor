#pragma once

#include <optional>
#include <string>
#include <vector>

namespace petrichor::compiler {

// Types the checker works with.
enum class TypeKind {
	Error, // Already reported; suppresses follow-up errors.
	Void,
	Task,
	Bool,
	Int,
	Float,
	String,
	Builtin, // Another built-in value type of the engine, named by `name`.
	Object, // An engine class named by `name`.
	Option, // T? of a value type T, which is `args[0]` (spec §3.4); objects carry `nullable` instead.
	Struct, // A struct (spec §3.6), named and declared as an interface is.
	Function, // A function value (spec §3.11): `args` holds the return type, then the parameter types.
	Signal, // A signal of an object: `args` holds the types of its arguments.
	None, // The literal None where nothing says which T? it is.
	Variant,
	Unsupported, // An engine type this compiler cannot handle yet.
};

struct Type {
	TypeKind kind = TypeKind::Error;
	// Builtin: the type, such as Vector3; Object: the class, or the name of a
	// class or interface of this language; Struct: its name, such as
	// Combat.Hit; Unsupported: the engine's name.
	std::string name;
	bool nullable = false; // Object: T? rather than T (spec §3.3).
	// Option: the value type inside; Array: the element type, if typed;
	// Dictionary: the key and value types, if typed; PackedScene: the type of
	// the scene's root, if known.
	std::vector<Type> args;
	// Object: a class or interface of this language, declared in this script
	// (its res:// path), and the engine class its objects have. Struct: the
	// script that declares it.
	std::string script;
	std::string base;
	bool is_interface = false;
	bool is_async = false;
	static Type task(const Type &p_result) { return Type{ TypeKind::Task, std::string(), false, { p_result } }; }

	static Type of(TypeKind p_kind) { return Type{ p_kind }; }
	static Type builtin(const std::string &p_name) { return Type{ TypeKind::Builtin, p_name }; }
	static Type object(const std::string &p_class, bool p_nullable = false) { return Type{ TypeKind::Object, p_class, p_nullable }; }
	static Type option(const Type &p_inner) { return Type{ TypeKind::Option, std::string(), false, { p_inner } }; }
	static Type array(const Type &p_element) { return Type{ TypeKind::Builtin, "Array", false, { p_element } }; }
	static Type dictionary(const Type &p_key, const Type &p_value) { return Type{ TypeKind::Builtin, "Dictionary", false, { p_key, p_value } }; }
	// PackedScene[T], a scene whose root is a T (spec §7.4).
	static Type scene(const Type &p_root) { return Type{ TypeKind::Object, "PackedScene", false, { p_root } }; }
	static Type function(const Type &p_return, const std::vector<Type> &p_params) {
		Type type{ TypeKind::Function, std::string(), false, { p_return } };
		type.args.insert(type.args.end(), p_params.begin(), p_params.end());
		return type;
	}
	static Type signal(const std::vector<Type> &p_params) { return Type{ TypeKind::Signal, std::string(), false, p_params }; }
	// A class of this language (spec §11.1): its name, its script and the
	// engine class it extends.
	static Type script_class(const std::string &p_name, const std::string &p_script, const std::string &p_base) {
		Type type = object(p_name);
		type.script = p_script;
		type.base = p_base;
		return type;
	}
	// An interface (spec §3.9), named as written, such as Combat.Damageable.
	static Type interface(const std::string &p_name, const std::string &p_script) {
		Type type = script_class(p_name, p_script, "Object");
		type.is_interface = true;
		return type;
	}
	static Type structure(const std::string &p_name, const std::string &p_script) {
		Type type{ TypeKind::Struct, p_name };
		type.script = p_script;
		return type;
	}

	bool operator==(const Type &p_other) const {
		return kind == p_other.kind && name == p_other.name && nullable == p_other.nullable && args == p_other.args && script == p_other.script && is_interface == p_other.is_interface && is_async == p_other.is_async;
	}
	bool operator!=(const Type &p_other) const { return !(*this == p_other); }

	bool is_error() const { return kind == TypeKind::Error; }
	bool is_numeric() const { return kind == TypeKind::Int || kind == TypeKind::Float; }
	// Bool, int, float and String have instructions of their own; the other
	// value types go through the engine's functions for each type.
	bool is_primitive() const { return kind == TypeKind::Bool || is_numeric() || kind == TypeKind::String; }
	bool is_storable() const { return kind != TypeKind::Error && kind != TypeKind::Void && kind != TypeKind::Unsupported; }
	// May be None: T? of an object or of a value type.
	bool is_optional() const { return kind == TypeKind::Option || ((kind == TypeKind::Object || kind == TypeKind::Task) && nullable); }
	// The type inside T?, which unwrap() gives.
	Type unwrapped() const;
	// Object: the engine class its objects have.
	const std::string &engine_class() const { return script.empty() ? name : base; }
	// The name of the type in the engine's built-in table: "int", "Vector3"…;
	// empty for objects, options and Variant.
	std::string builtin_name() const;
	// An interface's or a struct's name in its script: Damageable of Combat.Damageable.
	std::string interface_name() const { return name.substr(name.find_last_of('.') + 1); }
	std::string to_string() const;
};

// A type named as godot-cpp's extension API names it: "float", "Vector3",
// "Variant", "enum::Vector3.Axis", a class… Objects the engine returns are
// nullable until audited (spec §10.2), so `p_object_nullable` says how to read
// a class name.
Type api_type(const std::string &p_name, bool p_object_nullable);

// How classes relate: engine classes as ClassDB says, classes of this
// language as their declarations say.
class ClassRelation {
public:
	virtual ~ClassRelation() = default;
	// Whether every object of class `p_from` is a `p_to`; nullability aside.
	virtual bool is_a(const Type &p_from, const Type &p_to) const = 0;
};

// Whether a value of type `p_from` can be stored where `p_to` is expected:
// the same type, int to float (spec §3.2), String to and from StringName and
// NodePath (§10.7), an object to a nullable or a base class, or anything to
// Variant.
bool is_assignable(const Type &p_from, const Type &p_to, const ClassRelation &p_classes);

// Values that refer to shared engine storage: changing one through a method
// changes it for everyone who holds it, so it is not written back.
bool is_shared(const Type &p_type);

// The elements of a value that can be indexed and iterated (spec §3.8, §5.7);
// Variant for an untyped Array.
std::optional<Type> element_of(const Type &p_type);

// The key and value types of a Dictionary (spec §3.8); Variant for an untyped
// one.
std::optional<std::pair<Type, Type>> entries_of(const Type &p_type);

// An Array[T] or a Dictionary[K, V], which is made empty and typed.
bool is_typed_container(const Type &p_type);

// Whether a container keeps values of the type boxed (architecture §2.2): a
// struct, a T? of a value type, or a container that keeps its elements or keys
// boxed, which then enters a Variant only through `Array(hits)` or
// `Dictionary(d)` (spec §3.8).
bool is_boxed(const Type &p_type);

// T? of a type: nullable for an object, an Option otherwise.
Type optional_of(const Type &p_type);

} // namespace petrichor::compiler
