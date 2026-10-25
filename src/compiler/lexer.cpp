#include "compiler/lexer.h"

#include <cerrno>
#include <cstdlib>
#include <locale>
#include <sstream>
#include <unordered_map>

namespace petrichor::compiler {

const char *token_name(TokenKind p_kind) {
	switch (p_kind) {
		case TokenKind::EndOfFile:
			return "end of file";
		case TokenKind::Newline:
			return "line break";
		case TokenKind::Indent:
			return "indent";
		case TokenKind::Dedent:
			return "dedent";
		case TokenKind::Identifier:
			return "identifier";
		case TokenKind::Int:
			return "integer";
		case TokenKind::Float:
			return "float";
		case TokenKind::String:
			return "string";
		case TokenKind::And:
			return "\"and\"";
		case TokenKind::Break:
			return "\"break\"";
		case TokenKind::ClassName:
			return "\"class_name\"";
		case TokenKind::Continue:
			return "\"continue\"";
		case TokenKind::Elif:
			return "\"elif\"";
		case TokenKind::Else:
			return "\"else\"";
		case TokenKind::Extends:
			return "\"extends\"";
		case TokenKind::False:
			return "\"false\"";
		case TokenKind::Func:
			return "\"func\"";
		case TokenKind::If:
			return "\"if\"";
		case TokenKind::Not:
			return "\"not\"";
		case TokenKind::Or:
			return "\"or\"";
		case TokenKind::Pass:
			return "\"pass\"";
		case TokenKind::Pub:
			return "\"pub\"";
		case TokenKind::Return:
			return "\"return\"";
		case TokenKind::Self:
			return "\"self\"";
		case TokenKind::True:
			return "\"true\"";
		case TokenKind::Var:
			return "\"var\"";
		case TokenKind::While:
			return "\"while\"";
		case TokenKind::At:
			return "\"@\"";
		case TokenKind::Question:
			return "\"?\"";
		case TokenKind::Colon:
			return "\":\"";
		case TokenKind::Comma:
			return "\",\"";
		case TokenKind::Period:
			return "\".\"";
		case TokenKind::ParenOpen:
			return "\"(\"";
		case TokenKind::ParenClose:
			return "\")\"";
		case TokenKind::Arrow:
			return "\"->\"";
		case TokenKind::Plus:
			return "\"+\"";
		case TokenKind::Minus:
			return "\"-\"";
		case TokenKind::Star:
			return "\"*\"";
		case TokenKind::Slash:
			return "\"/\"";
		case TokenKind::Percent:
			return "\"%\"";
		case TokenKind::Equal:
			return "\"=\"";
		case TokenKind::PlusEqual:
			return "\"+=\"";
		case TokenKind::MinusEqual:
			return "\"-=\"";
		case TokenKind::StarEqual:
			return "\"*=\"";
		case TokenKind::SlashEqual:
			return "\"/=\"";
		case TokenKind::PercentEqual:
			return "\"%=\"";
		case TokenKind::EqualEqual:
			return "\"==\"";
		case TokenKind::BangEqual:
			return "\"!=\"";
		case TokenKind::Less:
			return "\"<\"";
		case TokenKind::LessEqual:
			return "\"<=\"";
		case TokenKind::Greater:
			return "\">\"";
		case TokenKind::GreaterEqual:
			return "\">=\"";
	}
	return "?";
}

namespace {

const std::unordered_map<std::string, TokenKind> &keywords() {
	static const std::unordered_map<std::string, TokenKind> table = {
		{ "and", TokenKind::And },
		{ "break", TokenKind::Break },
		{ "class_name", TokenKind::ClassName },
		{ "continue", TokenKind::Continue },
		{ "elif", TokenKind::Elif },
		{ "else", TokenKind::Else },
		{ "extends", TokenKind::Extends },
		{ "false", TokenKind::False },
		{ "func", TokenKind::Func },
		{ "if", TokenKind::If },
		{ "not", TokenKind::Not },
		{ "or", TokenKind::Or },
		{ "pass", TokenKind::Pass },
		{ "pub", TokenKind::Pub },
		{ "return", TokenKind::Return },
		{ "self", TokenKind::Self },
		{ "true", TokenKind::True },
		{ "var", TokenKind::Var },
		{ "while", TokenKind::While },
	};
	return table;
}

bool is_identifier_start(unsigned char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80;
}

bool is_identifier_char(unsigned char c) {
	return is_identifier_start(c) || (c >= '0' && c <= '9');
}

bool is_digit(unsigned char c) {
	return c >= '0' && c <= '9';
}

class Lexer {
	const std::string &source;
	std::vector<Diagnostic> &errors;
	std::vector<Token> tokens;

	size_t at = 0;
	int line = 1;
	int column = 1;
	int paren_depth = 0;
	char indent_char = 0;
	std::vector<int> indents = { 0 };
	bool line_has_tokens = false;

