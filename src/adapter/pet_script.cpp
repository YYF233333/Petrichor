#include "adapter/pet_script.h"

#include "adapter/pet_compiled.h"
#include "adapter/pet_instance.h"
#include "adapter/pet_language.h"

#ifdef TOOLS_ENABLED
#include "adapter/editor/script_compiler.h"
#endif
#ifdef PETRICHOR_PROBES
#include "probe/probe_methods.h"
#endif

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/error_macros.hpp>

using namespace godot;

namespace petrichor {

using bytecode::ValueType;

static StringName init_name() {
	return "_init";
}

static Dictionary property_dict(Variant::Type p_type, const StringName &p_name, uint32_t p_usage, const StringName &p_class_name = StringName()) {
	Dictionary dict;
	dict["name"] = p_name;
	dict["class_name"] = p_class_name;
	dict["type"] = p_type;
	dict["hint"] = PROPERTY_HINT_NONE;
	dict["hint_string"] = String();
	dict["usage"] = p_usage;
	return dict;
}

static Dictionary method_dict(const StringName &p_name, const Array &p_args, const Dictionary &p_return, uint32_t p_flags) {
	Dictionary dict;
	dict["name"] = p_name;
	dict["args"] = p_args;
	dict["default_args"] = Array();
	dict["flags"] = p_flags;
	dict["id"] = 0;
	dict["return"] = p_return;
	return dict;
}

// An object of the class as the engine and GDScript see it: T and T? alike
// (spec §10.1).
static StringName class_of(bytecode::ValueType p_type, const std::string &p_class) {
	return p_type == bytecode::ValueType::Object ? StringName(String::utf8(p_class.c_str())) : StringName();
}

// A Variant slot, such as int?, is NIL to the engine, which reads that as
// void unless told otherwise.
static uint32_t usage_of(bytecode::ValueType p_type, uint32_t p_usage) {
	return p_type == bytecode::ValueType::Variant ? p_usage | PROPERTY_USAGE_NIL_IS_VARIANT : p_usage;
}

// Array[T] is the engine's typed array, which it describes with a hint.
template <typename T>
static Dictionary typed(Dictionary p_dict, const T &p_slot) {
	if (p_slot.element != bytecode::ValueType::Void) {
		p_dict["hint"] = PROPERTY_HINT_ARRAY_TYPE;
		p_dict["hint_string"] = p_slot.element == bytecode::ValueType::Object ? String::utf8(p_slot.class_name.c_str()) : Variant::get_type_name(variant_type(p_slot.element));
	}
	return p_dict;
}

static Dictionary function_info(const ClassImage &p_image, int p_index) {
	const bytecode::Function &function = p_image.module.functions[p_index];
	Array args;
	for (const bytecode::Slot &param : function.params) {
		args.push_back(typed(property_dict(variant_type(param.type), String::utf8(param.name.c_str()), usage_of(param.type, PROPERTY_USAGE_DEFAULT), class_of(param.type, param.class_name)), param));
	}
	const Dictionary result = property_dict(variant_type(function.return_type), StringName(), usage_of(function.return_type, PROPERTY_USAGE_DEFAULT), class_of(function.return_type, function.return_class));
	Dictionary method = method_dict(p_image.function_names[p_index], args, result, METHOD_FLAGS_DEFAULT | (function.is_static ? METHOD_FLAG_STATIC : 0));
	Array defaults;
	for (const bytecode::Constant &value : function.defaults) {
		defaults.push_back(literal_value(value));
	}
	method["default_args"] = defaults;
	return method;
}

// How the inspector picks an object of the class, as for GDScript's exports:
// from resources or from nodes.
static PropertyHint picker_of(const String &p_class) {
	ClassDBSingleton *db = ClassDBSingleton::get_singleton();
	if (p_class == "Resource" || db->is_parent_class(p_class, "Resource")) {
		return PROPERTY_HINT_RESOURCE_TYPE;
	}
	if (p_class == "Node" || db->is_parent_class(p_class, "Node")) {
		return PROPERTY_HINT_NODE_TYPE;
	}
	return PROPERTY_HINT_NONE;
}

Dictionary property_info(const bytecode::Member &p_member) {
	// Exported members are stored in scenes and shown in the inspector; public
	// ones are only visible to other code.
	uint32_t usage = PROPERTY_USAGE_SCRIPT_VARIABLE;
	if (p_member.is_export) {
		usage |= PROPERTY_USAGE_DEFAULT;
	}
	Dictionary dict = property_dict(variant_type(p_member.type), String::utf8(p_member.name.c_str()), usage_of(p_member.type, usage), class_of(p_member.type, p_member.class_name));
	// The inspector picks a class of this language by its global name.
	String class_name = String::utf8(p_member.class_name.c_str());
	if (!p_member.script.empty()) {
		const Ref<Script> script = ResourceLoader::get_singleton()->load(String::utf8(p_member.script.c_str()));
		if (script.is_valid() && !script->get_global_name().is_empty()) {
			class_name = script->get_global_name();
		}
	}
	if (p_member.hint != 0) {
		dict["hint"] = p_member.hint;
		dict["hint_string"] = String::utf8(p_member.hint_string.c_str());
	} else if (p_member.is_export && p_member.type == ValueType::Object) {
		dict["hint"] = picker_of(class_name);
		dict["hint_string"] = class_name;
	} else if (p_member.is_export && p_member.element != ValueType::Void) {
		// An exported typed array: "type/hint:class" per element, as GDScript writes it.
		dict["hint"] = PROPERTY_HINT_TYPE_STRING;
		dict["hint_string"] = p_member.element == ValueType::Object ? vformat("%d/%d:%s", Variant::OBJECT, picker_of(class_name), class_name) : vformat("%d:", variant_type(p_member.element));
	} else {
		dict = typed(dict, p_member);
	}
	return dict;
}

// Headings from @export_category, @export_group and @export_subgroup.
static Dictionary group_info(const bytecode::ExportGroup &p_group) {
	static const uint32_t usages[] = { PROPERTY_USAGE_CATEGORY, PROPERTY_USAGE_GROUP, PROPERTY_USAGE_SUBGROUP };
	Dictionary dict = property_dict(Variant::NIL, String::utf8(p_group.name.c_str()), usages[p_group.level]);
	dict["hint_string"] = String::utf8(p_group.prefix.c_str());
	return dict;
}

TypedArray<Dictionary> visible_properties(const ClassImage &p_image) {
	TypedArray<Dictionary> list;
	const bytecode::Module &module = p_image.module;
	for (size_t i = 0; i <= module.members.size(); i++) {
		for (const bytecode::ExportGroup &group : module.groups) {
			if (group.before_member == i) {
				list.push_back(group_info(group));
			}
		}
		if (i < module.members.size() && PetScript::is_visible(module.members[i])) {
			list.push_back(property_info(module.members[i]));
		}
	}
	return list;
}

TypedArray<Dictionary> visible_methods(const ClassImage &p_image) {
	TypedArray<Dictionary> list;
	for (size_t i = 0; i < p_image.module.functions.size(); i++) {
		if (PetScript::is_visible(p_image.module.functions[i])) {
			list.push_back(function_info(p_image, int(i)));
		}
	}
	return list;
}

Variant initial_value(const ClassImage &p_image, const bytecode::Member &p_member) {
	if (!p_member.has_initial) {
		return default_variant(p_member.type);
	}
	return literal_value(p_member.initial);
}

PetHeader parse_header(const String &p_source) {
	// The class header comes first (spec §2.1): read it until the members start.
	PetHeader header;
	bool has_base = false;
	const PackedStringArray lines = p_source.split("\n");
	for (int64_t i = 0; i < lines.size(); i++) {
		String code = lines[i];
		const int64_t comment = code.find("#");
		if (comment >= 0) {
			code = code.substr(0, comment);
		}
		const PackedStringArray words = code.replace("\t", " ").replace("\r", " ").strip_edges().split(" ", false);
		if (words.is_empty()) {
			continue;
		}
		if (words[0] != "class_name" && words[0] != "extends") {
			break;
		}
		for (int64_t w = 0; w + 1 < words.size(); w++) {
			if (words[w] == "class_name" && header.class_name.is_empty()) {
				header.class_name = words[w + 1];
			} else if (words[w] == "extends" && !has_base) {
				header.base = words[w + 1];
				has_base = true;
			}
		}
	}
	return header;
}

/* PetScript */

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
	ERR_FAIL_COND_V_MSG(!get_image(), Variant(), get_script_path() + " cannot be constructed: it has errors.");
	Variant object = ClassDBSingleton::get_singleton()->instantiate(_get_instance_base_type());
	Object *instance = object;
	ERR_FAIL_NULL_V(instance, Variant());
	instance->set_script(Ref<PetScript>(this));
	return object;
}

