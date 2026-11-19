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
// Other scripts of this language come from `r_classes`. With `r_warnings`,
// the warnings, which @warning_ignore has not filtered yet. In
// CheckMode::Cursor, `r_completion` takes what can be written at the cursor.
// `r_node_paths` records where the literal node paths lead.
void check(ClassDecl &r_class, const EngineApi &p_engine, const CompileOptions &p_options, std::vector<Diagnostic> &r_errors, ScriptClasses &r_classes, std::vector<Reference> *r_references = nullptr, std::vector<Diagnostic> *r_warnings = nullptr, Completion *r_completion = nullptr,
		std::vector<NodePathWalk> *r_node_paths = nullptr);

// Each literal node path in the code of a class, outside its inner classes
// and static functions, before types are known (spec §7.1): `$X`, and the
// call of get_node() or get_node_or_null() of one String on self.
void for_each_node_path(const ClassDecl &p_class, const std::function<void(const Expr &)> &p_fn);

// The parts of a node path between its "/", and the path of a node's parent
// from the root, "." for a child of the root.
std::vector<std::string> path_parts(const std::string &p_path);
std::string parent_path(const std::string &p_path);
// Whether `$` takes a path without quotes: names joined by "/", each
// possibly after "%".
bool plain_path(const std::string &p_path);
// A node path in a String, quoted with `p_quote`.
std::string quoted(const std::string &p_path, char p_quote);

// The annotations of the @export family that a member may take (spec §2.3),
// each with whether it takes arguments.
std::vector<std::pair<std::string, bool>> export_annotations();

namespace builtin {
struct Argument;
}

// A signature as completion's call hint writes it (spec §13.2):
// `name(a: T, b: U = 1) -> R`, the parameter at `p_current` between two
// U+FFFF, none for -1, and without a result, a signal's. With `p_vararg`, a
// last `...args` takes the rest.
std::string signature(const std::string &p_name, std::vector<std::string> p_params, bool p_vararg, int p_current, const std::string &p_result);
// A function of this language's, its defaults as written in `p_source`.
std::string declared_signature(const std::string &p_name, const FuncDecl &p_function, const SourceFile &p_source, int p_current, const Type &p_result);
// A function of the engine's API (builtin_api.h).
std::string api_signature(const std::string &p_name, const builtin::Argument *p_args, uint32_t p_count, bool p_vararg, const char *p_result, int p_current);
// A method of an engine class, which takes `p_params`.
std::string engine_signature(const std::string &p_name, const EngineMethod &p_method, const std::vector<Type> &p_params, int p_current, const std::string &p_result);

// What an engine method takes at `p_index`: an object, unless the engine
// requires one (spec §10.2), may be None.
Type engine_param(const EngineMethod &p_method, size_t p_index);
// What the engine returns: T? unless the binding metadata says it is null
// only when misused (spec §10.2), which a check at run time reports.
Type engine_result(const EngineMethod &p_method);
// What an engine signal passes, an object only where the audit says it may
// pass null being T? (spec §4.7, §10.2). A script of another language, the
// owner by its path, passes T? as reflected (§11.1).
Type signal_type(const EngineMethod &p_signal);

} // namespace petrichor::compiler
