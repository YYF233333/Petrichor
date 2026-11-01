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
		case TokenKind::StringName:
			return "StringName";
		case TokenKind::NodePath:
			return "NodePath";
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
		case TokenKind::Const:
			return "\"const\"";
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
		case TokenKind::AmpersandEqual:
			return "\"&=\"";
		case TokenKind::PipeEqual:
			return "\"|=\"";
		case TokenKind::CaretEqual:
			return "\"^=\"";
		case TokenKind::ShiftLeftEqual:
			return "\"<<=\"";
		case TokenKind::ShiftRightEqual:
			return "\">>=\"";
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
		case TokenKind::Ampersand:
			return "\"&\"";
		case TokenKind::Pipe:
			return "\"|\"";
		case TokenKind::Caret:
			return "\"^\"";
		case TokenKind::Tilde:
			return "\"~\"";
		case TokenKind::ShiftLeft:
			return "\"<<\"";
		case TokenKind::ShiftRight:
			return "\">>\"";
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
		case TokenKind::PlusEqual:
			return TokenKind::Plus;
		case TokenKind::MinusEqual:
			return TokenKind::Minus;
		case TokenKind::StarEqual:
			return TokenKind::Star;
		case TokenKind::SlashEqual:
			return TokenKind::Slash;
		case TokenKind::PercentEqual:
			return TokenKind::Percent;
		case TokenKind::AmpersandEqual:
			return TokenKind::Ampersand;
		case TokenKind::PipeEqual:
			return TokenKind::Pipe;
		case TokenKind::CaretEqual:
			return TokenKind::Caret;
		case TokenKind::ShiftLeftEqual:
			return TokenKind::ShiftLeft;
		case TokenKind::ShiftRightEqual:
			return TokenKind::ShiftRight;
		default:
			return TokenKind::Invalid;
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
		{ "const", TokenKind::Const },
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
	std::vector<std::pair<uint32_t, uint32_t>> *recovered;

	struct Bracket {
		uint32_t offset;
		int indent; // Of its line.
		bool unclosed; // The text never closes it.
	};

	uint32_t at = 0;
	std::vector<Bracket> brackets; // Open ones, innermost last: line breaks inside them do not count.
	uint32_t cleared = 0; // Brackets taken as closed early, whose closing ones may still come.
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

	// False for a closing bracket without an opening one, which becomes trivia.
	bool close_bracket() {
		if (brackets.empty()) {
			return false;
		}
		brackets.pop_back();
		return true;
	}

	void unmatched(uint32_t p_start) {
		advance();
		trivia(TokenKind::Invalid, p_start);
		if (cleared > 0) {
			cleared--; // It closes one taken as closed already.
		} else {
			error(p_start, at, std::string("Unmatched \"") + source[p_start] + "\".");
		}
	}

	// A bracket the text never closes is reported at itself, once; whatever
	// the parser finds wrong up to where it is taken as closed follows from it.
	void never_closed(const Bracket &p_bracket, uint32_t p_close) {
		error(p_bracket.offset, p_bracket.offset + 1, std::string("\"") + source[p_bracket.offset] + "\" is never closed.");
		if (recovered) {
			recovered->push_back({ p_bracket.offset, p_close });
		}
	}

	// Takes the brackets from the one at `p_index` on as closed here.
	void close_from(size_t p_index) {
		for (size_t i = p_index; i < brackets.size(); i++) {
			if (brackets[i].unclosed) {
				never_closed(brackets[i], at);
			} else {
				cleared++;
			}
		}
		brackets.resize(p_index);
	}

	// At a line break inside brackets: takes them as closed where the parser
	// asked for it, and from a bracket the text never closes on where the next
	// line with code is indented no deeper than that bracket's line and does
	// not begin with a closing one.
	void recover_brackets() {
		if (std::binary_search(resets.begin(), resets.end(), at)) {
			close_from(0);
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
				close_from(i);
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

	// A StringName or NodePath literal begins with its prefix, & or ^, and a
	// raw string with r; otherwise they are written as strings
	// (modules/gdscript/gdscript_tokenizer.cpp:849-1120).
	void string(TokenKind p_kind = TokenKind::String, bool p_raw = false) {
		const uint32_t start = at;
		if (p_kind != TokenKind::String || p_raw) {
			advance();
		}
		const unsigned char quote = peek();
		// """…""" and '''…''' may span lines, as in GDScript.
		const bool triple = peek(1) == quote && peek(2) == quote;
		for (int i = 0; i < (triple ? 3 : 1); i++) {
			advance();
		}
		std::string value;
		while (true) {
			if (at_end()) {
				error(start, at, "Unterminated string.");
				break;
			}
			if (peek() == '\n' && !triple) {
				error(start, at, "Unterminated string; a string that spans lines is written in triple quotes (spec §2.2).");
				break;
			}
			const unsigned char c = peek();
			if (c == quote && (!triple || (peek(1) == quote && peek(2) == quote))) {
				for (int i = 0; i < (triple ? 3 : 1); i++) {
					advance();
				}
				break;
			}
			advance();
			if (c != '\\') {
				value.push_back(char(c));
			} else if (p_raw) {
				// The backslash stays, and keeps a quote or a backslash after it
				// from ending the string.
				value.push_back('\\');
				if (peek() == quote || peek() == '\\') {
					value.push_back(char(peek()));
					advance();
				}
			} else {
				escape(at - 1, value);
			}
		}
		push(p_kind, start).text = value;
	}

	// What follows a backslash at `p_begin`
	// (modules/gdscript/gdscript_tokenizer.cpp:929-1062).
	void escape(uint32_t p_begin, std::string &r_value) {
		if (at_end()) {
			return; // Reported as an unterminated string.
		}
		const unsigned char c = peek();
		advance();
		switch (c) {
			case 'a':
				r_value.push_back('\a');
				return;
			case 'b':
				r_value.push_back('\b');
				return;
			case 'f':
				r_value.push_back('\f');
				return;
			case 'n':
				r_value.push_back('\n');
				return;
			case 'r':
				r_value.push_back('\r');
				return;
			case 't':
				r_value.push_back('\t');
				return;
			case 'v':
				r_value.push_back('\v');
				return;
			case '\\':
			case '"':
			case '\'':
				r_value.push_back(char(c));
				return;
			// A backslash at the end of a line continues the string on the next.
			case '\n':
				return;
			case '\r':
				if (peek() == '\n') {
					advance();
					return;
				}
				break;
			case 'u':
			case 'U': {
				uint32_t code = 0;
				if (!hex_digits(c == 'U' ? 6 : 4, code)) {
					return;
				}
				// Beyond U+FFFF also as a UTF-16 pair, such as 😀.
				if (code >= 0xD800 && code <= 0xDBFF && peek() == '\\' && peek(1) == 'u') {
					advance();
					advance();
					uint32_t trail = 0;
					if (!hex_digits(4, trail)) {
						return;
					}
					if (trail >= 0xDC00 && trail <= 0xDFFF) {
						code = 0x10000 + ((code - 0xD800) << 10) + (trail - 0xDC00);
					}
				}
				if ((code >= 0xD800 && code <= 0xDFFF) || code > 0x10FFFF) {
					error(p_begin, at, "Invalid Unicode code point in escape sequence.");
				} else {
					append_utf8(code, r_value);
				}
				return;
			}
			default:
				break;
		}
		error(p_begin, at, "Unknown escape sequence.");
	}

	bool hex_digits(int p_count, uint32_t &r_code) {
		for (int i = 0; i < p_count; i++) {
			const unsigned char c = peek();
			uint32_t digit = 0;
			if (is_digit(c)) {
				digit = c - '0';
			} else if (c >= 'a' && c <= 'f') {
				digit = c - 'a' + 10;
			} else if (c >= 'A' && c <= 'F') {
				digit = c - 'A' + 10;
			} else {
				error(at, at_end() ? at : at + 1, "Invalid hexadecimal digit in Unicode escape sequence.");
				return false;
			}
			r_code = r_code << 4 | digit;
			advance();
		}
		return true;
	}

	static void append_utf8(uint32_t p_code, std::string &r_text) {
		if (p_code < 0x80) {
			r_text.push_back(char(p_code));
		} else if (p_code < 0x800) {
			r_text.push_back(char(0xC0 | p_code >> 6));
			r_text.push_back(char(0x80 | (p_code & 0x3F)));
		} else if (p_code < 0x10000) {
			r_text.push_back(char(0xE0 | p_code >> 12));
			r_text.push_back(char(0x80 | (p_code >> 6 & 0x3F)));
			r_text.push_back(char(0x80 | (p_code & 0x3F)));
		} else {
			r_text.push_back(char(0xF0 | p_code >> 18));
			r_text.push_back(char(0x80 | (p_code >> 12 & 0x3F)));
			r_text.push_back(char(0x80 | (p_code >> 6 & 0x3F)));
			r_text.push_back(char(0x80 | (p_code & 0x3F)));
		}
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
		auto three = [&](TokenKind p_kind) {
			advance();
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
				close_bracket() ? one(TokenKind::ParenClose) : unmatched(start);
				return;
			// Line breaks inside brackets are ignored too, as in GDScript.
			case '[':
				open_bracket();
				one(TokenKind::BracketOpen);
				return;
			case ']':
				close_bracket() ? one(TokenKind::BracketClose) : unmatched(start);
				return;
			case '{':
				open_bracket();
				one(TokenKind::BraceOpen);
				return;
			case '}':
				close_bracket() ? one(TokenKind::BraceClose) : unmatched(start);
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
			// && and || are GDScript's spellings of and and or.
			case '&':
				if (next == '&' || next == '=') {
					two(next == '&' ? TokenKind::And : TokenKind::AmpersandEqual);
				} else if (next == '"' || next == '\'') {
					string(TokenKind::StringName);
				} else {
					one(TokenKind::Ampersand);
				}
				return;
			case '|':
				if (next == '|' || next == '=') {
					two(next == '|' ? TokenKind::Or : TokenKind::PipeEqual);
				} else {
					one(TokenKind::Pipe);
				}
				return;
			case '^':
				if (next == '=') {
					two(TokenKind::CaretEqual);
				} else if (next == '"' || next == '\'') {
					string(TokenKind::NodePath);
				} else {
					one(TokenKind::Caret);
				}
				return;
			case '~':
				one(TokenKind::Tilde);
				return;
			case '<':
				if (next == '<') {
					peek(2) == '=' ? three(TokenKind::ShiftLeftEqual) : two(TokenKind::ShiftLeft);
				} else {
					next == '=' ? two(TokenKind::LessEqual) : one(TokenKind::Less);
				}
				return;
			case '>':
				if (next == '>') {
					peek(2) == '=' ? three(TokenKind::ShiftRightEqual) : two(TokenKind::ShiftRight);
				} else {
					next == '=' ? two(TokenKind::GreaterEqual) : one(TokenKind::Greater);
				}
				return;
			default:
				break;
		}
		advance();
		trivia(TokenKind::Invalid, start);
		error(start, at, std::string("Unexpected character \"") + char(c) + "\".");
	}

public:
	Lexer(SourceFile &r_file, const std::vector<uint32_t> &p_resets, const std::vector<uint32_t> &p_unclosed, std::vector<Diagnostic> &r_errors, std::vector<std::pair<uint32_t, uint32_t>> *r_recovered) :
			file(r_file), source(r_file.text), resets(p_resets), unclosed(p_unclosed), errors(r_errors), recovered(r_recovered) {}

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
			} else if (c == 'r' && (peek(1) == '"' || peek(1) == '\'')) {
				string(TokenKind::String, true);
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
			if (bracket.unclosed) {
				never_closed(bracket, at);
			}
		}
		return open;
	}
};

} // namespace

std::vector<uint32_t> lex(SourceFile &r_file, const std::vector<uint32_t> &p_resets, const std::vector<uint32_t> &p_unclosed, std::vector<Diagnostic> &r_errors, std::vector<std::pair<uint32_t, uint32_t>> *r_recovered) {
	r_file.tokens.clear();
	r_file.trivia.clear();
	r_file.line_starts = { 0 };
	for (uint32_t i = 0; i < r_file.text.size(); i++) {
		if (r_file.text[i] == '\n') {
			r_file.line_starts.push_back(i + 1);
		}
	}
	return Lexer(r_file, p_resets, p_unclosed, r_errors, r_recovered).run();
}

std::shared_ptr<const SourceFile> tokenize(std::string p_source, std::vector<Diagnostic> &r_errors) {
	auto file = std::make_shared<SourceFile>();
	file->text = std::move(p_source);
	lex(*file, {}, {}, r_errors);
	return file;
}

} // namespace petrichor::compiler
