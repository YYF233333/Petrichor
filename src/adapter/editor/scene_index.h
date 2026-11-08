#pragma once

#include "compiler/project_api.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace petrichor {

// The project as the compiler sees it. Scenes for the scene model (spec §7)
// are read from the engine's scene states, so that text and binary scenes and
// instances read as the engine reads them, and read again when their file or
// the file of a scene they instance changes; loading a scene loads what it
// refers to, so scenes are only asked for outside of resource loading
// (CompileMode::Full). Global classes and autoloads come from the project
// settings, and sources from the files.
class SceneIndex : public compiler::ProjectApi {
	struct Entry {
		uint64_t modified = 0;
		// The scenes it instances, at any depth, and when they changed: an
		// instance takes the class and the script of its scene's root.
		std::vector<std::pair<std::string, uint64_t>> instanced;
		compiler::Scene scene;
	};
	mutable std::mutex mutex; // Only around `scenes`: loading takes the engine's locks.
	mutable std::map<std::string, std::shared_ptr<const Entry>> scenes;
	// What a pass has read of the project.
	struct Snapshot {
		bool listed = false;
		std::vector<std::shared_ptr<const Entry>> scenes;
		std::map<std::string, std::string> bases; // Of scripts of this language; "" for none.
	};
	static thread_local int pass_depth;
	static thread_local std::unique_ptr<Snapshot> snapshot;

	// The scene, read again unless it is up to date.
	std::shared_ptr<const Entry> read(const std::string &p_path) const;
	// The scenes with a node for which `p_uses` holds.
	std::vector<compiler::Scene> scenes_where(const std::function<bool(const compiler::SceneNode &)> &p_uses) const;
	// Whether the script is `p_base` or extends it, as the headers say.
	bool is_or_extends(const std::string &p_script, const std::string &p_base) const;

public:
	// An analysis, such as a build or the compilation of a script with the
	// scripts it uses, sees the project's files as they were when it began:
	// scenes are listed and checked for changes once, not at every query.
	// Passes nest, and the outermost on a thread counts.
	class Pass {
	public:
		Pass();
		~Pass();
		Pass(const Pass &) = delete;
		Pass &operator=(const Pass &) = delete;
	};

	std::vector<compiler::Scene> scenes_with_script(const std::string &p_script_path) const override;
	std::vector<compiler::Scene> scenes_instancing_script(const std::string &p_script_path) const override;
	std::optional<compiler::Scene> scene(const std::string &p_path) const override;
	std::optional<compiler::ResourceInfo> resource(const std::string &p_path) const override;
	std::string uid_path(const std::string &p_uid) const override;
	std::vector<std::string> dependencies(const std::string &p_path) const override;
	std::string global_class_path(const std::string &p_name) const override;
	std::string autoload_path(const std::string &p_name) const override;
	std::map<std::string, std::string> global_class_paths() const override;
	std::vector<std::string> autoload_names() const override;
	std::vector<std::string> resource_files() const override;
	std::vector<std::string> input_actions() const override;
	std::optional<std::string> script_source(const std::string &p_path) const override;
	std::optional<compiler::ForeignClass> foreign_class(const std::string &p_path, bool p_members) const override;

	// The editor saved the scene: it and the scenes that instance it are read
	// again, also within the second their modification times keep.
	void forget(const std::string &p_path) const;

	static const SceneIndex &get();
};

} // namespace petrichor
