#include "compiler/script_classes.h"

#include "bytecode/module.h"
#include "compiler/checker.h"
#include "compiler/lexer.h"
#include "compiler/parser.h"

#include <algorithm>
#include <iterator>
#include <type_traits>

namespace petrichor::compiler {

namespace {

// The inner class of a file's class that the names after its file in
// `p_path` lead to; the class itself for a file's path.
const ClassDecl *walk(const ClassDecl *p_decl, const std::string &p_path) {
	for (size_t at = p_path.find("::"); p_decl && at != std::string::npos;) {
		const size_t next = p_path.find("::", at + 2);
		p_decl = p_decl->find_class(p_path.substr(at + 2, next == std::string::npos ? next : next - at - 2));
		at = next;
	}
	return p_decl;
}

// FNV-1a, as bytecode::hash_source().
struct Hash {
	uint64_t value = 14695981039346656037ull;

	void add(const void *p_data, size_t p_size) {
		const unsigned char *data = static_cast<const unsigned char *>(p_data);
		for (size_t i = 0; i < p_size; i++) {
			value ^= data[i];
			value *= 1099511628211ull;
		}
	}
	template <typename T>
	void add(const T &p_value) {
		add(&p_value, sizeof(p_value));
	}
	void add(const std::string &p_text) {
		add(uint64_t(p_text.size()));
		add(p_text.data(), p_text.size());
	}
};

// The tokens of a function's body, which other scripts do not see: from the
// Indent that opens a body on lines of its own to the Dedent that closes it,
// or the statements on the function's line.
std::optional<TokenRange> body_of(const FuncDecl &p_function, const std::vector<Token> &p_tokens) {
	if (!p_function.has_body || p_function.body.empty()) {
		return std::nullopt;
	}
	TokenRange body = { p_function.body.front()->range.first, p_function.body.back()->range.last };
	if (body.first > 0 && p_tokens[body.first - 1].kind == TokenKind::Indent) {
		int depth = 0;
		for (uint32_t i = --body.first; i < p_tokens.size(); i++) {
			if (p_tokens[i].kind == TokenKind::Indent) {
				depth++;
			} else if (p_tokens[i].kind == TokenKind::Dedent && --depth == 0) {
				body.last = i;
				break;
			}
		}
	}
	return body;
}

// Calls `p_fn` on each function the class declares, its structs' methods
// included.
template <typename Fn>
void for_each_function(const ClassDecl &p_decl, Fn &&p_fn) {
	for (const FuncDecl &fn : p_decl.functions) {
		p_fn(fn);
	}
	for (const StructDecl &decl : p_decl.structs) {
		for (const FuncDecl &method : decl.methods) {
			p_fn(method);
		}
	}
}

void mark_bodies(const ClassDecl &p_decl, std::vector<bool> &r_body) {
	for_each_function(p_decl, [&](const FuncDecl &p_fn) {
		if (const std::optional<TokenRange> body = p_fn.name == "_init" ? std::nullopt : body_of(p_fn, p_decl.source->tokens)) {
			for (uint32_t i = body->first; i <= body->last; i++) {
				r_body[i] = true;
			}
		}
	});
	for (const std::unique_ptr<ClassDecl> &inner : p_decl.classes) {
		mark_bodies(*inner, r_body);
	}
}

// Notes in `r_marked` the declarations of `p_decl` and its inner classes on
// whose lines `p_errors` fall, with `p_key` and the line of the first; a
// class itself by the line of its extends. Errors in function bodies, those
// of inline setters and getters included, mark none.
void mark_errors(const ClassDecl &p_decl, const std::vector<Diagnostic> &p_errors, const std::string &p_key, std::map<const void *, std::pair<std::string, int>> &r_marked) {
	const SourceFile &source = *p_decl.source;
	std::vector<TokenRange> bodies;
	for_each_function(p_decl, [&](const FuncDecl &p_fn) {
		if (const std::optional<TokenRange> body = body_of(p_fn, source.tokens)) {
			bodies.push_back(*body);
		}
	});
	const auto in_body = [&](uint32_t p_offset) {
		return std::any_of(bodies.begin(), bodies.end(), [&](const TokenRange &p_body) {
			return p_offset >= source.begin(p_body) && p_offset < source.end(p_body);
		});
	};
	const auto mark = [&](const void *p_node, const TokenRange &p_range) {
		const uint32_t begin = source.begin(p_range);
		const uint32_t end = source.end(p_range);
		const int first = source.position(begin).line;
		const int last = source.position(end > begin ? end - 1 : begin).line;
		for (const Diagnostic &error : p_errors) {
			const int line = source.position(error.begin).line;
			if (line < first || line > last || in_body(error.begin)) {
				continue;
			}
			const auto [entry, added] = r_marked.emplace(p_node, std::make_pair(p_key, line));
			if (!added) {
				entry->second.second = std::min(entry->second.second, line);
			}
		}
	};
	if (p_decl.extends_token != NO_TOKEN) {
		mark(&p_decl, TokenRange{ p_decl.extends_token, p_decl.extends_token });
	}
	for_each_child(p_decl, [&](const auto &p_child) {
		if constexpr (std::is_same_v<std::decay_t<decltype(p_child)>, ClassDecl>) {
			mark_errors(p_child, p_errors, p_key, r_marked);
		} else {
			mark(&p_child, p_child.range);
		}
	});
}

// A scene as a script check reads it (architecture §6.4): its nodes, their
// classes, scripts and instances, without what it stores, connects or names.
Scene structure(Scene p_scene) {
	for (SceneNode &node : p_scene.nodes) {
		node.properties.clear();
	}
	p_scene.connections.clear();
	p_scene.named_members.clear();
	p_scene.resources.clear();
	return p_scene;
}

} // namespace

std::vector<std::pair<bool, std::string>> ProjectApi::literal_loads(const std::string &p_path) const {
	std::vector<Diagnostic> ignored;
	const std::shared_ptr<const SourceFile> source = tokenize(script_source(p_path).value_or(std::string()), ignored);
	const std::vector<Token> &tokens = source->tokens;
	std::vector<std::pair<bool, std::string>> found;
	for (size_t i = 0; i + 3 < tokens.size(); i++) {
		if (tokens[i].kind == TokenKind::Identifier && (tokens[i].text == "preload" || tokens[i].text == "load") && tokens[i + 1].kind == TokenKind::ParenOpen && tokens[i + 2].kind == TokenKind::String && tokens[i + 3].kind == TokenKind::ParenClose) {
			found.emplace_back(tokens[i].text == "preload", tokens[i + 2].text);
		}
	}
	return found;
}

uint64_t declaration_hash(const std::optional<std::string> &p_source) {
	if (!p_source) {
		return 0;
	}
	std::vector<Diagnostic> errors;
	const ClassDecl decl = parse(*p_source, errors);
	Hash hash;
	if (!errors.empty()) {
		hash.add(uint8_t(1));
		hash.add(*p_source);
		return hash.value;
	}
	const std::vector<Token> &tokens = decl.source->tokens;
	std::vector<bool> body(tokens.size());
	mark_bodies(decl, body);
	for (size_t i = 0; i < tokens.size(); i++) {
		if (!body[i]) {
			hash.add(tokens[i].kind);
			hash.add(tokens[i].text);
			hash.add(tokens[i].int_value);
			hash.add(tokens[i].float_value);
			hash.add(tokens[i].int_overflows);
		}
	}
	return hash.value;
}

void Reads::merge(const Reads &p_other) {
	scripts.insert(p_other.scripts.begin(), p_other.scripts.end());
	sources.insert(p_other.sources.begin(), p_other.sources.end());
	answers.insert(p_other.answers.begin(), p_other.answers.end());
	partial = partial || p_other.partial;
}

template <typename T>
const T &ScriptClasses::asked(const std::string &p_key, const std::function<T(ScriptClasses &)> &p_question) {
	auto found = answered.find(p_key);
	if (found == answered.end()) {
		std::vector<std::pair<std::string, Reads>> asking;
		asking.swap(frames);
		T given = p_question(*this);
		frames.swap(asking);
		found = answered.emplace(p_key, std::move(given)).first;
	}
	return *std::any_cast<T>(&found->second);
}

template <typename T>
T ScriptClasses::answer(const std::string &p_key, std::function<T(ScriptClasses &)> p_question) {
	const T &given = asked(p_key, p_question);
	if (!frames.empty()) {
		frames.back().second.answers.emplace(p_key, [p_key, given, p_question](ScriptClasses &r_classes) {
			return r_classes.asked(p_key, p_question) == given;
		});
	}
	return given;
}

template <typename T>
T ScriptClasses::RecordedProject::ask(const std::string &p_key, std::function<T(const ProjectApi &)> p_question) const {
	return classes.answer<T>(p_key, [p_question](ScriptClasses &r_classes) {
		return p_question(*r_classes.sources);
	});
}

std::vector<std::string> ScriptClasses::RecordedProject::scene_paths() const {
	return ask<std::vector<std::string>>("scene paths", [](const ProjectApi &p_project) {
		return p_project.scene_paths();
	});
}

std::optional<Scene> ScriptClasses::RecordedProject::scene(const std::string &p_path) const {
	return ask<std::optional<Scene>>("scene " + p_path, [p_path](const ProjectApi &p_project) {
		return p_project.scene(p_path);
	});
}

std::string ScriptClasses::RecordedProject::main_scene() const {
	return ask<std::string>("main scene", [](const ProjectApi &p_project) {
		return p_project.main_scene();
	});
}

std::optional<ResourceInfo> ScriptClasses::RecordedProject::resource(const std::string &p_path) const {
	return ask<std::optional<ResourceInfo>>("resource " + p_path, [p_path](const ProjectApi &p_project) {
		return p_project.resource(p_path);
	});
}

std::string ScriptClasses::RecordedProject::uid_path(const std::string &p_uid) const {
	return ask<std::string>("uid " + p_uid, [p_uid](const ProjectApi &p_project) {
		return p_project.uid_path(p_uid);
	});
}

std::vector<std::string> ScriptClasses::RecordedProject::dependencies(const std::string &p_path) const {
	return ask<std::vector<std::string>>("dependencies " + p_path, [p_path](const ProjectApi &p_project) {
		return p_project.dependencies(p_path);
	});
}

std::string ScriptClasses::RecordedProject::global_class_path(const std::string &p_name) const {
	return ask<std::string>("global class " + p_name, [p_name](const ProjectApi &p_project) {
		return p_project.global_class_path(p_name);
	});
}

std::string ScriptClasses::RecordedProject::autoload_path(const std::string &p_name) const {
	return ask<std::string>("autoload " + p_name, [p_name](const ProjectApi &p_project) {
		return p_project.autoload_path(p_name);
	});
}

std::map<std::string, std::string> ScriptClasses::RecordedProject::global_class_paths() const {
	return ask<std::map<std::string, std::string>>("global classes", [](const ProjectApi &p_project) {
		return p_project.global_class_paths();
	});
}

std::vector<std::string> ScriptClasses::RecordedProject::autoload_names() const {
	return ask<std::vector<std::string>>("autoload names", [](const ProjectApi &p_project) {
		return p_project.autoload_names();
	});
}

std::vector<std::string> ScriptClasses::RecordedProject::resource_files() const {
	return ask<std::vector<std::string>>("resource files", [](const ProjectApi &p_project) {
		return p_project.resource_files();
	});
}

std::vector<std::string> ScriptClasses::RecordedProject::input_actions() const {
	return ask<std::vector<std::string>>("input actions", [](const ProjectApi &p_project) {
		return p_project.input_actions();
	});
}

std::vector<std::string> ScriptClasses::RecordedProject::script_paths() const {
	return ask<std::vector<std::string>>("script paths", [](const ProjectApi &p_project) {
		return p_project.script_paths();
	});
}

std::optional<std::string> ScriptClasses::RecordedProject::script_source(const std::string &p_path) const {
	return classes.sources->script_source(p_path);
}

std::optional<ForeignClass> ScriptClasses::RecordedProject::foreign_class(const std::string &p_path, bool p_members) const {
	return ask<std::optional<ForeignClass>>(std::string(p_members ? "foreign class with members " : "foreign class ") + p_path, [p_path, p_members](const ProjectApi &p_project) {
		return p_project.foreign_class(p_path, p_members);
	});
}

std::string ScriptClasses::base_path(const ClassDecl &p_decl, const std::string &p_path) {
	if (!p_decl.extends_path) {
		const size_t dot = p_decl.extends.find('.');
		const std::string head = p_decl.extends.substr(0, dot);
		if (dot == std::string::npos && engine.class_exists(head)) {
			return std::string();
		}
		std::string path;
		// An inner class names the inner classes of the classes around it
		// (spec §4.1), nearest first.
		for (std::string scope = p_path; path.empty() && scope.size() > script_file(scope).size();) {
			scope = scope.substr(0, scope.rfind("::"));
			const ClassDecl *decl = header(scope);
			if (decl && decl->find_class(head)) {
				path = scope + "::" + head;
			}
		}
		// The class of its own file, by its class_name, also before the project
		// lists the class.
		const ClassDecl *file = path.empty() ? header(script_file(p_path)) : nullptr;
		if (file && !file->class_name.empty() && file->class_name == head) {
			path = script_file(p_path);
		}
		if (path.empty() && project) {
			path = project->global_class_path(head);
		}
		if (path.empty()) {
			return path;
		}
		// `Other.Inner`.
		for (size_t at = dot; at != std::string::npos;) {
			const size_t next = p_decl.extends.find('.', at + 1);
			path += "::" + p_decl.extends.substr(at + 1, next == std::string::npos ? next : next - at - 1);
			at = next;
		}
		return path;
	}
	std::string path = p_decl.extends;
	if (path.rfind("res://", 0) != 0) {
		path = p_path.substr(0, p_path.find_last_of('/') + 1) + path;
	}
	return simplify_path(path);
}

std::string ScriptClasses::native_base(const ClassDecl &p_decl, const std::string &p_path) {
	const ClassDecl *decl = &p_decl;
	std::string path = p_path;
	std::set<std::string> seen;
	while (seen.insert(path).second) {
		if (!decl->extends_path && engine.class_exists(decl->extends)) {
			return decl->extends;
		}
		path = base_path(*decl, path);
		const ClassDecl *parent = path.empty() ? nullptr : header(path);
		if (!parent) {
			return std::string();
		}
		decl = parent;
	}
	return std::string();
}

std::unique_ptr<ScriptClasses::File> ScriptClasses::parse_file(const std::string &p_file, const std::string &p_key) {
	const std::optional<std::string> source = sources ? sources->script_source(p_file) : std::nullopt;
	if (!source) {
		return nullptr;
	}
	std::vector<Diagnostic> errors;
	ClassDecl decl = parse(*source, errors);
	std::unique_ptr<File> file = std::make_unique<File>();
	if (!errors.empty()) {
		const auto first = std::min_element(errors.begin(), errors.end(), [](const Diagnostic &a, const Diagnostic &b) { return a.begin < b.begin; });
		file->syntax_error_line = decl.source->position(first->begin).line;
	}
	file->decl = std::move(decl);
	if (!p_key.empty()) {
		mark_errors(file->decl, errors, p_key, erring);
	}
	return file;
}

const ClassDecl *ScriptClasses::file_header(const std::string &p_file) {
	const auto local = checking.find(p_file);
	if (local != checking.end()) {
		return local->second.decl;
	}
	const auto found = files.find(p_file);
	if (found != files.end()) {
		return found->second ? &found->second->decl : nullptr;
	}
	std::unique_ptr<File> &file = files[p_file];
	file = parse_file(p_file, p_file);
	return file ? &file->decl : nullptr;
}

void ScriptClasses::read_script(const std::string &p_file) {
	if (frames.empty() || frames.back().first == p_file) {
		return;
	}
	Reads &reads = frames.back().second;
	reads.partial = reads.partial || checking.count(p_file);
	reads.scripts.emplace(p_file, p_file == compiled.first ? compiled.second : script_hash(p_file));
}

const ClassDecl *ScriptClasses::header(const std::string &p_path) {
	const std::string file = script_file(p_path);
	read_script(file);
	return walk(file_header(file), p_path);
}

const ClassDecl *ScriptClasses::declarations(const std::string &p_path, bool p_with_scenes) {
	const std::string file = script_file(p_path);
	read_script(file);
	const auto local = checking.find(file);
	if (local != checking.end()) {
		// Null while its declarations are being checked, as when a script it
		// uses uses it in turn.
		return local->second.declarations ? local->second.declarations(p_path) : walk(local->second.decl, p_path);
	}
	if (!file_header(file)) {
		return nullptr;
	}
	const bool with_scenes = !declarations_only && p_with_scenes;
	std::string key = file;
	std::unique_ptr<File> *slot = &files[key];
	if (((*slot)->checked || (*slot)->failed) && (*slot)->with_scenes != with_scenes) {
		key += with_scenes ? "\nwith scenes" : "\nwithout scenes";
		slot = &files[key];
		if (!*slot && !(*slot = parse_file(file, key))) {
			return nullptr;
		}
	}
	File &entry = **slot;
	if (!entry.checked && !entry.failed) {
		CompileOptions options;
		options.path = file;
		options.project = project;
		options.mode = with_scenes ? CheckMode::ForOthersWithScenes : CheckMode::ForOthers;
		options.inflows = inflows;
		std::vector<Diagnostic> errors;
		const bool recording = !frames.empty();
		if (recording) {
			frames.emplace_back(file, Reads());
		}
		check(entry.decl, engine, options, errors, *this);
		checking.erase(file);
		if (recording) {
			entry.reads = std::move(frames.back().second);
			frames.pop_back();
			for (const auto &[read, hash] : entry.reads.scripts) {
				readers[read].insert(key);
			}
			if (entry.reads.partial) {
				partial.push_back(key);
			}
		}
		entry.with_scenes = with_scenes;
		entry.failed = !entry.decl.declared;
		entry.checked = !entry.failed;
		// The inference of a migration takes them as the loop of fixes after
		// it leaves them (architecture §6.6).
		if (inflows) {
			errors.erase(std::remove_if(errors.begin(), errors.end(), [](const Diagnostic &p_error) { return std::any_of(p_error.fixes.begin(), p_error.fixes.end(), [](const Fix &p_fix) { return p_fix.assumed(); }); }), errors.end());
		}
		mark_errors(entry.decl, errors, key, erring);
	}
	if (!frames.empty() && frames.back().first != file) {
		frames.back().second.merge(entry.reads);
	}
	const ClassDecl *decl = entry.failed ? nullptr : walk(&entry.decl, p_path);
	return decl && decl->declared ? decl : nullptr;
}

std::optional<std::pair<std::string, int>> ScriptClasses::errors_in(const void *p_decl) const {
	const auto found = erring.find(p_decl);
	if (found == erring.end()) {
		return std::nullopt;
	}
	const std::string &key = found->second.first;
	return std::make_pair(key.substr(0, key.find('\n')), found->second.second);
}

std::optional<int> ScriptClasses::first_error(const std::string &p_path) const {
	const std::string file = script_file(p_path);
	std::optional<int> first;
	for (const auto &[decl, mark] : erring) {
		if (mark.first.substr(0, mark.first.find('\n')) == file) {
			first = first ? std::min(*first, mark.second) : mark.second;
		}
	}
	return first;
}

std::optional<std::pair<std::string, int>> ScriptClasses::syntax_error(const std::string &p_path) {
	std::set<std::string> seen;
	for (std::string path = p_path; !path.empty() && seen.insert(path).second;) {
		const std::string file = script_file(path);
		const auto found = files.find(file);
		if (found != files.end() && found->second && found->second->syntax_error_line) {
			return std::make_pair(file, found->second->syntax_error_line);
		}
		const ClassDecl *decl = header(path);
		path = decl ? base_path(*decl, path) : std::string();
	}
	return std::nullopt;
}

void ScriptClasses::enter(const std::string &p_file, ClassDecl &r_decl, std::function<const ClassDecl *(const std::string &)> p_declarations) {
	checking[p_file] = { &r_decl, std::move(p_declarations) };
}

void ScriptClasses::leave(const std::string &p_file) {
	// The file being compiled stays as checked, for what is done with it next;
	// declarations() keeps a file read for its declarations with the others.
	checking[p_file].declarations = nullptr;
}

uint64_t ScriptClasses::script_hash(const std::string &p_file) {
	const auto found = hashes.find(p_file);
	if (found != hashes.end()) {
		return found->second.first;
	}
	const std::optional<std::string> source = sources ? sources->script_source(p_file) : std::nullopt;
	return (hashes[p_file] = { declaration_hash(source), source ? bytecode::hash_source(*source) : 0 }).first;
}

uint64_t ScriptClasses::source_hash(const std::string &p_file) {
	script_hash(p_file);
	return hashes[p_file].second;
}

std::string file_at(const std::string &p_path, const std::string &p_from, const ProjectApi &p_project) {
	const std::string path = resolved_path(p_path, p_from);
	return path.rfind("uid://", 0) == 0 ? p_project.uid_path(path) : path;
}

namespace {

bool ends_with(const std::string &p_text, const std::string &p_end) {
	return p_text.size() > p_end.size() && p_text.compare(p_text.size() - p_end.size(), p_end.size(), p_end) == 0;
}

bool is_script_file(const std::string &p_file) {
	return ends_with(p_file, ".pet") || ends_with(p_file, ".gd") || ends_with(p_file, ".cs");
}

// preload() of one literal String, as the checker takes it; "" for another.
const std::string *preload_written(const Expr &p_expr) {
	const bool literal = p_expr.kind == ExprKind::Call && !p_expr.base && p_expr.text == "preload" && p_expr.args.size() == 1 && p_expr.args[0]->kind == ExprKind::StringLiteral && p_expr.args[0]->op == TokenKind::String;
	return literal ? &p_expr.args[0]->text : nullptr;
}

template <typename Node, typename Fn>
void for_each_expr(const Node &p_node, Fn &&p_fn) {
	const auto visit = [&](const auto &p_self, const auto &p_child) -> void {
		if constexpr (std::is_same_v<std::decay_t<decltype(p_child)>, Expr>) {
			p_fn(p_child);
		}
		for_each_child(p_child, [&](const auto &p_next) { p_self(p_self, p_next); });
	};
	visit(visit, p_node);
}

template <typename Node, typename Fn>
void for_each_type(const Node &p_node, Fn &&p_fn) {
	const auto visit = [&](const auto &p_self, const auto &p_child) -> void {
		if constexpr (std::is_same_v<std::decay_t<decltype(p_child)>, TypeRef>) {
			p_fn(p_child);
		}
		for_each_child(p_child, [&](const auto &p_next) { p_self(p_self, p_next); });
	};
	visit(visit, p_node);
}

} // namespace

std::vector<LoadEdge> load_edges(const ClassDecl &p_file, const std::string &p_path, const ProjectApi &p_project) {
	const std::string file = script_file(p_path);
	const std::set<std::string> read = names_read(p_file);
	std::map<std::string, const ConstDecl *> constants; // Those of inner classes too, the outer first.
	std::set<std::string> classes;
	if (!p_file.class_name.empty()) {
		classes.insert(p_file.class_name);
	}
	std::vector<const ClassDecl *> all;
	const auto collect = [&](const auto &p_self, const ClassDecl &p_class) -> void {
		all.push_back(&p_class);
		for (const ConstDecl &constant : p_class.constants) {
			constants.emplace(constant.name, &constant);
		}
		for (const std::unique_ptr<ClassDecl> &inner : p_class.classes) {
			classes.insert(inner->name);
			p_self(p_self, *inner);
		}
	};
	collect(collect, p_file);
	// The file of the class a type or a value names by its first name.
	const auto named_file = [&](const std::string &p_name) -> std::string {
		const std::string head = p_name.substr(0, p_name.find('.'));
		if (head.empty()) {
			return std::string();
		}
		const auto constant = constants.find(head);
		const std::string *preloaded = constant != constants.end() && constant->second->initializer ? preload_written(*constant->second->initializer) : nullptr;
		if (preloaded) {
			return file_at(*preloaded, p_path, p_project);
		}
		return classes.count(head) ? file : script_file(p_project.global_class_path(head));
	};
	// The file of the preload that a constant holds: its own, or that of a
	// constant or an inner class of another file that it copies, as
	// `const P = Board.Piece` does.
	const auto held = [&](const Expr &p_value) -> std::string {
		if (const std::string *preloaded = preload_written(p_value)) {
			return file_at(*preloaded, p_path, p_project);
		}
		if (p_value.kind != ExprKind::Member || !p_value.base || p_value.base->kind != ExprKind::Name) {
			return std::string();
		}
		const std::string from = named_file(p_value.base->text);
		const std::optional<std::string> source = from.empty() || from == file || !is_script_path(from) ? std::nullopt : p_project.script_source(from);
		if (!source) {
			return std::string();
		}
		std::vector<Diagnostic> ignored;
		const ClassDecl other = parse(*source, ignored);
		for (const ConstDecl &constant : other.constants) {
			const std::string *preloaded = constant.name == p_value.text && constant.initializer ? preload_written(*constant.initializer) : nullptr;
			if (preloaded) {
				return file_at(*preloaded, from, p_project);
			}
		}
		return other.find_class(p_value.text) ? from : std::string();
	};
	std::vector<LoadEdge> edges;
	std::set<const Expr *> held_by_constants;
	for (const ClassDecl *cls : all) {
		for (const ConstDecl &constant : cls->constants) {
			if (!constant.initializer) {
				continue;
			}
			held_by_constants.insert(constant.initializer.get());
			const std::string target = held(*constant.initializer);
			if (!target.empty() && !type_only(constant, read, is_script_file(target))) {
				edges.push_back({ target, constant.initializer->range, false });
			}
		}
	}
	for_each_expr(p_file, [&](const Expr &p_expr) {
		const std::string *preloaded = held_by_constants.count(&p_expr) ? nullptr : preload_written(p_expr);
		if (preloaded) {
			edges.push_back({ file_at(*preloaded, p_path, p_project), p_expr.range, false });
		}
	});
	// The containers built as the class loads, but of the file's own classes,
	// whose scripts the loading file gives them (spec §7.6).
	const auto containers = [&](const auto &p_node) {
		for_each_type(p_node, [&](const TypeRef &p_type) {
			if (p_type.name != "Array" && p_type.name != "Dictionary") {
				return;
			}
			for (const TypeRef &item : p_type.args) {
				const std::string target = named_file(item.name);
				if (is_script_file(target) && target != file) {
					edges.push_back({ target, item.range, true });
				}
			}
		});
	};
	for (const ClassDecl *cls : all) {
		for (const ConstDecl &constant : cls->constants) {
			containers(constant);
		}
		for (const VarDecl &variable : cls->static_vars) {
			containers(variable);
		}
		for (const FuncDecl &function : cls->functions) {
			if (function.name == "_static_init") {
				containers(function);
			}
		}
	}
	return edges;
}

std::vector<std::string> ScriptClasses::loaded_files(const std::string &p_file) {
	return recorded.ask<std::vector<std::string>>("files loaded with " + p_file, [p_file](const ProjectApi &p_project) {
		std::vector<std::string> files;
		if (is_script_path(p_file)) {
			std::vector<Diagnostic> ignored;
			for (const LoadEdge &edge : load_edges(parse(p_project.script_source(p_file).value_or(std::string()), ignored), p_file, p_project)) {
				files.push_back(edge.file);
			}
			return files;
		}
		for (const auto &[preload, path] : p_project.literal_loads(p_file)) {
			if (preload) {
				files.push_back(file_at(path, p_file, p_project));
			}
		}
		std::vector<Diagnostic> ignored;
		const std::shared_ptr<const SourceFile> source = tokenize(p_project.script_source(p_file).value_or(std::string()), ignored);
		const std::vector<Token> &tokens = source->tokens;
		const std::map<std::string, std::string> globals = p_project.global_class_paths();
		for (size_t i = 0; i < tokens.size(); i++) {
			const auto global = tokens[i].kind == TokenKind::Identifier && (i == 0 || tokens[i - 1].kind != TokenKind::Period) ? globals.find(tokens[i].text) : globals.end();
			if (global != globals.end() && is_script_file(global->second) && !ends_with(global->second, ".gd")) {
				files.push_back(global->second);
			}
		}
		return files;
	});
}

std::optional<Scene> ScriptClasses::scene_structure(const std::string &p_path) {
	return recorded.ask<std::optional<Scene>>("structure of the scene " + p_path, [p_path](const ProjectApi &p_project) {
		const std::optional<Scene> scene = p_project.scene(p_path);
		return scene ? std::optional<Scene>(structure(*scene)) : scene;
	});
}

std::vector<Scene> ScriptClasses::scenes_where(const std::function<bool(const Scene &)> &p_uses) {
	if (!listed) {
		listed.emplace();
		for (const std::string &path : sources->scene_paths()) {
			if (const std::optional<Scene> scene = sources->scene(path)) {
				listed->push_back(structure(*scene));
			}
		}
	}
	std::vector<Scene> found;
	std::copy_if(listed->begin(), listed->end(), std::back_inserter(found), p_uses);
	return found;
}

bool ScriptClasses::is_or_extends(std::string p_script, const std::string &p_base) {
	std::set<std::string> seen;
	while (!p_script.empty() && p_script != p_base && seen.insert(p_script).second) {
		const auto [base, added] = bases.try_emplace(p_script);
		if (added && is_script_path(p_script)) {
			// As saved, as the project indexes it, also while it is being compiled.
			const std::string file = script_file(p_script);
			const std::unique_ptr<File> saved = file == compiled.first ? parse_file(file, std::string()) : nullptr;
			const ClassDecl *decl = walk(file == compiled.first ? (saved ? &saved->decl : nullptr) : file_header(file), p_script);
			base->second = decl ? base_path(*decl, p_script) : std::string();
		}
		p_script = base->second;
	}
	return p_script == p_base;
}

const ClassDecl *ScriptClasses::nearest(std::string p_path, const std::function<bool(const ClassDecl &)> &p_has) {
	for (std::set<std::string> seen; !p_path.empty() && seen.insert(p_path).second;) {
		const ClassDecl *decl = header(p_path);
		if (!decl || p_has(*decl)) {
			return decl;
		}
		p_path = base_path(*decl, p_path);
	}
	return nullptr;
}

bool ScriptClasses::has_static(const std::string &p_path, const std::string &p_name) {
	return nearest(p_path, [&](const ClassDecl &p_decl) { return std::any_of(p_decl.functions.begin(), p_decl.functions.end(), [&](const FuncDecl &p_function) { return p_function.name == p_name && p_function.is_static; }); }) != nullptr;
}

const ClassDecl *ScriptClasses::declaring(const std::string &p_path, const std::string &p_name) {
	const auto named = [&](const auto &p_list) { return std::any_of(p_list.begin(), p_list.end(), [&](const auto &p_decl) { return p_decl.name == p_name; }); };
	const ClassDecl *found = nearest(p_path, [&](const ClassDecl &p_decl) { return named(p_decl.enums) || named(p_decl.constants) || p_decl.find_class(p_name); });
	return found ? found : header(p_path);
}

std::vector<Scene> ScriptClasses::scenes_with_script(const std::string &p_script) {
	return answer<std::vector<Scene>>("scenes with " + p_script, [p_script](ScriptClasses &r_classes) {
		return r_classes.scenes_where([&](const Scene &p_scene) {
			return std::any_of(p_scene.nodes.begin(), p_scene.nodes.end(), [&](const SceneNode &p_node) { return r_classes.is_or_extends(p_node.script, p_script); });
		});
	});
}

std::vector<Scene> ScriptClasses::scenes_placing(const std::string &p_script) {
	return answer<std::vector<Scene>>("scenes placing " + p_script, [p_script](ScriptClasses &r_classes) {
		return r_classes.scenes_where([&](const Scene &p_scene) {
			return std::any_of(p_scene.nodes.begin(), p_scene.nodes.end(), [&](const SceneNode &p_node) { return r_classes.is_or_extends(p_node.script.empty() ? p_node.instance_script : p_node.script, p_script); });
		});
	});
}

std::vector<Scene> ScriptClasses::scenes_instancing(const std::string &p_scene) {
	return answer<std::vector<Scene>>("scenes instancing the scene " + p_scene, [p_scene](ScriptClasses &r_classes) {
		return r_classes.scenes_where([&](const Scene &p_each) {
			return p_each.base == p_scene || std::any_of(p_each.nodes.begin(), p_each.nodes.end(), [&](const SceneNode &p_node) { return p_node.instance == p_scene; });
		});
	});
}

// From the whole project, read once in a pass; the first use found is given.
// A relative path of load() is from res:// (core/io/resource_loader.cpp:693-694; ed1daf0).
std::string ScriptClasses::root_use(const std::string &p_scene) {
	return answer<std::string>("use of the scene " + p_scene + " as a root", [p_scene](ScriptClasses &r_classes) {
		const ProjectApi &project = *r_classes.sources;
		std::optional<std::map<std::string, std::string>> &uses = r_classes.roots;
		if (!uses) {
			uses.emplace();
			uses->emplace(project.main_scene(), "the scene is the main scene");
			for (const std::string &script : project.script_paths()) {
				for (const auto &[preload, written] : project.literal_loads(script)) {
					const std::string path = resolved_path(written, preload ? script : std::string("res://"));
					uses->emplace(path.rfind("uid://", 0) == 0 ? project.uid_path(path) : path, script + (preload ? " preloads the scene" : " loads the scene"));
				}
			}
			const auto stored = [&](const std::string &p_path, const std::vector<std::pair<std::string, SceneValue>> &p_properties, const std::string &p_holder) {
				for (const auto &[name, value] : p_properties) {
					std::vector<std::string> held;
					value.add_scenes(held);
					for (const std::string &each : held) {
						uses->emplace(each, p_path + " stores the scene in \"" + name + "\" of " + p_holder);
					}
				}
			};
			for (const std::string &path : project.scene_paths()) {
				const std::optional<Scene> scene = project.scene(path);
				for (const SceneNode &node : scene ? scene->nodes : std::vector<SceneNode>()) {
					stored(path, node.properties, node.path == "." ? std::string("the root") : node.path);
				}
				for (const SceneResource &resource : scene ? scene->resources : std::vector<SceneResource>()) {
					stored(path, resource.properties, resource.shown());
				}
			}
		}
		const auto found = uses->find(p_scene);
		return found == uses->end() ? std::string() : found->second;
	});
}

void ScriptClasses::begin_compile(const std::string &p_file, const std::string &p_source) {
	compiled = { p_file, declaration_hash(p_source) };
	frames.emplace_back(p_file, Reads());
}

Reads ScriptClasses::end_compile() {
	Reads reads = std::move(frames.back().second);
	frames.clear();
	checking.erase(compiled.first);
	// Checks that saw a file in progress, such as the compiled one, may refer
	// to its classes, and so may checks that read theirs; they are done again
	// when needed.
	while (!partial.empty()) {
		const std::string key = std::move(partial.back());
		partial.pop_back();
		const auto entry = files.find(key);
		if (entry == files.end() || !entry->second) {
			continue;
		}
		for (const auto &[read, hash] : entry->second->reads.scripts) {
			readers[read].erase(key);
		}
		files.erase(entry);
		for (auto mark = erring.begin(); mark != erring.end();) {
			mark = mark->second.first == key ? erring.erase(mark) : std::next(mark);
		}
		const std::set<std::string> &read_by = readers[key.substr(0, key.find('\n'))];
		partial.insert(partial.end(), read_by.begin(), read_by.end());
	}
	compiled = {};
	return reads;
}

} // namespace petrichor::compiler
