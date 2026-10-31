#pragma once

#include "bytecode/module.h"
#include "compiler/ast.h"
#include "compiler/diagnostic.h"

#include <functional>
#include <vector>

namespace petrichor::compiler {

// The declaration of a struct of another script, with its fields checked;
// null when that script has errors.
using StructLookup = std::function<const StructDecl *(const Type &)>;

// Turns a checked class without errors into a module: class metadata, constant
// and call tables, and each function lowered to IR and emitted. With
// `p_declarations_only`, functions get no code and the module cannot run. What
// the checker passed but lowering has no code for is an internal error in
// `r_errors`, and the module is not valid then.
bytecode::Module lower(const ClassDecl &p_class, bool p_declarations_only, std::vector<Diagnostic> &r_errors, const StructLookup &p_structs = {});

} // namespace petrichor::compiler
