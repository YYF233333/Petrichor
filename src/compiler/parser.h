#pragma once

#include "compiler/ast.h"
#include "compiler/diagnostic.h"
#include "compiler/lexer.h"

#include <functional>
#include <utility>
#include <vector>

namespace petrichor::compiler {

// Where an annotation is written, which decides the names it may take.
enum class AnnotationPlace {
	Header, // Of the file's class, before its members (spec §2.1).
	Member,
	Statement,
	Interface, // Before a property of an interface (spec §3.9).
};

// The names an annotation may take at a place, each with whether it takes
// arguments; at a member, those of the @export family besides, which the
// checker knows.
std::vector<std::pair<std::string, bool>> annotation_names(AnnotationPlace p_place);

// Parses one source file into a class declaration, with its lexer's errors.
// After an error the line keeps what it parsed, with error nodes for what is
// missing, and parsing goes on at the next line (architecture §2.1).
// `p_on_annotation` hears the name of each annotation and where it is.
ClassDecl parse(std::string p_source, std::vector<Diagnostic> &r_errors, bool p_header_only = false, const std::function<void(const Token &, AnnotationPlace)> &p_on_annotation = nullptr);

// Parses a single expression, such as a default value from the engine's API;
// null after an error. Its token indices refer to a file of its own.
std::unique_ptr<Expr> parse_expression(const std::string &p_source, std::vector<Diagnostic> &r_errors);

} // namespace petrichor::compiler
