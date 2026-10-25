#include "helpers.h"

#include "doctest.h"

using namespace petrichor::tests;

// Spec §5.4 and the narrowing row of §15: locals and parameters narrow after
// `!= None`, `is` and an early return; so do the branches of a conditional
// expression and the right side of `and` and `or`; members do not.

namespace {

std::string narrowing(const std::string &p_body, const std::string &p_signature = "func f(p: Node?, e: InputEvent) -> void:") {
	return joined(errors_of("extends Node\nvar member: Node?\n\n" + p_signature + "\n" + [&] {
		std::string body;
		size_t start = 0;
		while (start <= p_body.size()) {
			const size_t end = p_body.find('\n', start);
			body += "\t" + p_body.substr(start, end == std::string::npos ? std::string::npos : end - start) + "\n";
			if (end == std::string::npos) {
				break;
			}
			start = end + 1;
		}
		return body;
	}()));
}

} // namespace

TEST_CASE("!= None and == None narrow a T? local") {
	CHECK(narrowing("if p != None:\n\tp.queue_free()").empty());
	CHECK(narrowing("if p == None:\n\tpass\nelse:\n\tp.queue_free()").empty());
	CHECK(narrowing("if None != p:\n\tp.queue_free()").empty());
	CHECK(narrowing("if not (p == None):\n\tp.queue_free()").empty());
	CHECK(narrowing("p.queue_free()").find("may be None (Node?)") != std::string::npos);
	CHECK(narrowing("if p != None:\n\tpass\np.queue_free()").find("may be None") != std::string::npos);
	CHECK(narrowing("if p != null:\n\tp.queue_free()").empty()); // null is None (spec §2.2).
}

TEST_CASE("an early return narrows the rest of the block") {
	CHECK(narrowing("if p == None:\n\treturn\np.queue_free()").empty());
	CHECK(narrowing("for i in 3:\n\tif p == None:\n\t\tcontinue\n\tp.queue_free()").empty());
	CHECK(narrowing("if p == None:\n\tprint(1)\np.queue_free()").find("may be None") != std::string::npos);
}

TEST_CASE("is narrows to the class") {
	CHECK(narrowing("if e is InputEventMouseMotion:\n\tvar r: Vector2 = e.relative").empty());
	CHECK(narrowing("if e is not InputEventMouseMotion:\n\treturn\nvar r: Vector2 = e.relative").empty());
	CHECK(narrowing("var r: Vector2 = e.relative").find("InputEvent has no property \"relative\"") != std::string::npos);
	CHECK(narrowing("if p is Node3D:\n\tvar v: Vector3 = p.position").empty());
	CHECK(narrowing("if e is Node:\n\tpass").find("InputEvent and Node are unrelated classes; is would always be false.") != std::string::npos);
}

TEST_CASE("the right side of and and or, and the branches of a conditional expression") {
	CHECK(narrowing("if e is InputEventMouseMotion and e.relative.x > 0:\n\tvar r: Vector2 = e.relative").empty());
	CHECK(narrowing("if p == None or str(p.name) == \"x\":\n\tpass").empty());
	CHECK(narrowing("var n: String = str(p.name) if p != None else \"none\"").empty());
	CHECK(narrowing("var n: String = \"none\" if p == None else str(p.name)").empty());
	CHECK(narrowing("if p != None or true:\n\tp.queue_free()").find("may be None") != std::string::npos);
}

TEST_CASE("members do not narrow") {
	CHECK(narrowing("if member != None:\n\tmember.queue_free()").find("may be None (Node?)") != std::string::npos);
}

TEST_CASE("an assignment narrows by the new value, also in loops") {
	CHECK(narrowing("var q: Node? = get_parent()\nq = Node.new()\nq.queue_free()").empty());
	CHECK(narrowing("if p != None:\n\tp = get_parent()\n\tp.queue_free()").find("may be None") != std::string::npos);
	// The second iteration sees the None that the first one assigned.
	CHECK(narrowing("if p != None:\n\twhile true:\n\t\tp.queue_free()\n\t\tp = None").find("may be None") != std::string::npos);
	CHECK(narrowing("var q: Node? = get_parent()\nif q != None:\n\tq = None\nq.queue_free()").find("may be None") != std::string::npos);
}

TEST_CASE("an option of a value narrows too") {
	CHECK(narrowing("if x != None:\n\tvar y: int = x + 1", "func f(x: int?) -> void:").empty());
	CHECK(narrowing("var y: int = x + 1", "func f(x: int?) -> void:").find("may be None (int?)") != std::string::npos);
}

TEST_CASE("None goes where a T? is expected") {
	CHECK(narrowing("var q: Node? = None\nvar n: int? = null\np = None").empty());
	CHECK(narrowing("var q = None").find("None does not say which T? it is") != std::string::npos);
	CHECK(narrowing("var q: Node = None").find("Cannot assign a value of type None as Node") != std::string::npos);
	CHECK(narrowing("var q := 1 == None").find("A value of type int is never None.") != std::string::npos);
	CHECK(narrowing("if e == None:\n\tpass").find("A value of type InputEvent is never None. Check whether it is alive with is_instance_valid(x)") != std::string::npos);
}
