#include "compiler/analysis.h"

#include "compiler/ast.h"

namespace petrichor::compiler {

Analysis::Pass::Pass(Analysis &r_analysis) :
		analysis(r_analysis), lock(r_analysis.mutex) {
	if (analysis.passes++ == 0) {
		analysis.classes = std::make_unique<ScriptClasses>(analysis.engine, &analysis.project, false, true);
	}
}

Analysis::Pass::~Pass() {
	if (--analysis.passes == 0) {
		analysis.classes.reset();
	}
}

bool Analysis::unchanged(const Reads &p_reads) {
	for (const auto &[file, hash] : p_reads.scripts) {
		if (classes->script_hash(file) != hash) {
			return false;
		}
	}
	for (const auto &[question, same] : p_reads.answers) {
		if (!same(project)) {
			return false;
		}
	}
	return true;
}

std::shared_ptr<const CompileResult> Analysis::compile(const std::string &p_path, const std::string &p_source) {
	const Pass pass(*this);
	const uint64_t source_hash = bytecode::hash_source(p_source);
	const auto found = compiled.find(p_path);
	if (found != compiled.end() && found->second.source_hash == source_hash && unchanged(found->second.reads)) {
		return found->second.result;
	}
	checks++;
	CompileOptions options;
	options.path = p_path;
	options.project = classes->get_project();
	ClassDecl cls;
	classes->begin_compile(p_path, p_source);
	const std::shared_ptr<const CompileResult> result = std::make_shared<const CompileResult>(compiler::compile(p_source, engine, options, *classes, cls));
	Compiled &entry = compiled[p_path];
	entry = { source_hash, result, classes->end_compile() };
	return result;
}

void Analysis::clear() {
	const std::lock_guard<std::recursive_mutex> lock(mutex);
	compiled.clear();
}

} // namespace petrichor::compiler
