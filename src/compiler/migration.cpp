#include "compiler/migration.h"

#include "compiler/analysis.h"
#include "compiler/ast.h"
#include "compiler/compiler.h"
#include "compiler/lexer.h"
#include "compiler/parser.h"
#include "compiler/script_classes.h"
#include "compiler/workspace.h"

#include <algorithm>
#include <functional>
#include <iterator>
#include <tuple>

namespace petrichor::compiler {

namespace {

bool is_gdscript(const std::string &p_path) {
	return p_path.size() > 3 && p_path.compare(p_path.size() - 3, 3, ".gd") == 0;
}

// A class-level access from a GDScript file to the class of another that
// GDScript cannot make to a class of this language (spec §11.3).
struct Access {
	enum Kind {
		StaticCall,
		Type,
		Constant,
		Autoload,
	};
	std::string user;
	Kind kind = StaticCall;
	std::string name;

	// What the user does, with the target as `p_target` says.
	std::string said(const std::string &p_target) const {
		switch (kind) {
			case StaticCall:
				return "calls the static method " + name + "() of " + p_target;
			case Type:
				return "uses " + name + " of " + p_target + " as a type";
			case Constant:
				return "uses " + name + " of " + p_target + " in a constant expression";
			default:
				return "uses the autoload " + name + " of " + p_target + " as a type";
		}
	}
};

// Where GDScript reads the name at token `p_at` as a type: after `->`, `as`,
// `is` and the colon of a declaration, and between the brackets of a typed
// Array or Dictionary. `p_typed` and `p_parenthesized` tell of each token
// whether the innermost bracket around it is such or a parenthesis.
bool in_type(const std::vector<Token> &p_tokens, size_t p_at, const std::vector<bool> &p_typed, const std::vector<bool> &p_parenthesized) {
	if (p_typed[p_at]) {
		return true;
	}
	const TokenKind before = p_at > 0 ? p_tokens[p_at - 1].kind : TokenKind::EndOfFile;
	if (before == TokenKind::Arrow || before == TokenKind::As || before == TokenKind::Is) {
		return true;
	}
	if (before != TokenKind::Colon || p_at < 3 || p_tokens[p_at - 2].kind != TokenKind::Identifier) {
		return false;
	}
	const TokenKind declared = p_tokens[p_at - 3].kind;
	const bool parameter = p_parenthesized[p_at] && (declared == TokenKind::ParenOpen || declared == TokenKind::Comma);
	return parameter || declared == TokenKind::Var || declared == TokenKind::Const || declared == TokenKind::For;
}

// Whether the token at `p_at` is in the initializer of a constant declared on
// its line.
bool in_constant(const std::vector<Token> &p_tokens, size_t p_at) {
	for (size_t i = p_at; i-- > 0;) {
		const TokenKind kind = p_tokens[i].kind;
		if (kind == TokenKind::Const) {
			return true;
		}
		if (kind == TokenKind::Newline || kind == TokenKind::Indent || kind == TokenKind::Dedent || kind == TokenKind::Semicolon) {
			return false;
		}
	}
	return false;
}

// The GDScript project as the set reads it: each file's tree, by a
// representative, and the class-level accesses to each file.
struct Scripts {
	std::set<std::string> files;
	std::map<std::string, std::string> up; // Toward the representative of its tree.
	std::set<std::string> tools;
	std::map<std::string, std::vector<Access>> accessed; // By the file accessed.

	std::string tree(const std::string &p_file) {
		std::string &next = up[p_file];
		if (next.empty() || next == p_file) {
			return next = p_file;
		}
		return next = tree(next);
	}

	std::vector<std::string> members(const std::string &p_file) {
		std::vector<std::string> found;
		const std::string root = tree(p_file);
		for (const std::string &file : files) {
			if (tree(file) == root) {
				found.push_back(file);
			}
		}
		return found;
	}

