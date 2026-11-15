#pragma once

#include "compiler/lexer.h"

#include <optional>
#include <string>

namespace petrichor::compiler {

// Lays a source file out by the rules of spec §13.2. Gives nothing for a file
// with syntax errors, nor when the result would not lex to the same tokens
// and comments, with the reason in `r_error`.
std::optional<std::string> format(const std::string &p_source, std::string *r_error = nullptr);

// The edits, in order, that make `p_source` into `p_formatted`, which
// format() gave for it, so differs from it only in blanks. Blank lines are
// removed and added after the line before them, so that an editor keeps what
// it marks on the other lines.
std::vector<Edit> format_edits(const std::string &p_source, const std::string &p_formatted);

} // namespace petrichor::compiler
