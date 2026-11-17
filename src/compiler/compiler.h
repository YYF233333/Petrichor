#pragma once

#include "bytecode/module.h"
#include "compiler/diagnostic.h"
#include "compiler/engine_api.h"
#include "compiler/project_api.h"
#include "compiler/references.h"

#include <algorithm>
#include <string>
#include <vector>

namespace petrichor::compiler {

struct CompileResult {
	std::vector<Diagnostic> errors; // Sorted by position.
	// Warnings that no @warning_ignore ignores, and hints; none with syntax
	// errors. Sorted by position.
	std::vector<Diagnostic> warnings;
	bytecode::Module module; // Valid when ok().
	// Where the file resolves names (architecture §2.1), also with syntax errors.
	std::vector<Reference> references;

	// Without errors, or with only those after which the script runs.
	bool ok() const {
		return std::all_of(errors.begin(), errors.end(), [](const Diagnostic &p_error) { return p_error.runs; });
	}
};

// What a check covers.
enum class CheckMode {
	Program,
	// The class header, the members and the signatures, without function
	// bodies or scenes: the editor's view of a script while it loads. The
	// module describes the class but cannot run.
	Declarations,
	// As Declarations, for the scripts that use the class, with what _init
	// assigns (architecture §6.4) but none of its errors; or with scenes.
	ForOthers,
	ForOthersWithScenes,
	// How the scene at `CompileOptions::scene` uses the class (architecture
	// §6.4), declared as for others; its own errors are left out.
	Scene,
	// What can be written at the cursor that U+FFFF marks (spec §13.2), of
	// the function bodies checking only the one around it.
	Cursor,
};

struct CompileOptions {
	// The script's res:// path: to find the scenes that use it, to tell the
	// class from others of this language, and to resolve relative paths.
	std::string path;
	// The project's scenes, global classes, autoloads and other scripts;
	// without it, none of them is known.
	const ProjectApi *project = nullptr;
	CheckMode mode = CheckMode::Program;
	std::string scene; // The res:// path of the scene that CheckMode::Scene checks.
};

struct ClassDecl;
class ScriptClasses;

// Compiles one source file (UTF-8). With syntax errors, only they are reported.
CompileResult compile(const std::string &p_source, const EngineApi &p_engine, const CompileOptions &p_options = CompileOptions());
// With the other scripts as `r_classes` has read them, into `r_class`, which
// they may refer to until the compile ends (ScriptClasses::end_compile()).
CompileResult compile(const std::string &p_source, const EngineApi &p_engine, const CompileOptions &p_options, ScriptClasses &r_classes, ClassDecl &r_class);

} // namespace petrichor::compiler
