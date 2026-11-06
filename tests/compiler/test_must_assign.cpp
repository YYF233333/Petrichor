#include "doctest.h"
#include "helpers.h"

using namespace petrichor;
using namespace petrichor::tests;

// Members that must be assigned (spec §4.3, §6.2, §6.4).

TEST_CASE("a member whose type has no default must be assigned, which is checked before it is read") {
	const char *source = "extends Node3D\n\nstruct Hit:\n\tvar damage: int\n\nenum Mode { AIM = 1, FIRE = 2 }\nenum Order { HOLD, MOVE(to: Vector3) }\n\nvar target: Node3D\nvar last: Hit\nvar mode: Mode\nvar order: Order\nvar on_fire: func(int)\nvar aimed: Node3D?\n\nfunc aim() -> float:\n\ttarget = Node3D.new()\n\treturn target.position.x\n";
	const compiler::CompileResult result = compile(source);
	REQUIRE_MESSAGE(result.ok(), messages(result));
	std::string error;
	CHECK_MESSAGE(bytecode::verify(result.module, error), error);
	for (const char *name : { "target", "last", "mode", "order", "on_fire" }) {
		CHECK_MESSAGE(member_named(result.module, name).must_assign, name);
	}
	CHECK(!member_named(result.module, "aimed").must_assign);
	// Marked when written, checked when read.
	const std::string aim = bytecode::disassemble(result.module, function_named(result.module, "aim"));
	REQUIRE(aim.find("MARK_ASSIGNED") != std::string::npos);
	CHECK(aim.find("CHECK_MUST") > aim.find("MARK_ASSIGNED"));
	// The node checks them when it becomes ready; a resource only when read.
	REQUIRE(result.module.ready_function >= 0);
	CHECK(contains(bytecode::disassemble(result.module, result.module.functions[result.module.ready_function]), "CHECK_PROVIDED"));
	CHECK(compile("extends Resource\nvar target: Node3D\n").module.ready_function < 0);
}
