#include "compiler/compiler.h"

#include "compiler/checker.h"
#include "compiler/lexer.h"
#include "compiler/lower.h"
#include "compiler/parser.h"
#include "compiler/script_classes.h"
#include "compiler/warnings.h"

#include <algorithm>

namespace petrichor::compiler {

CompileResult compile(const std::string &p_source, const EngineApi &p_engine, const CompileOptions &p_options) {
	ScriptClasses classes(p_engine, p_options.project, p_options.declarations_only);
	ClassDecl cls;
	return compile(p_source, p_engine, p_options, classes, cls);
}

CompileResult compile(const std::string &p_source, const EngineApi &p_engine, const CompileOptions &p_options, ScriptClasses &r_classes, ClassDecl &r_class) {
	CompileResult result;
	r_class = parse(p_source, result.errors);
	// With syntax errors only they are reported, but the checker runs on the
	// recovered tree all the same, as the tools will (architecture §2.1).
	std::vector<Diagnostic> unreported;
	const bool parsed = result.errors.empty();
	check(r_class, p_engine, p_options, parsed ? result.errors : unreported, r_classes, true, &result.references, parsed ? &result.warnings : nullptr);
	if (parsed) {
		apply_warning_ignores(r_class, result.warnings);
	}
	for (std::vector<Diagnostic> *diagnostics : { &result.errors, &result.warnings }) {
		std::stable_sort(diagnostics->begin(), diagnostics->end(), [](const Diagnostic &a, const Diagnostic &b) {
			return a.begin < b.begin;
		});
	}
	if (result.errors.empty()) {
		const StructLookup structs = [&](const Type &p_struct) -> const StructDecl * {
			const ClassDecl *decl = r_classes.declarations(p_struct.script);
			const StructDecl *found = decl ? decl->find_struct(p_struct.interface_name()) : nullptr;
			return found && !r_classes.errors_in(found) ? found : nullptr;
		};
		result.module = lower(r_class, p_options.declarations_only, result.errors, structs);
		const uint64_t source_hash = bytecode::hash_source(p_source);
		const auto hash = [&](bytecode::Module &r_module, const auto &p_self) -> void {
			r_module.source_hash = source_hash;
			for (bytecode::Module &inner : r_module.inner_classes) {
				p_self(inner, p_self);
			}
		};
		hash(result.module, hash);
	}
	for (std::vector<Diagnostic> *diagnostics : { &result.errors, &result.warnings }) {
		for (Diagnostic &diagnostic : *diagnostics) {
			const SourcePosition position = r_class.source->position(diagnostic.begin);
			diagnostic.line = position.line;
			diagnostic.column = position.column;
		}
	}
	return result;
}

} // namespace petrichor::compiler
