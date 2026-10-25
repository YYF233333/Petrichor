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
void check(ClassDecl &r_class, const EngineApi &p_engine, const CompileOptions &p_options, std::vector<Diagnostic> &r_errors, ScriptClasses &r_classes);

// How classes relate: engine classes as ClassDB says, classes of this
// language as their declarations say.
class ClassRelation {
public:
	virtual ~ClassRelation() = default;
	// Whether every object of class `p_from` is a `p_to`; nullability aside.
	virtual bool is_a(const Type &p_from, const Type &p_to) const = 0;
};

// Whether a value of type `p_from` can be stored where `p_to` is expected:
// the same type, int to float (spec §3.2), String to and from StringName and
// NodePath (§10.7), an object to a nullable or a base class, or anything to
// Variant.
bool is_assignable(const Type &p_from, const Type &p_to, const ClassRelation &p_classes);

} // namespace petrichor::compiler
