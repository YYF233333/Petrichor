#pragma once

#include <string>

namespace petrichor::tests {

// Checks the conventions of architecture §2.1 on a source: its token stream is
// lossless, and, when it parses, each node's range lies in its parent's after
// the siblings before it, with each name token spelling the name.
void check_syntax_conventions(const std::string &p_source);

} // namespace petrichor::tests
