#include "helpers.h"

#include "doctest.h"

using namespace petrichor::tests;

TEST_CASE("a well-formed class compiles") {
	const char *source = R"(class_name Counter
extends Node

@export pub var speed: float = 2.0
var count := 0
var label = "n"

func _process(delta):
	count += 1
	var step: float = delta * speed

pub func add(a: int, b: int) -> int:
	return a + b

pub func clamp_step(x: float) -> float:
	var y := x * 2
	if y > 10.0:
		return 10.0
	elif y < 0:
		return 0
	while y < 1.0:
		y += 1
	return y

func _ready():
	print("ready ", count, " ", label)
	set_process(true)
	var children := get_child_count()
	var half: int = 7 / 2
)";
	CHECK(joined(errors_of(source)) == "");
}

// Spec §15, numbers.
TEST_CASE("float to int needs an explicit conversion") {
	CHECK(has_error(in_function("var hp: int = 10\nhp = 2.5"), "Convert explicitly with int(x)."));
	CHECK(errors_of(in_function("var hp: int = int(2.5)")).empty());
	CHECK(errors_of(in_function("var x: float = 1")).empty());
}

TEST_CASE("an int argument is accepted for a float parameter") {
	const char *source = "extends Node\n\nfunc half(x: float) -> float:\n\treturn x / 2\n\nfunc f() -> void:\n\thalf(3)\n";
	CHECK(errors_of(source).empty());
}

TEST_CASE("% requires int operands") {
	CHECK(has_error(in_function("var x := 5.0 % 2.0"), "fmod"));
}

// Spec §15, equality.
TEST_CASE("comparisons") {
	CHECK(errors_of(in_function("var a := 1 == 1.0")).empty());
	CHECK(has_error(in_function("var hp := 3\nvar b := hp == \"3\""), "Cannot compare int with String"));
}

// Spec §15, narrowing: conditions must be bool.
TEST_CASE("a condition must be bool") {
	CHECK(has_error(in_function("if 1:\n\tpass"), "Compare explicitly, for example x != 0."));
	CHECK(has_error(in_function("var s := \"\"\nwhile s:\n\tpass"), "x != \"\""));
	CHECK(has_error(in_function("var b := not 1"), "requires bool"));
	CHECK(has_error(in_function("var b := 1 and true"), "requires bool operands"));
}

// Spec §15, signatures.
TEST_CASE("overrides take their types from the engine") {
	CHECK(errors_of("extends Node\n\nfunc _process(delta):\n\tvar d: float = delta\n").empty());
	CHECK(has_error("extends Node\n\nfunc _process(delta):\n\tvar d: int = delta\n", "Convert explicitly"));
	CHECK(has_error("extends Node\n\nfunc _process(delta: int):\n\tpass\n", "declares this parameter as float"));
	CHECK(has_error("extends Node\n\nfunc _process():\n\tpass\n", "1 parameter"));
	CHECK(has_error("extends Node\n\nfunc get_child_count() -> int:\n\treturn 0\n", "cannot be overridden"));
}

TEST_CASE("a function without a return type returns void") {
	CHECK(has_error("extends Node\n\nfunc f():\n\treturn 1\n", "returns void"));
	CHECK(has_error("extends Node\n\nfunc f(x):\n\tpass\n", "needs a type"));
}

TEST_CASE("every path of a function with a result returns") {
	CHECK(has_error("extends Node\n\nfunc f(x: int) -> int:\n\tif x > 0:\n\t\treturn 1\n", "Not all paths"));
	CHECK(errors_of("extends Node\n\nfunc f(x: int) -> int:\n\tif x > 0:\n\t\treturn 1\n\telse:\n\t\treturn 2\n").empty());
	CHECK(has_error("extends Node\n\nfunc f() -> int:\n\treturn\n", "must return a value"));
}

TEST_CASE("names") {
	CHECK(has_error(in_function("x = 1"), "\"x\" is not declared"));
	CHECK(has_error(in_function("var x"), "needs a type or an initial value"));
	CHECK(has_error(in_function("var x := 1\nvar x := 2"), "already declared"));
	CHECK(has_error(in_function("break"), "only valid in a loop"));
	CHECK(has_error(in_function("nope()"), "not found"));
	CHECK(has_error("extends Missing\n", "Unknown base class"));
}

TEST_CASE("engine calls") {
	CHECK(errors_of(in_function("set_process(false)\nvar n: int = get_child_count(true)")).empty());
	CHECK(has_error(in_function("set_process(1)"), "Cannot pass a value of type int as bool"));
	CHECK(has_error(in_function("add_child(1)"), "not supported yet"));
	CHECK(has_error(in_function("var p = get_parent()"), "not supported yet"));
	CHECK(has_error(in_function("_ready()"), "virtual engine method"));
}

TEST_CASE("syntax errors are reported at their position") {
	const auto errors = errors_of("extends Node\n\nfunc f() -> void:\n\tvar x := 1 +\n\tpass\n");
	REQUIRE(errors.size() >= 1);
	CHECK(errors[0].rfind("4:", 0) == 0);
}

TEST_CASE("member initial values are literals so far") {
	CHECK(has_error("extends Node\nvar a := 1\nvar b := a\n", "literals"));
	CHECK(errors_of("extends Node\nvar a: float = -1\n").empty());
}
