#pragma once

#include <functional>
#include <string>

namespace petrichor::tests {

// Checks the conventions of architecture §2.1 on a source: its token stream is
// lossless, and each node's range lies in its parent's after the siblings
// before it, with each name token spelling the name, syntax errors or not.
void check_syntax_conventions(const std::string &p_source);

// For a source without syntax errors: cuts each line in the middle, as while
// typing, and checks that `p_compile` (if any) takes the result, that the
// conventions hold, and that the statements of the other lines are all still
// in the syntax tree (architecture §3).
void check_recovery(const std::string &p_source, const std::function<void(const std::string &)> &p_compile);

} // namespace petrichor::tests
