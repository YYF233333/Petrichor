#include "compiler/parser.h"
#include "conventions.h"
#include "doctest.h"
#include "helpers.h"

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace petrichor;
using namespace petrichor::compiler;

// Architecture §2.1, §3: real code, which the compiler tests can parse but not
// check without the engine's API. Run from the repository's root.
TEST_CASE("the scripts of the engine tests keep the conventions of the syntax tree, also cut line by line") {
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
		tests::check_recovery(text.str(), nullptr);
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

namespace {

// The body of the only function of a source with syntax errors.
const Block &body_of(const ClassDecl &p_class) {
	REQUIRE(p_class.functions.size() == 1);
	return p_class.functions[0].body;
}

} // namespace

// Architecture §2.1: recovery works by line.
TEST_CASE("an error keeps what its line parsed, with an error node for the rest") {
	std::vector<Diagnostic> errors;
	const ClassDecl cls = parse("func f():\n\tx = a + \n\ty = 1\n", errors);
	CHECK(errors.size() == 1);
	const Block &body = body_of(cls);
	REQUIRE(body.size() == 2);
	REQUIRE(body[0]->kind == StmtKind::Assign);
	CHECK(body[0]->value->kind == ExprKind::Binary);
	CHECK(body[0]->value->right->kind == ExprKind::Error);
	CHECK(body[1]->kind == StmtKind::Assign);
}

TEST_CASE("the block under a header in error is kept") {
	std::vector<Diagnostic> errors;
	const ClassDecl cls = parse("func f():\n\tif x y:\n\t\tpass\n\treturn\n", errors);
	CHECK(errors.size() == 1);
	const Block &body = body_of(cls);
	REQUIRE(body.size() == 2);
	CHECK(body[0]->kind == StmtKind::If);
	CHECK(body[0]->body.size() == 1);
}

TEST_CASE("an indented block that nothing opens is kept as an error statement") {
	std::vector<Diagnostic> errors;
	const ClassDecl cls = parse("func f():\n\tx = 1\n\t\ty = 2\n", errors);
	CHECK(errors.size() == 1);
	const Block &body = body_of(cls);
	REQUIRE(body.size() == 2);
	CHECK(body[1]->kind == StmtKind::Error);
	CHECK(body[1]->body.size() == 1);
}

TEST_CASE("a bracket left open ends at the next line indented no deeper than its own") {
	std::vector<Diagnostic> errors;
	const ClassDecl cls = parse("func f():\n\tprint(\"a\",\n\tset_process(true)\n\tx = 1\n", errors);
	CHECK(errors.size() == 1);
	const Block &body = body_of(cls);
	REQUIRE(body.size() == 3);
	CHECK(body[0]->value->kind == ExprKind::Call);
	REQUIRE(body[0]->value->args.size() == 2);
	CHECK(body[0]->value->args[1]->kind == ExprKind::Error);
	CHECK(body[1]->value->text == "set_process");
}

TEST_CASE("a function header left open still takes the body under it") {
	std::vector<Diagnostic> errors;
	const ClassDecl cls = parse("func f(a: int\n\tvar x := a\n\treturn\n", errors);
	CHECK(errors.size() == 1);
	CHECK(cls.functions[0].params.size() == 1);
	const Block &body = body_of(cls);
	REQUIRE(body.size() == 2);
	CHECK(body[0]->kind == StmtKind::Var);
}

TEST_CASE("with syntax errors, diagnostics report only them") {
	const auto errors = tests::errors_of(tests::in_function("var x: int = \"s\"\ny = "));
	REQUIRE(errors.size() == 1);
	CHECK(tests::contains(errors[0], "Expected an expression"));
}
