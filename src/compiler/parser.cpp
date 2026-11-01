#include "compiler/parser.h"

#include <algorithm>

namespace petrichor::compiler {

namespace {

constexpr uint32_t NO_OFFSET = UINT32_MAX;

bool is_layout(TokenKind p_kind) {
	return p_kind == TokenKind::Newline || p_kind == TokenKind::Indent || p_kind == TokenKind::Dedent;
}

// Recovery works by line (architecture §2.1). An error sets `failed` and skips
// the rest of its line; what the line parsed is kept, a missing expression or
// type becomes an error node, and the next line break clears `failed`. Inside
// brackets the line ends at the next line break, where the lexer takes the
// brackets as closed: the parser asks for it in `reset` and is run again.
class Parser {
	const SourceFile &file;
	const std::vector<Token> &tokens;
	std::vector<Diagnostic> &errors;
	bool unclosed; // Brackets are open at the end of the file.
	const std::vector<uint32_t> &resets; // Those of earlier runs.
	size_t at = 0;
	// The last token consumed that can end a node: not a line break, an
	// indent, a dedent or a semicolon.
	uint32_t last = 0;
	bool failed = false;
	bool line_failed = false; // Whether the line the last line break ended had failed.

	const Token &peek(size_t p_offset = 0) const {
		const size_t index = at + p_offset;
		return index < tokens.size() ? tokens[index] : tokens.back();
	}
	bool check(TokenKind p_kind) const { return peek().kind == p_kind; }
	const Token &advance() {
		const Token &token = peek();
		if (at < tokens.size() - 1) {
			if (token.kind == TokenKind::Newline) {
				line_failed = failed;
			}
			if (is_layout(token.kind)) {
				failed = false;
			} else if (token.kind != TokenKind::Semicolon) {
				last = uint32_t(at);
			}
			at++;
		}
		return token;
	}
	uint32_t index(const Token &p_token) const { return uint32_t(&p_token - tokens.data()); }
	// From `p_first` to the last token consumed. A node that consumed none,
	// such as what an error left empty at a line break, ends where it begins,
	// and the enclosing nodes reach it.
	TokenRange span(uint32_t p_first) {
		last = std::max(last, p_first);
		return { p_first, last };
	}
	bool match(TokenKind p_kind) {
		if (check(p_kind)) {
			advance();
			return true;
		}
		return false;
	}

	void report(const Token &p_token, const std::string &p_message) {
		errors.push_back({ p_token.offset, p_token.end(), p_message });
	}
	// Reports an error, unless the line has one, and skips the rest of the line.
	void fail(const Token &p_token, const std::string &p_message) {
		if (!failed) {
			report(p_token, p_message);
		}
		failed = true;
		skip_line();
	}
	// Returns the token, or after an error the end of the line.
	const Token &expect(TokenKind p_kind, const char *p_what) {
		if (check(p_kind)) {
			return advance();
		}
		fail(peek(), std::string("Expected ") + p_what + ", found " + token_name(peek().kind) + ".");
		return peek();
	}

	void skip_line() {
		while (!check(TokenKind::EndOfFile) && !is_layout(peek().kind)) {
			if (unclosed && joined(at)) {
				const uint32_t offset = line_break(tokens[at - 1].end(), tokens[at].offset);
				if (!std::binary_search(resets.begin(), resets.end(), offset)) {
					// Ends this run.
					reset = offset;
					at = tokens.size() - 1;
					return;
				}
			}
			advance();
		}
	}

	// Whether the token begins a line that the lexer joined to the one
	// before, because brackets were open.
	bool joined(size_t p_index) const {
		return p_index > 0 && !is_layout(tokens[p_index - 1].kind) && line_break(tokens[p_index - 1].end(), tokens[p_index].offset) != NO_OFFSET;
	}

	// The first line break between two offsets that no backslash continues.
	uint32_t line_break(uint32_t p_from, uint32_t p_to) const {
		for (uint32_t i = p_from; i < p_to; i++) {
			if (file.text[i] != '\n') {
				continue;
			}
			const uint32_t end = i > p_from && file.text[i - 1] == '\r' ? i - 1 : i;
			const auto continuation = std::lower_bound(file.trivia.begin(), file.trivia.end(), end, [](const Token &p_trivia, uint32_t p_end) {
				return p_trivia.end() < p_end;
			});
			if (continuation == file.trivia.end() || continuation->end() != end || continuation->kind != TokenKind::LineContinuation) {
				return i;
			}
		}
		return NO_OFFSET;
	}

	TypeRef type_ref() {
		TypeRef ref;
		const uint32_t first = uint32_t(at);
		if (!check(TokenKind::Identifier)) {
			// An error type, which has no name.
			fail(peek(), std::string("Expected a type name, found ") + token_name(peek().kind) + ".");
			ref.range = span(first);
			ref.name_token = ref.range.first;
			return ref;
		}
		ref.name = advance().text;
		ref.name_token = first;
		// A type declared in a class, such as Combat.Damageable.
		while (check(TokenKind::Period) && peek(1).kind == TokenKind::Identifier) {
			advance();
			ref.name += "." + advance().text;
		}
		if (match(TokenKind::BracketOpen)) {
			do {
				ref.args.push_back(type_ref());
			} while (match(TokenKind::Comma));
			expect(TokenKind::BracketClose, "\"]\"");
		}
		ref.nullable = match(TokenKind::Question);
		ref.range = span(first);
		return ref;
	}

	/* Expressions */

	std::unique_ptr<Expr> make(ExprKind p_kind, const Token &p_at) {
		auto expr = std::make_unique<Expr>();
		expr->kind = p_kind;
		expr->range = { index(p_at), index(p_at) };
		return expr;
	}

