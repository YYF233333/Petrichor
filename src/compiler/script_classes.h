#pragma once

#include "compiler/ast.h"
#include "compiler/engine_api.h"
#include "compiler/project_api.h"

#include <map>
#include <memory>
#include <set>
#include <string>

namespace petrichor::compiler {

// Another script of this language, as a script's checker sees it (spec
// §11.1): parsed, and its declarations checked without function bodies when
// something needs them.
struct ScriptClass {
	std::string path;
	ClassDecl decl;
	bool checked = false; // The checker's fields of `decl` are filled in.

	// Its class_name, or the file's name for a class without one.
	std::string name() const;
	// The type of its objects.
	Type type() const;
	const InterfaceDecl *find_interface(const std::string &p_name) const;
};

// The other scripts that one compile looks at, each read once (architecture
// §2.1). A class's header is enough to name it as a type; its declarations
// are checked when a member is used, so that two classes can refer to each
// other.
class ScriptClasses {
	const EngineApi &engine;
	const ProjectApi *project;
	std::map<std::string, std::unique_ptr<ScriptClass>> scripts; // Null: it cannot be read or parsed.
	std::set<std::string> checking;
	std::set<std::string> failed;

public:
	ScriptClasses(const EngineApi &p_engine, const ProjectApi *p_project) :
			engine(p_engine), project(p_project) {}

	const ProjectApi *get_project() const { return project; }
	// The parsed script, or null when it cannot be read or parsed.
	const ScriptClass *header(const std::string &p_path);
	// With its declarations checked; null when they have errors or depend on
	// themselves.
	const ScriptClass *declarations(const std::string &p_path);
};

} // namespace petrichor::compiler
