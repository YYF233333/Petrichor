#include "adapter/editor/class_db_api.h"

#include "runtime/class_image.h"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/variant/typed_array.hpp>

using namespace godot;

namespace petrichor {

using compiler::EngineMethod;
using compiler::EngineParam;
using compiler::Type;
using compiler::TypeKind;

namespace {

std::string to_std(const String &p_text) {
	const CharString utf8 = p_text.utf8();
	return std::string(utf8.get_data(), size_t(utf8.length()));
}

String to_godot(const std::string &p_text) {
	return String::utf8(p_text.c_str(), int64_t(p_text.size()));
}

Type type_of(const Dictionary &p_property, bool p_is_return) {
	const Variant::Type type = Variant::Type(int(p_property.get("type", Variant::NIL)));
	const uint32_t usage = uint32_t(int(p_property.get("usage", 0)));
	switch (type) {
		case Variant::NIL:
			if (usage & PROPERTY_USAGE_NIL_IS_VARIANT) {
				return Type::of(TypeKind::Variant);
			}
			return Type::of(p_is_return ? TypeKind::Void : TypeKind::Variant);
		case Variant::BOOL:
			return Type::of(TypeKind::Bool);
		case Variant::INT:
			return Type::of(TypeKind::Int);
		case Variant::FLOAT:
			return Type::of(TypeKind::Float);
		case Variant::STRING:
			return Type::of(TypeKind::String);
		case Variant::OBJECT: {
			String class_name = p_property.get("class_name", String());
			return Type{ TypeKind::Object, to_std(class_name.is_empty() ? String("Object") : class_name) };
		}
		default:
			return Type{ TypeKind::Unsupported, to_std(Variant::get_type_name(type)) };
	}
}

} // namespace

const ClassDBApi &ClassDBApi::get() {
	static const ClassDBApi api;
	return api;
}

bool ClassDBApi::class_exists(const std::string &p_class) const {
	return ClassDBSingleton::get_singleton()->class_exists(to_godot(p_class));
}

std::string ClassDBApi::parent_class(const std::string &p_class) const {
	return to_std(ClassDBSingleton::get_singleton()->get_parent_class(to_godot(p_class)));
}

std::optional<EngineMethod> ClassDBApi::find_method(const std::string &p_class, const std::string &p_method) const {
	ClassDBSingleton *db = ClassDBSingleton::get_singleton();
	const StringName method_name = to_godot(p_method);
	for (StringName cls = to_godot(p_class); cls != StringName(); cls = db->get_parent_class(cls)) {
		const TypedArray<Dictionary> methods = db->class_get_method_list(cls, true);
		for (int64_t i = 0; i < methods.size(); i++) {
			const Dictionary info = methods[i];
			if (StringName(info.get("name", String())) != method_name) {
				continue;
			}
			EngineMethod method;
			method.name = p_method;
			const uint32_t flags = uint32_t(int(info.get("flags", 0)));
			method.is_virtual = flags & (METHOD_FLAG_VIRTUAL | METHOD_FLAG_VIRTUAL_REQUIRED);
			method.is_static = flags & METHOD_FLAG_STATIC;
			method.is_vararg = flags & METHOD_FLAG_VARARG;
			const Array args = info.get("args", Array());
			for (int64_t a = 0; a < args.size(); a++) {
				const Dictionary arg = args[a];
				method.params.push_back({ to_std(arg.get("name", String())), type_of(arg, false) });
			}
			method.default_count = int(Array(info.get("default_args", Array())).size());
			method.return_type = type_of(info.get("return", Dictionary()), true);
			return method;
		}
	}
	return std::nullopt;
}

std::optional<EngineMethod> ClassDBApi::find_utility(const std::string &p_function) const {
	for (const auto &entry : utility_functions()) {
		if (p_function == entry.first) {
			EngineMethod method;
			method.name = p_function;
			method.is_vararg = true;
			return method;
		}
	}
	return std::nullopt;
}

} // namespace petrichor
