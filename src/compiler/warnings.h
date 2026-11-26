#pragma once

#include "compiler/ast.h"
#include "compiler/diagnostic.h"

#include <vector>

namespace petrichor::compiler {

// Drops the warnings that the file's @warning_ignore annotations ignore, as
// GDScript does by lines (spec §2.3), and adds a hint for each name that
// ignores none.
void apply_warning_ignores(const ClassDecl &p_class, std::vector<Diagnostic> &r_warnings);

// Whether @warning_ignore takes the name for a warning or a hint of this
// language (spec §2.3).
bool is_warning_name(const std::string &p_name);

} // namespace petrichor::compiler
