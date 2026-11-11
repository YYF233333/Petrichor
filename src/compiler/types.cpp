#include "compiler/types.h"

#include "bytecode/module.h"
#include "compiler/builtin_api.h"

#include <algorithm>
#include <mutex>
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
			if (!args.empty() && args[0].kind == TypeKind::Function) {
				return "(" + args[0].to_string() + ")?"; // As written (spec §2.4).
			}
			return (args.empty() ? std::string("<error>") : args[0].to_string()) + "?";
		case TypeKind::Task:
			return "Task[" + args[0].to_string() + "]" + (nullable ? "?" : "");
		case TypeKind::Function:
			return std::string(is_async ? "async func" : "func") + signature(args, 1) + (args.empty() || args[0].kind == TypeKind::Void ? std::string() : " -> " + args[0].to_string());
		case TypeKind::Signal:
			return "Signal" + signature(args, 0);
		case TypeKind::Builtin:
		case TypeKind::PayloadEnum:
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
		case TypeKind::Enum:
			return name;
		default:
			return builtin_name();
	}
}

Type Type::result(const Type &p_value, const Type &p_error) {
	auto values = std::make_shared<EnumValues>();
	values->list = { { "Ok", 0 }, { "Err", 1 } };
	values->fields = { { { "value", p_value } }, { { "error", p_error } } };
	Type type = enumeration("Result", std::string(), std::move(values), TypeKind::PayloadEnum);
	type.args = { p_value, p_error };
	return type;
}

Type Type::settled(const Type &p_value) {
	auto values = std::make_shared<EnumValues>();
	values->list = { { "Finished", 0 }, { "Cancelled", 1 } };
	values->fields = { {}, { { "reason", cancel_reason() } } };
	if (p_value.kind != TypeKind::Void) {
		values->fields[0].push_back({ "value", p_value });
	}
	Type type = enumeration("Settled", std::string(), std::move(values), TypeKind::PayloadEnum);
	type.args = { p_value };
	return type;
}

