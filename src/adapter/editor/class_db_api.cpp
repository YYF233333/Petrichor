#include "adapter/editor/class_db_api.h"

#include "compiler/builtin_api.h"
#include "runtime/text.h"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/templates/hashfuncs.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace petrichor {

using compiler::EngineMethod;
using compiler::EngineParam;
using compiler::EngineProperty;
using compiler::Type;
using compiler::TypeKind;

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
			return Type::object(to_utf8(class_name.is_empty() ? String("Object") : class_name), true);
		}
		case Variant::ARRAY:
			// A typed array, such as the Array[Node] of get_children() (spec §3.8).
			if (int(p_property.get("hint", 0)) == PROPERTY_HINT_ARRAY_TYPE) {
				return compiler::api_type("typedarray::" + to_utf8(p_property.get("hint_string", String())), true);
			}
			return Type::builtin("Array");
		case Variant::INT:
			// An enum, as "Node.ProcessMode" or "Key"; a bitfield stays int (spec §3.7).
			if (usage & PROPERTY_USAGE_CLASS_IS_ENUM) {
				const String name = p_property.get("class_name", String());
				const int64_t dot = name.find(".");
				const std::optional<Type> found = dot > 0 && ClassDBSingleton::get_singleton()->class_exists(name.substr(0, dot)) ? ClassDBApi::get().find_enum(to_utf8(name.substr(0, dot)), to_utf8(name.substr(dot + 1))) : compiler::builtin_enum(to_utf8(name));
				if (found) {
					return *found;
				}
			}
			return Type::of(TypeKind::Int);
		default:
			// The names ClassDB gives value types are the extension API's names.
			return compiler::api_type(to_utf8(Variant::get_type_name(type)), true);
	}
}

EngineMethod method_of(const Dictionary &p_info, const std::string &p_owner) {
	EngineMethod method;
	method.name = to_utf8(p_info.get("name", String()));
	method.owner = p_owner;
	const uint32_t flags = uint32_t(int(p_info.get("flags", 0)));
	method.is_virtual = flags & (METHOD_FLAG_VIRTUAL | METHOD_FLAG_VIRTUAL_REQUIRED);
	method.is_static = flags & METHOD_FLAG_STATIC;
	method.is_vararg = flags & METHOD_FLAG_VARARG;
	const Array args = p_info.get("args", Array());
	for (int64_t a = 0; a < args.size(); a++) {
		const Dictionary arg = args[a];
		method.params.push_back({ to_utf8(arg.get("name", String())), type_of(arg, false) });
	}
	const Array defaults = p_info.get("default_args", Array());
	method.default_count = int(defaults.size());
	for (int64_t d = 0; d < defaults.size() && d < int64_t(method.params.size()); d++) {
		method.params[method.params.size() - defaults.size() + d].default_value = to_utf8(UtilityFunctions::var_to_str(defaults[d]));
	}
	method.return_type = type_of(p_info.get("return", Dictionary()), true);
	return method;
}

// The hash the engine finds a MethodBind by, from what ClassDB tells of it
// (core/object/method_info.cpp:87-116, core/object/class_db.cpp:955-974). A
// change in the engine's hash makes every native call fail to load.
static uint32_t method_hash(const Dictionary &p_info) {
	const Dictionary result = p_info.get("return", Dictionary());
	const Array args = p_info.get("args", Array());
	const Array defaults = p_info.get("default_args", Array());
	const uint32_t flags = uint32_t(int(p_info.get("flags", 0)));
	const bool has_return = int(result.get("type", 0)) != Variant::NIL || (uint32_t(int(result.get("usage", 0))) & PROPERTY_USAGE_NIL_IS_VARIANT);
	uint32_t hash = hash_murmur3_one_32(has_return);
	hash = hash_murmur3_one_32(uint32_t(args.size()), hash);
	const auto add_type = [&](const Dictionary &p_property) {
		hash = hash_murmur3_one_32(uint32_t(int(p_property.get("type", 0))), hash);
		const StringName class_name = p_property.get("class_name", StringName());
		if (class_name != StringName()) {
			hash = hash_murmur3_one_32(uint32_t(class_name.hash()), hash);
		}
	};
	if (has_return) {
		add_type(result);
	}
	for (int64_t a = 0; a < args.size(); a++) {
		add_type(args[a]);
	}
	hash = hash_murmur3_one_32(uint32_t(defaults.size()), hash);
	for (int64_t d = 0; d < defaults.size(); d++) {
		hash = hash_murmur3_one_32(defaults[d].hash(), hash);
	}
	hash = hash_murmur3_one_32(flags & METHOD_FLAG_CONST ? 1 : 0, hash);
	hash = hash_murmur3_one_32(flags & METHOD_FLAG_VARARG ? 1 : 0, hash);
	return hash_fmix32(hash);
}

