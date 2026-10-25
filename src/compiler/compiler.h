#pragma once

#include "bytecode/module.h"
#include "compiler/diagnostic.h"
#include "compiler/engine_api.h"
#include "compiler/project_api.h"

#include <string>
#include <vector>

namespace petrichor::compiler {

struct CompileResult {
	std::vector<Diagnostic> errors; // Sorted by position.
	bytecode::Module module;        // Valid only without errors.

	bool ok() const { return errors.empty(); }
};

struct CompileOptions {
	// The script's res:// path, to find the scenes that use it.
	std::string path;
	// Scene checks need the project; without it, scenes are unknown.
	const ProjectApi *project = nullptr;
	// Checks the class header, the members and the signatures, but no function
	// bodies, and needs no scenes: the editor's view of a script while it is
	// being loaded, when the scenes that use it cannot be loaded. The module
	// describes the class but cannot run.
	bool declarations_only = false;
};

// Compiles one source file (UTF-8). Checking stops after syntax errors.
CompileResult compile(const std::string &p_source, const EngineApi &p_engine, const CompileOptions &p_options = CompileOptions());

} // namespace petrichor::compiler
