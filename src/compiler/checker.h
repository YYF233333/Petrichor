#pragma once

#include "compiler/ast.h"
#include "compiler/diagnostic.h"
#include "compiler/engine_api.h"

#include <vector>

namespace petrichor::compiler {

// Resolves names and types and fills the checker fields of the AST.
void check(ClassDecl &r_class, const EngineApi &p_engine, std::vector<Diagnostic> &r_errors);

// Whether a value of type `p_from` can be stored where `p_to` is expected:
// the same type, int to float (spec §3.2), String to and from StringName and
// NodePath (§10.7), an object to a nullable or a base class, or anything to
// Variant.
bool is_assignable(const Type &p_from, const Type &p_to, const EngineApi &p_engine);

} // namespace petrichor::compiler