const ClassDBApi &ClassDBApi::get() {
	static const ClassDBApi api;
	return api;
}

void ClassDBApi::extensions_changed() {
	const ClassDBApi &api = get();
	std::lock_guard<std::mutex> lock(api.mutex);
	api.classes.clear();
}

std::shared_ptr<const ClassDBApi::Members> ClassDBApi::members(const std::string &p_class) const {
	{
		std::lock_guard<std::mutex> lock(mutex);
		const auto found = classes.find(p_class);
		if (found != classes.end()) {
			return found->second;
		}
	}
	ClassDBSingleton *db = ClassDBSingleton::get_singleton();
	const StringName cls = to_godot(p_class);
	if (p_class.empty() || !db->class_exists(cls)) {
		return nullptr;
	}
	// Built without the lock: the types of members look up enums.
	const std::shared_ptr<Members> result = std::make_shared<Members>();
	result->parent = to_utf8(db->get_parent_class(cls));
	const TypedArray<Dictionary> methods = db->class_get_method_list(cls, true);
	for (int64_t i = 0; i < methods.size(); i++) {
		const Dictionary info = methods[i];
		EngineMethod method = method_of(info, p_class);
		if (db->class_has_method(cls, info.get("name", StringName()), true)) {
			method.hash = method_hash(info);
		}
		result->methods.emplace(method.name, method);
	}
	const TypedArray<Dictionary> properties = db->class_get_property_list(cls, true);
	for (int64_t i = 0; i < properties.size(); i++) {
		const Dictionary info = properties[i];
		const uint32_t usage = uint32_t(int(info.get("usage", 0)));
		if (usage & (PROPERTY_USAGE_GROUP | PROPERTY_USAGE_SUBGROUP | PROPERTY_USAGE_CATEGORY)) {
			continue;
		}
		const String name = info.get("name", String());
		EngineProperty property;
		property.name = to_utf8(name);
		property.owner = p_class;
		// Of its getter's type, as GDScript (modules/gdscript/gdscript_analyzer.cpp:4343-4347):
		// the property list gives a resource's hint as its class, such as
		// "CanvasItemMaterial,ShaderMaterial" (core/object/property_info.h:152-153,
		// scene/main/canvas_item.cpp:1601), and enums without their names
		// (scene/main/node.cpp:4046).
		const auto accessor = [&](const StringName &p_method) -> std::optional<EngineMethod> {
			const std::string method = to_utf8(p_method);
			if (method.empty()) {
				return std::nullopt;
			}
			const auto own = result->methods.find(method);
			return own != result->methods.end() ? own->second : find_method(result->parent, method);
		};
		const std::optional<EngineMethod> read = accessor(db->class_get_property_getter(cls, name));
		const std::optional<EngineMethod> write = accessor(db->class_get_property_setter(cls, name));
		property.type = read ? read->return_type : type_of(info, false); // A property is never void.
		property.writable = write.has_value();
		// The index of a family of another extension's class is unknown here, and
		// such a property is read and written by name.
		property.index = compiler::builtin::property_index(p_class, property.name);
		const size_t indexed = property.index >= 0 ? 1 : 0;
		if (read && read->hash && read->params.size() == indexed) {
			property.getter = read;
		}
		if (write && write->hash && write->params.size() == indexed + 1) {
			property.setter = write;
		}
		result->properties.emplace(property.name, property);
	}
	std::lock_guard<std::mutex> lock(mutex);
	return classes.emplace(p_class, result).first->second;
}

bool ClassDBApi::class_exists(const std::string &p_class) const {
	return ClassDBSingleton::get_singleton()->class_exists(to_godot(p_class));
}

std::string ClassDBApi::parent_class(const std::string &p_class) const {
	return to_utf8(ClassDBSingleton::get_singleton()->get_parent_class(to_godot(p_class)));
}

std::optional<EngineMethod> ClassDBApi::find_method(const std::string &p_class, const std::string &p_method) const {
	for (std::shared_ptr<const Members> cls = members(p_class); cls; cls = members(cls->parent)) {
		const auto found = cls->methods.find(p_method);
		if (found != cls->methods.end()) {
			return found->second;
		}
	}
	return std::nullopt;
}

