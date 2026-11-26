#pragma once

#include "compiler/diagnostic.h"
#include "compiler/engine_api.h"
#include "compiler/project_api.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace petrichor::compiler {

// res://a.pet converted back: res://a.gd.
std::string reversed_path(const std::string &p_file);

// What converting a file back erased that the tree of its GDScript lacks, by
// the tokens of the .pet file: the calls that read as their receiver, as
// `.unwrap()` and `.detach()` (-1), or as their argument, as `Some(v)` (0),
// by their names; and the types of value types' T? that GDScript leaves
// untyped, by their names (spec §10.1).
struct Erased {
	std::map<uint32_t, int> calls;
	std::set<uint32_t> types;
};

// Reverse conversion (spec §13.4, architecture §6.6) of the .pet files under
// `p_paths` (res://, the whole project when none), from their tokens and
// checked trees only, so that files edited after migrating convert too. A
// file's whole inheritance tree among the .pet files comes with it; a tree
// with a file that uses what GDScript lacks, or a form not converted back
// yet, stays, each place listed. Each file's GDScript must parse to the tree
// of its .pet file without what was erased, or it is a defect of the
// converter.
struct Reversal {
	std::map<std::string, std::string> texts; // The GDScript of the files converted, by their .pet paths.
	std::map<std::string, Erased> erased; // Of the files converted.
	std::vector<std::pair<std::string, std::string>> added; // Each with why.
	std::vector<std::pair<std::string, std::string>> left_out; // Each with why, by path.
	std::vector<Diagnostic> refused; // Each place that keeps a file, naming its file.
	// In the .pet files that stay, each class-level access to a class that
	// converts which this language cannot make to a class of GDScript (spec
	// §11.1).
	std::vector<Diagnostic> lost;
	std::vector<Diagnostic> defects; // Of the re-parse check, each naming its file.
	std::vector<std::string> unknown; // Paths given without a .pet file there.
};
Reversal reverse(const EngineApi &p_engine, const ProjectApi &p_project, const std::vector<std::string> &p_paths);

// The re-parse check of the GDScript `p_gdscript` converted from the .pet
// file `p_source` at `p_path`, the paths of `p_renamed` named by their new
// ones: "" when it parses to the tree of `p_source` without what `p_erased`
// says, else where they first differ.
std::string reparse_difference(const std::string &p_path, const std::string &p_source, const Erased &p_erased, const std::string &p_gdscript, const std::map<std::string, std::string> &p_renamed);

} // namespace petrichor::compiler
