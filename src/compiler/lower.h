#pragma once

#include "bytecode/module.h"
#include "compiler/ast.h"

namespace petrichor::compiler {

// Turns a checked class without errors into a module: class metadata, constant
// and call tables, and each function lowered to IR and emitted.
bytecode::Module lower(const ClassDecl &p_class);

} // namespace petrichor::compiler
