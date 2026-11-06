#pragma once

#include "compiler/ast.h"
#include "compiler/compiler.h"
#include "compiler/diagnostic.h"
#include "compiler/engine_api.h"
#include "compiler/references.h"

#include <vector>

namespace petrichor::compiler {

class ScriptClasses;
struct Completion;

// Resolves names and types and fills the checker fields of the AST, and with
// `r_references` records where each name is resolved (architecture §2.1).
// Other scripts of this language come from `r_classes`. With `r_completion`,
// gives what can be written at the cursor that U+FFFF marks (spec §13.2),
// and of the function bodies checks only the one around it.
void check(ClassDecl &r_class, const EngineApi &p_engine, const CompileOptions &p_options, std::vector<Diagnostic> &r_errors, ScriptClasses &r_classes, bool p_check_bodies = true, std::vector<Reference> *r_references = nullptr, Completion *r_completion = nullptr);

} // namespace petrichor::compiler
