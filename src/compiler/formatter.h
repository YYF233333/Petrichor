#pragma once

#include <optional>
#include <string>

namespace petrichor::compiler {

// Lays a source file out by the rules of spec §13.2. Gives nothing for a file
// with syntax errors, nor when the result would not lex to the same tokens
// and comments, with the reason in `r_error`.
std::optional<std::string> format(const std::string &p_source, std::string *r_error = nullptr);

} // namespace petrichor::compiler
