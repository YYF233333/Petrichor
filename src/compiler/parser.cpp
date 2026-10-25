#include "compiler/parser.h"

namespace petrichor::compiler {

namespace {

// Exceptions are disabled in engine builds, so an error sets `failed`; every
// parsing function then returns early, and the statement or member level skips
// the rest of the line and resumes.
class Parser {
	const std::vector<Token> &tokens;
	std::vector<Diagnostic> &errors;
	size_t at = 0;
	bool failed = false;

	const Token &peek(size_t p_offset = 0) const {
		const size_t index = at + p_offset;
		return index < tokens.size() ? tokens[index] : tokens.back();
	}
	bool check(TokenKind p_kind) const { return peek().kind == p_kind; }
	const Token &advance() {
		const Token &token = peek();
		if (at < tokens.size() - 1) {
			at++;
		}
		return token;
	}
	bool match(TokenKind p_kind) {
		if (check(p_kind)) {
			advance();
			return true;
		}
		return false;
	}

	void fail(const Token &p_token, const std::string &p_message) {
		if (!failed) {
			errors.push_back({ p_token.line, p_token.column, p_message });
		}
		failed = true;
	}
	// Returns the token, or a placeholder after reporting an error.
	const Token &expect(TokenKind p_kind, const char *p_what) {
		if (failed) {
			return peek();
		}
		if (!check(p_kind)) {
			fail(peek(), std::string("Expected ") + p_what + ", found " + token_name(peek().kind) + ".");
			return peek();
		}
		return advance();
	}

	// Skips the rest of the line and any block that follows it.
	void synchronize() {
		int depth = 0;
		while (!check(TokenKind::EndOfFile)) {
			const TokenKind kind = advance().kind;
			if (kind == TokenKind::Indent) {
				depth++;
			} else if (kind == TokenKind::Dedent) {
				if (depth == 0) {
					at--; // Leave the dedent to the enclosing block.
					return;
				}
				depth--;
			} else if (kind == TokenKind::Newline && depth == 0 && !check(TokenKind::Indent)) {
				return;
			}
		}
	}

	TypeRef type_ref() {
		const Token &name = expect(TokenKind::Identifier, "a type name");
		return TypeRef{ name.text, name.line, name.column };
	}

	/* Expressions */

	std::unique_ptr<Expr> make(ExprKind p_kind, const Token &p_at) {
		auto expr = std::make_unique<Expr>();
		expr->kind = p_kind;
		expr->line = p_at.line;
		expr->column = p_at.column;
		return expr;
	}

	std::unique_ptr<Expr> binary(const Token &p_op, std::unique_ptr<Expr> p_left, std::unique_ptr<Expr> p_right) {
		if (!p_left || !p_right) {
			return nullptr;
		}
		auto expr = make(ExprKind::Binary, p_op);
		expr->op = p_op.kind;
		expr->left = std::move(p_left);
		expr->right = std::move(p_right);
		return expr;
	}

	std::unique_ptr<Expr> expression() { return or_expr(); }

	std::unique_ptr<Expr> or_expr() {
		auto left = and_expr();
		while (left && check(TokenKind::Or)) {
			const Token &op = advance();
			left = binary(op, std::move(left), and_expr());
		}
		return left;
	}

	std::unique_ptr<Expr> and_expr() {
		auto left = not_expr();
		while (left && check(TokenKind::And)) {
			const Token &op = advance();
			left = binary(op, std::move(left), not_expr());
		}
		return left;
	}

	std::unique_ptr<Expr> not_expr() {
		if (check(TokenKind::Not)) {
			const Token &op = advance();
			auto expr = make(ExprKind::Unary, op);
			expr->op = op.kind;
			expr->left = not_expr();
			return expr->left ? std::move(expr) : nullptr;
		}
		return comparison();
	}

	static bool is_comparison(TokenKind p_kind) {
		return p_kind == TokenKind::EqualEqual || p_kind == TokenKind::BangEqual || p_kind == TokenKind::Less ||
				p_kind == TokenKind::LessEqual || p_kind == TokenKind::Greater || p_kind == TokenKind::GreaterEqual;
	}

	std::unique_ptr<Expr> comparison() {
		auto left = additive();
		while (left && is_comparison(peek().kind)) {
			const Token &op = advance();
			left = binary(op, std::move(left), additive());
		}
		return left;
	}

	std::unique_ptr<Expr> additive() {
		auto left = multiplicative();
		while (left && (check(TokenKind::Plus) || check(TokenKind::Minus))) {
			const Token &op = advance();
			left = binary(op, std::move(left), multiplicative());
		}
		return left;
	}

	std::unique_ptr<Expr> multiplicative() {
		auto left = unary();
		while (left && (check(TokenKind::Star) || check(TokenKind::Slash) || check(TokenKind::Percent))) {
			const Token &op = advance();
			left = binary(op, std::move(left), unary());
		}
		return left;
	}