	std::unique_ptr<Expr> error_expr(uint32_t p_first) {
		auto expr = std::make_unique<Expr>();
		expr->kind = ExprKind::Error;
		expr->range = span(p_first);
		return expr;
	}

	std::unique_ptr<Expr> binary(const Token &p_op, std::unique_ptr<Expr> p_left, std::unique_ptr<Expr> p_right) {
		auto expr = make(ExprKind::Binary, p_op);
		expr->op = p_op.kind;
		expr->op_token = index(p_op);
		expr->range = { p_left->range.first, p_right->range.last };
		expr->left = std::move(p_left);
		expr->right = std::move(p_right);
		return expr;
	}

	// `as` binds loosest, then `a if c else b`, as in GDScript
	// (gdscript_parser.h:1435-1436).
	std::unique_ptr<Expr> expression() {
		auto expr = conditional();
		while (check(TokenKind::As)) {
			auto cast = make(ExprKind::Cast, advance());
			cast->cast_type = type_ref();
			cast->range = span(expr->range.first);
			cast->left = std::move(expr);
			expr = std::move(cast);
		}
		return expr;
	}

	std::unique_ptr<Expr> conditional() {
		auto expr = or_expr();
		if (!check(TokenKind::If)) {
			return expr;
		}
		auto result = make(ExprKind::Conditional, advance());
		const uint32_t first = expr->range.first;
		result->left = std::move(expr);
		result->base = conditional();
		expect(TokenKind::Else, "\"else\"");
		result->right = conditional();
		result->range = span(first);
		return result;
	}

	std::unique_ptr<Expr> or_expr() {
		auto left = and_expr();
		while (check(TokenKind::Or)) {
			const Token &op = advance();
			left = binary(op, std::move(left), and_expr());
		}
		return left;
	}

	std::unique_ptr<Expr> and_expr() {
		auto left = not_expr();
		while (check(TokenKind::And)) {
			const Token &op = advance();
			left = binary(op, std::move(left), not_expr());
		}
		return left;
	}

	std::unique_ptr<Expr> not_expr() {
		// `!` is `not`, as in GDScript.
		if (check(TokenKind::Not) || check(TokenKind::Bang)) {
			const Token &op = advance();
			auto expr = make(ExprKind::Unary, op);
			expr->op = TokenKind::Not;
			expr->op_token = index(op);
			expr->left = not_expr();
			expr->range = span(index(op));
			return expr;
		}
		return comparison();
	}

	static bool is_comparison(TokenKind p_kind) {
		return p_kind == TokenKind::EqualEqual || p_kind == TokenKind::BangEqual || p_kind == TokenKind::Less ||
				p_kind == TokenKind::LessEqual || p_kind == TokenKind::Greater || p_kind == TokenKind::GreaterEqual;
	}

	std::unique_ptr<Expr> comparison() {
		auto left = additive();
		while (is_comparison(peek().kind)) {
			const Token &op = advance();
			left = binary(op, std::move(left), additive());
		}
		return left;
	}

	std::unique_ptr<Expr> additive() {
		auto left = multiplicative();
		while (check(TokenKind::Plus) || check(TokenKind::Minus)) {
			const Token &op = advance();
			left = binary(op, std::move(left), multiplicative());
		}
		return left;
	}

	std::unique_ptr<Expr> multiplicative() {
		auto left = unary();
		while (check(TokenKind::Star) || check(TokenKind::Slash) || check(TokenKind::Percent)) {
			const Token &op = advance();
			left = binary(op, std::move(left), unary());
		}
		return left;
	}

	std::unique_ptr<Expr> unary() {
		if (check(TokenKind::Await)) {
			auto expr = make(ExprKind::Await, advance());
			expr->left = unary();
			expr->range = span(expr->range.first);
			return expr;
		}
		if (check(TokenKind::Minus)) {
			const Token &op = advance();
			auto expr = make(ExprKind::Unary, op);
			expr->op = op.kind;
			expr->op_token = index(op);
			expr->left = unary();
			expr->range = span(index(op));
			if (expr->left->kind == ExprKind::IntLiteral && expr->left->int_overflows) {
				// -9223372036854775808 is the smallest int; its magnitude alone is not.
				auto literal = std::move(expr->left);
				literal->int_overflows = false;
				literal->range = expr->range;
				return literal;
			}
			return expr;
		}
		return type_test();
	}

	// `x is T` and `x is not T` bind tighter than arithmetic, as in GDScript
	// (gdscript_parser.h:1451).
	std::unique_ptr<Expr> type_test() {
		auto expr = postfix();
		while (check(TokenKind::Is)) {
			auto test = make(ExprKind::TypeTest, advance());
			test->bool_value = match(TokenKind::Not);
			test->cast_type = type_ref();
			test->range = span(expr->range.first);
			test->left = std::move(expr);
			expr = std::move(test);
		}
		return expr;
	}

	void call_arguments(Expr &r_call) {
		expect(TokenKind::ParenOpen, "\"(\"");
		while (!failed && !check(TokenKind::ParenClose)) {
			std::string name;
			uint32_t name_token = 0;
			if (check(TokenKind::Identifier) && peek(1).kind == TokenKind::Equal) {
				name_token = uint32_t(at);
				name = advance().text;
				advance();
			}
			auto arg = expression();
			if (!name.empty() || !r_call.arg_names.empty()) {
				r_call.arg_names.resize(r_call.args.size());
				r_call.arg_names.push_back(name);
				r_call.arg_name_tokens.resize(r_call.args.size());
				r_call.arg_name_tokens.push_back(name_token);
			}
			r_call.args.push_back(std::move(arg));
			if (!match(TokenKind::Comma)) {
				break;
			}
		}
		expect(TokenKind::ParenClose, "\")\"");
	}

