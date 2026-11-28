#include "compiler/lexer.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace petrichor::compiler {

#define PETRICHOR_TOKEN_NONE(m_name, m_text)
#define PETRICHOR_TOKEN_NAME(m_name, m_text) m_text,
#define PETRICHOR_TOKEN_QUOTED(m_name, m_text) "\"" m_text "\"",
#define PETRICHOR_TOKEN_SPELLING(m_name, m_text) { m_text, TokenKind::m_name },

const char *token_name(TokenKind p_kind) {
	static const char *const names[] = { PETRICHOR_TOKENS(PETRICHOR_TOKEN_NAME, PETRICHOR_TOKEN_QUOTED, PETRICHOR_TOKEN_QUOTED) };
	return names[int(p_kind)];
}

size_t node_path_end(const std::vector<Token> &p_tokens, size_t p_start, bool &r_complete) {
	bool unique = p_tokens[p_start].kind == TokenKind::Percent;
	size_t i = p_start + 1;
	if (!unique && p_tokens[i].kind == TokenKind::Slash) {
		i++;
	}
	while (true) {
		if (!unique && p_tokens[i].kind == TokenKind::Percent) {
			i++;
		}
		unique = false;
		r_complete = p_tokens[i].kind == TokenKind::String || is_node_name(p_tokens[i].kind);
		if (!r_complete || p_tokens[++i].kind != TokenKind::Slash) {
			return i;
		}
		const TokenKind after = p_tokens[i + 1].kind;
		if (after != TokenKind::String && after != TokenKind::Percent && !is_node_name(after)) {
			return i;
		}
		i++;
	}
}

std::vector<uint32_t> line_starts_of(const std::string &p_text) {
	std::vector<uint32_t> starts{ 0 };
	for (size_t at = p_text.find('\n'); at != std::string::npos; at = p_text.find('\n', at + 1)) {
		starts.push_back(uint32_t(at + 1));
	}
	return starts;
}

SourcePosition position_in(const std::string &p_text, const std::vector<uint32_t> &p_line_starts, uint32_t p_offset) {
	const auto next = std::upper_bound(p_line_starts.begin(), p_line_starts.end(), p_offset);
	const uint32_t start = *(next - 1);
	int column = 1;
	for (uint32_t at = start; at < p_offset && at < p_text.size(); at++) {
		// Columns count code points, not UTF-8 bytes.
		if ((static_cast<unsigned char>(p_text[at]) & 0xC0) != 0x80) {
			column++;
		}
	}
	return { int(next - p_line_starts.begin()), column };
}

namespace {

// Of the code point whose UTF-8 begins with `p_byte`; none for a byte inside one.
uint32_t utf16_units(char p_byte) {
	const unsigned char byte = static_cast<unsigned char>(p_byte);
	if ((byte & 0xC0) == 0x80) {
		return 0;
	}
	return byte >= 0xF0 ? 2 : 1;
}

} // namespace

std::vector<Utf16Position> utf16_positions(const std::string &p_text, const std::vector<uint32_t> &p_offsets) {
	std::vector<size_t> order(p_offsets.size());
	for (size_t i = 0; i < order.size(); i++) {
		order[i] = i;
	}
	std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return p_offsets[a] < p_offsets[b]; });
	std::vector<Utf16Position> result(p_offsets.size());
	Utf16Position at;
	size_t byte = 0;
	for (const size_t index : order) {
		for (const size_t target = std::min<size_t>(p_offsets[index], p_text.size()); byte < target; byte++) {
			const char c = p_text[byte];
			if (c == '\n' || (c == '\r' && p_text.compare(byte, 2, "\r\n") != 0)) {
				at = { at.line + 1, 0 };
			} else if (c != '\r') {
				at.character += utf16_units(c);
			}
		}
		result[index] = at;
	}
	return result;
}

