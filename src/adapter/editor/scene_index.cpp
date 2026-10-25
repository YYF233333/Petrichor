#include "adapter/editor/scene_index.h"

#include "adapter/editor/project_files.h"
#include "adapter/pet_compiled.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/scene_state.hpp>
#include <godot_cpp/classes/script.hpp>

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

compiler::Scene read_scene(const String &p_path, const Ref<SceneState> &p_state) {
	compiler::Scene scene;
	scene.path = to_utf8(p_path);
	for (int32_t i = 0; i < p_state->get_node_count(); i++) {
		compiler::SceneNode node;
		node.path = node_path(p_state->get_node_path(i));
		node.type = to_utf8(node_type(p_state, i));
		node.is_instance = p_state->get_node_instance(i).is_valid();
		for (int32_t p = 0; p < p_state->get_node_property_count(i); p++) {
			if (p_state->get_node_property_name(i, p) == StringName("script")) {
				const Ref<Script> script = p_state->get_node_property_value(i, p);
				if (script.is_valid()) {
					node.script = to_utf8(script->get_path());
				}
			}
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

std::vector<compiler::Scene> SceneIndex::scenes_with_script(const std::string &p_script_path) const {
	const PackedStringArray paths = project_files(PackedStringArray({ "tscn", "scn" }));
	std::vector<compiler::Scene> result;
	std::lock_guard<std::mutex> lock(mutex);
	for (const String &path : paths) {
		const std::string key = to_utf8(path);
		const uint64_t modified = FileAccess::get_modified_time(path);
		auto found = scenes.find(key);
		if (found == scenes.end() || found->second.modified != modified) {
			Entry entry;
			entry.modified = modified;
			const Ref<PackedScene> packed = ResourceLoader::get_singleton()->load(path, "PackedScene");
			if (packed.is_valid() && packed->get_state().is_valid()) {
				entry.scene = read_scene(path, packed->get_state());
			}
			found = scenes.insert_or_assign(key, entry).first;
		}
		for (const compiler::SceneNode &node : found->second.scene.nodes) {
			if (node.script == p_script_path) {
				result.push_back(found->second.scene);
				break;
			}
		}
	}
	return result;
}

} // namespace petrichor
