#pragma once

#include "bytecode/module.h"
#include "compiler/diagnostic.h"
#include "compiler/engine_api.h"

#include <string>
#include <vector>

namespace petrichor::compiler {

struct CompileResult {
	std::vector<Diagnostic> errors; // Sorted by position.
	bytecode::Module module;        // Valid only without errors.

	bool ok() const { return errors.empty(); }
};

// Compiles one source file (UTF-8). Checking stops after syntax errors.
CompileResult compile(const std::string &p_source, const EngineApi &p_engine);

} // namespace petrichor::compiler
