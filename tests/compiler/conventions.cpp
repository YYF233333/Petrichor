#include "conventions.h"

#include "compiler/checker.h"
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

	void type(const TypeRef &p_type, const TokenRange &p_parent, int64_t &r_cursor) {
		enter(p_type.range, p_parent, r_cursor);
		name(p_type.name_token, p_type.name.substr(0, p_type.name.find('.')), p_type.range);
		int64_t cursor = int64_t(p_type.range.first) - 1;
		for (const TypeRef &arg : p_type.args) {
			type(arg, p_type.range, cursor);
		}
	}

	void expr(const Expr *p_expr, const TokenRange &p_parent, int64_t &r_cursor) {
		if (!p_expr) {
			return;
		}
		const Expr &e = *p_expr;
		enter(e.range, p_parent, r_cursor);
		int64_t cursor = int64_t(e.range.first) - 1;
		switch (e.kind) {
			case ExprKind::Name:
			case ExprKind::Member:
			case ExprKind::Call:
				name(e.name_token, e.text, e.range);
				break;
			case ExprKind::Unary:
			case ExprKind::Binary:
				CHECK(e.op_token >= e.range.first);
				CHECK(e.op_token <= e.range.last);
				break;
			default:
				break;
		}
		switch (e.kind) {
			case ExprKind::Conditional:
				expr(e.left.get(), e.range, cursor);
				expr(e.base.get(), e.range, cursor);
				expr(e.right.get(), e.range, cursor);
				return;
			case ExprKind::Cast:
			case ExprKind::TypeTest:
				expr(e.left.get(), e.range, cursor);
				type(e.cast_type, e.range, cursor);
				return;
			default:
				break;
		}
		expr(e.base.get(), e.range, cursor);
		expr(e.left.get(), e.range, cursor);
		expr(e.right.get(), e.range, cursor);
		for (size_t i = 0; i < e.args.size(); i++) {
			if (i < e.arg_names.size() && !e.arg_names[i].empty()) {
				name(e.arg_name_tokens[i], e.arg_names[i], e.range);
			}
			expr(e.args[i].get(), e.range, cursor);
		}
	}

	void block(const Block &p_block, const TokenRange &p_parent, int64_t &r_cursor) {
		for (const auto &stmt : p_block) {
			statement(*stmt, p_parent, r_cursor);
		}
	}

	void pattern(const Pattern &p_pattern, const TokenRange &p_parent, int64_t &r_cursor) {
		enter(p_pattern.range, p_parent, r_cursor);
		if (p_pattern.kind == PatternKind::Bind) {
			name(p_pattern.name_token, p_pattern.name, p_pattern.range);
		}
		int64_t cursor = int64_t(p_pattern.range.first) - 1;
		expr(p_pattern.value.get(), p_pattern.range, cursor);
		for (const Pattern &item : p_pattern.items) {
			pattern(item, p_pattern.range, cursor);
		}
	}

	void statement(const Stmt &p_stmt, const TokenRange &p_parent, int64_t &r_cursor) {
		enter(p_stmt.range, p_parent, r_cursor);
		const TokenRange &range = p_stmt.range;
		int64_t cursor = int64_t(range.first) - 1;
		if (p_stmt.kind == StmtKind::Var || p_stmt.kind == StmtKind::Const || p_stmt.kind == StmtKind::For) {
			name(p_stmt.name_token, p_stmt.name, range);
		}
		if (p_stmt.declared_type) {
			type(*p_stmt.declared_type, range, cursor);
		}
		expr(p_stmt.target.get(), range, cursor);
		expr(p_stmt.condition.get(), range, cursor);
		expr(p_stmt.value.get(), range, cursor);
		block(p_stmt.body, range, cursor);
		block(p_stmt.else_body, range, cursor);
		for (const MatchBranch &branch : p_stmt.branches) {
			enter(branch.range, range, cursor);
			int64_t inner = int64_t(branch.range.first) - 1;
			for (const Pattern &item : branch.patterns) {
				pattern(item, branch.range, inner);
			}
			expr(branch.guard.get(), branch.range, inner);
			block(branch.body, branch.range, inner);
		}
	}

	void annotation(const Annotation &p_annotation, const TokenRange &p_parent, int64_t &r_cursor) {
		enter(p_annotation.range, p_parent, r_cursor);
		name(p_annotation.name_token, p_annotation.name, p_annotation.range);
		int64_t cursor = int64_t(p_annotation.range.first) - 1;
		for (const auto &arg : p_annotation.args) {
			expr(arg.get(), p_annotation.range, cursor);
		}
	}

	void variable(const VarDecl &p_decl, const TokenRange &p_parent, int64_t &r_cursor) {
		enter(p_decl.range, p_parent, r_cursor);
		name(p_decl.name_token, p_decl.name, p_decl.range);
		int64_t cursor = int64_t(p_decl.range.first) - 1;
		for (const Annotation &item : p_decl.annotations) {
			annotation(item, p_decl.range, cursor);
		}
		if (p_decl.declared_type) {
			type(*p_decl.declared_type, p_decl.range, cursor);
		}
		expr(p_decl.initializer.get(), p_decl.range, cursor);
	}

	void params(const std::vector<ParamDecl> &p_params, const TokenRange &p_parent, int64_t &r_cursor) {
		for (const ParamDecl &param : p_params) {
			enter(param.range, p_parent, r_cursor);
			name(param.name_token, param.name, param.range);
			int64_t cursor = int64_t(param.range.first) - 1;
			if (param.declared_type) {
				type(*param.declared_type, param.range, cursor);
			}
			expr(param.default_value.get(), param.range, cursor);
		}
	}

	void function(const FuncDecl &p_decl, const TokenRange &p_parent, int64_t &r_cursor) {
		enter(p_decl.range, p_parent, r_cursor);
		// An inline accessor, @set_x or @get_x, is named by its keyword.
		name(p_decl.name_token, p_decl.name.rfind('@', 0) == 0 ? p_decl.name.substr(1, 3) : p_decl.name, p_decl.range);
		int64_t cursor = int64_t(p_decl.range.first) - 1;
		params(p_decl.params, p_decl.range, cursor);
		if (p_decl.return_type_ref) {
			type(*p_decl.return_type_ref, p_decl.range, cursor);
		}
		block(p_decl.body, p_decl.range, cursor);
	}

