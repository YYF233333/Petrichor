#pragma once

#include <cstdint>
#include <string>

namespace petrichor::compiler {

enum class Severity {
	Error,
	Warning,
	Hint,
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
	// An error after which the script still compiles and runs, as a check at
	// run time reports it too: a value that a scene lacks (spec §7.5). Exports
	// and the headless build fail on it.
	bool runs = false;
};

} // namespace petrichor::compiler
