#pragma once

#include "compiler/engine_api.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace petrichor::compiler {

// A value that a scene stores for a property, as the checks of exports read
// it (spec §4.3, §7.4, §7.5).
struct SceneValue {
	std::string type; // The engine's name of its type: "int", "Dictionary", "Nil"…
	std::string class_name; // An object's class, and its script if it has one.
	std::string script;
	int64_t integer = 0; // An int, or a float that is exactly one (`whole`).
	bool whole = false;
	std::string text; // A String or a StringName.
	std::vector<SceneValue> items; // An Array's elements, or a Dictionary's keys and values one after another.
	std::string path; // A resource's res:// path, such as a scene's, if it has one.

	// The paths of the scenes it holds, itself or in its items.
	void add_scenes(std::vector<std::string> &r_paths) const {
		if (class_name == "PackedScene" && !path.empty()) {
			r_paths.push_back(path);
		}
		for (const SceneValue &item : items) {
			item.add_scenes(r_paths);
		}
	}

	bool operator==(const SceneValue &p_other) const {
		return type == p_other.type && class_name == p_other.class_name && script == p_other.script && path == p_other.path && integer == p_other.integer && whole == p_other.whole && text == p_other.text &&
				items == p_other.items;
	}
};

// A node of a scene, as the scene model sees it (spec §7.1).
struct SceneNode {
	std::string path; // From the scene's root: "." for the root, "Head/Camera".
	std::string type; // Its engine class; for an instanced scene, the class of that scene's root.
	std::string script; // The res:// path of the script attached in this scene, if any.
	bool is_instance = false; // An instanced scene, whose own nodes are not listed here.
	// For an instanced scene: the script its root has there, unless this scene
	// attaches another.
	std::string instance_script;
	bool unique = false; // Reached from the scene's nodes as `%Name` (unique_name_in_owner).
	// What this scene itself stores for the properties of a node with a script
	// of this language, or of an instanced scene's node it changes, by name; a
	// base or an instanced scene stores the rest. Of another node, only the
	// values that hold scenes.
	std::vector<std::pair<std::string, SceneValue>> properties;
	std::string instance; // For an instanced scene: its res:// path.

	bool operator==(const SceneNode &p_other) const {
		return path == p_other.path && type == p_other.type && script == p_other.script && is_instance == p_other.is_instance && instance_script == p_other.instance_script && unique == p_other.unique &&
				properties == p_other.properties && instance == p_other.instance;
	}
};

// A signal connection saved in a scene (spec §7.7).
struct SceneConnection {
	std::string signal;
	std::string source; // Node paths from the scene's root.
	std::string target;
	std::string method;
	int binds = 0; // Arguments bound to the call, after the signal's.
	int unbinds = 0; // Signal arguments dropped from the end.

	bool operator==(const SceneConnection &p_other) const {
		return signal == p_other.signal && source == p_other.source && target == p_other.target && method == p_other.method && binds == p_other.binds && unbinds == p_other.unbinds;
	}
};

// A member that a scene names for the engine to find by name (spec §7.7,
// §10.5): a property or a method that a track of an animation names, or a
// property that a MultiplayerSynchronizer synchronizes.
struct SceneNamedMember {
	std::string from; // The node that names it, from the scene's root.
	std::string animation; // The track's animation; "" for a synchronized property.
	std::string node; // The path from `from` to the node that has the member.
	std::string name;
	bool method = false;

	bool operator==(const SceneNamedMember &p_other) const {
		return from == p_other.from && animation == p_other.animation && node == p_other.node && name == p_other.name && method == p_other.method;
	}
};

// A resource that a scene or a resource file holds, with a script of this
// language or holding scenes (spec §4.3, §6.2, §7.4).
struct SceneResource {
	std::string path; // Its res:// path; a sub-resource's is the file's, "::" and its id.
	std::string script; // The res:// path of its script, if any.
	// What the file stores for its properties, as the engine loads them, but
	// null, which the check takes as none. Of a resource without a script of
	// this language, only the values that hold scenes.
	std::vector<std::pair<std::string, SceneValue>> properties;

	// As messages name it: "the resource", or "the resource Resource_x1" for a
	// sub-resource.
	std::string shown() const {
		const size_t at = path.find("::");
		return at == std::string::npos ? std::string("the resource") : "the resource " + path.substr(at + 2);
	}

	bool operator==(const SceneResource &p_other) const { return path == p_other.path && script == p_other.script && properties == p_other.properties; }
};

