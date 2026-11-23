#pragma once

#include "compiler/diagnostic.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace petrichor::compiler {

enum class TokenKind {
	EndOfFile,
	Newline,
	Indent,
	Dedent,

	Identifier,
	Int,
	Float,
	String,
	StringName, // `&"…"`
	NodePath, // `^"…"`

	// Keywords. Those this language adds are identifiers (is_word()).
	Await,
	And,
	As,
	Assert,
	Break,
	Breakpoint,
	Class,
	ClassName,
	Const,
	Continue,
	Elif,
	Else,
	Enum,
	Extends,
	False,
	For,
	Func,
	If,
	In,
	Is,
	Match,
	Not,
	Null,
	Or,
	Pass,
	Return,
	Self,
	Super,
	Signal,
	Static,
	True,
	Var,
	When,
	While,

	// Punctuation and operators.
	At,
	Dollar,
	Question,
	Bang,
	Colon,
	Semicolon,
	Comma,
	Period,
	Ellipsis,
	ParenOpen,
	ParenClose,
	BracketOpen,
	BracketClose,
	BraceOpen,
	BraceClose,
	Arrow,
	Plus,
	Minus,
	Star,
	StarStar,
	Slash,
	Percent,
	Equal,
	PlusEqual,
	MinusEqual,
	StarEqual,
	StarStarEqual,
	SlashEqual,
	PercentEqual,
	AmpersandEqual,
	PipeEqual,
	CaretEqual,
	ShiftLeftEqual,
	ShiftRightEqual,
	EqualEqual,
	BangEqual,
	Less,
	LessEqual,
	Greater,
	GreaterEqual,
	Ampersand,
	Pipe,
	Caret,
	Tilde,
	ShiftLeft,
	ShiftRight,

	// Trivia, which the parser does not read.
	Comment,
	LineContinuation, // A backslash that continues a line.
	Invalid, // A character that begins no token, or a closing bracket that closes none.
};

const char *token_name(TokenKind p_kind);

// Names in a node path may be keywords, as in GDScript.
inline bool is_node_name(TokenKind p_kind) {
	return p_kind == TokenKind::Identifier || (p_kind >= TokenKind::Await && p_kind <= TokenKind::While);
}

// The operator of a compound assignment: + for +=; Invalid for other tokens.
TokenKind compound_operator(TokenKind p_assign);

// Whether an operator after the token is binary, and a bracket after it calls
// or subscripts it.
bool ends_operand(TokenKind p_kind);

struct Token {
	TokenKind kind = TokenKind::EndOfFile;
	uint32_t offset = 0; // In bytes.
	uint32_t length = 0; // Zero for Newline, Indent, Dedent and EndOfFile.
	std::string text; // Identifier or keyword as written, or decoded string literal.
	int64_t int_value = 0;
	double float_value = 0.0;
	// 9223372036854775808, which is only an int after a unary minus.
	bool int_overflows = false;

	uint32_t end() const { return offset + length; }
};

// `pub`, `struct`, `interface`, `implements`, `async` and `None` are keywords
// only where their syntax begins, and names elsewhere (spec §2.2).
inline bool is_word(const Token &p_token, const char *p_word) {
	return p_token.kind == TokenKind::Identifier && p_token.text == p_word;
}

// The first and last syntax token of a node, by index in SourceFile::tokens;
// parentheses around the node are included (architecture §2.1).
struct TokenRange {
	uint32_t first = 0;
	uint32_t last = 0;
};

struct SourcePosition {
	int line = 0; // From 1.
	int column = 0; // From 1, in code points.
};

// The offsets at which the lines of `p_text` begin.
std::vector<uint32_t> line_starts_of(const std::string &p_text);

// Where byte `p_offset` of `p_text` is, whose lines begin at `p_line_starts`.
SourcePosition position_in(const std::string &p_text, const std::vector<uint32_t> &p_line_starts, uint32_t p_offset);

// A place as the Language Server Protocol counts it: lines from 0, which
// "\n", "\r\n" and "\r" end, and columns in UTF-16 code units (LSP 3.17,
// Position).
struct Utf16Position {
	uint32_t line = 0;
	uint32_t character = 0;
};

// Where each byte offset of `p_offsets` is in `p_text`, in one pass.
std::vector<Utf16Position> utf16_positions(const std::string &p_text, const std::vector<uint32_t> &p_offsets);
// The byte offset of `p_position` in `p_text`. Past the end of its line, it
// is the line's end; past the last line, the text's end (LSP 3.17, Position).
uint32_t utf16_offset(const std::string &p_text, Utf16Position p_position);

// A change to a source: its bytes from `begin` to `end` replaced by `text`.
struct Edit {
	uint32_t begin = 0;
	uint32_t end = 0;
	std::string text;
};

// A source file as the lexer gives it to the parser and the tools
// (architecture §2.1). Merged by offset, its tokens and trivia leave only
// whitespace between them.
struct SourceFile {
	std::string text;
	std::vector<Token> tokens; // What the parser reads.
	std::vector<Token> trivia; // Comments, line continuations and invalid characters, in order.
	std::vector<uint32_t> line_starts;

	SourcePosition position(uint32_t p_offset) const { return position_in(text, line_starts, p_offset); }
	int line(uint32_t p_token) const { return position(tokens[p_token].offset).line; }
	uint32_t begin(const TokenRange &p_range) const { return tokens[p_range.first].offset; }
	uint32_t end(const TokenRange &p_range) const { return tokens[p_range.last].end(); }
	std::string text_of(const TokenRange &p_range) const { return text.substr(begin(p_range), end(p_range) - begin(p_range)); }
};

// Indentation follows GDScript: a block is opened by a deeper indentation and
// closed by returning to an outer one; line breaks inside parentheses and
// brackets are ignored. A file indents with tabs or with spaces, not both.
std::shared_ptr<const SourceFile> tokenize(std::string p_source, std::vector<Diagnostic> &r_errors);

// Fills the tokens, trivia and line starts of `r_file` from its text, and
// returns the offsets of the brackets open at the end. How brackets left open
// are recovered from (architecture §2.1): one at `p_unclosed` is taken as
// closed at the first line break after it that is followed by a line indented
// no deeper than its own and not beginning with a closing bracket; at the
// line breaks at `p_resets`, which the parser asks for, all are. Each at
// `p_unclosed` is reported, and `r_recovered` receives the range from it to
// where it was taken as closed.
std::vector<uint32_t> lex(SourceFile &r_file, const std::vector<uint32_t> &p_resets, const std::vector<uint32_t> &p_unclosed, std::vector<Diagnostic> &r_errors, std::vector<std::pair<uint32_t, uint32_t>> *r_recovered = nullptr);

inline void append_utf8(std::string &r_text, uint32_t p_code) {
	if (p_code < 0x80) {
		r_text += char(p_code);
	} else if (p_code < 0x800) {
		r_text += char(0xC0 | p_code >> 6);
		r_text += char(0x80 | (p_code & 0x3F));
	} else if (p_code < 0x10000) {
		r_text += char(0xE0 | p_code >> 12);
		r_text += char(0x80 | (p_code >> 6 & 0x3F));
		r_text += char(0x80 | (p_code & 0x3F));
	} else {
		r_text += char(0xF0 | p_code >> 18);
		r_text += char(0x80 | (p_code >> 12 & 0x3F));
		r_text += char(0x80 | (p_code >> 6 & 0x3F));
		r_text += char(0x80 | (p_code & 0x3F));
	}
}

} // namespace petrichor::compiler
