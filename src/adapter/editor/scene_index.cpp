#include "adapter/editor/scene_index.h"

#include "adapter/editor/class_db_api.h"
#include "adapter/editor/project_files.h"
#include "adapter/pet_compiled.h"
#include "adapter/pet_script.h"
#include "runtime/text.h"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_file_system_directory.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/resource_uid.hpp>
#include <godot_cpp/classes/scene_state.hpp>
#include <godot_cpp/classes/script.hpp>

#include <algorithm>
#include <optional>
#include <set>

using namespace godot;

namespace petrichor {

namespace {

// Node paths from the scene's root: "." for the root, "Head/Camera" below it.
std::string node_path(const NodePath &p_path) {
	String path = String(p_path);
	if (path.begins_with("./")) {
		path = path.substr(2);
	}
	return to_utf8(path.is_empty() ? String(".") : path);
}

// The class of a node; an instanced scene's node takes its root's class.
String node_type(const Ref<SceneState> &p_state, int32_t p_index) {
	const String type = p_state->get_node_type(p_index);
	if (!type.is_empty()) {
		return type;
	}
	const Ref<PackedScene> instance = p_state->get_node_instance(p_index);
	if (instance.is_valid() && instance->get_state().is_valid() && instance->get_state()->get_node_count() > 0) {
		return node_type(instance->get_state(), 0);
	}
	return String();
}

// The value a scene gives a property of the node, or nil, also when it gives
// none; `r_given` tells the two apart.
Variant node_property(const Ref<SceneState> &p_state, int32_t p_index, const char *p_name, bool *r_given = nullptr) {
	for (int32_t p = 0; p < p_state->get_node_property_count(p_index); p++) {
		if (p_state->get_node_property_name(p_index, p) == StringName(p_name)) {
			if (r_given) {
				*r_given = true;
			}
			return p_state->get_node_property_value(p_index, p);
		}
	}
	return Variant();
}

// The script a scene sets on the node, if it sets one: an empty path when it
// takes away the one the node has from a base or an instanced scene
// (scene/resources/packed_scene.cpp:423-426; ed1daf0).
std::optional<std::string> attached_script(const Ref<SceneState> &p_state, int32_t p_index) {
	bool given = false;
	const Ref<Script> script = node_property(p_state, p_index, "script", &given);
	if (!given) {
		return std::nullopt;
	}
	return script.is_valid() ? to_utf8(script->get_path()) : std::string();
}

// The script an instanced scene's root has, through instances of instances.
std::string instanced_script(const Ref<SceneState> &p_state, int32_t p_index) {
	const Ref<PackedScene> instance = p_state->get_node_instance(p_index);
	if (instance.is_null() || instance->get_state().is_null() || instance->get_state()->get_node_count() == 0) {
		return std::string();
	}
	const std::optional<std::string> script = attached_script(instance->get_state(), 0);
	return script ? *script : instanced_script(instance->get_state(), 0);
}

// The paths of the scenes a scene instances, through instances of instances.
// An inherited scene's root is an instance of its base
// (scene/resources/packed_scene.cpp:1921-1924; ed1daf0).
void instanced_scenes(const Ref<SceneState> &p_state, std::set<std::string> &r_paths) {
	for (int32_t i = 0; i < p_state->get_node_count(); i++) {
		const Ref<PackedScene> instance = p_state->get_node_instance(i);
		if (instance.is_valid() && r_paths.insert(to_utf8(instance->get_path())).second && instance->get_state().is_valid()) {
			instanced_scenes(instance->get_state(), r_paths);
		}
	}
}

// An inherited scene stores only the nodes it adds or changes and the
// connections it adds; the rest are in its base (scene/resources/packed_scene.cpp:1483-1490,
// 1888-1894, 1912-1928; ed1daf0).
compiler::Scene read_scene(const String &p_path, const Ref<SceneState> &p_state) {
	const Ref<SceneState> base = p_state->get_base_scene_state();
	compiler::Scene scene = base.is_valid() ? read_scene(p_path, base) : compiler::Scene();
	scene.path = to_utf8(p_path);
	for (int32_t i = 0; i < p_state->get_node_count(); i++) {
		const std::string path = node_path(p_state->get_node_path(i));
		const std::optional<std::string> script = attached_script(p_state, i);
		const auto inherited = std::find_if(scene.nodes.begin(), scene.nodes.end(), [&](const compiler::SceneNode &p_node) { return p_node.path == path; });
		if (inherited != scene.nodes.end()) {
			if (script) {
				inherited->script = *script;
				inherited->instance_script.clear();
			}
			const Variant unique = node_property(p_state, i, "unique_name_in_owner");
			if (unique.get_type() == Variant::BOOL) {
				inherited->unique = unique;
			}
			continue;
		}
		compiler::SceneNode node;
		node.path = path;
		node.type = to_utf8(node_type(p_state, i));
		node.is_instance = p_state->get_node_instance(i).is_valid();
		node.script = script.value_or(std::string());
		// A node of an instanced scene that this scene only changes has the
		// instance's root as its owner, and its unique name is that root's
		// (scene/resources/packed_scene.cpp:567-573, 1888-1894; ed1daf0).
		node.unique = (node.is_instance || !p_state->get_node_type(i).is_empty()) && bool(node_property(p_state, i, "unique_name_in_owner"));
		if (node.is_instance && !script) {
			node.instance_script = instanced_script(p_state, i);
		}
		scene.nodes.push_back(node);
	}
	for (int32_t i = 0; i < p_state->get_connection_count(); i++) {
		compiler::SceneConnection connection;
		connection.signal = to_utf8(p_state->get_connection_signal(i));
		connection.source = node_path(p_state->get_connection_source(i));
		connection.target = node_path(p_state->get_connection_target(i));
		connection.method = to_utf8(p_state->get_connection_method(i));
		connection.binds = int(p_state->get_connection_binds(i).size());
		connection.unbinds = int(p_state->get_connection_unbinds(i));
		scene.connections.push_back(connection);
	}
	return scene;
}

// The type of a constant by its value (spec §11.1); an object by its class,
// or the global name of its script's class.
compiler::Type value_type(const Variant &p_value) {
	switch (p_value.get_type()) {
		case Variant::NIL:
			return compiler::Type::of(compiler::TypeKind::Variant);
		case Variant::OBJECT: {
			const Object *object = p_value;
			if (!object) {
				return compiler::Type::object("Object", true);
			}
			const Ref<Script> script = object->get_script();
			const String global = Object::cast_to<Script>(object) || script.is_null() ? String() : String(script->get_global_name());
			return compiler::Type::object(to_utf8(global.is_empty() ? object->get_class() : global));
		}
		case Variant::ARRAY: {
			const Array array = p_value;
			if (!array.is_typed()) {
				return compiler::Type::builtin("Array");
			}
			const String element = array.get_typed_builtin() == Variant::OBJECT ? String(array.get_typed_class_name()) : Variant::get_type_name(Variant::Type(array.get_typed_builtin()));
			return compiler::api_type("typedarray::" + to_utf8(element), true);
		}
		default:
			return compiler::api_type(to_utf8(Variant::get_type_name(p_value.get_type())), true);
	}
}

} // namespace

const SceneIndex &SceneIndex::get() {
	static const SceneIndex index;
	return index;
}

std::shared_ptr<const SceneIndex::Entry> SceneIndex::read(const std::string &p_path) const {
	const String path = to_godot(p_path);
	// Before loading, so that a change during the load is seen next time.
	const uint64_t modified = FileAccess::get_modified_time(path);
	std::shared_ptr<const Entry> cached;
	{
		std::lock_guard<std::mutex> lock(mutex);
		const auto found = scenes.find(p_path);
		if (found != scenes.end()) {
			cached = found->second;
		}
	}
	if (cached && cached->modified == modified && std::all_of(cached->instanced.begin(), cached->instanced.end(), [](const auto &p_instanced) {
			return FileAccess::get_modified_time(to_godot(p_instanced.first)) == p_instanced.second;
		})) {
		return cached;
	}

	const std::shared_ptr<Entry> entry = std::make_shared<Entry>();
	entry->modified = modified;
	// From the file: the editor keeps the scene it loaded once, such as one in
	// an exported property, after the file changes (core/io/resource_loader.cpp:800-803,
	// scene/resources/packed_scene.h:256).
	const Ref<PackedScene> packed = ResourceLoader::get_singleton()->load(path, "PackedScene", ResourceLoader::CACHE_MODE_IGNORE);
	if (packed.is_valid() && packed->get_state().is_valid()) {
		entry->scene = read_scene(path, packed->get_state());
		std::set<std::string> instanced;
		instanced_scenes(packed->get_state(), instanced);
		for (const std::string &instanced_path : instanced) {
			entry->instanced.emplace_back(instanced_path, FileAccess::get_modified_time(to_godot(instanced_path)));
		}
	}
	std::lock_guard<std::mutex> lock(mutex);
	scenes.insert_or_assign(p_path, entry);
	return entry;
}

thread_local int SceneIndex::pass_depth = 0;
thread_local std::unique_ptr<SceneIndex::Snapshot> SceneIndex::snapshot;

SceneIndex::Pass::Pass() {
	if (pass_depth++ == 0) {
		snapshot = std::make_unique<Snapshot>();
	}
}

SceneIndex::Pass::~Pass() {
	if (--pass_depth == 0) {
		snapshot.reset();
	}
}

void SceneIndex::forget(const std::string &p_path) const {
	std::lock_guard<std::mutex> lock(mutex);
	for (auto entry = scenes.begin(); entry != scenes.end();) {
		const auto &instanced = entry->second->instanced;
		if (entry->first == p_path || std::any_of(instanced.begin(), instanced.end(), [&](const auto &p_scene) { return p_scene.first == p_path; })) {
			entry = scenes.erase(entry);
		} else {
			++entry;
		}
	}
}

std::vector<compiler::Scene> SceneIndex::scenes_where(const std::function<bool(const compiler::SceneNode &)> &p_uses) const {
	const Pass pass;
	if (!snapshot->listed) {
		for (const String &path : project_files(PackedStringArray({ "tscn", "scn" }))) {
			snapshot->scenes.push_back(read(to_utf8(path)));
		}
		snapshot->listed = true;
	}
	std::vector<compiler::Scene> result;
	for (const std::shared_ptr<const Entry> &entry : snapshot->scenes) {
		if (std::any_of(entry->scene.nodes.begin(), entry->scene.nodes.end(), p_uses)) {
			result.push_back(entry->scene);
		}
	}
	return result;
}

// A scene with a script that extends this one uses it too.
bool SceneIndex::is_or_extends(const std::string &p_script, const std::string &p_base) const {
	const Pass pass;
	std::set<std::string> seen;
	for (std::string path = p_script; !path.empty() && seen.insert(path).second;) {
		if (path == p_base) {
			return true;
		}
		const auto found = snapshot->bases.find(path);
		if (found != snapshot->bases.end()) {
			path = found->second;
			continue;
		}
		const String script = to_godot(path);
		const std::string base = script.get_extension() == "pet" ? to_utf8(header_base_path(parse_header(FileAccess::get_file_as_string(script)), script)) : std::string();
		snapshot->bases.emplace(path, base);
		path = base;
	}
	return false;
}

std::vector<compiler::Scene> SceneIndex::scenes_with_script(const std::string &p_script_path) const {
	return scenes_where([&](const compiler::SceneNode &p_node) {
		return is_or_extends(p_node.script, p_script_path);
	});
}

std::vector<compiler::Scene> SceneIndex::scenes_instancing_script(const std::string &p_script_path) const {
	return scenes_where([&](const compiler::SceneNode &p_node) {
		return p_node.is_instance && p_node.script.empty() && is_or_extends(p_node.instance_script, p_script_path);
	});
}

std::optional<compiler::Scene> SceneIndex::scene(const std::string &p_path) const {
	if (!FileAccess::file_exists(to_godot(p_path))) {
		return std::nullopt;
	}
	const std::shared_ptr<const Entry> entry = read(p_path);
	return entry->scene.nodes.empty() ? std::nullopt : std::optional<compiler::Scene>(entry->scene);
}

namespace {

// The value of `p_name="…"` in a line of a resource file, or "".
String attribute(const String &p_line, const String &p_name) {
	const String start = p_name + String("=\"");
	const int64_t at = p_line.find(start);
	if (at < 0) {
		return String();
	}
	const int64_t end = p_line.find("\"", at + start.length());
	return end < 0 ? String() : p_line.substr(at + start.length(), end - at - start.length());
}

} // namespace

// The class a file loads as, from the editor's file system, which knows
// imported files too, or from the file: the header of a text resource,
// `[gd_resource type="Resource" script_class="Weapon" …]`, or the remap of an
// imported one. The script class comes from the header, which the editor
// keeps but does not expose (editor/file_system/editor_file_system.cpp:203-206).
std::optional<compiler::ResourceInfo> SceneIndex::resource(const std::string &p_path) const {
	const String path = to_godot(p_path);
	if (!FileAccess::file_exists(path)) {
		return std::nullopt;
	}
	compiler::ResourceInfo info;
	EditorInterface *editor = EditorInterface::get_singleton();
	if (editor && editor->get_resource_filesystem()) {
		info.type = to_utf8(editor->get_resource_filesystem()->get_file_type(path));
	}
	String header;
	if (path.get_extension() == "tres") {
		header = FileAccess::get_file_as_string(path).get_slicec('\n', 0);
	} else if (FileAccess::file_exists(path + String(".import"))) {
		const PackedStringArray lines = FileAccess::get_file_as_string(path + String(".import")).split("\n");
		for (int64_t i = 0; i < lines.size() && header.is_empty(); i++) {
			if (lines[i].begins_with("type=")) {
				header = lines[i];
			}
		}
	}
	if (info.type.empty()) {
		info.type = to_utf8(attribute(header, "type"));
	}
	info.script_class = to_utf8(attribute(header, "script_class"));
	return info;
}

std::string SceneIndex::uid_path(const std::string &p_uid) const {
	return to_utf8(ResourceUID::uid_to_path(to_godot(p_uid)));
}

std::vector<std::string> SceneIndex::dependencies(const std::string &p_path) const {
	std::vector<std::string> paths;
	// A path, or "uid::::path" with the path as it was saved, which the uid
	// overrides when the file moved (doc/classes/ResourceLoader.xml, get_dependencies).
	const PackedStringArray listed = ResourceLoader::get_singleton()->get_dependencies(to_godot(p_path));
	for (int64_t i = 0; i < listed.size(); i++) {
		const String &dependency = listed[i];
		const bool with_uid = dependency.contains("::");
		String path = with_uid ? dependency.get_slice("::", 2) : dependency;
		if (with_uid && !FileAccess::file_exists(path)) {
			path = ResourceUID::uid_to_path(dependency.get_slice("::", 0));
		}
		paths.push_back(to_utf8(path));
	}
	return paths;
}

std::string SceneIndex::global_class_path(const std::string &p_name) const {
	const StringName name = to_godot(p_name);
	const TypedArray<Dictionary> classes = ProjectSettings::get_singleton()->get_global_class_list();
	for (int64_t i = 0; i < classes.size(); i++) {
		const Dictionary entry = classes[i];
		if (StringName(entry.get("class", StringName())) == name) {
			return to_utf8(entry.get("path", String()));
		}
	}
	return std::string();
}

std::string SceneIndex::autoload_path(const std::string &p_name) const {
	// "*res://scripts/audio.gd": the star makes it a global name.
	const String setting = "autoload/" + to_godot(p_name);
	ProjectSettings *settings = ProjectSettings::get_singleton();
	if (!settings->has_setting(setting)) {
		return std::string();
	}
	const String value = settings->get_setting(setting);
	return value.begins_with("*") ? to_utf8(value.substr(1)) : std::string();
}

std::map<std::string, std::string> SceneIndex::global_class_paths() const {
	std::map<std::string, std::string> paths;
	const TypedArray<Dictionary> classes = ProjectSettings::get_singleton()->get_global_class_list();
	for (int64_t i = 0; i < classes.size(); i++) {
		const Dictionary entry = classes[i];
		paths[to_utf8(entry.get("class", String()))] = to_utf8(entry.get("path", String()));
	}
	return paths;
}

std::vector<std::string> SceneIndex::autoload_names() const {
	std::vector<std::string> names;
	const TypedArray<Dictionary> properties = ProjectSettings::get_singleton()->get_property_list();
	for (int64_t i = 0; i < properties.size(); i++) {
		const String name = Dictionary(properties[i]).get("name", String());
		if (name.begins_with("autoload/") && !autoload_path(to_utf8(name.trim_prefix("autoload/"))).empty()) {
			names.push_back(to_utf8(name.trim_prefix("autoload/")));
		}
	}
	return names;
}

// As GDScript lists the paths it completes
// (modules/gdscript/gdscript_editor.cpp:954-970).
std::vector<std::string> SceneIndex::resource_files() const {
	std::vector<std::string> paths;
	const auto collect = [&](EditorFileSystemDirectory *p_dir, const auto &p_self) -> void {
		for (int64_t i = 0; i < p_dir->get_file_count(); i++) {
			paths.push_back(to_utf8(p_dir->get_file_path(i)));
		}
		for (int64_t i = 0; i < p_dir->get_subdir_count(); i++) {
			p_self(p_dir->get_subdir(i), p_self);
		}
	};
	EditorInterface *editor = EditorInterface::get_singleton();
	if (EditorFileSystemDirectory *root = editor && editor->get_resource_filesystem() ? editor->get_resource_filesystem()->get_filesystem() : nullptr) {
		collect(root, collect);
	}
	return paths;
}

std::vector<std::string> SceneIndex::input_actions() const {
	std::vector<std::string> names;
	const TypedArray<Dictionary> properties = ProjectSettings::get_singleton()->get_property_list();
	for (int64_t i = 0; i < properties.size(); i++) {
		const String name = Dictionary(properties[i]).get("name", String());
		if (name.begins_with("input/")) {
			names.push_back(to_utf8(name.trim_prefix("input/")));
		}
	}
	return names;
}

std::optional<compiler::ForeignClass> SceneIndex::foreign_class(const std::string &p_path, bool p_members) const {
	const String path = to_godot(p_path);
	if (!FileAccess::file_exists(path)) {
		return std::nullopt;
	}
	compiler::ForeignClass result;
	if (!p_members) {
		// The global class list says what each global class extends, without
		// loading its script.
		const TypedArray<Dictionary> classes = ProjectSettings::get_singleton()->get_global_class_list();
		const auto entry_where = [&](const char *p_key, const Variant &p_value) {
			for (int64_t i = 0; i < classes.size(); i++) {
				const Dictionary entry = classes[i];
				if (entry.get(p_key, Variant()) == p_value) {
					return entry;
				}
			}
			return Dictionary();
		};
		Dictionary entry = entry_where("path", path);
		result.name = to_utf8(entry.get("class", String()));
		if (entry.is_empty()) {
			// A script without a class_name: the editor's file system read what
			// it extends from the file, a native or a global class, as for global
			// classes (editor/file_system/editor_file_system.cpp:2102-2110; ed1daf0).
			EditorInterface *editor = EditorInterface::get_singleton();
			EditorFileSystem *files = editor ? editor->get_resource_filesystem() : nullptr;
			EditorFileSystemDirectory *dir = files ? files->get_filesystem_path(path.get_base_dir()) : nullptr;
			const int64_t index = dir ? dir->find_file_index(path.get_file()) : -1;
			if (index >= 0) {
				entry["base"] = StringName(dir->get_file_script_class_extends(index));
			}
		}
		while (!entry.is_empty()) {
			const StringName base = entry.get("base", StringName());
			if (ClassDBSingleton::get_singleton()->class_exists(base)) {
				result.native = to_utf8(base);
				break;
			}
			entry = entry_where("class", base);
			if (result.base.empty() && !entry.is_empty()) {
				result.base = to_utf8(entry.get("path", String()));
			}
		}
		return result;
	}
	const Ref<Script> script = ResourceLoader::get_singleton()->load(path, "Script");
	if (script.is_null()) {
		return std::nullopt;
	}
	result.name = to_utf8(script->get_global_name());
	result.native = to_utf8(script->get_instance_base_type());
	const Ref<Script> base = script->get_base_script();
	if (base.is_valid()) {
		result.base = to_utf8(base->get_path());
	}
	const TypedArray<Dictionary> properties = script->get_script_property_list();
	for (int64_t i = 0; i < properties.size(); i++) {
		const Dictionary info = properties[i];
		// Members; the list also has the groups and categories of exports.
		if (uint32_t(int(info.get("usage", 0))) & PROPERTY_USAGE_SCRIPT_VARIABLE) {
			result.properties.push_back({ to_utf8(info.get("name", String())), type_of(info, false), true, p_path });
		}
	}
	const TypedArray<Dictionary> methods = script->get_script_method_list();
	for (int64_t i = 0; i < methods.size(); i++) {
		result.methods.push_back(method_of(methods[i], p_path));
	}
	const TypedArray<Dictionary> signals = script->get_script_signal_list();
	for (int64_t i = 0; i < signals.size(); i++) {
		result.signals.push_back(method_of(signals[i], p_path));
	}
	const Dictionary constants = script->get_script_constant_map();
	const Array names = constants.keys();
	for (int64_t i = 0; i < names.size(); i++) {
		result.constants.push_back({ to_utf8(names[i]), value_type(constants[names[i]]), false, p_path });
	}
	result.members_known = true;
	return result;
}

std::optional<std::string> SceneIndex::script_source(const std::string &p_path) const {
	const String path = to_godot(p_path);
	if (!FileAccess::file_exists(path)) {
		return std::nullopt;
	}
	return to_utf8(FileAccess::get_file_as_string(path));
}

} // namespace petrichor
