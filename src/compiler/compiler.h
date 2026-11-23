#pragma once

#include "bytecode/module.h"
#include "compiler/diagnostic.h"
#include "compiler/engine_api.h"
#include "compiler/project_api.h"
#include "compiler/references.h"

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace petrichor::compiler {

// Where a literal node path leads from a node that has the script, in one
// scene (spec §7.1), for the editor to write it anew when nodes move
// (§13.3).
struct NodePathWalk {
	// A node of the scene walked in: its path there, and the scenes it is in,
	// innermost first, each with the path of the node it is instanced at, "."
	// for the scene walked in.
	struct Node {
		std::string path;
		std::vector<std::pair<std::string, std::string>> scenes;
	};
	// The path's bytes from its "$" or "%", or of the String of get_node(), and
	// what it holds.
	uint32_t begin = 0;
	uint32_t end = 0;
	std::string text;
	std::string scene;
	// The node it starts from, or the one its last `%Name` part names, and the
	// node it reaches.
	Node from;
	Node to;
};

struct CompileResult {
	std::vector<Diagnostic> errors; // Sorted by position.
	// Warnings that no @warning_ignore ignores, and hints; none with syntax
	// errors. Sorted by position.
	std::vector<Diagnostic> warnings;
	bytecode::Module module; // Valid when ok().
	// Where the file resolves names (architecture §2.1), also with syntax errors.
	std::vector<Reference> references;
	std::vector<NodePathWalk> node_paths;

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

struct Inflows;

struct CompileOptions {
	// The script's res:// path: to find the scenes that use it, to tell the
	// class from others of this language, and to resolve relative paths.
	std::string path;
	// The project's scenes, global classes, autoloads and other scripts;
	// without it, none of them is known.
	const ProjectApi *project = nullptr;
	CheckMode mode = CheckMode::Program;
	std::string scene; // The res:// path of the scene that CheckMode::Scene checks.
	// In a migration's inference: the untyped slots of its files take the
	// types tried for them without errors, and what flows into them is
	// recorded. Nothing is lowered.
	Inflows *inflows = nullptr;
};

struct ClassDecl;
class ScriptClasses;

// Compiles one source file (UTF-8). With syntax errors, only they are reported.
CompileResult compile(const std::string &p_source, const EngineApi &p_engine, const CompileOptions &p_options = CompileOptions());
// With the other scripts as `r_classes` has read them, into `r_class`, which
// they may refer to until the compile ends (ScriptClasses::end_compile()).
CompileResult compile(const std::string &p_source, const EngineApi &p_engine, const CompileOptions &p_options, ScriptClasses &r_classes, ClassDecl &r_class);

// The build of this compiler, which the bytecode it writes records, so that
// a game run with its sources runs no bytecode of another (architecture
// §2.4).
uint64_t compiler_id();

// The edits of the fixes of `p_fixes` that apply together, by file, each
// fix's edits without one in the file given with it: each fix unless an edit
// of it overlaps or starts where one of a fix before it does, which fixing
// again finds. A fix that another offered already is applied once.
// `r_applied` gets those applied.
std::map<std::string, std::vector<CheckedEdit>> fixed_together(const std::vector<std::pair<const Fix *, std::string>> &p_fixes, std::vector<const Fix *> &r_applied);

// The edits that fix all of an automatic kind in the file compiled
// (architecture §2.1), in order: of each fix of the kind that edits only
// that file, applied together (fixed_together()).
std::vector<CheckedEdit> fix_all(const CompileResult &p_result, FixKind p_kind);

} // namespace petrichor::compiler
