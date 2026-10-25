#include "helpers.h"

#include "doctest.h"

using namespace petrichor::tests;

// Built-in value types and engine objects (spec §3.1, §3.3, §5, §10).

TEST_CASE("operators follow the engine's table (spec §5.1)") {
	CHECK(type_of("Vector2(1, 2) * 2.0") == "Vector2");
	CHECK(type_of("Vector2i(1, 2) * 2.0") == "Vector2");
	CHECK(type_of("2.0 * Vector3(1, 2, 3)") == "Vector3");
	CHECK(type_of("Vector3(1, 2, 3) / 30") == "Vector3");
	CHECK(type_of("-Vector3(1, 2, 3)") == "Vector3");
	CHECK(type_of("7 / 2") == "int");
	CHECK(type_of("Vector2(1, 2) + Vector2i(1, 2)").find("Invalid operands Vector2 and Vector2i") != std::string::npos);
}

TEST_CASE("== follows the engine's table too (spec §5.2)") {
	CHECK(type_of("Vector2(1, 2) == Vector2(1, 2)") == "bool");
	CHECK(type_of("Vector2() == Vector2i()").find("Invalid operands") != std::string::npos);
}

TEST_CASE("utility functions bound with Variant are typed by their arguments (spec §10.3)") {
	CHECK(type_of("clamp(0.5, 0.0, 1.0)") == "float");
	CHECK(type_of("abs(-3)") == "int");
	CHECK(type_of("wrap(5, 0, 3)") == "int");
	CHECK(type_of("lerp(Vector3(), Vector3(1, 1, 1), 0.5)") == "Vector3");
	CHECK(type_of("lerp(0.0, 1.0, 1)") == "float"); // An int weight converts (spec §3.2).
	CHECK(type_of("lerp(0.0, 1.0, \"a\")").find("must be a number") != std::string::npos);
	CHECK(type_of("min(1.5, 1)").find("Write the int argument as a float") != std::string::npos);
	CHECK(type_of("lerp(Vector3(), 1.0, 0.5)").find("same type") != std::string::npos);
	CHECK(type_of("clamp(\"a\", \"b\", \"c\")").find("numbers or vectors") != std::string::npos);
}

TEST_CASE("other utility functions take the engine's types") {
	CHECK(type_of("cos(1)") == "float");
	CHECK(type_of("deg_to_rad(90)") == "float");
	CHECK(type_of("randi()") == "int");
	CHECK(type_of("str(1, \"a\")") == "String");
	CHECK(has_error(in_function("var r := randf_range(\"a\", 1.0)"), "Cannot pass a value of type String as float"));
}

TEST_CASE("members, constants and constructors of built-in types") {
	CHECK(type_of("v.y", "var v := Vector3(1, 2, 3)") == "float");
	CHECK(type_of("t.origin", "var t := Transform3D()") == "Vector3");
	CHECK(type_of("Vector3.UP") == "Vector3");
	CHECK(type_of("Vector3(1, 2, 3).normalized()") == "Vector3");
	CHECK(type_of("Vector3().lerp(Vector3.ONE, 0.5)") == "Vector3");
	CHECK(type_of("v.w", "var v := Vector3()").find("Vector3 has no member \"w\"") != std::string::npos);
	CHECK(type_of("Vector3.NOPE").find("no constant") != std::string::npos);
	CHECK(type_of("Vector3(\"a\")").find("No constructor of Vector3 takes (String)") != std::string::npos);
}

TEST_CASE("engine properties and objects") {
	CHECK(type_of("position") == "Vector3");
	CHECK(type_of("position.y") == "float");
	CHECK(type_of("RandomNumberGenerator.new()") == "RandomNumberGenerator");
	CHECK(type_of("rng.randf_range(0, 1)", "var rng := RandomNumberGenerator.new()") == "float");
	CHECK(type_of("rng.seed", "var rng := RandomNumberGenerator.new()") == "int");
	CHECK(type_of("rng.nope", "var rng := RandomNumberGenerator.new()").find("has no property") != std::string::npos);
	CHECK(type_of("RandomNumberGenerator.new(1)").find("takes no arguments") != std::string::npos);
	CHECK(type_of("Vector3").find("is a type, not a value") != std::string::npos);
}

TEST_CASE("assignment writes members of built-in values back to their place") {
	CHECK(errors_of(in_function("position.y += 1.0\nrotation.y = 2\nvar v := Vector3()\nv.x = 1", "func f() -> void:", "Node3D")).empty());
	CHECK(errors_of(in_function("var rng := RandomNumberGenerator.new()\nrng.seed = 3")).empty());
	CHECK(has_error(in_function("Vector3(1, 2, 3).y = 2", "func f() -> void:", "Node3D"), "member of a temporary"));
	CHECK(has_error(in_function("var rng := RandomNumberGenerator.new()\nrng.seed = 1.5"), "Convert explicitly with int(x)."));
	CHECK(has_error(in_function("position = 1", "func f() -> void:", "Node3D"), "Cannot assign a value of type int as Vector3"));
}

TEST_CASE("a method that changes a value needs a place to write it back") {
	CHECK(errors_of(in_function("var a := PackedInt32Array()\na.push_back(1)")).empty());
	CHECK(has_error(in_function("PackedInt32Array().push_back(1)"), "a temporary value"));
	// Arrays are shared: a method changes them in place.
	CHECK(errors_of(in_function("Array().append(1)")).empty());
}

TEST_CASE("T? is a nullable object type (spec §3.3)") {
	CHECK(errors_of("extends Node\nvar target: Node3D?\n").empty());
	CHECK(errors_of(in_function("var p: Node?\np = get_parent()")).empty());
	CHECK(type_of("rng", "var rng: RandomNumberGenerator? = RandomNumberGenerator.new()") == "RandomNumberGenerator?");
	CHECK(has_error(in_function("var n: Node = get_parent()"), "Cannot assign a value of type Node? as Node"));
	CHECK(has_error(in_function("var n: int?"), "Optional values of type int are not supported yet"));
}

// Spec §10.2: what the engine passes in is T; a parameter may accept more (§4.6).
TEST_CASE("an override may take T? where the engine passes T") {
	CHECK(errors_of("extends Node\n\nfunc _input(event):\n\tpass\n").empty());
	CHECK(errors_of("extends Node\n\nfunc _input(event: InputEvent):\n\tpass\n").empty());
	CHECK(errors_of("extends Node\n\nfunc _input(event: InputEvent?):\n\tpass\n").empty());
	CHECK(errors_of("extends Node\n\nfunc _input(event: Resource):\n\tpass\n").empty());
	CHECK(has_error("extends Node\n\nfunc _input(event: Node):\n\tpass\n", "declares this parameter as InputEvent"));
}

TEST_CASE("objects that must be assigned are not supported yet (spec §6.2)") {
	CHECK(has_error("extends Node\nvar n: Node3D\n", "not supported yet"));
	CHECK(has_error(in_function("var n: Node3D"), "needs an initial value"));
	CHECK(errors_of(in_function("var v: Vector3\nvar s: StringName")).empty());
}
