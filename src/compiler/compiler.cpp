#include "compiler/compiler.h"

#include "compiler/checker.h"
#include "compiler/lexer.h"
#include "compiler/lower.h"
#include "compiler/parser.h"

#include <algorithm>

namespace petrichor::compiler {

CompileResult compile(const std::string &p_source, const EngineApi &p_engine) {
	CompileResult result;
	const std::vector<Token> tokens = tokenize(p_source, result.errors);
	ClassDecl cls = parse(tokens, result.errors);
	if (result.errors.empty()) {
		check(cls, p_engine, result.errors);
	}
	std::stable_sort(result.errors.begin(), result.errors.end(), [](const Diagnostic &a, const Diagnostic &b) {
		return a.line != b.line ? a.line < b.line : a.column < b.column;
	});
	if (result.errors.empty()) {
		result.module = lower(cls);
		result.module.source_hash = bytecode::hash_source(p_source);
	}
	return result;
}

} // namespace petrichor::compiler
