#pragma once

#include <cstdint>
#include <string>

namespace petrichor::compiler {

// A compile error (spec §1.4), over a byte range of the source. compile()
// adds the line and column of its start, from 1, columns in code points.
struct Diagnostic {
	uint32_t begin = 0;
	uint32_t end = 0;
	std::string message;
	int line = 0;
	int column = 0;
	// An error in how a scene uses the script is also marked on the node of
	// that scene that has it (spec §7, §13.2), by its path from the root.
	std::string scene;
	std::string node;
};

} // namespace petrichor::compiler
