#include "adapter/editor/scene_index.h"

#include "adapter/editor/class_db_api.h"
#include "adapter/editor/project_files.h"
#include "adapter/pet_compiled.h"
#include "adapter/pet_placeholder.h"
#include "adapter/pet_script.h"
#include "runtime/text.h"

#include <godot_cpp/classes/animation.hpp>
#include <godot_cpp/classes/animation_library.hpp>
#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_file_system_directory.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/classes/resource_uid.hpp>
#include <godot_cpp/classes/scene_replication_config.hpp>
#include <godot_cpp/classes/scene_state.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/script_editor.hpp>
#include <godot_cpp/classes/script_editor_base.hpp>

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

// The type of a typed container's elements or keys, as a value's.
compiler::SceneValue typing(int64_t p_builtin, const StringName &p_class, const Variant &p_script) {
	compiler::SceneValue typed;
	typed.type = to_utf8(Variant::get_type_name(Variant::Type(p_builtin)));
	typed.class_name = to_utf8(p_class);
	const Ref<Script> script = p_script;
	typed.script = script.is_valid() ? to_utf8(script->get_path()) : std::string();
	return typed;
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
		case Variant::NODE_PATH:
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
			if (array.is_typed()) {
				value.typed.push_back(typing(array.get_typed_builtin(), array.get_typed_class_name(), array.get_typed_script()));
			}
			for (int64_t i = 0; i < array.size(); i++) {
				value.items.push_back(scene_value(array[i]));
			}
			break;
		}
		case Variant::DICTIONARY: {
			const Dictionary dictionary = p_value;
			if (dictionary.is_typed()) {
				value.typed.push_back(typing(dictionary.get_typed_key_builtin(), dictionary.get_typed_key_class_name(), dictionary.get_typed_key_script()));
				value.typed.push_back(typing(dictionary.get_typed_value_builtin(), dictionary.get_typed_value_class_name(), dictionary.get_typed_value_script()));
			}
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

compiler::Scene read_scene(const String &p_path, const Ref<PackedScene> &p_scene, std::set<std::string> &r_files, std::vector<compiler::Scene> &r_embedded);

// The resources of the file at `p_file` that `p_value` holds at any depth: its
// own and its sub-resources, but not those of other files, which are checked
// with them. Those with a script of this language, whose files go to
// `r_files` as their properties come through it, or that hold scenes. The
// engine sets each property the file stores, and saves only those that differ
// from the default (scene/resources/resource_format_text.cpp:640-716,
// 1955-1994; ed1daf0); a placeholder keeps them so. A scene among them, as a
// scene file embeds one, goes to `r_embedded`, read as a scene of its own;
// `p_file` may be such a scene's path.
void read_resources(const String &p_file, const Variant &p_value, compiler::Scene &r_scene, std::set<std::string> &r_files, std::set<uint64_t> &r_seen, std::vector<compiler::Scene> &r_embedded) {
	if (p_value.get_type() == Variant::ARRAY) {
		const Array array = p_value;
		for (int64_t i = 0; i < array.size(); i++) {
			read_resources(p_file, array[i], r_scene, r_files, r_seen, r_embedded);
		}
		return;
	}
	if (p_value.get_type() == Variant::DICTIONARY) {
		const Dictionary dictionary = p_value;
		const Array keys = dictionary.keys();
		for (int64_t i = 0; i < keys.size(); i++) {
			read_resources(p_file, keys[i], r_scene, r_files, r_seen, r_embedded);
			read_resources(p_file, dictionary[keys[i]], r_scene, r_files, r_seen, r_embedded);
		}
		return;
	}
	Resource *resource = p_value.get_type() == Variant::OBJECT ? Object::cast_to<Resource>(p_value.get_validated_object()) : nullptr;
	const String path = resource ? resource->get_path() : String();
	const String file = p_file.get_slice("::", 0);
	if (!resource || (path != file && !path.begins_with(file + String("::"))) || !r_seen.insert(resource->get_instance_id()).second) {
		return;
	}
	if (const Ref<PackedScene> scene = Ref<PackedScene>(Object::cast_to<PackedScene>(resource)); scene.is_valid()) {
		if (scene->get_state().is_valid()) {
			r_embedded.push_back(read_scene(path, scene, r_files, r_embedded));
		}
		return;
	}
	const Ref<Script> script = resource->get_script();
	compiler::SceneResource read;
	read.path = to_utf8(path);
	read.script = script.is_valid() ? to_utf8(script->get_path()) : std::string();
	const bool ours = to_godot(read.script).get_extension() == "pet";
	if (ours) {
		r_files.insert(read.script);
	}
	// Its placeholder then lists no property the file stores.
	const PetScript *pet = Object::cast_to<PetScript>(script.ptr());
	read.unread = pet && !pet->_is_valid();
	const TypedArray<Dictionary> properties = resource->get_property_list();
	for (int64_t i = 0; i < properties.size(); i++) {
		const Dictionary property = properties[i];
		const String name = property.get("name", String());
		const uint32_t usage = uint32_t(int(property.get("usage", 0)));
		if (!(usage & PROPERTY_USAGE_STORAGE) || name == "script") {
			continue;
		}
		const Variant value = resource->get(name);
		// A placeholder gives a property's default, which the file does not store.
		if (pet && PetPlaceholder::at_default(*pet, name, value)) {
			continue;
		}
		read_resources(p_file, value, r_scene, r_files, r_seen, r_embedded);
		compiler::SceneValue stored = scene_value(value);
		std::vector<std::string> scenes;
		stored.add_scenes(scenes);
		if (stored.type != "Nil" && ((ours && (usage & PROPERTY_USAGE_SCRIPT_VARIABLE)) || !scenes.empty())) {
			read.properties.emplace_back(to_utf8(name), std::move(stored));
		}
	}
	if (ours || !read.properties.empty()) {
		r_scene.resources.push_back(std::move(read));
	}
}

// Whether each property of each node holds nodes as NodePaths, by the
// indices of the node and the property, which the scene marks in the bundle
// it is saved as (scene/resources/packed_scene.cpp:1002, 1811-1831;
// packed_scene.h:126; ed1daf0).
std::vector<std::vector<bool>> node_references(const Ref<PackedScene> &p_scene) {
	const Dictionary bundle = p_scene->get("_bundled");
	const PackedInt32Array nodes = bundle.get("nodes", PackedInt32Array());
	std::vector<std::vector<bool>> references(size_t(int64_t(bundle.get("node_count", 0))));
	int64_t at = 0;
	for (std::vector<bool> &properties : references) {
		at += 5; // Its parent, owner, type, name and instance.
		for (int32_t count = nodes[at++]; count > 0; count--, at += 2) {
			properties.push_back((nodes[at] & (1 << 30)) != 0);
		}
		at += 1 + nodes[at]; // Its groups.
	}
	return references;
}

// What the scene itself stores for the properties of the node.
std::vector<std::pair<std::string, compiler::SceneValue>> stored_properties(const Ref<SceneState> &p_state, int32_t p_index, const std::vector<bool> &p_references) {
	std::vector<std::pair<std::string, compiler::SceneValue>> properties;
	for (int32_t p = 0; p < p_state->get_node_property_count(p_index); p++) {
		const StringName name = p_state->get_node_property_name(p_index, p);
		if (name != StringName("script")) {
			properties.emplace_back(to_utf8(name), scene_value(p_state->get_node_property_value(p_index, p)));
			properties.back().second.to_nodes = p_references[p];
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
// synchronizes from its root path, both ".." unless set. The animations
// include those under the `anims/` keys of 3.x, which the engine adds to the
// default library. A property track or a synchronized property names the
// node's property by its first subname
// (scene/animation/animation_mixer.cpp:60-91, 722, 2077-2093, 2547;
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
	const auto add = [&](const std::string &p_animation, const NodePath &p_root, const NodePath &p_path, const StringName &p_name, bool p_method, const compiler::ScenePlace &p_place) {
		const std::optional<std::string> node = joined(p_root, p_path);
		if (!node) {
			return;
		}
		const compiler::SceneNamedMember named{ node_path(p_state->get_node_path(p_index)), p_animation, *node, to_utf8(p_name), p_method };
		auto same = std::find_if(r_scene.named_members.begin(), r_scene.named_members.end(), [&](const compiler::SceneNamedMember &p_other) {
			return p_other.from == named.from && p_other.animation == named.animation && p_other.node == named.node && p_other.name == named.name && p_other.method == named.method;
		});
		if (same == r_scene.named_members.end()) {
			same = r_scene.named_members.insert(same, named);
		}
		same->places.push_back(p_place);
	};
	const auto watch = [&](const Ref<Resource> &p_resource) {
		const String path = p_resource->get_path();
		if (path.begins_with("res://") && !path.contains("::")) {
			r_files.insert(to_utf8(path));
		}
	};
	const auto read_animation = [&](const String &p_name, const Ref<Animation> &p_animation) {
		if (p_animation.is_null()) {
			return;
		}
		watch(p_animation);
		const std::string shown = to_utf8(p_name);
		const std::string resource = to_utf8(p_animation->get_path());
		for (int32_t t = 0; t < p_animation->get_track_count(); t++) {
			const NodePath path = p_animation->track_get_path(t);
			const Animation::TrackType type = p_animation->track_get_type(t);
			if ((type == Animation::TYPE_VALUE || type == Animation::TYPE_BEZIER) && path.get_subname_count() > 0) {
				add(shown, root_node, path, path.get_subname(0), false, { resource, t });
			}
			for (int32_t k = 0; type == Animation::TYPE_METHOD && k < p_animation->track_get_key_count(t); k++) {
				add(shown, root_node, path, p_animation->method_track_get_name(t, k), true, { resource, t, k });
			}
		}
	};
	const auto read_library = [&](const String &p_name, const Ref<AnimationLibrary> &p_library) {
		if (p_library.is_null()) {
			return;
		}
		watch(p_library);
		const TypedArray<StringName> animations = p_library->get_animation_list();
		for (int64_t a = 0; a < animations.size(); a++) {
			const String name = animations[a];
			read_animation(p_name.is_empty() ? name : p_name + String("/") + name, p_library->get_animation(name));
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
		} else if (name.begins_with("anims/")) {
			read_animation(name.get_slicec('/', 1), value);
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
					add(std::string(), root_path, property, property.get_subname(0), false, { to_utf8(config->get_path()), int(q) });
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
// 1888-1894, 1912-1928; ed1daf0). The scenes it embeds go to `r_embedded`.
compiler::Scene read_scene(const String &p_path, const Ref<PackedScene> &p_scene, std::set<std::string> &r_files, std::vector<compiler::Scene> &r_embedded) {
	const Ref<SceneState> state = p_scene->get_state();
	const Ref<PackedScene> base = state->get_base_scene_state().is_valid() ? state->get_node_instance(0) : Ref<PackedScene>();
	std::vector<compiler::Scene> based; // The base's own.
	compiler::Scene scene = base.is_valid() ? read_scene(base->get_path(), base, r_files, based) : compiler::Scene();
	scene.base = base.is_valid() ? scene.path : std::string();
	scene.path = to_utf8(p_path);
	// The base stores them, and the check reads them there.
	for (compiler::SceneNode &node : scene.nodes) {
		node.properties.clear();
	}
	scene.resources.clear();
	std::set<uint64_t> seen;
	const std::vector<std::vector<bool>> references = node_references(p_scene);
	for (int32_t i = 0; i < state->get_node_count(); i++) {
		for (int32_t p = 0; p < state->get_node_property_count(i); p++) {
			read_resources(p_path, state->get_node_property_value(i, p), scene, r_files, seen, r_embedded);
		}
		read_named_members(state, i, scene, r_files);
		const std::string path = node_path(state->get_node_path(i));
		const std::optional<std::string> script = attached_script(state, i);
		const auto inherited = std::find_if(scene.nodes.begin(), scene.nodes.end(), [&](const compiler::SceneNode &p_node) { return p_node.path == path; });
		// For a script of this language, whose exports the check reads, and for
		// a change of an instanced scene's node, whose script that scene gives.
		// Of other nodes, the scenes they hold, which are used as roots (spec §7.5).
		const auto store = [&](compiler::SceneNode &r_node) {
			const std::string &attached = r_node.script.empty() ? r_node.instance_script : r_node.script;
			const bool change = r_node.type.empty() && !r_node.is_instance;
			r_node.properties = stored_properties(state, i, references[i]);
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
			const Variant unique = node_property(state, i, "unique_name_in_owner");
			if (unique.get_type() == Variant::BOOL) {
				inherited->unique = unique;
			}
			store(*inherited);
			continue;
		}
		compiler::SceneNode node;
		node.path = path;
		node.type = to_utf8(node_type(state, i));
		node.is_instance = state->get_node_instance(i).is_valid();
		node.instance = node.is_instance ? to_utf8(state->get_node_instance(i)->get_path()) : std::string();
		node.script = script.value_or(std::string());
		// A node of an instanced scene that this scene only changes has the
		// instance's root as its owner, and its unique name is that root's
		// (scene/resources/packed_scene.cpp:567-573, 1888-1894; ed1daf0).
		node.unique = (node.is_instance || !state->get_node_type(i).is_empty()) && bool(node_property(state, i, "unique_name_in_owner"));
		if (node.is_instance && !script) {
			node.instance_script = instanced_script(state, i);
		}
		store(node);
		scene.nodes.push_back(node);
	}
	for (int32_t i = 0; i < state->get_connection_count(); i++) {
		compiler::SceneConnection connection;
		connection.signal = to_utf8(state->get_connection_signal(i));
		connection.source = node_path(state->get_connection_source(i));
		connection.target = node_path(state->get_connection_target(i));
		connection.method = to_utf8(state->get_connection_method(i));
		connection.binds = int(state->get_connection_binds(i).size());
		connection.unbinds = int(state->get_connection_unbinds(i));
		connection.file = scene.path;
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

// Checked since the last change on disk, or else by the stats of its files
// (architecture §2.1).
bool SceneIndex::current(const Read &p_read) const {
	uint64_t now;
	{
		std::lock_guard<std::mutex> lock(mutex);
		now = changes;
		if (p_read.checked == now) {
			return true;
		}
	}
	if (!std::all_of(p_read.files.begin(), p_read.files.end(), [](const auto &p_file) { return file_stat(p_file.first) == p_file.second; })) {
		return false;
	}
	std::lock_guard<std::mutex> lock(mutex);
	p_read.checked = now;
	return true;
}

void SceneIndex::stamp(Read &r_read, const std::set<std::string> &p_files) const {
	for (const std::string &file : p_files) {
		r_read.files.emplace_back(file, file_stat(file));
	}
	std::lock_guard<std::mutex> lock(mutex);
	r_read.checked = changes;
}

void SceneIndex::disk_changed() const {
	std::lock_guard<std::mutex> lock(mutex);
	changes++;
}

void SceneIndex::files_changed(const std::set<std::string> &p_files) const {
	const auto reads = [&](const Read &p_read) { return std::any_of(p_read.files.begin(), p_read.files.end(), [&](const auto &p_file) { return p_files.count(p_file.first) > 0; }); };
	const auto drop = [&](auto &r_reads) {
		for (auto entry = r_reads.begin(); entry != r_reads.end();) {
			entry = reads(*entry->second) ? r_reads.erase(entry) : std::next(entry);
		}
	};
	std::lock_guard<std::mutex> lock(mutex);
	drop(scenes);
	drop(loads);
	changes++;
}

std::shared_ptr<const SceneIndex::Entry> SceneIndex::read(const std::string &p_path) const {
	const String path = to_godot(p_path);
	std::shared_ptr<const Entry> cached;
	{
		std::lock_guard<std::mutex> lock(mutex);
		const auto found = scenes.find(p_path);
		if (found != scenes.end()) {
			cached = found->second;
		}
	}
	if (cached && current(*cached)) {
		return cached;
	}

	const std::shared_ptr<Entry> entry = std::make_shared<Entry>();
	// From the file: the editor keeps the scene it loaded once, such as one in
	// an exported property, after the file changes (core/io/resource_loader.cpp:800-803,
	// scene/resources/packed_scene.h:256).
	std::set<std::string> files{ p_path };
	const String extension = path.get_extension().to_lower();
	if (extension == "tscn" || extension == "scn") {
		const Ref<PackedScene> packed = ResourceLoader::get_singleton()->load(path, "PackedScene", ResourceLoader::CACHE_MODE_IGNORE);
		if (packed.is_valid() && packed->get_state().is_valid()) {
			entry->scene = read_scene(path, packed, files, entry->embedded);
			instanced_scenes(packed->get_state(), files);
		}
	} else {
		// Only a resource file that may hold something the check reads: its
		// scripts and the scenes it holds are external resources, which the
		// engine lists without loading it.
		const std::vector<std::string> dependencies = this->dependencies(p_path);
		if (std::any_of(dependencies.begin(), dependencies.end(), [](const std::string &p_dependency) {
				const String extension = to_godot(p_dependency).get_extension();
				return extension == "pet" || extension == "tscn" || extension == "scn";
			})) {
			std::set<uint64_t> seen;
			entry->scene.path = p_path;
			read_resources(path, ResourceLoader::get_singleton()->load(path, "", ResourceLoader::CACHE_MODE_IGNORE), entry->scene, files, seen, entry->embedded);
		}
	}
	stamp(*entry, files);
	std::lock_guard<std::mutex> lock(mutex);
	scenes.insert_or_assign(p_path, entry);
	return entry;
}

std::vector<std::string> SceneIndex::scene_paths() const {
	std::vector<std::string> paths;
	for (const String &path : project_files(PackedStringArray({ "tscn", "scn", "tres", "res" }))) {
		paths.push_back(to_utf8(path));
		const std::vector<std::string> embedded = embedded_scenes(paths.back());
		paths.insert(paths.end(), embedded.begin(), embedded.end());
	}
	return paths;
}

std::vector<std::string> SceneIndex::embedded_scenes(const std::string &p_file) const {
	std::vector<std::string> paths;
	for (const compiler::Scene &embedded : read(p_file)->embedded) {
		paths.push_back(embedded.path);
	}
	return paths;
}

std::optional<compiler::Scene> SceneIndex::scene(const std::string &p_path) const {
	const size_t embedded = p_path.find("::");
	if (!FileAccess::file_exists(to_godot(p_path.substr(0, embedded)))) {
		return std::nullopt;
	}
	const std::shared_ptr<const Entry> entry = read(p_path.substr(0, embedded));
	if (embedded != std::string::npos) {
		const auto found = std::find_if(entry->embedded.begin(), entry->embedded.end(), [&](const compiler::Scene &p_scene) { return p_scene.path == p_path; });
		return found == entry->embedded.end() ? std::nullopt : std::optional<compiler::Scene>(*found);
	}
	return entry->scene.nodes.empty() && entry->scene.resources.empty() ? std::nullopt : std::optional<compiler::Scene>(entry->scene);
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

// The editor stores a new list when a class changes. Each call copies the list
// into a new typed Array that shares its Dictionaries, so comparing it with
// the list read before compares a pointer per class
// (core/config/project_settings.cpp:1473-1480, core/variant/typed_array.h:43-50,
// core/variant/array.cpp:137-159, core/variant/dictionary.cpp:263-267; ed1daf0).
std::shared_ptr<const std::map<std::string, std::string>> SceneIndex::listed_classes() const {
	const Array listed = ProjectSettings::get_singleton()->get_global_class_list();
	std::lock_guard<std::mutex> lock(mutex);
	if (!global_list || *global_list != listed) {
		std::map<std::string, std::string> paths;
		for (int64_t i = 0; i < listed.size(); i++) {
			const Dictionary entry = listed[i];
			paths.emplace(to_utf8(entry.get("class", String())), to_utf8(entry.get("path", String())));
		}
		global_classes = std::make_shared<const std::map<std::string, std::string>>(std::move(paths));
	}
	global_list = listed;
	return global_classes;
}

std::string SceneIndex::global_class_path(const std::string &p_name) const {
	const std::shared_ptr<const std::map<std::string, std::string>> classes = listed_classes();
	const auto found = classes->find(p_name);
	return found == classes->end() ? std::string() : found->second;
}

std::string SceneIndex::autoload_path(const std::string &p_name) const {
	// "*res://scripts/audio.gd": the star makes it a global name. The editor
	// adds one by its uid, which loading resolves
	// (editor/settings/editor_autoload_settings.cpp:787, main/main.cpp:4524; ed1daf0).
	const String setting = "autoload/" + to_godot(p_name);
	ProjectSettings *settings = ProjectSettings::get_singleton();
	if (!settings->has_setting(setting)) {
		return std::string();
	}
	const String value = settings->get_setting(setting);
	return value.begins_with("*") ? to_utf8(ResourceUID::ensure_path(value.substr(1))) : std::string();
}

std::map<std::string, std::string> SceneIndex::global_class_paths() const {
	return *listed_classes();
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
	// Each script lists its own properties, methods and signals before those of
	// its base, and its own constants only (modules/gdscript/gdscript.cpp:296-309,
	// 315-352, 908-914, 963-971, 1291-1308; modules/mono/csharp_script.cpp:2509-2522,
	// 2678-2694, 2725-2751; ed1daf0).
	const auto script_properties = [](const Ref<Script> &p_script) {
		TypedArray<Dictionary> listed;
		const TypedArray<Dictionary> all = p_script.is_valid() ? p_script->get_script_property_list() : TypedArray<Dictionary>();
		for (int64_t i = 0; i < all.size(); i++) {
			const Dictionary info = all[i];
			// Members; the list also has the groups and categories of exports.
			if (uint32_t(int(info.get("usage", 0))) & PROPERTY_USAGE_SCRIPT_VARIABLE) {
				listed.push_back(info);
			}
		}
		return listed;
	};
	TypedArray<Dictionary> properties = script_properties(script);
	TypedArray<Dictionary> methods = script->get_script_method_list();
	TypedArray<Dictionary> signals = script->get_script_signal_list();
	std::set<const Script *> seen;
	for (Ref<Script> current = script; current.is_valid() && seen.insert(current.ptr()).second;) {
		const std::string owner = current == script ? p_path : to_utf8(current->get_path());
		const Ref<Script> next = current->get_base_script();
		const TypedArray<Dictionary> next_properties = script_properties(next);
		const TypedArray<Dictionary> next_methods = next.is_valid() ? next->get_script_method_list() : TypedArray<Dictionary>();
		const TypedArray<Dictionary> next_signals = next.is_valid() ? next->get_script_signal_list() : TypedArray<Dictionary>();
		for (int64_t i = 0; i < properties.size() - next_properties.size(); i++) {
			const Dictionary info = properties[i];
			result.properties.push_back({ to_utf8(info.get("name", String())), type_of(info, false), true, owner });
		}
		for (int64_t i = 0; i < methods.size() - next_methods.size(); i++) {
			result.methods.push_back(method_of(methods[i], owner));
		}
		for (int64_t i = 0; i < signals.size() - next_signals.size(); i++) {
			result.signals.push_back(method_of(signals[i], owner));
		}
		const Dictionary constants = current->get_script_constant_map();
		const Array names = constants.keys();
		for (int64_t i = 0; i < names.size(); i++) {
			const std::string name = to_utf8(names[i]);
			if (std::none_of(result.constants.begin(), result.constants.end(), [&](const compiler::EngineProperty &p_constant) { return p_constant.name == name; })) {
				result.constants.push_back({ name, value_type(constants[names[i]]), false, owner });
			}
		}
		current = next;
		properties = next_properties;
		methods = next_methods;
		signals = next_signals;
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

// Read again only when the file changed, as a scene is.
std::vector<std::pair<bool, std::string>> SceneIndex::literal_loads(const std::string &p_path) const {
	std::shared_ptr<const Loads> cached;
	{
		std::lock_guard<std::mutex> lock(mutex);
		const auto found = loads.find(p_path);
		if (found != loads.end()) {
			cached = found->second;
		}
	}
	if (cached && current(*cached)) {
		return cached->found;
	}
	const std::shared_ptr<Loads> read = std::make_shared<Loads>();
	read->found = ProjectApi::literal_loads(p_path);
	stamp(*read, { p_path });
	std::lock_guard<std::mutex> lock(mutex);
	loads.insert_or_assign(p_path, read);
	return read->found;
}

namespace {

// By path: the editor of each script open in the script editor, and the
// version of its text that the workspace has.
std::map<std::string, std::pair<uint64_t, uint32_t>> open_scripts;
std::set<std::string> packed_scenes;
// By path, the text of each LSP session that has it open, the one changed last
// at the end.
std::map<std::string, std::vector<std::pair<const void *, std::string>>> in_client;
// The MD5 of each file as the editor last wrote or read it.
std::map<std::string, std::string> seen;

// The scenes open with unsaved changes, by path.
std::map<std::string, Ref<PackedScene>> unsaved_scenes() {
	EditorInterface *editor = EditorInterface::get_singleton();
	const PackedStringArray unsaved = editor->get_unsaved_scenes();
	const TypedArray<Node> roots = editor->get_open_scene_roots();
	std::map<std::string, Ref<PackedScene>> packed;
	for (int64_t i = 0; i < roots.size(); i++) {
		Node *root = Object::cast_to<Node>(roots[i]);
		const String path = root->get_scene_file_path();
		Ref<PackedScene> scene;
		scene.instantiate();
		if (!path.is_empty() && unsaved.has(path) && scene->pack(root) == OK) {
			packed[to_utf8(path)] = scene;
		}
	}
	return packed;
}

} // namespace

std::string without_cr(std::string p_text) {
	p_text.erase(std::remove(p_text.begin(), p_text.end(), '\r'), p_text.end());
	return p_text;
}

compiler::Workspace &workspace() {
	static compiler::Workspace workspace(SceneIndex::get());
	EditorInterface *editor = EditorInterface::get_singleton();
	ScriptEditor *script_editor = editor ? editor->get_script_editor() : nullptr;
	if (!script_editor || workspace.in_pass()) {
		return workspace;
	}
	// The open scripts are those of the editors of ScriptTextEditor, in order;
	// the others edit text files (editor/script/script_editor_plugin.cpp:3549-3575,
	// script_text_editor.cpp:2788-2793, text_editor.cpp:68-73). An editor's
	// version goes up with each edit, and undoing or redoing one gives back the
	// version of its text (scene/gui/text_edit.cpp:4784-4848, 9193, 9233; ed1daf0).
	const TypedArray<Ref<Script>> scripts = script_editor->get_open_scripts();
	const TypedArray<ScriptEditorBase> editors = script_editor->get_open_script_editors();
	std::set<std::string> shown;
	int64_t index = 0;
	for (int64_t i = 0; i < editors.size(); i++) {
		const ScriptEditorBase *each = Object::cast_to<ScriptEditorBase>(editors[i]);
		if (each->get_class() != String("ScriptTextEditor")) {
			continue;
		}
		const Ref<Script> script = scripts[index++];
		CodeEdit *code_edit = Object::cast_to<CodeEdit>(each->get_base_editor());
		const String path = script->get_path();
		if (!code_edit || !path.begins_with("res://") || path.contains("::")) {
			continue;
		}
		const std::string file = to_utf8(path);
		const std::pair<uint64_t, uint32_t> now{ code_edit->get_instance_id(), code_edit->get_version() };
		shown.insert(file);
		if (open_scripts[file] != now) {
			open_scripts[file] = now;
			const std::string text = to_utf8(code_edit->get_text());
			if (!in_client.count(file)) {
				workspace.set_script(file, text);
			}
			// Saved as it opens, saves or reloads its file
			// (editor/script/script_text_editor.cpp:248-251,
			// editor/script/script_editor_plugin.cpp:723-728,
			// editor/script/script_editor_base.cpp:575-604; ed1daf0).
			if (code_edit->get_version() == code_edit->get_saved_version() && text == without_cr(SceneIndex::get().script_source(file).value_or(std::string()))) {
				file_seen(file);
			}
		}
	}
	for (auto entry = open_scripts.begin(); entry != open_scripts.end();) {
		if (shown.count(entry->first)) {
			++entry;
			continue;
		}
		if (!in_client.count(entry->first)) {
			workspace.drop_script(entry->first);
		}
		entry = open_scripts.erase(entry);
	}
	return workspace;
}

void pack_open_scenes() {
	compiler::Workspace &open = workspace();
	std::set<std::string> packed;
	for (const auto &[path, scene] : unsaved_scenes()) {
		std::set<std::string> files;
		std::vector<compiler::Scene> embedded;
		open.set_scene(path, read_scene(to_godot(path), scene, files, embedded));
		packed.insert(path);
		for (const compiler::Scene &each : embedded) {
			packed.insert(each.path);
			open.set_scene(each.path, each);
		}
	}
	for (const std::string &path : packed_scenes) {
		if (!packed.count(path)) {
			open.drop_scene(path);
		}
	}
	packed_scenes = std::move(packed);
}

CodeEdit *open_code_edit(const std::string &p_path) {
	workspace();
	const auto found = open_scripts.find(p_path);
	return found == open_scripts.end() ? nullptr : Object::cast_to<CodeEdit>(ObjectDB::get_instance(found->second.first));
}

std::set<std::string> client_scripts() {
	std::set<std::string> paths;
	for (const auto &[path, texts] : in_client) {
		paths.insert(path);
	}
	return paths;
}

namespace {

std::vector<std::pair<const void *, std::string>> &without(const std::string &p_path, const void *p_session) {
	std::vector<std::pair<const void *, std::string>> &texts = in_client[p_path];
	texts.erase(std::remove_if(texts.begin(), texts.end(), [&](const auto &p_text) { return p_text.first == p_session; }), texts.end());
	return texts;
}

} // namespace

void set_client_script(const std::string &p_path, const void *p_session, std::string p_text) {
	compiler::Workspace &open = workspace();
	without(p_path, p_session).emplace_back(p_session, p_text);
	open.set_script(p_path, std::move(p_text));
}

void drop_client_script(const std::string &p_path, const void *p_session) {
	compiler::Workspace &open = workspace();
	const std::vector<std::pair<const void *, std::string>> &texts = without(p_path, p_session);
	if (!texts.empty()) {
		open.set_script(p_path, texts.back().second);
		return;
	}
	in_client.erase(p_path);
	if (CodeEdit *code_edit = open_code_edit(p_path)) {
		open.set_script(p_path, to_utf8(code_edit->get_text()));
	} else {
		open.drop_script(p_path);
	}
}

bool client_unsaved(const std::string &p_path) {
	const auto found = in_client.find(p_path);
	if (found == in_client.end()) {
		return false;
	}
	const std::string file = without_cr(SceneIndex::get().script_source(p_path).value_or(std::string()));
	return std::any_of(found->second.begin(), found->second.end(), [&](const auto &p_text) { return without_cr(p_text.second) != file; });
}

void client_reloads(const std::string &p_path) {
	if (in_client.count(p_path)) {
		workspace().set_script(p_path, SceneIndex::get().script_source(p_path).value_or(std::string()));
	}
}

std::optional<std::string> file_text(const std::string &p_path) {
	String path = to_godot(p_path);
	const std::map<std::string, Ref<PackedScene>> unsaved = unsaved_scenes();
	const auto scene = unsaved.find(p_path);
	if (scene != unsaved.end()) {
		// Outside res://, so that the editor does not take it as a file of the
		// project; under .godot/, so that it gives it no uid
		// (core/io/resource_saver.cpp:146-147, editor/file_system/editor_file_system.cpp:3672-3676; ed1daf0).
		path = ProjectSettings::get_singleton()->globalize_path("res://.godot/petrichor/rename." + path.get_extension());
		DirAccess::make_dir_recursive_absolute(path.get_base_dir());
		if (ResourceSaver::get_singleton()->save(scene->second, path) != OK) {
			return std::nullopt;
		}
	}
	const PackedByteArray bytes = FileAccess::get_file_as_bytes(path);
	const Error error = FileAccess::get_open_error();
	if (scene != unsaved.end()) {
		DirAccess::remove_absolute(path);
	}
	if (error != OK) {
		return std::nullopt;
	}
	return bytes.is_empty() ? std::string() : std::string(reinterpret_cast<const char *>(bytes.ptr()), size_t(bytes.size()));
}

void file_seen(const std::string &p_path) {
	seen[p_path] = to_utf8(FileAccess::get_md5(to_godot(p_path)));
}

bool file_unchanged(const std::string &p_path) {
	const auto found = seen.find(p_path);
	return found != seen.end() && found->second == to_utf8(FileAccess::get_md5(to_godot(p_path)));
}

} // namespace petrichor
