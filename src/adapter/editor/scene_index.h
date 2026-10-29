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

	// The scene, read again unless it is up to date.
	std::shared_ptr<const Entry> read(const std::string &p_path) const;

public:
	std::vector<compiler::Scene> scenes_with_script(const std::string &p_script_path) const override;
	std::optional<compiler::Scene> scene(const std::string &p_path) const override;
	std::string global_class_path(const std::string &p_name) const override;
	std::string autoload_path(const std::string &p_name) const override;
	std::optional<std::string> script_source(const std::string &p_path) const override;
	std::optional<compiler::ForeignClass> foreign_class(const std::string &p_path, bool p_members) const override;

	static const SceneIndex &get();
};

} // namespace petrichor
