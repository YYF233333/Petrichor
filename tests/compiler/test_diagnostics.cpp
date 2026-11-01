#include "doctest.h"
#include "helpers.h"

using namespace petrichor::tests;

// Syntax errors as one meets them while typing (architecture §2.1, §3): each
// reported once, where the mistake is, in its own words. CPython 3.10's
// messages and rustc's are the reference: an unclosed bracket at the
// bracket, a missing comma where it goes.

using Errors = std::vector<std::string>;

TEST_CASE("a bracket that is never closed is reported at the bracket, once") {
	CHECK(errors_of("func f():\n\tprint(\"ready \",\n\tset_process(true)\n\tvar n := 1\n") == Errors{ "2:7: \"(\" is never closed." });
	CHECK(errors_of("func f():\n\tfoo(a\n\tx = 1\n") == Errors{ "2:5: \"(\" is never closed." });
	CHECK(errors_of("func f():\n\tfoo(bar(1)\n\tx = 1\n") == Errors{ "2:5: \"(\" is never closed." });
	CHECK(errors_of("func f():\n\tvar a = [1, 2\n\tvar b = 3\n") == Errors{ "2:10: \"[\" is never closed." });
	CHECK(errors_of("func f():\n\tfoo(") == Errors{ "2:5: \"(\" is never closed." });
	// A header whose body, indented deeper, is read into the bracket at first.
	CHECK(errors_of("func set_y(\n\ty = value\n\tprint(y)\n") == Errors{ "1:11: \"(\" is never closed." });
	CHECK(errors_of("func f(a: int\n\tvar x := a\n\treturn\n") == Errors{ "1:7: \"(\" is never closed." });
}

TEST_CASE("a missing comma is reported where it goes") {
	CHECK(errors_of("func f():\n\tfoo(a b)\n") == Errors{ "2:7: Missing \",\"." });
	CHECK(errors_of("func f():\n\tvar a = [1 2]\n") == Errors{ "2:12: Missing \",\"." });
	CHECK(errors_of("func f(a: int b: int):\n\tpass\n") == Errors{ "1:14: Missing \",\"." });
	CHECK(errors_of("func f():\n\tvar d = {\n\t\t\"a\": 1\n\t\t\"b\": 2,\n\t}\n") == Errors{ "3:9: Missing \",\"." });
}

TEST_CASE("a closing bracket that closes none is reported as unmatched") {
	CHECK(errors_of("func f():\n\tfoo(a, b))\n") == Errors{ "2:11: Unmatched \")\"." });
}

TEST_CASE("one cause gives one error") {
	CHECK(errors_of("func f():\n\tif x:\n\t\tpass\n\t  y = 1\n") == Errors{ "4:1: Indentation mixes tabs and spaces." });
	CHECK(errors_of("func f():\n    if x:\n        pass\n  y = 1\n") == Errors{ "4:1: Indentation does not match any outer block." });
	CHECK(errors_of("func f():\n\tif x > 0\n\t\tpass\n") == Errors{ "2:10: Expected \":\", found line break." });
	CHECK(errors_of("func f()\n\tpass\n") == Errors{ "1:9: Expected \":\", found line break." });
}

TEST_CASE("a mistake within a line is reported where it is") {
	CHECK(errors_of("func f():\n\tx = a +\n\ty = 1\n") == Errors{ "2:9: Expected an expression, found line break." });
	// GDScript lets the string go on to the next line (spec §2.2).
	CHECK(errors_of("func f():\n\tvar s = \"abc\n\ty = 1\n") == Errors{ "2:10: Unterminated string; a string that spans lines is written in triple quotes (spec §2.2)." });
	CHECK(errors_of("func f():\n\tvar s = \"abc") == Errors{ "2:10: Unterminated string." });
	CHECK(errors_of("func f():\n\tx = 1\n\t\ty = 2\n") == Errors{ "3:3: Unexpected indentation." });
}
