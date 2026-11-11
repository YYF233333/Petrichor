#pragma once

#include "compiler/ast.h"
#include "compiler/engine_api.h"
#include "compiler/project_api.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace petrichor::compiler {

// What other scripts can see of a script: its syntax tokens outside function
// bodies but _init's, whose assignments they see, and its literal node paths;
// all its text if it has syntax errors, whose recovery may read any line as a
// declaration; 0 for none.
uint64_t declaration_hash(const std::optional<std::string> &p_source);

// Each name that a source assigns with `=`, by its tokens: a member's or a
// local's, and an argument's by name, so more than it assigns.
std::set<std::string> assigned_names(const SourceFile &p_source);

class ScriptClasses;

// What a check read (architecture §2.1): other scripts, by declaration_hash(),
// and each question asked of the project, with a test of whether a later pass
// still gets the same answer.
struct Reads {
	std::map<std::string, uint64_t> scripts;
	std::map<std::string, std::function<bool(ScriptClasses &)>> answers;
	// It read a file whose check was in progress, so no later check reuses it.
	bool partial = false;

	void merge(const Reads &p_other);
};

// The other scripts that one compile looks at, each read once (architecture
// §2.1), as their classes: a file's, or an inner class's, whose path is its
// file's followed by "::" and the names down to it (spec §2.1). A class's
// header is enough to name it as a type; its declarations are checked when a
// member is used, so that two classes can refer to each other. One that
// records reads is shared by the compiles of a pass of Analysis.
class ScriptClasses {
	struct File {
		ClassDecl decl; // Recovered from syntax errors, if any.
		int syntax_error_line = 0; // The first one's; 0 for none.
		// The declarations with syntax errors or errors of their check, each
		// with the line of its first.
		std::map<const void *, int> errors;
		bool checked = false; // The checker's fields of its classes are filled in.
		bool failed = false; // Its class could not be declared, as when its base has errors.
		bool with_scenes = false; // How they were checked.
		Reads reads; // Of its declarations' check.
	};
	// A file being checked, whose classes come from its checkers.
	struct Checking {
		ClassDecl *decl;
		std::function<const ClassDecl *(const std::string &)> declarations; // Null while they are not known yet.
	};
	// The project as checks see it: each answer goes to the reads of the check
	// that asked; sources are read as scripts.
	class RecordedProject : public ProjectApi {
		ScriptClasses &classes;

	public:
		explicit RecordedProject(ScriptClasses &r_classes) :
				classes(r_classes) {}

		template <typename T>
		T ask(const std::string &p_key, std::function<T(const ProjectApi &)> p_question) const;

		std::vector<std::string> scene_paths() const override;
		std::optional<Scene> scene(const std::string &p_path) const override;
		std::string main_scene() const override;
		std::optional<ResourceInfo> resource(const std::string &p_path) const override;
		std::string uid_path(const std::string &p_uid) const override;
		std::vector<std::string> dependencies(const std::string &p_path) const override;
		std::string global_class_path(const std::string &p_name) const override;
		std::string autoload_path(const std::string &p_name) const override;
		std::map<std::string, std::string> global_class_paths() const override;
		std::vector<std::string> autoload_names() const override;
		std::vector<std::string> resource_files() const override;
		std::vector<std::string> input_actions() const override;
		std::vector<std::string> script_paths() const override;
		std::optional<std::string> script_source(const std::string &p_path) const override;
		std::optional<ForeignClass> foreign_class(const std::string &p_path, bool p_members) const override;
	};

	const EngineApi &engine;
	const ProjectApi *sources;
	RecordedProject recorded;
	const ProjectApi *project; // `recorded` while reads are recorded.
	bool declarations_only;
	// Null: it cannot be read. A file whose declarations are wanted
	// both with and without scenes has a second entry, its path followed by a
	// line break and the way.
	std::map<std::string, std::unique_ptr<File>> files;
	std::map<std::string, Checking> checking;
	// The checks that record reads, innermost last, with their files.
	std::vector<std::pair<std::string, Reads>> frames;
	std::map<std::string, uint64_t> hashes; // declaration_hash() of files read.
	std::pair<std::string, uint64_t> compiled; // The file being compiled, and its hash.
	std::optional<std::vector<Scene>> listed; // The project's scenes, as scene_structure() gives them.
	std::map<std::string, std::string> bases; // The base paths of the scripts on their nodes.
	std::optional<std::map<std::string, std::string>> roots; // The uses of scenes as roots, as root_use() gives them.

