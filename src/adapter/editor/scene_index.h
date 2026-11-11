#pragma once

#include "compiler/project_api.h"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace petrichor {

// The project as the compiler sees it. Scenes for the scene model (spec §7)
// are read from the engine's scene states, so that text and binary scenes and
// instances read as the engine reads them, and read again when a file they
// are read from changes; loading a scene loads what it
// refers to, so scenes are only asked for outside of resource loading
// (CompileMode::Full). Global classes and autoloads come from the project
// settings, and sources from the files.
class SceneIndex : public compiler::ProjectApi {
	struct Entry {
		uint64_t modified = 0;
		// The other files it is read from, and when they changed: the scenes it
		// instances, at any depth, as an instance takes the class and the script
		// of its scene's root, and animation libraries stored apart.
		std::vector<std::pair<std::string, uint64_t>> files;
		compiler::Scene scene;
	};
	mutable std::mutex mutex; // Only around `scenes`: loading takes the engine's locks.
	mutable std::map<std::string, std::shared_ptr<const Entry>> scenes;

	// The scene, read again unless it is up to date.
	std::shared_ptr<const Entry> read(const std::string &p_path) const;

public:
	std::vector<std::string> scene_paths() const override;
	std::optional<compiler::Scene> scene(const std::string &p_path) const override;
	std::string main_scene() const override;
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

	// The editor saved the file: it and the scenes read from it are read again,
	// also within the second their modification times keep.
	void forget(const std::string &p_path) const;

	static const SceneIndex &get();
};

} // namespace petrichor
