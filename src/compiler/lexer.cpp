#include "compiler/lexer.h"

#include <algorithm>
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
		case TokenKind::Async:
			return "async";
		case TokenKind::Await:
			return "await";
		case TokenKind::And:
			return "\"and\"";
		case TokenKind::As:
			return "\"as\"";
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
		case TokenKind::For:
			return "\"for\"";
		case TokenKind::Func:
			return "\"func\"";
		case TokenKind::If:
			return "\"if\"";
		case TokenKind::Implements:
			return "\"implements\"";
		case TokenKind::In:
			return "\"in\"";
		case TokenKind::Interface:
			return "\"interface\"";
		case TokenKind::Is:
			return "\"is\"";
		case TokenKind::Match:
			return "\"match\"";
		case TokenKind::None:
			return "\"None\"";
		case TokenKind::Null:
			return "\"null\"";
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
		case TokenKind::Super:
			return "super";
		case TokenKind::Self:
			return "\"self\"";
		case TokenKind::Signal:
			return "\"signal\"";
		case TokenKind::Static:
			return "\"static\"";
		case TokenKind::Struct:
			return "\"struct\"";
		case TokenKind::True:
			return "\"true\"";
		case TokenKind::Var:
			return "\"var\"";
		case TokenKind::When:
			return "\"when\"";
		case TokenKind::While:
			return "\"while\"";
		case TokenKind::At:
			return "\"@\"";
		case TokenKind::Dollar:
			return "\"$\"";
		case TokenKind::Question:
			return "\"?\"";
		case TokenKind::Bang:
			return "\"!\"";
		case TokenKind::Colon:
			return "\":\"";
		case TokenKind::Semicolon:
			return "\";\"";
		case TokenKind::Comma:
			return "\",\"";
		case TokenKind::Period:
			return "\".\"";
		case TokenKind::ParenOpen:
			return "\"(\"";
		case TokenKind::ParenClose:
			return "\")\"";
		case TokenKind::BracketOpen:
			return "\"[\"";
		case TokenKind::BracketClose:
			return "\"]\"";
		case TokenKind::BraceOpen:
			return "\"{\"";
		case TokenKind::BraceClose:
			return "\"}\"";
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
		case TokenKind::Comment:
			return "comment";
		case TokenKind::LineContinuation:
			return "line continuation";
		case TokenKind::Invalid:
			return "invalid character";
	}
	return "?";
}

SourcePosition SourceFile::position(uint32_t p_offset) const {
	const auto next = std::upper_bound(line_starts.begin(), line_starts.end(), p_offset);
	const uint32_t start = *(next - 1);
	int column = 1;
	for (uint32_t at = start; at < p_offset && at < text.size(); at++) {
		// Columns count code points, not UTF-8 bytes.
		if ((static_cast<unsigned char>(text[at]) & 0xC0) != 0x80) {
			column++;
		}
	}
	return { int(next - line_starts.begin()), column };
}

TokenKind compound_operator(TokenKind p_assign) {
	switch (p_assign) {
		case TokenKind::MinusEqual:
			return TokenKind::Minus;
		case TokenKind::StarEqual:
			return TokenKind::Star;
		case TokenKind::SlashEqual:
			return TokenKind::Slash;
		case TokenKind::PercentEqual:
			return TokenKind::Percent;
		default:
			return TokenKind::Plus;
	}
}