String PetScript::get_script_path() const {
	return get_path();
}

std::shared_ptr<const ClassImage> PetScript::get_image() const {
	std::lock_guard<std::mutex> lock(mutex);
	return image;
}

void PetScript::set_loaded(const String &p_source, const std::shared_ptr<const ClassImage> &p_image) {
	source = p_source;
	header = parse_header(p_source);
	set_image(p_image);
}

void PetScript::set_image(const std::shared_ptr<const ClassImage> &p_image) {
	bool kept_old_code = false;
	{
		std::lock_guard<std::mutex> lock(mutex);
		image = p_image;
		if (p_image) {
			// Each instance is compared with the image it holds, which can be
			// older than the script's previous one: an instance that kept its
			// code through one reload keeps its member layout too.
			for (PetInstance *instance : instances) {
				const std::shared_ptr<const ClassImage> held = instance->current_image();
				if (held == p_image) {
					continue;
				}
				if (p_image->same_declarations(*held)) {
					// Only function bodies differ: the instance switches to the
					// new code, and frames already running finish with the old
					// (spec §12.4).
					instance->replace_image(p_image);
				} else {
					kept_old_code = true;
				}
			}
		}
	}
	if (kept_old_code) {
		WARN_PRINT(get_script_path() + ": more than function bodies changed since some instances were created. They keep their old code; run the scene again to apply the change.");
	}
	_update_exports();
}

