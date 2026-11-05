#pragma once

#include "bytecode/module.h"
#include "compiler/diagnostic.h"
#include "compiler/engine_api.h"
#include "compiler/project_api.h"
#include "compiler/references.h"

#include <string>
#include <vector>

namespace petrichor::compiler {

struct CompileResult {
	std::vector<Diagnostic> errors; // Sorted by position.
	// Warnings that no @warning_ignore ignores, and hints; none with syntax
	// errors. Sorted by position.
	std::vector<Diagnostic> warnings;
	bytecode::Module module; // Valid only without errors.
	// Where the file resolves names (architecture §2.1), also with syntax errors.
	std::vector<Reference> references;

	bool ok() const { return errors.empty(); }
};

struct CompileOptions {
	// The script's res:// path: to find the scenes that use it, to tell the
	// class from others of this language, and to resolve relative paths.
	std::string path;
	// The project's scenes, global classes, autoloads and other scripts;
	// without it, none of them is known.
	const ProjectApi *project = nullptr;
	// Checks the class header, the members and the signatures, but no function
	// bodies, and needs no scenes: the editor's view of a script while it is
	// being loaded, when the scenes that use it cannot be loaded. The module
	// describes the class but cannot run.
	bool declarations_only = false;
};

struct ClassDecl;
class ScriptClasses;

// Compiles one source file (UTF-8). With syntax errors, only they are reported.
CompileResult compile(const std::string &p_source, const EngineApi &p_engine, const CompileOptions &p_options = CompileOptions());
// With the other scripts as `r_classes` has read them, into `r_class`, which
// they may refer to until the compile ends (ScriptClasses::end_compile()).
CompileResult compile(const std::string &p_source, const EngineApi &p_engine, const CompileOptions &p_options, ScriptClasses &r_classes, ClassDecl &r_class);

} // namespace petrichor::compiler
