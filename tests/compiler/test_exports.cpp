#include "doctest.h"
#include "helpers.h"

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
	CHECK(member_named(result.module, "cooldown").hint_string == "0.1,1.0");
	CHECK(member_named(result.module, "max_distance").hint_string == "1.0,20.0");
	CHECK(member_named(result.module, "damage").hint_string == "-5.0,100.0,0.5,or_greater");
	CHECK(member_named(result.module, "damage").initial.f == 25.0);

	CHECK(contains(exports_errors("@export_range(0, 1) var s: String"), "@export_range applies to int and float, not String"));
	CHECK(contains(exports_errors("@export_range(1) var x: int"), "a minimum, a maximum and an optional step"));
	CHECK(contains(exports_errors("@export_range(0, \"or_greater\", 1) var x: int"), "numbers, then options"));
}

TEST_CASE("@export_enum and @export_multiline give the inspector GDScript's hints") {
	const compiler::CompileResult result = compile(
			"extends Resource\n"
			"@export_enum(\"basic_enemy\", \"basic_brick\", \" \") var death_type: String = \" \"\n"
			"@export_enum(\"a\", \"b\") var index := 1\n"
			"@export_multiline var description: String\n");
	REQUIRE_MESSAGE(result.ok(), messages(result));
	CHECK(member_named(result.module, "death_type").hint == 2); // PROPERTY_HINT_ENUM
	CHECK(member_named(result.module, "death_type").hint_string == "basic_enemy,basic_brick, ");
	CHECK(member_named(result.module, "index").hint_string == "a,b");
	CHECK(member_named(result.module, "description").hint == 18); // PROPERTY_HINT_MULTILINE_TEXT
	CHECK(contains(exports_errors("@export_enum(\"a\") var x := 1.5"), "@export_enum applies to int and String, not float."));
	CHECK(contains(exports_errors("@export_multiline var x := 1"), "@export_multiline applies to String and Dictionary, not int."));
	CHECK(contains(exports_errors("@export_enum var x := 1"), "@export_enum takes the names of the values."));
}

// modules/gdscript/gdscript_parser.cpp:153-176, 4660-4992, 4997-5045.
TEST_CASE("the rest of the @export family gives the inspector GDScript's hints") {
	const compiler::CompileResult result = compile(
			"extends Node\n"
			"@export_file(\"*.json\", \"*.txt\") var data: String\n"
			"@export_dir var folder: String\n"
			"@export_placeholder(\"Name, if any\") var title: String\n"
			"@export_node_path(\"Label\", \"Node3D\") var target: NodePath\n"
			"@export_flags(\"Fire\", \"Water:4\") var elements: int\n"
			"@export_flags_2d_physics var layers: int\n"
			"@export_file(\"*.png\") var images: Array[String]\n"
			"@export_range(0, 1) var weights: PackedFloat32Array\n"
			"@export_storage var saved := 3\n"
			"@export_custom(PROPERTY_HINT_LINK, \"suffix:m/s\", PROPERTY_USAGE_EDITOR) var speed := Vector2.ONE\n"
			"var plain := 1\n");
	REQUIRE_MESSAGE(result.ok(), messages(result));
	const auto hint = [&](const char *p_name) { return std::make_pair(member_named(result.module, p_name).hint, member_named(result.module, p_name).hint_string); };
	CHECK(hint("data") == std::make_pair(13u, std::string("*.json,*.txt"))); // PROPERTY_HINT_FILE
	CHECK(hint("folder") == std::make_pair(14u, std::string()));
	CHECK(hint("title") == std::make_pair(20u, std::string("Name, if any")));
	CHECK(hint("target") == std::make_pair(26u, std::string("Label,Node3D")));
	CHECK(hint("elements") == std::make_pair(6u, std::string("Fire,Water:4")));
	CHECK(hint("layers").first == 8u);
	// PROPERTY_HINT_TYPE_STRING: "type/hint:hint string" of the elements.
	CHECK(hint("images") == std::make_pair(23u, std::string("4/13:*.png")));
	CHECK(hint("weights") == std::make_pair(23u, std::string("3/1:0.0,1.0")));
	CHECK(hint("speed") == std::make_pair(5u, std::string("suffix:m/s")));
	CHECK(member_named(result.module, "data").usage == 6); // PROPERTY_USAGE_DEFAULT
	CHECK(member_named(result.module, "saved").usage == 2); // PROPERTY_USAGE_STORAGE
	CHECK(member_named(result.module, "speed").usage == 4); // PROPERTY_USAGE_EDITOR
	CHECK(member_named(result.module, "plain").usage == 0);

	CHECK(contains(exports_errors("@export_file var x: int"), "@export_file applies to String, not int."));
	CHECK(contains(exports_errors("@export_file var x: Array"), "@export_file applies to String, not Array."));
	CHECK(contains(exports_errors("@export_dir(\"a\") var x: String"), "@export_dir takes no arguments."));
	CHECK(contains(exports_errors("@export_placeholder var x: String"), "@export_placeholder takes one argument."));
	CHECK(contains(exports_errors("@export_flags(\"\") var x: int"), "@export_flags takes no empty strings."));
	CHECK(contains(exports_errors("@export_flags(\"A,B\") var x: int"), "contains a comma"));
	CHECK(contains(exports_errors("@export_flags(\"A:0\") var x: int"), "an integer from 1 to 2 ** 32 - 1"));
	CHECK(contains(exports_errors("@export_node_path(\"Resource\") var x: NodePath"), "@export_node_path takes classes of nodes, and Resource is not one."));
	CHECK(contains(exports_errors("@export @export_range(0, 1) var x: int"), "@export_range cannot be used with another @export annotation."));
	CHECK(contains(exports_errors("@export_custom(1) var x: int"), "@export_custom takes a hint, a hint string and optional usage flags."));
	CHECK(contains(exports_errors("@export_tool_button(\"Go\") var x := 1"), "needs @tool, which is not supported yet"));
	CHECK(contains(exports_errors("@export_nope var x := 1"), "The annotation @export_nope is not supported yet."));
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