	std::unique_ptr<Expr> postfix() {
		auto expr = primary();
		while (true) {
			if (check(TokenKind::ParenOpen) && (expr->kind == ExprKind::Name || expr->kind == ExprKind::Member)) {
				// `f(...)` or `base.f(...)`.
				auto call = make(ExprKind::Call, peek());
				call->name_token = expr->name_token;
				call->text = expr->text;
				call->base = std::move(expr->base);
				call_arguments(*call);
				call->range = span(expr->range.first);
				expr = std::move(call);
			} else if (check(TokenKind::Period)) {
				advance();
				auto member = std::make_unique<Expr>();
				member->kind = ExprKind::Member;
				const uint32_t name_at = uint32_t(at);
				const Token &name = expect(TokenKind::Identifier, "a member name");
				member->text = name.text;
				member->name_token = failed ? span(name_at).first : index(name);
				member->range = span(expr->range.first);
				member->base = std::move(expr);
				expr = std::move(member);
			} else if (check(TokenKind::BracketOpen)) {
				auto subscript = make(ExprKind::Subscript, advance());
				subscript->right = expression();
				expect(TokenKind::BracketClose, "\"]\"");
				subscript->range = span(expr->range.first);
				subscript->base = std::move(expr);
				expr = std::move(subscript);
			} else {
				return expr;
			}
		}
	}

	// Node names in a path may be keywords, as in GDScript.
	static bool is_node_name(TokenKind p_kind) {
		return p_kind == TokenKind::Identifier || (p_kind >= TokenKind::And && p_kind <= TokenKind::While);
	}

	// `$Name`, `$Head/Camera` or `$"Head/Camera"` (spec §7.1).
	std::unique_ptr<Expr> get_node() {
		auto expr = make(ExprKind::GetNode, advance());
		if (check(TokenKind::String)) {
			expr->text = advance().text;
		} else if (!is_node_name(peek().kind)) {
			fail(peek(), std::string("Expected a node path after \"$\", found ") + token_name(peek().kind) + ".");
		} else {
			expr->text = advance().text;
			while (check(TokenKind::Slash) && is_node_name(peek(1).kind)) {
				advance();
				expr->text += "/" + advance().text;
			}
		}
		expr->range = span(expr->range.first);
		return expr;
	}

	std::unique_ptr<Expr> primary() {
		if (failed) {
			return error_expr(uint32_t(at));
		}
		const Token &token = peek();
		switch (token.kind) {
			case TokenKind::Int: {
				advance();
				auto expr = make(ExprKind::IntLiteral, token);
				expr->int_value = token.int_value;
				expr->int_overflows = token.int_overflows;
				return expr;
			}
			case TokenKind::Float: {
				advance();
				auto expr = make(ExprKind::FloatLiteral, token);
				expr->float_value = token.float_value;
				return expr;
			}
			case TokenKind::String: {
				advance();
				auto expr = make(ExprKind::StringLiteral, token);
				expr->text = token.text;
				return expr;
			}
			case TokenKind::True:
			case TokenKind::False: {
				advance();
				auto expr = make(ExprKind::BoolLiteral, token);
				expr->bool_value = token.kind == TokenKind::True;
				return expr;
			}
			case TokenKind::Super:
			case TokenKind::Identifier: {
				advance();
				auto expr = make(ExprKind::Name, token);
				expr->text = token.text;
				expr->name_token = index(token);
				return expr;
			}
			case TokenKind::Self:
				advance();
				return make(ExprKind::Self, token);
			case TokenKind::None:
			case TokenKind::Null:
				advance();
				return make(ExprKind::NoneLiteral, token);
			case TokenKind::Dollar:
				return get_node();
			case TokenKind::BracketOpen: {
				auto expr = make(ExprKind::ArrayLiteral, advance());
				while (!failed && !check(TokenKind::BracketClose)) {
					expr->args.push_back(expression());
					if (!match(TokenKind::Comma)) {
						break;
					}
				}
				expect(TokenKind::BracketClose, "\"]\"");
				expr->range = span(expr->range.first);
				return expr;
			}
			case TokenKind::BraceOpen:
				return dictionary_literal();
			case TokenKind::ParenOpen: {
				advance();
				auto expr = expression();
				expect(TokenKind::ParenClose, "\")\"");
				expr->range = span(index(token));
				return expr;
			}
			default:
				fail(token, std::string("Expected an expression, found ") + token_name(token.kind) + ".");
				return error_expr(index(token));
		}
	}

	// `{k: v}`, or `{name = v}` with names or strings as keys, one style per
	// literal, as in GDScript (modules/gdscript/gdscript_parser.cpp:3302-3342).
	std::unique_ptr<Expr> dictionary_literal() {
		auto expr = make(ExprKind::DictionaryLiteral, advance());
		while (!failed && !check(TokenKind::BraceClose)) {
			const Token &start = peek();
			std::unique_ptr<Expr> key;
			if (start.kind == TokenKind::Identifier && peek(1).kind == TokenKind::Equal) {
				key = make(ExprKind::StringLiteral, advance());
				key->text = start.text;
			} else {
				key = expression();
			}
			const bool lua = check(TokenKind::Equal);
			if (!expr->args.empty() && lua != expr->bool_value) {
				fail(peek(), "A dictionary is written with \":\" or \"=\" after every key, not both.");
			} else if (lua && key->kind != ExprKind::StringLiteral) {
				fail(start, "A key written with \"=\" is a name or a string, as in { key = value }.");
			}
			expr->bool_value = lua;
			if (!match(TokenKind::Equal)) {
				expect(TokenKind::Colon, "\":\" or \"=\" after the key");
			}
			expr->args.push_back(std::move(key));
			expr->args.push_back(expression());
			if (!match(TokenKind::Comma)) {
				break;
			}
		}
		expect(TokenKind::BraceClose, "\"}\"");
		expr->range = span(expr->range.first);
		return expr;
	}

