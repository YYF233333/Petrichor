#include "conventions.h"

#include "compiler/checker.h"
#include "compiler/formatter.h"
#include "compiler/parser.h"
#include "compiler/script_classes.h"
#include "doctest.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>

namespace petrichor::tests {

using namespace compiler;

namespace {

void check_lossless(const SourceFile &p_file) {
	std::vector<const Token *> all;
	for (const Token &token : p_file.tokens) {
		all.push_back(&token);
	}
	for (const Token &token : p_file.trivia) {
		all.push_back(&token);
	}
	std::stable_sort(all.begin(), all.end(), [](const Token *a, const Token *b) { return a->offset < b->offset; });
	uint32_t at = 0;
	for (const Token *token : all) {
		REQUIRE_MESSAGE(token->offset >= at, "tokens overlap at ", token->offset, " in:\n", p_file.text);
		for (; at < token->offset; at++) {
			const char c = p_file.text[at];
			REQUIRE_MESSAGE((c == ' ' || c == '\t' || c == '\r' || c == '\n'), "byte ", at, " is in no token in:\n", p_file.text);
		}
		at = token->end();
		// A word; && and || spell and and or without text.
		if (token->kind == TokenKind::Identifier || (token->kind >= TokenKind::Async && token->kind <= TokenKind::While && !token->text.empty())) {
			CHECK(p_file.text.substr(token->offset, token->length) == token->text);
		}
	}
	REQUIRE(at <= p_file.text.size());
	for (; at < p_file.text.size(); at++) {
		const char c = p_file.text[at];
		REQUIRE_MESSAGE((c == ' ' || c == '\t' || c == '\r' || c == '\n'), "byte ", at, " is in no token in:\n", p_file.text);
	}
}

class RangeChecker {
	const SourceFile &file;

	// Within `p_parent`, after the sibling that ends at `r_cursor`.
	void enter(const TokenRange &p_range, const TokenRange &p_parent, int64_t &r_cursor) {
		REQUIRE_MESSAGE(p_range.first <= p_range.last, "in:\n", file.text);
		REQUIRE_MESSAGE(p_range.last < file.tokens.size(), "in:\n", file.text);
		CHECK_MESSAGE(p_range.first >= p_parent.first, "a node begins before its parent in:\n", file.text);
		CHECK_MESSAGE(p_range.last <= p_parent.last, "a node ends after its parent in:\n", file.text);
		// Error nodes that a line left empty share the line break it ends with.
		const bool shared = int64_t(p_range.first) == r_cursor && file.tokens[p_range.first].length == 0;
		CHECK_MESSAGE((int64_t(p_range.first) > r_cursor || shared), "a node begins inside the sibling before it in:\n", file.text);
		r_cursor = p_range.last;
	}

	// A name a syntax error left empty has no token of its own.
	void name(uint32_t p_token, const std::string &p_name, const TokenRange &p_range) {
		if (p_name.empty()) {
			return;
		}
		REQUIRE(p_token < file.tokens.size());
		CHECK_MESSAGE(p_token >= p_range.first, p_name);
		CHECK_MESSAGE(p_token <= p_range.last, p_name);
		CHECK(file.tokens[p_token].text == p_name);
	}

	template <typename P>
	void children(const P &p_parent, const TokenRange &p_range) {
		int64_t cursor = int64_t(p_range.first) - 1;
		for_each_child(p_parent, [&](const auto &p_child, bool p_new_group) {
			if (p_new_group) {
				cursor = int64_t(p_range.first) - 1;
			}
			visit(p_child, p_range, cursor);
		});
	}

