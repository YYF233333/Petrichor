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
	return Type::script_class(name(), path, decl.extends);
}

const InterfaceDecl *ScriptClass::find_interface(const std::string &p_name) const {
	for (const InterfaceDecl &interface : decl.interfaces) {
		if (interface.name == p_name) {
			return &interface;
		}
	}
	return nullptr;
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

const ScriptClass *ScriptClasses::declarations(const std::string &p_path) {
	ScriptClass *script = const_cast<ScriptClass *>(header(p_path));
	if (!script || failed.count(p_path) || checking.count(p_path)) {
		return nullptr;
	}
	if (!script->checked) {
		checking.insert(p_path);
		CompileOptions options;
		options.path = p_path;
		options.project = project;
		options.declarations_only = true;
		std::vector<Diagnostic> errors;
		check(script->decl, engine, options, errors, *this);
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