	/* Statements */

	// After a colon: a statement on the same line, or an indented block.
	Block block() {
		Block body;
		if (!check(TokenKind::Newline)) {
			body.push_back(statement());
			return body;
		}
		advance();
		if (!check(TokenKind::Indent)) {
			// A line that failed may have lost the colon that wanted a block.
			if (!line_failed) {
				report(peek(), std::string("Expected an indented block, found ") + token_name(peek().kind) + ".");
			}
			return body;
		}
		advance();
		statements(body);
		match(TokenKind::Dedent);
		return body;
	}

	void statements(Block &r_body) {
		while (!check(TokenKind::Dedent) && !check(TokenKind::EndOfFile)) {
			if (check(TokenKind::Indent)) {
				auto stmt = std::make_unique<Stmt>();
				stmt->kind = StmtKind::Error;
				const uint32_t first = uint32_t(at);
				stmt->body = stray_block();
				stmt->range = span(first);
				r_body.push_back(std::move(stmt));
			} else if (!match(TokenKind::Newline)) {
				r_body.push_back(statement());
			}
		}
	}

	// An indented block that nothing opens, whose statements are kept for the
	// tools. After a line that failed it is a consequence of that error, and
	// its own errors are not reported.
	Block stray_block() {
		const bool quiet = line_failed;
		const size_t reported = errors.size();
		if (!quiet) {
			report(peek(), "Unexpected indentation.");
		}
		advance();
		Block body;
		statements(body);
		match(TokenKind::Dedent);
		if (quiet) {
			errors.resize(reported);
		}
		return body;
	}

	// A stray block where declarations go, as the body of a function without a name.
	FuncDecl stray_function() {
		FuncDecl function;
		function.name_token = uint32_t(at);
		function.body = stray_block();
		function.range = span(function.name_token);
		return function;
	}

	void end_of_statement() {
		// `a = 1; b = 2`: the next statement follows on the same line.
		if (match(TokenKind::Semicolon) && !check(TokenKind::Newline) && !check(TokenKind::Dedent) && !check(TokenKind::EndOfFile)) {
			return;
		}
		if (!check(TokenKind::Dedent) && !check(TokenKind::EndOfFile)) {
			expect(TokenKind::Newline, "a line break");
		}
	}

	// After the colon of a declaration with a block of its own: the line
	// break and the indent, false without them.
	bool open_block(const char *p_what) {
		if (!check(TokenKind::Newline)) {
			fail(peek(), std::string("Expected a line break, found ") + token_name(peek().kind) + ".");
		}
		if (!match(TokenKind::Newline)) {
			return false;
		}
		if (match(TokenKind::Indent)) {
			return true;
		}
		if (!line_failed) {
			report(peek(), std::string("Expected ") + p_what + ", found " + token_name(peek().kind) + ".");
		}
		return false;
	}

	std::unique_ptr<Stmt> make_stmt(StmtKind p_kind, const Token &p_at) {
		auto stmt = std::make_unique<Stmt>();
		stmt->kind = p_kind;
		stmt->range = { index(p_at), index(p_at) };
		return stmt;
	}

	std::unique_ptr<Stmt> if_statement(const Token &p_keyword) {
		auto stmt = make_stmt(StmtKind::If, p_keyword);
		stmt->condition = expression();
		expect(TokenKind::Colon, "\":\"");
		stmt->body = block();
		if (check(TokenKind::Elif)) {
			const Token &elif = advance();
			stmt->else_body.push_back(if_statement(elif));
		} else if (match(TokenKind::Else)) {
			expect(TokenKind::Colon, "\":\"");
			stmt->else_body = block();
		}
		stmt->range = span(index(p_keyword));
		return stmt;
	}

	// `match value:` and its branches (spec §5.6).
	std::unique_ptr<Stmt> match_statement(const Token &p_keyword) {
		auto stmt = make_stmt(StmtKind::Match, p_keyword);
		stmt->value = expression();
		expect(TokenKind::Colon, "\":\"");
		if (open_block("an indented block of patterns")) {
			while (!check(TokenKind::Dedent) && !check(TokenKind::EndOfFile)) {
				MatchBranch branch;
				const uint32_t first = uint32_t(at);
				if (check(TokenKind::Indent)) {
					// A branch without patterns, for the statements of the block.
					branch.body = stray_block();
				} else if (begins_statement_only(peek().kind)) {
					// So is a statement where a pattern should be.
					report(peek(), std::string("Expected a pattern, found ") + token_name(peek().kind) + ".");
					branch.body.push_back(statement());
				} else {
					do {
						branch.patterns.push_back(pattern());
					} while (match(TokenKind::Comma));
					if (match(TokenKind::When)) {
						branch.guard = expression();
					}
					expect(TokenKind::Colon, "\":\"");
					branch.body = block();
				}
				branch.range = span(first);
				stmt->branches.push_back(std::move(branch));
			}
			match(TokenKind::Dedent);
		}
		stmt->range = span(index(p_keyword));
		return stmt;
	}

