#pragma once

#include "compiler/ast.h"
#include "compiler/compiler.h"
#include "compiler/diagnostic.h"
#include "compiler/engine_api.h"
#include "compiler/references.h"

#include <vector>

namespace petrichor::compiler {

class ScriptClasses;

// Resolves names and types and fills the checker fields of the AST, and with
// `r_references` records where each name is resolved (architecture §2.1),
// with `r_warnings` the warnings, which @warning_ignore has not filtered yet.
// Other scripts of this language come from `r_classes`.
void check(ClassDecl &r_class, const EngineApi &p_engine, const CompileOptions &p_options, std::vector<Diagnostic> &r_errors, ScriptClasses &r_classes, bool p_check_bodies = true, std::vector<Reference> *r_references = nullptr, std::vector<Diagnostic> *r_warnings = nullptr);

} // namespace petrichor::compiler
