#pragma once

#include "compiler/compiler.h"
#include "compiler/script_classes.h"
#include "compiler/workspace.h"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace petrichor::compiler {

// The compiled scripts of a project and the checks of its scenes, each kept for
// as long as what its check read is unchanged: its source, the declarations of
// other scripts and the project's answers (architecture §2.1). In a pass the project is taken to stay
// as it is, so each answer is checked once and each other script's
// declarations are checked once; between passes only the results are kept,
// since checked declarations point into the syntax trees of other files. A
// result found unchanged at a revision of the workspace is not read again
// until the revision changes.
class Analysis {
	struct Compiled {
		uint64_t source_hash = 0;
		std::shared_ptr<const CompileResult> result;
		Reads reads;
		// The workspace's revisions when it was computed and when it was last
		// found unchanged; 0 when read from another project.
		uint64_t computed = 0;
		uint64_t verified = 0;
	};

	const EngineApi &engine;
	Workspace &workspace;
	std::recursive_mutex mutex;
	int passes = 0; // Nested on the thread that holds `mutex`.
	const ProjectApi *reading = nullptr; // By the outermost pass.
	std::unique_ptr<ScriptClasses> classes; // Of the pass.
	std::map<std::string, Compiled> compiled; // Scripts, and the checks of scenes, by their paths.
	std::map<std::string, std::pair<uint64_t, std::vector<Diagnostic>>> placed; // placed_errors() by path, with the revision.
	int checks = 0;

	// The workspace's revision that the pass reads, or 0 for another project.
	uint64_t revision() const { return reading == &workspace ? workspace.revision() : 0; }
	bool unchanged(const Reads &p_reads);
	bool current(Compiled &r_entry);

public:
	Analysis(const EngineApi &p_engine, Workspace &r_workspace) :
			engine(p_engine), workspace(r_workspace) {}

	const EngineApi &engine_api() const { return engine; }
	const ProjectApi &project_api() const { return workspace; }

	// One at a time; nested ones are part of the outermost, and read its
	// project. An outermost one may read the project from `p_project`, as
	// renaming checks the scripts it would edit and the build reads the files;
	// what it compiles is kept as any other pass's.
	class Pass {
		Analysis &analysis;
		std::lock_guard<std::recursive_mutex> lock;

	public:
		explicit Pass(Analysis &r_analysis, const ProjectApi *p_project = nullptr);
		~Pass();
		Pass(const Pass &) = delete;
		Pass &operator=(const Pass &) = delete;
	};

	// The script at `p_path` (res://) compiled from `p_source` with its scenes,
	// as compile() does; the previous result if nothing it read has changed.
	std::shared_ptr<const CompileResult> compile(const std::string &p_path, const std::string &p_source);
	// The check of the scene at `p_path` (architecture §6.4): its errors in how
	// it places scripts of this language on its nodes, each in the source of
	// its `file`, and its references to their declarations, without a module;
	// the previous result if nothing it read has changed, the text of those
	// scripts included.
	std::shared_ptr<const CompileResult> check_scene(const std::string &p_path);
	// The errors that the checks of the scenes placing the script at `p_path`,
	// or instancing such a scene, and of the files whose resources have
	// scripts of this language, report in its source (architecture §6.4);
	// the previous ones while the workspace's revision stays.
	std::vector<Diagnostic> placed_errors(const std::string &p_path);
	// The workspace's revision when the result kept for `p_path` was computed,
	// for its stamp; 0 if none was computed from the workspace.
	uint64_t computed(const std::string &p_path);
	// The other scripts as the pass that is open reads them, for a check whose
	// result is not kept.
	ScriptClasses &pass_classes() { return *classes; }
	// Forgets every result, as when the engine's classes change.
	void clear();
	// Forgets the results for `p_path`, as for a script with no file whose
	// unsaved text is gone.
	void forget(const std::string &p_path);
	// How many scripts have been checked, for tests.
	int checks_run() const { return checks; }
};

} // namespace petrichor::compiler