	static bool begins_statement_only(TokenKind p_kind) {
		return p_kind == TokenKind::Return || p_kind == TokenKind::If || p_kind == TokenKind::While || p_kind == TokenKind::For ||
				p_kind == TokenKind::Match || p_kind == TokenKind::Pass || p_kind == TokenKind::Break || p_kind == TokenKind::Continue;
	}

	Pattern pattern() {
		Pattern result;
		const uint32_t first = uint32_t(at);
		if (check(TokenKind::Identifier) && peek().text == "_") {
			advance();
		} else if (match(TokenKind::Var)) {
			result.kind = PatternKind::Bind;
			const Token &name = expect(TokenKind::Identifier, "a variable name");
			result.name = name.text;
			result.name_token = index(name);
		} else if (check(TokenKind::BracketOpen)) {
			fail(peek(), "Array and dictionary patterns are not supported yet.");
		} else {
			result.kind = PatternKind::Value;
			result.value = expression();
		}
		result.range = span(first);
		return result;
	}

	std::unique_ptr<Stmt> statement() {
		const Token &token = peek();
		switch (token.kind) {
			case TokenKind::Match:
				advance();
				return match_statement(token);
			case TokenKind::Var: {
				advance();
				auto stmt = make_stmt(StmtKind::Var, token);
				const Token &name = expect(TokenKind::Identifier, "a variable name");
				stmt->name = name.text;
				stmt->name_token = index(name);
				if (match(TokenKind::Colon) && !check(TokenKind::Equal)) {
					stmt->declared_type = type_ref();
				}
				if (match(TokenKind::Equal)) {
					stmt->value = expression();
				}
				stmt->range = span(index(token));
				end_of_statement();
				return stmt;
			}
			case TokenKind::If:
				advance();
				return if_statement(token);
			case TokenKind::While: {
				advance();
				auto stmt = make_stmt(StmtKind::While, token);
				stmt->condition = expression();
				expect(TokenKind::Colon, "\":\"");
				stmt->body = block();
				stmt->range = span(index(token));
				return stmt;
			}
			case TokenKind::For: {
				advance();
				auto stmt = make_stmt(StmtKind::For, token);
				const Token &name = expect(TokenKind::Identifier, "a variable name");
				stmt->name = name.text;
				stmt->name_token = index(name);
				if (match(TokenKind::Colon)) {
					stmt->declared_type = type_ref();
				}
				expect(TokenKind::In, "\"in\"");
				stmt->value = expression();
				expect(TokenKind::Colon, "\":\"");
				stmt->body = block();
				stmt->range = span(index(token));
				return stmt;
			}
			case TokenKind::Return: {
				advance();
				auto stmt = make_stmt(StmtKind::Return, token);
				if (!check(TokenKind::Newline) && !check(TokenKind::Dedent) && !check(TokenKind::EndOfFile)) {
					stmt->value = expression();
				}
				stmt->range = span(index(token));
				end_of_statement();
				return stmt;
			}
			case TokenKind::Pass:
			case TokenKind::Break:
			case TokenKind::Continue: {
				advance();
				const StmtKind kind = token.kind == TokenKind::Pass ? StmtKind::Pass : (token.kind == TokenKind::Break ? StmtKind::Break : StmtKind::Continue);
				auto stmt = make_stmt(kind, token);
				end_of_statement();
				return stmt;
			}
			default:
				break;
		}

		auto expr = expression();
		const TokenKind op = peek().kind;
		if (op == TokenKind::Equal || op == TokenKind::PlusEqual || op == TokenKind::MinusEqual ||
				op == TokenKind::StarEqual || op == TokenKind::SlashEqual || op == TokenKind::PercentEqual) {
			advance();
			auto stmt = make_stmt(StmtKind::Assign, token);
			stmt->assign_op = op;
			stmt->target = std::move(expr);
			stmt->value = expression();
			stmt->range = span(index(token));
			end_of_statement();
			return stmt;
		}
		auto stmt = make_stmt(StmtKind::Expression, token);
		stmt->range = expr->range;
		stmt->value = std::move(expr);
		end_of_statement();
		return stmt;
	}

	/* Declarations */

	// After "(": parameters up to the ")".
	std::vector<ParamDecl> params() {
		std::vector<ParamDecl> result;
		while (!failed && !check(TokenKind::ParenClose)) {
			ParamDecl param;
			const uint32_t first = uint32_t(at);
			const Token &param_name = expect(TokenKind::Identifier, "a parameter name");
			param.name = param_name.text;
			param.name_token = index(param_name);
			if (match(TokenKind::Colon)) {
				param.declared_type = type_ref();
			}
			if (match(TokenKind::Equal)) {
				param.default_value = expression();
			}
			param.range = span(first);
			result.push_back(std::move(param));
			if (!match(TokenKind::Comma)) {
				break;
			}
		}
		expect(TokenKind::ParenClose, "\")\"");
		return result;
	}

