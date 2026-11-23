#include "compiler/compiler.h"

#include "compiler/checker.h"
#include "compiler/lexer.h"
#include "compiler/lower.h"
#include "compiler/parser.h"
#include "compiler/script_classes.h"
#include "compiler/warnings.h"
#include "compiler_id.gen.h"

#include <algorithm>

namespace petrichor::compiler {

CompileResult compile(const std::string &p_source, const EngineApi &p_engine, const CompileOptions &p_options) {
	ScriptClasses classes(p_engine, p_options.project, p_options.mode == CheckMode::Declarations);
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
	check(r_class, p_engine, p_options, parsed ? result.errors : unreported, r_classes, &result.references, parsed ? &result.warnings : nullptr, nullptr, &result.node_paths);
	if (parsed) {
		apply_warning_ignores(r_class, result.warnings);
	}
	for (std::vector<Diagnostic> *diagnostics : { &result.errors, &result.warnings }) {
		std::stable_sort(diagnostics->begin(), diagnostics->end(), [](const Diagnostic &a, const Diagnostic &b) {
			return a.begin < b.begin;
		});
	}
	if (result.ok()) {
		const StructLookup structs = [&](const Type &p_struct) -> const StructDecl * {
			const ClassDecl *decl = r_classes.declarations(p_struct.script);
			const StructDecl *found = decl ? decl->find_struct(p_struct.interface_name()) : nullptr;
			return found && !r_classes.errors_in(found) ? found : nullptr;
		};
		result.module = lower(r_class, p_options.mode == CheckMode::Declarations, result.errors, structs);
		const uint64_t source_hash = bytecode::hash_source(p_source);
		const auto hash = [&](bytecode::Module &r_module, const auto &p_self) -> void {
			r_module.source_hash = source_hash;
			for (bytecode::Module &inner : r_module.inner_classes) {
				p_self(inner, p_self);
			}
		};
		hash(result.module, hash);
		result.module.compiler_id = COMPILER_ID;
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

uint64_t compiler_id() {
	return COMPILER_ID;
}

std::map<std::string, std::vector<CheckedEdit>> fixed_together(const std::vector<std::pair<const Fix *, std::string>> &p_fixes, std::vector<const Fix *> &r_applied) {
	std::map<std::string, std::vector<CheckedEdit>> kept;
	const auto clash = [](const CheckedEdit &p_a, const CheckedEdit &p_b) { return p_a.begin == p_b.begin || (p_a.begin < p_b.end && p_b.begin < p_a.end); };
	for (const auto &[fix, file] : p_fixes) {
		const bool clashes = std::any_of(fix->edits.begin(), fix->edits.end(), [&](const Fix::Edit &p_edit) {
			const auto found = kept.find(p_edit.file.empty() ? file : p_edit.file);
			return found != kept.end() && std::any_of(found->second.begin(), found->second.end(), [&](const CheckedEdit &p_kept) { return clash(p_edit.change, p_kept); });
		});
		if (clashes) {
			continue;
		}
		for (const Fix::Edit &edit : fix->edits) {
			kept[edit.file.empty() ? file : edit.file].push_back(edit.change);
		}
		r_applied.push_back(fix);
	}
	for (auto &[file, edits] : kept) {
		std::sort(edits.begin(), edits.end(), [](const CheckedEdit &p_a, const CheckedEdit &p_b) { return p_a.begin < p_b.begin; });
	}
	return kept;
}

std::vector<CheckedEdit> fix_all(const CompileResult &p_result, FixKind p_kind) {
	std::vector<std::pair<const Fix *, std::string>> fixes;
	for (const Diagnostic &error : p_result.errors) {
		for (const Fix &fix : error.fixes) {
			if (fix.kind == p_kind && fix.automatic() && error.file.empty() && std::all_of(fix.edits.begin(), fix.edits.end(), [](const Fix::Edit &p_edit) { return p_edit.file.empty(); })) {
				fixes.emplace_back(&fix, std::string());
			}
		}
	}
	const auto first = [](const Fix *p_fix) {
		return std::min_element(p_fix->edits.begin(), p_fix->edits.end(), [](const Fix::Edit &p_a, const Fix::Edit &p_b) { return p_a.change.begin < p_b.change.begin; })->change.begin;
	};
	std::stable_sort(fixes.begin(), fixes.end(), [&](const auto &p_a, const auto &p_b) { return first(p_a.first) < first(p_b.first); });
	std::vector<const Fix *> applied;
	std::map<std::string, std::vector<CheckedEdit>> edits = fixed_together(fixes, applied);
	return edits[std::string()];
}

} // namespace petrichor::compiler
