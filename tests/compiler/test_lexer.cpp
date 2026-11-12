#include "compiler/lexer.h"
#include "doctest.h"

#include <limits>

using namespace petrichor::compiler;

namespace {

std::vector<TokenKind> kinds(const std::string &p_source, std::vector<Diagnostic> *r_errors = nullptr) {
	std::vector<Diagnostic> errors;
	std::vector<TokenKind> result;
	const auto file = tokenize(p_source, errors);
	for (const Token &token : file->tokens) {
		result.push_back(token.kind);
	}
	if (r_errors) {
		*r_errors = errors;
	}
	return result;
}

} // namespace

TEST_CASE("blocks open on deeper indentation and close on shallower") {
	using K = TokenKind;
	const std::vector<K> expected = {
		K::Func, K::Identifier, K::ParenOpen, K::ParenClose, K::Colon, K::Newline,
		K::Indent, K::If, K::True, K::Colon, K::Newline,
		K::Indent, K::Pass, K::Newline,
		K::Dedent, K::Return, K::Newline,
		K::Dedent, K::EndOfFile
	};
	CHECK(kinds("func f():\n\tif true:\n\t\tpass\n\treturn\n") == expected);
}

TEST_CASE("blank lines, comments and line breaks in parentheses do not affect blocks") {
	using K = TokenKind;
	const std::vector<K> expected = {
		K::Identifier, K::ParenOpen, K::Int, K::Comma, K::Int, K::ParenClose, K::Newline, K::EndOfFile
	};
	CHECK(kinds("# comment\n\nf(1,\n      2)  # trailing\n\n") == expected);
}

TEST_CASE("&& and || spell and and or, and a backslash continues a line, as in GDScript") {
	using K = TokenKind;
	const std::vector<K> expected = {
		K::Identifier, K::Equal, K::Identifier, K::And, K::Identifier, K::Or, K::Identifier, K::Newline, K::EndOfFile
	};
	CHECK(kinds("x = a && b || c\n") == expected);
	CHECK(kinds("x = a && \\\n\t\tb || c\n") == expected);
	CHECK(kinds("x = a && \\\r\n\t\tb || c\r\n") == expected);
}

TEST_CASE("mixing tabs and spaces is an error") {
	std::vector<Diagnostic> errors;
	kinds("func f():\n\tpass\n    pass\n", &errors);
	REQUIRE(errors.size() >= 1);
	CHECK(errors[0].message.find("tabs and spaces") != std::string::npos);
}

TEST_CASE("an indentation that matches no outer block is an error") {
	std::vector<Diagnostic> errors;
	const auto file = tokenize("func f():\n    if true:\n        pass\n  pass\n", errors);
	REQUIRE(errors.size() == 1);
	CHECK(file->position(errors[0].begin).line == 4);
}

TEST_CASE("numbers") {
	std::vector<Diagnostic> errors;
	const auto file = tokenize("1_000 0xFF 0b101 2.5 1e3 7", errors);
	const std::vector<Token> &tokens = file->tokens;
	CHECK(errors.empty());
	CHECK(tokens[0].int_value == 1000);
	CHECK(tokens[1].int_value == 255);
	CHECK(tokens[2].int_value == 5);
	CHECK(tokens[3].kind == TokenKind::Float);
	CHECK(tokens[3].float_value == 2.5);
	CHECK(tokens[4].kind == TokenKind::Float);
	CHECK(tokens[4].float_value == 1000.0);
	CHECK(tokens[5].kind == TokenKind::Int);
}

// core/string/ustring.cpp:2551-2565 (ed1daf0).
TEST_CASE("a float literal out of range is infinity or zero, whatever the host's library stores") {
	std::vector<Diagnostic> errors;
	const auto file = tokenize("1e999 1e-999", errors);
	CHECK(errors.empty());
	CHECK(file->tokens[0].float_value == std::numeric_limits<double>::infinity());
	CHECK(file->tokens[1].float_value == 0.0);
}

TEST_CASE("a digit the base lacks, or a letter after a number, is an error") {
	for (const char *source : { "0b102", "0b2", "0xFG", "12px" }) {
		std::vector<Diagnostic> errors;
		tokenize(source, errors);
		CHECK_MESSAGE(errors.size() == 1, source);
	}
	std::vector<Diagnostic> errors;
	const auto file = tokenize("0b1_01 1.5.floor()", errors);
	const std::vector<Token> &tokens = file->tokens;
	CHECK(errors.empty());
	CHECK(tokens[0].int_value == 5);
}

TEST_CASE("an integer literal beyond int64 is an error") {
	std::vector<Diagnostic> errors;
	tokenize("9223372036854775809", errors);
	CHECK(errors.size() == 1);
	// The magnitude of the smallest int is kept for a unary minus.
	errors.clear();
	const auto file = tokenize("9223372036854775808", errors);
	const std::vector<Token> &tokens = file->tokens;
	CHECK(errors.empty());
	CHECK(tokens[0].int_overflows);
}

