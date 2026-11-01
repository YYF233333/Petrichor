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
};

} // namespace petrichor::compiler
