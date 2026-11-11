#pragma once

#include "compiler/ast.h"
#include "compiler/compiler.h"
#include "compiler/diagnostic.h"
#include "compiler/engine_api.h"
#include "compiler/references.h"

#include <functional>
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

// Each literal node path in the code of a class, outside its inner classes
// and static functions, before types are known (spec §7.1): `$X`, and the
// call of get_node() or get_node_or_null() of one String on self.
void for_each_node_path(const ClassDecl &p_class, const std::function<void(const Expr &)> &p_fn);

// The annotations of the @export family that a member may take (spec §2.3),
// each with whether it takes arguments.
std::vector<std::pair<std::string, bool>> export_annotations();

} // namespace petrichor::compiler
