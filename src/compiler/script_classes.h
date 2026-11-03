#pragma once

#include "compiler/ast.h"
#include "compiler/engine_api.h"
#include "compiler/project_api.h"

#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>

namespace petrichor::compiler {

// The other scripts that one compile looks at, each read once (architecture
// §2.1), as their classes: a file's, or an inner class's, whose path is its
// file's followed by "::" and the names down to it (spec §2.1). A class's
// header is enough to name it as a type; its declarations are checked when a
// member is used, so that two classes can refer to each other.
class ScriptClasses {
	struct File {
		ClassDecl decl;
		bool checked = false; // The checker's fields of its classes are filled in.
	};
	// A file being checked, whose classes come from its checkers.
	struct Checking {
		ClassDecl *decl;
		std::function<const ClassDecl *(const std::string &)> declarations; // Null while they are not known yet.
	};

	const EngineApi &engine;
	const ProjectApi *project;
	bool declarations_only;
	std::map<std::string, std::unique_ptr<File>> files; // Null: it cannot be read or parsed.
	std::map<std::string, Checking> checking;
	std::set<std::string> failed;

	const ClassDecl *file_header(const std::string &p_file);

public:
	ScriptClasses(const EngineApi &p_engine, const ProjectApi *p_project, bool p_declarations_only = false) :
			engine(p_engine), project(p_project), declarations_only(p_declarations_only) {}

	// The class that `p_decl` at `p_path` extends, of this language or
	// another: by its path, by a global class_name, or, for an inner class,
	// by the name of an inner class around it (spec §4.1); empty for an
	// engine class or none.
	std::string base_path(const ClassDecl &p_decl, const std::string &p_path);
	std::string native_base(const ClassDecl &p_decl, const std::string &p_path);
	const ProjectApi *get_project() const { return project; }
	// The parsed class, or null when it cannot be read or parsed.
	const ClassDecl *header(const std::string &p_path);
	// With its declarations checked; null when they have errors or depend on
	// themselves.
	const ClassDecl *declarations(const std::string &p_path, bool p_with_scenes = false);
	// While a file is checked, its classes as its checkers have them.
	void enter(const std::string &p_file, ClassDecl &r_decl, std::function<const ClassDecl *(const std::string &)> p_declarations);
	void leave(const std::string &p_file);
};

} // namespace petrichor::compiler
