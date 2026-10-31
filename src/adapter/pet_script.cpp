#include "adapter/pet_script.h"

#include "adapter/pet_compiled.h"
#include "adapter/pet_instance.h"
#include "adapter/pet_language.h"
#include "runtime/text.h"

#ifdef TOOLS_ENABLED
#include "adapter/editor/project_files.h"
#include "adapter/editor/script_compiler.h"
#endif
#ifdef PETRICHOR_PROBES
#include "probe/probe_methods.h"
#endif

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/error_macros.hpp>

#include <set>

using namespace godot;

namespace petrichor {

using bytecode::ValueType;

namespace {
std::mutex scripts_mutex;
std::set<PetScript *> scripts;
} //namespace

PetScript::PetScript() {
	std::lock_guard<std::mutex> lock(scripts_mutex);
	scripts.insert(this);
}

PetScript::~PetScript() {
	std::lock_guard<std::mutex> lock(scripts_mutex);
	scripts.erase(this);
}

String header_base_path(const PetHeader &p_header, const String &p_path) {
	if (p_header.base_is_path) {
		return (p_header.base.begins_with("res://") ? p_header.base : p_path.get_base_dir().path_join(p_header.base)).simplify_path();
	}
	if (ClassDBSingleton::get_singleton()->class_exists(p_header.base)) {
		return String();
	}
	const TypedArray<Dictionary> classes = ProjectSettings::get_singleton()->get_global_class_list();
	for (int64_t i = 0; i < classes.size(); ++i) {
		const Dictionary entry = classes[i];
		if (String(entry.get("class", String())) == p_header.base) {
			return entry.get("path", String());
		}
	}
#ifdef TOOLS_ENABLED
	if (Engine::get_singleton()->is_editor_hint()) {
		for (const String &path : project_files(PackedStringArray({ "pet" }))) {
			if (parse_header(FileAccess::get_file_as_string(path)).class_name == p_header.base) {
				return path;
			}
		}
	}
#endif
	return String();
}

String header_native_base(const PetHeader &p_header, const String &p_path) {
	PetHeader header = p_header;
	String path = p_path;
	std::set<String> seen;
	while (seen.insert(path).second) {
		if (!header.base_is_path && ClassDBSingleton::get_singleton()->class_exists(header.base)) {
			return header.base;
		}
		path = header_base_path(header, path);
		if (path.is_empty() || path.get_extension() != "pet") {
			break;
		}
		header = parse_header(FileAccess::get_file_as_string(path));
	}
	return "RefCounted";
}

bool header_extends(const PetHeader &p_header, const String &p_path, const String &p_base) {
	std::set<String> seen;
	for (String path = header_base_path(p_header, p_path); !path.is_empty() && seen.insert(path).second;) {
		if (path == p_base) {
			return true;
		}
		if (path.get_extension() != "pet") {
			return false;
		}
		path = header_base_path(parse_header(FileAccess::get_file_as_string(path)), path);
	}
	return false;
}

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

// A value of the type as the engine and GDScript see it: an object as its
// class, T and T? alike (spec §10.1); a Variant slot, such as int?, is NIL to
// the engine, which reads that as void unless told otherwise.
static Dictionary value_dict(const TypeImage &p_type, const StringName &p_name, uint32_t p_usage) {
	const bool object = p_type.storage == bytecode::ValueType::Object;
	const uint32_t usage = p_type.storage == bytecode::ValueType::Variant || p_type.kind == bytecode::TypeKind::Option ? p_usage | PROPERTY_USAGE_NIL_IS_VARIANT : p_usage;
	return property_dict(variant_type(p_type.storage), p_name, usage, object ? p_type.check.class_name : StringName());
}

// Array[T] is the engine's typed array, which it describes with a hint.
static Dictionary typed(Dictionary p_dict, const TypeImage &p_type) {
	const bytecode::ValueType element = p_type.check.element;
	if (element != bytecode::ValueType::Void) {
		p_dict["hint"] = PROPERTY_HINT_ARRAY_TYPE;
		p_dict["hint_string"] = element == bytecode::ValueType::Object ? String(p_type.check.class_name) : Variant::get_type_name(variant_type(element));
	}
	return p_dict;
}

static Dictionary function_info(const ClassImage &p_image, int p_index) {
	const bytecode::Function &function = p_image.module.functions[p_index];
	Array args;
	for (const bytecode::Slot &param : function.params) {
		args.push_back(typed(value_dict(p_image.types[param.type], to_godot(param.name), PROPERTY_USAGE_DEFAULT), p_image.types[param.type]));
	}
	const Dictionary result = function.is_async ? property_dict(Variant::NIL, StringName(), PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_NIL_IS_VARIANT, StringName()) : value_dict(p_image.types[function.result.type], StringName(), PROPERTY_USAGE_DEFAULT);
	Dictionary method = method_dict(p_image.function_names[p_index], args, result, METHOD_FLAGS_DEFAULT | (function.is_static ? METHOD_FLAG_STATIC : 0));
	Array defaults;
	for (const bytecode::Constant &constant : function.defaults) {
		Variant value;
		String error;
		if (!constant_value(p_image.types, constant, value, error)) {
			ERR_PRINT(vformat("%s: a default argument of %s: %s", p_image.path, p_image.function_names[p_index], error));
		}
		defaults.push_back(value);
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

Dictionary property_info(const ClassImage &p_image, const bytecode::Member &p_member) {
	// Exported members are stored in scenes and shown in the inspector; public
	// ones are only visible to other code.
	uint32_t usage = PROPERTY_USAGE_SCRIPT_VARIABLE;
	if (p_member.is_export) {
		usage |= PROPERTY_USAGE_DEFAULT;
	}
	const TypeImage &type = p_image.types[p_member.type];
	Dictionary dict = value_dict(type, to_godot(p_member.name), usage);
	// The inspector picks a class of this language by its global name.
	String class_name = type.check.class_name;
	if (!type.check.script.is_empty()) {
		const Ref<Script> script = ResourceLoader::get_singleton()->load(type.check.script);
		if (script.is_valid() && !script->get_global_name().is_empty()) {
			class_name = script->get_global_name();
		}
	}
	const ValueType element = type.check.element;
	if (p_member.hint != 0) {
		dict["hint"] = p_member.hint;
		dict["hint_string"] = to_godot(p_member.hint_string);
	} else if (p_member.is_export && type.storage == ValueType::Object) {
		dict["hint"] = picker_of(class_name);
		dict["hint_string"] = class_name;
	} else if (p_member.is_export && element != ValueType::Void) {
		// An exported typed array: "type/hint:class" per element, as GDScript writes it.
		dict["hint"] = PROPERTY_HINT_TYPE_STRING;
		dict["hint_string"] = element == ValueType::Object ? vformat("%d/%d:%s", Variant::OBJECT, picker_of(class_name), class_name) : vformat("%d:", variant_type(element));
	} else {
		dict = typed(dict, type);
	}
	return dict;
}

// Headings from @export_category, @export_group and @export_subgroup.
static Dictionary group_info(const bytecode::ExportGroup &p_group) {
	static const uint32_t usages[] = { PROPERTY_USAGE_CATEGORY, PROPERTY_USAGE_GROUP, PROPERTY_USAGE_SUBGROUP };
	Dictionary dict = property_dict(Variant::NIL, to_godot(p_group.name), usages[p_group.level]);
	dict["hint_string"] = to_godot(p_group.prefix);
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
			list.push_back(property_info(p_image, module.members[i]));
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

Variant initial_value(const ClassImage &p_image, size_t p_member) {
	const bytecode::Member &member = p_image.module.members[p_member];
	if (!member.has_initial) {
		return Variant();
	}
	// Built for each request, so that no two holders share an array, and late,
	// so that the script that types an array has been loaded.
	Variant value;
	String error;
	if (!constant_value(p_image.types, member.initial, value, error)) {
		ERR_PRINT(vformat("%s: the initial value of %s: %s", p_image.path, p_image.member_names[p_member], error));
		return default_variant(p_image.types[member.type]);
	}
	return value;
}

PetHeader parse_header(const String &p_source) {
	// The class header comes first (spec §2.1): class annotations, class_name,
	// extends and implements. It is read up to the first member, past
	// comments, strings and the arguments of annotations, such as @icon("…").
	PetHeader header;
	const int64_t length = p_source.length();
	const auto is_word = [](char32_t p_char) {
		return p_char == '_' || p_char == '.' || (p_char >= '0' && p_char <= '9') || (p_char >= 'a' && p_char <= 'z') || (p_char >= 'A' && p_char <= 'Z') || p_char >= 0x80;
	};
	const auto skip_string = [&](int64_t p_at) {
		const char32_t quote = p_source[p_at++];
		while (p_at < length && p_source[p_at] != quote && p_source[p_at] != '\n') {
			p_at += p_source[p_at] == '\\' ? 2 : 1;
		}
		return p_at + 1;
	};
	String keyword; // class_name or extends, whose name comes next.
	bool has_name = false;
	bool has_base = false;
	for (int64_t i = 0; i < length;) {
		const char32_t c = p_source[i];
		if (c == '#') {
			while (i < length && p_source[i] != '\n') {
				i++;
			}
		} else if (c == '"' || c == '\'') {
			const int64_t start = i;
			i = skip_string(i);
			if (keyword == "extends" && !has_base) {
				header.base = p_source.substr(start + 1, i - start - 2);
				header.base_is_path = true;
				has_base = true;
			}
			keyword = String();
		} else if (c == '@') {
			for (i++; i < length && is_word(p_source[i]); i++) {
			}
			while (i < length && (p_source[i] == ' ' || p_source[i] == '\t')) {
				i++;
			}
			if (i < length && p_source[i] == '(') {
				for (int depth = 0; i < length;) {
					if (p_source[i] == '"' || p_source[i] == '\'') {
						i = skip_string(i);
						continue;
					}
					depth += p_source[i] == '(' ? 1 : (p_source[i] == ')' ? -1 : 0);
					i++;
					if (depth == 0) {
						break;
					}
				}
			}
		} else if (is_word(c)) {
			const int64_t start = i;
			while (i < length && is_word(p_source[i])) {
				i++;
			}
			const String word = p_source.substr(start, i - start);
			if (keyword == "class_name" && !has_name) {
				header.class_name = word;
				has_name = true;
			} else if (keyword == "extends" && !has_base) {
				header.base = word;
				has_base = true;
			} else if (word != "class_name" && word != "extends") {
				break; // implements, or the first member.
			}
			keyword = word == "class_name" || word == "extends" ? word : String();
		} else {
			i++;
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

void PetScript::set_loaded(const String &p_source, const std::shared_ptr<const ClassImage> &p_image, const Ref<PetScript> &p_base) {
	source = p_source;
	header = parse_header(p_source);
	set_image(p_image, p_base);
}

void PetScript::set_image(const std::shared_ptr<const ClassImage> &p_image, const Ref<PetScript> &p_base) {
	{
		std::lock_guard<std::mutex> lock(mutex);
		image = p_image;
		if (p_base.is_valid()) {
			base_script = p_base;
		} else if (!p_image || p_image->module.base_script.empty() || (base_script.is_valid() && base_script->get_script_path() != to_godot(p_image->module.base_script))) {
			base_script.unref();
		}
		// Instances exist only in the game, where a reload gets here only when
		// function bodies alone changed (_reload()): they switch to the new
		// code, and frames already running finish with the old (spec §12.4).
		for (PetInstance *instance : instances) {
			if (p_image) {
				instance->replace_image(p_image);
			}
		}
	}
	if (p_image) {
		_update_exports();
	}
}

namespace {
std::shared_ptr<const ClassImage> rebased(const std::shared_ptr<const ClassImage> &p_image, const std::shared_ptr<const ClassImage> &p_new, String &r_error) {
	if (p_image->path == p_new->path) {
		return p_new;
	}
	if (!p_image->base) {
		return p_image;
	}
	const auto base = rebased(p_image->base, p_new, r_error);
	if (!base) {
		return nullptr;
	}
	if (base == p_image->base) {
		return p_image;
	}
	return ClassImage::load(bytecode::Module(p_image->module), p_image->path, r_error, base);
}
} //namespace

bool PetScript::apply_image(const std::shared_ptr<const ClassImage> &p_image, const Ref<PetScript> &p_base) {
	if (!p_image) {
		return false;
	}
	std::vector<Ref<PetScript>> live;
	{
		std::lock_guard<std::mutex> lock(scripts_mutex);
		for (PetScript *script : scripts) {
			if (script != this) {
				live.push_back(Ref<PetScript>(script));
			}
		}
	}
	std::vector<std::pair<Ref<PetScript>, std::shared_ptr<const ClassImage>>> updates;
	String error;
	for (const Ref<PetScript> &script : live) {
		const auto old = script->get_image();
		bool dependent = false;
		for (auto base = old ? old->base : nullptr; base; base = base->base) {
			if (base->path == p_image->path) {
				dependent = true;
			}
		}
#ifdef TOOLS_ENABLED
		if (!old && Engine::get_singleton()->is_editor_hint()) {
			dependent = header_extends(script->header, script->get_script_path(), p_image->path);
		}
#endif
		if (!dependent) {
			continue;
		}
		std::shared_ptr<const ClassImage> next;
#ifdef TOOLS_ENABLED
		if (Engine::get_singleton()->is_editor_hint()) {
			next = compile_script(script->get_script_path(), script->_get_source_code(), nullptr, true, old && old->module.declarations_only ? CompileMode::Declarations : CompileMode::Full);
		} else
#endif
		{
			next = rebased(old, p_image, error);
			if (!next || !next->same_declarations(*old)) {
				PetLanguage::get_singleton()->reloads_refused = true;
				WARN_PRINT("Inherited declarations changed; restart the game. " + error);
				return false;
			}
		}
		updates.emplace_back(script, next);
	}
	set_image(p_image, p_base);
	for (const auto &update : updates) {
		update.first->set_image(update.second);
	}
	return true;
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
				values[current->member_names[i]] = initial_value(*current, i);
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
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (base_script.is_valid()) {
			return base_script;
		}
	}
	const auto current = get_image();
	const String path = current ? to_godot(current->module.base_script) : header_base_path(header, get_script_path());
	if (path.is_empty()) {
		return Ref<Script>();
	}
	thread_local std::set<String> resolving;
	if (!resolving.insert(path).second) {
		return Ref<Script>();
	}
	Ref<PetScript> base = ResourceLoader::get_singleton()->load(path, "Script");
	resolving.erase(path);
	if (base.ptr() == this) {
		return Ref<Script>();
	}
	std::lock_guard<std::mutex> lock(mutex);
	base_script = base;
	return base_script;
}

StringName PetScript::_get_global_name() const {
	const std::shared_ptr<const ClassImage> current = get_image();
	return current ? current->global_name : StringName(header.class_name);
}

bool PetScript::_inherits_script(const Ref<Script> &p_script) const {
	if (p_script.ptr() == this) {
		return true;
	}
	const Ref<PetScript> other = p_script;
	if (other.is_null()) {
		return false;
	}
	for (auto current = get_image(); current; current = current->base) {
		if (current->path == other->get_script_path()) {
			return true;
		}
	}
	return false;
}

StringName PetScript::_get_instance_base_type() const {
	const std::shared_ptr<const ClassImage> current = get_image();
	return current ? current->base_class : StringName(header_native_base(header, get_script_path()));
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
		return compiled && apply_image(compiled) ? OK : ERR_PARSE_ERROR;
	}
#endif
	// In the game, the editor has already compiled the new source into the cache.
	if (!FileAccess::file_exists(path)) {
		return OK; // An exported game has no sources to reload from.
	}
	const String new_source = FileAccess::get_file_as_string(path);
	String error;
	Ref<PetScript> base;
	const std::shared_ptr<const ClassImage> loaded = load_compiled(cache_path(path), path, &new_source, error, &base);
	if (!loaded) {
		ERR_PRINT(vformat("%s %s", path, error));
		return ERR_FILE_CORRUPT;
	}
	// Only function bodies change in a running game. After any other change
	// it keeps the code it has, for this script and every later reload, so
	// that a class has one version in a run (spec §12.4).
	PetLanguage *language = PetLanguage::get_singleton();
	const std::shared_ptr<const ClassImage> current = get_image();
	if (language->reloads_refused) {
		WARN_PRINT(vformat("%s: not reloaded, since an earlier change needs the game to run again.", path));
		return OK;
	}
	if (current && !loaded->same_declarations(*current)) {
		language->reloads_refused = true;
		WARN_PRINT(vformat("%s: more than function bodies changed, so the running game keeps its code, and reloads no script until it runs again.", path));
		return OK;
	}
	source = new_source;
	header = parse_header(new_source);
	return apply_image(loaded, base) ? OK : ERR_INVALID_DATA;
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
	const std::shared_ptr<const ClassImage> current = get_image();
	if (current && (find_visible_function(*current, p_method) >= 0 || p_method == init_name())) {
		return true;
	}
#ifdef PETRICHOR_PROBES
	return probe_methods::has(get_path(), p_method);
#else
	return false;
#endif
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
	if (probe_methods::has(get_path(), p_method)) {
		return probe_methods::info(get_path(), p_method);
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
		if (p_signal == StringName(to_godot(signal.name))) {
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
				args.push_back(typed(value_dict(current->types[param.type], to_godot(param.name), PROPERTY_USAGE_DEFAULT), current->types[param.type]));
			}
			list.push_back(method_dict(to_godot(signal.name), args, property_dict(Variant::NIL, StringName(), PROPERTY_USAGE_DEFAULT), METHOD_FLAGS_DEFAULT));
		}
	}
	return list;
}

bool PetScript::_has_property_default_value(const StringName &p_property) const {
	const std::shared_ptr<const ClassImage> current = get_image();
	if (!current) {
		return false;
	}
	// A constant initial value is a default. Every export has one, as in
	// GDScript (modules/gdscript/gdscript.cpp:557-563), since a placeholder
	// takes a value only for a property that has a default
	// (core/object/script_language.cpp:680-707): None where code computes the
	// value or the scene must assign it, so that the scene stores only what was
	// set (spec §6.1, §7.5).
	const int index = current->find_member(p_property);
	if (index < 0 || !is_visible(current->module.members[index])) {
		return false;
	}
	const bytecode::Member &member = current->module.members[index];
	return member.has_initial || member.is_export;
}

Variant PetScript::_get_property_default_value(const StringName &p_property) const {
	const std::shared_ptr<const ClassImage> current = get_image();
	if (!current) {
		return Variant();
	}
	const int index = current->find_member(p_property);
	return index >= 0 ? initial_value(*current, size_t(index)) : Variant();
}

void PetScript::_update_exports() {
	if (!get_image()) {
		return; // The placeholders keep what they have (_is_placeholder_fallback_enabled()).
	}
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
	probe_methods::append_to(get_path(), list);
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

// While the script has errors, placeholders keep the properties and values they
// had, and take any value a scene sets, so that saving the scene loses none, as
// for GDScript (modules/gdscript/gdscript.cpp:575-581,
// core/object/script_language.cpp:854-884).
bool PetScript::_is_placeholder_fallback_enabled() const {
	return !get_image();
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