Type Type::cancel_reason() {
	static const std::shared_ptr<const EnumValues> values = [] {
		auto list = std::make_shared<EnumValues>();
		list->list = { { "REQUESTED", 0 }, { "OWNER_FREED", 1 }, { "AWAITED_FREED", 2 } };
		return list;
	}();
	return enumeration("CancelReason", std::string(), values);
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
	if (p_name.rfind("enum::", 0) == 0) {
		if (const std::optional<Type> found = builtin_enum(p_name.substr(6))) {
			return *found;
		}
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

Type engine_enum(const std::string &p_name, const std::function<std::vector<std::pair<std::string, int64_t>>()> &p_values) {
	static std::mutex mutex;
	static std::unordered_map<std::string, std::shared_ptr<const EnumValues>> known;
	std::lock_guard<std::mutex> lock(mutex);
	std::shared_ptr<const EnumValues> &values = known[p_name];
	if (!values) {
		auto made = std::make_shared<EnumValues>();
		made->list = p_values();
		made->is_engine = true;
		values = made;
	}
	return Type::enumeration(p_name, std::string(), values);
}

std::optional<Type> builtin_enum(const std::string &p_name) {
	const builtin::Enum *found = builtin::find_enum(p_name);
	if (!found || found->is_bitfield) {
		return std::nullopt;
	}
	return engine_enum(p_name, [found]() {
		std::vector<std::pair<std::string, int64_t>> values;
		for (uint32_t i = 0; i < found->value_count; i++) {
			values.push_back({ found->values[i].name, found->values[i].value });
		}
		return values;
	});
}

static bool is_string_like(const Type &p_type) {
	return p_type.kind == TypeKind::String || (p_type.kind == TypeKind::Builtin && (p_type.name == "StringName" || p_type.name == "NodePath"));
}

Conversion conversion(const Type &p_from, const Type &p_to, const ClassRelation &p_classes) {
	using C = Conversion;
	const auto allowed = [](bool p_allowed) { return p_allowed ? C::Same : C::None; };
	if (p_from.is_error() || p_to.is_error() || p_from == p_to) {
		return C::Same;
	}
	if (p_from.kind == TypeKind::Task && p_to.kind == TypeKind::Task) {
		return allowed(p_from.args == p_to.args && (!p_from.nullable || p_to.nullable));
	}
	if (p_from.kind == TypeKind::None) {
		return p_to.is_optional() || p_to.kind == TypeKind::Variant ? C::Nothing : C::None;
	}
	// An enum's value is an int, but an int is one of its values only explicitly (spec §3.7).
	if ((p_from.kind == TypeKind::Int || p_from.kind == TypeKind::Enum) && p_to.kind == TypeKind::Float) {
		return C::IntToFloat;
	}
	if (p_from.kind == TypeKind::Enum && p_to.kind == TypeKind::Int) {
		return C::Same;
	}
	// Converted where needed, as in GDScript (spec §10.7).
	if (is_string_like(p_from) && is_string_like(p_to) && (p_from.kind == TypeKind::String || p_to.kind == TypeKind::String)) {
		return C::Construct;
	}
	// The integer vectors and Rect2i widen to their float counterparts (spec §3.14).
	if (p_from.kind == TypeKind::Builtin && p_to.kind == TypeKind::Builtin && p_from.name == p_to.name + "i" && (p_to.name == "Vector2" || p_to.name == "Vector3" || p_to.name == "Vector4" || p_to.name == "Rect2")) {
		return C::Construct;
	}
	if (p_from.kind == TypeKind::Object && p_to.kind == TypeKind::Object) {
		// PackedScene[T] is covariant in T, and a scene of unknown root is
		// not one (spec §3.14, §7.4).
		const bool scene = p_to.args.empty() || (!p_from.args.empty() && p_classes.is_a(p_from.args[0], p_to.args[0]));
		return allowed(scene && (p_to.nullable || !p_from.nullable) && p_classes.is_a(p_from, p_to));
	}
	if (p_to.kind == TypeKind::Option) {
		// A T is wrapped implicitly, as many times as T?? takes; the T inside a
		// T? does not convert (spec §3.14).
		const C inner = conversion(p_from, p_to.args[0], p_classes);
		return inner == C::None || (p_from.kind == TypeKind::Option && inner != C::Same && inner != C::Wrap) ? C::None : C::Wrap;
	}
	if (p_from.kind == TypeKind::Function && p_to.kind == TypeKind::Function) {
		// Parameters contravariant, the result covariant (spec §3.14).
		if (p_from.is_async != p_to.is_async || p_from.args.size() != p_to.args.size()) {
			return C::None;
		}
		for (size_t i = 1; i < p_to.args.size(); i++) {
			if (!is_assignable(p_to.args[i], p_from.args[i], p_classes)) {
				return C::None;
			}
		}
		return allowed(p_to.args[0].kind == TypeKind::Void ? p_from.args[0].kind == TypeKind::Void : is_assignable(p_from.args[0], p_to.args[0], p_classes));
	}
	// The engine takes them as its Callable and Signal. A function value's
	// Callable is called as a pub function is (spec §10.1).
	const bool called_encoded = p_from.kind == TypeKind::Function && std::any_of(p_from.args.begin(), p_from.args.end(), [&](const Type &p_part) { return lost_in_encoding(p_part, p_classes).has_value(); });
	if ((p_from.kind == TypeKind::Function || p_from.kind == TypeKind::Signal) && p_to.kind == TypeKind::Builtin && p_to.name == p_from.builtin_name()) {
		return allowed(!called_encoded);
	}
	// A typed container is the same container through an untyped view (spec
	// §3.14), unless it keeps boxes, which leave only as an encoded copy.
	if (is_typed_container(p_from) && !is_boxed(p_from) && p_to.kind == TypeKind::Builtin && p_to.args.empty() && p_to.name == p_from.name) {
		return C::Same;
	}
	return p_to.kind == TypeKind::Variant && p_from.is_storable() && !(is_typed_container(p_from) && is_boxed(p_from)) && !lost_in_encoding(p_from, p_classes) && !called_encoded ? C::ToVariant : C::None;
}

static std::optional<Type> lost_in_encoding(const Type &p_type, const ClassRelation &p_classes, bool p_copied, std::vector<Type> &r_seen) {
	if (p_type.kind == TypeKind::Option && (p_type.args[0].is_optional() || p_type.args[0].kind == TypeKind::Variant)) {
		return p_type;
	}
	std::vector<Type> parts;
	if (p_type.kind == TypeKind::Option || (p_copied && is_typed_container(p_type))) {
		parts = p_type.args;
	} else if (p_type.kind == TypeKind::Struct || p_type.kind == TypeKind::PayloadEnum) {
		// A struct may hold itself through a container (spec §3.6).
		if (std::find(r_seen.begin(), r_seen.end(), p_type) != r_seen.end()) {
			return std::nullopt;
		}
		r_seen.push_back(p_type);
		p_copied = true;
		if (p_type.kind == TypeKind::Struct) {
			parts = p_classes.fields(p_type);
		}
		for (size_t v = 0; p_type.kind == TypeKind::PayloadEnum && p_type.values && v < p_type.values->fields.size(); v++) {
			for (const auto &[name, type] : p_type.values->fields[v]) {
				parts.push_back(type);
			}
		}
	}
	for (const Type &part : parts) {
		if (std::optional<Type> lost = lost_in_encoding(part, p_classes, p_copied, r_seen)) {
			return lost;
		}
	}
	return std::nullopt;
}

std::optional<Type> lost_in_encoding(const Type &p_type, const ClassRelation &p_classes, bool p_copied) {
	std::vector<Type> seen;
	return lost_in_encoding(p_type, p_classes, p_copied, seen);
}

bool is_assignable(const Type &p_from, const Type &p_to, const ClassRelation &p_classes) {
	return conversion(p_from, p_to, p_classes) != Conversion::None;
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
	const bytecode::ValueType elem = bytecode::packed_element(bytecode::value_type_from_name(p_type.name));
	if (elem == bytecode::ValueType::Void) {
		return std::nullopt;
	}
	return elem == bytecode::ValueType::Int ? Type::of(TypeKind::Int) : (elem == bytecode::ValueType::Float ? Type::of(TypeKind::Float) : (elem == bytecode::ValueType::String ? Type::of(TypeKind::String) : Type::builtin(bytecode::value_type_name(elem))));
}

std::optional<std::pair<Type, Type>> entries_of(const Type &p_type) {
	if (p_type.kind != TypeKind::Builtin || p_type.name != "Dictionary") {
		return std::nullopt;
	}
	if (p_type.args.size() != 2) {
		return std::make_pair(Type::of(TypeKind::Variant), Type::of(TypeKind::Variant));
	}
	return std::make_pair(p_type.args[0], p_type.args[1]);
}

bool is_typed_container(const Type &p_type) {
	return p_type.kind == TypeKind::Builtin && (p_type.name == "Array" || p_type.name == "Dictionary") && !p_type.args.empty();
}

bool is_format_string(const Type &p_type) {
	return p_type.kind == TypeKind::String || (p_type.kind == TypeKind::Builtin && p_type.name == "StringName");
}

bool is_boxed(const Type &p_type) {
	switch (p_type.kind) {
		case TypeKind::Option:
		case TypeKind::Struct:
		case TypeKind::PayloadEnum:
		case TypeKind::Function:
		case TypeKind::Task:
			return true;
		default:
			break;
	}
	return is_typed_container(p_type) && std::any_of(p_type.args.begin(), p_type.args.end(), is_boxed);
}

Type optional_of(const Type &p_type) {
	if ((p_type.kind == TypeKind::Object || p_type.kind == TypeKind::Task) && !p_type.nullable) {
		Type type = p_type;
		type.nullable = true;
		return type;
	}
	return Type::option(p_type);
}

} // namespace petrichor::compiler
