#pragma once

#include <string>
#include <vector>

namespace petrichor::compiler {

// A node of a scene, as the scene model sees it (spec §7.1).
struct SceneNode {
	std::string path; // From the scene's root: "." for the root, "Head/Camera".
	std::string type; // Its engine class; for an instanced scene, the class of that scene's root.
	std::string script; // The res:// path of the script attached in this scene, if any.
	bool is_instance = false; // An instanced scene, whose own nodes are not listed here.
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

// What the compiler knows about the project (architecture §2.1). The editor
// reads it from the engine's scene states; tests describe scenes by hand.
class ProjectApi {
public:
	virtual ~ProjectApi() = default;

	// The scenes in which some node has the script attached.
	virtual std::vector<Scene> scenes_with_script(const std::string &p_script_path) const = 0;
};

} // namespace petrichor::compiler
