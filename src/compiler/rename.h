#pragma once

#include "compiler/navigation.h"
#include "compiler/scene_text.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace petrichor::compiler {

// A change that renaming makes (spec §13.3): what it renames, as finding
// references lists it, and the bytes it replaces.
struct RenameEdit {
	Occurrence occurrence;
	CheckedEdit edit;
};

struct Rename {
	// By file: in scripts and in text scenes and resource files, then the
	// candidates, which start unchecked.
	std::vector<RenameEdit> edits;
	// When it cannot rename, each place that keeps it from it: the file, the
	// line from 1 if there is one, and why. There are then no edits.
	std::vector<Diagnostic> refusals;
};

// The name that renaming at byte `p_offset` of the script at `p_path` changes
// to any new name: its bytes in the script. Otherwise the refusal of rename()
// that holds for every new name, as for a node, an engine's declaration or an
// override of one.
struct RenamedName {
	uint32_t begin = 0;
	uint32_t end = 0;
	std::optional<Diagnostic> refusal;
};
RenamedName renamed_name(Analysis &r_analysis, const std::string &p_path, uint32_t p_offset, const std::vector<std::string> &p_other_scripts);

// What renaming the name at byte `p_offset` of the script at `p_path` to
// `p_new` changes, in every place that finding references
// lists (spec §13.3). `p_text` gives the text of a scene or a resource file,
// nullopt if it cannot be read. It refuses:
// - a name that is not an identifier, or a declaration not of this language;
// - a name stored in a binary or an imported file, or in a scene that a file
//   embeds, whose names cannot be rewritten;
// - a name that a text file does not store where the references say;
// - a key `p_new` already stored on a node or a resource with the class, or
//   beside a struct's field it renames, whose value would then take effect;
// - a place it rewrites that a scene uses for another declaration;
// - edits after which the scripts of this language have an error they did
//   not have, or a name refers to another declaration.
// The last is decided by checking the edited scripts in a pass of its own, so
// it is called outside any pass.
Rename rename(Analysis &r_analysis, const std::string &p_path, uint32_t p_offset, const std::string &p_new, const std::vector<std::string> &p_other_scripts,
		const std::function<std::optional<std::string>(const std::string &)> &p_text);

// What the scene editor's renaming or moving nodes of the scene at `p_scene`
// changes in the scripts of this language (spec §13.3). `p_moved` gives the
// new path of each node whose path changes, by its old path. The project of
// `r_analysis` has that scene after the move. The scenes that inherit it may
// have its nodes as before or as after. A literal node path to or from a
// node moved is written anew, as the path from where it starts to where it
// leads (editor/docks/scene_tree_dock.cpp:2077-2120; ed1daf0). A `%Name`
// part stays, with the node's new name. The edits are by file, then by
// offset. These are only listed among the reports, each with its file and
// line:
// - a path that, written anew, would lead elsewhere in some scene that
//   places the script, as when another scene has the old node;
// - a get_node() of no literal path from self, whose Strings name a node
//   renamed or moved by its own name.
// It reads the scripts in passes of its own, so it is called outside any
// pass.
struct MovedNodes {
	std::vector<RenameEdit> edits;
	std::vector<Diagnostic> reports;
};
MovedNodes move_nodes(Analysis &r_analysis, const std::string &p_scene, const std::map<std::string, std::string> &p_moved);

} // namespace petrichor::compiler
