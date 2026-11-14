#include "compiler/checker_internal.h"

namespace petrichor::compiler::checking {

/* Node paths and scene views (spec §7.1) */

// A node path as written: $Head/Camera, %Name or $"../Level".
std::string Checker::shown_path(const std::string &p_path) {
	if (p_path[0] == '%') {
		return p_path;
	}
	return plain_path(p_path) ? "$" + p_path : "$\"" + p_path + "\"";
}

// Whether `$` takes a path without quotes: names joined by "/", each
// possibly after "%".
bool Checker::plain_path(const std::string &p_path) {
	for (const std::string &part : path_parts(p_path)) {
		const size_t name = !part.empty() && part[0] == '%' ? 1 : 0;
		if (part.size() == name || std::isdigit((unsigned char)part[name]) || !std::all_of(part.begin() + name, part.end(), [](char p_char) { return std::isalnum((unsigned char)p_char) || p_char == '_'; })) {
			return false;
		}
	}
	return true;
}

std::vector<std::string> Checker::path_parts(const std::string &p_path) {
	std::vector<std::string> parts;
	for (size_t start = 0;;) {
		const size_t slash = p_path.find('/', start);
		parts.push_back(p_path.substr(start, slash - start));
		if (slash == std::string::npos) {
			return parts;
		}
		start = slash + 1;
	}
}

std::string Checker::parent_path(const std::string &p_path) {
	const size_t slash = p_path.rfind('/');
	return slash == std::string::npos ? std::string(".") : p_path.substr(0, slash);
}

// The nodes of a scene as the engine builds it (spec §7.1): its own, with
// the nodes of each scene it instances under the instance, whose root owns
// them, and the changes it stores for those
// (scene/resources/packed_scene.cpp:282-300, 567-573; ed1daf0).
const std::vector<Checker::ViewNode> &Checker::view_of(const Scene &p_scene, int p_depth) {
	const auto known = views.find(p_scene.path);
	if (known != views.end()) {
		return known->second;
	}
	std::vector<ViewNode> nodes;
	for (const SceneNode &node : p_scene.nodes) {
		if (node.type.empty() && !node.is_instance) {
			continue; // A change of an instanced scene's node, applied below.
		}
		nodes.push_back({ node.path, node, p_scene.path, node.path == "." ? std::string() : std::string(".") });
		// A scene cannot instance itself; the depth stops a project that says so.
		const std::optional<Scene> inner = node.instance.empty() || p_depth > 32 ? std::nullopt : classes.scene_structure(node.instance);
		for (const ViewNode &each : inner ? view_of(*inner, p_depth + 1) : std::vector<ViewNode>()) {
			if (each.path != ".") {
				nodes.push_back({ node.path + "/" + each.path, each.node, each.scene, each.owner == "." ? node.path : node.path + "/" + each.owner });
			}
		}
	}
	for (const SceneNode &change : p_scene.nodes) {
		const auto changed = std::find_if(nodes.begin(), nodes.end(), [&](const ViewNode &p_node) { return p_node.path == change.path; });
		if (change.type.empty() && !change.is_instance && !change.script.empty() && changed != nodes.end()) {
			changed->node.script = change.script;
			changed->node.instance_script.clear();
		}
	}
	return views.emplace(p_scene.path, std::move(nodes)).first->second;
}

const Checker::ViewNode *Checker::view_node(const Scene &p_scene, const std::function<bool(const ViewNode &)> &p_is) {
	const std::vector<ViewNode> &view = view_of(p_scene);
	const auto found = std::find_if(view.begin(), view.end(), p_is);
	return found == view.end() ? nullptr : &*found;
}

Checker::NodeWalk Checker::walk_node_path(const Scene &p_scene, const std::string &p_from, const std::string &p_path) {
	const auto find = [&](const std::function<bool(const ViewNode &)> &p_is) { return view_node(p_scene, p_is); };
	const auto at_path = [&](const std::string &p_at) { return view_node(p_scene, p_at); };
	NodeWalk walk;
	walk.scene = p_scene.path;
	walk.missing = p_scene.path + ": " + (p_from == "." ? std::string("the root") : p_from) + " has no node at \"" + p_path + "\".";
	const ViewNode *at = at_path(p_from);
	for (const std::string &part : path_parts(p_path)) {
		if (!at) {
			return walk;
		}
		if (part == "..") {
			if (at->path == ".") {
				walk.leaves = true;
				return walk;
			}
			at = at_path(parent_path(at->path));
		} else if (!part.empty() && part[0] == '%') {
			const std::string name = part.substr(1);
			const auto owned_by = [&](const std::string &p_owner) {
				return find([&](const ViewNode &p_node) { return p_node.node.unique && p_node.owner == p_owner && p_node.path.substr(p_node.path.rfind('/') + 1) == name; });
			};
			const ViewNode *unique = owned_by(at->path);
			if (!unique && at->owner.empty()) {
				walk.leaves = true;
				return walk;
			}
			at = unique ? unique : owned_by(at->owner);
			if (!at) {
				walk.missing = p_scene.path + ": no node has the unique name " + part + ".";
			}
		} else if (!part.empty() && part != ".") {
			at = at_path(at->path == "." ? part : at->path + "/" + part);
		}
		walk.reached.push_back(at);
	}
	walk.node = at;
	return walk;
}

// Each node that the path `p_at` holds leads to from a node of a scene that
// has the script (spec §7.1): in that scene, or, past its root, in each
// scene that instances it, as it is there. A scene that lacks the node has the error
// marked on it, at `p_marked`, unless the path is optional or the scene
// is used only as the base of inherited scenes (spec §7.2).
void Checker::walk_from(const Expr &p_at, const Scene &p_scene, const std::string &p_from, const std::string &p_marked, const std::string &p_shown, bool p_optional, std::vector<NodeWalk> &r_found, int p_depth) {
	NodeWalk walk = walk_node_path(p_scene, p_from, p_at.text);
	if (walk.node) {
		r_found.push_back(std::move(walk));
		return;
	}
	std::string missing = walk.missing;
	const std::vector<Scene> outers = p_depth > 32 ? std::vector<Scene>() : classes.scenes_instancing(p_scene.path);
	bool instanced = false;
	for (const Scene &outer : walk.leaves ? outers : std::vector<Scene>()) {
		for (const SceneNode &node : outer.nodes) {
			if (node.instance == p_scene.path) {
				instanced = true;
				walk_from(p_at, outer, p_from == "." ? node.path : node.path + "/" + p_from, node.path, p_shown, p_optional, r_found, p_depth + 1);
			}
		}
	}
	if (walk.leaves) {
		missing = p_scene.path + ": " + p_shown + " leads out of the scene, and no scene instances it.";
	}
	// Such as $"../Music" for an autoload beside the main scene (spec §7.3).
	std::string named = p_at.text;
	while (named.rfind("../", 0) == 0 || named.rfind("./", 0) == 0) {
		named = named.substr(named.find('/') + 1);
	}
	const std::string way = autoload_way(named);
	if (!way.empty()) {
		missing += " " + way.substr(0, way.find('.')) + " is an autoload; write " + way + ".";
	}
	const bool only_inherited = !outers.empty() && p_scene.path != options.project->main_scene() && std::all_of(outers.begin(), outers.end(), [&](const Scene &p_outer) {
		return p_outer.base == p_scene.path && std::none_of(p_outer.nodes.begin(), p_outer.nodes.end(), [&](const SceneNode &p_node) { return p_node.instance == p_scene.path; });
	});
	if (!instanced && !p_optional && !only_inherited) {
		error_in_scene(p_at.range, p_scene, p_marked, missing);
	}
}

// Why a literal path from the root into an autoload is a compile error:
// the autoload is reached by its name (spec §7.3). Empty for another path.
std::string Checker::through_autoload(const std::string &p_path, const std::string &p_written) const {
	const std::string root = "/root/";
	const std::string way = p_path.rfind(root, 0) == 0 ? autoload_way(p_path.substr(root.size())) : std::string();
	if (way.empty()) {
		return std::string();
	}
	const std::string name = way.substr(0, way.find('.'));
	return p_written + (way == name ? " is the autoload " : " is in the autoload ") + name + "; write " + way + ".";
}

// How to reach what a path names when its first name is an autoload's:
// the autoload, or get_node() of the rest on it. Empty for another path.
std::string Checker::autoload_way(const std::string &p_path) const {
	const size_t end = p_path.find('/');
	const std::string name = p_path.substr(0, end);
	if (name.empty() || !options.project || options.project->autoload_path(name).empty()) {
		return std::string();
	}
	return end == std::string::npos ? name : name + ".get_node(\"" + p_path.substr(end + 1) + "\")";
}

// `$X` (spec §7.1): the node at the path from every node that has this
// script, in every scene; they must agree on its class. `$".."` is any
// Node, or what @parent(T) requires (spec §7.3).
Type Checker::check_get_node(Expr &r_expr) {
	if (options.declarations_only) {
		return set(r_expr, Type::of(TypeKind::Error)); // Scenes are not read while loading.
	}
	const std::string &path = r_expr.text;
	if (refuse_in_static(r_expr, shown_path(path))) {
		return set(r_expr, Type::of(TypeKind::Error));
	}
	// Through the MethodBind, as GDScript calls it (modules/gdscript/gdscript_compiler.cpp:742-753).
	if (const std::optional<EngineMethod> get_node = engine.find_method("Node", "get_node")) {
		r_expr.hash = get_node->hash;
	}
	if (path == "..") {
		const Type parent = cls.parent_ref ? cls.parent_type : Type::object("Node");
		r_expr.owner = parent.engine_class();
		return set(r_expr, parent);
	}
	const std::string autoload = through_autoload(path, shown_path(path));
	if (!autoload.empty()) {
		return fail(r_expr, autoload);
	}
	if (path.empty()) {
		return fail(r_expr, "Only paths from this node, such as $Head/Camera, are supported yet.");
	}
	// A node that code put under the root, as get_node() types it (spec §7.3).
	if (path[0] == '/') {
		r_expr.owner = "Node";
		return set(r_expr, Type::object("Node"));
	}
	const Type type = scene_node_at(r_expr, shown_path(path), false);
	r_expr.owner = type.engine_class();
	return set(r_expr, type);
}

// Walks the path that `p_path_expr` holds from each node of the scenes with
// the class; whether there is one.
bool Checker::walk_placements(const std::vector<Scene> &p_scenes, const Expr &p_path_expr, const std::string &p_shown, bool p_optional, std::vector<NodeWalk> &r_found) {
	bool attached = false;
	for (const Scene &scene : p_scenes) {
		for (const SceneNode &node : scene.nodes) {
			if (extends_script(node.script, options.path)) {
				attached = true;
				walk_from(p_path_expr, scene, node.path, node.path, p_shown, p_optional, r_found);
			}
		}
	}
	return attached;
}

// Each literal node path of the class leads to a node from the nodes of
// this scene with the class (spec §7.1). Where no scene that places it has
// the node, the script's check reports them instead, as `$X` has no type.
void Checker::check_node_paths() {
	for_each_node_path(cls, [&](const Expr &p_expr) {
		const bool call = p_expr.kind == ExprKind::Call;
		const Expr &path = call ? *p_expr.args[0] : p_expr;
		if (path.text.empty() || path.text[0] == '/' || path.text == "..") {
			return;
		}
		const std::string shown = call ? p_expr.text + "(\"" + path.text + "\")" : shown_path(path.text);
		const bool optional = p_expr.text == "get_node_or_null";
		std::vector<NodeWalk> found;
		const size_t lacking = errors.size();
		walk_placements(scenes, path, shown, optional, found);
		if (found.empty() && errors.size() > lacking) {
			const size_t elsewhere = errors.size();
			walk_placements(classes.scenes_with_script(options.path), path, shown, optional, found);
			errors.erase(errors.begin() + std::ptrdiff_t(found.empty() ? lacking : elsewhere), errors.end());
		}
	});
}

// The node at a literal path from this node, `p_path_expr` holding the
// path, as `$X` and get_node() name it (spec §7.1, §7.2): its type in every
// scene that gives this script a node, which must agree. With `p_optional`,
// for get_node_or_null(), a scene may lack it and the type is T?, or Node?
// when no scene has it. Where some scene has it, the checks of those that
// lack it report them (architecture §6.4); completion still offers only
// the paths every scene has. Error after reporting.
Type Checker::scene_node_at(const Expr &p_path_expr, const std::string &p_shown, bool p_optional) {
	std::vector<NodeWalk> found;
	const size_t lacking = errors.size();
	const bool attached = walk_placements(scenes, p_path_expr, p_shown, p_optional, found);
	if (!found.empty() && !completion) {
		errors.erase(errors.begin() + std::ptrdiff_t(lacking), errors.end());
	}
	std::optional<Type> type;
	std::string type_scene;
	for (const NodeWalk &walk : found) {
		refer_node_path(p_path_expr, walk);
		const Type found_type = scene_node_type(p_path_expr.range, walk.node->node, p_shown);
		if (found_type.is_error()) {
			return found_type;
		}
		if (!type) {
			type = found_type;
			type_scene = walk.scene;
		} else if (found_type != *type) {
			error(p_path_expr.range, p_shown + " is a " + type->to_string() + " in " + type_scene + " but a " + found_type.to_string() + " in " + walk.scene + "; assign it to a variable typed with a class both extend.");
			return Type::of(TypeKind::Error);
		}
	}
	if (!type && p_optional) {
		return Type::object("Node", true);
	}
	if (!attached) {
		error(p_path_expr.range, p_shown + " is typed from the scenes that use this script, and no scene does.");
	}
	if (!type) {
		return Type::of(TypeKind::Error); // Reported for each scene above.
	}
	Type result = *type;
	result.nullable = p_optional;
	return result;
}

// get_node() and get_node_or_null() of a literal path from this node,
// typed as `$X` (spec §7.1, §7.2); get_node("..") as `$".."`, and
// get_node_or_null("..") as the engine types it (§7.3). nullopt for any
// other call, and for a path from the root.
std::optional<Type> Checker::literal_node_type(const Expr &p_call, const EngineMethod &p_method) {
	const bool optional = p_method.name == "get_node_or_null";
	if (p_method.owner != "Node" || (p_method.name != "get_node" && !optional) || p_call.args.size() != 1 || p_call.args[0]->kind != ExprKind::StringLiteral || (p_call.base && p_call.base->kind != ExprKind::Self)) {
		return std::nullopt;
	}
	const std::string &path = p_call.args[0]->text;
	if (path.empty() || path[0] == '/' || (optional && path == "..")) {
		return std::nullopt;
	}
	if (options.declarations_only) {
		return Type::of(TypeKind::Error); // Scenes are not read while loading.
	}
	if (path == "..") {
		return cls.parent_ref ? cls.parent_type : Type::object("Node");
	}
	return scene_node_at(*p_call.args[0], p_call.text + "(\"" + path + "\")", optional);
}

// Each name in the path of `$X` refers to the node it reaches in a scene;
// the name of `%Name` without the "%". Not the parts of a string written
// with escapes.
void Checker::refer_node_path(const Expr &p_get_node, const NodeWalk &p_walk) {
	const std::vector<Token> &tokens = cls.source->tokens;
	using Bytes = std::optional<std::pair<uint32_t, uint32_t>>;
	std::vector<Bytes> parts;
	for (uint32_t i = p_get_node.range.first; i <= p_get_node.range.last && i < tokens.size(); i++) {
		const Token &token = tokens[i];
		if (token.kind == TokenKind::Dollar || token.kind == TokenKind::Percent || token.kind == TokenKind::Slash) {
			continue;
		}
		if (token.kind != TokenKind::String) {
			parts.push_back(Bytes({ token.offset, token.end() }));
			continue;
		}
		const bool plain = token.length == token.text.size() + 2;
		uint32_t first = 0; // Of the part in the string.
		for (uint32_t c = 0; c <= token.text.size(); c++) {
			if (c < token.text.size() && token.text[c] != '/') {
				continue;
			}
			const uint32_t unique = c > first && token.text[first] == '%';
			parts.push_back(plain ? Bytes({ token.offset + 1 + first + unique, token.offset + 1 + c }) : Bytes());
			first = c + 1;
		}
	}
	const std::vector<std::string> names = path_parts(p_get_node.text);
	if (parts.size() != names.size() || p_walk.reached.size() != names.size()) {
		return;
	}
	for (size_t i = 0; i < names.size(); i++) {
		if (parts[i] && !names[i].empty() && names[i] != "." && names[i] != "..") {
			refer_bytes(parts[i]->first, parts[i]->second, { DeclId::Kind::Node, p_walk.reached[i]->scene, p_walk.reached[i]->node.path });
		}
	}
}

// The type of a node of a scene (spec §7.1, §7.4): the class of its script
// when that is a script of this language, or else its engine class; Error
// after reporting. `p_what` names the node in messages.
Type Checker::scene_node_type(const TokenRange &p_at, const SceneNode &p_node, const std::string &p_what) {
	const std::string &script = p_node.script.empty() ? p_node.instance_script : p_node.script;
	if (is_script_path(script)) {
		const ClassDecl *header = script_header(script);
		if (!header) {
			error(p_at, p_what + " has the script " + script + ", which cannot be read.");
			return Type::of(TypeKind::Error);
		}
		return script_class_type(*header, script);
	}
	// A script of another language, by its class as reflection shows it
	// (spec §11.1); by the engine class when it cannot be read.
	const ForeignClass *foreign = script.empty() ? nullptr : foreign_class(script);
	if (foreign && foreign->members_known && !foreign->native.empty()) {
		return foreign_class_type(script, *foreign);
	}
	if (!engine.class_exists(p_node.type)) {
		error(p_at, p_what + " is a " + p_node.type + ", which is not supported yet.");
		return Type::of(TypeKind::Error);
	}
	return Type::object(p_node.type);
}

/* load() and preload() */

bool Checker::has_extension(const std::string &p_path, std::initializer_list<const char *> p_extensions) {
	const size_t dot = p_path.find_last_of("./");
	const std::string extension = dot != std::string::npos && p_path[dot] == '.' ? p_path.substr(dot) : std::string();
	return std::any_of(p_extensions.begin(), p_extensions.end(), [&](const char *p_extension) { return extension == p_extension; });
}

// The path that a preload() of one literal String names, from this
// script's directory when it is relative, as in GDScript; a uid:// path as
// written. Empty for any other preload().
std::string Checker::preload_path(const Expr &p_call) const {
	// As in GDScript, not a StringName or a NodePath
	// (modules/gdscript/gdscript_analyzer.cpp:4735-4736).
	if (p_call.args.size() != 1 || p_call.args[0]->kind != ExprKind::StringLiteral || p_call.args[0]->op != TokenKind::String) {
		return std::string();
	}
	return resolved_path(p_call.args[0]->text, options.path);
}

// A preload whose file loads this script again is a compile error: the
// class loads its preloads as it loads, which that file would wait for
// (spec §7.6).
void Checker::check_load_cycle(const Expr &p_preload, const std::string &p_file) {
	std::set<std::string> seen;
	std::vector<std::string> chain;
	if (!loads_this_script(p_file, seen, chain)) {
		return;
	}
	std::string cycle;
	for (const std::string &file : chain) {
		cycle += file + " -> ";
	}
	error(p_preload.range, "This preload loads this script again (" + cycle + script_file(options.path) + "); load one of them with load() where it is used.");
}

bool Checker::loads_this_script(const std::string &p_file, std::set<std::string> &r_seen, std::vector<std::string> &r_chain) {
	if (p_file == script_file(options.path)) {
		return true;
	}
	if (p_file.empty() || !r_seen.insert(p_file).second) {
		return false;
	}
	r_chain.push_back(p_file);
	for (const std::string &next : loaded_with(p_file)) {
		if (loads_this_script(next, r_seen, r_chain)) {
			return true;
		}
	}
	r_chain.pop_back();
	return false;
}

// What loading a file loads with it: a scene's or a resource's
// ext_resources; a script's base and the files it preloads, which GDScript
// loads as it compiles the script and this language as the class loads.
const std::vector<std::string> &Checker::loaded_with(const std::string &p_file) {
	const auto known = loaded_files.find(p_file);
	if (known != loaded_files.end()) {
		return known->second;
	}
	std::vector<std::string> files;
	if (!is_script_file(p_file)) {
		files = options.project->dependencies(p_file);
	} else {
		if (has_extension(p_file, { ".pet" })) {
			if (const ClassDecl *header = classes.header(p_file)) {
				files.push_back(script_file(classes.base_path(*header, p_file)));
			}
		} else if (const std::optional<ForeignClass> foreign = options.project->foreign_class(p_file, false)) {
			files.push_back(foreign->base);
		}
		for (const std::string &path : classes.preloads(p_file)) {
			files.push_back(preload_file(resolved_path(path, p_file)));
		}
	}
	return loaded_files.emplace(p_file, std::move(files)).first->second;
}

// The file at a preload's path: the file a uid names; empty when there is none.
std::string Checker::preload_file(const std::string &p_path) const {
	if (p_path.rfind("uid://", 0) == 0) {
		return options.project ? options.project->uid_path(p_path) : std::string();
	}
	return p_path;
}

// The type of the resource in a file other than a script, by what it holds
// (spec §7.4): a scene is PackedScene[T], T its root's type; any other
// resource the class its file loads as, or its script's class. nullopt
// when the project has no such file.
std::optional<Type> Checker::content_type(const Expr &p_at, const std::string &p_file) {
	if (is_scene_file(p_file)) {
		const std::optional<Scene> scene = options.project ? options.project->scene(p_file) : std::nullopt;
		const SceneNode *root = scene ? scene->find_node(".") : nullptr;
		if (!root) {
			return std::nullopt;
		}
		const Type root_type = scene_node_type(p_at.range, *root, "The root of " + p_file);
		return root_type.is_error() ? root_type : Type::scene(root_type);
	}
	const std::optional<ResourceInfo> resource = options.project ? options.project->resource(p_file) : std::nullopt;
	if (!resource) {
		return std::nullopt;
	}
	return resource_type(p_at, p_file, *resource);
}

// The file that load() of one literal path names; "" for another path.
std::string Checker::literal_load_file(const Expr &p_call) const {
	if (p_call.args.size() != 1 || p_call.args[0]->kind != ExprKind::StringLiteral || p_call.args[0]->op != TokenKind::String) {
		return std::string();
	}
	const std::string &written = p_call.args[0]->text;
	if (written.find("://") != std::string::npos && written.rfind("res://", 0) != 0 && written.rfind("uid://", 0) != 0) {
		return std::string(); // Such as user://.
	}
	return preload_file(resolved_path(written, "res://"));
}

// The scene that a PackedScene[T] is statically known to be (spec §7.4):
// that of preload(), of load() of a literal path, or of a constant one of
// them initializes; nullopt for another.
std::optional<Scene> Checker::known_scene(const Expr &p_value) {
	std::string file;
	if (p_value.constant && p_value.constant->kind == ConstValue::Kind::Preload) {
		file = preload_file(p_value.constant->text);
	} else if (p_value.kind == ExprKind::Call && p_value.text == "load" && p_value.base && p_value.base->text == "ResourceLoader") {
		file = literal_load_file(p_value); // load(), checked as ResourceLoader.load().
	}
	return is_scene_file(file) && options.project && !options.declarations_only ? options.project->scene(file) : std::nullopt;
}

// load() of one literal path in the project, typed as preload() types it
// (spec §7.4), but a script as the resource it is, not its class (§2.1).
// A relative path is from res://, as the engine resolves it
// (core/io/resource_loader.cpp:693-694, ed1daf0). nullopt for any other
// path, and for a file the project does not have, which the engine's
// Resource? types.
std::optional<Type> Checker::literal_load(const Expr &p_call) {
	const std::string file = literal_load_file(p_call);
	if (file.empty()) {
		return std::nullopt;
	}
	if (is_script_file(file)) {
		return options.project && options.project->resource(file) ? std::optional<Type>(script_value_type(file)) : std::nullopt;
	}
	if (options.declarations_only) {
		return Type::of(TypeKind::Error); // Resources are not read while loading.
	}
	return content_type(p_call, file);
}

// preload() of a resource (spec §7.4): a scene is PackedScene[T], T its
// root's type; a script its class (§2.1); any other resource the class its
// file loads as, or its script's class. It is a constant, which the class
// loads as it loads, as GDScript loads it as it compiles the script
// (modules/gdscript/gdscript_analyzer.cpp:4723-4780).
Type Checker::check_preload(Expr &r_expr) {
	if (completion && r_expr.args.size() == 1 && r_expr.args[0]->kind == ExprKind::StringLiteral && at_cursor(r_expr.args[0]->text)) {
		complete_string(nullptr, 0, false);
		return set(r_expr, Type::of(TypeKind::Error));
	}
	const std::string path = preload_path(r_expr);
	if (path.empty()) {
		return fail_call(r_expr, "preload() takes one literal path, a String.");
	}
	r_expr.args[0]->text = path;
	// A constant expression (spec §4.5, §7.4).
	const auto loaded = [&](const Type &p_type) {
		auto value = std::make_shared<ConstValue>();
		value->kind = ConstValue::Kind::Preload;
		value->type = p_type;
		value->text = path;
		return value;
	};
	// A uid stays what is loaded, so that the file can move.
	const std::string file = preload_file(path);
	if (file.empty() && !options.declarations_only) {
		return fail(r_expr, "There is no resource with the uid " + path + ".");
	}
	Type type;
	Type script_class;
	if (is_script_file(file)) {
		// Read without loading, also while only the declarations are
		// checked, so that other classes see the types it names.
		script_class = preloaded_class(r_expr, file);
		if (script_class.is_error() && !options.declarations_only) {
			return set(r_expr, script_class);
		}
		type = script_value_type(file);
	} else if (options.declarations_only) {
		// Resources are not read while loading, so its type is unknown here;
		// a constant still holds it, for GDScript to read (spec §11.3).
		r_expr.constant = loaded(Type::object("Resource"));
		return set(r_expr, Type::of(TypeKind::Error));
	} else {
		const std::optional<Type> content = content_type(r_expr, file);
		if (!content) {
			return fail(r_expr, std::string(is_scene_file(file) ? "There is no scene at " : "There is no resource at ") + file + ".");
		}
		if (content->is_error()) {
			return set(r_expr, *content);
		}
		type = *content;
	}
	if (options.project && !options.declarations_only) {
		check_load_cycle(r_expr, file);
	}
	refer(r_expr.name_token, language_id("@GlobalScope", "preload"));
	check_value(*r_expr.args[0]);
	const auto value = loaded(type);
	if (is_script_file(file)) {
		value->script = file;
		value->script_class = script_class;
	}
	r_expr.constant = value;
	return set(r_expr, type);
}

// The class of a preloaded script (spec §2.1), from its header, or from
// reflection for another language; Error after reporting one that cannot
// be read, and also without reporting one of another language without a
// global name while only the declarations are checked, which load no
// script.
Type Checker::preloaded_class(const Expr &p_preload, const std::string &p_file) {
	if (is_script_path(p_file)) {
		const ClassDecl *header = script_header(p_file);
		if (!header) {
			error(p_preload.range, "There is no script of this language at " + p_file + ".");
			return Type::of(TypeKind::Error);
		}
		return script_class_type(*header, p_file);
	}
	const ForeignClass *foreign = foreign_class(p_file);
	if (!foreign || foreign->native.empty()) {
		if (!options.declarations_only) {
			error(p_preload.range, "There is no script at " + p_file + ", or it cannot be loaded.");
		}
		return Type::of(TypeKind::Error);
	}
	return foreign_class_type(p_file, *foreign);
}

// A script used as a value (spec §2.1): of the class of its resource, such
// as GDScript, as the project knows it.
Type Checker::script_value_type(const std::string &p_file) const {
	const std::optional<ResourceInfo> resource = options.project ? options.project->resource(p_file) : std::nullopt;
	if (resource && engine.class_exists(resource->type) && engine.inherits(resource->type, "Script")) {
		return Type::object(resource->type);
	}
	return Type::object("Script");
}

// The class of a preloaded resource: its script's, or the engine class its
// file loads as, as GDScript types it by the resource it loads
// (modules/gdscript/gdscript_analyzer.cpp:4757-4780).
Type Checker::resource_type(const Expr &p_preload, const std::string &p_path, const ResourceInfo &p_resource) {
	if (!p_resource.script_class.empty()) {
		TypeRef ref;
		ref.name = p_resource.script_class;
		ref.range = p_preload.range;
		ref.name_token = NO_TOKEN;
		return resolve(ref, false);
	}
	if (!engine.class_exists(p_resource.type) || !(p_resource.type == "Resource" || engine.inherits(p_resource.type, "Resource"))) {
		error(p_preload.range, "The file at " + p_path + " is not a resource the engine knows" + (p_resource.type.empty() ? std::string(".") : ": " + p_resource.type + "."));
		return Type::of(TypeKind::Error);
	}
	return Type::object(p_resource.type);
}

/* Connections and attached scripts (spec §7.7) */

// The members of this class that the scene's animation tracks and
// synchronized properties name (spec §7.7), each resolved as $X is. The
// class must have them, and the engine finds them by name, so they must be
// pub (§10.5). An engine class may take names that it does not list: those
// with a slash, as metadata/ (core/object/object.cpp:279; ed1daf0), and any
// where the class has _set() or _get(). A path that leads to no node is
// left to the engine at run time.
void Checker::check_named_members() {
	const TokenRange header{ cls.extends_token, cls.extends_token };
	const bool takes_any = function_index.count("_set") || function_index.count("_get");
	for (const Scene &scene : scenes) {
		for (const SceneNamedMember &named : scene.named_members) {
			const ViewNode *node = walk_node_path(scene, named.from, named.node).node;
			if (!node || (node->node.script.empty() ? node->node.instance_script : node->node.script) != options.path) {
				continue;
			}
			const auto shown = [](const std::string &p_path) { return p_path == "." ? std::string("the root") : p_path; };
			const std::string what = named.method ? "calls " + named.name + "() on " + shown(node->path) : (named.animation.empty() ? "synchronizes \"" : "animates \"") + named.name + "\" of " + shown(node->path);
			const std::string where = scene.path + ": " + (named.animation.empty() ? shown(named.from) : "the animation \"" + named.animation + "\" of " + shown(named.from)) + " " + what;
			if (named.method) {
				const auto found = function_index.find(named.name);
				const FuncDecl *method = found == function_index.end() ? nullptr : cls.all_functions[found->second];
				const std::optional<EngineMethod> native = method ? std::nullopt : engine.find_method(cls.native_base, named.name);
				if (method && !method->is_pub && !method->is_override) {
					error_in_scene(own_name(*method, cls.functions, header), scene, node->path, where + not_pub(true));
				} else if (!method && (!native || native->is_virtual)) {
					error_in_scene(header, scene, node->path, where + ", which this class does not declare.");
				}
				continue;
			}
			const auto found = member_index.find(named.name);
			const VarDecl *member = found == member_index.end() ? nullptr : cls.all_members[found->second];
			if (member && !member->is_pub) {
				error_in_scene(own_name(*member, cls.members, header), scene, node->path, where + not_pub(false));
			} else if (!member && !static_index.count(named.name) && !takes_any && named.name.find('/') == std::string::npos && !engine.find_property(cls.native_base, named.name)) {
				error_in_scene(header, scene, node->path, where + ", which this class does not declare.");
			}
		}
	}
}

// Connections saved in scenes to methods of this class (spec §7.7), on a
// node with it or on an instance of a scene whose root has it; either end
// may be inside a scene it instances. The method must exist and take what
// the signal passes. The engine finds the method and the signal by name,
// so both must be pub (§10.5). A method of the engine class that is not
// virtual is found so too.
void Checker::check_connections() {
	const TokenRange header{ cls.extends_token, cls.extends_token };
	for (const Scene &scene : scenes) {
		for (const SceneConnection &connection : scene.connections) {
			const ViewNode *target = view_node(scene, connection.target);
			if (!target || (target->node.script.empty() ? target->node.instance_script : target->node.script) != options.path) {
				continue;
			}
			const std::string where = scene.path + ": the signal \"" + connection.signal + "\" of " + connection.source + " is connected to " + connection.method + "()";
			const auto found = function_index.find(connection.method);
			const FuncDecl *method = found == function_index.end() ? nullptr : cls.all_functions[found->second];
			const std::optional<EngineMethod> native = method ? std::nullopt : engine.find_method(cls.native_base, connection.method);
			if (!method && (!native || native->is_virtual)) {
				error_in_scene(header, scene, connection.target, where + ", which this class does not declare.");
				continue;
			}
			const TokenRange at = method ? own_name(*method, cls.functions, header) : header;
			if (method && !method->is_pub) {
				error_in_scene(at, scene, connection.target, where + not_pub(true));
			}
			if (method && references) {
				Reference reference;
				reference.target = script_id(declaring(cls, *method, &ClassDecl::functions).path, method->name);
				reference.scene = scene.path;
				reference.connection = connection;
				references->push_back(std::move(reference));
			}
			const ViewNode *source = view_node(scene, connection.source);
			bool missing = false;
			bool hidden = false;
			const std::optional<std::vector<Type>> arguments = source ? signal_arguments(source->node, connection.signal, missing, hidden) : std::nullopt;
			if (missing) {
				error_in_scene(header, scene, connection.target, where + ", but " + connection.source + " has no such signal.");
			} else if (hidden) {
				error_in_scene(header, scene, connection.target, where + ", but that signal is not pub; the engine connects only pub signals by name.");
			}
			if (!arguments) {
				continue;
			}
			const int passed = int(arguments->size()) - connection.unbinds + connection.binds;
			const int most = int(method ? method->params.size() : native->params.size());
			const int required = method ? required_params(*method) : most - native->default_count;
			const bool rest = method ? method->has_rest() : native->is_vararg;
			if (passed < required || (!rest && passed > most)) {
				error_in_scene(at, scene, connection.target, where + ", which passes " + std::to_string(passed) + " argument(s); " + connection.method + "() takes " + argument_count(required, most, rest) + ".");
				continue;
			}
			for (int i = 0; i < int(arguments->size()) - connection.unbinds; i++) {
				std::optional<Type> param = method ? argument_type(*method, size_t(i)) : std::nullopt;
				// The rest of a vararg engine method are Variant.
				if (!method && i < most) {
					param = native->params[i].type;
				}
				if (param && param->is_storable() && !assignable((*arguments)[i], *param)) {
					const bool own = method && is_own(*method, cls.functions);
					const ParamDecl *named = own ? &method->params[std::min(size_t(i), method->params.size() - 1)] : nullptr;
					error_in_scene(named ? TokenRange{ named->name_token, named->name_token } : header, scene, connection.target, where + ", which passes " + (*arguments)[i].to_string() + " here.");
				}
			}
		}
	}
}

// Each node of the scene and each resource it holds that has the class
// set, with its name in messages and the node it marks.
void Checker::for_each_attached(const std::function<void(const std::string &, const std::string &)> &p_check) {
	for (const SceneNode &node : scenes[0].nodes) {
		if (node.script == options.path) {
			p_check(node.path == "." ? std::string("the root") : node.path, node.path);
		}
	}
	for (const SceneResource &resource : scenes[0].resources) {
		if (resource.script == options.path) {
			p_check(resource.shown(), std::string());
		}
	}
}

// No node or resource has an abstract class (spec §4.1), which the engine
// refuses to set (core/object/object.cpp:976; ed1daf0); nor one whose
// _init() has required parameters, which the engine does not pass (§6.2).
void Checker::check_attached() {
	const TokenRange header{ cls.extends_token, cls.extends_token };
	const FuncDecl *init = init_of(cls);
	const bool takes = init && required_params(*init) > 0;
	for_each_attached([&](const std::string &p_shown, const std::string &p_node) {
		const std::string has = scenes[0].path + ": " + p_shown + " has " + display_name(cls, options.path);
		if (cls.is_abstract) {
			error_in_scene(header, scenes[0], p_node, has + ", which is abstract and cannot be set on a " + (p_node.empty() ? "resource." : "node."));
		}
		if (takes) {
			error_in_scene(own_name(*init, cls.functions, header), scenes[0], p_node, has + ", whose _init() has required parameters, which the engine does not pass.");
		}
	});
}

// What a signal of a node in a scene passes: one its script of this
// language declares (spec §4.7), or one of its engine class. nullopt when
// that is not known: a script of another language, or one with errors;
// `r_missing` when the node certainly has no such signal, `r_hidden` when
// it is not pub.
std::optional<std::vector<Type>> Checker::signal_arguments(const SceneNode &p_node, const std::string &p_signal, bool &r_missing, bool &r_hidden) {
	const std::string &script = p_node.script.empty() ? p_node.instance_script : p_node.script;
	if (!script.empty() && !is_script_path(script)) {
		return std::nullopt;
	}
	if (!script.empty()) {
		const ClassDecl *decl = script_decl(script);
		if (!decl) {
			return std::nullopt;
		}
		if (const SignalDecl *signal = find_named(decl->all_signals, p_signal)) {
			r_hidden = !signal->is_pub;
			return param_types(signal->params);
		}
	}
	const auto signal = engine.find_signal(p_node.type, p_signal);
	if (!signal) {
		r_missing = engine.class_exists(p_node.type);
		return std::nullopt;
	}
	return signal_type(*signal).args;
}

/* Scene requirements and stored exports (spec §7) */

// @owner(T) and @parent(T): a class of this language, an interface or an
// engine class, on a class that extends Node (spec §7.3).
void Checker::declare_requirement(const std::optional<TypeRef> &p_ref, Type &r_type, const std::string &p_annotation) {
	if (!p_ref) {
		return;
	}
	r_type = resolve(*p_ref, false);
	if (r_type.is_error()) {
		return;
	}
	if (r_type.kind != TypeKind::Object || r_type.nullable) {
		error(p_ref->range, p_annotation + " takes a class or an interface, not " + r_type.to_string() + ".");
		r_type = Type::of(TypeKind::Error);
	} else if (!engine.inherits(cls.native_base, "Node")) {
		error(p_ref->range, p_annotation + " is for classes that extend Node.");
		r_type = Type::of(TypeKind::Error);
	}
}

// Each node of the scene whose check this is that has the class or a
// subclass: attached, or as the root of a scene it instances.
void Checker::for_each_placement(const std::function<void(const Scene &, const SceneNode &)> &p_check) {
	for (const Scene &scene : scenes) {
		for (const SceneNode &node : scene.nodes) {
			if (extends_script(node.script.empty() ? node.instance_script : node.script, options.path)) {
				p_check(scene, node);
			}
		}
	}
}

// That a node of a scene is what @owner(T) or @parent(T) requires; a class
// of another language is left to the check at run time.
void Checker::check_required(const TokenRange &p_at, const Scene &p_scene, const SceneNode &p_placed, const SceneNode &p_node, const Type &p_required, const std::string &p_what, const std::string &p_annotation) {
	const std::string &script = p_node.script.empty() ? p_node.instance_script : p_node.script;
	if (!script.empty() && !is_script_path(script) && !p_required.is_interface && !p_required.script.empty()) {
		return;
	}
	check_required_type(p_at, p_scene, p_placed, scene_node_type(p_at, p_node, p_scene.path + ": " + p_what), p_required, p_what, p_annotation);
}

void Checker::check_required_type(const TokenRange &p_at, const Scene &p_scene, const SceneNode &p_placed, const Type &p_found, const Type &p_required, const std::string &p_what, const std::string &p_annotation) {
	if (!p_found.is_error() && !assignable(p_found, p_required)) {
		error_in_scene(p_at, p_scene, p_placed.path, p_scene.path + ": " + p_what + " is " + p_found.to_string() + ", not " + p_required.to_string() + " (" + p_annotation + ").");
	}
}

// Where scenes place the script, its owner satisfies @owner(T) (spec §7.3):
// that of a node is the root of the scene that stores it, and that of a
// scene's root the root of each scene that instances it. The root of a
// scene used as a root has none.
void Checker::check_owner_scenes() {
	if (!cls.owner_ref || cls.owner_type.is_error()) {
		return;
	}
	for_each_placement([&](const Scene &p_scene, const SceneNode &p_node) {
		const ViewNode *placed = view_node(p_scene, p_node.path);
		const ViewNode *owner = placed && !placed->owner.empty() ? view_node(p_scene, placed->owner) : nullptr;
		const std::string use = placed && !owner ? classes.root_use(p_scene.path) : std::string();
		if (owner) {
			check_required(cls.owner_ref->range, p_scene, p_node, owner->node, cls.owner_type, "the owner of " + p_node.path, "@owner");
		} else if (!use.empty()) {
			const bool main = p_scene.path == options.project->main_scene();
			error_in_scene(cls.owner_ref->range, p_scene, p_node.path, p_scene.path + (main ? ": the main scene's root has no owner (@owner)." : ": the root has no owner where " + use + " (@owner)."));
		}
	});
}

// Where scenes place the script, its parent satisfies @parent(T) (spec
// §7.3): the parent in the scene, that of a scene's root in each scene that
// instances it, and the root viewport, a Window, for the main scene's root
// (scene/main/scene_tree.cpp:1762-1765, 2068; ed1daf0). The root of a scene
// used otherwise is checked at run time.
void Checker::check_parent_scenes() {
	if (!cls.parent_ref || cls.parent_type.is_error()) {
		return;
	}
	const TokenRange &at = cls.parent_ref->range;
	for_each_placement([&](const Scene &p_scene, const SceneNode &p_node) {
		const NodeWalk walk = walk_node_path(p_scene, p_node.path, "..");
		if (walk.node) {
			check_required(at, p_scene, p_node, walk.node->node, cls.parent_type, "the parent of " + p_node.path, "@parent");
		} else if (walk.leaves && p_scene.path == options.project->main_scene()) {
			check_required_type(at, p_scene, p_node, Type::object("Window"), cls.parent_type, "the parent of the main scene's root", "@parent");
		}
	});
}

// The values that the scene stores for this class's exported structs and
// enums with payloads, which the entry check decodes as the scene is
// instantiated (spec §4.3), and for its exported scenes, which may leave
// to instantiate(...) only what PackedScene[T] asks for (§7.4): on the
// nodes with the class, on the instances of scenes whose root has it, on
// the nodes with it inside instanced scenes that the scene changes, and on
// the resources with it that the scene or resource file holds.
void Checker::check_stored_exports() {
	std::vector<const VarDecl *> exports;
	for (const VarDecl &member : cls.members) {
		const TypeKind kind = member.type.unwrapped().kind;
		if (member.is_export && (kind == TypeKind::Struct || kind == TypeKind::PayloadEnum || is_typed_scene(member.type))) {
			exports.push_back(&member);
		}
	}
	const auto check = [&](const Scene &p_scene, const std::vector<std::pair<std::string, SceneValue>> &p_properties, const std::string &p_shown, const std::string &p_node) {
		for (const VarDecl *member : exports) {
			for (const auto &[name, value] : p_properties) {
				if (name != member->name) {
					continue;
				}
				const TokenRange at{ member->name_token, member->name_token };
				if (is_typed_scene(member->type)) {
					const std::optional<Scene> scene = is_scene_file(value.path) ? options.project->scene(value.path) : std::nullopt;
					const std::string unfit = scene ? unfit_scene(*scene, member->type) : std::string();
					if (!unfit.empty()) {
						error_in_scene(at, p_scene, p_node, p_scene.path + ": \"" + name + "\" of " + p_shown + " holds " + scene->path + ", which " + unfit);
					}
					continue;
				}
				const std::string why = undecodable(member->type, value, std::string());
				if (!why.empty()) {
					error_in_scene(at, p_scene, p_node, p_scene.path + ": the value stored for \"" + name + "\" of " + p_shown + " does not decode: " + why + ".");
				}
			}
		}
	};
	if (exports.empty()) {
		return;
	}
	for_each_placement([&](const Scene &p_scene, const SceneNode &p_node) {
		if (placed_class(p_node.script.empty() ? p_node.instance_script : p_node.script)) {
			check(p_scene, p_node.properties, p_node.path, p_node.path);
		}
	});
	for (const SceneNode &change : scenes[0].nodes) {
		const ViewNode *changed = change.type.empty() && !change.is_instance && change.script.empty() ? view_node(scenes[0], change.path) : nullptr;
		const std::string script = changed ? (changed->node.script.empty() ? changed->node.instance_script : changed->node.script) : std::string();
		if (extends_script(script, options.path) && placed_class(script)) {
			check(scenes[0], change.properties, change.path, change.path);
		}
	}
	for (const SceneResource &resource : scenes[0].resources) {
		if (extends_script(resource.script, options.path) && !resource.unread && placed_class(resource.script)) {
			check(scenes[0], resource.properties, resource.shown(), std::string());
		}
	}
}

// A resource has no ready phase, so the file gives each member of a
// resource with the class that must be assigned and that the _init it
// runs does not assign (spec §6.2). Only exports are stored. The game runs
// all the same: reading one without a value fails the check at run time.
void Checker::check_resource_values() {
	for (const SceneResource &resource : scenes[0].resources) {
		const ClassDecl *placed = extends_script(resource.script, options.path) && !resource.unread ? placed_class(resource.script) : nullptr;
		if (!placed) {
			continue;
		}
		for (const VarDecl &member : cls.members) {
			const bool given = std::any_of(resource.properties.begin(), resource.properties.end(), [&](const auto &p_property) { return p_property.first == member.name && p_property.second.type != "Nil"; });
			if (member.must_assign && !given && !init_assigns(*placed, member.name)) {
				const std::string why = member.is_export ? ", an export that must be assigned." : ", which must be assigned and is not exported.";
				error_in_scene(TokenRange{ member.name_token, member.name_token }, scenes[0], std::string(), scenes[0].path + ": " + resource.shown() + " has no value for \"" + member.name + "\"" + why);
				errors.back().runs = true;
			}
		}
	}
}

// Where the scene is used as a root, each node of its view with the class
// gets a value for each of its exports that must be assigned and that the
// _init it runs does not assign, from any of the scenes it is built from
// (spec §7.5); the root only in the main scene,
// since whoever instantiates another may give its root the rest (§7.4). One
// that nothing references only warns. The game runs all the same: a node
// without one fails the check as it becomes ready.
void Checker::check_scene_exports() {
	const Scene &scene = scenes[0];
	const bool main = scene.path == options.project->main_scene();
	const bool any = std::any_of(cls.members.begin(), cls.members.end(), [](const VarDecl &p_member) { return p_member.is_export && p_member.must_assign; });
	if (!any || (!main && classes.root_use(scene.path).empty())) {
		return;
	}
	for (const ViewNode &node : view_of(scene)) {
		const std::string &script = node.node.script.empty() ? node.node.instance_script : node.node.script;
		const ClassDecl *placed = (main || node.path != ".") && extends_script(script, options.path) ? placed_class(script) : nullptr;
		if (!placed) {
			continue;
		}
		for (const VarDecl &member : cls.members) {
			if (member.is_export && member.must_assign && !init_assigns(*placed, member.name) && !stored_value(scene, node.path, member.name).value_or(false)) {
				error_in_scene(TokenRange{ member.name_token, member.name_token }, scene, node.path, scene.path + ": " + (node.path == "." ? std::string("the root") : node.path) + " has no value for \"" + member.name + "\", an export that must be assigned.");
				errors.back().runs = true;
			}
		}
	}
}

// Whether the node at `p_path` of a scene has a value other than null for
// a member, as the last scene to set one gives it; nullopt if none does.
// The engine sets an instanced or inherited scene's values first, then the
// outer scene's (scene/resources/packed_scene.cpp:263-266, 494; ed1daf0).
std::optional<bool> Checker::stored_value(const Scene &p_scene, const std::string &p_path, const std::string &p_name, int p_depth) {
	const SceneNode *node = p_scene.find_node(p_path);
	for (const auto &[name, value] : node ? node->properties : std::vector<std::pair<std::string, SceneValue>>()) {
		if (name == p_name) {
			return value.type != "Nil";
		}
	}
	const std::optional<Scene> base = p_scene.base.empty() || p_depth > 32 ? std::nullopt : options.project->scene(p_scene.base);
	const std::optional<bool> inherited = base ? stored_value(*base, p_path, p_name, p_depth + 1) : std::nullopt;
	if (inherited) {
		return inherited;
	}
	// The innermost instance on the path.
	const SceneNode *instance = nullptr;
	for (const SceneNode &each : p_scene.nodes) {
		if (!each.instance.empty() && (each.path == p_path || p_path.rfind(each.path + "/", 0) == 0) && (!instance || each.path.size() > instance->path.size())) {
			instance = &each;
		}
	}
	const std::optional<Scene> inner = !instance || p_depth > 32 ? std::nullopt : options.project->scene(instance->instance);
	return inner ? stored_value(*inner, instance->path == p_path ? std::string(".") : p_path.substr(instance->path.size() + 1), p_name, p_depth + 1) : std::nullopt;
}

// Why a value that a scene stores does not decode as the type, as the
// entry check says (spec §3.6, §10.1), after `p_at`, the path to where it
// lies, such as "Combat.Hit.damage"; "" when it decodes.
std::string Checker::undecodable(const Type &p_type, const SceneValue &p_value, const std::string &p_at) {
	const std::string at = p_at.empty() ? std::string() : p_at + ": ";
	const auto mismatch = [&]() {
		const std::string found = p_value.type == "Nil" ? std::string("null") : stored_type(p_value).to_string();
		return at + "expected " + p_type.to_string() + ", found " + found;
	};
	if (p_type.kind == TypeKind::Option && p_value.type != "Nil") {
		return undecodable(p_type.args[0], p_value, p_at);
	}
	if (p_type.kind == TypeKind::Struct || p_type.kind == TypeKind::PayloadEnum) {
		return p_value.type == "Dictionary" ? undecodable_fields(p_type, p_value, p_at.empty() ? p_type.to_string() : p_at) : mismatch();
	}
	// A container by its elements, as decoding builds a new one of them.
	if (is_typed_container(p_type)) {
		const bool array = p_type.args.size() == 1;
		if (p_value.type != (array ? "Array" : "Dictionary")) {
			return mismatch();
		}
		const std::string where = p_at.empty() ? p_type.to_string() : p_at;
		for (size_t i = 0; i < p_value.items.size(); i++) {
			const bool key = !array && i % 2 == 0;
			const std::string why = undecodable(p_type.args[key || array ? 0 : 1], p_value.items[i], where + (key ? " (a key)" : "[" + std::to_string(array ? i : i / 2) + "]"));
			if (!why.empty()) {
				return why;
			}
		}
		return std::string();
	}
	// An int also from a whole float, and an enum of this language only one
	// of its values (spec §3.7, §10.8).
	const bool integer = p_value.type == "int" || (p_value.type == "float" && p_value.whole);
	if (p_type.kind == TypeKind::Enum && integer) {
		const bool closed = p_type.values && !p_type.values->is_engine;
		return closed && !p_type.values->has(p_value.integer) ? at + "expected " + p_type.to_string() + ", found " + std::to_string(p_value.integer) + ", which is none of its values" : std::string();
	}
	if (p_type.kind == TypeKind::Int && integer) {
		return std::string();
	}
	return assignable(stored_type(p_value), p_type) ? std::string() : mismatch();
}

// The fields of a stored struct, or of the variant that "$variant" names,
// each by its name as a key; a struct's field with a default may be left
// out (spec §3.6, §10.1).
std::string Checker::undecodable_fields(const Type &p_type, const SceneValue &p_value, const std::string &p_at) {
	const auto entry = [&](const std::string &p_key) -> const SceneValue * {
		for (size_t i = 0; i + 1 < p_value.items.size(); i += 2) {
			const SceneValue &key = p_value.items[i];
			if ((key.type == "String" || key.type == "StringName") && key.text == p_key) {
				return &p_value.items[i + 1];
			}
		}
		return nullptr;
	};
	std::vector<std::pair<std::string, Type>> fields;
	std::vector<bool> defaults;
	std::string at = p_at;
	if (p_type.kind == TypeKind::Struct) {
		const StructDecl *decl = struct_decl(p_type);
		for (size_t f = 0; decl && f < decl->fields.size(); f++) {
			fields.emplace_back(decl->fields[f].name, decl->fields[f].type);
			defaults.push_back(bool(decl->fields[f].initializer));
		}
	} else if (p_type.values) {
		const SceneValue *name = entry("$variant");
		const auto &variants = p_type.values->list;
		const auto named = std::find_if(variants.begin(), variants.end(), [&](const auto &p_variant) { return name && (name->type == "String" || name->type == "StringName") && name->text == p_variant.first; });
		if (named == variants.end()) {
			return p_at + ": expected " + p_type.to_string() + ", found a Dictionary whose \"$variant\" names none of its variants";
		}
		at += "." + named->first;
		fields = p_type.values->fields[named - variants.begin()];
		defaults.resize(fields.size(), false);
	}
	for (size_t f = 0; f < fields.size(); f++) {
		const SceneValue *value = entry(fields[f].first);
		const std::string why = value ? undecodable(fields[f].second, *value, at + "." + fields[f].first) : std::string();
		if (!value && !defaults[f]) {
			return at + "." + fields[f].first + " is missing";
		}
		if (!why.empty()) {
			return why;
		}
	}
	return std::string();
}

// The type of a value that a scene stores: an object of the class of its
// script, as a node's (spec §7.1, §11.1).
Type Checker::stored_type(const SceneValue &p_value) {
	if (p_value.type == "Nil") {
		return Type::of(TypeKind::None);
	}
	if (p_value.type != "Object") {
		return api_type(p_value.type, false);
	}
	if (is_script_path(p_value.script)) {
		const ClassDecl *header = script_header(p_value.script);
		return header ? script_class_type(*header, p_value.script) : Type::object(p_value.class_name);
	}
	const ForeignClass *foreign = p_value.script.empty() ? nullptr : foreign_class(p_value.script);
	return foreign && foreign->members_known && !foreign->native.empty() ? foreign_class_type(p_value.script, *foreign) : Type::object(p_value.class_name);
}

} // namespace petrichor::compiler::checking
