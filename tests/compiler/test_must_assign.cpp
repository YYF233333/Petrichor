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

TEST_CASE("_init reads a member that must be assigned only once it has assigned it on every path") {
	const auto init = [](const std::string &p_body) { return messages(compile("extends Node\n\nvar target: Node3D\n\nfunc _init():\n" + p_body + "\n\nfunc helper() -> void:\n\tprint(target)\n")); };
	const std::string read_first = init("\tprint(target)");
	CHECK(contains(read_first, "_init reads \"target\" before assigning it: a member that must be assigned gets its value from the scene or the creator only after _init."));
	// After both branches assign it; in a lambda, which runs later; through a call, at run time.
	CHECK(init("\tif randi() > 0:\n\t\ttarget = Node3D.new()\n\telse:\n\t\ttarget = Node3D.new()\n\tprint(target.position)").empty());
	CHECK(init("\tvar later := func(): print(target)\n\thelper()\n\ttarget = Node3D.new()").empty());
	CHECK(contains(init("\tif randi() > 0:\n\t\ttarget = Node3D.new()\n\tprint(target)"), "_init reads \"target\" before assigning it"));
	CHECK(contains(init("\twhile randi() > 0:\n\t\ttarget = Node3D.new()\n\tprint(target)"), "_init reads \"target\" before assigning it"));
	CHECK(contains(init("\ttarget.position = Vector3.ZERO"), "_init reads \"target\" before assigning it"));
}

namespace {

std::string made_errors(const std::string &p_body) {
	FakeProject project;
	project.add_script("res://bullet.pet", "class_name Bullet\nextends Node3D\n\npub var target: Node3D\npub var speed := 1.0\n@export pub var trail: Node3D\n", "Bullet");
	project.add_script("res://hidden.pet", "class_name Hidden\nextends Node\n\nvar secret: Node3D\n", "Hidden");
	return messages(compile_in("extends Node\n\nfunc f(scene: PackedScene[Bullet], hidden: PackedScene[Hidden]) -> void:\n" + p_body + "\n", project));
}

} // namespace

TEST_CASE("new(...) and instantiate(...) take the members that must be assigned by name") {
	const std::string made = made_errors("\tvar a := Bullet.new(target = Node3D.new())\n\tvar b := scene.instantiate(target = Node3D.new(), trail = a)\n\tadd_child(a)\n\tadd_child(b)");
	CHECK_MESSAGE(made.empty(), made);
	// The scene may assign an export, but nothing else.
	CHECK(contains(made_errors("\tadd_child(scene.instantiate())"), "instantiate() needs target = ..., a member of Bullet that must be assigned and the scene cannot."));
	CHECK(contains(made_errors("\tadd_child(hidden.instantiate())"), "instantiate() cannot give \"secret\", a member of Hidden that must be assigned and is not pub."));
	CHECK(contains(made_errors("\tadd_child(Bullet.new(speed = 2.0))"), "\"speed\" has a value of its own; by name, new(...) takes only members that must be assigned."));
	CHECK(contains(made_errors("\tadd_child(Hidden.new(secret = Node3D.new()))"), "\"secret\" of Hidden is not pub."));
	CHECK(contains(made_errors("\tadd_child(Bullet.new(nope = 1))"), "Bullet has no member \"nope\"."));
	CHECK(contains(made_errors("\tadd_child(Bullet.new(target = Node3D.new(), 1))"), "Arguments by position come before those by name."));
	CHECK(contains(made_errors("\tadd_child(Node.new(name = &\"x\"))"), "Only new(...) of a class of this language and instantiate(...) of a PackedScene[T] take members by name."));
}