namespace {

const std::unordered_map<std::string, TokenKind> &keywords() {
	static const std::unordered_map<std::string, TokenKind> table = {
		{ "async", TokenKind::Async },
		{ "await", TokenKind::Await },
		{ "and", TokenKind::And },
		{ "as", TokenKind::As },
		{ "break", TokenKind::Break },
		{ "class_name", TokenKind::ClassName },
		{ "continue", TokenKind::Continue },
		{ "elif", TokenKind::Elif },
		{ "else", TokenKind::Else },
		{ "extends", TokenKind::Extends },
		{ "false", TokenKind::False },
		{ "for", TokenKind::For },
		{ "func", TokenKind::Func },
		{ "if", TokenKind::If },
		{ "implements", TokenKind::Implements },
		{ "in", TokenKind::In },
		{ "interface", TokenKind::Interface },
		{ "is", TokenKind::Is },
		{ "match", TokenKind::Match },
		{ "None", TokenKind::None },
		{ "null", TokenKind::Null },
		{ "not", TokenKind::Not },
		{ "or", TokenKind::Or },
		{ "pass", TokenKind::Pass },
		{ "pub", TokenKind::Pub },
		{ "return", TokenKind::Return },
		{ "self", TokenKind::Self },
		{ "super", TokenKind::Super },
		{ "signal", TokenKind::Signal },
		{ "static", TokenKind::Static },
		{ "struct", TokenKind::Struct },
		{ "true", TokenKind::True },
		{ "var", TokenKind::Var },
		{ "when", TokenKind::When },
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
	SourceFile &file;
	const std::string &source;
	const std::vector<uint32_t> &resets;
	const std::vector<uint32_t> &unclosed;
	std::vector<Diagnostic> &errors;

	struct Bracket {
		uint32_t offset;
		int indent; // Of its line.
		bool unclosed; // The text never closes it.
	};

	uint32_t at = 0;
	std::vector<Bracket> brackets; // Open ones, innermost last: line breaks inside them do not count.
	char indent_char = 0;
	std::vector<int> indents = { 0 };
	bool line_has_tokens = false;

	bool at_end() const {
		return at >= source.size();
	}

	unsigned char peek(size_t p_offset = 0) const {
		return at + p_offset < source.size() ? static_cast<unsigned char>(source[at + p_offset]) : 0;
	}

	void advance() {
		at++;
	}

	void error(uint32_t p_begin, uint32_t p_end, const std::string &p_message) {
		errors.push_back({ p_begin, p_end, p_message });
	}

	// A token from `p_start` to here.
	Token &push(TokenKind p_kind, uint32_t p_start) {
		file.tokens.push_back(Token());
		Token &token = file.tokens.back();
		token.kind = p_kind;
		token.offset = p_start;
		token.length = at - p_start;
		if (p_kind != TokenKind::Newline && p_kind != TokenKind::Indent && p_kind != TokenKind::Dedent) {
			line_has_tokens = true;
		}
		return token;
	}

	int line_indent(uint32_t p_offset) const {
		uint32_t start = p_offset;
		while (start > 0 && source[start - 1] != '\n') {
			start--;
		}
		int width = 0;
		while (start + width < source.size() && (source[start + width] == ' ' || source[start + width] == '\t')) {
			width++;
		}
		return width;
	}

	void open_bracket() {
		brackets.push_back({ at, line_indent(at), std::binary_search(unclosed.begin(), unclosed.end(), at) });
	}

	void close_bracket() {
		if (!brackets.empty()) {
			brackets.pop_back();
		}
	}

	// At a line break inside brackets: takes them as closed where the parser
	// asked for it, and from a bracket the text never closes on where the next
	// line with code is indented no deeper than that bracket's line and does
	// not begin with a closing one.
	void recover_brackets() {
		if (std::binary_search(resets.begin(), resets.end(), at)) {
			brackets.clear();
			return;
		}
		uint32_t next = at + 1;
		int width = 0;
		while (true) {
			width = 0;
			uint32_t i = next;
			while (i < source.size() && (source[i] == ' ' || source[i] == '\t')) {
				i++;
				width++;
			}
			if (i >= source.size()) {
				return;
			}
			const char c = source[i];
			if (c == '\r' || c == '\n' || c == '#') {
				while (i < source.size() && source[i] != '\n') {
					i++;
				}
				next = i + 1;
				continue;
			}
			if (c == ')' || c == ']' || c == '}') {
				return;
			}
			break;
		}
		for (size_t i = 0; i < brackets.size(); i++) {
			if (brackets[i].unclosed && width <= brackets[i].indent) {
				brackets.resize(i);
				return;
			}
		}
	}

	void trivia(TokenKind p_kind, uint32_t p_start) {
		Token token;
		token.kind = p_kind;
		token.offset = p_start;
		token.length = at - p_start;
		file.trivia.push_back(token);
	}

	// From "#" to the end of the line, which a carriage return may precede.
	void comment() {
		const uint32_t start = at;
		while (!at_end() && peek() != '\n' && !(peek() == '\r' && peek(1) == '\n')) {
			advance();
		}
		trivia(TokenKind::Comment, start);
	}

	// At the start of a line outside parentheses: measures the indentation and
	// emits Indent or Dedent. Blank and comment-only lines are skipped.
	void line_start() {
		while (true) {
			const uint32_t start = at;
			int width = 0;
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
			if (at_end()) {
				return;
			}
			const unsigned char c = peek();
			if (c == '\r' || c == '\n') {
				advance();
				continue;
			}
			if (c == '#') {
				comment();
				continue;
			}
			if (mixed) {
				error(start, at, "Indentation mixes tabs and spaces.");
			}
			if (width > indents.back()) {
				indents.push_back(width);
				push(TokenKind::Indent, at);
			} else {
				while (width < indents.back()) {
					indents.pop_back();
					push(TokenKind::Dedent, at);
				}
				if (width != indents.back()) {
					// A block of its own, so that dedents still balance indents.
					error(start, at, "Indentation does not match any outer block.");
					indents.push_back(width);
					push(TokenKind::Indent, at);
				}
			}
			return;
		}
	}

	void end_line() {
		if (line_has_tokens) {
			push(TokenKind::Newline, at);
			line_has_tokens = false;
		}
	}

	void number() {
		const uint32_t start = at;
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
				if ((base == 2 ? c == '0' || c == '1' : is_digit(c)) || hex) {
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
		// A digit the base does not have, or a letter, ends the number in an
		// error: 0b102, 0xFG, 12px, as in GDScript
		// (modules/gdscript/gdscript_tokenizer.cpp:824-827).
		const bool trailing = is_identifier_char(peek());
		while (is_identifier_char(peek())) {
			advance();
		}
		if (digits.empty()) {
			error(start, at, "Expected digits after the number prefix.");
			digits = "0";
		} else if (trailing) {
			error(start, at, "Invalid numeric notation.");
		}

		errno = 0;
		if (is_float) {
			Token &token = push(TokenKind::Float, start);
			// Whatever C locale the host process has set.
			std::istringstream stream(digits);
			stream.imbue(std::locale::classic());
			stream >> token.float_value;
		} else {
			Token &token = push(TokenKind::Int, start);
			const unsigned long long value = std::strtoull(digits.c_str(), nullptr, base);
			if (errno == ERANGE || (base == 10 && value > 9223372036854775808ull)) {
				error(start, at, "Integer literal is too large for int.");
			}
			token.int_overflows = base == 10 && value == 9223372036854775808ull;
			token.int_value = int64_t(value);
		}
	}

	void string() {
		const uint32_t start = at;
		const unsigned char quote = peek();
		// """…""" and '''…''' may span lines, as in GDScript.
		const bool triple = peek(1) == quote && peek(2) == quote;
		for (int i = 0; i < (triple ? 3 : 1); i++) {
			advance();
		}
		std::string value;
		while (true) {
			if (at_end() || (peek() == '\n' && !triple)) {
				error(start, at, "Unterminated string.");
				break;
			}
			const unsigned char c = peek();
			if (c == quote && (!triple || (peek(1) == quote && peek(2) == quote))) {
				for (int i = 0; i < (triple ? 3 : 1); i++) {
					advance();
				}
				break;
			}
			if (c == '\\') {
				const uint32_t escape = at;
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
						error(escape, at_end() ? at : at + 1, "Unknown escape sequence.");
						break;
				}
				if (!at_end() && escaped != '\n') {
					advance();
				}
				continue;
			}
			value.push_back(char(c));
			advance();
		}
		push(TokenKind::String, start).text = value;
	}

	void punctuation() {
		const uint32_t start = at;
		const unsigned char c = peek();
		const unsigned char next = peek(1);
		auto one = [&](TokenKind p_kind) {
			advance();
			push(p_kind, start);
		};
		auto two = [&](TokenKind p_kind) {
			advance();
			advance();
			push(p_kind, start);
		};
		switch (c) {
			case '@':
				one(TokenKind::At);
				return;
			case '$':
				one(TokenKind::Dollar);
				return;
			case '?':
				one(TokenKind::Question);
				return;
			case ':':
				one(TokenKind::Colon);
				return;
			case ';':
				one(TokenKind::Semicolon);
				return;
			case ',':
				one(TokenKind::Comma);
				return;
			case '.':
				one(TokenKind::Period);
				return;
			case '(':
				open_bracket();
				one(TokenKind::ParenOpen);
				return;
			case ')':
				close_bracket();
				one(TokenKind::ParenClose);
				return;
			// Line breaks inside brackets are ignored too, as in GDScript.
			case '[':
				open_bracket();
				one(TokenKind::BracketOpen);
				return;
			case ']':
				close_bracket();
				one(TokenKind::BracketClose);
				return;
			case '{':
				open_bracket();
				one(TokenKind::BraceOpen);
				return;
			case '}':
				close_bracket();
				one(TokenKind::BraceClose);
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
				next == '=' ? two(TokenKind::BangEqual) : one(TokenKind::Bang);
				return;
			// GDScript's spellings of and and or.
			case '&':
				if (next == '&') {
					two(TokenKind::And);
					return;
				}
				break;
			case '|':
				if (next == '|') {
					two(TokenKind::Or);
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
		advance();
		trivia(TokenKind::Invalid, start);
		error(start, at, std::string("Unexpected character \"") + char(c) + "\".");
	}

public:
	Lexer(SourceFile &r_file, const std::vector<uint32_t> &p_resets, const std::vector<uint32_t> &p_unclosed, std::vector<Diagnostic> &r_errors) :
			file(r_file), source(r_file.text), resets(p_resets), unclosed(p_unclosed), errors(r_errors) {}

	// Returns the brackets open at the end.
	std::vector<uint32_t> run() {
		line_start();
		while (!at_end()) {
			const unsigned char c = peek();
			if (c == ' ' || c == '\t' || c == '\r') {
				advance();
			} else if (c == '#') {
				comment();
			} else if (c == '\\' && (peek(1) == '\n' || (peek(1) == '\r' && peek(2) == '\n'))) {
				// A backslash at the end of a line continues it, as in GDScript.
				const uint32_t start = at;
				advance();
				trivia(TokenKind::LineContinuation, start);
				while (peek() != '\n') {
					advance();
				}
				advance();
			} else if (c == '\n') {
				if (!brackets.empty()) {
					recover_brackets();
				}
				// The line break token belongs to the line it ends.
				if (brackets.empty()) {
					end_line();
					advance();
					line_start();
				} else {
					advance();
				}
			} else if (is_digit(c)) {
				number();
			} else if (is_identifier_start(c)) {
				const uint32_t start = at;
				while (is_identifier_char(peek())) {
					advance();
				}
				std::string name = source.substr(start, at - start);
				const auto keyword = keywords().find(name);
				// A keyword keeps its spelling, since a node path may use it as a name.
				push(keyword != keywords().end() ? keyword->second : TokenKind::Identifier, start).text = std::move(name);
			} else if (c == '"' || c == '\'') {
				string();
			} else {
				punctuation();
			}
		}
		end_line();
		while (indents.size() > 1) {
			indents.pop_back();
			push(TokenKind::Dedent, at);
		}
		push(TokenKind::EndOfFile, at);
		std::vector<uint32_t> open;
		for (const Bracket &bracket : brackets) {
			open.push_back(bracket.offset);
		}
		return open;
	}
};

} // namespace

std::vector<uint32_t> lex(SourceFile &r_file, const std::vector<uint32_t> &p_resets, const std::vector<uint32_t> &p_unclosed, std::vector<Diagnostic> &r_errors) {
	r_file.tokens.clear();
	r_file.trivia.clear();
	r_file.line_starts = { 0 };
	for (uint32_t i = 0; i < r_file.text.size(); i++) {
		if (r_file.text[i] == '\n') {
			r_file.line_starts.push_back(i + 1);
		}
	}
	return Lexer(r_file, p_resets, p_unclosed, r_errors).run();
}

std::shared_ptr<const SourceFile> tokenize(std::string p_source, std::vector<Diagnostic> &r_errors) {
	auto file = std::make_shared<SourceFile>();
	file->text = std::move(p_source);
	lex(*file, {}, {}, r_errors);
	return file;
}

} // namespace petrichor::compiler