	unsigned char peek(size_t p_offset = 0) const {
		return at + p_offset < source.size() ? static_cast<unsigned char>(source[at + p_offset]) : 0;
	}

	void advance() {
		const unsigned char c = peek();
		at++;
		if (c == '\n') {
			line++;
			column = 1;
		} else if ((c & 0xC0) != 0x80) {
			// Columns count code points, not UTF-8 bytes.
			column++;
		}
	}

	void error(int p_line, int p_column, const std::string &p_message) {
		errors.push_back({ p_line, p_column, p_message });
	}

	Token &push(TokenKind p_kind, int p_line, int p_column) {
		tokens.push_back(Token());
		Token &token = tokens.back();
		token.kind = p_kind;
		token.line = p_line;
		token.column = p_column;
		if (p_kind != TokenKind::Newline && p_kind != TokenKind::Indent && p_kind != TokenKind::Dedent) {
			line_has_tokens = true;
		}
		return token;
	}

	// At the start of a line outside parentheses: measures the indentation and
	// emits Indent or Dedent. Blank and comment-only lines are skipped.
	void line_start() {
		while (true) {
			int width = 0;
			const int start_line = line;
			bool mixed = false;
			while (peek() == ' ' || peek() == '\t') {
				const char c = char(peek());
				if (indent_char == 0) {
					indent_char = c;
				} else if (c != indent_char) {
					mixed = true;
				}
				width++;
				advance();
			}
			const unsigned char c = peek();
			if (c == '\r') {
				advance();
				continue;
			}
			if (c == '\n') {
				advance();
				continue;
			}
			if (c == '#') {
				while (peek() != '\n' && peek() != 0) {
					advance();
				}
				continue;
			}
			if (c == 0) {
				return;
			}
			if (mixed) {
				error(start_line, 1, "Indentation mixes tabs and spaces.");
			}
			if (width > indents.back()) {
				indents.push_back(width);
				push(TokenKind::Indent, line, column);
			} else {
				while (width < indents.back()) {
					indents.pop_back();
					push(TokenKind::Dedent, line, column);
				}
				if (width != indents.back()) {
					error(line, column, "Indentation does not match any outer block.");
					indents.push_back(width);
				}
			}
			return;
		}
	}

	void end_line() {
		if (line_has_tokens) {
			push(TokenKind::Newline, line, column);
			line_has_tokens = false;
		}
	}

	void number() {
		const int start_line = line;
		const int start_column = column;
		std::string digits;
		bool is_float = false;
		int base = 10;
		if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X' || peek(1) == 'b' || peek(1) == 'B')) {
			base = (peek(1) == 'x' || peek(1) == 'X') ? 16 : 2;
			advance();
			advance();
		}
		auto take_digits = [&]() {
			while (true) {
				const unsigned char c = peek();
				const bool hex = base == 16 && ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'));
				if (is_digit(c) || hex) {
					digits.push_back(char(c));
					advance();
				} else if (c == '_') {
					advance();
				} else {
					break;
				}
			}
		};
		take_digits();
		if (base == 10 && peek() == '.' && is_digit(peek(1))) {
			is_float = true;
			digits.push_back('.');
			advance();
			take_digits();
		}
		if (base == 10 && (peek() == 'e' || peek() == 'E')) {
			const size_t sign = (peek(1) == '+' || peek(1) == '-') ? 1 : 0;
			if (is_digit(peek(1 + sign))) {
				is_float = true;
				digits.push_back('e');
				advance();
				if (sign) {
					digits.push_back(char(peek()));
					advance();
				}
				take_digits();
			}
		}
		if (digits.empty()) {
			error(start_line, start_column, "Expected digits after the number prefix.");
			digits = "0";
		}

		errno = 0;
		if (is_float) {
			Token &token = push(TokenKind::Float, start_line, start_column);
			// Whatever C locale the host process has set.
			std::istringstream stream(digits);
			stream.imbue(std::locale::classic());
			stream >> token.float_value;
		} else {
			Token &token = push(TokenKind::Int, start_line, start_column);
			const unsigned long long value = std::strtoull(digits.c_str(), nullptr, base);
			if (errno == ERANGE || (base == 10 && value > 9223372036854775808ull)) {
				error(start_line, start_column, "Integer literal is too large for int.");
			}
			token.int_overflows = base == 10 && value == 9223372036854775808ull;
			token.int_value = int64_t(value);
		}
	}

