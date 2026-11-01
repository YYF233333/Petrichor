#include "compiler/compiler.h"

#include "compiler/checker.h"
#include "compiler/lexer.h"
#include "compiler/lower.h"
#include "compiler/parser.h"
#include "compiler/script_classes.h"

#include <algorithm>

namespace petrichor::compiler {

CompileResult compile(const std::string &p_source, const EngineApi &p_engine, const CompileOptions &p_options) {
	CompileResult result;
	ClassDecl cls = parse(p_source, result.errors);
	ScriptClasses classes(p_engine, p_options.project, p_options.declarations_only);
	// With syntax errors only they are reported, but the checker runs on the
	// recovered tree all the same, as the tools will (architecture §2.1).
	std::vector<Diagnostic> unreported;
	check(cls, p_engine, p_options, result.errors.empty() ? result.errors : unreported, classes, true, &result.references);
	std::stable_sort(result.errors.begin(), result.errors.end(), [](const Diagnostic &a, const Diagnostic &b) {
		return a.begin < b.begin;
	});
	if (result.errors.empty()) {
		const StructLookup structs = [&](const Type &p_struct) -> const StructDecl * {
			const ScriptClass *script = classes.declarations(p_struct.script);
			return script ? script->decl.find_struct(p_struct.interface_name()) : nullptr;
		};
		result.module = lower(cls, p_options.declarations_only, result.errors, structs);
		result.module.source_hash = bytecode::hash_source(p_source);
	}
	for (Diagnostic &error : result.errors) {
		const SourcePosition position = cls.source->position(error.begin);
		error.line = position.line;
		error.column = position.column;
	}
	return result;
}

} // namespace petrichor::compiler