	std::unique_ptr<Expr> unary() {
		if (check(TokenKind::Minus)) {
			const Token &op = advance();
			auto expr = make(ExprKind::Unary, op);
			expr->op = op.kind;
			expr->left = unary();
			return expr->left ? std::move(expr) : nullptr;
		}
		return postfix();
	}

	bool call_arguments(Expr &r_call) {
		expect(TokenKind::ParenOpen, "\"(\"");
		if (!failed && !check(TokenKind::ParenClose)) {
			do {
				if (check(TokenKind::ParenClose)) {
					break; // Trailing comma.
				}
				auto arg = expression();
				if (!arg) {
					return false;
				}
				r_call.args.push_back(std::move(arg));
			} while (match(TokenKind::Comma));
		}
		expect(TokenKind::ParenClose, "\")\"");
		return !failed;
	}

	std::unique_ptr<Expr> postfix() {
		auto expr = primary();
		if (!expr) {
			return nullptr;
		}
		if (expr->kind == ExprKind::Name && check(TokenKind::ParenOpen)) {
			auto call = make(ExprKind::Call, peek());
			call->line = expr->line;
			call->column = expr->column;
			call->text = expr->text;
			return call_arguments(*call) ? std::move(call) : nullptr;
		}
		if (expr->kind == ExprKind::Self && check(TokenKind::Period)) {
			advance();
			const Token &name = expect(TokenKind::Identifier, "a method name");
			if (failed) {
				return nullptr;
			}
			auto call = make(ExprKind::Call, name);
			call->text = name.text;
			call->base = std::move(expr);
			return call_arguments(*call) ? std::move(call) : nullptr;
		}
		if (check(TokenKind::Period)) {
			fail(peek(), "Member access is only supported on self so far.");
			return nullptr;
		}
		return expr;
	}

