#pragma once

#include "compiler/diagnostic.h"
#include "compiler/project_api.h"
#include "compiler/types.h"

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace petrichor::compiler {

// What checks record of the slots without a type in a migration's files and
// of the values that flow into them (architecture §6.6), given the types
// tried for the slots. A slot is known by its file and the offset of its
// name, its function's for what that returns.
struct Inflows {
	enum class Kind {
		Parameter,
		Return,
		Member, // Or a static variable or a constant.
		Local,
		Signal, // A parameter of a signal.
	};
	using Key = std::pair<std::string, uint32_t>;
	struct Slot {
		Kind kind = Kind::Local;
		std::string name;
		// A parameter's function or signal, its index there, and how many
		// arguments the function takes at least and at most, which a call by
		// name meets.
		std::string function;
		int index = 0;
		int least = 0;
		int most = 0;
		Key key;
		std::string owner; // The class, but of a local.
		// A variable that starts as an empty container, whose elements are the
		// slot: "Array" or "Dictionary".
		std::string container;
	};
	// A value into a slot, at an offset of a file, or the arguments of a call by
	// name, on a value whose class is not known to have the method. One into
	// an empty container may be an element, or a key of a Dictionary.
	enum class Part {
		Whole,
		Element,
		Key,
	};
	struct Flow {
		int slot = -1;
		Type type;
		Key at;
		Part part = Part::Whole;
		uint32_t last = 0; // Where the value's last token is.
	};
	struct Call {
		std::string name;
		std::vector<Type> args;
		std::vector<Key> at;
	};
	// An error about other places than its own range, by its file and range:
	// an assigned value that does not fit is about its target too, a call
	// that fails for its receiver only about the receiver, and an operation
	// that fails with one Variant operand only about that operand.
	using Range = std::pair<uint32_t, uint32_t>;
	struct About {
		std::string file;
		Range error;
		std::vector<Range> places;
	};
	std::set<std::string> files;
	std::map<Key, Type> tried;
	std::vector<Slot> slots;
	std::map<Key, int> found;
	// An override's parameter without a type, by where its name is, to the
	// slot of its base's, whose type it takes.
	std::map<Key, Key> shared;
	std::vector<Flow> flows;
	std::vector<Call> calls;
	std::vector<About> about;
	// Members typed from the scenes and constants typed by resources that
	// another file read without their types, which the next round takes as
	// slots.
	std::vector<Slot> unknown;
	// The reads of variables that give for an int what the same float
	// gives: beside a float in arithmetic, compared, or where a float is
	// expected. Each by where its name is, with where it begins.
	std::vector<std::pair<Key, uint32_t>> kept;

	// The slot's index, once found.
	int slot(const Slot &p_slot);
	// What is tried for it; Error for nothing yet.
	Type type(int p_slot) const;
};

// The GDScript files that migrating those under `p_paths` (res://, the whole
// project when none) takes (architecture §6.6). Their whole inheritance trees
// come with them (spec §11.2), and the GDScript files that make a class-level
// access to them that GDScript cannot make to a class of this language (spec
// §11.3). The trees with a file with @tool are left out (spec §13.4), and
// then a file that one left out accesses so, with its tree. So are the
// script templates, the files under `p_templates`.
struct MigrationSet {
	std::set<std::string> files;
	std::vector<std::pair<std::string, std::string>> added; // Each with why.
	std::vector<std::pair<std::string, std::string>> left_out; // Each with why, by path.
	std::vector<std::string> unknown; // Paths given without a GDScript file there.
};
MigrationSet migration_set(const EngineApi &p_engine, const ProjectApi &p_project, const std::vector<std::string> &p_paths, const std::string &p_templates = std::string());

// res://a.gd once migrated: res://a.pet.
std::string migrated_path(const std::string &p_file);

// `p_text`, a script's at `p_path`, with each String literal that is exactly
// a path of `p_renamed`, from res:// or from the script, naming its new path;
// `r_count` counts them.
std::string renamed_in_script(const std::string &p_text, const std::string &p_path, const std::map<std::string, std::string> &p_renamed, int &r_count);
// The text of a scene, a resource or project.godot with each quoted path of
// `p_renamed` naming its new path, also after the `*` of an autoload;
// `r_count` counts them.
std::string renamed_in_resource(const std::string &p_text, const std::map<std::string, std::string> &p_renamed, int &r_count);

// `p_text` with each string in single or double quotes that spans lines, as
// GDScript reads it, written in triple quotes (spec §2.2); `r_count` counts
// them.
std::string triple_quoted(const std::string &p_text, int &r_count);

// The project once the files of a migration are renamed (architecture §6.6),
// over `under`: its texts are `texts`, with their strings that span lines in
// triple quotes, and every path of one of them names its new path, as
// scenes, global classes, autoloads and other scripts would.
class Migrated : public ProjectApi {
	const ProjectApi &under;
	std::map<std::string, std::string> renamed; // From the .gd paths.
	std::map<std::string, std::string> back;
	mutable std::map<std::string, std::string> others; // Other scripts' texts, with the paths renamed.

public:
	std::map<std::string, std::string> texts; // Of the files migrated, by their new paths.
	int quoted = 0; // The strings written in triple quotes.

	// `p_files` start as their texts, with the paths renamed.
	Migrated(const ProjectApi &p_under, const std::set<std::string> &p_files);

	const std::map<std::string, std::string> &renames() const { return renamed; }
	// A path, its file's renamed if it is one of them; an inner class's too.
	std::string path(const std::string &p_path) const;

	std::vector<std::string> scene_paths() const override { return under.scene_paths(); }
	std::optional<Scene> scene(const std::string &p_path) const override;
	std::string main_scene() const override { return under.main_scene(); }
	std::optional<ResourceInfo> resource(const std::string &p_path) const override;
	std::string uid_path(const std::string &p_uid) const override { return path(under.uid_path(p_uid)); }
	std::vector<std::string> dependencies(const std::string &p_path) const override;
	std::string global_class_path(const std::string &p_name) const override { return path(under.global_class_path(p_name)); }
	std::string autoload_path(const std::string &p_name) const override { return path(under.autoload_path(p_name)); }
	std::map<std::string, std::string> global_class_paths() const override;
	std::vector<std::string> autoload_names() const override { return under.autoload_names(); }
	std::vector<std::string> resource_files() const override;
	std::vector<std::string> input_actions() const override { return under.input_actions(); }
	std::vector<std::string> script_paths() const override;
	std::optional<std::string> script_source(const std::string &p_path) const override;
	std::optional<ForeignClass> foreign_class(const std::string &p_path, bool p_members) const override;
};

// What the inference, the loop of quick fixes and the check after it gave
// (architecture §6.6): the texts; how many strings were written in triple
// quotes; how many slots were typed, how many left without a type, how many
// have types that rest on incomplete evidence, and how many received int and
// float, which a use tells apart; how many fixes of each kind were applied,
// in how many rounds; and the errors left in the project, each naming its
// file.
// The hints at the slots say why they have no type, with what they
// received, or what calls by name gave their types and what Variant values
// they leave out.
struct Migration {
	std::map<std::string, std::string> texts;
	int quoted = 0;
	int typed = 0;
	int withdrawn = 0;
	int partial = 0;
	int numbers = 0;
	std::vector<Diagnostic> notes;
	std::map<FixKind, int> fixes;
	int rounds = 0;
	bool settled = false;
	std::vector<Diagnostic> errors;
};
// Checks the project with `p_files` migrated, in memory. It infers the types
// of their slots without one, in at most `p_rounds` rounds of checks, and
// writes them. Then it applies in each round the fixes of the automatic
// kinds, Async and Unwrap, and Optional for a value that a scene lacks, as
// fixing all of a kind does, but not one that edits a file outside them;
// until a round applies none, or after `p_rounds` rounds.
Migration migrate(const EngineApi &p_engine, const ProjectApi &p_project, const std::set<std::string> &p_files, int p_rounds);

} // namespace petrichor::compiler