	// After "interface": the name and a block of method signatures and
	// properties (spec §3.9).
	InterfaceDecl interface(bool p_is_pub, uint32_t p_first, ClassDecl &r_class) {
		InterfaceDecl decl;
		const Token &name = expect(TokenKind::Identifier, "an interface name");
		decl.name = name.text;
		decl.name_token = index(name);
		decl.is_pub = p_is_pub;
		expect(TokenKind::Colon, "\":\"");
		if (open_block("an indented block")) {
			while (!check(TokenKind::Dedent) && !check(TokenKind::EndOfFile)) {
				if (check(TokenKind::Indent)) {
					r_class.functions.push_back(stray_function());
					continue;
				}
				if (match(TokenKind::Pass)) {
					end_of_statement();
					continue;
				}
				const uint32_t first = uint32_t(at);
				bool is_readonly = false;
				if (match(TokenKind::At)) {
					const Token &annotation = expect(TokenKind::Identifier, "an annotation name");
					if (!failed && annotation.text != "readonly") {
						fail(annotation, "An interface property takes only @readonly.");
					}
					is_readonly = true;
					match(TokenKind::Newline);
				}
				if (match(TokenKind::Var)) {
					InterfaceProperty property;
					const Token &property_name = expect(TokenKind::Identifier, "a property name");
					property.name = property_name.text;
					property.name_token = index(property_name);
					property.is_readonly = is_readonly;
					expect(TokenKind::Colon, "\":\" and the property's type");
					property.type_ref = type_ref();
					property.range = span(first);
					end_of_statement();
					decl.properties.push_back(std::move(property));
					continue;
				}
				if (is_readonly) {
					fail(peek(), "@readonly is only for interface properties (spec §3.9).");
				} else if (check(TokenKind::Signal)) {
					fail(peek(), "Signals of interfaces are not supported yet.");
				}
				FuncDecl method;
				method.is_async = match(TokenKind::Async);
				expect(TokenKind::Func, "\"func\"");
				if (failed) {
					end_of_statement();
					continue;
				}
				const Token &method_name = expect(TokenKind::Identifier, "a method name");
				method.name = method_name.text;
				method.name_token = index(method_name);
				method.is_pub = true;
				expect(TokenKind::ParenOpen, "\"(\"");
				method.params = params();
				if (match(TokenKind::Arrow)) {
					method.return_type_ref = type_ref();
				}
				if (check(TokenKind::Colon)) {
					fail(peek(), "Default methods of interfaces are not supported yet.");
				}
				method.range = span(first);
				end_of_statement();
				decl.methods.push_back(std::move(method));
			}
			match(TokenKind::Dedent);
		}
		decl.range = span(p_first);
		return decl;
	}

	// After "struct": the name and a block of fields (spec §2.4, §3.6).
	StructDecl structure(bool p_is_pub, uint32_t p_first, ClassDecl &r_class) {
		StructDecl decl;
		const Token &name = expect(TokenKind::Identifier, "a struct name");
		decl.name = name.text;
		decl.name_token = index(name);
		decl.is_pub = p_is_pub;
		expect(TokenKind::Colon, "\":\"");
		if (open_block("an indented block")) {
			while (!check(TokenKind::Dedent) && !check(TokenKind::EndOfFile)) {
				if (check(TokenKind::Indent)) {
					r_class.functions.push_back(stray_function());
					continue;
				}
				if (match(TokenKind::Pass)) {
					end_of_statement();
					continue;
				}
				if (check(TokenKind::Func) || check(TokenKind::Static)) {
					fail(peek(), "Methods of structs are not supported yet.");
					end_of_statement();
					continue;
				}
				if (!check(TokenKind::Var)) {
					fail(peek(), std::string("Expected a field, declared with var, found ") + token_name(peek().kind) + ".");
					end_of_statement();
					continue;
				}
				const uint32_t first = uint32_t(at);
				advance();
				VarDecl field;
				const Token &field_name = expect(TokenKind::Identifier, "a field name");
				field.name = field_name.text;
				field.name_token = index(field_name);
				if (match(TokenKind::Colon) && !check(TokenKind::Equal)) {
					field.declared_type = type_ref();
				}
				if (match(TokenKind::Equal)) {
					field.initializer = expression();
				}
				if (check(TokenKind::Colon)) {
					fail(peek(), "A field has no setter or getter.");
				}
				field.range = span(first);
				end_of_statement();
				decl.fields.push_back(std::move(field));
			}
			match(TokenKind::Dedent);
		}
		decl.range = span(p_first);
		return decl;
	}

	// After `var x: T = v:`, the property's setter and getter (spec §4.4):
	// `set = f, get = g` on the same line, or a block of `set(value):` and
	// `get:`, which become the functions @set_x and @get_x, and of the named
	// forms.
	void accessors(VarDecl &r_decl, ClassDecl &r_class) {
		r_decl.accessor_token = uint32_t(at);
		if (!check(TokenKind::Newline)) {
			named_accessors(r_decl);
			end_of_statement();
			return;
		}
		if (!open_block("an indented block of set and get")) {
			return;
		}
		while (!check(TokenKind::Dedent) && !check(TokenKind::EndOfFile)) {
			if (check(TokenKind::Indent)) {
				r_class.functions.push_back(stray_function());
				continue;
			}
			if (peek(1).kind == TokenKind::Equal) {
				named_accessors(r_decl);
				end_of_statement();
				continue;
			}
			const Token &keyword = expect(TokenKind::Identifier, "set or get");
			if (!failed && keyword.text != "set" && keyword.text != "get") {
				fail(keyword, "Expected set or get.");
			}
			if (failed) {
				end_of_statement();
				continue;
			}
			FuncDecl function;
			function.name_token = index(keyword);
			function.accessor_of = r_decl.name;
			if (keyword.text == "set") {
				expect(TokenKind::ParenOpen, "\"(\"");
				const Token &param = expect(TokenKind::Identifier, "the parameter's name");
				ParamDecl value;
				value.name = param.text;
				value.name_token = index(param);
				value.range = span(value.name_token);
				expect(TokenKind::ParenClose, "\")\"");
				function.params.push_back(std::move(value));
				function.name = "@set_" + r_decl.name;
			} else {
				function.name = "@get_" + r_decl.name;
			}
			if (!take_accessor(keyword.text == "set" ? r_decl.setter : r_decl.getter, function.name, keyword)) {
				end_of_statement();
				continue;
			}
			expect(TokenKind::Colon, "\":\"");
			function.body = block();
			function.range = span(function.name_token);
			r_class.functions.push_back(std::move(function));
		}
		match(TokenKind::Dedent);
	}

