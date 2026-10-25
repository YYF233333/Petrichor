#include "adapter/pet_script.h"

#include "adapter/pet_instance.h"
#include "adapter/pet_language.h"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/error_macros.hpp>

using namespace godot;

namespace petrichor {

static Dictionary property_dict(Variant::Type p_type, const StringName &p_name, uint32_t p_usage) {
	Dictionary dict;
	dict["name"] = p_name;
	dict["class_name"] = StringName();
	dict["type"] = p_type;
	dict["hint"] = PROPERTY_HINT_NONE;
	dict["hint_string"] = String();
	dict["usage"] = p_usage;
	return dict;
}

static Dictionary method_dict(const StringName &p_name, const Dictionary &p_return, const Array &p_args = Array(), uint32_t p_flags = METHOD_FLAGS_DEFAULT) {
	Dictionary dict;
	dict["name"] = p_name;
	dict["args"] = p_args;
	dict["default_args"] = Array();
	dict["flags"] = p_flags;
	dict["id"] = 0;
	dict["return"] = p_return;
	return dict;
}

static Dictionary return_dict(Variant::Type p_type) {
	return property_dict(p_type, StringName(), PROPERTY_USAGE_DEFAULT);
}

// Async methods report an untyped result to the engine (spec §9.8).
static Dictionary variant_dict(const StringName &p_name) {
	return property_dict(Variant::NIL, p_name, PROPERTY_USAGE_NIL_IS_VARIANT);
}

PetHeader parse_header(const String &p_source) {
	PetHeader header;
	bool has_base = false;
	const PackedStringArray lines = p_source.split("\n");
	for (int64_t i = 0; i < lines.size(); i++) {
		const PackedStringArray words = lines[i].strip_edges().split(" ", false);
		for (int64_t w = 0; w + 1 < words.size(); w++) {
			if (words[w] == "class_name" && header.class_name.is_empty()) {
				header.class_name = words[w + 1];
			} else if (words[w] == "extends" && !has_base) {
				header.base = words[w + 1];
				has_base = true;
			}
		}
		if (words.size() > 0 && (words[0] == "func" || words[0] == "var")) {
			break;
		}
	}
	return header;
}

void PetScript::_bind_methods() {
	MethodInfo info;
	info.name = "new";
	ClassDB::bind_vararg_method(METHOD_FLAGS_DEFAULT, "new", &PetScript::_new, info);
}

Variant PetScript::_new(const Variant **p_args, GDExtensionInt p_argcount, GDExtensionCallError &r_error) {
	r_error.error = GDEXTENSION_CALL_OK;
	if (p_argcount > 0) {
		r_error.error = GDEXTENSION_CALL_ERROR_TOO_MANY_ARGUMENTS;
		r_error.expected = 0;
		return Variant();
	}
	ERR_FAIL_COND_V_MSG(!valid, Variant(), vformat("%s cannot be constructed: unknown base class '%s'.", get_script_path(), base_type));

	Variant object = ClassDBSingleton::get_singleton()->instantiate(base_type);
	Object *instance = object;
	ERR_FAIL_NULL_V(instance, Variant());
	instance->set_script(Ref<PetScript>(this));
	return object;
}

String PetScript::get_script_path() const {
	return get_path();
}

void PetScript::set_source(const String &p_source) {
	source = p_source;
	parse();
}

void PetScript::parse() {
	const PetHeader header = parse_header(source);
	global_name = header.class_name;
	base_type = header.base;
	valid = ClassDBSingleton::get_singleton()->class_exists(base_type);
}

void PetScript::instance_freed(PetInstance *p_instance) {
	std::lock_guard<std::mutex> lock(mutex);
	instances.erase(p_instance);
}

void PetScript::update_placeholder(void *p_placeholder) const {
	Array properties = _get_script_property_list();
	Dictionary values;
	values[message_name()] = message_default();
	gdextension_interface::placeholder_script_instance_update(p_placeholder, properties._native_ptr(), values._native_ptr());
}

bool PetScript::_editor_can_reload_from_file() {
	return true;
}

void PetScript::_placeholder_erased(void *p_placeholder) {
	std::lock_guard<std::mutex> lock(mutex);
	placeholders.erase(p_placeholder);
}

bool PetScript::_can_instantiate() const {
	return valid && (_is_tool() || !Engine::get_singleton()->is_editor_hint());
}

Ref<Script> PetScript::_get_base_script() const {
	return Ref<Script>();
}

StringName PetScript::_get_global_name() const {
	return global_name;
}

bool PetScript::_inherits_script(const Ref<Script> &p_script) const {
	return p_script.ptr() == this;
}

StringName PetScript::_get_instance_base_type() const {
	return base_type;
}

void *PetScript::_instance_create(Object *p_for_object) const {
	ERR_FAIL_COND_V(!valid, nullptr);
	if (!ClassDBSingleton::get_singleton()->is_parent_class(p_for_object->get_class(), base_type)) {
		ERR_PRINT(vformat("%s extends %s and cannot be attached to %s.", get_script_path(), base_type, p_for_object->get_class()));
		return nullptr;
	}

	PetInstance *instance = memnew(PetInstance);
	instance->owner = p_for_object->_owner;
	instance->script = Ref<PetScript>(const_cast<PetScript *>(this));
	instance->message = message_default();
	{
		std::lock_guard<std::mutex> lock(mutex);
		instances.insert(instance);
	}
	return gdextension_interface::script_instance_create3(&PetInstance::info, instance);
}

void *PetScript::_placeholder_instance_create(Object *p_for_object) const {
	void *placeholder = gdextension_interface::placeholder_script_instance_create(PetLanguage::get_singleton()->_owner, _owner, p_for_object->_owner);
	{
		std::lock_guard<std::mutex> lock(mutex);
		placeholders.insert(placeholder);
	}
	update_placeholder(placeholder);
	return placeholder;
}

bool PetScript::_has_source_code() const {
	return true;
}

String PetScript::_get_source_code() const {
	return source;
}

void PetScript::_set_source_code(const String &p_code) {
	source = p_code;
}

Error PetScript::_reload(bool p_keep_state) {
	parse();
	_update_exports();
	if (!valid) {
		ERR_PRINT(vformat("%s: unknown base class '%s'.", get_script_path(), base_type));
		return ERR_PARSE_ERROR;
	}
	return OK;
}

StringName PetScript::_get_doc_class_name() const {
	return StringName();
}

TypedArray<Dictionary> PetScript::_get_documentation() const {
	return TypedArray<Dictionary>();
}

String PetScript::_get_class_icon_path() const {
	return String();
}

bool PetScript::_has_method(const StringName &p_method) const {
	return !_get_method_info(p_method).is_empty();
}

bool PetScript::_has_static_method(const StringName &p_method) const {
	return p_method == init_name();
}

int64_t PetScript::method_argument_count(const StringName &p_method, bool &r_valid) const {
	const Dictionary info = _get_method_info(p_method);
	r_valid = !info.is_empty();
	return r_valid ? Array(info["args"]).size() : 0;
}

Variant PetScript::_get_script_method_argument_count(const StringName &p_method) const {
	bool valid_method = false;
	const int64_t count = method_argument_count(p_method, valid_method);
	return valid_method ? Variant(count) : Variant();
}

Dictionary PetScript::_get_method_info(const StringName &p_method) const {
	if (p_method == greet_name()) {
		return method_dict(greet_name(), return_dict(Variant::STRING));
	}
	if (p_method == ready_name()) {
		return method_dict(ready_name(), return_dict(Variant::NIL));
	}
	if (p_method == echo_later_name()) {
		Array args;
		args.push_back(variant_dict("value"));
		args.push_back(property_dict(Variant::INT, "frames", PROPERTY_USAGE_DEFAULT));
		return method_dict(echo_later_name(), variant_dict(StringName()), args);
	}
	if (p_method == init_name()) {
		// Reported as a static method returning the class, which is how GDScript
		// types `new` (spec §10.1).
		Dictionary result = return_dict(Variant::OBJECT);
		result["class_name"] = global_name.is_empty() ? base_type : global_name;
		return method_dict(init_name(), result, Array(), METHOD_FLAGS_DEFAULT | METHOD_FLAG_STATIC);
	}
	return Dictionary();
}

bool PetScript::_is_tool() const {
	return false;
}

bool PetScript::_is_valid() const {
	return valid;
}

bool PetScript::_is_abstract() const {
	return false;
}

ScriptLanguage *PetScript::_get_language() const {
	return PetLanguage::get_singleton();
}

bool PetScript::_has_script_signal(const StringName &p_signal) const {
	return false;
}

TypedArray<Dictionary> PetScript::_get_script_signal_list() const {
	return TypedArray<Dictionary>();
}

bool PetScript::_has_property_default_value(const StringName &p_property) const {
	return p_property == message_name();
}

Variant PetScript::_get_property_default_value(const StringName &p_property) const {
	if (p_property == message_name()) {
		return message_default();
	}
	return Variant();
}

void PetScript::_update_exports() {
	Vector<void *> targets;
	{
		std::lock_guard<std::mutex> lock(mutex);
		for (void *placeholder : placeholders) {
			targets.push_back(placeholder);
		}
	}
	for (void *placeholder : targets) {
		update_placeholder(placeholder);
	}
}

TypedArray<Dictionary> PetScript::_get_script_method_list() const {
	TypedArray<Dictionary> list;
	list.push_back(_get_method_info(greet_name()));
	list.push_back(_get_method_info(echo_later_name()));
	list.push_back(_get_method_info(ready_name()));
	return list;
}

TypedArray<Dictionary> PetScript::_get_script_property_list() const {
	TypedArray<Dictionary> list;
	list.push_back(property_dict(Variant::STRING, message_name(), PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_SCRIPT_VARIABLE));
	return list;
}

int32_t PetScript::_get_member_line(const StringName &p_member) const {
	return -1;
}

Dictionary PetScript::_get_constants() const {
	return Dictionary();
}

TypedArray<StringName> PetScript::_get_members() const {
	TypedArray<StringName> members;
	members.push_back(message_name());
	return members;
}

bool PetScript::_is_placeholder_fallback_enabled() const {
	return false;
}

Variant PetScript::_get_rpc_config() const {
	return Dictionary();
}

bool PetScript::_instance_has(Object *p_object) const {
	std::lock_guard<std::mutex> lock(mutex);
	for (const PetInstance *instance : instances) {
		if (instance->owner == p_object->_owner) {
			return true;
		}
	}
	return false;
}

} // namespace petrichor
