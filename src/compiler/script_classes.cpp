#include "compiler/script_classes.h"

#include "compiler/checker.h"
#include "compiler/lexer.h"
#include "compiler/parser.h"

#include <algorithm>

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

// The tokens of each function body: from the Indent that opens a body on
// lines of its own to the Dedent that closes it, or the statements on the
// function's line.
void mark_bodies(const ClassDecl &p_decl, std::vector<bool> &r_body) {
	const std::vector<Token> &tokens = p_decl.source->tokens;
	for (const FuncDecl &fn : p_decl.functions) {
		if (!fn.has_body || fn.body.empty()) {
			continue;
		}
		uint32_t first = fn.body.front()->range.first;
		uint32_t last = fn.body.back()->range.last;
		if (first > 0 && tokens[first - 1].kind == TokenKind::Indent) {
			int depth = 0;
			for (uint32_t i = --first; i < tokens.size(); i++) {
				if (tokens[i].kind == TokenKind::Indent) {
					depth++;
				} else if (tokens[i].kind == TokenKind::Dedent && --depth == 0) {
					last = i;
					break;
				}
			}
		}
		for (uint32_t i = first; i <= last && i < tokens.size(); i++) {
			r_body[i] = true;
		}
	}
	for (const std::unique_ptr<ClassDecl> &inner : p_decl.classes) {
		mark_bodies(*inner, r_body);
	}
}

} // namespace

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
	answers.insert(p_other.answers.begin(), p_other.answers.end());
	partial = partial || p_other.partial;
}

template <typename T>
T ScriptClasses::RecordedProject::ask(const std::string &p_key, std::function<T(const ProjectApi &)> p_question) const {
	T answer = p_question(*classes.sources);
	if (!classes.frames.empty()) {
		classes.frames.back().second.answers.emplace(p_key, [answer, p_question](const ProjectApi &p_project) {
			return p_question(p_project) == answer;
		});
	}
	return answer;
}

std::vector<Scene> ScriptClasses::RecordedProject::scenes_with_script(const std::string &p_script_path) const {
	return ask<std::vector<Scene>>("scenes with " + p_script_path, [p_script_path](const ProjectApi &p_project) {
		return p_project.scenes_with_script(p_script_path);
	});
}

std::vector<Scene> ScriptClasses::RecordedProject::scenes_instancing_script(const std::string &p_script_path) const {
	return ask<std::vector<Scene>>("scenes instancing " + p_script_path, [p_script_path](const ProjectApi &p_project) {
		return p_project.scenes_instancing_script(p_script_path);
	});
}

std::optional<Scene> ScriptClasses::RecordedProject::scene(const std::string &p_path) const {
	return ask<std::optional<Scene>>("scene " + p_path, [p_path](const ProjectApi &p_project) {
		return p_project.scene(p_path);
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
	std::vector<std::string> parts;
	for (size_t at = 6; at <= path.size();) {
		const size_t end = path.find('/', at);
		const std::string part = path.substr(at, end == std::string::npos ? end : end - at);
		if (part == "..") {
			if (!parts.empty()) {
				parts.pop_back();
			}
		} else if (!part.empty() && part != ".") {
			parts.push_back(part);
		}
		if (end == std::string::npos) {
			break;
		}
		at = end + 1;
	}
	path = "res://";
	for (size_t i = 0; i < parts.size(); ++i) {
		path += (i ? "/" : "") + parts[i];
	}
	return path;
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

std::unique_ptr<ScriptClasses::File> ScriptClasses::parse_file(const std::string &p_file) const {
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
	file = parse_file(p_file);
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
	std::unique_ptr<File> *slot = &files[file];
	if (((*slot)->checked || (*slot)->failed) && (*slot)->with_scenes != with_scenes) {
		slot = &files[file + (with_scenes ? "\nwith scenes" : "\nwithout scenes")];
		if (!*slot && !(*slot = parse_file(file))) {
			return nullptr;
		}
	}
	File &entry = **slot;
	if (!entry.checked && !entry.failed) {
		CompileOptions options;
		options.path = file;
		options.project = project;
		options.declarations_only = !with_scenes;
		std::vector<Diagnostic> errors;
		const bool recording = !frames.empty();
		if (recording) {
			frames.emplace_back(file, Reads());
		}
		check(entry.decl, engine, options, errors, *this, false);
		checking.erase(file);
		if (recording) {
			entry.reads = std::move(frames.back().second);
			frames.pop_back();
		}
		entry.with_scenes = with_scenes;
		entry.failed = !errors.empty();
		entry.checked = !entry.failed;
	}
	if (!frames.empty() && frames.back().first != file) {
		frames.back().second.merge(entry.reads);
	}
	return entry.failed ? nullptr : walk(&entry.decl, p_path);
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
		return found->second;
	}
	return hashes[p_file] = declaration_hash(sources ? sources->script_source(p_file) : std::nullopt);
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
	std::set<std::string> dropped;
	for (bool again = true; again;) {
		again = false;
		for (auto entry = files.begin(); entry != files.end();) {
			const Reads *of = entry->second ? &entry->second->reads : nullptr;
			if (of && (of->partial || std::any_of(of->scripts.begin(), of->scripts.end(), [&](const auto &p_read) { return dropped.count(p_read.first) > 0; }))) {
				dropped.insert(entry->first.substr(0, entry->first.find('\n')));
				entry = files.erase(entry);
				again = true;
			} else {
				++entry;
			}
		}
	}
	compiled = {};
	return reads;
}

} // namespace petrichor::compiler
