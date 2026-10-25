#include "compiler/types.h"

#include "compiler/builtin_api.h"

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
		default:
			return std::string();
	}
}

std::string Type::to_string() const {
	switch (kind) {
		case TypeKind::Error:
			return "<error>";
		case TypeKind::Void:
			return "void";
		case TypeKind::Variant:
			return "Variant";
		case TypeKind::Object:
			return nullable ? name + "?" : name;
		case TypeKind::Option:
			return (args.empty() ? std::string("<error>") : args[0].to_string()) + "?";
		case TypeKind::Unsupported:
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
		return Type::builtin("Array"); // Element types come with containers (spec §3.8).
	}
	if (builtin::find_class(p_name)) {
		return Type::builtin(p_name);
	}
	if (p_name.find("::") != std::string::npos || p_name.find('*') != std::string::npos) {
		return Type{ TypeKind::Unsupported, p_name, false };
	}
	return Type::object(p_name, p_object_nullable);
}

} // namespace petrichor::compiler
