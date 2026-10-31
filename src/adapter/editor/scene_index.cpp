#include "adapter/editor/scene_index.h"

#include "adapter/editor/class_db_api.h"
#include "adapter/editor/project_files.h"
#include "adapter/pet_compiled.h"
#include "adapter/pet_script.h"
#include "runtime/text.h"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/scene_state.hpp>
#include <godot_cpp/classes/script.hpp>

#include <algorithm>
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

// The script a scene attaches to the node, if any.
String attached_script(const Ref<SceneState> &p_state, int32_t p_index) {
	for (int32_t p = 0; p < p_state->get_node_property_count(p_index); p++) {
		if (p_state->get_node_property_name(p_index, p) == StringName("script")) {
			const Ref<Script> script = p_state->get_node_property_value(p_index, p);
			return script.is_valid() ? script->get_path() : String();
		}
	}
	return String();
}

// The script an instanced scene's root has, through instances of instances.
String instanced_script(const Ref<SceneState> &p_state, int32_t p_index) {
	const Ref<PackedScene> instance = p_state->get_node_instance(p_index);
	if (instance.is_null() || instance->get_state().is_null() || instance->get_state()->get_node_count() == 0) {
		return String();
	}
	const String script = attached_script(instance->get_state(), 0);
	return script.is_empty() ? instanced_script(instance->get_state(), 0) : script;
}

// The paths of the scenes a scene instances, through instances of instances.
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
		const std::string script = to_utf8(attached_script(p_state, i));
		const auto inherited = std::find_if(scene.nodes.begin(), scene.nodes.end(), [&](const compiler::SceneNode &p_node) { return p_node.path == path; });
		if (inherited != scene.nodes.end()) {
			if (!script.empty()) {
				inherited->script = script;
				inherited->instance_script.clear();
			}
			continue;
		}
		compiler::SceneNode node;
		node.path = path;
		node.type = to_utf8(node_type(p_state, i));
		node.is_instance = p_state->get_node_instance(i).is_valid();
		node.script = script;
		if (node.is_instance && node.script.empty()) {
			node.instance_script = to_utf8(instanced_script(p_state, i));
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
	const Ref<PackedScene> packed = ResourceLoader::get_singleton()->load(path, "PackedScene");
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

std::vector<compiler::Scene> SceneIndex::scenes_where(bool (*p_uses)(const compiler::SceneNode &, const std::string &), const std::string &p_script_path) const {
	const PackedStringArray paths = project_files(PackedStringArray({ "tscn", "scn" }));
	std::vector<compiler::Scene> result;
	for (const String &path : paths) {
		const std::shared_ptr<const Entry> entry = read(to_utf8(path));
		if (std::any_of(entry->scene.nodes.begin(), entry->scene.nodes.end(), [&](const compiler::SceneNode &p_node) { return p_uses(p_node, p_script_path); })) {
			result.push_back(entry->scene);
		}
	}
	return result;
}

// A scene with a script that extends this one uses it too.
static bool is_or_extends(const std::string &p_script, const std::string &p_base) {
	const String script = to_godot(p_script);
	return p_script == p_base || (script.get_extension() == "pet" && header_extends(parse_header(FileAccess::get_file_as_string(script)), script, to_godot(p_base)));
}

std::vector<compiler::Scene> SceneIndex::scenes_with_script(const std::string &p_script_path) const {
	return scenes_where([](const compiler::SceneNode &p_node, const std::string &p_script) {
		return is_or_extends(p_node.script, p_script);
	},
			p_script_path);
}

std::vector<compiler::Scene> SceneIndex::scenes_instancing_script(const std::string &p_script_path) const {
	return scenes_where([](const compiler::SceneNode &p_node, const std::string &p_script) {
		return p_node.is_instance && p_node.script.empty() && is_or_extends(p_node.instance_script, p_script);
	},
			p_script_path);
}

std::optional<compiler::Scene> SceneIndex::scene(const std::string &p_path) const {
	if (!FileAccess::file_exists(to_godot(p_path))) {
		return std::nullopt;
	}
	const std::shared_ptr<const Entry> entry = read(p_path);
	return entry->scene.nodes.empty() ? std::nullopt : std::optional<compiler::Scene>(entry->scene);
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
		for (Dictionary entry = entry_where("path", path); !entry.is_empty();) {
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
