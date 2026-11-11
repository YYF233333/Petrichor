#include "adapter/editor/scene_index.h"

#include "adapter/editor/class_db_api.h"
#include "adapter/editor/project_files.h"
#include "adapter/pet_compiled.h"
#include "adapter/pet_script.h"
#include "runtime/text.h"

#include <godot_cpp/classes/animation.hpp>
#include <godot_cpp/classes/animation_library.hpp>
#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_file_system_directory.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/resource_uid.hpp>
#include <godot_cpp/classes/scene_replication_config.hpp>
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

// A value as the checks of exports read it (spec §4.3, §7.4); a float is
// whole as spec §10.8 says.
compiler::SceneValue scene_value(const Variant &p_value) {
	compiler::SceneValue value;
	const Object *object = p_value.get_type() == Variant::OBJECT ? p_value.get_validated_object() : nullptr;
	value.type = to_utf8(Variant::get_type_name(p_value.get_type() == Variant::OBJECT && !object ? Variant::NIL : p_value.get_type()));
	switch (p_value.get_type()) {
		case Variant::INT:
			value.integer = p_value;
			break;
		case Variant::FLOAT: {
			const double number = p_value;
			value.whole = number >= -9223372036854775808.0 && number < 9223372036854775808.0 && number == double(int64_t(number));
			value.integer = value.whole ? int64_t(number) : 0;
			break;
		}
		case Variant::STRING:
		case Variant::STRING_NAME:
			value.text = to_utf8(String(p_value));
			break;
		case Variant::OBJECT:
			if (object) {
				const Ref<Script> script = object->get_script();
				const Resource *resource = Object::cast_to<Resource>(object);
				value.class_name = to_utf8(object->get_class());
				value.script = script.is_valid() ? to_utf8(script->get_path()) : std::string();
				value.path = resource ? to_utf8(resource->get_path()) : std::string();
			}
			break;
		case Variant::ARRAY: {
			const Array array = p_value;
			for (int64_t i = 0; i < array.size(); i++) {
				value.items.push_back(scene_value(array[i]));
			}
			break;
		}
		case Variant::DICTIONARY: {
			const Dictionary dictionary = p_value;
			const Array keys = dictionary.keys();
			for (int64_t i = 0; i < keys.size(); i++) {
				value.items.push_back(scene_value(keys[i]));
				value.items.push_back(scene_value(dictionary[keys[i]]));
			}
			break;
		}
		default:
			break;
	}
	return value;
}

// What the scene itself stores for the properties of the node.
std::vector<std::pair<std::string, compiler::SceneValue>> stored_properties(const Ref<SceneState> &p_state, int32_t p_index) {
	std::vector<std::pair<std::string, compiler::SceneValue>> properties;
	for (int32_t p = 0; p < p_state->get_node_property_count(p_index); p++) {
		const StringName name = p_state->get_node_property_name(p_index, p);
		if (name != StringName("script")) {
			properties.emplace_back(to_utf8(name), scene_value(p_state->get_node_property_value(p_index, p)));
		}
	}
	return properties;
}

// The path from a node to the node that `p_path` names from `p_root`, itself
// a path from that node; nullopt from an empty root, which finds no node
// (scene/main/node.cpp:1906-1908; ed1daf0), and for an absolute path.
std::optional<std::string> joined(const NodePath &p_root, const NodePath &p_path) {
	if (p_root.is_empty() || p_root.is_absolute() || p_path.is_absolute()) {
		return std::nullopt;
	}
	const String names = p_path.get_concatenated_names();
	return to_utf8(String(p_root.get_concatenated_names()) + (names.is_empty() ? String() : "/" + names));
}