public:
	explicit RangeChecker(const SourceFile &p_file) :
			file(p_file) {}

	// The nodes of a class, within p_within for an inner class.
	void run(const ClassDecl &p_class, std::optional<TokenRange> p_within = std::nullopt) {
		const TokenRange whole = p_within ? *p_within : TokenRange{ 0, uint32_t(file.tokens.size() - 1) };
		int64_t cursor = -1;
		for (const VarDecl &member : p_class.members) {
			variable(member, whole, cursor);
		}
		cursor = -1;
		for (const ConstDecl &constant : p_class.constants) {
			enter(constant.range, whole, cursor);
			name(constant.name_token, constant.name, constant.range);
			int64_t inner = int64_t(constant.range.first) - 1;
			if (constant.declared_type) {
				type(*constant.declared_type, constant.range, inner);
			}
			expr(constant.initializer.get(), constant.range, inner);
		}
		cursor = -1;
		for (const FuncDecl &function_decl : p_class.functions) {
			function(function_decl, whole, cursor);
		}
		cursor = -1;
		for (const SignalDecl &signal : p_class.signals) {
			enter(signal.range, whole, cursor);
			name(signal.name_token, signal.name, signal.range);
			int64_t inner = int64_t(signal.range.first) - 1;
			params(signal.params, signal.range, inner);
		}
		cursor = -1;
		for (const StructDecl &decl : p_class.structs) {
			enter(decl.range, whole, cursor);
			name(decl.name_token, decl.name, decl.range);
			int64_t inner = int64_t(decl.range.first) - 1;
			for (const VarDecl &field : decl.fields) {
				variable(field, decl.range, inner);
			}
		}
		cursor = -1;
		for (const InterfaceDecl &decl : p_class.interfaces) {
			enter(decl.range, whole, cursor);
			name(decl.name_token, decl.name, decl.range);
			int64_t inner = int64_t(decl.range.first) - 1;
			for (const InterfaceProperty &property : decl.properties) {
				enter(property.range, decl.range, inner);
				name(property.name_token, property.name, property.range);
				int64_t type_cursor = int64_t(property.range.first) - 1;
				type(property.type_ref, property.range, type_cursor);
			}
			inner = int64_t(decl.range.first) - 1;
			for (const FuncDecl &method : decl.methods) {
				function(method, decl.range, inner);
			}
		}
		cursor = -1;
		for (const TypeRef &ref : p_class.implements) {
			type(ref, whole, cursor);
		}
		cursor = -1;
		if (p_class.owner_ref) {
			type(*p_class.owner_ref, whole, cursor);
		}
		cursor = -1;
		for (const ExportGroupDecl &group : p_class.groups) {
			annotation(group.annotation, whole, cursor);
		}
		cursor = -1;
		for (const std::unique_ptr<ClassDecl> &inner : p_class.classes) {
			enter(inner->range, whole, cursor);
			name(inner->name_token, inner->name, inner->range);
			run(*inner, inner->range);
		}
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

// Where each statement begins, by line.
void statement_starts(const Block &p_block, const SourceFile &p_file, std::vector<std::pair<int, uint32_t>> &r_starts) {
	for (const auto &stmt : p_block) {
		const uint32_t begin = p_file.begin(stmt->range);
		r_starts.push_back({ p_file.position(begin).line, begin });
		statement_starts(stmt->body, p_file, r_starts);
		statement_starts(stmt->else_body, p_file, r_starts);
		for (const MatchBranch &branch : stmt->branches) {
			statement_starts(branch.body, p_file, r_starts);
		}
	}
}

std::vector<std::pair<int, uint32_t>> statement_starts(const ClassDecl &p_class) {
	std::vector<std::pair<int, uint32_t>> starts;
	for (const FuncDecl &function : p_class.functions) {
		statement_starts(function.body, *p_class.source, starts);
	}
	for (const std::unique_ptr<ClassDecl> &inner : p_class.classes) {
		const std::vector<std::pair<int, uint32_t>> inner_starts = statement_starts(*inner);
		starts.insert(starts.end(), inner_starts.begin(), inner_starts.end());
	}
	return starts;
}

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

	void expr(const Expr *p_expr) {
		if (!p_expr) {
			return;
		}
		if ((p_expr->kind == ExprKind::Name || p_expr->kind == ExprKind::Member || p_expr->kind == ExprKind::Call) && p_expr->target != Target::None && p_expr->target != Target::NoneComparison) {
			add(p_expr->name_token, p_expr->text, "use of");
		}
		expr(p_expr->left.get());
		expr(p_expr->right.get());
		expr(p_expr->base.get());
		for (const auto &arg : p_expr->args) {
			expr(arg.get());
		}
	}

	void block(const Block &p_block) {
		for (const auto &stmt : p_block) {
			if (stmt->kind == StmtKind::Var || stmt->kind == StmtKind::Const || stmt->kind == StmtKind::For) {
				add(stmt->name_token, stmt->name, "local");
			}
			expr(stmt->target.get());
			expr(stmt->condition.get());
			expr(stmt->value.get());
			block(stmt->body);
			block(stmt->else_body);
			for (const MatchBranch &branch : stmt->branches) {
				for (const Pattern &pattern : branch.patterns) {
					if (pattern.kind == PatternKind::Bind) {
						add(pattern.name_token, pattern.name, "local");
					}
					expr(pattern.value.get());
				}
				expr(branch.guard.get());
				block(branch.body);
			}
		}
	}

public:
	ResolvedNames(const ClassDecl &p_class, const SourceFile &p_file) :
			file(p_file) {
		for (const VarDecl &member : p_class.members) {
			add(member.name_token, member.name, "member");
			for (const Annotation &annotation : member.annotations) {
				for (const auto &arg : annotation.args) {
					expr(arg.get());
				}
			}
			expr(member.initializer.get());
		}
		for (const ExportGroupDecl &group : p_class.groups) {
			for (const auto &arg : group.annotation.args) {
				expr(arg.get());
			}
		}
		for (const ConstDecl &constant : p_class.constants) {
			add(constant.name_token, constant.name, "constant");
			expr(constant.initializer.get());
		}
		for (const FuncDecl &function : p_class.functions) {
			add(function.name_token, function.name, "function");
			for (const ParamDecl &param : function.params) {
				add(param.name_token, param.name, "parameter");
				expr(param.default_value.get());
			}
			block(function.body);
		}
		for (const SignalDecl &signal : p_class.signals) {
			add(signal.name_token, signal.name, "signal");
		}
		for (const StructDecl &decl : p_class.structs) {
			add(decl.name_token, decl.name, "struct");
			for (const VarDecl &field : decl.fields) {
				add(field.name_token, field.name, "field");
				expr(field.initializer.get());
			}
		}
		for (const InterfaceDecl &decl : p_class.interfaces) {
			add(decl.name_token, decl.name, "interface");
			for (const FuncDecl &method : decl.methods) {
				add(method.name_token, method.name, "method");
			}
			for (const InterfaceProperty &property : decl.properties) {
				add(property.name_token, property.name, "property");
			}
		}
		for (const std::unique_ptr<ClassDecl> &inner : p_class.classes) {
			add(inner->name_token, inner->name, "class");
			const ResolvedNames names(*inner, p_file);
			this->names.insert(this->names.end(), names.names.begin(), names.names.end());
		}
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

void check_recovery(const std::string &p_source, const std::function<void(const std::string &)> &p_compile) {
	std::vector<Diagnostic> errors;
	const ClassDecl original = parse(p_source, errors);
	if (!errors.empty()) {
		return;
	}
	const SourceFile &file = *original.source;
	const std::vector<std::pair<int, uint32_t>> starts = statement_starts(original);
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
		for (const auto &entry : statement_starts(parse(truncated, ignored))) {
			found.insert(entry.second);
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
		for (const auto &[statement_line, begin] : starts) {
			if (statement_line != int(line + 1) && (statement_line < exempt_from || statement_line >= exempt_to)) {
				CHECK_MESSAGE(found.count(begin < cut ? begin : begin - (end - cut)), "the statement of line ", statement_line, " is lost");
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
