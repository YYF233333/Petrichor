#include "compiler/warnings.h"

#include "compiler/lexer.h"

#include <climits>
#include <map>
#include <string>

namespace petrichor::compiler {

namespace {

const char *const UNUSED_IGNORE = "unused_ignore";

// The warnings and hints of this language, which @warning_ignore takes by
// name (spec §2.3).
const char *const NAMES[] = {
	"modified_copy", // §3.6; off until the IR has liveness (BACKLOG).
	UNUSED_IGNORE,
};

bool is_name(const std::string &p_name) {
	for (const char *name : NAMES) {
		if (p_name == name) {
			return true;
		}
	}
	return false;
}

// The last token of the head of the statement or declaration after the
// annotation that ends at `p_last`, past line breaks and other annotations:
// the line break that ends the line it starts on, as line breaks inside
// brackets or after `\` are no tokens.
uint32_t head_end(const std::vector<Token> &p_tokens, uint32_t p_last) {
	const uint32_t count = uint32_t(p_tokens.size());
	uint32_t at = p_last + 1;
	for (;;) {
		while (at < count && p_tokens[at].kind == TokenKind::Newline) {
			at++;
		}
		if (at >= count || p_tokens[at].kind != TokenKind::At) {
			break;
		}
		at += 2; // `@name`.
		for (int depth = 0; at < count && (depth > 0 || p_tokens[at].kind == TokenKind::ParenOpen); at++) {
			if (p_tokens[at].kind == TokenKind::ParenOpen) {
				depth++;
			} else if (p_tokens[at].kind == TokenKind::ParenClose && --depth == 0) {
				at++;
				break;
			}
		}
	}
	while (at < count && p_tokens[at].kind != TokenKind::Newline && p_tokens[at].kind != TokenKind::EndOfFile) {
		at++;
	}
	return at < count ? at : count - 1;
}

// The lines one name of a @warning_ignore reaches.
struct Ignore {
	std::string name;
	int first_line;
	int last_line;
	const Expr *arg;
	bool used = false;
};

} // namespace

void apply_warning_ignores(const ClassDecl &p_class, std::vector<Diagnostic> &r_warnings) {
	const SourceFile &source = *p_class.source;
	std::vector<Ignore> ignores;
	std::map<std::string, size_t> started;
	for (const Annotation &annotation : p_class.warning_ignores) {
		const int line = source.line(annotation.range.first);
		for (const std::unique_ptr<Expr> &arg : annotation.args) {
			if (arg->kind != ExprKind::StringLiteral) {
				continue; // An error of its own.
			}
			const std::string &name = arg->text;
			if (annotation.name == "warning_ignore") {
				ignores.push_back({ name, line, source.line(head_end(source.tokens, annotation.range.last)), arg.get() });
			} else if (annotation.name == "warning_ignore_start") {
				if (!started.count(name)) {
					started[name] = ignores.size();
					ignores.push_back({ name, line, INT_MAX, arg.get() });
				}
			} else {
				const auto found = started.find(name);
				if (found != started.end()) {
					ignores[found->second].last_line = line;
					started.erase(found);
				}
			}
		}
	}
	// Whether an ignore takes the diagnostic, which then counts as used.
	const auto ignored = [&](const Diagnostic &p_diagnostic) {
		const int line = source.position(p_diagnostic.begin).line;
		bool taken = false;
		for (Ignore &ignore : ignores) {
			if (ignore.name == p_diagnostic.warning && line >= ignore.first_line && line <= ignore.last_line) {
				ignore.used = true;
				taken = true;
			}
		}
		return taken;
	};
	std::vector<Diagnostic> kept;
	for (Diagnostic &warning : r_warnings) {
		if (!ignored(warning)) {
			kept.push_back(std::move(warning));
		}
	}
	// A hint for each unused ignore, those of other names first, since an
	// ignore of unused_ignore is used once it takes one of their hints.
	for (const bool of_hints : { false, true }) {
		for (size_t i = 0; i < ignores.size(); i++) {
			const Ignore &ignore = ignores[i];
			if (ignore.used || (ignore.name == UNUSED_IGNORE) != of_hints) {
				continue;
			}
			Diagnostic hint{ source.begin(ignore.arg->range), source.end(ignore.arg->range) };
			hint.severity = Severity::Hint;
			hint.warning = UNUSED_IGNORE;
			if (is_name(ignore.name)) {
				hint.message = "This ignores no \"" + ignore.name + "\" " + (of_hints ? "hint." : "warning.");
			} else {
				hint.message = "\"" + ignore.name + "\" is not a warning or hint of this language, so this ignores nothing.";
			}
			if (of_hints || !ignored(hint)) {
				kept.push_back(std::move(hint));
			}
		}
	}
	r_warnings = std::move(kept);
}

} // namespace petrichor::compiler
