#pragma once

#include "compiler/ast.h"
#include "compiler/compiler.h"
#include "compiler/diagnostic.h"
#include "compiler/engine_api.h"

#include <vector>

namespace petrichor::compiler {

class ScriptClasses;

// Resolves names and types and fills the checker fields of the AST. Other
// scripts of this language come from `r_classes`.
void check(ClassDecl &r_class, const EngineApi &p_engine, const CompileOptions &p_options, std::vector<Diagnostic> &r_errors, ScriptClasses &r_classes, bool p_check_bodies = true);

} // namespace petrichor::compiler
