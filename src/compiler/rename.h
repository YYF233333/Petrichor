#pragma once

#include "compiler/navigation.h"
#include "compiler/scene_text.h"

#include <functional>
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

} // namespace petrichor::compiler