	template <typename T>
	void visit(const T &p_node, const TokenRange &p_parent, int64_t &r_cursor) {
		enter(p_node.range, p_parent, r_cursor);
		if constexpr (std::is_same_v<T, TypeRef>) {
			name(p_node.name_token, p_node.name.substr(0, p_node.name.find('.')), p_node.range);
		} else if constexpr (std::is_same_v<T, Expr>) {
			if (p_node.kind == ExprKind::Name || p_node.kind == ExprKind::Member || p_node.kind == ExprKind::Call) {
				name(p_node.name_token, p_node.text, p_node.range);
			} else if (p_node.kind == ExprKind::Unary || p_node.kind == ExprKind::Binary) {
				CHECK(p_node.op_token >= p_node.range.first);
				CHECK(p_node.op_token <= p_node.range.last);
			}
			for (size_t i = 0; i < p_node.arg_names.size(); i++) {
				name(p_node.arg_name_tokens[i], p_node.arg_names[i], p_node.range);
			}
		} else if constexpr (std::is_same_v<T, Pattern>) {
			if (p_node.kind == PatternKind::Bind) {
				name(p_node.name_token, p_node.name, p_node.range);
			}
		} else if constexpr (std::is_same_v<T, Stmt>) {
			if (p_node.kind == StmtKind::Var || p_node.kind == StmtKind::Const || p_node.kind == StmtKind::For) {
				name(p_node.name_token, p_node.name, p_node.range);
			}
		} else if constexpr (std::is_same_v<T, FuncDecl>) {
			// An inline accessor, @set_x or @get_x, is named by its keyword.
			name(p_node.name_token, p_node.name.rfind('@', 0) == 0 ? p_node.name.substr(1, 3) : p_node.name, p_node.range);
		} else if constexpr (!std::is_same_v<T, MatchBranch>) {
			name(p_node.name_token, p_node.name, p_node.range);
		}
		children(p_node, p_node.range);
	}

public:
	explicit RangeChecker(const SourceFile &p_file) :
			file(p_file) {}

	void run(const ClassDecl &p_class) {
		children(p_class, { 0, uint32_t(file.tokens.size() - 1) });
	}
};

// Of the line at index `p_line`.
int indentation(const SourceFile &p_file, int p_line) {
	uint32_t at = p_file.line_starts[p_line];
	int width = 0;
	while (at < p_file.text.size() && (p_file.text[at] == ' ' || p_file.text[at] == '\t')) {
		at++;
		width++;
	}
	return width;
}

// Whether the line at index `p_line` has no code.
bool blank(const SourceFile &p_file, int p_line) {
	uint32_t at = p_file.line_starts[p_line] + uint32_t(indentation(p_file, p_line));
	return at >= p_file.text.size() || p_file.text[at] == '\n' || p_file.text[at] == '\r' || p_file.text[at] == '#';
}

// Where a statement begins, and the lines of the lambdas around it.
struct StatementStart {
	int line;
	uint32_t begin;
	std::vector<int> lambdas;
};

// Of functions and of the lambdas in them and in members.
class StatementStarts {
	const SourceFile &file;
	std::vector<int> lambdas;

	template <typename T>
	void visit(const T &p_node) {
		if constexpr (std::is_same_v<T, Stmt>) {
			const uint32_t begin = file.begin(p_node.range);
			starts.push_back({ file.position(begin).line, begin, lambdas });
		} else if constexpr (std::is_same_v<T, FuncDecl>) {
			if (p_node.is_lambda) {
				lambdas.push_back(file.position(file.begin(p_node.range)).line);
				for_each_child(p_node, [&](const auto &p_child) { visit(p_child); });
				lambdas.pop_back();
				return;
			}
		}
		for_each_child(p_node, [&](const auto &p_child) { visit(p_child); });
	}

public:
	std::vector<StatementStart> starts;

