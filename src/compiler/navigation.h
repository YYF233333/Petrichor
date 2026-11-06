#pragma once

#include "compiler/analysis.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace petrichor::compiler {

struct ClassDecl;

// The name the editor's documentation gives the class of a script file, by
// its path: its class_name, else the name of the autoload it is, else its
// quoted path, as GDScript names it
// (modules/gdscript/editor/gdscript_docgen.cpp:40-46, 339-347, 594-598),
// which only the engine knows for autoloads.
using DocName = std::function<std::string(const std::string &p_file)>;

// The documentation's name of a class of this language by its path, as
// "res://combat.pet::Shot" is Combat.Shot.
std::string doc_class_name(const DocName &p_doc_name, const std::string &p_path);

// What a name refers to, as the script editor's lookup shows it (spec §13.2):
// where going to its definition leads, and what hover shows.
struct Lookup {
	SymbolKind kind = SymbolKind::Class;
	// The class whose documentation has it, an engine class, a built-in type
	// or a class of a script, and its name there.
	std::string class_name;
	std::string member;
	std::string type; // Of a local.
	// The file that declares it and the line, from 1; none for the engine's.
	std::string path;
	int line = 0;
};

// What the name at byte `p_offset` of the script at `p_path` refers to, the
// script compiled from `p_source`; nullopt where nothing is, or what the
// editor cannot show, such as a node of a scene.
std::optional<Lookup> lookup(Analysis &r_analysis, const EngineApi &p_engine, const ProjectApi &p_project, const DocName &p_doc_name, const std::string &p_path, const std::string &p_source, uint32_t p_offset);

// A member in the editor's documentation (core/doc_data.h).
struct DocMember {
	std::string name;
	// A property's or constant's type, a method's return type; an enum is int
	// with the enum's name.
	std::string type;
	std::string enumeration;
	std::string value; // A constant's value, or the default of a property or an argument, as written.
	std::string qualifiers; // A method's: "static", "async", "abstract".
	std::vector<DocMember> arguments; // A method's or a signal's.
};

// The documentation of a class, also of a struct or an interface.
struct ClassDoc {
	std::string name;
	std::string inherits;
	std::vector<DocMember> constants;
	std::vector<DocMember> properties;
	std::vector<DocMember> methods;
	std::vector<DocMember> signals;
	std::vector<std::string> enums; // Their values are among the constants.
};

// The documentation of the classes of the script at `p_path`, whose
// declarations are checked, with every member, pub or not: the inner classes
// first, and the script's class last, where the inspector looks for it
// (editor/inspector/editor_inspector.cpp:4742-4747).
std::vector<ClassDoc> document(const ClassDecl &p_class, const std::string &p_path, const DocName &p_doc_name);

} // namespace petrichor::compiler