void PetScript::instance_freed(PetInstance *p_instance) {
	std::lock_guard<std::mutex> lock(mutex);
	instances.erase(p_instance);
}

int PetScript::find_visible_function(const ClassImage &p_image, const StringName &p_name) {
	const int index = p_image.find_function(p_name);
	return index >= 0 && is_visible(p_image.module.functions[index]) ? index : -1;
}

void PetScript::update_placeholder(void *p_placeholder) const {
	Array properties = _get_script_property_list();
	Dictionary values;
	if (const std::shared_ptr<const ClassImage> current = get_image()) {
		for (size_t i = 0; i < current->module.members.size(); i++) {
			const bytecode::Member &member = current->module.members[i];
			if (is_visible(member) && member.has_initial) {
				values[current->member_names[i]] = initial_value(*current, member);
			}
		}
	}
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
	const std::shared_ptr<const ClassImage> current = get_image();
	return current && !current->module.declarations_only && (_is_tool() || !Engine::get_singleton()->is_editor_hint());
}

Ref<Script> PetScript::_get_base_script() const {
	return Ref<Script>();
}

StringName PetScript::_get_global_name() const {
	const std::shared_ptr<const ClassImage> current = get_image();
	return current ? current->global_name : StringName(header.class_name);
}

bool PetScript::_inherits_script(const Ref<Script> &p_script) const {
	return p_script.ptr() == this;
}

StringName PetScript::_get_instance_base_type() const {
	const std::shared_ptr<const ClassImage> current = get_image();
	return current ? current->base_class : StringName(header.base);
}

void *PetScript::_instance_create(Object *p_for_object) const {
	const std::shared_ptr<const ClassImage> current = get_image();
	ERR_FAIL_COND_V_MSG(!current, nullptr, get_script_path() + " has errors and cannot be instantiated.");
	ERR_FAIL_COND_V_MSG(current->module.declarations_only, nullptr, get_script_path() + " was only described for the editor; it runs once the project is built.");
	if (!ClassDBSingleton::get_singleton()->is_parent_class(p_for_object->get_class(), current->base_class)) {
		ERR_PRINT(vformat("%s extends %s and cannot be attached to %s.", get_script_path(), current->base_class, p_for_object->get_class()));
		return nullptr;
	}
	PetInstance *instance = memnew(PetInstance(const_cast<PetScript *>(this), current, p_for_object));
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
	return !source.is_empty();
}

String PetScript::_get_source_code() const {
	return source;
}

void PetScript::_set_source_code(const String &p_code) {
	source = p_code;
	header = parse_header(p_code);
}

Error PetScript::_reload(bool p_keep_state) {
	const String path = get_script_path();
#ifdef TOOLS_ENABLED
	if (Engine::get_singleton()->is_editor_hint()) {
		const std::shared_ptr<const ClassImage> compiled = compile_script(path, source, nullptr, true);
		set_image(compiled);
		return compiled ? OK : ERR_PARSE_ERROR;
	}
#endif
	// In the game, the editor has already compiled the new source into the cache.
	if (!FileAccess::file_exists(path)) {
		return OK; // An exported game has no sources to reload from.
	}
	const String new_source = FileAccess::get_file_as_string(path);
	String error;
	const std::shared_ptr<const ClassImage> loaded = load_compiled(cache_path(path), path, &new_source, error);
	if (!loaded) {
		ERR_PRINT(vformat("%s %s", path, error));
		return ERR_FILE_CORRUPT;
	}
	source = new_source;
	header = parse_header(new_source);
	set_image(loaded);
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
#ifdef PETRICHOR_PROBES
	if (probe_methods::has(p_method)) {
		return true;
	}
#endif
	const std::shared_ptr<const ClassImage> current = get_image();
	return current && (find_visible_function(*current, p_method) >= 0 || p_method == init_name());
}

bool PetScript::_has_static_method(const StringName &p_method) const {
	return p_method == init_name() && get_image();
}

Variant PetScript::_get_script_method_argument_count(const StringName &p_method) const {
	// One snapshot: a reload may replace the image between two reads.
	const std::shared_ptr<const ClassImage> current = get_image();
	const int index = current ? find_visible_function(*current, p_method) : -1;
	if (index < 0) {
		return Variant();
	}
	return int64_t(current->module.functions[index].params.size());
}

