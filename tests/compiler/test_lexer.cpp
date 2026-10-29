#include "compiler/lexer.h"
#include "doctest.h"

using namespace petrichor::compiler;

namespace {

std::vector<TokenKind> kinds(const std::string &p_source, std::vector<Diagnostic> *r_errors = nullptr) {
	std::vector<Diagnostic> errors;
	std::vector<TokenKind> result;
	for (const Token &token : tokenize(p_source, errors)) {
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

TEST_CASE("mixing tabs and spaces is an error") {
	std::vector<Diagnostic> errors;
	kinds("func f():\n\tpass\n    pass\n", &errors);
	REQUIRE(errors.size() >= 1);
	CHECK(errors[0].message.find("tabs and spaces") != std::string::npos);
}

TEST_CASE("an indentation that matches no outer block is an error") {
	std::vector<Diagnostic> errors;
	kinds("func f():\n    if true:\n        pass\n  pass\n", &errors);
	REQUIRE(errors.size() == 1);
	CHECK(errors[0].line == 4);
}

TEST_CASE("numbers") {
	std::vector<Diagnostic> errors;
	const std::vector<Token> tokens = tokenize("1_000 0xFF 0b101 2.5 1e3 7", errors);
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

TEST_CASE("a digit the base lacks, or a letter after a number, is an error") {
	for (const char *source : { "0b102", "0b2", "0xFG", "12px" }) {
		std::vector<Diagnostic> errors;
		tokenize(source, errors);
		CHECK_MESSAGE(errors.size() == 1, source);
	}
	std::vector<Diagnostic> errors;
	const std::vector<Token> tokens = tokenize("0b1_01 1.5.floor()", errors);
	CHECK(errors.empty());
	CHECK(tokens[0].int_value == 5);
}

TEST_CASE("an integer literal beyond int64 is an error") {
	std::vector<Diagnostic> errors;
	tokenize("9223372036854775809", errors);
	CHECK(errors.size() == 1);
	// The magnitude of the smallest int is kept for a unary minus.
	errors.clear();
	const std::vector<Token> tokens = tokenize("9223372036854775808", errors);
	CHECK(errors.empty());
	CHECK(tokens[0].int_overflows);
}

TEST_CASE("triple-quoted strings span lines") {
	std::vector<Diagnostic> errors;
	const std::vector<Token> tokens = tokenize("\"\"\"a\n\"b\"\n\"\"\" '''c'''", errors);
	CHECK(errors.empty());
	REQUIRE(tokens.size() >= 2);
	CHECK(tokens[0].kind == TokenKind::String);
	CHECK(tokens[0].text == "a\n\"b\"\n");
	CHECK(tokens[1].text == "c");
}

TEST_CASE("strings decode escapes") {
	std::vector<Diagnostic> errors;
	const std::vector<Token> tokens = tokenize("\"a\\n\\\"b\" 'c'", errors);
	CHECK(errors.empty());
	CHECK(tokens[0].text == "a\n\"b");
	CHECK(tokens[1].text == "c");
}

TEST_CASE("columns count code points") {
	std::vector<Diagnostic> errors;
	const std::vector<Token> tokens = tokenize("\"雨后\" x", errors);
	CHECK(tokens[1].column == 6);
}
