#pragma once

#include <string>

namespace petrichor::compiler {

// A compile error (spec §1.4). Lines and columns start at 1.
struct Diagnostic {
	int line = 0;
	int column = 0;
	std::string message;
};

} // namespace petrichor::compiler