	explicit StatementStarts(const ClassDecl &p_class) :
			file(*p_class.source) {
		visit(p_class);
	}
};

// The spelling a reference must have, or "" where the declaration does not
// say: a class of this language may be named by its global name or its path,
// and a local by whatever its declaration spells.
std::string spelling(const DeclId &p_id) {
	switch (p_id.kind) {
		case DeclId::Kind::Script:
		case DeclId::Kind::Foreign:
			return p_id.name.substr(p_id.name.find('.') + 1);
		case DeclId::Kind::Engine:
		case DeclId::Kind::Language:
			return p_id.name.empty() ? p_id.scope : p_id.name;
		case DeclId::Kind::Node:
			return p_id.name.substr(p_id.name.find_last_of('/') + 1);
		case DeclId::Kind::Autoload:
		case DeclId::Kind::Unresolved:
			return p_id.name;
		case DeclId::Kind::Local:
			break;
	}
	return std::string();
}

// The names that a checked class resolves or declares, which must all have a
// reference.
class ResolvedNames {
	const SourceFile &file;
	std::vector<std::pair<uint32_t, std::string>> names; // Token and what it is.

	void add(uint32_t p_token, const std::string &p_name, const char *p_what) {
		if (p_token < file.tokens.size() && file.tokens[p_token].length > 0 && !p_name.empty() && p_name[0] != '@') {
			names.push_back({ p_token, std::string(p_what) + " " + p_name });
		}
	}

	template <typename T>
	void visit(const T &p_node) {
		if constexpr (std::is_same_v<T, Expr>) {
			if ((p_node.kind == ExprKind::Name || p_node.kind == ExprKind::Member || p_node.kind == ExprKind::Call) && p_node.target != Target::None && p_node.target != Target::NoneComparison) {
				add(p_node.name_token, p_node.text, "use of");
			}
		} else if constexpr (std::is_same_v<T, Stmt>) {
			if (p_node.kind == StmtKind::Var || p_node.kind == StmtKind::Const || p_node.kind == StmtKind::For) {
				add(p_node.name_token, p_node.name, "local");
			}
		} else if constexpr (std::is_same_v<T, Pattern>) {
			if (p_node.kind == PatternKind::Bind) {
				add(p_node.name_token, p_node.name, "local");
			}
		} else if constexpr (std::is_same_v<T, FuncDecl>) {
			if (!p_node.is_lambda) {
				add(p_node.name_token, p_node.name, "function");
			}
			if (p_node.has_body || p_node.is_abstract) {
				for (const ParamDecl &param : p_node.params) {
					add(param.name_token, param.name, "parameter");
				}
			}
		} else if constexpr (!std::is_same_v<T, TypeRef> && !std::is_same_v<T, MatchBranch> && !std::is_same_v<T, Annotation> && !std::is_same_v<T, ParamDecl>) {
			add(p_node.name_token, p_node.name, "declaration");
		}
		for_each_child(p_node, [&](const auto &p_child) { visit(p_child); });
	}

public:
	ResolvedNames(const ClassDecl &p_class, const SourceFile &p_file) :
			file(p_file) {
		visit(p_class);
	}

