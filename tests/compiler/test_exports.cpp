#include "helpers.h"

#include "doctest.h"

using namespace petrichor;
using namespace petrichor::tests;

// Exports (spec §2.3, §4.3, §7.4, §7.5), as scripts/weapon.gd of the FPS
// starter kit writes them.

namespace {

std::string exports_errors(const std::string &p_members) {
	return joined(errors_of("extends Resource\n" + p_members + "\n"));
}

} // namespace

TEST_CASE("@export_range gives the inspector GDScript's range hint") {
	const compiler::CompileResult result = compile(
			"extends Resource\n"
			"@export_range(0.1, 1) var cooldown: float = 0.1\n"
			"@export_range(1, 20) var max_distance: int = 10\n"
			"@export_range(-5, 100, 0.5, \"or_greater\") var damage: float = 25\n");
	REQUIRE_MESSAGE(result.ok(), messages(result));
	CHECK(member_named(result.module, "cooldown").is_export);
	CHECK(member_named(result.module, "cooldown").hint == 1); // PROPERTY_HINT_RANGE
	CHECK(member_named(result.module, "cooldown").hint_string == "0.1,1");
	CHECK(member_named(result.module, "max_distance").hint_string == "1,20");
	CHECK(member_named(result.module, "damage").hint_string == "-5,100,0.5,or_greater");
	CHECK(member_named(result.module, "damage").initial.f == 25.0);

	CHECK(contains(exports_errors("@export_range(0, 1) var s: String"), "@export_range applies to int and float, not String"));
	CHECK(contains(exports_errors("@export_range(1) var x: int"), "a minimum, a maximum and an optional step"));
	CHECK(contains(exports_errors("@export_range(0, \"or_greater\", 1) var x: int"), "numbers, then options"));
	CHECK(contains(exports_errors("@export_enum(\"a\") var x := 1"), "@export_enum is not supported yet"));
}

TEST_CASE("@export_group and @export_subgroup head the exports after them") {
	const compiler::CompileResult result = compile(
			"extends Resource\n"
			"@export_subgroup(\"Model\")\n"
			"@export var position: Vector3\n"
			"@export_group(\"Sounds\", \"sound_\")\n"
			"@export var sound_shoot: String\n");
	REQUIRE_MESSAGE(result.ok(), messages(result));
	REQUIRE(result.module.groups.size() == 2);
	CHECK(result.module.groups[0].name == "Model");
	CHECK(result.module.groups[0].level == 2);
	CHECK(result.module.groups[0].before_member == 0);
	CHECK(result.module.groups[1].prefix == "sound_");
	CHECK(result.module.groups[1].level == 1);
	CHECK(result.module.groups[1].before_member == 1);
	CHECK(contains(exports_errors("@export_category(\"A\", \"a_\")\n@export var x := 1"), "takes a name, as strings"));
}

TEST_CASE("an export of an object type must be assigned, and is checked when read (spec §7.5)") {
	const char *source = "extends Resource\n@export var crosshair: Texture2D\n\npub func get_crosshair() -> Texture2D:\n\treturn crosshair\n";
	const compiler::CompileResult result = compile(source);
	REQUIRE_MESSAGE(result.ok(), messages(result));
	const bytecode::Function &function = function_named(result.module, "get_crosshair");
	CHECK(contains(bytecode::disassemble(result.module, function), "CHECK_ASSIGNED"));
	CHECK(contains(exports_errors("var crosshair: Texture2D"), "not supported yet unless exported"));
	CHECK(exports_errors("@export var crosshair: Texture2D?").empty());
	// A resource never becomes ready.
	CHECK(result.module.ready_function < 0);
}

TEST_CASE("a node is checked for the exports it must be assigned when it becomes ready (spec §7.5)") {
	// enemy.gd of the FPS starter kit, cut down.
	const char *source = "extends Node3D\n@export var player: Node3D\n@onready var aim := player.position\n";
	const compiler::CompileResult result = compile(source);
	REQUIRE_MESSAGE(result.ok(), messages(result));
	REQUIRE(result.module.ready_function >= 0);
	// Before the @onready initializers, which may read the export.
	const std::string ready = bytecode::disassemble(result.module, result.module.functions[result.module.ready_function]);
	const size_t check = ready.find("CHECK_ASSIGNED");
	REQUIRE(check != std::string::npos);
	CHECK(ready.find("GET_PROPERTY") > check);
	CHECK(compile("extends Node3D\n@export var player: Node3D\n").module.ready_function >= 0);
	CHECK(compile("extends Node3D\n@export var player: Node3D?\n").module.ready_function < 0);
}

TEST_CASE("an exported scene is written PackedScene[T] (spec §7.4)") {
	CHECK(exports_errors("@export var model: PackedScene[Node3D]").empty());
	CHECK(contains(exports_errors("@export var model: PackedScene"), "written PackedScene[T]"));
	CHECK(contains(exports_errors("@export var model: PackedScene[Resource]"), "The root of a scene is a node, not Resource"));
	const std::string scenes = "@export var model: PackedScene[Node3D]\n\nfunc f():\n\t";
	CHECK(exports_errors(scenes + "var n: Node3D = model.instantiate()\n\tvar p: PackedScene = model").empty());
	CHECK(exports_errors(scenes + "var s: PackedScene[Node] = model").empty()); // Covariant in T.
	CHECK(contains(exports_errors(scenes + "var s: PackedScene[Timer] = model"), "Cannot assign a value of type PackedScene[Node3D] as PackedScene[Timer]"));
}