	// The names that `p_file` binds to other files, with whether each is an
	// autoload's: its constants and variables initialized with a preload() or a
	// load() of one literal path, the global classes and the autoloads, a scene
	// autoload by the script of its root.
	void scan(const std::string &p_file, const SourceFile &p_source, ScriptClasses &r_classes, const std::map<std::string, std::pair<std::string, bool>> &p_global) {
		const std::vector<Token> &tokens = p_source.tokens;
		std::map<std::string, std::pair<std::string, bool>> names = p_global;
		for (size_t i = 0; i + 1 < tokens.size(); i++) {
			if (tokens[i].kind == TokenKind::At && is_word(tokens[i + 1], "tool")) {
				tools.insert(p_file);
			}
			if ((tokens[i].kind != TokenKind::Const && tokens[i].kind != TokenKind::Var) || tokens[i + 1].kind != TokenKind::Identifier) {
				continue;
			}
			size_t at = i + 2;
			at += at < tokens.size() && tokens[at].kind == TokenKind::Colon;
			if (at + 4 < tokens.size() && tokens[at].kind == TokenKind::Equal && (is_word(tokens[at + 1], "preload") || is_word(tokens[at + 1], "load")) && tokens[at + 2].kind == TokenKind::ParenOpen && tokens[at + 3].kind == TokenKind::String && tokens[at + 4].kind == TokenKind::ParenClose) {
				names[tokens[i + 1].text] = { resolved_path(tokens[at + 3].text, p_file), false };
			}
		}
		// The innermost bracket around each token.
		std::vector<bool> typed(tokens.size());
		std::vector<bool> parenthesized(tokens.size());
		std::vector<std::pair<TokenKind, bool>> open;
		for (size_t i = 0; i < tokens.size(); i++) {
			typed[i] = !open.empty() && open.back().second;
			parenthesized[i] = !open.empty() && open.back().first == TokenKind::ParenOpen;
			const TokenKind kind = tokens[i].kind;
			if (kind == TokenKind::ParenOpen || kind == TokenKind::BraceOpen) {
				open.emplace_back(kind, false);
			} else if (kind == TokenKind::BracketOpen) {
				const bool container = i > 0 && (is_word(tokens[i - 1], "Array") || is_word(tokens[i - 1], "Dictionary"));
				open.emplace_back(kind, container || typed[i]);
			} else if ((kind == TokenKind::ParenClose || kind == TokenKind::BracketClose || kind == TokenKind::BraceClose) && !open.empty()) {
				open.pop_back();
			}
		}
		for (size_t i = 0; i < tokens.size(); i++) {
			const auto named = tokens[i].kind == TokenKind::Identifier && (i == 0 || tokens[i - 1].kind != TokenKind::Period) ? names.find(tokens[i].text) : names.end();
			if (named == names.end() || !files.count(named->second.first) || named->second.first == p_file) {
				continue;
			}
			const auto &[target, autoload] = named->second;
			const bool member = i + 2 < tokens.size() && tokens[i + 1].kind == TokenKind::Period && tokens[i + 2].kind == TokenKind::Identifier;
			if (autoload && !member && in_type(tokens, i, typed, parenthesized)) {
				accessed[target].push_back({ p_file, Access::Autoload, tokens[i].text });
			}
			const ClassDecl *decl = member ? r_classes.header(target) : nullptr;
			if (!decl) {
				continue;
			}
			const std::string &name = tokens[i + 2].text;
			const bool is_static = std::any_of(decl->functions.begin(), decl->functions.end(), [&](const FuncDecl &p_function) { return p_function.name == name && p_function.is_static; });
			const bool declared = std::any_of(decl->enums.begin(), decl->enums.end(), [&](const EnumDecl &p_enum) { return p_enum.name == name; }) ||
					std::any_of(decl->constants.begin(), decl->constants.end(), [&](const ConstDecl &p_constant) { return p_constant.name == name; }) || decl->find_class(name);
			const std::string written = tokens[i].text + "." + name;
			if (is_static && !autoload) {
				accessed[target].push_back({ p_file, Access::StaticCall, name });
			} else if (declared && in_type(tokens, i, typed, parenthesized)) {
				accessed[target].push_back({ p_file, Access::Type, written });
			} else if (declared && in_constant(tokens, i)) {
				accessed[target].push_back({ p_file, Access::Constant, written });
			}
		}
	}
};

} // namespace

MigrationSet migration_set(const EngineApi &p_engine, const ProjectApi &p_project, const std::vector<std::string> &p_paths, const std::string &p_templates) {
	ScriptClasses classes(p_engine, &p_project, true);
	Scripts scripts;
	std::set<std::string> templates;
	const std::string folder = p_templates.empty() || p_templates.back() == '/' ? p_templates : p_templates + "/";
	for (const std::string &path : p_project.script_paths()) {
		if (is_gdscript(path)) {
			(!folder.empty() && path.rfind(folder, 0) == 0 ? templates : scripts.files).insert(path);
		}
	}
	std::map<std::string, std::pair<std::string, bool>> global;
	for (const auto &[name, path] : p_project.global_class_paths()) {
		global[name] = { path, false };
	}
	for (const std::string &name : p_project.autoload_names()) {
		std::string path = p_project.autoload_path(name);
		if (!is_gdscript(path)) {
			const std::optional<Scene> scene = p_project.scene(path);
			const SceneNode *root = scene ? scene->find_node(".") : nullptr;
			path = root ? root->script : std::string();
		}
		global[name] = { path, true };
	}
	for (const std::string &file : scripts.files) {
		const ClassDecl *decl = classes.header(file);
		if (!decl) {
			continue;
		}
		const auto extend = [&](const ClassDecl &p_class, const std::string &p_path, const auto &p_self) -> void {
			const std::string base = script_file(classes.base_path(p_class, p_path));
			if (scripts.files.count(base) && base != file) {
				scripts.up[scripts.tree(base)] = scripts.tree(file);
			}
			for (const std::unique_ptr<ClassDecl> &inner : p_class.classes) {
				p_self(*inner, p_path + "::" + inner->name, p_self);
			}
		};
		extend(*decl, file, extend);
		scripts.scan(file, *decl->source, classes, global);
	}

	MigrationSet result;
	std::map<std::string, std::string> taken; // Each with why it was added; "" when asked for.
	std::set<std::string> asked_templates;
	for (const std::string &path : p_paths) {
		const std::string directory = path.back() == '/' ? path : path + "/";
		const auto under = [&](const std::string &p_file) { return p_file == path || p_file.rfind(directory, 0) == 0; };
		if (std::none_of(scripts.files.begin(), scripts.files.end(), under) && std::none_of(templates.begin(), templates.end(), under)) {
			result.unknown.push_back(path);
		}
		for (const std::string &file : scripts.files) {
			if (under(file)) {
				taken[file];
			}
		}
		std::copy_if(templates.begin(), templates.end(), std::inserter(asked_templates, asked_templates.end()), under);
	}
	if (p_paths.empty()) {
		for (const std::string &file : scripts.files) {
			taken[file];
		}
		asked_templates = templates;
	}
	for (const std::string &file : asked_templates) {
		result.left_out.emplace_back(file, "a script template, from which the editor makes new scripts");
	}
	// The trees with @tool stay in GDScript, and so does what a file staying
	// there accesses at class level.
	std::map<std::string, std::string> left;
	const auto leave = [&](const std::string &p_file, const std::string &p_why, const std::string &p_tree) {
		left.emplace(p_file, p_why);
		for (const std::string &member : scripts.members(p_file)) {
			left.emplace(member, "in the inheritance tree of " + p_file + p_tree);
		}
	};
	for (const std::string &tool : scripts.tools) {
		leave(tool, "it has @tool", ", which has @tool");
	}
	for (bool more = true; more;) {
		more = false;
		for (const std::string &file : scripts.files) {
			const std::vector<Access> &accesses = scripts.accessed[file];
			const auto user = std::find_if(accesses.begin(), accesses.end(), [&](const Access &p_access) { return left.count(p_access.user) > 0; });
			if (!left.count(file) && user != accesses.end()) {
				leave(file, user->user + ", left out, " + user->said("it"), ", left out");
				more = true;
			}
		}
	}
	// What the files asked for take with them; a file left out takes only
	// its tree, which the report names with it.
	std::vector<std::string> pending;
	for (const auto &[file, why] : taken) {
		pending.push_back(file);
	}
	while (!pending.empty()) {
		const std::string file = pending.back();
		pending.pop_back();
		for (const std::string &member : scripts.members(file)) {
			if (taken.emplace(member, "in the inheritance tree of " + file).second) {
				pending.push_back(member);
			}
		}
		for (const Access &access : left.count(file) ? std::vector<Access>() : scripts.accessed[file]) {
			if (taken.emplace(access.user, access.said(file)).second) {
				pending.push_back(access.user);
			}
		}
	}
	for (const auto &[file, why] : taken) {
		if (left.count(file)) {
			result.left_out.emplace_back(file, left.at(file));
		}
	}
	std::sort(result.left_out.begin(), result.left_out.end());
	for (const auto &[file, why] : taken) {
		if (left.count(file)) {
			continue;
		}
		result.files.insert(file);
		if (!why.empty()) {
			result.added.emplace_back(file, why);
		}
	}
	return result;
}

std::string migrated_path(const std::string &p_file) {
	return p_file.substr(0, p_file.size() - 3) + ".pet";
}

std::string renamed_in_script(const std::string &p_text, const std::string &p_path, const std::map<std::string, std::string> &p_renamed, int &r_count) {
	std::vector<Diagnostic> ignored;
	const std::shared_ptr<const SourceFile> source = tokenize(p_text, ignored);
	std::string result;
	size_t done = 0;
	for (const Token &token : source->tokens) {
		// Its extension ends the literal, before the closing quote.
		const size_t extension = token.kind == TokenKind::String && p_renamed.count(resolved_path(token.text, p_path)) ? p_text.rfind(".gd", token.end()) : std::string::npos;
		if (extension == std::string::npos || extension < token.offset) {
			continue;
		}
		result += p_text.substr(done, extension - done) + ".pet";
		done = extension + 3;
		r_count++;
	}
	return result + p_text.substr(done);
}

std::string renamed_in_resource(const std::string &p_text, const std::map<std::string, std::string> &p_renamed, int &r_count) {
	std::string result;
	size_t done = 0;
	for (size_t at = p_text.find("res://"); at != std::string::npos; at = p_text.find("res://", at + 1)) {
		// After a quote that is not escaped, or after `"*` in project.godot.
		const size_t quote = at > 0 && p_text[at - 1] == '*' ? at - 1 : at;
		const bool opens = quote > 0 && p_text[quote - 1] == '"' && (quote < 2 || p_text[quote - 2] != '\\');
		const size_t end = opens ? p_text.find('"', at) : std::string::npos;
		const auto found = end == std::string::npos ? p_renamed.end() : p_renamed.find(p_text.substr(at, end - at));
		if (found == p_renamed.end()) {
			continue;
		}
		result += p_text.substr(done, at - done) + found->second;
		done = end;
		r_count++;
	}
	return result + p_text.substr(done);
}

Migrated::Migrated(const ProjectApi &p_under, const std::set<std::string> &p_files) :
		under(p_under) {
	for (const std::string &file : p_files) {
		renamed[file] = migrated_path(file);
		back[migrated_path(file)] = file;
	}
	for (const std::string &file : p_files) {
		int count = 0;
		texts[migrated_path(file)] = renamed_in_script(under.script_source(file).value_or(std::string()), migrated_path(file), renamed, count);
	}
}

std::string Migrated::path(const std::string &p_path) const {
	const auto found = renamed.find(script_file(p_path));
	return found == renamed.end() ? p_path : found->second + p_path.substr(found->first.size());
}

std::optional<Scene> Migrated::scene(const std::string &p_path) const {
	std::optional<Scene> scene = under.scene(p_path);
	if (!scene) {
		return scene;
	}
	const auto value = [&](SceneValue &r_value, const auto &p_self) -> void {
		r_value.script = path(r_value.script);
		r_value.path = path(r_value.path);
		for (std::vector<SceneValue> *values : { &r_value.items, &r_value.typed }) {
			for (SceneValue &each : *values) {
				p_self(each, p_self);
			}
		}
	};
	for (SceneNode &node : scene->nodes) {
		node.script = path(node.script);
		node.instance_script = path(node.instance_script);
		for (auto &[name, stored] : node.properties) {
			value(stored, value);
		}
	}
	for (SceneResource &resource : scene->resources) {
		resource.script = path(resource.script);
		for (auto &[name, stored] : resource.properties) {
			value(stored, value);
		}
	}
	return scene;
}

std::optional<ResourceInfo> Migrated::resource(const std::string &p_path) const {
	if (renamed.count(p_path)) {
		return std::nullopt;
	}
	const auto found = back.find(p_path);
	return under.resource(found == back.end() ? p_path : found->second);
}

std::vector<std::string> Migrated::dependencies(const std::string &p_path) const {
	std::vector<std::string> paths = under.dependencies(p_path);
	for (std::string &each : paths) {
		each = path(each);
	}
	return paths;
}

std::map<std::string, std::string> Migrated::global_class_paths() const {
	std::map<std::string, std::string> paths = under.global_class_paths();
	for (auto &[name, each] : paths) {
		each = path(each);
	}
	return paths;
}

std::vector<std::string> Migrated::resource_files() const {
	std::vector<std::string> paths = under.resource_files();
	for (std::string &each : paths) {
		each = path(each);
	}
	return paths;
}

std::vector<std::string> Migrated::script_paths() const {
	std::vector<std::string> paths = under.script_paths();
	for (std::string &each : paths) {
		each = path(each);
	}
	return paths;
}

std::optional<std::string> Migrated::script_source(const std::string &p_path) const {
	const auto text = texts.find(p_path);
	if (text != texts.end()) {
		return text->second;
	}
	if (renamed.count(p_path)) {
		return std::nullopt;
	}
	const auto other = others.find(p_path);
	if (other != others.end()) {
		return other->second;
	}
	const std::optional<std::string> source = under.script_source(p_path);
	int count = 0;
	return source ? std::optional<std::string>(others[p_path] = renamed_in_script(*source, p_path, renamed, count)) : source;
}

std::optional<ForeignClass> Migrated::foreign_class(const std::string &p_path, bool p_members) const {
	if (renamed.count(p_path) || back.count(p_path)) {
		return std::nullopt;
	}
	std::optional<ForeignClass> foreign = under.foreign_class(p_path, p_members);
	if (foreign) {
		foreign->base = path(foreign->base);
	}
	return foreign;
}

int Inflows::slot(const Slot &p_slot) {
	const auto [at, added] = found.emplace(p_slot.key, int(slots.size()));
	if (added) {
		slots.push_back(p_slot);
	}
	return at->second;
}

Type Inflows::type(int p_slot) const {
	const auto at = tried.find(slots[size_t(p_slot)].key);
	return at == tried.end() ? Type::of(TypeKind::Error) : at->second;
}

namespace {

// The fix of the error that the loop of fixes applies: the first of the
// automatic kinds, Async or Unwrap, unless it edits a file outside those
// migrated.
const Fix *loop_fix(const Diagnostic &p_error, const Migrated &p_project) {
	const auto fix = std::find_if(p_error.fixes.begin(), p_error.fixes.end(), [](const Fix &p_fix) { return p_fix.automatic() || p_fix.kind == FixKind::Async || p_fix.kind == FixKind::Unwrap; });
	const bool inside = fix != p_error.fixes.end() && std::all_of(fix->edits.begin(), fix->edits.end(), [&](const Fix::Edit &p_edit) { return p_project.texts.count(p_edit.file.empty() ? p_error.file : p_edit.file) > 0; });
	return inside ? &*fix : nullptr;
}

// Each script of this language in the project, and each scene, checked once
// with the types that `r_inflows` tries, recording afresh what flows into
// the slots; the results of the scripts by path, with the errors that the
// checks of the scenes report in them, each error naming its file.
std::map<std::string, CompileResult> checked_with(const EngineApi &p_engine, const ProjectApi &p_project, Inflows &r_inflows) {
	r_inflows.flows.clear();
	r_inflows.calls.clear();
	r_inflows.unfit.clear();
	r_inflows.unknown.clear();
	Workspace workspace(p_project);
	Analysis analysis(p_engine, workspace);
	analysis.inflows = &r_inflows;
	const Analysis::Pass pass(analysis, &p_project);
	std::map<std::string, CompileResult> results;
	for (const std::string &path : p_project.script_paths()) {
		const std::optional<std::string> source = is_script_path(path) ? p_project.script_source(path) : std::nullopt;
		if (source) {
			results[path] = *analysis.compile(path, *source);
			for (Diagnostic &error : results[path].errors) {
				error.file = path;
			}
		}
	}
	for (const std::string &path : p_project.scene_paths()) {
		for (const Diagnostic &error : analysis.check_scene(path)->errors) {
			results[error.file].errors.push_back(error);
		}
	}
	return results;
}

// A value that flowed into a slot: its type, where it is, and whether a call
// by name passed it.
struct Received {
	Type type;
	Inflows::Key at;
	bool by_name = false;
	Inflows::Part part = Inflows::Part::Whole;
};

// What each slot received in the check, once from each place, by the slot's
// index. A call by name passes its arguments to each function so named
// that takes as many (architecture §6.6).
std::vector<std::vector<Received>> received(const Inflows &p_inflows) {
	std::vector<std::map<Inflows::Key, Received>> found(p_inflows.slots.size());
	// A place checked twice, as an initializer is with the declarations for
	// another file, may be known only once.
	const auto add = [&](int p_slot, const Received &p_received) {
		const auto [at, added] = found[size_t(p_slot)].emplace(p_received.at, p_received);
		if (!added && at->second.type.is_error()) {
			at->second = p_received;
		}
	};
	std::multimap<std::string, int> parameters;
	for (size_t i = 0; i < p_inflows.slots.size(); i++) {
		if (p_inflows.slots[i].kind == Inflows::Kind::Parameter) {
			parameters.emplace(p_inflows.slots[i].function, int(i));
		}
	}
	for (const Inflows::Flow &flow : p_inflows.flows) {
		add(flow.slot, { flow.type, flow.at, false, flow.part });
	}
	for (const Inflows::Call &call : p_inflows.calls) {
		const int count = int(call.args.size());
		const auto [first, last] = parameters.equal_range(call.name);
		for (auto each = first; each != last; ++each) {
			const Inflows::Slot &slot = p_inflows.slots[size_t(each->second)];
			if (slot.least <= count && count <= slot.most && slot.index < count) {
				add(each->second, { call.args[size_t(slot.index)], call.at[size_t(slot.index)], true });
			}
		}
	}
	std::vector<std::vector<Received>> result(found.size());
	for (size_t i = 0; i < found.size(); i++) {
		for (const auto &[at, each] : found[i]) {
			result[i].push_back(each);
		}
	}
	return result;
}

std::string file_name(const std::string &p_path) {
	return p_path.substr(p_path.find_last_of('/') + 1);
}

// The type of the objects of the class of this language at `p_path`, named
// as the checker names it.
Type class_type(const std::string &p_path, ScriptClasses &r_classes) {
	const ClassDecl *file = r_classes.header(script_file(p_path));
	std::string name = file && !file->class_name.empty() ? file->class_name : file_name(script_file(p_path));
	for (size_t at = p_path.find("::"); at != std::string::npos;) {
		const size_t next = p_path.find("::", at + 2);
		name += "." + p_path.substr(at + 2, next == std::string::npos ? next : next - at - 2);
		at = next;
	}
	const ClassDecl *decl = r_classes.header(p_path);
	return Type::script_class(name, p_path, decl ? r_classes.native_base(*decl, p_path) : std::string("Object"));
}

// The classes that an object of the class is, its own first, up through the
// engine's.
std::vector<Type> lineage(const Type &p_class, ScriptClasses &r_classes, const EngineApi &p_engine) {
	std::vector<Type> up = { p_class };
	std::set<std::string> seen;
	for (std::string path = p_class.script; is_script_path(path) && seen.insert(path).second;) {
		const ClassDecl *header = r_classes.header(path);
		path = header ? r_classes.base_path(*header, path) : std::string();
		if (is_script_path(path)) {
			up.push_back(class_type(path, r_classes));
		}
	}
	for (std::string native = p_class.engine_class(); !native.empty(); native = p_engine.parent_class(native)) {
		if (native != p_class.name || !p_class.script.empty()) {
			up.push_back(Type::object(native));
		}
	}
	return up;
}

// What the types received merge into (architecture §6.6): the one type, as
// T? with None; the nearest common base of classes; an untyped Array or
// Dictionary for containers of one kind; otherwise Variant. Nullopt when no
// type is known. `r_numbers` tells int and float, which merge into float
// only with `p_floats`.
std::optional<Type> merged(const std::vector<Received> &p_received, ScriptClasses &r_classes, const EngineApi &p_engine, bool &r_numbers, bool p_floats = false) {
	r_numbers = false;
	bool none = false;
	std::vector<Type> types;
	for (const Received &each : p_received) {
		Type type = each.type;
		if (type.is_error() || type.kind == TypeKind::Void) {
			continue;
		}
		if (type.is_optional() || type.kind == TypeKind::None) {
			none = true;
			if (type.kind == TypeKind::None) {
				continue;
			}
			type = type.unwrapped();
		}
		if (type.kind == TypeKind::Signal) {
			type = Type::builtin("Signal");
		}
		if (std::find(types.begin(), types.end(), type) == types.end()) {
			types.push_back(type);
		}
	}
	if (types.empty()) {
		return none ? std::optional<Type>(Type::of(TypeKind::Variant)) : std::nullopt;
	}
	const auto all = [&](const std::function<bool(const Type &)> &p_is) { return std::all_of(types.begin(), types.end(), p_is); };
	const auto key = [](const Type &p_class) { return p_class.script.empty() ? p_class.name : p_class.script; };
	Type result = Type::of(TypeKind::Variant);
	if (types.size() == 1) {
		result = types[0];
	} else if (all([](const Type &p_type) { return p_type.kind == TypeKind::Object && !p_type.is_interface && p_type.args.empty(); })) {
		std::vector<Type> common = lineage(types[0], r_classes, p_engine);
		for (size_t i = 1; i < types.size(); i++) {
			std::set<std::string> keys;
			for (const Type &each : lineage(types[i], r_classes, p_engine)) {
				keys.insert(key(each));
			}
			common.erase(std::remove_if(common.begin(), common.end(), [&](const Type &p_each) { return !keys.count(key(p_each)); }), common.end());
		}
		result = common.empty() ? result : common.front();
	} else if (all([](const Type &p_type) { return p_type.kind == TypeKind::Builtin && p_type.name == "Array"; })) {
		result = Type::builtin("Array");
	} else if (all([](const Type &p_type) { return p_type.kind == TypeKind::Builtin && p_type.name == "Dictionary"; })) {
		result = Type::builtin("Dictionary");
	} else {
		r_numbers = types.size() == 2 && all([](const Type &p_type) { return p_type.is_numeric(); });
		result = r_numbers && p_floats ? Type::of(TypeKind::Float) : result;
	}
	return none && result.kind != TypeKind::Variant ? optional_of(result) : result;
}

// What flows into an empty container of the kind merges into (architecture
// §6.6): Array[T] or Dictionary[K, V] when its elements, and its keys,
// merge to a type; otherwise the untyped container. A container that flows
// in gives its elements and keys. Nullopt when no type is known.
std::optional<Type> merged_container(const std::string &p_kind, const std::vector<Received> &p_received, ScriptClasses &r_classes, const EngineApi &p_engine, bool &r_numbers) {
	std::vector<Received> elements;
	std::vector<Received> keys;
	bool known = false;
	bool untyped = false;
	for (const Received &each : p_received) {
		known = known || !each.type.is_error();
		if (each.part != Inflows::Part::Whole) {
			(each.part == Inflows::Part::Key ? keys : elements).push_back(each);
		} else if (each.type.kind == TypeKind::Builtin && each.type.name == p_kind && !each.type.args.empty()) {
			elements.push_back({ each.type.args.back(), each.at });
			keys.push_back({ each.type.args.front(), each.at });
		} else {
			untyped = untyped || !each.type.is_error();
		}
	}
	if (!known) {
		return std::nullopt;
	}
	bool numbers[2] = {};
	const std::optional<Type> element = merged(elements, r_classes, p_engine, numbers[0]);
	const std::optional<Type> key = p_kind == "Dictionary" ? merged(keys, r_classes, p_engine, numbers[1]) : Type::of(TypeKind::Int);
	r_numbers = numbers[0] || numbers[1];
	const auto typed = [](const std::optional<Type> &p_type) { return p_type && p_type->kind != TypeKind::Variant; };
	if (untyped || !typed(element) || !typed(key)) {
		return Type::builtin(p_kind);
	}
	return p_kind == "Array" ? Type::array(*element) : Type::dictionary(*key, *element);
}

// How the file writes the type; empty when it cannot, as a class without a
// name, which the checker calls by its file. The file's own enums and inner
// classes go without its name.
std::string spelled(const Type &p_type, const std::string &p_file) {
	std::string text = p_type.to_string();
	const std::string own = file_name(p_file) + ".";
	for (size_t at = text.find(own); at != std::string::npos; at = text.find(own, at)) {
		text.erase(at, own.size());
	}
	const bool unnamed = text.find(".pet") != std::string::npos || text.find(".gd") != std::string::npos || text.find(".cs") != std::string::npos;
	return unnamed || text.find('<') != std::string::npos || p_type.kind == TypeKind::Unsupported ? std::string() : text;
}

// Where the type of each slot of a file is written: after its name, over
// the colon of `:=`, or after the parameters of the function whose return it
// is; with the ranges of that function's bare returns, which report a
// return type that does not fit.
struct Place {
	uint32_t at = 0;
	uint32_t end = 0;
	std::string before = ": ";
	std::string after;
	std::vector<std::pair<uint32_t, uint32_t>> bare;
};

// Each statement of the block, and of the blocks in it.
void each_statement(const Block &p_block, const std::function<void(const Stmt &)> &p_visit) {
	for (const std::unique_ptr<Stmt> &stmt : p_block) {
		p_visit(*stmt);
		each_statement(stmt->body, p_visit);
		each_statement(stmt->else_body, p_visit);
		for (const MatchBranch &branch : stmt->branches) {
			each_statement(branch.body, p_visit);
		}
	}
}

// Each statement of the functions of the class and of its inner classes.
void each_statement(const ClassDecl &p_class, const std::function<void(const Stmt &)> &p_visit) {
	for (const FuncDecl &function : p_class.functions) {
		each_statement(function.body, p_visit);
	}
	for (const std::unique_ptr<ClassDecl> &inner : p_class.classes) {
		each_statement(*inner, p_visit);
	}
}

std::map<Inflows::Key, Place> places(const std::string &p_file, const std::string &p_text, const Inflows &p_inflows) {
	std::map<Inflows::Key, Place> found;
	std::vector<Diagnostic> ignored;
	const ClassDecl decl = parse(p_text, ignored);
	const std::vector<Token> &tokens = decl.source->tokens;
	for (const Inflows::Slot &slot : p_inflows.slots) {
		if (slot.key.first != p_file || slot.kind == Inflows::Kind::Return) {
			continue;
		}
		const auto name = std::lower_bound(tokens.begin(), tokens.end(), slot.key.second, [](const Token &p_token, uint32_t p_offset) { return p_token.offset < p_offset; });
		Place &place = found[slot.key];
		place.at = place.end = name->end();
		if (name + 2 < tokens.end() && name[1].kind == TokenKind::Colon && name[2].kind == TokenKind::Equal) {
			place.end = name[1].end();
			place.after = " ";
		}
	}
	const auto functions = [&](const ClassDecl &p_class, const auto &p_self) -> void {
		for (const FuncDecl &function : p_class.functions) {
			const Inflows::Key key = { p_file, tokens[function.name_token].offset };
			if (!p_inflows.found.count(key)) {
				continue;
			}
			Place &place = found[key];
			place.before = " -> ";
			int depth = 0;
			for (size_t i = function.name_token + 1; i < tokens.size(); i++) {
				if (tokens[i].kind == TokenKind::ParenOpen) {
					depth++;
				} else if (tokens[i].kind == TokenKind::ParenClose && --depth == 0) {
					place.at = place.end = tokens[i].end();
					break;
				}
			}
			each_statement(function.body, [&](const Stmt &p_stmt) {
				if (p_stmt.kind == StmtKind::Return && !p_stmt.value) {
					place.bare.emplace_back(decl.source->begin(p_stmt.range), decl.source->end(p_stmt.range));
				}
			});
		}
		for (const std::unique_ptr<ClassDecl> &inner : p_class.classes) {
			p_self(*inner, p_self);
		}
	};
	functions(decl, functions);
	return found;
}

// The reads of locals and members that a check found, each by where its name
// is, to tell whether an int joins a float (architecture §6.6).
struct Uses {
	std::set<Inflows::Key> kept; // Of those that give for an int what the same float gives.
	std::set<Inflows::Key> written; // As the targets of assignments with =.
	std::map<Inflows::Key, Inflows::Key> initializes; // The whole initializer of a local without a type, to it.
	std::map<Inflows::Key, std::vector<Inflows::Key>> of_local;
	std::map<std::pair<std::string, std::string>, std::vector<Inflows::Key>> of_member; // By class and name.
	std::set<std::string> outside; // The names read after a dot in the GDScript files left out, which no check sees.
};

Uses uses_of(const std::map<std::string, CompileResult> &p_results, const Inflows &p_inflows, const ProjectApi &p_project) {
	Uses found;
	// A read that flows into a slot whose type the check tried keeps nothing.
	std::set<std::tuple<std::string, uint32_t, uint32_t>> flowed;
	for (const Inflows::Flow &flow : p_inflows.flows) {
		flowed.emplace(flow.at.first, flow.at.second, flow.last);
	}
	for (const auto &[name, begin] : p_inflows.kept) {
		if (!flowed.count({ name.first, begin, name.second })) {
			found.kept.insert(name);
		}
	}
	for (const auto &[path, result] : p_results) {
		std::vector<Diagnostic> ignored;
		const ClassDecl decl = parse(p_project.script_source(path).value_or(std::string()), ignored);
		const auto at = [&](uint32_t p_token) { return Inflows::Key(path, decl.source->tokens[p_token].offset); };
		each_statement(decl, [&](const Stmt &p_stmt) {
			const Expr *read = p_stmt.kind == StmtKind::Assign ? p_stmt.target.get() : p_stmt.value.get();
			if (!read || (read->kind != ExprKind::Name && read->kind != ExprKind::Member)) {
				return;
			}
			if (p_stmt.kind == StmtKind::Assign && p_stmt.assign_op == TokenKind::Equal) {
				found.written.insert(at(read->name_token));
			} else if (p_stmt.kind == StmtKind::Var && !p_stmt.declared_type) {
				found.initializes[at(read->name_token)] = at(p_stmt.name_token);
			}
		});
		for (const Reference &reference : result.references) {
			const DeclId &target = reference.target;
			if (reference.declares || !reference.scene.empty()) {
				continue;
			}
			if (target.kind == DeclId::Kind::Local && !target.name.empty()) {
				const Inflows::Key local = { script_file(target.scope), uint32_t(std::stoul(target.name)) };
				const auto base = p_inflows.shared.find(local);
				found.of_local[base == p_inflows.shared.end() ? local : base->second].emplace_back(path, reference.begin);
			} else if (target.kind == DeclId::Kind::Script) {
				found.of_member[{ target.scope, target.name }].emplace_back(path, reference.begin);
			}
		}
	}
	for (const std::string &path : p_project.script_paths()) {
		if (!is_gdscript(path)) {
			continue;
		}
		std::vector<Diagnostic> ignored;
		const std::shared_ptr<const SourceFile> source = tokenize(p_project.script_source(path).value_or(std::string()), ignored);
		for (size_t i = 1; i < source->tokens.size(); i++) {
			if (source->tokens[i - 1].kind == TokenKind::Period && source->tokens[i].kind == TokenKind::Identifier) {
				found.outside.insert(source->tokens[i].text);
			}
		}
	}
	return found;
}

// Whether an int joins a float in the slot (architecture §6.6): each read of
// it gives for an int what the same float gives, or initializes a local whose
// reads do. Those of a parameter or a local are in its function; those of a
// member, in every file, so none joins that a GDScript file left out reads.
bool joins(const Inflows::Slot &p_slot, Uses &r_uses) {
	const bool followed = p_slot.kind == Inflows::Kind::Parameter || p_slot.kind == Inflows::Kind::Local || (p_slot.kind == Inflows::Kind::Member && !r_uses.outside.count(p_slot.name));
	if (!followed || !p_slot.container.empty()) {
		return false;
	}
	std::set<Inflows::Key> seen;
	const auto kept = [&](const std::vector<Inflows::Key> &p_reads, const auto &p_self) -> bool {
		return std::all_of(p_reads.begin(), p_reads.end(), [&](const Inflows::Key &p_read) {
			const auto local = r_uses.initializes.find(p_read);
			if (r_uses.kept.count(p_read) || r_uses.written.count(p_read)) {
				return true;
			}
			return local != r_uses.initializes.end() && (!seen.insert(local->second).second || p_self(r_uses.of_local[local->second], p_self));
		});
	};
	return kept(p_slot.kind == Inflows::Kind::Member ? r_uses.of_member[{ p_slot.owner, p_slot.name }] : r_uses.of_local[p_slot.key], kept);
}

} // namespace

// Stage 2 of a migration (architecture §6.6): rounds of checks that record
// what flows into each slot without a type, trying for each what it
// received, merged, until the types tried stay; then for those still
// without one, Variant. The types are written into the texts, and those
// whose written form the check then refuses are withdrawn.
static void infer(const EngineApi &p_engine, Migrated &r_project, int p_rounds, Migration &r_result) {
	Inflows inflows;
	for (const auto &[file, text] : r_project.texts) {
		inflows.files.insert(file);
	}
	ScriptClasses classes(p_engine, &r_project);
	// What the check reports already, which no type causes.
	std::set<std::tuple<std::string, uint32_t, std::string>> reported;
	std::map<std::string, CompileResult> results = checked_with(p_engine, r_project, inflows);
	for (const auto &[path, result] : results) {
		for (const Diagnostic &error : result.errors) {
			reported.emplace(path, error.begin, error.message);
		}
	}
	// What a slot's types merge into, and the type it tries without any.
	const auto merge = [&](const Inflows::Slot &p_slot, const std::vector<Received> &p_received, bool &r_numbers) {
		if (p_slot.container.empty()) {
			return std::make_pair(merged(p_received, classes, p_engine, r_numbers), Type::of(TypeKind::Variant));
		}
		return std::make_pair(merged_container(p_slot.container, p_received, classes, p_engine, r_numbers), Type::builtin(p_slot.container));
	};
	const auto candidates = [&](bool p_unknown) {
		for (const Inflows::Slot &each : inflows.unknown) {
			inflows.slot(each);
		}
		std::map<Inflows::Key, Type> next;
		const std::vector<std::vector<Received>> flowed = received(inflows);
		for (size_t i = 0; i < flowed.size(); i++) {
			bool numbers = false;
			const auto [type, otherwise] = merge(inflows.slots[i], flowed[i], numbers);
			if (type || p_unknown) {
				next[inflows.slots[i].key] = type.value_or(otherwise);
			}
		}
		return next;
	};
	// The slots with values of known types first, then with Variant for the
	// others, each part in at most `p_rounds` rounds. The members that a round
	// found read elsewhere without their types are slots from the next one.
	for (const bool unknown : { false, true }) {
		for (int round = 0; round < p_rounds; round++) {
			std::map<Inflows::Key, Type> next = candidates(unknown);
			if (next == inflows.tried && inflows.unknown.empty()) {
				break;
			}
			inflows.tried = std::move(next);
			results = checked_with(p_engine, r_project, inflows);
		}
	}
	const std::vector<std::vector<Received>> flowed = received(inflows);
	const std::vector<Inflows::Slot> slots = inflows.slots;
	std::map<Inflows::Key, Type> tried = inflows.tried;
	// The slots that received int and float tried Variant, so the reads beside
	// a float that the last check found are beside floats in GDScript too.
	std::optional<Uses> uses;
	for (size_t i = 0; i < slots.size(); i++) {
		bool numbers = false;
		merge(slots[i], flowed[i], numbers);
		if (numbers && !uses) {
			uses = uses_of(results, inflows, r_project);
		}
		if (numbers && joins(slots[i], *uses)) {
			tried[slots[i].key] = *merged(flowed[i], classes, p_engine, numbers, true);
		} else {
			r_result.numbers += numbers;
		}
	}

	// A place in a file as the report names it, with its line.
	const std::map<std::string, std::string> original = r_project.texts;
	std::map<std::string, std::pair<std::string, std::vector<uint32_t>>> lines;
	const auto where = [&](const Inflows::Key &p_at) {
		if (!is_script_path(p_at.first)) {
			return p_at.first; // A scene's connection.
		}
		auto found = lines.find(p_at.first);
		if (found == lines.end()) {
			const auto text = original.find(p_at.first);
			std::string read = text != original.end() ? text->second : r_project.script_source(p_at.first).value_or(std::string());
			std::vector<uint32_t> starts = line_starts_of(read);
			found = lines.emplace(p_at.first, std::make_pair(std::move(read), std::move(starts))).first;
		}
		return p_at.first + ":" + std::to_string(position_in(found->second.first, found->second.second, p_at.second).line);
	};
	std::map<Inflows::Key, Place> place;
	for (const auto &[file, text] : original) {
		const std::map<Inflows::Key, Place> in_file = places(file, text, inflows);
		place.insert(in_file.begin(), in_file.end());
	}
	// The slots each reference to a declaration may name.
	std::multimap<std::pair<std::string, std::string>, Inflows::Key> named;
	for (const Inflows::Slot &slot : slots) {
		if (slot.kind == Inflows::Kind::Member || slot.kind == Inflows::Kind::Return) {
			named.emplace(std::make_pair(slot.owner, slot.name), slot.key);
		} else if (slot.kind == Inflows::Kind::Signal) {
			named.emplace(std::make_pair(slot.owner, slot.function), slot.key);
		}
	}

	// The types written, by file, in order: what each replaces in the
	// original text, and the slot.
	struct Written {
		uint32_t at;
		uint32_t end;
		std::string text;
		Inflows::Key slot;
	};
	std::map<std::string, std::vector<Written>> insertions;
	// A hint at the slot, in the text written.
	const auto note = [&](const Inflows::Key &p_slot, const std::string &p_message) {
		uint32_t at = p_slot.second;
		for (const Written &each : insertions[p_slot.first]) {
			at += each.at < p_slot.second ? uint32_t(each.text.size()) - (each.end - each.at) : 0;
		}
		const std::string &text = r_project.texts.at(p_slot.first);
		const SourcePosition position = position_in(text, line_starts_of(text), at);
		Diagnostic hint{ at, at, p_message };
		hint.severity = Severity::Hint;
		hint.file = p_slot.first;
		hint.line = position.line;
		hint.column = position.column;
		r_result.notes.push_back(std::move(hint));
	};
	std::map<Inflows::Key, std::string> written;
	std::map<Inflows::Key, std::string> refusals;
	for (const Inflows::Slot &slot : slots) {
		const auto found = tried.find(slot.key);
		const std::string text = found == tried.end() ? std::string() : spelled(found->second, slot.key.first);
		if (!text.empty() && place.count(slot.key)) {
			written[slot.key] = text;
		} else if (found != tried.end()) {
			refusals[slot.key] = found->second.to_string() + " has no name here";
		} else {
			refusals[slot.key] = "no type was tried for it in " + std::to_string(p_rounds) + " rounds";
		}
	}
	Inflows check;
	check.files = inflows.files;
	for (int pass = 0;; pass++) {
		insertions.clear();
		for (const auto &[slot, text] : written) {
			const Place &at = place.at(slot);
			insertions[slot.first].push_back({ at.at, at.end, at.before + text + at.after, slot });
		}
		for (auto &[file, text] : r_project.texts) {
			std::vector<Written> &in_file = insertions[file];
			std::sort(in_file.begin(), in_file.end(), [](const Written &p_a, const Written &p_b) { return p_a.at < p_b.at; });
			text = original.at(file);
			for (auto each = in_file.rbegin(); each != in_file.rend(); ++each) {
				text.replace(each->at, each->end - each->at, each->text);
			}
		}
		if (pass == p_rounds) {
			break;
		}
		// Where a byte of the text written was in the original, and the slot
		// whose type it is part of, if any.
		const auto original_at = [&](const std::string &p_file, uint32_t p_offset) -> std::pair<uint32_t, std::optional<Inflows::Key>> {
			int64_t shift = 0;
			for (const Written &each : insertions[p_file]) {
				if (p_offset < each.at + shift) {
					break;
				}
				if (p_offset < each.at + shift + int64_t(each.text.size())) {
					return { each.at, each.slot };
				}
				shift += int64_t(each.text.size()) - (each.end - each.at);
			}
			return { uint32_t(p_offset - shift), std::nullopt };
		};
		const std::map<std::string, CompileResult> checked = checked_with(p_engine, r_project, check);
		// The locals typed by a value, by where each is declared in the text
		// written, with where the value is.
		using Range = Inflows::Range;
		std::map<Inflows::Key, Range> typed_by;
		for (const auto &[path, result] : checked) {
			std::vector<Diagnostic> ignored;
			const ClassDecl decl = parse(r_project.script_source(path).value_or(std::string()), ignored);
			const SourceFile &source = *decl.source;
			each_statement(decl, [&](const Stmt &p_stmt) {
				if ((p_stmt.kind == StmtKind::Var || p_stmt.kind == StmtKind::For) && !p_stmt.declared_type && p_stmt.value) {
					typed_by[{ path, source.tokens[p_stmt.name_token].offset }] = Range(source.begin(p_stmt.value->range), source.end(p_stmt.value->range));
				}
			});
		}
		// The slots whose types an error between the offsets is about: those
		// it names, and those that the values of the locals it names do.
		const auto involved = [&](const CompileResult &p_result, uint32_t p_begin, uint32_t p_end, std::set<Inflows::Key> &r_seen, std::set<Inflows::Key> &r_slots, const auto &p_self) -> void {
			for (const Reference &reference : p_result.references) {
				const DeclId &target = reference.target;
				if (reference.begin < p_begin || reference.end > p_end) {
					continue;
				}
				if (target.kind == DeclId::Kind::Local && !target.name.empty()) {
					const Inflows::Key at = { script_file(target.scope), uint32_t(std::stoul(target.name)) };
					Inflows::Key local = { at.first, original_at(at.first, at.second).first };
					const auto base = inflows.shared.find(local);
					local = base == inflows.shared.end() ? local : base->second;
					const auto value = typed_by.find(at);
					if (written.count(local)) {
						r_slots.insert(local);
					} else if (value != typed_by.end() && r_seen.insert(at).second) {
						p_self(p_result, value->second.first, value->second.second, r_seen, r_slots, p_self);
					}
				}
				const auto [first, last] = named.equal_range({ target.scope, target.name });
				for (auto each = first; target.kind == DeclId::Kind::Script && each != last; ++each) {
					r_slots.insert(each->second);
				}
			}
		};
		std::set<Inflows::Key> refused;
		for (const auto &[path, result] : checked) {
			for (const Diagnostic &error : result.errors) {
				const auto [begin, inside] = original_at(path, error.begin);
				if (loop_fix(error, r_project) || reported.count({ path, begin, error.message })) {
					continue;
				}
				if (inside) {
					refused.insert(*inside);
					continue;
				}
				std::set<Inflows::Key> seen;
				involved(result, error.begin, error.end, seen, refused, involved);
				// An assigned value that does not fit is about its target too.
				for (const Inflows::Unfit &unfit : check.unfit) {
					if (unfit.file == path && unfit.value == Range(error.begin, error.end)) {
						involved(result, unfit.target.first, unfit.target.second, seen, refused, involved);
					}
				}
				for (const auto &[slot, at] : place) {
					const bool bare = slot.first == path && std::any_of(at.bare.begin(), at.bare.end(), [&](const std::pair<uint32_t, uint32_t> &p_return) { return p_return.first <= begin && begin < p_return.second; });
					if (bare && written.count(slot)) {
						refused.insert(slot);
					}
				}
			}
		}
		bool withdrew = false;
		for (const Inflows::Key &slot : refused) {
			if (written.erase(slot) > 0) {
				refusals[slot] = tried.at(slot).to_string() + " does not check";
				withdrew = true;
			}
		}
		if (!withdrew) {
			break;
		}
	}
	for (const Inflows::Slot &slot : slots) {
		const size_t index = size_t(inflows.found.at(slot.key));
		std::string from;
		std::string by_name;
		for (const Received &each : flowed[index]) {
			const char *const PARTS[] = { "", "an element ", "a key " };
			if (!each.type.is_error()) {
				from += std::string(from.empty() ? "" : ", ") + PARTS[size_t(each.part)] + each.type.to_string() + " (" + where(each.at) + ")";
			}
			if (each.by_name && !each.type.is_error()) {
				by_name += (by_name.empty() ? "" : ", ") + where(each.at);
			}
		}
		const auto refusal = refusals.find(slot.key);
		if (refusal != refusals.end()) {
			note(slot.key, "\"" + slot.name + "\" is left without a type: " + refusal->second + "." + (from.empty() ? " Nothing flows into it." : " It receives " + from + "."));
			r_result.withdrawn++;
		} else if (!by_name.empty() && written.count(slot.key)) {
			note(slot.key, "The type of \"" + slot.name + "\", " + written.at(slot.key) + ", rests on calls by name: " + by_name + ".");
			r_result.by_name++;
		}
	}
	r_result.typed = int(written.size());
}

Migration migrate(const EngineApi &p_engine, const ProjectApi &p_project, const std::set<std::string> &p_files, int p_rounds) {
	Migrated project(p_project, p_files);
	Migration result;
	infer(p_engine, project, p_rounds, result);
	Workspace workspace(p_project);
	Analysis analysis(p_engine, workspace);
	while (true) {
		// The errors of the scripts and of the checks of the scenes, which find
		// the fixes of connected methods too.
		std::vector<Diagnostic> errors;
		{
			const Analysis::Pass pass(analysis, &project);
			for (const std::string &path : project.script_paths()) {
				const std::optional<std::string> source = is_script_path(path) ? project.script_source(path) : std::nullopt;
				for (const Diagnostic &error : source ? analysis.compile(path, *source)->errors : std::vector<Diagnostic>()) {
					errors.push_back(error);
					errors.back().file = path;
				}
			}
			for (const std::string &path : project.scene_paths()) {
				const std::shared_ptr<const CompileResult> checked = analysis.check_scene(path);
				errors.insert(errors.end(), checked->errors.begin(), checked->errors.end());
			}
		}
		std::vector<std::pair<const Fix *, std::string>> fixes;
		for (const Diagnostic &error : errors) {
			if (const Fix *fix = loop_fix(error, project)) {
				fixes.emplace_back(fix, error.file);
			}
		}
		std::vector<const Fix *> applied;
		const std::map<std::string, std::vector<CheckedEdit>> edits = fixed_together(fixes, applied);
		if (applied.empty() || result.rounds == p_rounds) {
			result.settled = applied.empty();
			result.errors = std::move(errors);
			break;
		}
		for (const Fix *fix : applied) {
			result.fixes[fix->kind]++;
		}
		for (const auto &[file, in_file] : edits) {
			std::string &text = project.texts[file];
			for (auto edit = in_file.rbegin(); edit != in_file.rend(); ++edit) {
				text.replace(edit->begin, edit->end - edit->begin, edit->new_text);
			}
		}
		result.rounds++;
	}
	result.texts = project.texts;
	return result;
}

} // namespace petrichor::compiler
