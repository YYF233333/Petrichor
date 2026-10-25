#include "adapter/editor/class_db_api.h"

#include "runtime/class_image.h"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/variant/typed_array.hpp>

using namespace godot;

namespace petrichor {

using compiler::EngineMethod;
using compiler::EngineParam;
using compiler::EngineProperty;
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

// Objects the engine hands out are nullable until audited (spec §10.2).
Type type_of(const Dictionary &p_property, bool p_is_return) {
	const Variant::Type type = Variant::Type(int(p_property.get("type", Variant::NIL)));
	const uint32_t usage = uint32_t(int(p_property.get("usage", 0)));
	switch (type) {
		case Variant::NIL:
			if (usage & PROPERTY_USAGE_NIL_IS_VARIANT) {
				return Type::of(TypeKind::Variant);
			}
			return Type::of(p_is_return ? TypeKind::Void : TypeKind::Variant);
		case Variant::OBJECT: {
			const String class_name = p_property.get("class_name", String());
			return Type::object(to_std(class_name.is_empty() ? String("Object") : class_name), true);
		}
		case Variant::ARRAY:
			// A typed array, such as the Array[Node] of get_children() (spec §3.8).
			if (int(p_property.get("hint", 0)) == PROPERTY_HINT_ARRAY_TYPE) {
				return compiler::api_type("typedarray::" + to_std(p_property.get("hint_string", String())), true);
			}
			return Type::builtin("Array");
		default:
			// The names ClassDB gives value types are the extension API's names.
			return compiler::api_type(to_std(Variant::get_type_name(type)), true);
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

std::optional<EngineMethod> ClassDBApi::find_signal(const std::string &p_class, const std::string &p_signal) const {
	ClassDBSingleton *db = ClassDBSingleton::get_singleton();
	if (!db->class_exists(to_godot(p_class))) {
		return std::nullopt;
	}
	// ClassDB looks through the ancestors.
	const Dictionary info = db->class_get_signal(to_godot(p_class), to_godot(p_signal));
	if (info.is_empty()) {
		return std::nullopt;
	}
	EngineMethod signal;
	signal.name = p_signal;
	const Array args = info.get("args", Array());
	for (int64_t a = 0; a < args.size(); a++) {
		const Dictionary arg = args[a];
		signal.params.push_back({ to_std(arg.get("name", String())), type_of(arg, false) });
	}
	return signal;
}

bool ClassDBApi::can_instantiate(const std::string &p_class) const {
	return ClassDBSingleton::get_singleton()->can_instantiate(to_godot(p_class));
}

bool ClassDBApi::is_singleton(const std::string &p_class) const {
	return Engine::get_singleton()->has_singleton(to_godot(p_class));
}

std::optional<EngineProperty> ClassDBApi::find_property(const std::string &p_class, const std::string &p_property) const {
	ClassDBSingleton *db = ClassDBSingleton::get_singleton();
	const StringName property_name = to_godot(p_property);
	for (StringName cls = to_godot(p_class); cls != StringName(); cls = db->get_parent_class(cls)) {
		const TypedArray<Dictionary> properties = db->class_get_property_list(cls, true);
		for (int64_t i = 0; i < properties.size(); i++) {
			const Dictionary info = properties[i];
			const uint32_t usage = uint32_t(int(info.get("usage", 0)));
			if (usage & (PROPERTY_USAGE_GROUP | PROPERTY_USAGE_SUBGROUP | PROPERTY_USAGE_CATEGORY)) {
				continue;
			}
			if (StringName(info.get("name", String())) != property_name) {
				continue;
			}
			EngineProperty property;
			property.name = p_property;
			property.type = type_of(info, false); // A property is never void.
			property.writable = db->class_get_property_setter(cls, property_name) != StringName();
			return property;
		}
	}
	return std::nullopt;
}

} // namespace petrichor
