#include "helpers.h"

#include "doctest.h"

using namespace petrichor::tests;

// Spec §5.4 and the narrowing row of §15: locals and parameters narrow after
// `!= None`, `is` and an early return; so do the branches of a conditional
// expression and the right side of `and` and `or`; members do not.

namespace {

std::string narrowing(const std::string &p_body, const std::string &p_signature = "func f(p: Node?, e: InputEvent) -> void:") {
	return joined(errors_of("extends Node\nvar member: Node?\n\n" + p_signature + "\n" + indented(p_body)));
}

} // namespace

TEST_CASE("!= None and == None narrow a T? local") {
	CHECK(narrowing("if p != None:\n\tp.queue_free()").empty());
	CHECK(narrowing("if p == None:\n\tpass\nelse:\n\tp.queue_free()").empty());
	CHECK(narrowing("if None != p:\n\tp.queue_free()").empty());
	CHECK(narrowing("if not (p == None):\n\tp.queue_free()").empty());
	CHECK(contains(narrowing("p.queue_free()"), "may be None (Node?)"));
	CHECK(contains(narrowing("if p != None:\n\tpass\np.queue_free()"), "may be None"));
	CHECK(narrowing("if p != null:\n\tp.queue_free()").empty()); // null is None (spec §2.2).
}

TEST_CASE("a narrowed T? of a value type is read and written as the T inside") {
	const auto compiles = [](const std::string &p_body) {
		const auto result = compile(in_function(p_body, "func f(p: Vector3?) -> float:"));
		std::string error;
		return result.ok() && petrichor::bytecode::verify(result.module, error);
	};
	CHECK(compiles("if p != None:\n\treturn p.x\nreturn 0.0"));
	CHECK(compiles("if p != None:\n\tp.x = 2.0\n\treturn p.x\nreturn 0.0"));
	CHECK(compiles("if p != None:\n\tp = Vector3(1, 2, 3)\n\treturn p.y\nreturn 0.0"));
}

TEST_CASE("an early return narrows the rest of the block") {
	CHECK(narrowing("if p == None:\n\treturn\np.queue_free()").empty());
	CHECK(narrowing("for i in 3:\n\tif p == None:\n\t\tcontinue\n\tp.queue_free()").empty());
	CHECK(contains(narrowing("if p == None:\n\tprint(1)\np.queue_free()"), "may be None"));
}

TEST_CASE("after an if with a branch that always leaves, what the other branch ends with holds") {
	CHECK(narrowing("if p == None:\n\treturn\nelse:\n\tprint(1)\np.queue_free()").empty());
	CHECK(narrowing("if p == None:\n\treturn\nelse:\n\tp = Node.new()\np.queue_free()").empty());
	// The only path that reaches the call has just assigned None.
	CHECK(contains(narrowing("if p == None:\n\treturn\nelse:\n\tp = None\np.queue_free()"), "may be None"));
	CHECK(contains(narrowing("if p != None:\n\tp = None\nelse:\n\treturn\np.queue_free()"), "may be None"));
	CHECK(contains(narrowing("for i in 3:\n\tif p == None:\n\t\tbreak\n\telse:\n\t\tp = get_parent()\n\tp.queue_free()"), "may be None"));
	CHECK(contains(narrowing("if e is not InputEventMouseMotion:\n\treturn\nelse:\n\te = InputEventKey.new()\nvar r: Vector2 = e.relative"), "InputEvent has no property \"relative\""));
}

TEST_CASE("is narrows to the class") {
	CHECK(narrowing("if e is InputEventMouseMotion:\n\tvar r: Vector2 = e.relative").empty());
	CHECK(narrowing("if e is not InputEventMouseMotion:\n\treturn\nvar r: Vector2 = e.relative").empty());
	CHECK(contains(narrowing("var r: Vector2 = e.relative"), "InputEvent has no property \"relative\""));
	CHECK(narrowing("if p is Node3D:\n\tvar v: Vector3 = p.position").empty());
	CHECK(contains(narrowing("if e is Node:\n\tpass"), "InputEvent and Node are unrelated classes; is would always be false."));
}

TEST_CASE("the right side of and and or, and the branches of a conditional expression") {
	CHECK(narrowing("if e is InputEventMouseMotion and e.relative.x > 0:\n\tvar r: Vector2 = e.relative").empty());
	CHECK(narrowing("if p == None or str(p.name) == \"x\":\n\tpass").empty());
	CHECK(narrowing("var n: String = str(p.name) if p != None else \"none\"").empty());
	CHECK(narrowing("var n: String = \"none\" if p == None else str(p.name)").empty());
	CHECK(contains(narrowing("if p != None or true:\n\tp.queue_free()"), "may be None"));
}

TEST_CASE("members do not narrow") {
	CHECK(contains(narrowing("if member != None:\n\tmember.queue_free()"), "may be None (Node?)"));
}

TEST_CASE("an assignment narrows by the new value, also in loops") {
	CHECK(narrowing("var q: Node? = get_parent()\nq = Node.new()\nq.queue_free()").empty());
	CHECK(contains(narrowing("if p != None:\n\tp = get_parent()\n\tp.queue_free()"), "may be None"));
	// The second iteration sees the None that the first one assigned.
	CHECK(contains(narrowing("if p != None:\n\twhile true:\n\t\tp.queue_free()\n\t\tp = None"), "may be None"));
	CHECK(contains(narrowing("var q: Node? = get_parent()\nif q != None:\n\tq = None\nq.queue_free()"), "may be None"));
}

TEST_CASE("an option of a value narrows too") {
	CHECK(narrowing("if x != None:\n\tvar y: int = x + 1", "func f(x: int?) -> void:").empty());
	CHECK(contains(narrowing("var y: int = x + 1", "func f(x: int?) -> void:"), "may be None (int?)"));
}

TEST_CASE("a compound assignment reads a narrowed local as narrowed") {
	CHECK(narrowing("if x != None:\n\tx += 1\n\tvar y: int = x", "func f(x: int?) -> void:").empty());
	CHECK(contains(narrowing("x += 1", "func f(x: int?) -> void:"), "may be None (int?)"));
	const auto result = compile(in_function("if x != None:\n\tx += 1\n\treturn x\nreturn 0", "func f(x: int?) -> int:"));
	std::string error;
	CHECK(result.ok());
	CHECK(petrichor::bytecode::verify(result.module, error));
}

TEST_CASE("None goes where a T? is expected") {
	CHECK(narrowing("var q: Node? = None\nvar n: int? = null\np = None").empty());
	CHECK(contains(narrowing("var q = None"), "None does not say which T? it is"));
	CHECK(contains(narrowing("var q: Node = None"), "Cannot assign a value of type None as Node"));
	CHECK(contains(narrowing("var q := 1 == None"), "A value of type int is never None."));
	CHECK(contains(narrowing("if e == None:\n\tpass"), "A value of type InputEvent is never None. Check whether it is alive with is_instance_valid(x)"));
}
