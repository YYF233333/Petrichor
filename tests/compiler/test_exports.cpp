#include "helpers.h"

#include "doctest.h"

using namespace petrichor;
using namespace petrichor::tests;

// Exports (spec §2.3, §4.3, §7.4, §7.5), as scripts/weapon.gd of the FPS
// starter kit writes them.

namespace {

const bytecode::Member &member(const compiler::CompileResult &p_result, const char *p_name) {
	const int index = p_result.module.find_member(p_name);
	REQUIRE(index >= 0);
	return p_result.module.members[index];
}

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
	CHECK(member(result, "cooldown").is_export);
	CHECK(member(result, "cooldown").hint == 1); // PROPERTY_HINT_RANGE
	CHECK(member(result, "cooldown").hint_string == "0.1,1");
	CHECK(member(result, "max_distance").hint_string == "1,20");
	CHECK(member(result, "damage").hint_string == "-5,100,0.5,or_greater");
	CHECK(member(result, "damage").initial.f == 25.0);

	CHECK(exports_errors("@export_range(0, 1) var s: String").find("@export_range applies to int and float, not String") != std::string::npos);
	CHECK(exports_errors("@export_range(1) var x: int").find("a minimum, a maximum and an optional step") != std::string::npos);
	CHECK(exports_errors("@export_range(0, \"or_greater\", 1) var x: int").find("numbers, then options") != std::string::npos);
	CHECK(exports_errors("@export_enum(\"a\") var x := 1").find("@export_enum is not supported yet") != std::string::npos);
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
	CHECK(exports_errors("@export_category(\"A\", \"a_\")\n@export var x := 1").find("takes a name, as strings") != std::string::npos);
}

TEST_CASE("an export of an object type must be assigned, and is checked when read (spec §7.5)") {
	const char *source = "extends Resource\n@export var crosshair: Texture2D\n\npub func get_crosshair() -> Texture2D:\n\treturn crosshair\n";
	const compiler::CompileResult result = compile(source);
	REQUIRE_MESSAGE(result.ok(), messages(result));
	const bytecode::Function &function = result.module.functions[result.module.find_function("get_crosshair")];
	CHECK(bytecode::disassemble(result.module, function).find("CHECK_ASSIGNED") != std::string::npos);
	CHECK(exports_errors("var crosshair: Texture2D").find("not supported yet unless exported") != std::string::npos);
	CHECK(exports_errors("@export var crosshair: Texture2D?").empty());
}

TEST_CASE("an exported scene is written PackedScene[T] (spec §7.4)") {
	CHECK(exports_errors("@export var model: PackedScene[Node3D]").empty());
	CHECK(exports_errors("@export var model: PackedScene").find("written PackedScene[T]") != std::string::npos);
	CHECK(exports_errors("@export var model: PackedScene[Resource]").find("The root of a scene is a node, not Resource") != std::string::npos);
	const std::string scenes = "@export var model: PackedScene[Node3D]\n\nfunc f():\n\t";
	CHECK(exports_errors(scenes + "var n: Node3D = model.instantiate()\n\tvar p: PackedScene = model").empty());
	CHECK(exports_errors(scenes + "var s: PackedScene[Node] = model").empty()); // Covariant in T.
	CHECK(exports_errors(scenes + "var s: PackedScene[Timer] = model").find("Cannot assign a value of type PackedScene[Node3D] as PackedScene[Timer]") != std::string::npos);
}
