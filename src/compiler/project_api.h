#pragma once

#include "compiler/engine_api.h"

#include <optional>
#include <string>
#include <vector>

namespace petrichor::compiler {

// A node of a scene, as the scene model sees it (spec §7.1).
struct SceneNode {
	std::string path; // From the scene's root: "." for the root, "Head/Camera".
	std::string type; // Its engine class; for an instanced scene, the class of that scene's root.
	std::string script; // The res:// path of the script attached in this scene, if any.
	bool is_instance = false; // An instanced scene, whose own nodes are not listed here.
	// For an instanced scene: the script its root has there, unless this scene
	// attaches another.
	std::string instance_script;
};

// A signal connection saved in a scene (spec §7.7).
struct SceneConnection {
	std::string signal;
	std::string source; // Node paths from the scene's root.
	std::string target;
	std::string method;
	int binds = 0; // Arguments bound to the call, after the signal's.
	int unbinds = 0; // Signal arguments dropped from the end.
};

struct Scene {
	std::string path;
	std::vector<SceneNode> nodes;
	std::vector<SceneConnection> connections;

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
	std::string native; // The engine class it extends, through the scripts it extends; "" if not known.
	std::string base; // The script it extends, or "".
	bool members_known = false;
	// With those of the scripts it extends.
	std::vector<EngineProperty> properties;
	std::vector<EngineMethod> methods;
	std::vector<EngineMethod> signals;
};

// What the compiler knows about the project (architecture §2.1). The editor
// reads it from the engine's scene states; tests describe scenes by hand.
class ProjectApi {
public:
	virtual ~ProjectApi() = default;

	// The scenes in which some node has the script attached.
	virtual std::vector<Scene> scenes_with_script(const std::string &p_script_path) const = 0;
	// The scenes that instance a scene whose root has the script, without
	// attaching another to it there.
	virtual std::vector<Scene> scenes_instancing_script(const std::string &p_script_path) const = 0;
	// A scene by its res:// path, for preload (spec §7.4); nullopt if there is none.
	virtual std::optional<Scene> scene(const std::string &p_path) const = 0;

	// The script that a global class name (class_name) refers to, or "".
	virtual std::string global_class_path(const std::string &p_name) const = 0;
	// The script of an autoload, by its global name, or "" (spec §7.3).
	virtual std::string autoload_path(const std::string &p_name) const = 0;
	// The source of a script of this language, UTF-8; nullopt if it cannot be read.
	virtual std::optional<std::string> script_source(const std::string &p_path) const = 0;
	// A script of another language by its res:// path, nullopt if there is
	// none; its members only with `p_members`, which loads it, since a script
	// of this language that is being loaded may load no other.
	virtual std::optional<ForeignClass> foreign_class(const std::string &p_path, bool p_members) const = 0;
};

} // namespace petrichor::compiler
