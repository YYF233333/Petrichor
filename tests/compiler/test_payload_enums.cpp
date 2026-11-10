#include "doctest.h"
#include "helpers.h"

using namespace petrichor;
using namespace petrichor::tests;

// Enums with payloads (spec §3.7), and the patterns of match that take values
// apart (§5.6).

namespace {

const char *const HEAD = "class_name Moves\nextends Node\n\nenum Command { IDLE, MOVE(target: Vector3), ATTACK(target: Node3D, power: int) }\nenum Gait { SLOW, FAST }\nenum Step { STILL, WALK(gait: Gait, far: bool) }\nstruct Hit:\n\tvar damage: int\n\tvar crit: bool = false\n\n";

std::string source(const std::string &p_body) {
	return std::string(HEAD) + "func f(c: Command, s: Step, o: int?, h: Hit, n: Node3D) -> int:\n" + p_body + "\n";
}

std::string errors(const std::string &p_body) {
	return messages(compile(source(p_body)));
}

} // namespace

TEST_CASE("a variant is made with its fields by name, and compares and orders as a struct does") {
	CHECK(errors("\tvar a := Command.IDLE\n\tvar b := Command.MOVE(target = Vector3.ZERO)\n\tvar d := Command.ATTACK(power = 2, target = n)\n\tprint(a == b, b != d, s < s)\n\treturn 0").empty());
	CHECK(contains(errors("\tvar a := Command.MOVE\n\treturn 0"), "Command.MOVE has a payload; make it with Command.MOVE(target = ...)."));
	CHECK(contains(errors("\tvar a := Command.IDLE()\n\treturn 0"), "Command.IDLE has no payload; write it without parentheses."));
	CHECK(contains(errors("\tvar a := Command.MOVE(Vector3.ZERO)\n\treturn 0"), "A variant takes its fields by name, as in Command.MOVE(target = ...)."));
	CHECK(contains(errors("\tvar a := Command.ATTACK(power = 1)\n\treturn 0"), "Command.ATTACK() needs \"target\"."));
	CHECK(contains(errors("\tvar a := Command(1)\n\treturn 0"), "No int converts to Command, an enum with payloads."));
	CHECK(contains(errors("\tprint(c.target)\n\treturn 0"), "The fields of a variant are read by match, as in Command.MOVE(var target, ..)."));
	CHECK(contains(errors("\tprint(c < c)\n\treturn 0"), "Values of type Moves.Command are not ordered"));
}

TEST_CASE("the name of an enum with payloads is no value, but a constant initialized with it names it (spec §3.7)") {
	const auto aliased = [](const std::string &p_body) {
		return messages(compile(std::string(HEAD) + "const C = Command\n\nfunc f(c: Command) -> int:\n" + p_body + "\n"));
	};
	CHECK(aliased("\tvar a: C = C.MOVE(target = Vector3.ZERO)\n\tvar b: Command = a\n\tvar d := C.IDLE\n\tc = d\n\tmatch b:\n\t\tC.MOVE(var target):\n\t\t\treturn int(target.x)\n\t\t_:\n\t\t\treturn 0").empty());
	CHECK(contains(aliased("\tprint(C.keys())\n\treturn 0"), "Command has no variant \"keys\"."));
	CHECK(contains(aliased("\tvar d = C\n\treturn 0"), "C names an enum with payloads, whose name is no value; its variants are."));
}

TEST_CASE("an enum with payloads is named, without int values, has no default and is not exported") {
	CHECK(contains(messages(compile("extends Node\n\nenum { A(x: int) }\n")), "An enum with payloads needs a name"));
	CHECK(contains(messages(compile("extends Node\n\nenum E { A = 1, B(x: int) }\n")), "The variants of an enum with payloads have no int values."));
	CHECK(contains(messages(compile("extends Node\n\nenum Tree { LEAF, NODE(left: Tree) }\n")), "Tree contains itself through Tree.NODE.left"));
	CHECK(contains(errors("\tvar a: Command\n\treturn 0"), "\"a\" of type Moves.Command needs an initial value."));
	CHECK(contains(messages(compile("extends Node\n\nenum Command { IDLE, MOVE(target: Vector3) }\n@export pub var c: Command = Command.IDLE\n")), "Enums with payloads cannot be exported in the first version."));
}

TEST_CASE("a pattern takes a variant, a struct or a T? apart, and binds its fields") {
	const std::string body = "\tmatch c:\n\t\tCommand.IDLE:\n\t\t\treturn 0\n\t\tCommand.MOVE(var target):\n\t\t\treturn int(target.x)\n\t\tCommand.ATTACK(power = var p, ..):\n\t\t\treturn p";
	const compiler::CompileResult result = compile(source(body + "\n\nfunc g(h: Hit, o: int?) -> int:\n\tmatch h:\n\t\tHit(var damage, crit = true):\n\t\t\treturn damage\n\tmatch o:\n\t\tNone:\n\t\t\treturn 0\n\t\tSome(3):\n\t\t\treturn 3\n\t\tvar x:\n\t\t\treturn x + 1"));
	REQUIRE_MESSAGE(result.ok(), messages(result));
	std::string error;
	CHECK_MESSAGE(bytecode::verify(result.module, error), error);
	CHECK(contains(errors("\tmatch h:\n\t\tHit(damage = 3):\n\t\t\treturn 1\n\treturn 0"), "Hit(...) leaves out \"crit\"; match it, or end with `..`."));
	CHECK(contains(errors("\tmatch c:\n\t\tCommand.MOVE(Vector3.ZERO):\n\t\t\treturn 1\n\t\t_:\n\t\t\treturn 0"), "The fields of Command.MOVE are matched by name, as in target = pattern."));
	CHECK(contains(errors("\tmatch c:\n\t\tCommand.MOVE(var target), Command.IDLE:\n\t\t\treturn 1\n\t\t_:\n\t\t\treturn 0"), "Patterns separated by commas cannot bind."));
	CHECK(contains(errors("\tmatch 3:\n\t\tSome(var x):\n\t\t\treturn x\n\treturn 0"), "Some(...) matches a value of a type T?, not int."));
	// A call is a value, as in GDScript.
	CHECK(errors("\tmatch Vector2(1, 2):\n\t\tVector2(1, 2):\n\t\t\treturn 1\n\treturn 0").empty());
}

TEST_CASE("a match over an enum with payloads or a T? handles every value, also inside the fields") {
	CHECK(contains(errors("\tmatch c:\n\t\tCommand.IDLE:\n\t\t\treturn 0\n\t\tCommand.MOVE(var target):\n\t\t\treturn 1\n\treturn 2"), "This match over Command does not handle ATTACK(..); handle them or add a `_` branch."));
	CHECK(contains(errors("\tmatch c:\n\t\tCommand.IDLE, Command.ATTACK(..):\n\t\t\treturn 0\n\t\tCommand.MOVE(target = Vector3.ZERO):\n\t\t\treturn 1\n\treturn 2"), "does not handle other MOVE(..);"));
	CHECK(contains(errors("\tmatch o:\n\t\tSome(var x):\n\t\t\treturn x\n\treturn 0"), "This match over int? does not handle None;"));
	// Each Gait and each bool of WALK: every value, so the match ends the function.
	CHECK(errors("\tmatch s:\n\t\tStep.STILL:\n\t\t\treturn 0\n\t\tStep.WALK(gait = Gait.SLOW, ..):\n\t\t\treturn 1\n\t\tStep.WALK(gait = Gait.FAST, far = true):\n\t\t\treturn 2\n\t\tStep.WALK(gait = Gait.FAST, far = false):\n\t\t\treturn 3").empty());
}
