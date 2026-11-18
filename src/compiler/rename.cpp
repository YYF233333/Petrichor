#include "compiler/rename.h"

#include "compiler/ast.h"
#include "compiler/lexer.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace petrichor::compiler {

namespace {

using Kind = DeclId::Kind;

// The edits of one file, by offset.
using Edits = std::vector<CheckedEdit>;

// Where byte `p_offset` of the text before `p_edits` is after them, or of the
// text after them before them with `p_back`. One inside the bytes of an edit
// goes to the edit's start.
uint32_t moved(const Edits &p_edits, uint32_t p_offset, bool p_back) {
	int64_t delta = 0;
	for (const CheckedEdit &edit : p_edits) {
		const int64_t removed = int64_t(edit.end) - edit.begin;
		const int64_t added = int64_t(edit.new_text.size());
		const int64_t begin = p_back ? edit.begin + delta : edit.begin;
		if (p_offset < begin) {
			break;
		}
		if (p_offset < begin + (p_back ? added : removed)) {
			return uint32_t(p_back ? edit.begin : edit.begin + delta);
		}
		delta += added - removed;
	}
	return uint32_t(p_back ? p_offset - delta : p_offset + delta);
}

std::string applied(const std::string &p_text, const Edits &p_edits) {
	std::string result;
	uint32_t done = 0;
	for (const CheckedEdit &edit : p_edits) {
		result += p_text.substr(done, edit.begin - done) + edit.new_text;
		done = edit.end;
	}
	return result + p_text.substr(done);
}

// The project as the edits leave it: the sources of the scripts, and a global
// class name that renaming changes.
class EditedProject : public ProjectApi {
	const ProjectApi &project;
	const std::map<std::string, std::string> &sources;
	std::string old_global;
	std::string new_global;
	std::string global_path;

public:
	EditedProject(const ProjectApi &p_project, const std::map<std::string, std::string> &p_sources) :
			project(p_project), sources(p_sources) {}

	void rename_global(const std::string &p_old, const std::string &p_new, const std::string &p_path) {
		old_global = p_old;
		new_global = p_new;
		global_path = p_path;
	}

	std::vector<std::string> scene_paths() const override { return project.scene_paths(); }
	std::optional<Scene> scene(const std::string &p_path) const override { return project.scene(p_path); }
	std::string main_scene() const override { return project.main_scene(); }
	// The edits rewrite the headers that name the class (architecture §6.4).
	std::optional<ResourceInfo> resource(const std::string &p_path) const override {
		std::optional<ResourceInfo> found = project.resource(p_path);
		if (found && !global_path.empty() && found->script_class == old_global) {
			found->script_class = new_global;
		}
		return found;
	}
	std::string uid_path(const std::string &p_uid) const override { return project.uid_path(p_uid); }
	std::vector<std::string> dependencies(const std::string &p_path) const override { return project.dependencies(p_path); }
	std::string global_class_path(const std::string &p_name) const override {
		if (!global_path.empty() && (p_name == old_global || p_name == new_global)) {
			return p_name == new_global ? global_path : std::string();
		}
		return project.global_class_path(p_name);
	}
	std::string autoload_path(const std::string &p_name) const override { return project.autoload_path(p_name); }
	std::map<std::string, std::string> global_class_paths() const override {
		std::map<std::string, std::string> paths = project.global_class_paths();
		if (!global_path.empty()) {
			paths.erase(old_global);
			paths[new_global] = global_path;
		}
		return paths;
	}
	std::vector<std::string> autoload_names() const override { return project.autoload_names(); }
	std::vector<std::string> resource_files() const override { return project.resource_files(); }
	std::vector<std::string> input_actions() const override { return project.input_actions(); }
	std::vector<std::string> script_paths() const override { return project.script_paths(); }
	std::optional<std::string> script_source(const std::string &p_path) const override {
		const auto found = sources.find(p_path);
		return found == sources.end() ? project.script_source(p_path) : std::optional<std::string>(found->second);
	}
	std::vector<std::pair<bool, std::string>> literal_loads(const std::string &p_path) const override { return project.literal_loads(p_path); }
	std::optional<ForeignClass> foreign_class(const std::string &p_path, bool p_members) const override { return project.foreign_class(p_path, p_members); }
};

std::string extension(const std::string &p_file) {
	std::string found = p_file.substr(p_file.find_last_of('.') + 1);
	std::transform(found.begin(), found.end(), found.begin(), [](unsigned char p_char) { return char(std::tolower(p_char)); });
	return found;
}

Diagnostic refusal(const std::string &p_file, int p_line, const std::string &p_why) {
	Diagnostic made;
	made.file = p_file;
	made.line = p_line;
	made.message = p_why;
	return made;
}

Rename refused(const std::string &p_file, int p_line, const std::string &p_why) {
	Rename result;
	result.refusals.push_back(refusal(p_file, p_line, p_why));
	return result;
}

// The script of a node of a scene; a node inside a scene it instances, which
// it changes, has the one that scene gives it.
std::string script_of(const ProjectApi &p_project, const Scene &p_scene, const std::string &p_node) {
	const SceneNode *node = p_scene.find_node(p_node);
	if (node && (!node->type.empty() || node->is_instance || !node->script.empty())) {
		return node->script.empty() ? node->instance_script : node->script;
	}
	for (const SceneNode &instance : p_scene.nodes) {
		if (instance.is_instance && p_node.rfind(instance.path + "/", 0) == 0) {
			const std::optional<Scene> inner = p_project.scene(instance.instance);
			return inner ? script_of(p_project, *inner, p_node.substr(instance.path.size() + 1)) : std::string();
		}
	}
	return std::string();
}

} // namespace

