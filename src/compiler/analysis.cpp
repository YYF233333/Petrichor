#include "compiler/analysis.h"

#include "compiler/ast.h"
#include "compiler/checker.h"
#include "compiler/parser.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <set>

namespace petrichor::compiler {

Analysis::Pass::Pass(Analysis &r_analysis, const ProjectApi *p_project) :
		analysis(r_analysis), lock(r_analysis.mutex) {
	if (analysis.passes++ > 0) {
		if (p_project && p_project != analysis.reading) {
			std::fputs("petrichor: a pass nested in another was given a project of its own.\n", stderr);
			std::abort();
		}
		return;
	}
	analysis.reading = p_project ? p_project : &analysis.project_api();
	analysis.on_disk = analysis.reading == &analysis.workspace.disk || analysis.viewed_on_disk;
	analysis.workspace.passes++;
	analysis.held = analysis.reading->hold();
	analysis.classes = std::make_unique<ScriptClasses>(analysis.engine, analysis.reading, false, true);
	analysis.classes->inflows = analysis.inflows;
}

Analysis::Pass::~Pass() {
	if (--analysis.passes == 0) {
		analysis.classes.reset();
		analysis.held.reset();
		analysis.workspace.passes--;
		analysis.reading = nullptr;
	}
}

Analysis::View::View(Analysis &r_analysis, const ProjectApi &p_project, bool p_disk) :
		analysis(r_analysis), lock(r_analysis.mutex) {
	analysis.viewed = &p_project;
	analysis.viewed_on_disk = p_disk;
}

Analysis::View::~View() {
	analysis.viewed = nullptr;
	analysis.viewed_on_disk = false;
}

bool Analysis::unchanged(const Reads &p_reads) {
	for (const auto &[file, hash] : p_reads.scripts) {
		if (classes->script_hash(file) != hash) {
			return false;
		}
	}
	for (const auto &[file, hash] : p_reads.sources) {
		if (classes->source_hash(file) != hash) {
			return false;
		}
	}
	for (const auto &[question, same] : p_reads.answers) {
		if (!same(*classes)) {
			return false;
		}
	}
	return true;
}

// Found unchanged at this revision, or read again (salsa's verified_at).
bool Analysis::current(Compiled &r_entry) {
	const uint64_t now = revision();
	if (now != 0 && r_entry.verified == now) {
		return true;
	}
	if (!unchanged(r_entry.reads)) {
		return false;
	}
	if (now != 0) {
		r_entry.verified = now;
	}
	return true;
}

std::shared_ptr<const CompileResult> Analysis::compile(const std::string &p_path, const std::string &p_source) {
	const Pass pass(*this);
	const uint64_t source_hash = bytecode::hash_source(p_source);
	std::map<std::string, Compiled> &kept = compiled[on_disk];
	const auto found = kept.find(p_path);
	if (found != kept.end() && found->second.source_hash == source_hash && current(found->second)) {
		return found->second.result;
	}
	checks++;
	CompileOptions options;
	options.path = p_path;
	options.project = classes->get_project();
	options.inflows = inflows;
	ClassDecl cls;
	classes->begin_compile(p_path, p_source);
	const std::shared_ptr<const CompileResult> result = std::make_shared<const CompileResult>(compiler::compile(p_source, engine, options, *classes, cls));
	kept[p_path] = { source_hash, result, classes->end_compile(), revision(), revision() };
	return result;
}

std::shared_ptr<const CompileResult> Analysis::check_scene(const std::string &p_path) {
	const Pass pass(*this);
	const auto found = compiled[on_disk].find(p_path);
	if (found != compiled[on_disk].end() && current(found->second)) {
		return found->second.result;
	}
	checks++;
	classes->begin_compile(p_path, std::string());
	const std::optional<Scene> scene = classes->get_project()->scene(p_path);
	// The scripts of its resources, on its nodes and on the roots of the
	// scenes it instances, with the files of the classes they extend, inner
	// ones too, whose checks look at them too. When its tracks or synchronized
	// properties name members, or it connects a node inside a scene it
	// instances or stores values for one, also those deeper in the scenes it
	// instances, which they reach; and so when it is used as a root, as its
	// nodes all get the exports that must be assigned.
	std::set<std::string> extended;
	std::set<std::string> scripts;
	const auto add = [&](std::string p_class) {
		while (is_script_path(p_class) && extended.insert(p_class).second) {
			scripts.insert(script_file(p_class));
			const ClassDecl *header = classes->header(p_class);
			p_class = header ? classes->base_path(*header, p_class) : std::string();
		}
	};
	const auto inside_instance = [&](const SceneConnection &p_connection) {
		return std::any_of(scene->nodes.begin(), scene->nodes.end(), [&](const SceneNode &p_node) {
			const std::string inside = p_node.path + "/";
			return !p_node.instance.empty() && (p_connection.target.rfind(inside, 0) == 0 || p_connection.source.rfind(inside, 0) == 0);
		});
	};
	const auto changes = [](const SceneNode &p_node) {
		return p_node.type.empty() && !p_node.is_instance && !p_node.properties.empty();
	};
	const bool deep = scene && (!scene->named_members.empty() || std::any_of(scene->connections.begin(), scene->connections.end(), inside_instance) || std::any_of(scene->nodes.begin(), scene->nodes.end(), changes) || !classes->root_use(p_path).empty());
	for (const SceneResource &resource : scene ? scene->resources : std::vector<SceneResource>()) {
		add(resource.script);
	}
	std::vector<Scene> pending = scene ? std::vector<Scene>{ *scene } : std::vector<Scene>();
	std::set<std::string> instanced;
	while (!pending.empty()) {
		const Scene each = std::move(pending.back());
		pending.pop_back();
		for (const SceneNode &node : each.nodes) {
			add(node.script.empty() ? node.instance_script : node.script);
			if (!deep || node.instance.empty()) {
				continue;
			}
			std::optional<Scene> inner = instanced.insert(node.instance).second ? classes->get_project()->scene(node.instance) : std::nullopt;
			if (inner) {
				pending.push_back(std::move(*inner));
			}
		}
	}
	Reads reads = classes->end_compile();
	CompileResult result;
	for (const std::string &script : scripts) {
		// Its errors give lines anywhere in the script, and its literal node paths.
		reads.sources.emplace(script, classes->source_hash(script));
		const std::optional<std::string> source = classes->get_project()->script_source(script);
		std::vector<Diagnostic> errors;
		ClassDecl cls = parse(source.value_or(std::string()), errors);
		// Its own check reports what keeps it from being read.
		if (!source || !errors.empty()) {
			continue;
		}
		CompileOptions options;
		options.path = script;
		options.project = classes->get_project();
		options.mode = CheckMode::Scene;
		options.scene = p_path;
		options.inflows = inflows;
		classes->begin_compile(script, *source);
		std::vector<Reference> references;
		check(cls, engine, options, errors, *classes, &references);
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
	compiled[on_disk][p_path] = { 0, shared, std::move(reads), revision(), revision() };
	return shared;
}

std::vector<Diagnostic> Analysis::placed_errors(const std::string &p_path) {
	const Pass pass(*this);
	const uint64_t now = revision();
	const auto found = placed.find(p_path);
	if (now != 0 && found != placed.end() && found->second.first == now) {
		return found->second.second;
	}
	std::vector<Diagnostic> errors;
	// The scenes that place it, and those that instance them, which check the
	// roots of the scenes they instance and what their tracks name; and the
	// files whose resources have scripts of this language, which may extend it.
	std::vector<Scene> pending = classes->scenes_with_script(p_path);
	const ProjectApi &project = *classes->get_project();
	for (const std::string &file : project.scene_paths()) {
		const std::optional<Scene> held = project.scene(file);
		if (held && std::any_of(held->resources.begin(), held->resources.end(), [](const SceneResource &p_resource) { return is_script_path(p_resource.script); })) {
			pending.push_back(*held);
		}
	}
	std::set<std::string> seen;
	while (!pending.empty()) {
		const std::string scene = pending.back().path;
		pending.pop_back();
		if (!seen.insert(scene).second) {
			continue;
		}
		for (const Diagnostic &error : check_scene(scene)->errors) {
			if (error.file == p_path) {
				errors.push_back(error);
			}
		}
		const std::vector<Scene> outer = classes->scenes_instancing(scene);
		pending.insert(pending.end(), outer.begin(), outer.end());
	}
	if (now != 0) {
		placed[p_path] = { now, errors };
	}
	return errors;
}

uint64_t Analysis::computed(const std::string &p_path) {
	const std::lock_guard<std::recursive_mutex> lock(mutex);
	const auto found = compiled[viewed_on_disk].find(p_path);
	return found == compiled[viewed_on_disk].end() ? 0 : found->second.computed;
}

void Analysis::clear() {
	const std::lock_guard<std::recursive_mutex> lock(mutex);
	for (std::map<std::string, Compiled> &kept : compiled) {
		kept.clear();
	}
	placed.clear();
}

void Analysis::forget(const std::string &p_path) {
	const std::lock_guard<std::recursive_mutex> lock(mutex);
	for (std::map<std::string, Compiled> &kept : compiled) {
		kept.erase(p_path);
	}
	placed.erase(p_path);
}

} // namespace petrichor::compiler