	std::unique_ptr<File> parse_file(const std::string &p_file) const;
	const ClassDecl *file_header(const std::string &p_file);
	// Notes in the innermost check that it read `p_file`.
	void read_script(const std::string &p_file);
	// Notes in the innermost check the answer to a question, which it gets
	// again in a later pass to see whether the answer changed. What the
	// question reads is its own, not the check's.
	template <typename T>
	T answer(const std::string &p_key, std::function<T(ScriptClasses &)> p_question);
	std::vector<Scene> scenes_where(const std::function<bool(const Scene &)> &p_uses);
	// Whether the script is `p_base` or extends it, as the headers say.
	bool is_or_extends(std::string p_script, const std::string &p_base);

public:
	ScriptClasses(const EngineApi &p_engine, const ProjectApi *p_project, bool p_declarations_only = false, bool p_record = false) :
			engine(p_engine), sources(p_project), recorded(*this), project(p_record ? &recorded : p_project), declarations_only(p_declarations_only) {}
	ScriptClasses(const ScriptClasses &) = delete;
	ScriptClasses &operator=(const ScriptClasses &) = delete;

	// The class that `p_decl` at `p_path` extends, of this language or
	// another: by its path, by a global class_name, or, for an inner class,
	// by the name of an inner class around it (spec §4.1); empty for an
	// engine class or none.
	std::string base_path(const ClassDecl &p_decl, const std::string &p_path);
	std::string native_base(const ClassDecl &p_decl, const std::string &p_path);
	// The project as checks should ask it.
	const ProjectApi *get_project() const { return project; }
	// The parsed class, or null when it cannot be read. Other scripts see what
	// the parser recovers of a file with syntax errors, so that an error in a
	// function body leaves its declarations usable.
	const ClassDecl *header(const std::string &p_path);
	// With its declarations checked; null when the class could not be
	// declared, as when its base has errors or it depends on itself. Each of
	// its declarations that has errors says so through errors_in().
	const ClassDecl *declarations(const std::string &p_path, bool p_with_scenes = false);
	// The file and line of the first error in a declaration of a class that
	// declarations() gave, such as a VarDecl or a FuncDecl's signature;
	// nullopt for one without.
	std::optional<std::pair<std::string, int>> errors_in(const void *p_decl) const;
	// The line of the first error in a declaration of the file of the class at
	// `p_path`; nullopt for none.
	std::optional<int> first_error(const std::string &p_path) const;
	// The file and line of the first syntax error in the files of the class at
	// `p_path` and of the classes of this language it extends, which may have
	// lost a declaration that a script misses.
	std::optional<std::pair<std::string, int>> syntax_error(const std::string &p_path);
	// While a file is checked, its classes as its checkers have them.
	void enter(const std::string &p_file, ClassDecl &r_decl, std::function<const ClassDecl *(const std::string &)> p_declarations);
	void leave(const std::string &p_file);

	// The hash of the file as the project has it now, read once.
	uint64_t script_hash(const std::string &p_file);
	// The String of each preload() of one literal String in the file, as
	// written, in function bodies too (spec §7.6).
	std::vector<std::string> preloads(const std::string &p_file);
	// The scene at the path as a script check reads it (architecture §6.4):
	// its nodes, without what it stores, connects or names.
	std::optional<Scene> scene_structure(const std::string &p_path);
	// The reverse questions of a script check (architecture §6.4), with the
	// scenes so: those in which a node has the script or a subclass attached,
	// and those that instance the scene or inherit it.
	std::vector<Scene> scenes_with_script(const std::string &p_script);
	std::vector<Scene> scenes_instancing(const std::string &p_scene);
	// The files other than its own with a class that extends the class.
	std::vector<std::string> subclasses(const std::string &p_class);
	// assigned_names() of a saved file, an index kept per file.
	std::set<std::string> assigned_in(const std::string &p_file);
	// A reverse question of the check of a scene: how it is used as a root
	// (spec §7.5), as "res://spawner.gd loads the scene"; "" when it is not.
	// The main scene is, and so is a scene that a script of this language or
	// GDScript preloads or loads by a literal path, or that a scene stores.
	std::string root_use(const std::string &p_scene);
	// Records what the compile of `p_file` from `p_source` reads, until
	// end_compile(), after which nothing refers to its classes.
	void begin_compile(const std::string &p_file, const std::string &p_source);
	Reads end_compile();
};

} // namespace petrichor::compiler