	// `set = f` or `get = g`, separated by commas.
	void named_accessors(VarDecl &r_decl) {
		do {
			const Token &keyword = expect(TokenKind::Identifier, "set or get");
			if (!failed && keyword.text != "set" && keyword.text != "get") {
				fail(keyword, "Expected set or get.");
			}
			expect(TokenKind::Equal, "\"=\"");
			const Token &function = expect(TokenKind::Identifier, "a function name");
			if (failed || !take_accessor(keyword.text == "set" ? r_decl.setter : r_decl.getter, function.text, keyword)) {
				return;
			}
		} while (match(TokenKind::Comma));
	}

	bool take_accessor(std::string &r_accessor, const std::string &p_function, const Token &p_at) {
		if (!r_accessor.empty()) {
			fail(p_at, "The property already has a " + p_at.text + "ter.");
			return false;
		}
		r_accessor = p_function;
		return true;
	}

	static bool is_group_annotation(const std::string &p_name) {
		return p_name == "export_group" || p_name == "export_subgroup" || p_name == "export_category";
	}

	// The annotations of a class (spec §2.3, §7.3).
	static bool is_class_annotation(const std::string &p_name) {
		return p_name == "tool" || p_name == "icon" || p_name == "static_unload" || p_name == "abstract" || p_name == "parent" || p_name == "owner";
	}

	void member(ClassDecl &r_class) {
		const uint32_t first = uint32_t(at);
		std::vector<Annotation> annotations;
		while (check(TokenKind::At)) {
			const uint32_t at_sign = index(advance());
			const Token &name = expect(TokenKind::Identifier, "an annotation name");
			if (failed) {
				return;
			}
			Annotation annotation;
			annotation.name = name.text;
			annotation.name_token = index(name);
			if (check(TokenKind::ParenOpen)) {
				Expr holder;
				call_arguments(holder);
				if (failed) {
					return;
				}
				if (!holder.arg_names.empty()) {
					fail(name, "An annotation takes its arguments by position.");
					return;
				}
				annotation.args = std::move(holder.args);
			}
			annotation.range = span(at_sign);
			if (is_group_annotation(annotation.name)) {
				// Stands alone, as in GDScript.
				r_class.groups.push_back({ std::move(annotation), r_class.members.size() });
				end_of_statement();
				return;
			}
			if (annotation.name == "readonly") {
				fail(name, "@readonly is only for interface properties (spec §3.9).");
				return;
			}
			if (annotation.name != "export" && annotation.name != "export_range" && annotation.name != "export_enum" && annotation.name != "export_multiline" && annotation.name != "onready") {
				fail(name, "The annotation @" + name.text + " is not supported yet.");
				return;
			}
			annotations.push_back(std::move(annotation));
			match(TokenKind::Newline);
		}
		bool is_export = false;
		bool is_onready = false;
		for (const Annotation &annotation : annotations) {
			is_export = is_export || annotation.name.rfind("export", 0) == 0;
			is_onready = is_onready || annotation.name == "onready";
		}
		const bool is_pub = match(TokenKind::Pub);
		const bool is_static = match(TokenKind::Static);
		const bool is_async = match(TokenKind::Async);
		if (is_async && !check(TokenKind::Func)) {
			fail(peek(), "async must precede func.");
			return;
		}
		if (is_static && !check(TokenKind::Func)) {
			fail(peek(), "Static members are not supported yet; static functions are.");
			return;
		}

		if (match(TokenKind::Var)) {
			VarDecl decl;
			const Token &name = expect(TokenKind::Identifier, "a variable name");
			decl.name = name.text;
			decl.name_token = index(name);
			decl.is_pub = is_pub;
			decl.is_export = is_export;
			decl.is_onready = is_onready;
			decl.annotations = std::move(annotations);
			if (match(TokenKind::Colon) && !check(TokenKind::Equal)) {
				decl.declared_type = type_ref();
			}
			if (match(TokenKind::Equal)) {
				decl.initializer = expression();
			}
			if (match(TokenKind::Colon)) {
				accessors(decl, r_class);
				decl.range = span(first);
			} else {
				decl.range = span(first);
				end_of_statement();
			}
			r_class.members.push_back(std::move(decl));
			return;
		}

		if (match(TokenKind::Interface)) {
			if (!annotations.empty() || is_static) {
				fail(peek(), "An interface takes no annotations and is not static.");
				return;
			}
			r_class.interfaces.push_back(interface(is_pub, first, r_class));
			return;
		}

		if (match(TokenKind::Struct)) {
			if (!annotations.empty() || is_static) {
				fail(peek(), "A struct takes no annotations and is not static.");
				return;
			}
			r_class.structs.push_back(structure(is_pub, first, r_class));
			return;
		}

		if (match(TokenKind::Signal)) {
			if (!annotations.empty() || is_static) {
				fail(peek(), "A signal takes no annotations and is not static.");
				return;
			}
			SignalDecl decl;
			const Token &name = expect(TokenKind::Identifier, "a signal name");
			decl.name = name.text;
			decl.name_token = index(name);
			decl.is_pub = is_pub;
			// `signal died` has no arguments, as in GDScript.
			if (match(TokenKind::ParenOpen)) {
				decl.params = params();
			}
			decl.range = span(first);
			end_of_statement();
			r_class.signals.push_back(std::move(decl));
			return;
		}

		if (check(TokenKind::Func)) {
			if (!annotations.empty()) {
				fail(peek(), "@" + annotations[0].name + " applies to variables.");
				return;
			}
			advance();
			FuncDecl decl;
			const Token &name = expect(TokenKind::Identifier, "a function name");
			decl.name = name.text;
			decl.name_token = index(name);
			decl.is_pub = is_pub;
			decl.is_static = is_static;
			decl.is_async = is_async;
			expect(TokenKind::ParenOpen, "\"(\"");
			decl.params = params();
			if (match(TokenKind::Arrow)) {
				decl.return_type_ref = type_ref();
			}
			expect(TokenKind::Colon, "\":\"");
			decl.body = block();
			decl.range = span(first);
			r_class.functions.push_back(std::move(decl));
			return;
		}

		fail(peek(), std::string("Expected a variable or a function, found ") + token_name(peek().kind) + ".");
	}

public:
	// Where the lexer should take open brackets as closed before parsing
	// again, or NO_OFFSET.
	uint32_t reset = NO_OFFSET;

