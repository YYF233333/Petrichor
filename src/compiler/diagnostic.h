#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace petrichor::compiler {

enum class Severity {
	Error,
	Warning,
	Hint,
};

// A change to a file that names the text it replaces, which whoever applies it
// checks first (architecture §2.1).
struct CheckedEdit {
	uint32_t begin = 0;
	uint32_t end = 0;
	std::string old_text;
	std::string new_text;
};

// A quick fix of a diagnostic (spec §13.2), named by `title`.
struct Fix {
	std::string title;
	// It keeps what the code meant in GDScript, comes first and needs no
	// input; fixing all applies only these (architecture §2.1).
	bool automatic = false;
	struct Edit {
		std::string file; // The script it changes; empty for the script compiled.
		CheckedEdit change;
	};
	std::vector<Edit> edits;
};

// A compile error, a warning or a hint (spec §1.4), over a byte range of the
// source. compile() adds the line and column of its start, from 1, columns
// in code points.
struct Diagnostic {
	uint32_t begin = 0;
	uint32_t end = 0;
	std::string message;
	int line = 0;
	int column = 0;
	Severity severity = Severity::Error;
	std::string warning; // A warning's or a hint's name, which @warning_ignore takes (spec §2.3).
	// An error in how a scene uses the script is also marked on the node of
	// that scene that has it (spec §7, §13.2), by its path from the root.
	std::string scene;
	std::string node;
	// The script whose source its range is in, when a scene's check reports it
	// (architecture §6.4); empty for the script compiled.
	std::string file;
	// An error of a scene's check after which the game still runs, as a check
	// at run time reports it too: a value that a scene lacks (spec §7.5).
	// Exports and the headless build fail on it.
	bool runs = false;
	std::vector<Fix> fixes; // The automatic one first.
};

} // namespace petrichor::compiler
