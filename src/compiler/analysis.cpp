#include "compiler/analysis.h"

#include "compiler/ast.h"
#include "compiler/checker.h"
#include "compiler/parser.h"

#include <algorithm>
#include <set>

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

std::shared_ptr<const CompileResult> Analysis::check_scene(const std::string &p_path) {
	const Pass pass(*this);
	const auto found = compiled.find(p_path);
	if (found != compiled.end() && unchanged(found->second.reads)) {
		return found->second.result;
	}
	checks++;
	classes->begin_compile(p_path, std::string());
	const std::optional<Scene> scene = classes->get_project()->scene(p_path);
	Reads reads = classes->end_compile();
	std::set<std::string> scripts;
	for (const SceneNode &node : scene ? scene->nodes : std::vector<SceneNode>()) {
		if (node.script.size() > 4 && node.script.compare(node.script.size() - 4, 4, ".pet") == 0) {
			scripts.insert(node.script);
		}
	}
	CompileResult result;
	for (const std::string &script : scripts) {
		reads.scripts.emplace(script, classes->script_hash(script));
		const std::optional<std::string> source = project.script_source(script);
		std::vector<Diagnostic> errors;
		ClassDecl cls = parse(source.value_or(std::string()), errors);
		// Its own check reports what keeps it from being read.
		if (!source || !errors.empty()) {
			continue;
		}
		CompileOptions options;
		options.path = script;
		options.project = classes->get_project();
		options.declarations_only = true;
		options.scene = p_path;
		classes->begin_compile(script, *source);
		std::vector<Reference> references;
		check(cls, engine, options, errors, *classes, false, &references);
		reads.merge(classes->end_compile());
		result.references.insert(result.references.end(), references.begin(), references.end());
		std::stable_sort(errors.begin(), errors.end(), [](const Diagnostic &a, const Diagnostic &b) {
			return a.begin < b.begin;
		});
		for (Diagnostic &error : errors) {
			const SourcePosition position = cls.source->position(error.begin);
			error.line = position.line;
			error.column = position.column;
			error.file = script;
			result.errors.push_back(std::move(error));
		}
	}
	const std::shared_ptr<const CompileResult> shared = std::make_shared<const CompileResult>(std::move(result));
	compiled[p_path] = { 0, shared, std::move(reads) };
	return shared;
}

void Analysis::clear() {
	const std::lock_guard<std::recursive_mutex> lock(mutex);
	compiled.clear();
}

} // namespace petrichor::compiler
