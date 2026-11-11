#pragma once

#include "compiler/compiler.h"
#include "compiler/script_classes.h"

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
// since checked declarations point into the syntax trees of other files.
class Analysis {
	struct Compiled {
		uint64_t source_hash = 0;
		std::shared_ptr<const CompileResult> result;
		Reads reads;
	};

	const EngineApi &engine;
	const ProjectApi &project;
	std::recursive_mutex mutex;
	int passes = 0; // Nested on the thread that holds `mutex`.
	std::unique_ptr<ScriptClasses> classes; // Of the pass.
	std::map<std::string, Compiled> compiled; // Scripts, and the checks of scenes, by their paths.
	int checks = 0;

	bool unchanged(const Reads &p_reads);

public:
	Analysis(const EngineApi &p_engine, const ProjectApi &p_project) :
			engine(p_engine), project(p_project) {}

	// One at a time; nested ones are part of the outermost.
	class Pass {
		Analysis &analysis;
		std::lock_guard<std::recursive_mutex> lock;

	public:
		explicit Pass(Analysis &r_analysis);
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
	// The other scripts as the pass that is open reads them, for a check whose
	// result is not kept.
	ScriptClasses &pass_classes() { return *classes; }
	// Forgets every result, as when the engine's classes change.
	void clear();
	// How many scripts have been checked, for tests.
	int checks_run() const { return checks; }
};

} // namespace petrichor::compiler