Dictionary PetScript::_get_method_info(const StringName &p_method) const {
#ifdef PETRICHOR_PROBES
	if (probe_methods::has(p_method)) {
		return probe_methods::info(p_method);
	}
#endif
	const std::shared_ptr<const ClassImage> current = get_image();
	if (!current) {
		return Dictionary();
	}
	if (p_method == init_name()) {
		// A static method returning the class, which is how GDScript types
		// `new` (spec §10.1).
		const StringName type_name = current->global_name == StringName() ? current->base_class : current->global_name;
		return method_dict(init_name(), Array(), property_dict(Variant::OBJECT, StringName(), PROPERTY_USAGE_DEFAULT, type_name), METHOD_FLAGS_DEFAULT | METHOD_FLAG_STATIC);
	}
	const int index = find_visible_function(*current, p_method);
	return index >= 0 ? function_info(*current, index) : Dictionary();
}

bool PetScript::_is_tool() const {
	return false;
}

bool PetScript::_is_valid() const {
	return bool(get_image());
}

bool PetScript::_is_abstract() const {
	return false;
}

ScriptLanguage *PetScript::_get_language() const {
	return PetLanguage::get_singleton();
}

// Signals are always visible: the engine connects and emits them by name
// (spec §10.5).
bool PetScript::_has_script_signal(const StringName &p_signal) const {
	const std::shared_ptr<const ClassImage> current = get_image();
	if (!current) {
		return false;
	}
	for (const bytecode::SignalInfo &signal : current->module.signals) {
		if (p_signal == StringName(String::utf8(signal.name.c_str()))) {
			return true;
		}
	}
	return false;
}

TypedArray<Dictionary> PetScript::_get_script_signal_list() const {
	TypedArray<Dictionary> list;
	if (const std::shared_ptr<const ClassImage> current = get_image()) {
		for (const bytecode::SignalInfo &signal : current->module.signals) {
			Array args;
			for (const bytecode::Slot &param : signal.params) {
				args.push_back(typed(property_dict(variant_type(param.type), String::utf8(param.name.c_str()), usage_of(param.type, PROPERTY_USAGE_DEFAULT), class_of(param.type, param.class_name)), param));
			}
			list.push_back(method_dict(String::utf8(signal.name.c_str()), args, property_dict(Variant::NIL, StringName(), PROPERTY_USAGE_DEFAULT), METHOD_FLAGS_DEFAULT));
		}
	}
	return list;
}

bool PetScript::_has_property_default_value(const StringName &p_property) const {
	const std::shared_ptr<const ClassImage> current = get_image();
	if (!current) {
		return false;
	}
	// Only a literal initial value is a default; other initializers run per
	// instance (spec §6.1).
	const int index = current->find_member(p_property);
	return index >= 0 && is_visible(current->module.members[index]) && current->module.members[index].has_initial;
}

Variant PetScript::_get_property_default_value(const StringName &p_property) const {
	const std::shared_ptr<const ClassImage> current = get_image();
	if (!current) {
		return Variant();
	}
	const int index = current->find_member(p_property);
	return index >= 0 ? initial_value(*current, current->module.members[index]) : Variant();
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
	if (const std::shared_ptr<const ClassImage> current = get_image()) {
		list = visible_methods(*current);
	}
#ifdef PETRICHOR_PROBES
	probe_methods::append_to(list);
#endif
	return list;
}

TypedArray<Dictionary> PetScript::_get_script_property_list() const {
	const std::shared_ptr<const ClassImage> current = get_image();
	return current ? visible_properties(*current) : TypedArray<Dictionary>();
}

int32_t PetScript::_get_member_line(const StringName &p_member) const {
	return -1;
}

Dictionary PetScript::_get_constants() const {
	return Dictionary();
}

TypedArray<StringName> PetScript::_get_members() const {
	TypedArray<StringName> members;
	if (const std::shared_ptr<const ClassImage> current = get_image()) {
		for (size_t i = 0; i < current->module.members.size(); i++) {
			if (is_visible(current->module.members[i])) {
				members.push_back(current->member_names[i]);
			}
		}
	}
	return members;
}

bool PetScript::_is_placeholder_fallback_enabled() const {
	return false;
}

Variant PetScript::_get_rpc_config() const {
	return Dictionary();
}

bool PetScript::_instance_has(Object *p_object) const {
	return find_instance(p_object) != nullptr;
}

PetInstance *PetScript::find_instance(Object *p_object) const {
	if (!p_object) {
		return nullptr;
	}
	std::lock_guard<std::mutex> lock(mutex);
	for (PetInstance *instance : instances) {
		if (instance->owner == p_object->_owner) {
			return instance;
		}
	}
	return nullptr;
}

} // namespace petrichor
