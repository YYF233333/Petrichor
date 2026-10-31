#include "compiler/types.h"

#include "compiler/builtin_api.h"

#include <unordered_map>

namespace petrichor::compiler {

std::string Type::builtin_name() const {
	switch (kind) {
		case TypeKind::Bool:
			return "bool";
		case TypeKind::Int:
			return "int";
		case TypeKind::Float:
			return "float";
		case TypeKind::String:
			return "String";
		case TypeKind::Builtin:
			return name;
		// Their methods are the engine's, such as bind() and connect().
		case TypeKind::Function:
			return "Callable";
		case TypeKind::Signal:
			return "Signal";
		default:
			return std::string();
	}
}

// `func(int, Node) -> bool`, as written (spec §2.4); `func(int)` returns void.
static std::string signature(const std::vector<Type> &p_params, size_t p_first) {
	std::string text = "(";
	for (size_t i = p_first; i < p_params.size(); i++) {
		text += (i > p_first ? ", " : "") + p_params[i].to_string();
	}
	return text + ")";
}

std::string Type::to_string() const {
	switch (kind) {
		case TypeKind::Error:
			return "<error>";
		case TypeKind::Void:
			return "void";
		case TypeKind::Variant:
			return "Variant";
		case TypeKind::None:
			return "None";
		case TypeKind::Object: {
			// PackedScene[T] carries the type of the scene's root (spec §7.4).
			const std::string text = args.empty() ? name : name + "[" + args[0].to_string() + "]";
			return nullable ? text + "?" : text;
		}
		case TypeKind::Option:
			return (args.empty() ? std::string("<error>") : args[0].to_string()) + "?";
		case TypeKind::Task:
			return "Task[" + args[0].to_string() + "]" + (nullable ? "?" : "");
		case TypeKind::Function:
			return std::string(is_async ? "async func" : "func") + signature(args, 1) + (args.empty() || args[0].kind == TypeKind::Void ? std::string() : " -> " + args[0].to_string());
		case TypeKind::Signal:
			return "Signal" + signature(args, 0);
		case TypeKind::Builtin:
			if (!args.empty()) {
				std::string text = name + "[";
				for (size_t i = 0; i < args.size(); i++) {
					text += (i > 0 ? ", " : "") + args[i].to_string();
				}
				return text + "]";
			}
			return name;
		case TypeKind::Unsupported:
		case TypeKind::Struct:
			return name;
		default:
			return builtin_name();
	}
}

Type Type::unwrapped() const {
	if (kind == TypeKind::Option) {
		return args.empty() ? of(TypeKind::Error) : args[0];
	}
	Type result = *this;
	result.nullable = false;
	return result;
}

Type api_type(const std::string &p_name, bool p_object_nullable) {
	if (p_name.empty() || p_name == "void" || p_name == "Nil") {
		return Type::of(TypeKind::Void);
	}
	if (p_name == "bool") {
		return Type::of(TypeKind::Bool);
	}
	if (p_name == "int" || p_name.rfind("enum::", 0) == 0 || p_name.rfind("bitfield::", 0) == 0) {
		return Type::of(TypeKind::Int);
	}
	if (p_name == "float") {
		return Type::of(TypeKind::Float);
	}
	if (p_name == "String") {
		return Type::of(TypeKind::String);
	}
	if (p_name == "Variant") {
		return Type::of(TypeKind::Variant);
	}
	if (p_name.rfind("typedarray::", 0) == 0) {
		// The engine's typed array, whose elements are checked as they are read
		// (spec §3.8); hint forms such as "24/17:Mesh" stay untyped for now.
		const std::string element = p_name.substr(12);
		Type array = Type::builtin("Array");
		if (element.find_first_of(":/") == std::string::npos) {
			array.args.push_back(api_type(element, false));
		}
		return array;
	}
	if (builtin::find_class(p_name)) {
		return Type::builtin(p_name);
	}
	if (p_name.find("::") != std::string::npos || p_name.find('*') != std::string::npos) {
		return Type{ TypeKind::Unsupported, p_name, false };
	}
	return Type::object(p_name, p_object_nullable);
}

static bool is_string_like(const Type &p_type) {
	return p_type.kind == TypeKind::String || (p_type.kind == TypeKind::Builtin && (p_type.name == "StringName" || p_type.name == "NodePath"));
}

bool is_assignable(const Type &p_from, const Type &p_to, const ClassRelation &p_classes) {
	if (p_from.is_error() || p_to.is_error()) {
		return true;
	}
	if (p_from == p_to) {
		return true;
	}
	if (p_from.kind == TypeKind::Task && p_to.kind == TypeKind::Task) {
		return p_from.args == p_to.args && (!p_from.nullable || p_to.nullable);
	}
	if (p_from.kind == TypeKind::None) {
		return p_to.is_optional() || p_to.kind == TypeKind::Variant;
	}
	if (p_from.kind == TypeKind::Int && p_to.kind == TypeKind::Float) {
		return true;
	}
	// Converted where needed, as in GDScript (spec §10.7).
	if (is_string_like(p_from) && is_string_like(p_to) && (p_from.kind == TypeKind::String || p_to.kind == TypeKind::String)) {
		return true;
	}
	if (p_from.kind == TypeKind::Object && p_to.kind == TypeKind::Object) {
		if (!p_to.args.empty()) {
			// PackedScene[T] is covariant in T, and a scene of unknown root is
			// not one (spec §7.4).
			if (p_from.args.empty() || !p_classes.is_a(p_from.args[0], p_to.args[0])) {
				return false;
			}
		}
		return (p_to.nullable || !p_from.nullable) && p_classes.is_a(p_from, p_to);
	}
	if (p_to.kind == TypeKind::Option) {
		// A T is wrapped implicitly; the T inside does not convert (spec §3.4).
		return p_from.kind == TypeKind::Option ? p_from.args == p_to.args : is_assignable(p_from, p_to.args[0], p_classes);
	}
	if (p_from.kind == TypeKind::Function && p_to.kind == TypeKind::Function) {
		// Parameters contravariant, the result covariant (spec §3.11).
		if (p_from.is_async != p_to.is_async || p_from.args.size() != p_to.args.size()) {
			return false;
		}
		for (size_t i = 1; i < p_to.args.size(); i++) {
			if (!is_assignable(p_to.args[i], p_from.args[i], p_classes)) {
				return false;
			}
		}
		return p_to.args[0].kind == TypeKind::Void ? p_from.args[0].kind == TypeKind::Void : is_assignable(p_from.args[0], p_to.args[0], p_classes);
	}
	// The engine takes them as its Callable and Signal.
	if ((p_from.kind == TypeKind::Function || p_from.kind == TypeKind::Signal) && p_to.kind == TypeKind::Builtin && p_to.name == p_from.builtin_name()) {
		return true;
	}
	return p_to.kind == TypeKind::Variant && p_from.is_storable();
}

bool is_shared(const Type &p_type) {
	// A Signal names an object's signal; connecting changes the object.
	return (p_type.kind == TypeKind::Builtin && (p_type.name == "Array" || p_type.name == "Dictionary")) || p_type.kind == TypeKind::Signal;
}

std::optional<Type> element_of(const Type &p_type) {
	if (p_type.kind != TypeKind::Builtin) {
		return std::nullopt;
	}
	if (p_type.name == "Array") {
		return p_type.args.empty() ? Type::of(TypeKind::Variant) : p_type.args[0];
	}
	static const std::unordered_map<std::string, Type> packed = {
		{ "PackedByteArray", Type::of(TypeKind::Int) },
		{ "PackedInt32Array", Type::of(TypeKind::Int) },
		{ "PackedInt64Array", Type::of(TypeKind::Int) },
		{ "PackedFloat32Array", Type::of(TypeKind::Float) },
		{ "PackedFloat64Array", Type::of(TypeKind::Float) },
		{ "PackedStringArray", Type::of(TypeKind::String) },
		{ "PackedVector2Array", Type::builtin("Vector2") },
		{ "PackedVector3Array", Type::builtin("Vector3") },
		{ "PackedColorArray", Type::builtin("Color") },
		{ "PackedVector4Array", Type::builtin("Vector4") },
	};
	const auto found = packed.find(p_type.name);
	return found == packed.end() ? std::nullopt : std::optional<Type>(found->second);
}

Type optional_of(const Type &p_type) {
	if (p_type.kind == TypeKind::Object || p_type.kind == TypeKind::Task) {
		Type type = p_type;
		type.nullable = true;
		return type;
	}
	return Type::option(p_type);
}

} // namespace petrichor::compiler
