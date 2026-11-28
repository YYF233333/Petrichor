#include "compiler/rename.h"

#include "compiler/ast.h"
#include "compiler/checker.h"
#include "compiler/lexer.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <tuple>

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

// The project as the edits leave it: the sources of the scripts, a global
// class name that renaming changes, and scenes whose nodes moved.
class EditedProject : public Overlay {
	std::string old_global;
	std::string new_global;
	std::string global_path;

public:
	std::map<std::string, Scene> scenes;

	using Overlay::Overlay;

	void rename_global(const std::string &p_old, const std::string &p_new, const std::string &p_path) {
		old_global = p_old;
		new_global = p_new;
		global_path = p_path;
	}

	std::optional<Scene> scene(const std::string &p_path) const override {
		const auto found = scenes.find(p_path);
		return found == scenes.end() ? Overlay::scene(p_path) : std::optional<Scene>(found->second);
	}
	// The edits rewrite the headers that name the class (architecture §6.4).
	std::optional<ResourceInfo> resource(const std::string &p_path) const override {
		std::optional<ResourceInfo> found = Overlay::resource(p_path);
		if (found && !global_path.empty() && found->script_class == old_global) {
			found->script_class = new_global;
		}
		return found;
	}
	std::string global_class_path(const std::string &p_name) const override {
		if (!global_path.empty() && (p_name == old_global || p_name == new_global)) {
			return p_name == new_global ? global_path : std::string();
		}
		return Overlay::global_class_path(p_name);
	}
	std::map<std::string, std::string> global_class_paths() const override {
		std::map<std::string, std::string> paths = Overlay::global_class_paths();
		if (!global_path.empty()) {
			paths.erase(old_global);
			paths[new_global] = global_path;
		}
		return paths;
	}
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

// An edit of the file's text, as finding references shows its place.
RenameEdit shown_edit(const std::string &p_file, const std::string &p_text, const std::vector<uint32_t> &p_starts, const CheckedEdit &p_edit) {
	RenameEdit made{ {}, p_edit };
	Occurrence &shown = made.occurrence;
	shown.file = p_file;
	const SourcePosition begin = position_in(p_text, p_starts, p_edit.begin);
	shown.line = begin.line;
	shown.column = begin.column;
	shown.end_column = position_in(p_text, p_starts, p_edit.end).column;
	const uint32_t line_end = size_t(shown.line) < p_starts.size() ? p_starts[shown.line] - 1 : uint32_t(p_text.size());
	shown.text = p_text.substr(p_starts[shown.line - 1], line_end - p_starts[shown.line - 1]);
	if (!shown.text.empty() && shown.text.back() == '\r') {
		shown.text.pop_back();
	}
	return made;
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

// The property and the node or the resource of a file that hold the scene it
// embeds at `p_path`, as messages name them (ScriptClasses::root_use).
std::string holder(const ProjectApi &p_project, const std::string &p_path) {
	const std::string file = p_path.substr(0, p_path.find("::"));
	const auto held = [&](const std::vector<std::pair<std::string, SceneValue>> &p_properties) {
		for (const auto &[name, value] : p_properties) {
			std::vector<std::string> scenes;
			value.add_scenes(scenes);
			if (std::find(scenes.begin(), scenes.end(), p_path) != scenes.end()) {
				return "\"" + name + "\" of ";
			}
		}
		return std::string();
	};
	for (const std::string &path : p_project.scene_paths()) {
		const std::optional<Scene> scene = path == file || path.rfind(file + "::", 0) == 0 ? p_project.scene(path) : std::nullopt;
		for (const SceneNode &node : scene ? scene->nodes : std::vector<SceneNode>()) {
			if (const std::string property = held(node.properties); !property.empty()) {
				return property + (node.path == "." ? std::string("the root") : node.path);
			}
		}
		for (const SceneResource &resource : scene ? scene->resources : std::vector<SceneResource>()) {
			if (const std::string property = held(resource.properties); !property.empty()) {
				return property + resource.shown();
			}
		}
	}
	return file;
}

// Where the node at `p_path` is after the moves, which give each moved node's
// new path by its old one, the root's too; a descendant moves with it.
std::string moved_path(const std::map<std::string, std::string> &p_moves, const std::string &p_path) {
	for (std::string at = p_path;; at = parent_path(at)) {
		const auto found = p_moves.find(at);
		if (found != p_moves.end()) {
			const std::string rest = at == "." ? (p_path == "." ? std::string() : "/" + p_path) : p_path.substr(at.size());
			return found->second == "." && !rest.empty() ? rest.substr(1) : found->second + rest;
		}
		if (at == ".") {
			return p_path;
		}
	}
}

// With a parent that the step adds or removes along with the move, as
// Reparent to New Node adds one (editor/docks/scene_tree_dock.cpp:3223-3274;
// ed1daf0), but not inside an instanced scene, whose own nodes are not listed.
Scene with_moves(Scene p_scene, const std::map<std::string, std::string> &p_moves) {
	std::set<std::string> paths;
	std::vector<std::string> moved;
	for (SceneNode &node : p_scene.nodes) {
		const std::string path = moved_path(p_moves, node.path);
		if (path != node.path) {
			moved.push_back(path);
		}
		node.path = path;
		paths.insert(path);
	}
	const auto in_instance = [&](const std::string &p_path) {
		return std::any_of(p_scene.nodes.begin(), p_scene.nodes.end(), [&](const SceneNode &p_node) { return p_node.is_instance && p_path.rfind(p_node.path + "/", 0) == 0; });
	};
	for (const std::string &path : moved) {
		for (std::string at = parent_path(path); at != "." && !in_instance(at) && paths.insert(at).second; at = parent_path(at)) {
			p_scene.nodes.push_back({ at, "Node" });
		}
	}
	return p_scene;
}

// Where a node that a path reaches is after the moves of the nodes of the
// scenes `p_moving`, by its place in the innermost of them.
std::string moved_node(const NodePathWalk::Node &p_node, const std::set<std::string> &p_moving, const std::map<std::string, std::string> &p_moves) {
	for (const auto &[root, scene] : p_node.scenes) {
		if (p_moving.count(scene)) {
			const std::string inner = root == "." ? p_node.path : p_node.path == root ? std::string(".")
																					  : p_node.path.substr(root.size() + 1);
			const std::string moved = moved_path(p_moves, inner);
			return root == "." ? moved : moved == "." ? root
													  : root + "/" + moved;
		}
	}
	return p_node.path;
}

// The path from one node to another, both from the same root, as
// NodePath::rel_path_to gives it.
std::string relative_path(const std::string &p_from, const std::string &p_to) {
	const std::vector<std::string> from = p_from == "." ? std::vector<std::string>() : path_parts(p_from);
	const std::vector<std::string> to = p_to == "." ? std::vector<std::string>() : path_parts(p_to);
	size_t common = 0;
	while (common < from.size() && common < to.size() && from[common] == to[common]) {
		common++;
	}
	std::string path;
	for (size_t i = common; i < from.size(); i++) {
		path += path.empty() ? ".." : "/..";
	}
	for (size_t i = common; i < to.size(); i++) {
		path += (path.empty() ? "" : "/") + to[i];
	}
	return path.empty() ? std::string(".") : path;
}

// Of `p_found`, the references of the name at `p_offset` in `p_path`.
RenamedName named(const std::optional<std::vector<Occurrence>> &p_found, const std::string &p_path, uint32_t p_offset) {
	RenamedName result;
	const auto refuse = [&](const std::string &p_file, int p_line, const std::string &p_why) {
		result.refusal = refusal(p_file, p_line, p_why);
		return result;
	};
	if (!p_found) {
		return refuse(p_path, 0, "There is no name here to rename.");
	}
	for (const Occurrence &each : *p_found) {
		const Reference &reference = each.reference;
		if (each.candidate) {
			continue;
		}
		if (reference.target.kind == Kind::Node) {
			return refuse(p_path, 0, "A node is renamed in the scene editor.");
		}
		if (reference.target.kind != Kind::Script && reference.target.kind != Kind::Local) {
			return refuse(p_path, 0, "It is not declared in this language.");
		}
		if (reference.overrides && reference.overrides->kind != Kind::Script) {
			return refuse(each.file, each.line, "It overrides a method that the engine or another language calls by this name.");
		}
		if (each.file == p_path && each.line > 0 && reference.begin <= p_offset && p_offset <= reference.end) {
			result.begin = reference.begin;
			result.end = reference.end;
		}
	}
	return result.end > result.begin ? result : refuse(p_path, 0, "There is no name here to rename.");
}

// `p_path` written as `p_written` writes a node path: after its "$" or "%",
// in quotes only when needed; or in the String of get_node(), with its quotes
// and prefix, and with escapes when needed.
std::string written_path(const std::string &p_written, const std::string &p_path) {
	const char sigil = p_written[0];
	if (sigil == '$' || sigil == '%') {
		const std::string rest = sigil == '%' ? p_path.substr(1) : p_path;
		return sigil + (plain_path(rest) ? rest : quoted(rest, '"'));
	}
	const size_t open = p_written.find_first_of("\"'");
	const char quote = p_written[open];
	const size_t quotes = p_written.size() >= open + 6 && p_written.compare(open, 3, std::string(3, quote)) == 0 ? 3 : 1;
	if (p_path.find('\\') == std::string::npos && p_path.find(quote) == std::string::npos) {
		return p_written.substr(0, open + quotes) + p_path + p_written.substr(p_written.size() - quotes);
	}
	std::string prefix = p_written.substr(0, open);
	prefix.erase(std::remove(prefix.begin(), prefix.end(), 'r'), prefix.end());
	return prefix + quoted(p_path, '"');
}

} // namespace

RenamedName renamed_name(Analysis &r_analysis, const std::string &p_path, uint32_t p_offset, const std::vector<std::string> &p_other_scripts) {
	return named(find_references(r_analysis, p_path, p_offset, p_other_scripts), p_path, p_offset);
}

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
	// Where a pattern written out puts the name of a field, by file, and where
	// the name was before.
	std::map<std::string, std::map<uint32_t, uint32_t>> anchors;
	std::set<DeclId> family;
	std::string old_name;
	std::string global; // The file whose class's global name it renames.
	std::vector<RenameEdit> candidates;
	// The scripts without the edits and with them.
	std::map<std::string, std::shared_ptr<const CompileResult>> before;
	{
		const Analysis::Pass pass(r_analysis);
		const std::optional<std::vector<Occurrence>> found = find_references(r_analysis, p_path, p_offset, p_other_scripts);
		const RenamedName name = named(found, p_path, p_offset);
		if (name.refusal) {
			result.refusals.push_back(*name.refusal);
			return result;
		}
		old_name = source.substr(name.begin, name.end - name.begin);
		if (old_name == p_new) {
			return refused(p_path, 0, "It is named \"" + p_new + "\" already.");
		}
		bool signal = false;
		std::set<std::string> owners; // The classes whose variable it renames.
		for (const Occurrence &each : *found) {
			const Reference &reference = each.reference;
			if (each.candidate) {
				continue;
			}
			family.insert(reference.target);
			signal = signal || reference.declares == SymbolKind::Signal;
			if (reference.declares == SymbolKind::Property && reference.target.kind == Kind::Script && reference.target.name.find('.') == std::string::npos) {
				owners.insert(reference.target.scope);
			}
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
		// Where `var x` among named fields, which is `x = var x` (spec §5.6),
		// has the name of one of the two only: its `var`.
		const auto shorthand = [&](const Occurrence &p_each) -> std::optional<uint32_t> {
			const std::string &text = text_of(p_each.file);
			uint32_t start = p_each.reference.begin;
			while (start > 0 && (text[start - 1] == ' ' || text[start - 1] == '\t')) {
				start--;
			}
			const std::shared_ptr<const CompileResult> checked = r_analysis.compile(p_each.file, text);
			const bool shared = std::any_of(checked->references.begin(), checked->references.end(), [&](const Reference &p_other) {
				return p_other.begin == p_each.reference.begin && p_other.end == p_each.reference.end && !family.count(p_other.target);
			});
			return shared && start >= 3 && text.compare(start - 3, 3, "var") == 0 ? std::optional<uint32_t>(start - 3) : std::nullopt;
		};
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
				continue;
			}
			if (renamed.edit.old_text != old_name) {
				refuse(each.file, each.line, "The name is written otherwise here: " + renamed.edit.old_text + ".");
				continue;
			}
			// Written out, the pattern keeps the other one's name.
			const std::optional<uint32_t> var = shorthand(each);
			const bool field = each.reference.target.kind == Kind::Script;
			if (var) {
				const RenameEdit written{ each, { *var, *var, std::string(), (field ? p_new : old_name) + " = " } };
				result.edits.push_back(written);
				edits[each.file].push_back(written.edit);
				anchors[each.file][*var] = each.reference.begin;
			}
			if (!var || !field) {
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
			// A key under the new name on a node or a resource with the class, as
			// a removed export leaves: renaming would make its value take effect.
			// A binary scene with one refuses it.
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
			const std::string type = extension(path.substr(0, path.find("::")));
			if (type != "tscn" && type != "tres") {
				continue;
			}
			// The scene index cannot see one on a resource, which the engine
			// drops as it loads it (src/adapter/pet_placeholder.cpp:113-116).
			stale.node = false;
			for (const SceneResource &resource : scene->resources) {
				if (of_owner(resource.script)) {
					const size_t at = resource.path.find("::");
					stale.section = at == std::string::npos ? std::string() : resource.path.substr(at + 2);
					stored[resource.path.substr(0, at)].insert(stale);
				}
			}
			// The header of a resource file names its script's class, by which
			// preload() types it (scene/resources/resource_format_text.cpp:1806; ed1daf0).
			const std::optional<ResourceInfo> info = !global.empty() && type == "tres" && path.find("::") == std::string::npos ? project.resource(path) : std::nullopt;
			if (info && info->script_class == old_name && std::any_of(scene->resources.begin(), scene->resources.end(), [&](const SceneResource &p_resource) { return p_resource.path == path && p_resource.script == global; })) {
				StoredName header;
				header.kind = StoredName::Kind::Class;
				stored[path].insert(header);
			}
		}
		for (const auto &[file, names] : stored) {
			// Its nodes' names and their keys share the `names` table of `_bundled`.
			const size_t embedded = file.find("::");
			if (embedded != std::string::npos) {
				refuse(file.substr(0, embedded), 0, "It is stored in the scene embedded in " + holder(project, file) + ", whose names cannot be rewritten. Save that scene to a file of its own first, with \"Save As...\" on the property.");
				continue;
			}
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
				result.edits.push_back(shown_edit(file, *text, starts, edit));
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

	EditedProject edited(project);
	for (auto &[file, in_file] : edits) {
		std::sort(in_file.begin(), in_file.end(), [](const CheckedEdit &p_a, const CheckedEdit &p_b) { return p_a.begin < p_b.begin; });
		edited.scripts[file] = applied(text_of(file), in_file);
	}
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
		const std::shared_ptr<const CompileResult> after = r_analysis.compile(path, edited.script_source(path).value_or(std::string()));
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
				const uint32_t at = moved(in_file, reference.begin, true);
				const auto anchor = anchors[path].find(at);
				means[anchor == anchors[path].end() ? at : anchor->second].insert({ target, overridden(reference, false) });
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

MovedNodes move_nodes(Analysis &r_analysis, const std::string &p_scene, const std::map<std::string, std::string> &p_moved) {
	const ProjectApi &project = r_analysis.project_api();
	std::map<std::string, std::string> back;
	for (const auto &[from, to] : p_moved) {
		back[to] = from;
	}
	EditedProject before(project);
	EditedProject after(project);
	// The scene and those that inherit it have the nodes moved, by their own
	// paths. One that inherits it is taken as before or after the move,
	// whichever it is, unless the root moves, which every scene has.
	std::set<std::string> moving{ p_scene };
	for (const std::string &path : project.scene_paths()) {
		const std::optional<Scene> scene = project.scene(path);
		if (!scene) {
			continue;
		}
		std::optional<Scene> at = scene;
		for (int depth = 0; at && at->path != p_scene && depth < 32; depth++) {
			at = at->base.empty() ? std::nullopt : project.scene(at->base);
		}
		if (path == p_scene) {
			before.scenes[path] = with_moves(*scene, back);
		} else if (at && at->path == p_scene && !p_moved.count(".")) {
			moving.insert(path);
			before.scenes[path] = with_moves(*scene, back);
			after.scenes[path] = with_moves(*scene, p_moved);
		}
	}
	const auto after_move = [&](const NodePathWalk::Node &p_node) { return moved_node(p_node, moving, p_moved); };
	// A path keeps what it has up to its last `%Name`, which takes the node's
	// new name, and goes on from that node.
	const auto anew = [&](const NodePathWalk &p_walk) {
		const std::string from = after_move(p_walk.from);
		const std::string to = after_move(p_walk.to);
		const std::vector<std::string> parts = path_parts(p_walk.text);
		size_t unique = parts.size();
		for (size_t i = 0; i < parts.size(); i++) {
			unique = parts[i][0] == '%' ? i : unique;
		}
		if (unique == parts.size()) {
			return relative_path(from, to);
		}
		std::string path;
		for (size_t i = 0; i < unique; i++) {
			path += parts[i] + "/";
		}
		path += "%" + from.substr(from.rfind('/') + 1);
		return from == to ? path : path + "/" + relative_path(from, to);
	};

	MovedNodes result;
	std::map<std::string, std::string> texts;
	std::map<std::string, std::map<uint32_t, std::vector<NodePathWalk>>> walks; // By file and where each path begins.
	{
		const Analysis::Pass pass(r_analysis, &before);
		for (const std::string &path : project.script_paths()) {
			const std::optional<std::string> source = is_script_path(path) ? project.script_source(path) : std::nullopt;
			if (!source) {
				continue;
			}
			texts[path] = *source;
			for (const NodePathWalk &walk : r_analysis.compile(path, *source)->node_paths) {
				walks[path][walk.begin].push_back(walk);
			}
		}
	}
	std::map<std::string, std::vector<RenameEdit>> pending; // By file.
	std::map<std::string, Edits> edits; // The same.
	for (const auto &[file, in_file] : walks) {
		const std::string &text = texts[file];
		const std::vector<uint32_t> starts = line_starts_of(text);
		for (const auto &[begin, group] : in_file) {
			if (std::none_of(group.begin(), group.end(), [&](const NodePathWalk &p_walk) { return after_move(p_walk.from) != p_walk.from.path || after_move(p_walk.to) != p_walk.to.path; })) {
				continue;
			}
			const std::string written = text.substr(begin, group[0].end - begin);
			std::map<std::string, std::string> paths; // Written anew, each with a scene it is for.
			for (const NodePathWalk &walk : group) {
				paths.emplace(anew(walk), walk.scene);
			}
			if (paths.size() > 1) {
				std::string each;
				for (const auto &[path, scene] : paths) {
					each += (each.empty() ? "" : " and ") + written_path(written, path) + " in " + scene;
				}
				result.reports.push_back(refusal(file, position_in(text, starts, begin).line, written + " is not updated: it would be " + each + "."));
				continue;
			}
			pending[file].push_back(shown_edit(file, text, starts, { begin, group[0].end, written, written_path(written, paths.begin()->first) }));
			if (paths.begin()->first != group[0].text) {
				edits[file].push_back(pending[file].back().edit);
			}
		}
	}
	// Each path, written anew or not, must lead where the nodes it led to are,
	// in every scene where it did.
	for (const auto &[file, in_file] : pending) {
		after.scripts[file] = applied(texts[file], edits[file]);
	}
	const Analysis::Pass pass(r_analysis, &after);
	for (const auto &[file, in_file] : pending) {
		using Walked = std::set<std::tuple<std::string, std::string, std::string>>;
		std::map<uint32_t, Walked> reached;
		for (const NodePathWalk &walk : r_analysis.compile(file, after.scripts[file])->node_paths) {
			reached[moved(edits[file], walk.begin, true)].insert({ walk.scene, walk.from.path, walk.to.path });
		}
		for (const RenameEdit &edit : in_file) {
			Walked expected;
			for (const NodePathWalk &walk : walks[file][edit.edit.begin]) {
				expected.insert({ walk.scene, after_move(walk.from), after_move(walk.to) });
			}
			const Walked &found = reached[edit.edit.begin];
			const auto other = std::find_if(found.begin(), found.end(), [&](const auto &p_walk) { return !expected.count(p_walk); });
			const auto lacking = std::find_if(expected.begin(), expected.end(), [&](const auto &p_walk) { return !found.count(p_walk); });
			const bool same = edit.edit.old_text == edit.edit.new_text;
			if (other == found.end() && lacking == expected.end()) {
				if (!same) {
					result.edits.push_back(edit);
				}
				continue;
			}
			const std::string where = (other != found.end() ? "another node in " : "no node in ") + std::get<0>(other != found.end() ? *other : *lacking) + ".";
			result.reports.push_back(refusal(file, edit.occurrence.line, edit.edit.old_text + (same ? " now leads to " : " is not updated: " + edit.edit.new_text + " would lead to ") + where));
		}
	}

	// The names of the nodes renamed or moved themselves, not with a parent.
	std::set<std::string> names;
	for (const auto &[from, to] : p_moved) {
		const std::string name = from.substr(from.rfind('/') + 1);
		const std::string parent = moved_path(p_moved, parent_path(from));
		if (from != "." && to != (parent == "." ? name : parent + "/" + name)) {
			names.insert(name);
		}
	}
	std::set<std::string> placed;
	for (const std::string &scene : moving) {
		for (const SceneNode &node : before.scenes[scene].nodes) {
			placed.insert(node.script.empty() ? node.instance_script : node.script);
		}
	}
	for (const std::string &file : placed) {
		if (!texts.count(file)) {
			continue;
		}
		std::vector<Diagnostic> errors;
		const std::shared_ptr<const SourceFile> source = tokenize(texts[file], errors);
		const std::vector<Token> &tokens = source->tokens;
		for (size_t i = 0; i + 1 < tokens.size(); i++) {
			if (!(is_word(tokens[i], "get_node") || is_word(tokens[i], "get_node_or_null")) || tokens[i + 1].kind != TokenKind::ParenOpen) {
				continue;
			}
			std::vector<const Token *> strings;
			size_t close = i + 2;
			for (int depth = 1; close < tokens.size() && tokens[close].kind != TokenKind::EndOfFile; close++) {
				depth += tokens[close].kind == TokenKind::ParenOpen ? 1 : tokens[close].kind == TokenKind::ParenClose ? -1
																													  : 0;
				if (depth == 0) {
					break;
				}
				if (tokens[close].kind == TokenKind::String) {
					strings.push_back(&tokens[close]);
				}
			}
			// One String on self is a literal path, written anew above.
			const bool on_self = i == 0 || tokens[i - 1].kind != TokenKind::Period || (i >= 2 && tokens[i - 2].kind == TokenKind::Self);
			if (on_self && close == i + 3 && strings.size() == 1) {
				continue;
			}
			for (const Token *string : strings) {
				const std::vector<std::string> parts = path_parts(string->text);
				if (std::any_of(parts.begin(), parts.end(), [&](const std::string &p_part) { return names.count(p_part[0] == '%' ? p_part.substr(1) : p_part); })) {
					result.reports.push_back(refusal(file, source->line(uint32_t(i)), "This get_node() is not updated, though \"" + string->text + "\" names a node renamed or moved."));
					break;
				}
			}
		}
	}
	return result;
}

} // namespace petrichor::compiler