Rename rename(Analysis &r_analysis, const std::string &p_path, uint32_t p_offset, const std::string &p_new, const std::vector<std::string> &p_other_scripts,
		const std::function<std::optional<std::string>(const std::string &)> &p_text) {
	std::vector<Diagnostic> lexed_errors;
	const std::shared_ptr<const SourceFile> lexed = tokenize(p_new, lexed_errors);
	const Token &word = lexed->tokens[0];
	if (!lexed_errors.empty() || !lexed->trivia.empty() || word.kind != TokenKind::Identifier || word.offset != 0 || word.end() != p_new.size()) {
		return refused(p_path, 0, "\"" + p_new + "\" is not a name.");
	}
	Rename result;
	const auto refuse = [&](const std::string &p_file, int p_line, const std::string &p_why) {
		result.refusals.push_back(refusal(p_file, p_line, p_why));
	};
	const ProjectApi &project = r_analysis.project_api();
	std::map<std::string, std::string> texts; // Of the scripts read, by path.
	const auto text_of = [&](const std::string &p_file) -> const std::string & {
		auto known = texts.find(p_file);
		if (known == texts.end()) {
			known = texts.emplace(p_file, project.script_source(p_file).value_or(std::string())).first;
		}
		return known->second;
	};
	const std::string &source = text_of(p_path);
	// The scripts of this language, the one renamed in first.
	std::vector<std::string> scripts{ p_path };
	std::map<std::string, Edits> edits; // In them, by file.
	std::set<DeclId> family;
	std::string old_name;
	std::string global; // The file whose class's global name it renames.
	std::vector<RenameEdit> candidates;
	// The scripts without the edits and with them.
	std::map<std::string, std::shared_ptr<const CompileResult>> before;
	{
		const Analysis::Pass pass(r_analysis);
		const std::optional<std::vector<Occurrence>> found = find_references(r_analysis, p_path, p_offset, p_other_scripts);
		if (!found) {
			return refused(p_path, 0, "There is no name here to rename.");
		}
		bool signal = false;
		std::set<std::string> owners; // The classes whose variable it renames.
		for (const Occurrence &each : *found) {
			const Reference &reference = each.reference;
			if (each.candidate) {
				continue;
			}
			if (reference.target.kind == Kind::Node) {
				return refused(p_path, 0, "A node is renamed in the scene editor.");
			}
			if (reference.target.kind != Kind::Script && reference.target.kind != Kind::Local) {
				return refused(p_path, 0, "It is not declared in this language.");
			}
			if (reference.overrides && reference.overrides->kind != Kind::Script) {
				return refused(each.file, each.line, "It overrides a method that the engine or another language calls by this name.");
			}
			family.insert(reference.target);
			signal = signal || reference.declares == SymbolKind::Signal;
			if (reference.declares == SymbolKind::Property && reference.target.kind == Kind::Script && reference.target.name.find('.') == std::string::npos) {
				owners.insert(reference.target.scope);
			}
			if (each.file == p_path && each.line > 0 && reference.begin <= p_offset && p_offset <= reference.end) {
				old_name = source.substr(reference.begin, reference.end - reference.begin);
			}
		}
		if (old_name.empty() || old_name == p_new) {
			return refused(p_path, 0, old_name.empty() ? "There is no name here to rename." : "It is named \"" + p_new + "\" already.");
		}
		for (const DeclId &id : family) {
			// A file's class by its global name, which the project lists.
			if (id.kind == Kind::Script && id.name.empty() && id.scope.find("::") == std::string::npos) {
				if (!(project.global_class_path(p_new).empty() && project.autoload_path(p_new).empty())) {
					return refused(p_path, 0, "\"" + p_new + "\" names a class or an autoload already.");
				}
				global = id.scope;
			}
		}
		std::map<std::string, std::set<StoredName>> stored; // By file.
		for (const Occurrence &each : *found) {
			if (each.line == 0) {
				for (const auto &[file, name] : stored_names(each.reference)) {
					stored[file].insert(name);
				}
				continue;
			}
			const std::string &text = text_of(each.file);
			if (each.reference.end > text.size()) {
				refuse(each.file, each.line, "The file changed.");
				continue;
			}
			const RenameEdit renamed{ each, { each.reference.begin, each.reference.end, text.substr(each.reference.begin, each.reference.end - each.reference.begin), p_new } };
			if (each.candidate) {
				candidates.push_back(renamed);
			} else if (renamed.edit.old_text != old_name) {
				refuse(each.file, each.line, "The name is written otherwise here: " + renamed.edit.old_text + ".");
			} else {
				result.edits.push_back(renamed);
				edits[each.file].push_back(renamed.edit);
			}
		}
		const bool members = std::any_of(family.begin(), family.end(), [](const DeclId &p_id) { return p_id.kind == Kind::Script; });
		// A place it rewrites that a scene uses, `p_there` in this scene too.
		const auto rewrites = [&](const std::function<bool(const Reference &)> &p_uses, const std::string &p_there) {
			return std::any_of(found->begin(), found->end(), [&](const Occurrence &p_each) {
				return p_each.line == 0 && (p_there.empty() || p_each.reference.scene == p_there) && p_uses(p_each.reference);
			});
		};
		const auto of_owner = [&](const std::string &p_script) {
			return std::any_of(owners.begin(), owners.end(), [&](const std::string &p_owner) { return r_analysis.pass_classes().is_or_extends(p_script, p_owner); });
		};
		for (const std::string &path : members ? project.scene_paths() : std::vector<std::string>()) {
			const std::optional<Scene> scene = project.scene(path);
			if (!scene) {
				continue;
			}
			// Where the scene's check refers a place it rewrites to no declaration
			// it renames, as when another scene's script or a .gd has the name.
			const auto another = [&](const std::string &p_file) {
				refuse(path, 0, "\"" + old_name + "\" here is another declaration, but renaming changes it in " + p_file + ".");
			};
			for (const SceneNamedMember &named : scene->named_members) {
				const auto shares = [&](const Reference &p_reference) {
					return std::find_first_of(named.places.begin(), named.places.end(), p_reference.named.places.begin(), p_reference.named.places.end()) != named.places.end();
				};
				if (rewrites(shares, "") && !rewrites([&](const Reference &p_reference) { return p_reference.named == named; }, path)) {
					another(named.places[0].resource.substr(0, named.places[0].resource.find("::")));
				}
			}
			for (const SceneConnection &connection : scene->connections) {
				const auto same = [&](const Reference &p_reference) { return p_reference.connection == connection; };
				if (rewrites(same, "") && !rewrites(same, path)) {
					another(connection.file);
				}
			}
			const std::string type = extension(path);
			if (type != "tscn" && type != "tres") {
				continue;
			}
			// A key under the new name on a node or a resource with the class, as
			// a removed export leaves: renaming would make its value take effect.
			StoredName stale;
			stale.kind = StoredName::Kind::Stale;
			stale.key = p_new;
			stale.node = true;
			for (const SceneNode &node : scene->nodes) {
				const bool stores = std::any_of(node.properties.begin(), node.properties.end(), [&](const auto &p_property) { return p_property.first == p_new; });
				if (stores && of_owner(script_of(project, *scene, node.path))) {
					stale.section = node.path;
					stored[path].insert(stale);
				}
			}
			// The scene index cannot see one on a resource, which the engine
			// drops as it loads it (src/adapter/pet_placeholder.cpp:113-116).
			stale.node = false;
			for (const SceneResource &resource : scene->resources) {
				if (of_owner(resource.script)) {
					const size_t at = resource.path.find("::");
					stale.section = at == std::string::npos ? std::string() : resource.path.substr(at + 2);
					stored[path].insert(stale);
				}
			}
			// The header of a resource file names its script's class, by which
			// preload() types it (scene/resources/resource_format_text.cpp:1806; ed1daf0).
			const std::optional<ResourceInfo> info = !global.empty() && type == "tres" ? project.resource(path) : std::nullopt;
			if (info && info->script_class == old_name && std::any_of(scene->resources.begin(), scene->resources.end(), [&](const SceneResource &p_resource) { return p_resource.path == path && p_resource.script == global; })) {
				StoredName header;
				header.kind = StoredName::Kind::Class;
				stored[path].insert(header);
			}
		}
		for (const auto &[file, names] : stored) {
			const std::string type = extension(file);
			// The binary formats of what stores names: scenes, animations,
			// synchronization configurations and other resources
			// (scene/resources/packed_scene.h:248, animation.h:40;
			// modules/multiplayer/scene_replication_config.h:39; core/io/resource.h:59; ed1daf0).
			if (type == "scn" || type == "res" || type == "anim" || type == "repl") {
				refuse(file, 0, std::string("It is a binary file, whose names cannot be rewritten. Save it as ") + (type == "scn" ? ".tscn" : ".tres") + " first.");
				continue;
			}
			if (type != "tscn" && type != "tres") {
				refuse(file, 0, "It is imported, and its names cannot be rewritten.");
				continue;
			}
			const std::optional<std::string> text = p_text(file);
			const LocatedNames located = text ? locate_names(*text, names, signal, old_name, p_new) : LocatedNames{ {}, "It cannot be read.", 0 };
			if (!located.error.empty()) {
				refuse(file, located.line, located.error);
				continue;
			}
			const std::vector<uint32_t> starts = line_starts_of(*text);
			for (const CheckedEdit &edit : located.edits) {
				RenameEdit renamed{ {}, edit };
				Occurrence &shown = renamed.occurrence;
				shown.file = file;
				const SourcePosition begin = position_in(*text, starts, edit.begin);
				shown.line = begin.line;
				shown.column = begin.column;
				shown.end_column = position_in(*text, starts, edit.end).column;
				const uint32_t line_end = size_t(shown.line) < starts.size() ? starts[shown.line] - 1 : uint32_t(text->size());
				shown.text = text->substr(starts[shown.line - 1], line_end - starts[shown.line - 1]);
				if (!shown.text.empty() && shown.text.back() == '\r') {
					shown.text.pop_back();
				}
				result.edits.push_back(renamed);
			}
		}
		if (!result.refusals.empty()) {
			result.edits.clear();
			return result;
		}
		// A local is not seen from the other scripts.
		for (const std::string &path : members ? project.script_paths() : std::vector<std::string>()) {
			if (is_script_path(path) && path != p_path) {
				scripts.push_back(path);
			}
		}
		for (const std::string &path : scripts) {
			before[path] = r_analysis.compile(path, text_of(path));
		}
	}

	std::map<std::string, std::string> edited_sources;
	for (auto &[file, in_file] : edits) {
		std::sort(in_file.begin(), in_file.end(), [](const CheckedEdit &p_a, const CheckedEdit &p_b) { return p_a.begin < p_b.begin; });
		edited_sources[file] = applied(text_of(file), in_file);
	}
	EditedProject edited(project, edited_sources);
	if (!global.empty()) {
		edited.rename_global(old_name, p_new, global);
	}
	// What an id is after the edits: the offsets of locals move, and a
	// declaration renamed takes what it declares inside it along. A member of
	// a type of this language is told by its name, as its type's name may
	// change.
	const auto renamed_id = [&](DeclId p_id) {
		const auto in_file = edits.find(script_file(p_id.scope));
		if (p_id.kind == Kind::Language) {
			p_id.scope.clear();
		}
		if (p_id.kind == Kind::Local && in_file != edits.end() && !p_id.name.empty()) {
			p_id.name = std::to_string(moved(in_file->second, uint32_t(std::stoul(p_id.name)), false));
		}
		for (const DeclId &member : family) {
			if (p_id.kind != Kind::Script || member.kind != Kind::Script) {
				continue;
			}
			const size_t inner = member.scope.rfind("::");
			if (member.name.empty() && inner != std::string::npos && (p_id.scope == member.scope || p_id.scope.rfind(member.scope + "::", 0) == 0)) {
				p_id.scope = member.scope.substr(0, inner + 2) + p_new + p_id.scope.substr(member.scope.size());
			} else if (!member.name.empty() && p_id.scope == member.scope && (p_id.name == member.name || p_id.name.rfind(member.name + ".", 0) == 0)) {
				p_id.name = member.name.substr(0, member.name.size() - old_name.size()) + p_new + p_id.name.substr(member.name.size());
			}
		}
		return p_id;
	};
	using Meaning = std::set<std::pair<DeclId, std::optional<DeclId>>>;
	const Analysis::Pass pass(r_analysis, &edited);
	for (const std::string &path : scripts) {
		const auto found = edited_sources.find(path);
		const std::shared_ptr<const CompileResult> after = r_analysis.compile(path, found == edited_sources.end() ? text_of(path) : found->second);
		// What it read did not change, so it refers to nothing renamed.
		if (after == before[path]) {
			continue;
		}
		const Edits &in_file = edits[path];
		const std::string &text = text_of(path);
		const std::vector<uint32_t> starts = line_starts_of(text);
		const auto line = [&](uint32_t p_offset) { return position_in(text, starts, p_offset).line; };
		std::multiset<uint32_t> errors;
		for (const Diagnostic &error : before[path]->errors) {
			if (error.severity == Severity::Error && error.file.empty()) {
				errors.insert(error.begin);
			}
		}
		for (const Diagnostic &error : after->errors) {
			if (error.severity != Severity::Error || !error.file.empty()) {
				continue;
			}
			const uint32_t at = moved(in_file, error.begin, true);
			const auto known = errors.find(at);
			if (known != errors.end()) {
				errors.erase(known);
			} else {
				refuse(path, line(at), "After renaming: " + error.message);
			}
		}
		std::map<uint32_t, Meaning> meant;
		std::map<uint32_t, Meaning> means;
		const auto overridden = [&](const Reference &p_reference, bool p_rename) {
			const std::optional<DeclId> &overrides = p_reference.overrides;
			return overrides ? std::optional<DeclId>(p_rename ? renamed_id(*overrides) : *overrides) : std::nullopt;
		};
		for (const Reference &reference : before[path]->references) {
			if (reference.scene.empty()) {
				meant[reference.begin].insert({ renamed_id(reference.target), overridden(reference, true) });
			}
		}
		for (const Reference &reference : after->references) {
			if (reference.scene.empty()) {
				DeclId target = reference.target;
				if (target.kind == Kind::Language) {
					target.scope.clear();
				}
				means[moved(in_file, reference.begin, true)].insert({ target, overridden(reference, false) });
			}
		}
		std::set<int> lines;
		for (const auto &[at, meaning] : means) {
			const auto was = meant.find(at);
			if ((was == meant.end() || was->second != meaning) && lines.insert(line(at)).second) {
				refuse(path, line(at), "After renaming, the name here refers to another declaration.");
			}
		}
		for (const auto &[at, meaning] : meant) {
			if (!means.count(at) && lines.insert(line(at)).second) {
				refuse(path, line(at), "After renaming, the name here refers to nothing.");
			}
		}
	}
	if (!result.refusals.empty()) {
		result.edits.clear();
		return result;
	}
	result.edits.insert(result.edits.end(), candidates.begin(), candidates.end());
	return result;
}

} // namespace petrichor::compiler
