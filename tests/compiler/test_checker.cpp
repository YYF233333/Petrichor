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

// Spec §5.7.
TEST_CASE("for loops") {
	CHECK(errors_of(in_function("var total := 0\nfor i in 3:\n\ttotal += i\n\tif i == 1:\n\t\tbreak")).empty());
	CHECK(errors_of(in_function("for i: int in 3:\n\tpass")).empty());
	CHECK(has_error(in_function("for i: float in 3:\n\tpass"), "The elements are int, which the loop variable of type float cannot hold."));
	CHECK(has_error(in_function("for i in 3:\n\tpass\nvar j := i"), "\"i\" is not declared"));
	CHECK(has_error(in_function("for x in 2.5:\n\tpass"), "iterate over range(...) instead"));
	CHECK(has_error(in_function("for x in Vector2(2, 3):\n\tpass"), "iterate over range(...) instead"));
	CHECK(has_error(in_function("for x in true:\n\tpass"), "Iterating over bool is not supported yet."));
}

TEST_CASE("! is not, and ; separates statements, as in GDScript") {
	CHECK(errors_of(in_function("var a := !true; var b := not a")).empty());
	CHECK(has_error(in_function("var b := !1"), "requires bool"));
}

// Spec §5.5.
TEST_CASE("conditional expressions") {
	CHECK(type_of("-1 if randi() % 2 == 0 else 1") == "int");
	CHECK(type_of("1 if true else 2.5") == "float");
	CHECK(type_of("Node3D.new() if true else Node.new()") == "Node");
	CHECK(type_of("Node3D.new() if true else Timer.new()") == "Node");
	CHECK(type_of("\"a\" if true else 1").find("no common type") != std::string::npos);
	CHECK(has_error(in_function("var x := 1 if 0 else 2"), "A condition must be bool"));
}

// Spec §4.6.
TEST_CASE("default values of parameters") {
	const std::string source = "extends Node\n\nfunc add(a: int, b: int = 2, c: float = 0.5) -> float:\n\treturn a + b + c\n\nfunc f() -> void:\n\t";
	CHECK(errors_of(source + "var x := add(1)\n\tvar y := add(1, 2, 3)\n").empty());
	CHECK(has_error(source + "var x := add()\n", "takes 1 to 3 argument(s), 0 given"));
	CHECK(has_error("extends Node\n\nfunc g(a: int = 1, b: int):\n\tpass\n", "needs one too"));
	CHECK(has_error("extends Node\n\nfunc g(a: int = randi()):\n\tpass\n", "other than literals are not supported yet"));
	CHECK(has_error("extends Node\n\nfunc g(a: int = \"x\"):\n\tpass\n", "Cannot use a value of type String as int"));
}

// Spec §4.8.
TEST_CASE("static functions have no instance") {
	const std::string head = "extends Node\nvar hp := 1\n\nfunc instance_only():\n\tpass\n\nstatic func twice(x: int) -> int:\n\treturn x * 2\n\n";
	CHECK(errors_of(head + "func f() -> void:\n\tvar y := twice(3)\n").empty());
	CHECK(errors_of(head + "static func g() -> int:\n\treturn twice(2)\n").empty());
	CHECK(has_error(head + "static func g() -> int:\n\treturn hp\n", "\"hp\" belongs to an instance, which the static function \"g()\" does not have."));
	CHECK(has_error(head + "static func g():\n\tinstance_only()\n", "\"instance_only()\" belongs to an instance"));
	CHECK(has_error(head + "static func g():\n\tqueue_free()\n", "\"queue_free()\" belongs to an instance"));
	CHECK(has_error("extends Node\n\nstatic func _ready():\n\tpass\n", "cannot be static"));
}

TEST_CASE("engine calls") {
	CHECK(errors_of(in_function("set_process(false)\nvar n: int = get_child_count(true)")).empty());
	CHECK(has_error(in_function("set_process(1)"), "Cannot pass a value of type int as bool"));
	CHECK(has_error(in_function("add_child(1)"), "Cannot pass a value of type int as Node?"));
	CHECK(has_error(in_function("_ready()"), "virtual engine method"));
}

TEST_CASE("objects the engine returns are nullable until audited (spec §10.2)") {
	CHECK(errors_of(in_function("var p = get_parent()\nadd_child(p)")).empty());
	CHECK(has_error(in_function("var p = get_parent()\nprint(p.name)"), "may be None (Node?)"));
	CHECK(has_error(in_function("var p = get_parent()\np.queue_free()"), "may be None (Node?)"));
}

TEST_CASE("the smallest int can be written in decimal") {
	CHECK(errors_of(in_function("var x := -9223372036854775808")).empty());
	CHECK(has_error(in_function("var x := 9223372036854775808"), "too large for int"));
}

// Spec §2.1.
TEST_CASE("the class header comes before the members") {
	CHECK(has_error("class_name A\nclass_name B\nextends Node\n", "already has a name"));
	CHECK(has_error("extends Node\nvar a := 1\nclass_name A\n", "class header"));
	CHECK(has_error("extends Node\n\nfunc f():\n\tpass\n\nextends Node2D\n", "class header"));
}

// Spec §6.2: the engine constructs Node and Resource scripts with no arguments.
TEST_CASE("_init") {
	CHECK(errors_of("extends Node\nvar ready_count := 0\n\nfunc _init():\n\tready_count = 1\n").empty());
	CHECK(has_error("extends Node\n\nfunc _init(a: int):\n\tpass\n", "cannot take parameters"));
	CHECK(has_error("extends Node\n\nfunc _init() -> int:\n\treturn 1\n", "returns nothing"));
}

TEST_CASE("syntax errors are reported at their position") {
	const auto errors = errors_of("extends Node\n\nfunc f() -> void:\n\tvar x := 1 +\n\tpass\n");
	REQUIRE(errors.size() >= 1);
	CHECK(errors[0].rfind("4:", 0) == 0);
}

TEST_CASE("member initializers run in declaration order (spec §6.1)") {
	CHECK(errors_of("extends Node\nvar a := 1\nvar b := a + 1\n").empty());
	CHECK(errors_of("extends Node\nvar a: float = -1\n").empty());
	CHECK(has_error("extends Node\nvar b := a\nvar a := 1\n", "\"a\" is not initialized yet"));
	CHECK(has_error("extends Node\nvar a: int = a\n", "\"a\" is not initialized yet"));
	CHECK(errors_of("extends Node\n\nvar rng = RandomNumberGenerator.new()\nvar r := rng.randf_range(0, 1)\n").empty());
}
