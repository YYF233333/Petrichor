#include "compiler/migration.h"

#include "compiler/analysis.h"
#include "compiler/ast.h"
#include "compiler/compiler.h"
#include "compiler/lexer.h"
#include "compiler/script_classes.h"
#include "compiler/workspace.h"

#include <algorithm>
#include <functional>

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

MigrationSet migration_set(const EngineApi &p_engine, const ProjectApi &p_project, const std::vector<std::string> &p_paths) {
	ScriptClasses classes(p_engine, &p_project, true);
	Scripts scripts;
	for (const std::string &path : p_project.script_paths()) {
		if (is_gdscript(path)) {
			scripts.files.insert(path);
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
	for (const std::string &path : p_paths) {
		const std::string directory = path.back() == '/' ? path : path + "/";
		const auto under = [&](const std::string &p_file) { return p_file == path || p_file.rfind(directory, 0) == 0; };
		if (std::none_of(scripts.files.begin(), scripts.files.end(), under)) {
			result.unknown.push_back(path);
		}
		for (const std::string &file : scripts.files) {
			if (under(file)) {
				taken[file];
			}
		}
	}
	if (p_paths.empty()) {
		for (const std::string &file : scripts.files) {
			taken[file];
		}
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

Migration migrate(const EngineApi &p_engine, const ProjectApi &p_project, const std::set<std::string> &p_files, int p_rounds) {
	Migrated project(p_project, p_files);
	Workspace workspace(p_project);
	Analysis analysis(p_engine, workspace);
	Migration result;
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
			const auto fix = std::find_if(error.fixes.begin(), error.fixes.end(), [](const Fix &p_fix) { return p_fix.automatic() || p_fix.kind == FixKind::Async || p_fix.kind == FixKind::Unwrap; });
			const bool inside = fix != error.fixes.end() && std::all_of(fix->edits.begin(), fix->edits.end(), [&](const Fix::Edit &p_edit) { return project.texts.count(p_edit.file.empty() ? error.file : p_edit.file) > 0; });
			if (inside) {
				fixes.emplace_back(&*fix, error.file);
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
