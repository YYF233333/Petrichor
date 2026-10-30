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
		TypeRef ref{ name.text, name.line, name.column };
		// A type declared in a class, such as Combat.Damageable.
		while (!failed && check(TokenKind::Period) && peek(1).kind == TokenKind::Identifier) {
			advance();
			ref.name += "." + advance().text;
		}
		if (match(TokenKind::BracketOpen)) {
			do {
				ref.args.push_back(type_ref());
			} while (!failed && match(TokenKind::Comma));
			expect(TokenKind::BracketClose, "\"]\"");
		}
		ref.nullable = match(TokenKind::Question);
		return ref;
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

	// `as` binds loosest, then `a if c else b`, as in GDScript
	// (gdscript_parser.h:1435-1436).
	std::unique_ptr<Expr> expression() {
		auto expr = conditional();
		while (expr && check(TokenKind::As)) {
			auto cast = make(ExprKind::Cast, advance());
			cast->cast_type = type_ref();
			if (failed) {
				return nullptr;
			}
			cast->left = std::move(expr);
			expr = std::move(cast);
		}
		return expr;
	}

	std::unique_ptr<Expr> conditional() {
		auto expr = or_expr();
		if (!expr || !check(TokenKind::If)) {
			return expr;
		}
		auto result = make(ExprKind::Conditional, advance());
		result->line = expr->line;
		result->column = expr->column;
		result->left = std::move(expr);
		result->base = conditional();
		expect(TokenKind::Else, "\"else\"");
		if (failed || !result->base) {
			return nullptr;
		}
		result->right = conditional();
		return result->right ? std::move(result) : nullptr;
	}

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
		// `!` is `not`, as in GDScript.
		if (check(TokenKind::Not) || check(TokenKind::Bang)) {
			const Token &op = advance();
			auto expr = make(ExprKind::Unary, op);
			expr->op = TokenKind::Not;
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
		if (check(TokenKind::Await)) {
			auto expr = make(ExprKind::Await, advance());
			expr->left = unary();
			return expr->left ? std::move(expr) : nullptr;
		}
		if (check(TokenKind::Minus)) {
			const Token &op = advance();
			auto expr = make(ExprKind::Unary, op);
			expr->op = op.kind;
			expr->left = unary();
			if (expr->left && expr->left->kind == ExprKind::IntLiteral && expr->left->int_overflows) {
				// -9223372036854775808 is the smallest int; its magnitude alone is not.
				auto literal = std::move(expr->left);
				literal->int_overflows = false;
				literal->line = op.line;
				literal->column = op.column;
				return literal;
			}
			return expr->left ? std::move(expr) : nullptr;
		}
		return type_test();
	}

	// `x is T` and `x is not T` bind tighter than arithmetic, as in GDScript
	// (gdscript_parser.h:1451).
	std::unique_ptr<Expr> type_test() {
		auto expr = postfix();
		while (expr && check(TokenKind::Is)) {
			auto test = make(ExprKind::TypeTest, advance());
			test->line = expr->line;
			test->column = expr->column;
			test->bool_value = match(TokenKind::Not);
			test->cast_type = type_ref();
			if (failed) {
				return nullptr;
			}
			test->left = std::move(expr);
			expr = std::move(test);
		}
		return expr;
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
		while (expr) {
			if (check(TokenKind::ParenOpen) && (expr->kind == ExprKind::Name || expr->kind == ExprKind::Member)) {
				// `f(...)` or `base.f(...)`.
				auto call = make(ExprKind::Call, peek());
				call->line = expr->line;
				call->column = expr->column;
				call->text = expr->text;
				call->base = std::move(expr->base);
				if (!call_arguments(*call)) {
					return nullptr;
				}
				expr = std::move(call);
			} else if (check(TokenKind::Period)) {
				advance();
				const Token &name = expect(TokenKind::Identifier, "a member name");
				if (failed) {
					return nullptr;
				}
				auto member = make(ExprKind::Member, name);
				member->text = name.text;
				member->base = std::move(expr);
				expr = std::move(member);
			} else if (check(TokenKind::BracketOpen)) {
				auto subscript = make(ExprKind::Subscript, advance());
				subscript->right = expression();
				expect(TokenKind::BracketClose, "\"]\"");
				if (failed || !subscript->right) {
					return nullptr;
				}
				subscript->base = std::move(expr);
				expr = std::move(subscript);
			} else {
				break;
			}
		}
		return expr;
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
			return expr;
		}
		if (!is_node_name(peek().kind)) {
			fail(peek(), std::string("Expected a node path after \"$\", found ") + token_name(peek().kind) + ".");
			return nullptr;
		}
		expr->text = advance().text;
		while (check(TokenKind::Slash) && is_node_name(peek(1).kind)) {
			advance();
			expr->text += "/" + advance().text;
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
				while (!check(TokenKind::BracketClose)) {
					auto element = expression();
					if (!element) {
						return nullptr;
					}
					expr->args.push_back(std::move(element));
					if (!match(TokenKind::Comma)) {
						break;
					}
				}
				expect(TokenKind::BracketClose, "\"]\"");
				return failed ? nullptr : std::move(expr);
			}
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
		// `a = 1; b = 2`: the next statement follows on the same line.
		if (match(TokenKind::Semicolon) && !check(TokenKind::Newline) && !check(TokenKind::Dedent) && !check(TokenKind::EndOfFile)) {
			return;
		}
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
			if (auto branch = if_statement(elif)) {
				stmt->else_body.push_back(std::move(branch));
			}
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
			case TokenKind::For: {
				advance();
				auto stmt = make_stmt(StmtKind::For, token);
				const Token &name = expect(TokenKind::Identifier, "a variable name");
				stmt->name = name.text;
				stmt->line = name.line;
				stmt->column = name.column;
				if (match(TokenKind::Colon)) {
					stmt->declared_type = type_ref();
				}
				expect(TokenKind::In, "\"in\"");
				if (failed) {
					return nullptr;
				}
				stmt->value = expression();
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

	// After "(": parameters up to the ")".
	std::vector<ParamDecl> params() {
		std::vector<ParamDecl> result;
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
				if (match(TokenKind::Equal)) {
					param.default_value = expression();
				}
				result.push_back(std::move(param));
			} while (!failed && match(TokenKind::Comma));
		}
		expect(TokenKind::ParenClose, "\")\"");
		return result;
	}

	// After "interface": the name and a block of method signatures (spec §3.9).
	InterfaceDecl interface(bool p_is_pub) {
		InterfaceDecl decl;
		const Token &name = expect(TokenKind::Identifier, "an interface name");
		decl.name = name.text;
		decl.line = name.line;
		decl.column = name.column;
		decl.is_pub = p_is_pub;
		expect(TokenKind::Colon, "\":\"");
		expect(TokenKind::Newline, "a line break");
		expect(TokenKind::Indent, "an indented block");
		while (!failed && !check(TokenKind::Dedent) && !check(TokenKind::EndOfFile)) {
			if (match(TokenKind::Pass)) {
				end_of_statement();
				continue;
			}
			FuncDecl method;
			method.is_async = match(TokenKind::Async);
			expect(TokenKind::Func, "\"func\"");
			const Token &method_name = expect(TokenKind::Identifier, "a method name");
			method.name = method_name.text;
			method.line = method_name.line;
			method.column = method_name.column;
			method.is_pub = true;
			expect(TokenKind::ParenOpen, "\"(\"");
			method.params = params();
			if (match(TokenKind::Arrow)) {
				method.return_type_ref = type_ref();
			}
			end_of_statement();
			decl.methods.push_back(std::move(method));
		}
		match(TokenKind::Dedent);
		return decl;
	}

	static bool is_group_annotation(const std::string &p_name) {
		return p_name == "export_group" || p_name == "export_subgroup" || p_name == "export_category";
	}

	// The annotations of a class (spec §2.3, §7.3).
	static bool is_class_annotation(const std::string &p_name) {
		return p_name == "tool" || p_name == "icon" || p_name == "static_unload" || p_name == "abstract" || p_name == "parent" || p_name == "owner";
	}

	void member(ClassDecl &r_class) {
		std::vector<Annotation> annotations;
		while (match(TokenKind::At)) {
			const Token &name = expect(TokenKind::Identifier, "an annotation name");
			if (failed) {
				return;
			}
			Annotation annotation;
			annotation.name = name.text;
			annotation.line = name.line;
			annotation.column = name.column;
			if (check(TokenKind::ParenOpen)) {
				Expr holder;
				if (!call_arguments(holder)) {
					return;
				}
				annotation.args = std::move(holder.args);
			}
			if (is_group_annotation(annotation.name)) {
				// Stands alone, as in GDScript.
				r_class.groups.push_back({ std::move(annotation), r_class.members.size() });
				end_of_statement();
				return;
			}
			if (annotation.name != "export" && annotation.name != "export_range" && annotation.name != "onready") {
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
			decl.line = name.line;
			decl.column = name.column;
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
			end_of_statement();
			r_class.members.push_back(std::move(decl));
			return;
		}

		if (match(TokenKind::Interface)) {
			if (!annotations.empty() || is_static) {
				fail(peek(), "An interface takes no annotations and is not static.");
				return;
			}
			r_class.interfaces.push_back(interface(is_pub));
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
			decl.line = name.line;
			decl.column = name.column;
			decl.is_pub = is_pub;
			// `signal died` has no arguments, as in GDScript.
			if (match(TokenKind::ParenOpen)) {
				decl.params = params();
			}
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
			decl.line = name.line;
			decl.column = name.column;
			decl.is_pub = is_pub;
			decl.is_static = is_static;
			decl.is_async = is_async;
			expect(TokenKind::ParenOpen, "\"(\"");
			decl.params = params();
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

	// An expression that is the whole source.
	std::unique_ptr<Expr> single_expression() {
		auto expr = expression();
		match(TokenKind::Newline);
		if (expr && !check(TokenKind::EndOfFile)) {
			fail(peek(), std::string("Expected the end of the expression, found ") + token_name(peek().kind) + ".");
			return nullptr;
		}
		return expr;
	}

	ClassDecl run() {
		ClassDecl result;
		bool has_class_name = false;
		bool has_extends = false;
		bool seen_member = false;
		while (!check(TokenKind::EndOfFile)) {
			if (failed) {
				recover();
				// A stray indented block at class level.
				while (match(TokenKind::Dedent) || match(TokenKind::Indent)) {
				}
				continue;
			}
			if (match(TokenKind::Newline)) {
				continue;
			}
			// Class annotations open the header (spec §2.1, §2.3).
			if (!seen_member && check(TokenKind::At) && is_class_annotation(peek(1).text)) {
				advance();
				const Token &name = advance();
				errors.push_back({ name.line, name.column, "The class annotation @" + name.text + " is not supported yet." });
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
				result.extends_line = base.line;
				result.extends_column = base.column;
				has_extends = true;
				// `extends Node3D implements Combat.Damageable, Saveable` (spec §3.9).
				if (match(TokenKind::Implements)) {
					do {
						result.implements.push_back(type_ref());
					} while (!failed && match(TokenKind::Comma));
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

ClassDecl parse(const std::vector<Token> &p_tokens, std::vector<Diagnostic> &r_errors) {
	return Parser(p_tokens, r_errors).run();
}

std::unique_ptr<Expr> parse_expression(const std::string &p_source, std::vector<Diagnostic> &r_errors) {
	const size_t errors = r_errors.size();
	const std::vector<Token> tokens = tokenize(p_source, r_errors);
	std::unique_ptr<Expr> expr = Parser(tokens, r_errors).single_expression();
	return r_errors.size() == errors ? std::move(expr) : nullptr;
}

} // namespace petrichor::compiler