	Parser(const SourceFile &p_file, std::vector<Diagnostic> &r_errors, bool p_unclosed, const std::vector<uint32_t> &p_resets) :
			file(p_file), tokens(p_file.tokens), errors(r_errors), unclosed(p_unclosed), resets(p_resets) {}

	// An expression that is the whole source.
	std::unique_ptr<Expr> single_expression() {
		auto expr = expression();
		match(TokenKind::Newline);
		if (!check(TokenKind::EndOfFile)) {
			fail(peek(), std::string("Expected the end of the expression, found ") + token_name(peek().kind) + ".");
		}
		return expr;
	}

	ClassDecl run() {
		ClassDecl result;
		bool has_class_name = false;
		bool has_extends = false;
		bool seen_member = false;
		while (!check(TokenKind::EndOfFile)) {
			// Dedents balance indents; one that a block ending early left is skipped.
			if (match(TokenKind::Newline) || match(TokenKind::Dedent)) {
				continue;
			}
			if (check(TokenKind::Indent)) {
				result.functions.push_back(stray_function());
				continue;
			}
			// Class annotations open the header (spec §2.1, §2.3).
			if (!seen_member && check(TokenKind::At) && is_class_annotation(peek(1).text)) {
				advance();
				const Token &name = advance();
				if (name.text == "owner") {
					if (result.owner_ref) {
						fail(name, "The class already has @owner.");
						continue;
					}
					expect(TokenKind::ParenOpen, "\"(\"");
					result.owner_ref = type_ref();
					expect(TokenKind::ParenClose, "\")\"");
					end_of_statement();
					continue;
				}
				report(name, "The class annotation @" + name.text + " is not supported yet.");
				if (check(TokenKind::ParenOpen)) {
					Expr holder;
					call_arguments(holder);
				}
				match(TokenKind::Newline);
				continue;
			}
			// The class header comes before the members (spec §2.1).
			if ((check(TokenKind::ClassName) || check(TokenKind::Extends)) && seen_member) {
				fail(peek(), std::string(token_name(peek().kind)) + " belongs to the class header, before the members.");
				continue;
			}
			if (check(TokenKind::ClassName)) {
				const Token &keyword = advance();
				if (has_class_name) {
					fail(keyword, "The class already has a name.");
					continue;
				}
				result.class_name = expect(TokenKind::Identifier, "a class name").text;
				has_class_name = true;
				if (!check(TokenKind::Extends)) {
					end_of_statement();
					continue;
				}
			}
			if (check(TokenKind::Extends)) {
				const Token &keyword = advance();
				if (has_extends) {
					fail(keyword, "The class already has a base class.");
					continue;
				}
				result.extends_path = check(TokenKind::String);
				const Token &base = result.extends_path ? advance() : expect(TokenKind::Identifier, "a base class name or script path");
				result.extends = base.text;
				result.extends_token = index(base);
				has_extends = true;
				// `extends Node3D implements Combat.Damageable, Saveable` (spec §3.9).
				if (match(TokenKind::Implements)) {
					do {
						result.implements.push_back(type_ref());
					} while (match(TokenKind::Comma));
				}
				end_of_statement();
				continue;
			}
			seen_member = true;
			member(result);
		}
		return result;
	}
};

} // namespace

ClassDecl parse(std::string p_source, std::vector<Diagnostic> &r_errors) {
	const auto file = std::make_shared<SourceFile>();
	file->text = std::move(p_source);
	std::vector<Diagnostic> errors;
	const std::vector<uint32_t> unclosed = lex(*file, {}, {}, errors);
	std::vector<uint32_t> resets;
	while (true) {
		if (!unclosed.empty()) {
			errors.clear();
			lex(*file, resets, unclosed, errors);
		}
		Parser parser(*file, errors, !unclosed.empty(), resets);
		ClassDecl result = parser.run();
		if (parser.reset == NO_OFFSET) {
			r_errors.insert(r_errors.end(), errors.begin(), errors.end());
			result.source = file;
			return result;
		}
		resets.insert(std::upper_bound(resets.begin(), resets.end(), parser.reset), parser.reset);
	}
}

std::unique_ptr<Expr> parse_expression(const std::string &p_source, std::vector<Diagnostic> &r_errors) {
	const size_t errors = r_errors.size();
	const std::shared_ptr<const SourceFile> file = tokenize(p_source, r_errors);
	const std::vector<uint32_t> no_resets;
	std::unique_ptr<Expr> expr = Parser(*file, r_errors, false, no_resets).single_expression();
	return r_errors.size() == errors ? std::move(expr) : nullptr;
}

} // namespace petrichor::compiler