// A scene, or a resource file as one without nodes: the unit of the resource
// check (architecture §6.4).
struct Scene {
	std::string path;
	std::vector<SceneNode> nodes; // With those of the scene it inherits.
	std::vector<SceneConnection> connections;
	std::string base; // The scene it inherits, if any.
	std::vector<SceneNamedMember> named_members; // With those of the scene it inherits.
	std::vector<SceneResource> resources; // Those it holds itself, a resource file's own too.

	bool operator==(const Scene &p_other) const {
		return path == p_other.path && nodes == p_other.nodes && connections == p_other.connections && base == p_other.base && named_members == p_other.named_members && resources == p_other.resources;
	}

	const SceneNode *find_node(const std::string &p_path) const {
		for (const SceneNode &node : nodes) {
			if (node.path == p_path) {
				return &node;
			}
		}
		return nullptr;
	}
};

// A class of another language, such as GDScript, as reflection shows it
// (spec §11.1). Objects are named by the engine class, or by the global name of
// a script's class.
struct ForeignClass {
	std::string name; // Its global name (class_name), or "".
	std::string native; // The engine class it extends, through the scripts it extends; "" if not known.
	std::string base; // The script it extends, or "".
	bool members_known = false;
	// With those of the scripts it extends.
	std::vector<EngineProperty> properties;
	std::vector<EngineMethod> methods;
	std::vector<EngineMethod> signals;
	std::vector<EngineProperty> constants; // Typed by their values.

	bool operator==(const ForeignClass &p_other) const {
		return name == p_other.name && native == p_other.native && base == p_other.base && members_known == p_other.members_known && properties == p_other.properties &&
				methods == p_other.methods && signals == p_other.signals && constants == p_other.constants;
	}
};

// A resource that is not a scene, as its file says without loading it.
struct ResourceInfo {
	std::string type; // The engine class it loads as, such as CompressedTexture2D for an imported image.
	std::string script_class; // The global name of its script's class, if it has a script with one.

	bool operator==(const ResourceInfo &p_other) const { return type == p_other.type && script_class == p_other.script_class; }
};

// What the compiler knows about the project (architecture §2.1). The editor
// reads it from the engine's scene states; tests describe scenes by hand.
class ProjectApi {
public:
	virtual ~ProjectApi() = default;

	// The res:// paths of the project's scenes and resource files.
	virtual std::vector<std::string> scene_paths() const = 0;
	// A scene or a resource file by its res:// path; nullopt if there is
	// none, or if a resource file holds nothing the check reads.
	virtual std::optional<Scene> scene(const std::string &p_path) const = 0;
	// The res:// path of the main scene, or "" (spec §7.3).
	virtual std::string main_scene() const = 0;
	// Any other resource by its res:// path, for preload; nullopt if there is none.
	virtual std::optional<ResourceInfo> resource(const std::string &p_path) const = 0;
	// The res:// path a uid:// path names, or "".
	virtual std::string uid_path(const std::string &p_uid) const = 0;
	// The res:// paths of what the engine loads with a scene or a resource,
	// its ext_resources, as its file lists them (spec §7.6).
	virtual std::vector<std::string> dependencies(const std::string &p_path) const = 0;

	// The script that a global class name (class_name) refers to, or "".
	virtual std::string global_class_path(const std::string &p_name) const = 0;
	// The script of an autoload, by its global name, or "" (spec §7.3).
	virtual std::string autoload_path(const std::string &p_name) const = 0;
	// For completion (spec §13.2): the paths of the global classes by their
	// names, the global names of the autoloads, the res:// paths of the files
	// the editor knows as resources and scripts, and the input actions.
	virtual std::map<std::string, std::string> global_class_paths() const = 0;
	virtual std::vector<std::string> autoload_names() const = 0;
	virtual std::vector<std::string> resource_files() const = 0;
	virtual std::vector<std::string> input_actions() const = 0;
	// The res:// paths of the project's scripts of this language and GDScript.
	virtual std::vector<std::string> script_paths() const = 0;
	// The source of a script, UTF-8; nullopt if it cannot be read.
	virtual std::optional<std::string> script_source(const std::string &p_path) const = 0;
	// Each preload() and load() of one literal String in a script of this
	// language or GDScript, in function bodies too: whether it is a preload,
	// and the String as written (architecture §6.4). An index may keep them
	// per file.
	virtual std::vector<std::pair<bool, std::string>> literal_loads(const std::string &p_path) const;
	// A script of another language by its res:// path, nullopt if there is
	// none; its members only with `p_members`, which loads it, since a script
	// of this language that is being loaded may load no other.
	virtual std::optional<ForeignClass> foreign_class(const std::string &p_path, bool p_members) const = 0;
};

} // namespace petrichor::compiler
