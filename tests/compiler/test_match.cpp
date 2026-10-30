#include "doctest.h"
#include "helpers.h"

using namespace petrichor;
using namespace petrichor::tests;

// match (spec §5.6): literal and constant patterns, `_`, `var`, commas and when.

namespace {

std::string match_errors(const std::string &p_body) {
	return messages(compile("extends Node\n\nfunc f(s: String, n: int, v: Vector2) -> int:\n" + p_body + "\n"));
}

} // namespace

TEST_CASE("match patterns are compared as == compares, and bind in their branch") {
	CHECK(match_errors("\tmatch s:\n\t\t\"a\", \"b\":\n\t\t\treturn 1\n\t\tvar other when other.length() > 3:\n\t\t\treturn other.length()\n\t\t_:\n\t\t\tpass\n\tmatch v:\n\t\tVector2.UP:\n\t\t\treturn 2\n\treturn 0").empty());
	CHECK(contains(match_errors("\tmatch n:\n\t\t\"a\":\n\t\t\treturn 1\n\treturn 0"), "Cannot compare int with String."));
	CHECK(contains(match_errors("\tmatch n:\n\t\tvar a, 1:\n\t\t\treturn a\n\treturn 0"), "Patterns separated by commas cannot bind (spec §5.6)."));
	CHECK(contains(match_errors("\tmatch n:\n\t\trandi():\n\t\t\treturn 1\n\treturn 0"), "A pattern is a literal or a constant expression (spec §5.6)."));
	CHECK(contains(match_errors("\tmatch n:\n\t\tvar a:\n\t\t\tpass\n\treturn a"), "Identifier \"a\" is not declared in the current scope."));
	CHECK(contains(match_errors("\tmatch n:\n\t\t1 when s:\n\t\t\treturn 1\n\treturn 0"), "String"));
}

TEST_CASE("a match ends a function when a branch takes every value and every branch returns") {
	CHECK(match_errors("\tmatch n:\n\t\t1:\n\t\t\treturn 1\n\t\t_:\n\t\t\treturn 2").empty());
	CHECK(match_errors("\tmatch n:\n\t\tvar m:\n\t\t\treturn m").empty());
	CHECK(contains(match_errors("\tmatch n:\n\t\t1:\n\t\t\treturn 1"), "Not all paths of \"f()\" return a value."));
	CHECK(contains(match_errors("\tmatch n:\n\t\t_ when n > 0:\n\t\t\treturn 1"), "Not all paths of \"f()\" return a value."));
}

TEST_CASE("patterns that are not supported yet are refused") {
	CHECK(contains(messages(compile("func f(o: int?):\n\tmatch o:\n\t\t_:\n\t\t\tpass\n")), "A match over int? is not supported yet"));
	CHECK(contains(messages(compile("func f(a: Array[int]):\n\tmatch a:\n\t\t[1]:\n\t\t\tpass\n")), "Array and dictionary patterns are not supported yet."));
}