// What a node of a scene names for the engine to find by name (spec §7.7):
// the members that the tracks of an AnimationMixer's animations name from
// its root node, and the properties that a MultiplayerSynchronizer
// synchronizes from its root path, both ".." unless set. A property track
// or a synchronized property names the node's property by its first
// subname (scene/animation/animation_mixer.cpp:74-91, 722, 2077-2093, 2547;
// scene/main/node.cpp:3317-3340; modules/multiplayer/multiplayer_synchronizer.cpp:37-44,
// 164-180; multiplayer_synchronizer.h:55; ed1daf0). The files of libraries,
// animations and configurations stored apart go to `r_files`.
void read_named_members(const Ref<SceneState> &p_state, int32_t p_index, compiler::Scene &r_scene, std::set<std::string> &r_files) {
	const auto root = [&](const char *p_property) {
		bool given = false;
		const Variant path = node_property(p_state, p_index, p_property, &given);
		return given ? NodePath(path) : NodePath("..");
	};
	const NodePath root_node = root("root_node");
	const auto add = [&](const std::string &p_animation, const NodePath &p_root, const NodePath &p_path, const StringName &p_name, bool p_method) {
		const std::optional<std::string> node = joined(p_root, p_path);
		if (!node) {
			return;
		}
		const compiler::SceneNamedMember named{ node_path(p_state->get_node_path(p_index)), p_animation, *node, to_utf8(p_name), p_method };
		if (std::find(r_scene.named_members.begin(), r_scene.named_members.end(), named) == r_scene.named_members.end()) {
			r_scene.named_members.push_back(named);
		}
	};
	const auto watch = [&](const Ref<Resource> &p_resource) {
		const String path = p_resource->get_path();
		if (path.begins_with("res://") && !path.contains("::")) {
			r_files.insert(to_utf8(path));
		}
	};
	const auto read_library = [&](const String &p_name, const Ref<AnimationLibrary> &p_library) {
		if (p_library.is_null()) {
			return;
		}
		watch(p_library);
		const TypedArray<StringName> animations = p_library->get_animation_list();
		for (int64_t a = 0; a < animations.size(); a++) {
			const Ref<Animation> animation = p_library->get_animation(animations[a]);
			const String name = animations[a];
			const std::string shown = to_utf8(p_name.is_empty() ? name : p_name + String("/") + name);
			watch(animation);
			for (int32_t t = 0; t < animation->get_track_count(); t++) {
				const NodePath path = animation->track_get_path(t);
				const Animation::TrackType type = animation->track_get_type(t);
				if ((type == Animation::TYPE_VALUE || type == Animation::TYPE_BEZIER) && path.get_subname_count() > 0) {
					add(shown, root_node, path, path.get_subname(0), false);
				}
				for (int32_t k = 0; type == Animation::TYPE_METHOD && k < animation->track_get_key_count(t); k++) {
					add(shown, root_node, path, animation->method_track_get_name(t, k), true);
				}
			}
		}
	};
	for (int32_t p = 0; p < p_state->get_node_property_count(p_index); p++) {
		const String name = p_state->get_node_property_name(p_index, p);
		const Variant value = p_state->get_node_property_value(p_index, p);
		if (name == "libraries") {
			const Dictionary libraries = value;
			const Array keys = libraries.keys();
			for (int64_t k = 0; k < keys.size(); k++) {
				read_library(keys[k], libraries[keys[k]]);
			}
		} else if (name.begins_with("libraries/")) {
			read_library(name.trim_prefix("libraries/"), value);
		} else if (name == "replication_config") {
			const Ref<SceneReplicationConfig> config = value;
			if (config.is_null()) {
				continue;
			}
			watch(config);
			const NodePath root_path = root("root_path");
			const TypedArray<NodePath> properties = config->get_properties();
			for (int64_t q = 0; q < properties.size(); q++) {
				const NodePath property = properties[q];
				if (property.get_subname_count() > 0) {
					add(std::string(), root_path, property, property.get_subname(0), false);
				}
			}
		}
	}
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
compiler::Scene read_scene(const String &p_path, const Ref<SceneState> &p_state, std::set<std::string> &r_files) {
	const Ref<SceneState> base = p_state->get_base_scene_state();
	compiler::Scene scene = base.is_valid() ? read_scene(p_state->get_node_instance(0)->get_path(), base, r_files) : compiler::Scene();
	scene.base = base.is_valid() ? scene.path : std::string();
	scene.path = to_utf8(p_path);
	for (compiler::SceneNode &node : scene.nodes) {
		node.properties.clear(); // The base stores them, and the check reads them there.
	}
	for (int32_t i = 0; i < p_state->get_node_count(); i++) {
		read_named_members(p_state, i, scene, r_files);
		const std::string path = node_path(p_state->get_node_path(i));
		const std::optional<std::string> script = attached_script(p_state, i);
		const auto inherited = std::find_if(scene.nodes.begin(), scene.nodes.end(), [&](const compiler::SceneNode &p_node) { return p_node.path == path; });
		// For a script of this language, whose exports the check reads, and for
		// a change of an instanced scene's node, whose script that scene gives.
		// Of other nodes, the scenes they hold, which are used as roots (spec §7.5).
		const auto store = [&](compiler::SceneNode &r_node) {
			const std::string &attached = r_node.script.empty() ? r_node.instance_script : r_node.script;
			const bool change = r_node.type.empty() && !r_node.is_instance;
			r_node.properties = stored_properties(p_state, i);
			if (!change && to_godot(attached).get_extension() != "pet") {
				r_node.properties.erase(std::remove_if(r_node.properties.begin(), r_node.properties.end(), [](const auto &p_property) {
					std::vector<std::string> scenes;
					p_property.second.add_scenes(scenes);
					return scenes.empty();
				}),
						r_node.properties.end());
			}
		};
		if (inherited != scene.nodes.end()) {
			if (script) {
				inherited->script = *script;
				inherited->instance_script.clear();
			}
			const Variant unique = node_property(p_state, i, "unique_name_in_owner");
			if (unique.get_type() == Variant::BOOL) {
				inherited->unique = unique;
			}
			store(*inherited);
			continue;
		}
		compiler::SceneNode node;
		node.path = path;
		node.type = to_utf8(node_type(p_state, i));
		node.is_instance = p_state->get_node_instance(i).is_valid();
		node.instance = node.is_instance ? to_utf8(p_state->get_node_instance(i)->get_path()) : std::string();
		node.script = script.value_or(std::string());
		// A node of an instanced scene that this scene only changes has the
		// instance's root as its owner, and its unique name is that root's
		// (scene/resources/packed_scene.cpp:567-573, 1888-1894; ed1daf0).
		node.unique = (node.is_instance || !p_state->get_node_type(i).is_empty()) && bool(node_property(p_state, i, "unique_name_in_owner"));
		if (node.is_instance && !script) {
			node.instance_script = instanced_script(p_state, i);
		}
		store(node);
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
	if (cached && cached->modified == modified && std::all_of(cached->files.begin(), cached->files.end(), [](const auto &p_file) {
			return FileAccess::get_modified_time(to_godot(p_file.first)) == p_file.second;
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
		std::set<std::string> files;
		entry->scene = read_scene(path, packed->get_state(), files);
		instanced_scenes(packed->get_state(), files);
		for (const std::string &file : files) {
			entry->files.emplace_back(file, FileAccess::get_modified_time(to_godot(file)));
		}
	}
	std::lock_guard<std::mutex> lock(mutex);
	scenes.insert_or_assign(p_path, entry);
	return entry;
}

void SceneIndex::forget(const std::string &p_path) const {
	std::lock_guard<std::mutex> lock(mutex);
	for (auto entry = scenes.begin(); entry != scenes.end();) {
		const auto &files = entry->second->files;
		if (entry->first == p_path || std::any_of(files.begin(), files.end(), [&](const auto &p_file) { return p_file.first == p_path; })) {
			entry = scenes.erase(entry);
		} else {
			++entry;
		}
	}
}

std::vector<std::string> SceneIndex::scene_paths() const {
	std::vector<std::string> paths;
	for (const String &path : project_files(PackedStringArray({ "tscn", "scn" }))) {
		paths.push_back(to_utf8(path));
	}
	return paths;
}

std::optional<compiler::Scene> SceneIndex::scene(const std::string &p_path) const {
	if (!FileAccess::file_exists(to_godot(p_path))) {
		return std::nullopt;
	}
	const std::shared_ptr<const Entry> entry = read(p_path);
	return entry->scene.nodes.empty() ? std::nullopt : std::optional<compiler::Scene>(entry->scene);
}

// The setting may hold a uid:// path (main/main.cpp:4336-4345; ed1daf0).
std::string SceneIndex::main_scene() const {
	return to_utf8(ResourceUID::ensure_path(ProjectSettings::get_singleton()->get_setting("application/run/main_scene", String())));
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

std::vector<std::string> SceneIndex::script_paths() const {
	std::vector<std::string> paths;
	for (const String &path : project_files(PackedStringArray({ "pet", "gd" }))) {
		paths.push_back(to_utf8(path));
	}
	return paths;
}

std::optional<std::string> SceneIndex::script_source(const std::string &p_path) const {
	const String path = to_godot(p_path);
	if (!FileAccess::file_exists(path)) {
		return std::nullopt;
	}
	return to_utf8(FileAccess::get_file_as_string(path));
}

} // namespace petrichor