TEST_CASE("triple-quoted strings span lines") {
	std::vector<Diagnostic> errors;
	const auto file = tokenize("\"\"\"a\n\"b\"\n\"\"\" '''c'''", errors);
	const std::vector<Token> &tokens = file->tokens;
	CHECK(errors.empty());
	REQUIRE(tokens.size() >= 2);
	CHECK(tokens[0].kind == TokenKind::String);
	CHECK(tokens[0].text == "a\n\"b\"\n");
	CHECK(tokens[1].text == "c");
}

// modules/gdscript/gdscript_tokenizer.cpp:929-1062.
TEST_CASE("strings decode escapes") {
	std::vector<Diagnostic> errors;
	const auto file = tokenize("\"a\\n\\\"b\" 'c' \"\\a\\b\\f\\r\\t\\v\\\\\\'\" \"\\u00e9\\U01F600\\uD83D\\uDE00\" \"d\\\ne\\\r\nf\"", errors);
	const std::vector<Token> &tokens = file->tokens;
	CHECK(errors.empty());
	REQUIRE(tokens.size() >= 5);
	CHECK(tokens[0].text == "a\n\"b");
	CHECK(tokens[1].text == "c");
	CHECK(tokens[2].text == "\a\b\f\r\t\v\\'");
	CHECK(tokens[3].text == "\xC3\xA9\xF0\x9F\x98\x80\xF0\x9F\x98\x80");
	// A backslash at the end of a line continues the string.
	CHECK(tokens[4].text == "def");
}

TEST_CASE("an escape that is unknown or no code point is an error") {
	const auto messages = [](const std::string &p_source) {
		std::vector<Diagnostic> errors;
		tokenize(p_source, errors);
		std::vector<std::string> result;
		for (const Diagnostic &error : errors) {
			result.push_back(error.message);
		}
		return result;
	};
	CHECK(messages("\"\\q\"") == std::vector<std::string>{ "Unknown escape sequence." });
	CHECK(messages("\"\\u00g0\"") == std::vector<std::string>{ "Invalid hexadecimal digit in Unicode escape sequence." });
	CHECK(messages("\"\\uD83D\"") == std::vector<std::string>{ "Invalid Unicode code point in escape sequence." });
	CHECK(messages("\"\\uDE00\\uD83D\"") == std::vector<std::string>{ "Invalid Unicode code point in escape sequence.", "Invalid Unicode code point in escape sequence." });
	CHECK(messages("\"\\U110000\"") == std::vector<std::string>{ "Invalid Unicode code point in escape sequence." });
}

// modules/gdscript/gdscript_tokenizer.cpp:911-928, 1421-1423.
TEST_CASE("a raw string keeps its backslashes") {
	std::vector<Diagnostic> errors;
	const auto file = tokenize("r\"a\\n\\\"b\\\\\" r'''c\\d''' r x", errors);
	const std::vector<Token> &tokens = file->tokens;
	CHECK(errors.empty());
	REQUIRE(tokens.size() >= 4);
	CHECK((tokens[0].kind == TokenKind::String && tokens[0].text == "a\\n\\\"b\\\\"));
	CHECK(tokens[1].text == "c\\d");
	CHECK((tokens[2].kind == TokenKind::Identifier && tokens[2].text == "r"));
}

// modules/gdscript/gdscript_tokenizer.cpp:1554-1576: a quote right after & or
// ^ begins a literal; otherwise they are operators.
TEST_CASE("&\"…\" is a StringName and ^\"…\" a NodePath") {
	std::vector<Diagnostic> errors;
	const auto file = tokenize("&\"a\\tb\" ^'c/d' &'''e''' a & \"f\" b ^ 'g' a &= b", errors);
	const std::vector<Token> &tokens = file->tokens;
	CHECK(errors.empty());
	REQUIRE(tokens.size() >= 13);
	CHECK((tokens[0].kind == TokenKind::StringName && tokens[0].text == "a\tb" && tokens[0].length == 7));
	CHECK((tokens[1].kind == TokenKind::NodePath && tokens[1].text == "c/d"));
	CHECK((tokens[2].kind == TokenKind::StringName && tokens[2].text == "e"));
	CHECK(tokens[4].kind == TokenKind::Ampersand);
	CHECK(tokens[5].kind == TokenKind::String);
	CHECK(tokens[7].kind == TokenKind::Caret);
	CHECK(tokens[8].kind == TokenKind::String);
	CHECK(tokens[10].kind == TokenKind::AmpersandEqual);
}

TEST_CASE("columns count code points") {
	std::vector<Diagnostic> errors;
	const auto file = tokenize("\"雨后\" x", errors);
	const std::vector<Token> &tokens = file->tokens;
	CHECK(file->position(tokens[1].offset).column == 6);
}