	const std::vector<std::pair<uint32_t, std::string>> &all() const { return names; }
};

} // namespace

void check_references(const std::string &p_source, const EngineApi &p_engine, const CompileOptions &p_options) {
	std::vector<Diagnostic> errors;
	ClassDecl cls = parse(p_source, errors);
	ScriptClasses classes(p_engine, p_options.project, p_options.declarations_only);
	std::vector<Reference> references;
	check(cls, p_engine, p_options, errors, classes, true, &references);
	REQUIRE(errors.empty());
	const SourceFile &file = *cls.source;
	std::map<DeclId, std::string> locals;
	std::set<std::pair<uint32_t, uint32_t>> places;
	for (const Reference &reference : references) {
		if (!reference.scene.empty()) {
			continue;
		}
		const std::string text = file.text.substr(reference.begin, reference.end - reference.begin);
		places.insert({ reference.begin, reference.end });
		if (reference.target.kind == DeclId::Kind::Local) {
			const auto [entry, added] = locals.insert({ reference.target, text });
			CHECK_MESSAGE(entry->second == text, "a local spelled ", entry->second, " is referred to as ", text, " in:\n", p_source);
			continue;
		}
		const std::string expected = spelling(reference.target);
		CHECK_MESSAGE((expected.empty() || expected == text), "a reference to ", reference.target.scope, " ", reference.target.name, " is spelled ", text, " in:\n", p_source);
	}
	if (p_options.declarations_only) {
		return; // Without bodies, whose names are not resolved.
	}
	const ResolvedNames resolved(cls, file);
	for (const auto &[token, what] : resolved.all()) {
		CHECK_MESSAGE(places.count({ file.tokens[token].offset, file.tokens[token].end() }), "the ", what, " has no reference in:\n", p_source);
	}
}

void check_formatting(const std::string &p_source) {
	std::vector<Diagnostic> errors;
	parse(p_source, errors);
	if (!errors.empty()) {
		return;
	}
	std::string error;
	const std::optional<std::string> formatted = format(p_source, &error);
	CHECK_MESSAGE(formatted, error, " Formatting:\n", p_source);
	if (formatted) {
		CHECK_MESSAGE(format(*formatted) == formatted, "formatting again changes:\n", *formatted);
	}
}

void check_recovery(const std::string &p_source, const std::function<void(const std::string &)> &p_compile) {
	std::vector<Diagnostic> errors;
	const ClassDecl original = parse(p_source, errors);
	if (!errors.empty()) {
		return;
	}
	const SourceFile &file = *original.source;
	const std::vector<StatementStart> starts = StatementStarts(original).starts;
	for (size_t line = 0; line < file.line_starts.size(); line++) {
		const uint32_t start = file.line_starts[line];
		uint32_t end = line + 1 < file.line_starts.size() ? file.line_starts[line + 1] - 1 : uint32_t(file.text.size());
		if (end > start && file.text[end - 1] == '\r') {
			end--;
		}
		if (end - start < 2) {
			continue;
		}
		const uint32_t cut = start + (end - start) / 2;
		const std::string truncated = file.text.substr(0, cut) + file.text.substr(end);
		INFO("line ", line + 1, " cut in:\n", truncated);
		if (p_compile) {
			p_compile(truncated);
		}
		check_syntax_conventions(truncated);
		std::vector<Diagnostic> ignored;
		std::set<uint32_t> found;
		const ClassDecl recovered = parse(truncated, ignored);
		for (const StatementStart &entry : StatementStarts(recovered).starts) {
			found.insert(entry.begin);
		}
		// The lines after a bracket left open that are indented deeper than its
		// own may be read as inside it, as the body under `func f(` is.
		int exempt_from = 0;
		int exempt_to = 0;
		SourceFile plain;
		plain.text = truncated;
		const std::vector<uint32_t> open = lex(plain, {}, {}, ignored);
		if (!open.empty()) {
			exempt_from = plain.position(open[0]).line + 1;
			const int depth = indentation(plain, exempt_from - 2);
			exempt_to = exempt_from;
			while (exempt_to <= int(plain.line_starts.size()) && (blank(plain, exempt_to - 1) || indentation(plain, exempt_to - 1) > depth)) {
				exempt_to++;
			}
		}
		for (const StatementStart &statement : starts) {
			// A lambda whose head is cut leaves its body to the code around it.
			const bool headless = std::find(statement.lambdas.begin(), statement.lambdas.end(), int(line + 1)) != statement.lambdas.end();
			if (statement.line != int(line + 1) && !headless && (statement.line < exempt_from || statement.line >= exempt_to)) {
				const uint32_t begin = statement.begin;
				CHECK_MESSAGE(found.count(begin < cut ? begin : begin - (end - cut)), "the statement of line ", statement.line, " is lost");
			}
		}
	}
}

void check_syntax_conventions(const std::string &p_source) {
	std::vector<Diagnostic> errors;
	const ClassDecl cls = parse(p_source, errors);
	check_lossless(*cls.source);
	RangeChecker(*cls.source).run(cls);
}

} // namespace petrichor::tests
