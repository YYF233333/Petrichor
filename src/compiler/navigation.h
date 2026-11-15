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

// The `##` comment of a declaration (spec §2.2), as the documentation shows
// it.
struct DocComment {
	std::string brief; // A class's first paragraph.
	std::string description;
	std::vector<std::pair<std::string, std::string>> tutorials; // A class's, as title and link.
	// With the tag's message, which may be empty.
	std::optional<std::string> deprecated;
	std::optional<std::string> experimental;
};

// What a name refers to, as the script editor's lookup shows it (spec §13.2):
// where going to its definition leads, and what hover shows.
struct Lookup {
	SymbolKind kind = SymbolKind::Class;
	// The class whose documentation has it, an engine class, a built-in type
	// or a class of a script, and its name there.
	std::string class_name;
	std::string member;
	// Of a local, which has no documentation.
	std::string type;
	DocComment comment;
	// The file that declares it and the line, from 1; none for the engine's.
	std::string path;
	int line = 0;
};

// What the name at byte `p_offset` of the script at `p_path` refers to, the
// script compiled from `p_source`; nullopt where nothing is, or what the
// editor cannot show, such as a node of a scene.
std::optional<Lookup> lookup(Analysis &r_analysis, const EngineApi &p_engine, const ProjectApi &p_project, const DocName &p_doc_name, const std::string &p_path, const std::string &p_source, uint32_t p_offset);

// A place that refers to what a name declares, as finding references lists it
// (spec §13.2) and renaming changes it (§13.3).
struct Occurrence {
	std::string file; // The script, or the scene or resource file.
	// As the reference table has it: in a script, the name's bytes; in a scene
	// or a resource file, what stores or names the declaration there.
	Reference reference;
	// In a script: the line from 1, the columns of the name and after it from
	// 1, in code points, and the line's text.
	int line = 0;
	int column = 0;
	int end_column = 0;
	std::string text;
	bool declaration = false;
	// A name that may refer to it, but resolves to nothing: one found by name
	// at run time, or a word in a script of another language.
	bool candidate = false;
};

// Where what the name at byte `p_offset` of the script at `p_path` refers to
// is declared and referred to, the script compiled from `p_source`; also
// every declaration of this language that overriding or implementing joins
// to it, through others, as an interface joins its implementations. It
// searches the project's scripts of this language and the checks of its
// scenes and resource files, each as Analysis reads it. The declaration of
// the name comes first, then the files of the others, then the rest by file.
// Then, for a declaration of this language that is not a local, the
// candidates by file, the whole words in `p_other_scripts` among them.
// nullopt where the name refers to nothing.
std::optional<std::vector<Occurrence>> find_references(Analysis &r_analysis, const ProjectApi &p_project, const std::string &p_path, const std::string &p_source, uint32_t p_offset, const std::vector<std::string> &p_other_scripts);

// As the script editor shows a name it completes
// (core/object/script_language.h:293-306).
enum class CompletionKind {
	Class,
	Function,
	Signal,
	Variable,
	Member,
	Enum,
	Constant,
	NodePath,
	FilePath,
	PlainText, // An annotation's or an input action's name, as GDScript gives them.
	Keyword,
};

// A name that completion offers (spec §13.2).
struct CompletionItem {
	CompletionKind kind = CompletionKind::Keyword;
	std::string name;
	// What goes in: a function's name followed by "(", or by "()" when it
	// takes no arguments; a keyword's followed by what comes next; a node
	// path in quotes when it needs them.
	std::string insert;
	// Where it is declared, which orders the list: a local; a member of the
	// class `depth` up from the one whose members are completed; elsewhere in
	// the project; in the engine, as keywords are.
	enum class Place {
		Local,
		Member,
		Project,
		Engine,
	};
	Place place = Place::Engine;
	int depth = 0;
};

struct Completion {
	std::vector<CompletionItem> items;
	// Whether the list shows also right after "(", where the editor shows none
	// otherwise (scene/gui/code_edit.cpp:3832-3835): when it has the values of
	// the enum the argument takes.
	bool force = false;
	// The signature of the call around the cursor, with the parameter at the
	// cursor between two U+FFFF, as the script editor shows it
	// (scene/gui/code_edit.cpp:249-275).
	std::string call_hint;
};

// What can be written at the cursor, which U+FFFF marks in `p_source` as the
// script editor marks it (scene/gui/code_edit.cpp:2390-2400): each name that
// the checker's own lookup accepts there, so that none is an error, or an
// annotation's that the parser does. Only the declarations and the function
// around the cursor are checked.
Completion complete(Analysis &r_analysis, const EngineApi &p_engine, const std::string &p_path, const std::string &p_source);

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
	DocComment comment;
};

// The documentation of a class, also of a struct or an interface.
struct ClassDoc {
	std::string name;
	std::string inherits;
	DocComment comment;
	std::vector<DocMember> constants;
	std::vector<DocMember> properties;
	std::vector<DocMember> methods;
	std::vector<DocMember> signals;
	std::vector<DocMember> enums; // Their names and comments; their values are among the constants.
};

// The documentation of the classes of the script at `p_path`, whose
// declarations are checked, with every member, pub or not: the inner classes
// first, and the script's class last, where the inspector looks for it
// (editor/inspector/editor_inspector.cpp:4742-4747).
std::vector<ClassDoc> document(const ClassDecl &p_class, const std::string &p_path, const DocName &p_doc_name);

} // namespace petrichor::compiler