uint32_t utf16_offset(const std::string &p_text, Utf16Position p_position) {
	size_t byte = 0;
	for (uint32_t line = 0; line < p_position.line; line++) {
		const size_t end = p_text.find_first_of("\r\n", byte);
		if (end == std::string::npos) {
			return uint32_t(p_text.size());
		}
		byte = end + (p_text.compare(end, 2, "\r\n") == 0 ? 2 : 1);
	}
	for (uint32_t units = 0; byte < p_text.size() && p_text[byte] != '\n' && p_text[byte] != '\r';) {
		units += utf16_units(p_text[byte]);
		if (units > p_position.character) {
			break;
		}
		do {
			byte++;
		} while (byte < p_text.size() && utf16_units(p_text[byte]) == 0);
	}
	return uint32_t(byte);
}

TokenKind compound_operator(TokenKind p_assign) {
	switch (p_assign) {
		case TokenKind::PlusEqual:
			return TokenKind::Plus;
		case TokenKind::MinusEqual:
			return TokenKind::Minus;
		case TokenKind::StarEqual:
			return TokenKind::Star;
		case TokenKind::StarStarEqual:
			return TokenKind::StarStar;
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

bool ends_operand(TokenKind p_kind) {
	switch (p_kind) {
		case TokenKind::Identifier:
		case TokenKind::Int:
		case TokenKind::Float:
		case TokenKind::String:
		case TokenKind::StringName:
		case TokenKind::NodePath:
		case TokenKind::True:
		case TokenKind::False:
		case TokenKind::Null:
		case TokenKind::Self:
		case TokenKind::Super:
		case TokenKind::ParenClose:
		case TokenKind::BracketClose:
		case TokenKind::BraceClose:
		case TokenKind::Question:
			return true;
		default:
			return false;
	}
}

namespace {

const std::unordered_map<std::string, TokenKind> &keywords() {
	static const std::unordered_map<std::string, TokenKind> table = { PETRICHOR_TOKENS(PETRICHOR_TOKEN_NONE, PETRICHOR_TOKEN_SPELLING, PETRICHOR_TOKEN_NONE) };
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

	// The block of a lambda inside brackets (spec §3.11): its lines are
	// statements, with line breaks and indentation of their own, until a line
	// indented no deeper than the one its header ends on, a bracket opened
	// before it closes, or a comma ends a line, as in `[func(): ..., func(): ...]`,
	// or follows a statement of its first level.
	// GDScript's parser has its tokenizer do the same
	// (modules/gdscript/gdscript_parser.cpp:3735-3745, 3788-3794).
	struct LambdaBlock {
		size_t brackets; // Open at its header.
		std::vector<int> indents; // Those around it, back at its end.
	};
	// `func` inside brackets, up to the colon after its parameters and return type.
	struct LambdaHeader {
		size_t brackets;
		bool params = false; // Their parentheses are closed.
	};

	uint32_t at = 0;
	std::vector<Bracket> brackets; // Open ones, innermost last: line breaks inside them do not count.
	uint32_t cleared = 0; // Brackets taken as closed early, whose closing ones may still come.
	char indent_char = 0;
	std::vector<int> indents = { 0 }; // Those of a lambda's block start at its header's line.
	bool line_has_tokens = false;
	std::vector<LambdaBlock> lambda_blocks;
	std::optional<LambdaHeader> header;
	bool block_opens = false; // A lambda's header ended its line, and its block begins on the next.
	bool block_starts = false; // The next line is the first of a lambda's block.

	// Line breaks count outside brackets, or in a lambda's block outside those
	// opened in it.
	size_t statement_brackets() const {
		return lambda_blocks.empty() ? 0 : lambda_blocks.back().brackets;
	}
	bool at_statement() const {
		return brackets.size() == statement_brackets();
	}

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
			follow_header(p_kind);
		}
		return token;
	}

	void follow_header(TokenKind p_kind) {
		const size_t depth = brackets.size();
		if (p_kind == TokenKind::Func) {
			if (!header && depth > statement_brackets()) {
				header = LambdaHeader{ depth };
			}
			return;
		}
		if (!header || depth > header->brackets) {
			return;
		}
		if (depth < header->brackets || p_kind == TokenKind::Comma) {
			header.reset();
		} else if (p_kind == TokenKind::ParenClose) {
			header->params = true;
		} else if (p_kind == TokenKind::Colon && header->params) {
			header.reset();
			block_opens = rest_of_line_empty(at);
		}
	}

	bool rest_of_line_empty(uint32_t p_from) const {
		uint32_t i = p_from;
		while (i < source.size() && (source[i] == ' ' || source[i] == '\t' || source[i] == '\r')) {
			i++;
		}
		return i >= source.size() || source[i] == '\n' || source[i] == '#';
	}

	// At the line break after a lambda's header.
	void open_lambda_block() {
		block_opens = false;
		block_starts = true;
		lambda_blocks.push_back({ brackets.size(), indents });
		indents = { line_indent(at) };
	}

	void close_lambda_block() {
		end_line();
		while (indents.size() > 1) {
			indents.pop_back();
			push(TokenKind::Dedent, at);
		}
		indents = std::move(lambda_blocks.back().indents);
		lambda_blocks.pop_back();
	}

	// Before a bracket opened outside the innermost block closes.
	void close_lambda_blocks(size_t p_brackets) {
		while (!lambda_blocks.empty() && p_brackets < lambda_blocks.back().brackets) {
			close_lambda_block();
		}
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
		close_lambda_blocks(brackets.size() - 1);
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
		const size_t blocks = lambda_blocks.size();
		close_lambda_blocks(p_index);
		// The statement around a lambda's block ends here too, as one without
		// a lambda does.
		line_has_tokens = line_has_tokens || lambda_blocks.size() < blocks;
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
			// A block no deeper than the line its header ends on, as when the
			// parameters go on to the next lines, need only be deeper than the
			// statement around it. GDScript measures it from there
			// (modules/gdscript/gdscript_parser.cpp:3741-3742,
			// gdscript_tokenizer.cpp:289-291; ed1daf0).
			if (std::exchange(block_starts, false) && width <= indents.front() && width > lambda_blocks.back().indents.back()) {
				indents.front() = lambda_blocks.back().indents.back();
			}
			if (!lambda_blocks.empty() && width <= indents.front()) {
				// The lambda's block ends, and the expression around it goes on.
				close_lambda_block();
				return;
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

	// As in GDScript, a point not followed by another is a decimal point: `.5`
	// and `1.` are floats, and `1.x` is an error
	// (modules/gdscript/gdscript_tokenizer.cpp:683-684, 726-749, 818-827, 1496-1498; ed1daf0).
	void number() {
		const uint32_t start = at;
		std::string digits;
		bool point = peek() == '.';
		bool is_float = point;
		int base = 10;
		if (point) {
			digits.push_back('.');
			advance();
		} else if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X' || peek(1) == 'b' || peek(1) == 'B')) {
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
		if (peek() == '.' && peek(1) != '.' && !point) {
			if (base == 10) {
				point = is_float = true;
				digits.push_back('.');
				advance();
				take_digits();
			} else {
				error(at, at + 1, std::string("Cannot use a decimal point in a ") + (base == 16 ? "hexadecimal" : "binary") + " number.");
			}
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
		if (point && peek() == '.' && peek(1) != '.') {
			error(at, at + 1, "Cannot use a decimal point twice in a number.");
		}
		// A digit the base does not have, or a letter, ends the number in an
		// error: 0b102, 0xFG, 12px.
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
			// An overflow fails the stream, leaving the largest double or
			// infinity by the standard library; GDScript gives infinity
			// (core/string/ustring.cpp:2551-2565; ed1daf0). An underflow may
			// fail it too, leaving the value.
			if (stream.fail() && token.float_value >= 1.0) {
				token.float_value = std::numeric_limits<double>::infinity();
			}
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
				error(start, at, "Unterminated string; a string that spans lines is written in triple quotes.");
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
					append_utf8(r_value, code);
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

	void punctuation() {
		const uint32_t start = at;
		const unsigned char c = peek();
		const unsigned char next = peek(1);
		if (c == '.' && is_digit(next)) {
			number();
			return;
		}
		if ((c == '&' || c == '^') && (next == '"' || next == '\'')) {
			string(c == '&' ? TokenKind::StringName : TokenKind::NodePath);
			return;
		}
		static constexpr std::pair<std::string_view, TokenKind> spellings[] = { PETRICHOR_TOKENS(PETRICHOR_TOKEN_NONE, PETRICHOR_TOKEN_NONE, PETRICHOR_TOKEN_SPELLING) };
		TokenKind kind = TokenKind::Invalid;
		size_t length = 0;
		for (const auto &[spelling, candidate] : spellings) {
			if (spelling.size() > length && source.compare(at, spelling.size(), spelling) == 0) {
				kind = candidate;
				length = spelling.size();
			}
		}
		// && and || are GDScript's spellings of and and or.
		if ((c == '&' || c == '|') && next == c) {
			kind = c == '&' ? TokenKind::And : TokenKind::Or;
			length = 2;
		}
		switch (kind) {
			case TokenKind::Invalid:
				advance();
				trivia(TokenKind::Invalid, start);
				error(start, at, std::string("Unexpected character \"") + char(c) + "\".");
				return;
			case TokenKind::Comma:
				// Also after a statement of the block's first level, where no
				// pattern of a match can be, as GDScript ends a lambda at what
				// follows a statement (modules/gdscript/gdscript_parser.cpp:673-690; ed1daf0).
				if (!lambda_blocks.empty() && at_statement() && (rest_of_line_empty(at + 1) || indents.size() <= 2)) {
					close_lambda_block();
				}
				break;
			// Line breaks inside brackets are ignored too, as in GDScript.
			case TokenKind::ParenOpen:
			case TokenKind::BracketOpen:
			case TokenKind::BraceOpen:
				open_bracket();
				break;
			case TokenKind::ParenClose:
			case TokenKind::BracketClose:
			case TokenKind::BraceClose:
				if (!close_bracket()) {
					unmatched(start);
					return;
				}
				break;
			default:
				break;
		}
		at += uint32_t(length);
		push(kind, start);
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
				// A backslash at the end of a line continues it, past blank and
				// comment lines, as in GDScript
				// (modules/gdscript/gdscript_tokenizer.cpp:1409-1412; ed1daf0).
				const uint32_t start = at;
				advance();
				trivia(TokenKind::LineContinuation, start);
				while (peek() != '\n') {
					advance();
				}
				advance();
				while (!at_end() && rest_of_line_empty(at)) {
					while (peek() == ' ' || peek() == '\t' || peek() == '\r') {
						advance();
					}
					if (peek() == '#') {
						comment();
					}
					if (peek() == '\r') {
						advance();
					}
					if (peek() == '\n') {
						advance();
					}
				}
			} else if (c == '\n') {
				if (block_opens) {
					open_lambda_block();
				}
				if (!brackets.empty()) {
					recover_brackets();
				}
				// The line break token belongs to the line it ends.
				if (at_statement()) {
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
		close_lambda_blocks(0);
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
	r_file.line_starts = line_starts_of(r_file.text);
	return Lexer(r_file, p_resets, p_unclosed, r_errors, r_recovered).run();
}

std::shared_ptr<const SourceFile> tokenize(std::string p_source, std::vector<Diagnostic> &r_errors) {
	auto file = std::make_shared<SourceFile>();
	file->text = std::move(p_source);
	lex(*file, {}, {}, r_errors);
	return file;
}

} // namespace petrichor::compiler
