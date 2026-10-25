#pragma once

#include "bytecode/module.h"
#include "compiler/ast.h"

namespace petrichor::compiler {

// Turns a checked class without errors into a module: class metadata, constant
// and call tables, and each function lowered to IR and emitted. With
// `p_declarations_only`, functions get no code and the module cannot run.
bytecode::Module lower(const ClassDecl &p_class, bool p_declarations_only = false);

} // namespace petrichor::compiler
