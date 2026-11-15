#pragma once

#include "compiler/references.h"

#include <cstdint>
#include <string>
#include <vector>

namespace petrichor::compiler {

// A change to a file that names the text it replaces, which whoever applies it
// checks first (architecture §2.1).
struct CheckedEdit {
	uint32_t begin = 0;
	uint32_t end = 0;
	std::string old_text;
	std::string new_text;
};

// The files that store the names a reference of a scene's check stands for
// (architecture §6.4): the scene that stores the key or the connection, and
// the files of the animations and synchronization configurations.
std::vector<std::string> files_storing(const Reference &p_reference);

// Where the text of the scene or resource file `p_file`, in the engine's text
// format, stores the names that `p_references` stand for, each `p_old`
// (architecture §6.4): a connection's signal if `p_signal`, else its method.
// Each becomes `p_new` as the engine writes it there. `error` says what keeps
// it from it at `line`, from 1, or 0 for the whole file:
// - a name found not once, or that is not `p_old`;
// - a key `p_new` beside a key it renames, whose value would then take effect.
struct LocatedNames {
	std::vector<CheckedEdit> edits; // By offset.
	std::string error;
	int line = 0;
};
LocatedNames locate_names(const std::string &p_text, const std::string &p_file, const std::vector<Reference> &p_references, bool p_signal, const std::string &p_old, const std::string &p_new);

} // namespace petrichor::compiler
