#include "doctest.h"
#include "helpers.h"

using namespace petrichor;
using namespace petrichor::tests;

// Structs (spec §2.4, §3.6, §4.3).

namespace {

const char *const HIT = "struct Hit:\n\tvar damage: int\n\tvar point: Vector3\n\tvar crit: bool = false\n\nstruct Settings:\n\tvar volume: float = 0.5\n\tvar name: String = \"default\"\n\n";

std::string errors(const std::string &p_body, const std::string &p_members = "") {
	return joined(errors_of(std::string("class_name Shots\nextends Node\n\n") + HIT + p_members + "func f(a: Hit, b: Hit) -> void:\n" + indented(p_body)));
}

FakeProject project() {
	FakeProject project;
	project.add_script("res://combat.pet", "class_name Combat\n\npub struct Hit:\n\tvar damage: int\n\tvar crit: bool = false\n\nstruct Secret:\n\tvar code: int = 0\n", "Combat");
	return project;
}

} // namespace

TEST_CASE("a struct is constructed with its fields by name, and those with defaults may be left out") {
	CHECK(errors("var h := Hit(damage = 3, point = Vector3.ZERO)\nvar s := Settings(name = \"loud\")\nvar t := Settings()\nh.damage += s.name.length()").empty());
	CHECK(contains(errors("var h := Hit(3, Vector3.ZERO)"), "A struct takes its fields by name, as in Shots.Hit(damage = ...)"));
	CHECK(contains(errors("var h := Hit(damage = 3)"), "needs \"point\", which has no default value"));
	CHECK(contains(errors("var h := Hit(damage = 3, point = Vector3.ZERO, speed = 2)"), "has no field \"speed\""));
	CHECK(contains(errors("var h := Hit(damage = \"3\", point = Vector3.ZERO)"), "Cannot pass a value of type String as int"));
	CHECK(contains(errors("print(a.speed)"), "has no field \"speed\""));
	CHECK(contains(errors("add_child(Node.new(), force_readable_name = true)"), "takes them by position"));
}

TEST_CASE("a field is changed where its struct lives, never in a temporary") {
	const std::string make = "func make() -> Hit:\n\treturn Hit(damage = 1, point = Vector3.ZERO)\n\n";
	CHECK(errors("a.damage = 2\na.point.x = 1.0\na.point.x += 1.0", make).empty());
	CHECK(contains(errors("make().damage = 3", make), "temporary"));
}

TEST_CASE("structs compare field by field") {
	CHECK(errors("var same: bool = a == b\nvar other: bool = a != b").empty());
	CHECK(contains(errors("var s := Settings()\nvar x: bool = a == s"), "Invalid operands"));
}

TEST_CASE("a struct starts at its default only when every field has one") {
	CHECK(errors("var s: Settings\nprint(s.volume)", "var settings: Settings\n\n").empty());
	CHECK(contains(errors("var h: Hit"), "\"h\" of type Shots.Hit needs an initial value"));
	CHECK(contains(errors("pass", "var last: Hit\n\n"), "Members that must be assigned, such as \"last\" of type Shots.Hit"));
	CHECK(contains(errors("pass", "@export var settings: Settings\n\n"), "Structs cannot be exported"));
	CHECK(contains(errors("pass", "struct Odd:\n\tvar n: int = randi()\n\n"), "Non-constant default values are not supported yet."));
}

TEST_CASE("a struct that contains itself without a container has no size") {
	CHECK(contains(errors("pass", "struct Chain:\n\tvar next: Chain?\n\n"), "Chain contains itself through Chain.next"));
	CHECK(contains(errors("pass", "struct Outer:\n\tvar inner: Inner\n\nstruct Inner:\n\tvar outer: Outer\n\n"), "Outer contains itself through Outer.inner, Inner.outer"));
}

TEST_CASE("another class's struct is written ClassName.Name, and must be pub") {
	const compiler::CompileResult ok = compile_in("extends Node\n\nfunc f() -> Combat.Hit:\n\tvar h := Combat.Hit(damage = 2)\n\th.crit = true\n\treturn h\n", project());
	CHECK_MESSAGE(ok.ok(), messages(ok));
	CHECK(contains(messages(compile_in("extends Node\n\nfunc f() -> void:\n\tvar s := Combat.Secret()\n", project())), "Combat.Secret is not pub"));
}

TEST_CASE("a field of a struct in a register is a slot inside it, which the verifier accepts") {
	const compiler::CompileResult result = compile(std::string("extends Node\n\n") + HIT + "func f(a: Hit) -> bool:\n\tvar h := a\n\th.crit = h.damage > 2\n\th.point.y = 1.0\n\treturn h == a\n");
	REQUIRE_MESSAGE(result.ok(), messages(result));
	std::string error;
	CHECK_MESSAGE(bytecode::verify(result.module, error), error);
	const bytecode::Function &f = function_named(result.module, "f");
	find_instruction(f, bytecode::Op::EQUAL);
}
