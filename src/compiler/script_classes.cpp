#include "compiler/script_classes.h"

#include "compiler/checker.h"
#include "compiler/lexer.h"
#include "compiler/parser.h"

namespace petrichor::compiler {

std::string ScriptClass::name() const {
	if (!decl.class_name.empty()) {
		return decl.class_name;
	}
	const size_t slash = path.find_last_of('/');
	return path.substr(slash == std::string::npos ? 0 : slash + 1);
}

Type ScriptClass::type() const {
	return Type::script_class(name(), path, decl.native_base);
}

const InterfaceDecl *ScriptClass::find_interface(const std::string &p_name) const {
	for (const InterfaceDecl &interface : decl.interfaces) {
		if (interface.name == p_name) {
			return &interface;
		}
	}
	return nullptr;
}

std::string ScriptClasses::base_path(const ClassDecl &p_decl, const std::string &p_path) const {
	if (!p_decl.extends_path) {
		return engine.class_exists(p_decl.extends) || !project ? std::string() : project->global_class_path(p_decl.extends);
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
		const ScriptClass *parent = path.empty() ? nullptr : header(path);
		if (!parent) {
			return std::string();
		}
		decl = &parent->decl;
	}
	return std::string();
}

const ScriptClass *ScriptClasses::header(const std::string &p_path) {
	const auto found = scripts.find(p_path);
	if (found != scripts.end()) {
		return found->second.get();
	}
	std::unique_ptr<ScriptClass> &script = scripts[p_path];
	const std::optional<std::string> source = project ? project->script_source(p_path) : std::nullopt;
	if (!source) {
		return nullptr;
	}
	std::vector<Diagnostic> errors;
	const std::vector<Token> tokens = tokenize(*source, errors);
	ClassDecl decl = parse(tokens, errors);
	if (!errors.empty()) {
		return nullptr;
	}
	script = std::make_unique<ScriptClass>();
	script->path = p_path;
	script->decl = std::move(decl);
	return script.get();
}

const ScriptClass *ScriptClasses::declarations(const std::string &p_path, bool p_with_scenes) {
	ScriptClass *script = const_cast<ScriptClass *>(header(p_path));
	if (!script || failed.count(p_path) || checking.count(p_path)) {
		return nullptr;
	}
	if (!script->checked) {
		checking.insert(p_path);
		CompileOptions options;
		options.path = p_path;
		options.project = project;
		options.declarations_only = declarations_only || !p_with_scenes;
		std::vector<Diagnostic> errors;
		check(script->decl, engine, options, errors, *this, false);
		checking.erase(p_path);
		if (!errors.empty()) {
			failed.insert(p_path);
			return nullptr;
		}
		script->checked = true;
	}
	return script;
}

} // namespace petrichor::compiler