	std::unique_ptr<Expr> primary() {
		const Token &token = peek();
		switch (token.kind) {
			case TokenKind::Int: {
				advance();
				auto expr = make(ExprKind::IntLiteral, token);
				expr->int_value = token.int_value;
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
			case TokenKind::Identifier: {
				advance();
				auto expr = make(ExprKind::Name, token);
				expr->text = token.text;
				return expr;
			}
			case TokenKind::Self:
				advance();
				return make(ExprKind::Self, token);
			case TokenKind::ParenOpen: {
				advance();
				auto expr = expression();
				expect(TokenKind::ParenClose, "\")\"");
				return failed ? nullptr : std::move(expr);
			}
			default:
				fail(token, std::string("Expected an expression, found ") + token_name(token.kind) + ".");
				return nullptr;
		}
	}

	// After an error: skips the rest of the line and resumes.
	void recover() {
		failed = false;
		synchronize();
	}

	/* Statements */

	Block block() {
		Block body;
		if (!check(TokenKind::Newline)) {
			// A single statement on the same line as the colon.
			if (auto stmt = statement()) {
				body.push_back(std::move(stmt));
			}
			return body;
		}
		advance();
		expect(TokenKind::Indent, "an indented block");
		if (failed) {
			return body;
		}
		while (!check(TokenKind::Dedent) && !check(TokenKind::EndOfFile)) {
			auto stmt = statement();
			if (failed) {
				recover();
			} else if (stmt) {
				body.push_back(std::move(stmt));
			}
		}
		match(TokenKind::Dedent);
		return body;
	}

	void end_of_statement() {
		if (!check(TokenKind::Dedent) && !check(TokenKind::EndOfFile)) {
			expect(TokenKind::Newline, "a line break");
		}
	}

	std::unique_ptr<Stmt> make_stmt(StmtKind p_kind, const Token &p_at) {
		auto stmt = std::make_unique<Stmt>();
		stmt->kind = p_kind;
		stmt->line = p_at.line;
		stmt->column = p_at.column;
		return stmt;
	}

	std::unique_ptr<Stmt> if_statement(const Token &p_keyword) {
		auto stmt = make_stmt(StmtKind::If, p_keyword);
		stmt->condition = expression();
		expect(TokenKind::Colon, "\":\"");
		if (failed) {
			return nullptr;
		}
		stmt->body = block();
		if (check(TokenKind::Elif)) {
			const Token &elif = advance();
			stmt->else_body.push_back(if_statement(elif));
		} else if (match(TokenKind::Else)) {
			expect(TokenKind::Colon, "\":\"");
			stmt->else_body = block();
		}
		return stmt;
	}

	std::unique_ptr<Stmt> statement() {
		const Token &token = peek();
		switch (token.kind) {
			case TokenKind::Var: {
				advance();
				auto stmt = make_stmt(StmtKind::Var, token);
				const Token &name = expect(TokenKind::Identifier, "a variable name");
				stmt->name = name.text;
				stmt->line = name.line;
				stmt->column = name.column;
				if (match(TokenKind::Colon) && !check(TokenKind::Equal)) {
					stmt->declared_type = type_ref();
				}
				if (match(TokenKind::Equal)) {
					stmt->value = expression();
				}
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
				if (failed) {
					return nullptr;
				}
				stmt->body = block();
				return stmt;
			}
			case TokenKind::Return: {
				advance();
				auto stmt = make_stmt(StmtKind::Return, token);
				if (!check(TokenKind::Newline) && !check(TokenKind::Dedent) && !check(TokenKind::EndOfFile)) {
					stmt->value = expression();
				}
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
		if (!expr) {
			return nullptr;
		}
		const TokenKind op = peek().kind;
		if (op == TokenKind::Equal || op == TokenKind::PlusEqual || op == TokenKind::MinusEqual ||
				op == TokenKind::StarEqual || op == TokenKind::SlashEqual || op == TokenKind::PercentEqual) {
			const Token &op_token = advance();
			auto stmt = make_stmt(StmtKind::Assign, op_token);
			stmt->line = expr->line;
			stmt->column = expr->column;
			stmt->assign_op = op;
			stmt->target = std::move(expr);
			stmt->value = expression();
			end_of_statement();
			return stmt;
		}
		auto stmt = make_stmt(StmtKind::Expression, token);
		stmt->value = std::move(expr);
		end_of_statement();
		return stmt;
	}

	/* Declarations */

	void member(ClassDecl &r_class) {
		bool is_export = false;
		while (match(TokenKind::At)) {
			const Token &name = expect(TokenKind::Identifier, "an annotation name");
			if (failed) {
				return;
			}
			if (name.text != "export") {
				fail(name, "The annotation @" + name.text + " is not supported yet.");
				return;
			}
			is_export = true;
			match(TokenKind::Newline);
		}
		const bool is_pub = match(TokenKind::Pub);

		if (match(TokenKind::Var)) {
			VarDecl decl;
			const Token &name = expect(TokenKind::Identifier, "a variable name");
			decl.name = name.text;
			decl.line = name.line;
			decl.column = name.column;
			decl.is_pub = is_pub;
			decl.is_export = is_export;
			if (match(TokenKind::Colon) && !check(TokenKind::Equal)) {
				decl.declared_type = type_ref();
			}
			if (match(TokenKind::Equal)) {
				decl.initializer = expression();
			}
			end_of_statement();
			r_class.members.push_back(std::move(decl));
			return;
		}

		if (check(TokenKind::Func)) {
			if (is_export) {
				fail(peek(), "@export applies to variables.");
				return;
			}
			advance();
			FuncDecl decl;
			const Token &name = expect(TokenKind::Identifier, "a function name");
			decl.name = name.text;
			decl.line = name.line;
			decl.column = name.column;
			decl.is_pub = is_pub;
			expect(TokenKind::ParenOpen, "\"(\"");
			if (!check(TokenKind::ParenClose)) {
				do {
					if (check(TokenKind::ParenClose)) {
						break;
					}
					ParamDecl param;
					const Token &param_name = expect(TokenKind::Identifier, "a parameter name");
					param.name = param_name.text;
					param.line = param_name.line;
					param.column = param_name.column;
					if (match(TokenKind::Colon)) {
						param.declared_type = type_ref();
					}
					decl.params.push_back(std::move(param));
				} while (match(TokenKind::Comma));
			}
			expect(TokenKind::ParenClose, "\")\"");
			if (match(TokenKind::Arrow)) {
				decl.return_type_ref = type_ref();
			}
			expect(TokenKind::Colon, "\":\"");
			if (failed) {
				return;
			}
			decl.body = block();
			r_class.functions.push_back(std::move(decl));
			return;
		}

		fail(peek(), std::string("Expected a variable or a function, found ") + token_name(peek().kind) + ".");
	}

public:
	Parser(const std::vector<Token> &p_tokens, std::vector<Diagnostic> &r_errors) :
			tokens(p_tokens), errors(r_errors) {}

	ClassDecl run() {
		ClassDecl result;
		bool has_extends = false;
		while (!check(TokenKind::EndOfFile)) {
			if (failed) {
				recover();
				// A stray indented block at class level.
				while (match(TokenKind::Dedent) || match(TokenKind::Indent)) {
				}
				continue;
			}
			{
				if (match(TokenKind::Newline)) {
					continue;
				}
				if (check(TokenKind::ClassName)) {
					advance();
					result.class_name = expect(TokenKind::Identifier, "a class name").text;
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
					const Token &base = expect(TokenKind::Identifier, "a base class name");
					result.extends = base.text;
					result.extends_line = base.line;
					result.extends_column = base.column;
					has_extends = true;
					end_of_statement();
					continue;
				}
				member(result);
			}
		}
		return result;
	}
};

} // namespace

ClassDecl parse(const std::vector<Token> &p_tokens, std::vector<Diagnostic> &r_errors) {
	return Parser(p_tokens, r_errors).run();
}

} // namespace petrichor::compiler
