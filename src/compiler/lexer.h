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

	// Keywords.
	Async,
	Await,
	And,
	As,
	Break,
	ClassName,
	Continue,
	Elif,
	Else,
	Extends,
	False,
	For,
	Func,
	If,
	Implements,
	In,
	Interface,
	Is,
	Match,
	None,
	Not,
	Null,
	Or,
	Pass,
	Pub,
	Return,
	Self,
	Super,
	Signal,
	Static,
	Struct,
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
	Slash,
	Percent,
	Equal,
	PlusEqual,
	MinusEqual,
	StarEqual,
	SlashEqual,
	PercentEqual,
	EqualEqual,
	BangEqual,
	Less,
	LessEqual,
	Greater,
	GreaterEqual,

	// Trivia, which the parser does not read.
	Comment,
	LineContinuation, // A backslash that continues a line.
	Invalid, // A character that begins no token, or a closing bracket that closes none.
};

const char *token_name(TokenKind p_kind);

// The operator of a compound assignment: + for +=.
TokenKind compound_operator(TokenKind p_assign);

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

// A source file as the lexer gives it to the parser and the tools
// (architecture §2.1). Merged by offset, its tokens and trivia leave only
// whitespace between them.
struct SourceFile {
	std::string text;
	std::vector<Token> tokens; // What the parser reads.
	std::vector<Token> trivia; // Comments, line continuations and invalid characters, in order.
	std::vector<uint32_t> line_starts;

	SourcePosition position(uint32_t p_offset) const;
	int line(uint32_t p_token) const { return position(tokens[p_token].offset).line; }
	uint32_t begin(const TokenRange &p_range) const { return tokens[p_range.first].offset; }
	uint32_t end(const TokenRange &p_range) const { return tokens[p_range.last].end(); }
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

} // namespace petrichor::compiler