std::optional<EngineMethod> ClassDBApi::find_signal(const std::string &p_class, const std::string &p_signal) const {
	ClassDBSingleton *db = ClassDBSingleton::get_singleton();
	if (!db->class_exists(to_godot(p_class))) {
		return std::nullopt;
	}
	// ClassDB looks through the ancestors, and so for the one that declares it.
	const StringName signal = to_godot(p_signal);
	StringName owner = to_godot(p_class);
	const Dictionary info = db->class_get_signal(owner, signal);
	if (info.is_empty()) {
		return std::nullopt;
	}
	for (StringName up = db->get_parent_class(owner); up != StringName() && db->class_has_signal(up, signal); up = db->get_parent_class(up)) {
		owner = up;
	}
	return method_of(info, to_utf8(owner));
}

bool ClassDBApi::can_instantiate(const std::string &p_class) const {
	return ClassDBSingleton::get_singleton()->can_instantiate(to_godot(p_class));
}

std::optional<int64_t> ClassDBApi::find_constant(const std::string &p_class, const std::string &p_constant) const {
	ClassDBSingleton *db = ClassDBSingleton::get_singleton();
	const StringName cls = to_godot(p_class);
	const StringName name = to_godot(p_constant);
	if (!db->class_exists(cls) || !db->class_has_integer_constant(cls, name)) {
		return std::nullopt;
	}
	return db->class_get_integer_constant(cls, name);
}

std::optional<Type> ClassDBApi::find_enum(const std::string &p_class, const std::string &p_enum) const {
	ClassDBSingleton *db = ClassDBSingleton::get_singleton();
	const StringName name = to_godot(p_enum);
	for (StringName cls = to_godot(p_class); cls != StringName(); cls = db->get_parent_class(cls)) {
		if (!db->class_get_enum_list(cls, true).has(String(name))) {
			continue;
		}
		if (db->is_class_enum_bitfield(cls, name, true)) {
			return std::nullopt;
		}
		return compiler::engine_enum(to_utf8(cls) + "." + p_enum, [&]() {
			std::vector<std::pair<std::string, int64_t>> values;
			const PackedStringArray constants = db->class_get_enum_constants(cls, name, true);
			for (const String &constant : constants) {
				values.push_back({ to_utf8(constant), db->class_get_integer_constant(cls, constant) });
			}
			return values;
		});
	}
	return std::nullopt;
}

Type ClassDBApi::constant_type(const std::string &p_class, const std::string &p_constant) const {
	const StringName enum_name = ClassDBSingleton::get_singleton()->class_get_integer_constant_enum(to_godot(p_class), to_godot(p_constant), false);
	const std::optional<Type> found = enum_name == StringName() ? std::nullopt : find_enum(p_class, to_utf8(enum_name));
	return found ? *found : Type::of(TypeKind::Int);
}

std::vector<std::string> ClassDBApi::class_list() const {
	std::vector<std::string> names;
	for (const String &name : ClassDBSingleton::get_singleton()->get_class_list()) {
		names.push_back(to_utf8(name));
	}
	return names;
}

std::vector<std::string> ClassDBApi::member_names(const std::string &p_class) const {
	std::vector<std::string> names;
	for (std::shared_ptr<const Members> cls = members(p_class); cls; cls = members(cls->parent)) {
		for (const auto &method : cls->methods) {
			names.push_back(method.first);
		}
		for (const auto &property : cls->properties) {
			names.push_back(property.first);
		}
	}
	ClassDBSingleton *db = ClassDBSingleton::get_singleton();
	const StringName cls = to_godot(p_class);
	if (db->class_exists(cls)) {
		const TypedArray<Dictionary> signals = db->class_get_signal_list(cls, false);
		for (int64_t i = 0; i < signals.size(); i++) {
			names.push_back(to_utf8(Dictionary(signals[i]).get("name", String())));
		}
		for (const String &constant : db->class_get_integer_constant_list(cls, false)) {
			names.push_back(to_utf8(constant));
		}
	}
	return names;
}

std::vector<std::string> ClassDBApi::enum_names(const std::string &p_class) const {
	std::vector<std::string> names;
	ClassDBSingleton *db = ClassDBSingleton::get_singleton();
	const StringName cls = to_godot(p_class);
	if (db->class_exists(cls)) {
		for (const String &name : db->class_get_enum_list(cls, false)) {
			names.push_back(to_utf8(name));
		}
	}
	return names;
}

bool ClassDBApi::is_singleton(const std::string &p_class) const {
	return Engine::get_singleton()->has_singleton(to_godot(p_class));
}

std::optional<EngineProperty> ClassDBApi::find_property(const std::string &p_class, const std::string &p_property) const {
	for (std::shared_ptr<const Members> cls = members(p_class); cls; cls = members(cls->parent)) {
		const auto found = cls->properties.find(p_property);
		if (found != cls->properties.end()) {
			return found->second;
		}
	}
	return std::nullopt;
}

} // namespace petrichor
