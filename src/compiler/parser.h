#pragma once

#include "compiler/ast.h"
#include "compiler/diagnostic.h"
#include "compiler/lexer.h"

#include <vector>

namespace petrichor::compiler {

// Parses one source file into a class declaration. Errors are collected, and
// parsing continues at the next line.
ClassDecl parse(const std::vector<Token> &p_tokens, std::vector<Diagnostic> &r_errors);

// Parses a single expression, such as a default value from the engine's API;
// null after an error.
std::unique_ptr<Expr> parse_expression(const std::string &p_source, std::vector<Diagnostic> &r_errors);

} // namespace petrichor::compiler
