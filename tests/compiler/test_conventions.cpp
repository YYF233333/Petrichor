#include "compiler/lexer.h"
#include "conventions.h"
#include "doctest.h"

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace petrichor;
using namespace petrichor::compiler;

// Architecture §2.1, §3: real code, which the compiler tests can parse but not
// check without the engine's API. Run from the repository's root.
TEST_CASE("the scripts of the engine tests keep the conventions of the syntax tree") {
	size_t count = 0;
	for (const auto &entry : std::filesystem::recursive_directory_iterator("tests/engine")) {
		if (entry.path().extension() != ".pet") {
			continue;
		}
		std::ifstream file(entry.path(), std::ios::binary);
		std::stringstream text;
		text << file.rdbuf();
		INFO(entry.path().string());
		tests::check_syntax_conventions(text.str());
		count++;
	}
	CHECK(count > 20);
}

TEST_CASE("comments, line continuations and invalid characters are trivia") {
	std::vector<Diagnostic> errors;
	const auto file = tokenize("x = 1 # a\r\ny = a && \\\r\n\tb ` c\n", errors);
	REQUIRE(file->trivia.size() == 3);
	CHECK(file->trivia[0].kind == TokenKind::Comment);
	CHECK(file->text.substr(file->trivia[0].offset, file->trivia[0].length) == "# a");
	CHECK(file->trivia[1].kind == TokenKind::LineContinuation);
	CHECK(file->trivia[2].kind == TokenKind::Invalid);
	CHECK(errors.size() == 1);
	for (const char *source : { "", "x", "# only\n\n", "\"unterminated\nx = 1\n", "'''a\nb", "x = 0b12 + 12px\n", "func f():\n\tpass\n    pass\n" }) {
		tests::check_syntax_conventions(source);
	}
	tests::check_syntax_conventions(std::string("a\0b\n", 4));
}
