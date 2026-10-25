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
		case TypeKind::Function:
			return "func" + signature(args, 1) + (args.empty() || args[0].kind == TypeKind::Void ? std::string() : " -> " + args[0].to_string());
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

} // namespace petrichor::compiler
