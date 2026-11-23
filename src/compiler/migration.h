#pragma once

#include "compiler/diagnostic.h"
#include "compiler/project_api.h"

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace petrichor::compiler {

// The GDScript files that migrating those under `p_paths` (res://, the whole
// project when none) takes (architecture §6.6). Their whole inheritance trees
// come with them (spec §11.2), and the GDScript files that make a class-level
// access to them that GDScript cannot make to a class of this language (spec
// §11.3). The trees with a file with @tool are left out (spec §13.4), and
// then a file that one left out accesses so, with its tree.
struct MigrationSet {
	std::set<std::string> files;
	std::vector<std::pair<std::string, std::string>> added; // Each with why.
	std::vector<std::pair<std::string, std::string>> left_out; // Each with why.
	std::vector<std::string> unknown; // Paths given without a GDScript file there.
};
MigrationSet migration_set(const EngineApi &p_engine, const ProjectApi &p_project, const std::vector<std::string> &p_paths);

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

// The project once the files of a migration are renamed (architecture §6.6),
// over `under`: its texts are `texts`, and every path of one of them names
// its new path, as scenes, global classes, autoloads and other scripts
// would.
class Migrated : public ProjectApi {
	const ProjectApi &under;
	std::map<std::string, std::string> renamed; // From the .gd paths.
	std::map<std::string, std::string> back;
	mutable std::map<std::string, std::string> others; // Other scripts' texts, with the paths renamed.

public:
	std::map<std::string, std::string> texts; // Of the files migrated, by their new paths.

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

// What the loop of quick fixes and the check after it gave (architecture
// §6.6): the texts, how many fixes of each kind were applied, in how many
// rounds, and the errors left in the project, each naming its file.
struct Migration {
	std::map<std::string, std::string> texts;
	std::map<FixKind, int> fixes;
	int rounds = 0;
	bool settled = false;
	std::vector<Diagnostic> errors;
};
// Checks the project with `p_files` migrated, in memory, and applies in each
// round the fixes of the automatic kinds, Async and Unwrap, as fixing all of a
// kind does, but not one that edits a file outside them; until a round
// applies none, or after `p_rounds` rounds.
Migration migrate(const EngineApi &p_engine, const ProjectApi &p_project, const std::set<std::string> &p_files, int p_rounds);

} // namespace petrichor::compiler
