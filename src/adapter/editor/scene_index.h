#pragma once

#include "compiler/project_api.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace petrichor {

// The project as the compiler sees it. Scenes for the scene model (spec §7)
// are read from the engine's scene states, so that text and binary scenes and
// instances read as the engine reads them, and read again when their file
// changes; loading a scene loads what it refers to, so scenes are only asked
// for outside of resource loading (CompileMode::Full). Global classes and
// autoloads come from the project settings, and sources from the files.
class SceneIndex : public compiler::ProjectApi {
	struct Entry {
		uint64_t modified = 0;
		compiler::Scene scene;
	};
	mutable std::mutex mutex;
	mutable std::map<std::string, Entry> scenes;

	// Reads the scene unless it is up to date; the mutex is held.
	const compiler::Scene &read(const std::string &p_path) const;

public:
	std::vector<compiler::Scene> scenes_with_script(const std::string &p_script_path) const override;
	std::optional<compiler::Scene> scene(const std::string &p_path) const override;
	std::string global_class_path(const std::string &p_name) const override;
	std::string autoload_path(const std::string &p_name) const override;
	std::optional<std::string> script_source(const std::string &p_path) const override;

	static const SceneIndex &get();
};

} // namespace petrichor
