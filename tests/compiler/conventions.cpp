#include "conventions.h"

#include "compiler/parser.h"
#include "doctest.h"

#include <algorithm>

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
		CHECK_MESSAGE(int64_t(p_range.first) > r_cursor, "a node begins inside the sibling before it in:\n", file.text);
		r_cursor = p_range.last;
	}

	void name(uint32_t p_token, const std::string &p_name, const TokenRange &p_range) {
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
		if (p_stmt.kind == StmtKind::Var || p_stmt.kind == StmtKind::For) {
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
		name(p_decl.name_token, p_decl.name[0] == '@' ? p_decl.name.substr(1, 3) : p_decl.name, p_decl.range);
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

	void run(const ClassDecl &p_class) {
		const TokenRange whole{ 0, uint32_t(file.tokens.size() - 1) };
		int64_t cursor = -1;
		for (const VarDecl &member : p_class.members) {
			variable(member, whole, cursor);
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
	}
};

} // namespace

void check_syntax_conventions(const std::string &p_source) {
	std::vector<Diagnostic> errors;
	const std::shared_ptr<const SourceFile> file = tokenize(p_source, errors);
	check_lossless(*file);
	const ClassDecl cls = parse(file, errors);
	if (errors.empty()) {
		RangeChecker(*file).run(cls);
	}
}

} // namespace petrichor::tests
