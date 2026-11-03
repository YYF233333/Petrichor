#include "compiler/script_classes.h"

#include "compiler/checker.h"
#include "compiler/lexer.h"
#include "compiler/parser.h"

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

} // namespace

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
	const std::optional<std::string> source = project ? project->script_source(p_file) : std::nullopt;
	if (!source) {
		return nullptr;
	}
	std::vector<Diagnostic> errors;
	ClassDecl decl = parse(*source, errors);
	if (!errors.empty()) {
		return nullptr;
	}
	file = std::make_unique<File>();
	file->decl = std::move(decl);
	return &file->decl;
}

const ClassDecl *ScriptClasses::header(const std::string &p_path) {
	return walk(file_header(script_file(p_path)), p_path);
}

const ClassDecl *ScriptClasses::declarations(const std::string &p_path, bool p_with_scenes) {
	const std::string file = script_file(p_path);
	const auto local = checking.find(file);
	if (local != checking.end()) {
		// Null while its declarations are being checked, as when a script it
		// uses uses it in turn.
		return local->second.declarations ? local->second.declarations(p_path) : walk(local->second.decl, p_path);
	}
	if (!file_header(file) || failed.count(file)) {
		return nullptr;
	}
	File &entry = *files[file];
	if (!entry.checked) {
		CompileOptions options;
		options.path = file;
		options.project = project;
		options.declarations_only = declarations_only || !p_with_scenes;
		std::vector<Diagnostic> errors;
		check(entry.decl, engine, options, errors, *this, false);
		if (!errors.empty()) {
			failed.insert(file);
			return nullptr;
		}
		entry.checked = true;
	}
	return walk(&entry.decl, p_path);
}

void ScriptClasses::enter(const std::string &p_file, ClassDecl &r_decl, std::function<const ClassDecl *(const std::string &)> p_declarations) {
	checking[p_file] = { &r_decl, std::move(p_declarations) };
}

void ScriptClasses::leave(const std::string &p_file) {
	// A file read for its declarations is kept with the others; the one being
	// compiled stays as checked, for what is done with it next.
	const auto found = files.find(p_file);
	if (found != files.end() && found->second && &found->second->decl == checking[p_file].decl) {
		checking.erase(p_file);
	} else {
		checking[p_file].declarations = nullptr;
	}
}

} // namespace petrichor::compiler
