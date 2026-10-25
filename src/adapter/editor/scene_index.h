#pragma once

#include "compiler/project_api.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace petrichor {

// The scenes of the project for the compiler's scene model (spec §7), read
// from the engine's scene states, so that text and binary scenes and
// instances read as the engine reads them. A scene is read again when its
// file changes. Loading a scene loads what it refers to, so this is only used
// outside of resource loading (CompileMode::Full).
class SceneIndex : public compiler::ProjectApi {
	struct Entry {
		uint64_t modified = 0;
		compiler::Scene scene;
	};
	mutable std::mutex mutex;
	mutable std::map<std::string, Entry> scenes;

public:
	std::vector<compiler::Scene> scenes_with_script(const std::string &p_script_path) const override;

	static const SceneIndex &get();
};

} // namespace petrichor