	void string() {
		const int start_line = line;
		const int start_column = column;
		const unsigned char quote = peek();
		// """…""" and '''…''' may span lines, as in GDScript.
		const bool triple = peek(1) == quote && peek(2) == quote;
		for (int i = 0; i < (triple ? 3 : 1); i++) {
			advance();
		}
		std::string value;
		while (true) {
			const unsigned char c = peek();
			if (c == 0 || (c == '\n' && !triple)) {
				error(start_line, start_column, "Unterminated string.");
				break;
			}
			if (c == quote && (!triple || (peek(1) == quote && peek(2) == quote))) {
				for (int i = 0; i < (triple ? 3 : 1); i++) {
					advance();
				}
				break;
			}
			if (c == '\\') {
				advance();
				const unsigned char escaped = peek();
				switch (escaped) {
					case 'n':
						value.push_back('\n');
						break;
					case 't':
						value.push_back('\t');
						break;
					case 'r':
						value.push_back('\r');
						break;
					case '\\':
					case '"':
					case '\'':
						value.push_back(char(escaped));
						break;
					default:
						error(line, column, "Unknown escape sequence.");
						break;
				}
				if (escaped != 0 && escaped != '\n') {
					advance();
				}
				continue;
			}
			value.push_back(char(c));
			advance();
		}
		push(TokenKind::String, start_line, start_column).text = value;
	}

	void punctuation() {
		const int start_line = line;
		const int start_column = column;
		const unsigned char c = peek();
		const unsigned char next = peek(1);
		auto one = [&](TokenKind p_kind) {
			advance();
			push(p_kind, start_line, start_column);
		};
		auto two = [&](TokenKind p_kind) {
			advance();
			advance();
			push(p_kind, start_line, start_column);
		};
		switch (c) {
			case '@':
				one(TokenKind::At);
				return;
			case '?':
				one(TokenKind::Question);
				return;
			case ':':
				one(TokenKind::Colon);
				return;
			case ',':
				one(TokenKind::Comma);
				return;
			case '.':
				one(TokenKind::Period);
				return;
			case '(':
				paren_depth++;
				one(TokenKind::ParenOpen);
				return;
			case ')':
				if (paren_depth > 0) {
					paren_depth--;
				}
				one(TokenKind::ParenClose);
				return;
			case '+':
				next == '=' ? two(TokenKind::PlusEqual) : one(TokenKind::Plus);
				return;
			case '-':
				if (next == '>') {
					two(TokenKind::Arrow);
				} else {
					next == '=' ? two(TokenKind::MinusEqual) : one(TokenKind::Minus);
				}
				return;
			case '*':
				next == '=' ? two(TokenKind::StarEqual) : one(TokenKind::Star);
				return;
			case '/':
				next == '=' ? two(TokenKind::SlashEqual) : one(TokenKind::Slash);
				return;
			case '%':
				next == '=' ? two(TokenKind::PercentEqual) : one(TokenKind::Percent);
				return;
			case '=':
				next == '=' ? two(TokenKind::EqualEqual) : one(TokenKind::Equal);
				return;
			case '!':
				if (next == '=') {
					two(TokenKind::BangEqual);
					return;
				}
				break;
			case '<':
				next == '=' ? two(TokenKind::LessEqual) : one(TokenKind::Less);
				return;
			case '>':
				next == '=' ? two(TokenKind::GreaterEqual) : one(TokenKind::Greater);
				return;
			default:
				break;
		}
		error(start_line, start_column, std::string("Unexpected character \"") + char(c) + "\".");
		advance();
	}

public:
	Lexer(const std::string &p_source, std::vector<Diagnostic> &r_errors) :
			source(p_source), errors(r_errors) {}

	std::vector<Token> run() {
		line_start();
		while (true) {
			const unsigned char c = peek();
			if (c == 0) {
				break;
			}
			if (c == ' ' || c == '\t' || c == '\r') {
				advance();
			} else if (c == '#') {
				while (peek() != '\n' && peek() != 0) {
					advance();
				}
			} else if (c == '\n') {
				// The line break token belongs to the line it ends.
				if (paren_depth == 0) {
					end_line();
					advance();
					line_start();
				} else {
					advance();
				}
			} else if (is_digit(c)) {
				number();
			} else if (is_identifier_start(c)) {
				const int start_line = line;
				const int start_column = column;
				std::string name;
				while (is_identifier_char(peek())) {
					name.push_back(char(peek()));
					advance();
				}
				const auto keyword = keywords().find(name);
				if (keyword != keywords().end()) {
					push(keyword->second, start_line, start_column);
				} else {
					push(TokenKind::Identifier, start_line, start_column).text = name;
				}
			} else if (c == '"' || c == '\'') {
				string();
			} else {
				punctuation();
			}
		}
		end_line();
		while (indents.size() > 1) {
			indents.pop_back();
			push(TokenKind::Dedent, line, column);
		}
		push(TokenKind::EndOfFile, line, column);
		return std::move(tokens);
	}
};

} // namespace

std::vector<Token> tokenize(const std::string &p_source, std::vector<Diagnostic> &r_errors) {
	return Lexer(p_source, r_errors).run();
}

} // namespace petrichor::compiler
