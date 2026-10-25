#pragma once

#include "compiler/diagnostic.h"

#include <cstdint>
#include <string>
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
	And,
	Break,
	ClassName,
	Continue,
	Elif,
	Else,
	Extends,
	False,
	Func,
	If,
	Not,
	Or,
	Pass,
	Pub,
	Return,
	Self,
	True,
	Var,
	While,

	// Punctuation and operators.
	At,
	Colon,
	Comma,
	Period,
	ParenOpen,
	ParenClose,
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
};

const char *token_name(TokenKind p_kind);

struct Token {
	TokenKind kind = TokenKind::EndOfFile;
	int line = 0;
	int column = 0;
	std::string text; // Identifier name or decoded string literal.
	int64_t int_value = 0;
	double float_value = 0.0;
	// 9223372036854775808, which is only an int after a unary minus.
	bool int_overflows = false;
};

// Indentation follows GDScript: a block is opened by a deeper indentation and
// closed by returning to an outer one; line breaks inside parentheses are
// ignored. A file indents with tabs or with spaces, not both.
std::vector<Token> tokenize(const std::string &p_source, std::vector<Diagnostic> &r_errors);

} // namespace petrichor::compiler
