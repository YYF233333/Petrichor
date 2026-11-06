#include "doctest.h"
#include "helpers.h"

using namespace petrichor;
using namespace petrichor::tests;

// match (spec §5.6): value patterns, `_`, `var`, commas and when.

namespace {

std::string match_errors(const std::string &p_body) {
	return messages(compile("extends Node\n\nfunc f(s: String, n: int, v: Vector2) -> int:\n" + p_body + "\n"));
}

} // namespace

TEST_CASE("match patterns are compared as == compares, and bind in their branch") {
	CHECK(match_errors("\tmatch s:\n\t\t\"a\", \"b\":\n\t\t\treturn 1\n\t\tvar other when other.length() > 3:\n\t\t\treturn other.length()\n\t\t_:\n\t\t\tpass\n\tmatch v:\n\t\tVector2.UP:\n\t\t\treturn 2\n\treturn 0").empty());
	CHECK(contains(match_errors("\tmatch n:\n\t\t\"a\":\n\t\t\treturn 1\n\treturn 0"), "Cannot compare int with String."));
	CHECK(contains(match_errors("\tmatch n:\n\t\tvar a, 1:\n\t\t\treturn a\n\treturn 0"), "Patterns separated by commas cannot bind (spec §5.6)."));
	CHECK(contains(match_errors("\tmatch n:\n\t\trandi():\n\t\t\treturn 1\n\treturn 0"), "A pattern is a literal, a constant expression, an identifier or an attribute access (spec §5.6)."));
	CHECK(contains(match_errors("\tmatch n:\n\t\tvar a:\n\t\t\tpass\n\treturn a"), "Identifier \"a\" is not declared in the current scope."));
	CHECK(contains(match_errors("\tmatch n:\n\t\t1 when s:\n\t\t\treturn 1\n\treturn 0"), "String"));
}

TEST_CASE("a value pattern has the type of the value matched, and may be a name read when the match runs") {
	const std::string head = "extends Node\n\nenum Gait { WALK, RUN }\nenum Mood { CALM }\n\nvar limit := 3\nvar box := Vector2i(1, 2)\n\nfunc f(n: int, x: float, g: Gait, name: StringName, node: Node, child: Node2D) -> void:\n";
	CHECK(messages(compile(head + "\tmatch n:\n\t\tlimit, box.x, Gait.RUN:\n\t\t\tpass\n\tmatch name:\n\t\t\"idle\":\n\t\t\tpass\n\tmatch node:\n\t\tchild:\n\t\t\tpass\n")).empty());
	CHECK(contains(messages(compile(head + "\tmatch x:\n\t\t1:\n\t\t\tpass\n")), "The pattern is int, but the value matched is float; a value pattern must have its type (spec §5.6)."));
	CHECK(contains(messages(compile(head + "\tmatch g:\n\t\t0:\n\t\t\tpass\n\t\t_:\n\t\t\tpass\n")), "The pattern is int, but the value matched is "));
	CHECK(contains(messages(compile(head + "\tmatch g:\n\t\tMood.CALM:\n\t\t\tpass\n\t\t_:\n\t\t\tpass\n")), "The pattern is "));
}

TEST_CASE("a match ends a function when a branch takes every value and every branch returns") {
	CHECK(match_errors("\tmatch n:\n\t\t1:\n\t\t\treturn 1\n\t\t_:\n\t\t\treturn 2").empty());
	CHECK(match_errors("\tmatch n:\n\t\tvar m:\n\t\t\treturn m").empty());
	CHECK(contains(match_errors("\tmatch n:\n\t\t1:\n\t\t\treturn 1"), "Not all paths of \"f()\" return a value."));
	CHECK(contains(match_errors("\tmatch n:\n\t\t_ when n > 0:\n\t\t\treturn 1"), "Not all paths of \"f()\" return a value."));
}

TEST_CASE("print_debug() prints where it is called, as GDScript's does") {
	const auto result = compile("func f():\n\tprint_debug(\"x\", 1)\n");
	REQUIRE_MESSAGE(result.ok(), messages(result));
	bool where = false;
	for (const auto &constant : result.module.constants) {
		where = where || constant.s == "\n   At: :2:f()";
	}
	CHECK(where);
}

TEST_CASE("patterns that are not supported yet are refused") {
	CHECK(contains(messages(compile("func f(a: Array[int]):\n\tmatch a:\n\t\t[1]:\n\t\t\tpass\n")), "Array and dictionary patterns are not supported yet."));
}
