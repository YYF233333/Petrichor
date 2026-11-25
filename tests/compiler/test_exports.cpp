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

// The engine reaches them by name (spec §4.2, §10.5).
TEST_CASE("an export, an @rpc function and an override of an engine virtual are pub without writing it") {
	FakeProject project;
	project.add_script("res://weapon.pet", "class_name Weapon\nextends Node\n@export_range(0, 10) var speed := 2.0\n@rpc\nfunc fire() -> void:\n\tpass\nfunc _ready() -> void:\n\tpass\n", "Weapon");
	const auto result = compile_in("extends Weapon\npub func _ready() -> void:\n\tpass\nfunc aim(other: Weapon) -> void:\n\tother.speed = 3.0\n\tother.fire()\n\tother._ready()\n", project);
	REQUIRE_MESSAGE(result.ok(), messages(result));
}

TEST_CASE("@export_range gives the inspector GDScript's range hint") {
	const compiler::CompileResult result = compile(
			"extends Resource\n"
			"@export_range(0.1, 1) pub var cooldown: float = 0.1\n"
			"@export_range(1, 20) pub var max_distance: int = 10\n"
			"@export_range(-5, 100, 0.5, \"or_greater\") pub var damage: float = 25\n");
	REQUIRE_MESSAGE(result.ok(), messages(result));
	CHECK(member_named(result.module, "cooldown").is_export);
	CHECK(member_named(result.module, "cooldown").hint == 1); // PROPERTY_HINT_RANGE
	CHECK(member_named(result.module, "cooldown").hint_string == "0.1,1.0");
	CHECK(member_named(result.module, "max_distance").hint_string == "1.0,20.0");
	CHECK(member_named(result.module, "damage").hint_string == "-5.0,100.0,0.5,or_greater");
	CHECK(member_named(result.module, "damage").initial.f == 25.0);

	CHECK(contains(exports_errors("@export_range(0, 1) pub var s: String"), "@export_range applies to int and float, not String"));
	CHECK(contains(exports_errors("@export_range(1) pub var x: int"), "a minimum, a maximum and an optional step"));
	CHECK(contains(exports_errors("@export_range(0, \"or_greater\", 1) pub var x: int"), "numbers, then options"));
}

TEST_CASE("@export_enum and @export_multiline give the inspector GDScript's hints") {
	const compiler::CompileResult result = compile(
			"extends Resource\n"
			"@export_enum(\"basic_enemy\", \"basic_brick\", \" \") pub var death_type: String = \" \"\n"
			"@export_enum(\"a\", \"b\") pub var index := 1\n"
			"@export_multiline pub var description: String\n");
	REQUIRE_MESSAGE(result.ok(), messages(result));
	CHECK(member_named(result.module, "death_type").hint == 2); // PROPERTY_HINT_ENUM
	CHECK(member_named(result.module, "death_type").hint_string == "basic_enemy,basic_brick, ");
	CHECK(member_named(result.module, "index").hint_string == "a,b");
	CHECK(member_named(result.module, "description").hint == 18); // PROPERTY_HINT_MULTILINE_TEXT
	CHECK(contains(exports_errors("@export_enum(\"a\") pub var x := 1.5"), "@export_enum applies to int and String, not float."));
	CHECK(contains(exports_errors("@export_multiline pub var x := 1"), "@export_multiline applies to String and Dictionary, not int."));
	CHECK(contains(exports_errors("@export_enum pub var x := 1"), "@export_enum takes the names of the values."));
}

// modules/gdscript/gdscript_parser.cpp:153-176, 4660-4992, 4997-5045.
TEST_CASE("the rest of the @export family gives the inspector GDScript's hints") {
	const compiler::CompileResult result = compile(
			"extends Node\n"
			"@export_file(\"*.json\", \"*.txt\") pub var data: String\n"
			"@export_dir pub var folder: String\n"
			"@export_placeholder(\"Name, if any\") pub var title: String\n"
			"@export_node_path(\"Label\", \"Node3D\") pub var target: NodePath\n"
			"@export_flags(\"Fire\", \"Water:4\") pub var elements: int\n"
			"@export_flags_2d_physics pub var layers: int\n"
			"@export_file(\"*.png\") pub var images: Array[String]\n"
			"@export_range(0, 1) pub var weights: PackedFloat32Array\n"
			"@export_storage pub var saved := 3\n"
			"@export_custom(PROPERTY_HINT_LINK, \"suffix:m/s\", PROPERTY_USAGE_EDITOR) pub var speed := Vector2.ONE\n"
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

	CHECK(contains(exports_errors("@export_file pub var x: int"), "@export_file applies to String, not int."));
	CHECK(contains(exports_errors("@export_file pub var x: Array"), "@export_file applies to String, not Array."));
	CHECK(contains(exports_errors("@export_dir(\"a\") pub var x: String"), "@export_dir takes no arguments."));
	CHECK(contains(exports_errors("@export_placeholder pub var x: String"), "@export_placeholder takes one argument."));
	CHECK(contains(exports_errors("@export_flags(\"\") pub var x: int"), "@export_flags takes no empty strings."));
	CHECK(contains(exports_errors("@export_flags(\"A,B\") pub var x: int"), "contains a comma"));
	CHECK(contains(exports_errors("@export_flags(\"A:0\") pub var x: int"), "an integer from 1 to 2 ** 32 - 1"));
	CHECK(contains(exports_errors("@export_node_path(\"Resource\") pub var x: NodePath"), "@export_node_path takes classes of nodes, and Resource is not one."));
	CHECK(contains(exports_errors("@export @export_range(0, 1) pub var x: int"), "@export_range cannot be used with another @export annotation."));
	CHECK(contains(exports_errors("@export_custom(1) pub var x: int"), "@export_custom takes a hint, a hint string and optional usage flags."));
	CHECK(contains(exports_errors("@export_tool_button(\"Go\") pub var x := 1"), "needs @tool, which is not in the first version"));
	CHECK(contains(exports_errors("@export_nope pub var x := 1"), "The annotation @export_nope is not supported yet."));
}

TEST_CASE("@export_group and @export_subgroup head the exports after them") {
	const compiler::CompileResult result = compile(
			"extends Resource\n"
			"@export_subgroup(\"Model\")\n"
			"@export pub var position: Vector3\n"
			"@export_group(\"Sounds\", \"sound_\")\n"
			"@export pub var sound_shoot: String\n");
	REQUIRE_MESSAGE(result.ok(), messages(result));
	REQUIRE(result.module.groups.size() == 2);
	CHECK(result.module.groups[0].name == "Model");
	CHECK(result.module.groups[0].level == 2);
	CHECK(result.module.groups[0].before_member == 0);
	CHECK(result.module.groups[1].prefix == "sound_");
	CHECK(result.module.groups[1].level == 1);
	CHECK(result.module.groups[1].before_member == 1);
	CHECK(contains(exports_errors("@export_category(\"A\", \"a_\")\n@export pub var x := 1"), "takes a name, as strings"));
}

TEST_CASE("an export of an object type must be assigned, and is checked when read (spec §7.5)") {
	const char *source = "extends Resource\n@export pub var crosshair: Texture2D\n\npub func get_crosshair() -> Texture2D:\n\treturn crosshair\n";
	const compiler::CompileResult result = compile(source);
	REQUIRE_MESSAGE(result.ok(), messages(result));
	const bytecode::Function &function = function_named(result.module, "get_crosshair");
	CHECK(contains(bytecode::disassemble(result.module, function), "CHECK_MUST"));
	CHECK(exports_errors("@export pub var crosshair: Texture2D?").empty());
	// A resource never becomes ready.
	CHECK(result.module.ready_function < 0);
}

TEST_CASE("a node is checked for the exports it must be assigned when it becomes ready (spec §7.5)") {
	// enemy.gd of the FPS starter kit, cut down.
	const char *source = "extends Node3D\n@export pub var player: Node3D\n@onready var aim := player.position\n";
	const compiler::CompileResult result = compile(source);
	REQUIRE_MESSAGE(result.ok(), messages(result));
	REQUIRE(result.module.ready_function >= 0);
	// Before the @onready initializers, which may read the export.
	const std::string ready = bytecode::disassemble(result.module, result.module.functions[result.module.ready_function]);
	const size_t check = ready.find("CHECK_PROVIDED");
	REQUIRE(check != std::string::npos);
	CHECK(ready.find("GET_PROPERTY") > check);
	CHECK(compile("extends Node3D\n@export pub var player: Node3D\n").module.ready_function >= 0);
	CHECK(compile("extends Node3D\n@export pub var player: Node3D?\n").module.ready_function < 0);
}

TEST_CASE("an export that must be assigned gets a value from a scene the node is built from, which the main scene's check reports (spec §7.5)") {
	const char *source = "extends Node3D\n@export pub var player: Node3D\n";
	compiler::SceneValue path;
	path.type = "NodePath";
	path.text = ".";
	path.to_nodes = true;
	compiler::SceneValue null;
	null.type = "Nil";
	FakeProject project;
	compiler::Scene enemy;
	enemy.path = "res://enemy.tscn";
	enemy.nodes = { { ".", "Node3D", "res://test.pet" } };
	compiler::Scene level;
	level.path = "res://level.tscn";
	level.nodes = { { ".", "Node3D", "" }, { "Near", "Node3D", "", true, "res://test.pet" }, { "Far", "Node3D", "", true, "res://test.pet" } };
	level.nodes[1].instance = level.nodes[2].instance = enemy.path;
	level.nodes[1].properties = { { "player", path } };
	project.scenes = { enemy, level };
	// A scene that nothing references only warns.
	CHECK(check_scenes_in(source, project).errors.empty());
	// Each instance in the scene that has it; whoever instantiates a scene
	// gives its root the rest.
	project.main = level.path;
	const compiler::CompileResult result = check_scenes_in(source, project);
	REQUIRE(result.errors.size() == 1);
	CHECK(result.errors[0].message == "res://level.tscn: Far has no value for \"player\", an export that must be assigned.");
	CHECK((result.errors[0].scene == "res://level.tscn" && result.errors[0].node == "Far"));
	// The game runs all the same: the node fails the check as it becomes ready.
	CHECK(result.ok());
	CHECK(compile_in(source, project).errors.empty());
	// On a subclass's node, the base's check reports its export, once.
	FakeProject based = project;
	based.add_script("res://archer.pet", "extends \"res://test.pet\"\n");
	based.scenes[0].nodes[0].script = based.scenes[1].nodes[1].instance_script = based.scenes[1].nodes[2].instance_script = "res://archer.pet";
	const compiler::CompileResult subclass = check_scenes_in(source, based);
	REQUIRE(subclass.errors.size() == 1);
	CHECK(subclass.errors[0].file == "res://test.pet");
	// Up the instances, any scene may give it; the error is marked on the node.
	compiler::Scene game;
	game.path = "res://game.tscn";
	game.nodes = { { ".", "Node", "" }, { "Level", "Node3D", "", true }, { "Level/Far", "", "" } };
	game.nodes[1].instance = level.path;
	project.scenes.push_back(game);
	project.main = game.path;
	const compiler::CompileResult deep = check_scenes_in(source, project);
	REQUIRE(deep.errors.size() == 1);
	CHECK(deep.errors[0].message == "res://game.tscn: Level/Far has no value for \"player\", an export that must be assigned.");
	CHECK((deep.errors[0].scene == "res://game.tscn" && deep.errors[0].node == "Level/Far"));
	project.scenes[2].nodes[2].properties = { { "player", path } };
	CHECK(check_scenes_in(source, project).errors.empty());
	// The last to store one decides.
	project.scenes[0].nodes[0].properties = { { "player", path } };
	project.scenes[2].nodes[2].properties = { { "player", null } };
	CHECK(contains(messages(check_scenes_in(source, project)), "res://game.tscn: Level/Far has no value"));

	// The main scene, built from the scene it inherits.
	FakeProject inherited;
	compiler::Scene archer = enemy;
	archer.path = "res://archer.tscn";
	archer.base = enemy.path;
	inherited.scenes = { enemy, archer };
	inherited.main = archer.path;
	CHECK(contains(messages(check_scenes_in(source, inherited)), "res://archer.tscn: the root has no value for \"player\""));
	inherited.scenes[0].nodes[0].properties = { { "player", path } };
	CHECK(check_scenes_in(source, inherited).errors.empty());
}

TEST_CASE("a scene used as a root gives the nodes below its root the exports that must be assigned (spec §7.5)") {
	const char *source = "extends Node3D\n@export pub var player: Node3D\n";
	FakeProject project;
	compiler::Scene enemy;
	enemy.path = "res://enemy.tscn";
	enemy.nodes = { { ".", "Node3D", "res://test.pet" }, { "Gun", "Node3D", "res://test.pet" } };
	// Only instanced in another scene, which may give them.
	compiler::Scene level;
	level.path = "res://level.tscn";
	level.nodes = { { ".", "Node3D", "" }, { "Enemy", "Node3D", "", true, "res://test.pet" } };
	level.nodes[1].instance = enemy.path;
	project.scenes = { enemy, level };
	CHECK(check_scenes_in(source, project).errors.empty());
	const auto errors = [&](const FakeProject &p_project) {
		std::vector<std::string> found;
		for (const compiler::Diagnostic &error : check_scenes_in(source, p_project).errors) {
			found.push_back(error.message);
		}
		return found;
	};
	// Whoever instantiates it may give the root the rest (§7.4).
	const std::vector<std::string> gun = { "res://enemy.tscn: Gun has no value for \"player\", an export that must be assigned." };
	FakeProject preloaded = project;
	preloaded.add_script("res://spawner.pet", "extends Node\n\nconst ENEMY = preload(\"enemy.tscn\")\n");
	CHECK(errors(preloaded) == gun);
	FakeProject loaded = project;
	loaded.sources["res://spawner.gd"] = "extends Node\n\nfunc spawn():\n\tadd_child(load(\"res://enemy.tscn\").instantiate())\n";
	CHECK(errors(loaded) == gun);
	// A scene stores it, also on a node without a script of this language.
	FakeProject stored = project;
	compiler::SceneValue scene{ "Object", "PackedScene" };
	scene.path = enemy.path;
	compiler::Scene wave;
	wave.path = "res://wave.tscn";
	wave.nodes = { { ".", "Node", "res://wave.gd" } };
	wave.nodes[0].properties = { { "enemies", compiler::SceneValue{ "Array" } } };
	wave.nodes[0].properties[0].second.items = { scene };
	stored.scenes.push_back(wave);
	CHECK(errors(stored) == gun);
	// So does a resource file or a resource in a scene.
	FakeProject held = project;
	compiler::Scene waves;
	waves.path = "res://waves.tres";
	waves.resources = { { waves.path, "res://waves.gd", { { "first", scene } } } };
	held.scenes.push_back(waves);
	CHECK(errors(held) == gun);
	FakeProject inside = project;
	compiler::Scene arena;
	arena.path = "res://arena.tscn";
	arena.nodes = { { ".", "Node", "" } };
	arena.resources = { { "res://arena.tscn::Resource_w1", "", { { "enemies", wave.nodes[0].properties[0].second } } } };
	inside.scenes.push_back(arena);
	CHECK(errors(inside) == gun);
}

TEST_CASE("an exported scene is written PackedScene[T] (spec §7.4)") {
	CHECK(exports_errors("@export pub var model: PackedScene[Node3D]").empty());
	CHECK(contains(exports_errors("@export pub var model: PackedScene"), "written PackedScene[T]"));
	CHECK(contains(exports_errors("@export pub var model: PackedScene[Resource]"), "The root of a scene is a node, not Resource"));
	const std::string scenes = "@export pub var model: PackedScene[Node3D]\n\nfunc f():\n\t";
	CHECK(exports_errors(scenes + "var n: Node3D = model.instantiate()\n\tvar p: PackedScene = model").empty());
	CHECK(exports_errors(scenes + "var s: PackedScene[Node] = model").empty()); // Covariant in T.
	CHECK(contains(exports_errors(scenes + "var s: PackedScene[Timer] = model"), "Cannot assign a value of type PackedScene[Node3D] as PackedScene[Timer]"));
}

TEST_CASE("an exported Array[PackedScene[T]] holds scenes whose roots are a T, which leave to instantiate() only what it asks for (spec §7.4)") {
	FakeProject project;
	project.add_script("res://room.pet", "class_name Room\nextends Node\n@export pub var exit: Node\n", "Room");
	compiler::Scene shop;
	shop.path = "res://shop.tscn";
	shop.nodes = { { ".", "Node", "res://room.pet" } };
	compiler::Scene hall;
	hall.path = "res://hall.tscn";
	hall.nodes = { { ".", "Node2D", "" } };
	const auto scenes = [](const std::vector<std::string> &p_paths) {
		compiler::SceneValue array{ "Array" };
		array.typed = { { "Object", "PackedScene" } };
		for (const std::string &path : p_paths) {
			array.items.push_back({ "Object", "PackedScene" });
			array.items.back().path = path;
		}
		return array;
	};
	compiler::Scene pool;
	pool.path = "res://pool.tres";
	pool.resources = { { pool.path, "res://test.pet", { { "rooms", scenes({ shop.path }) }, { "halls", scenes({ shop.path, hall.path }) } } } };
	project.scenes = { shop, hall, pool };
	const std::string source = "extends Resource\n@export pub var rooms: Array[PackedScene[Room]]\n@export pub var halls: Array[PackedScene[Room]]\n\n\nfunc f() -> Room:\n\treturn rooms[0].instantiate()\n";
	CHECK(compile_in(source, project).errors.empty());
	const std::string errors = messages(check_scenes_in(source, project));
	CHECK(contains(errors, "res://pool.tres: \"rooms\" of the resource holds res://shop.tscn, which leaves \"exit\" to instantiate(), but a PackedScene[Room] does not ask for it."));
	CHECK(contains(errors, "res://pool.tres: the value stored for \"halls\" of the resource does not fit its type: Array[PackedScene[Room]][1]: expected PackedScene[Room], found PackedScene[Node2D]."));
}

// A constant as text, a Dictionary of the encoding (spec §10.1) with its
// keys in order, a constructed value as C(arguments).
static std::string shown(const bytecode::Module &p_module, const bytecode::Constant &p_constant) {
	std::string text;
	for (size_t i = 0; i < p_constant.args.size(); i++) {
		text += (i ? ", " : "") + shown(p_module, p_constant.args[i]);
	}
	switch (storage(p_module, p_constant.type)) {
		case bytecode::ValueType::Dictionary: {
			std::string entries;
			for (size_t i = 0; i + 1 < p_constant.args.size(); i += 2) {
				entries += (i ? ", " : "") + p_constant.args[i].s + ": " + shown(p_module, p_constant.args[i + 1]);
			}
			return "{" + entries + "}";
		}
		case bytecode::ValueType::Array:
			return "[" + text + "]";
		case bytecode::ValueType::Int:
			return std::to_string(p_constant.i);
		case bytecode::ValueType::Float:
			return std::to_string(int(p_constant.f)) + ".0";
		case bytecode::ValueType::Bool:
			return p_constant.b ? "true" : "false";
		case bytecode::ValueType::String:
			return p_constant.s;
		default:
			return p_constant.constructor >= 0 ? "C(" + text + ")" : "null";
	}
}

TEST_CASE("a struct or an enum with payloads is exported encoded, its default known when built of constants (spec §4.3, §10.1)") {
	const compiler::CompileResult result = compile(
			"class_name Loadout\n"
			"extends Resource\n\n"
			"pub struct Hit:\n"
			"\tvar damage: int = 3\n"
			"\tvar crit := false\n"
			"\tvar tags: Array[String] = []\n"
			"\tvar note: String? = None\n"
			"\tvar mark: int? = 1\n\n"
			"pub enum Order { WAIT, MOVE(target: Vector3, speed: float) }\n\n"
			"@export pub var hit: Hit\n"
			"@export pub var strong := Hit(damage = 9, mark = None)\n"
			"@export pub var order := Order.MOVE(target = Vector3(1, 2, 3), speed = 2)\n"
			"@export pub var idle: Order = Order.WAIT\n"
			"@export pub var maybe: Hit? = Hit(crit = true)\n"
			"@export pub var result: Result[int, String] = Ok(2)\n"
			"@export pub var nothing: Result[int?, String] = Ok(None)\n"
			"@export pub var rolled := Hit(damage = randi())\n"
			"@export pub var next: Order\n");
	REQUIRE_MESSAGE(result.ok(), messages(result));
	const auto initial = [&](const char *p_name) {
		const bytecode::Member &member = member_named(result.module, p_name);
		return member.has_initial ? shown(result.module, member.initial) : std::string("none");
	};
	// A T? field that is None has no key, unless its default is not None (spec §3.6, §3.7, §10.1).
	CHECK(initial("hit") == "{damage: 3, crit: false, tags: [], mark: 1}");
	CHECK(initial("strong") == "{damage: 9, crit: false, tags: [], mark: null}");
	CHECK(initial("order") == "{$variant: MOVE, target: C(1.0, 2.0, 3.0), speed: 2.0}");
	CHECK(initial("idle") == "{$variant: WAIT}");
	CHECK(initial("maybe") == "{damage: 3, crit: true, tags: [], mark: 1}");
	CHECK(initial("result") == "{$variant: Ok, value: 2}");
	CHECK(initial("nothing") == "{$variant: Ok}");
	// Code computes it, or the scene must assign it (spec §7.5).
	CHECK(initial("rolled") == "none");
	CHECK(initial("next") == "none");
	CHECK(member_named(result.module, "next").must_assign);
	// The hint string names the type, after keys that the inspector's
	// Dictionary editor leaves untyped (architecture §6.2).
	CHECK(member_named(result.module, "hit").hint_string == "0:Loadout.Hit");
	CHECK(member_named(result.module, "maybe").hint_string == "0:Loadout.Hit?");
	CHECK(member_named(result.module, "result").hint_string == "0:Result[int, String]");
	CHECK(member_named(result.module, "hit").hint == 0);
}

TEST_CASE("an exported value holds no node at any depth, which a scene would not store (architecture §6.2)") {
	const std::string types = "pub struct Shot:\n\tvar source: Node3D?\n\npub struct Volley:\n\tvar shots: Array[Shot]\n\npub enum Aim { NONE, AT(targets: Array[Node]) }\n\npub struct Cost:\n\tvar icon: Texture2D? = None\n\n";
	CHECK(contains(exports_errors(types + "@export pub var shot: Shot?"), "Shot.source holds a node, which a scene does not store inside an exported value; export the node as a member of its own."));
	CHECK(contains(exports_errors(types + "@export pub var volley: Volley"), "Shot.source holds a node"));
	CHECK(contains(exports_errors(types + "@export pub var aim: Aim = Aim.NONE"), "Aim.AT.targets holds a node"));
	CHECK(exports_errors(types + "@export pub var cost: Cost = Cost()").empty());
	CHECK(exports_errors(types + "pub var shot: Shot?").empty());
}

TEST_CASE("the values that scenes store for an exported struct or enum with payloads are checked as the entry check decodes them (spec §4.3)") {
	const std::string source = "class_name Loadout\nextends Node\n\npub struct Hit:\n\tvar damage: int\n\tvar crit := false\n\tvar tags: Array[String] = []\n\tvar note: String?\n\npub enum Order { WAIT, MOVE(target: Vector3), AIM(at: Vector3?) }\n\n@export pub var hit: Hit?\n@export pub var order := Order.WAIT\n";
	using Value = compiler::SceneValue;
	const auto of = [](const std::string &p_type, int64_t p_integer = 0) {
		Value value;
		value.type = p_type;
		value.integer = p_integer;
		value.whole = p_type == "float";
		return value;
	};
	const auto text = [](const std::string &p_text) {
		Value value;
		value.type = "String";
		value.text = p_text;
		return value;
	};
	const auto entries = [&](const std::vector<std::pair<std::string, Value>> &p_entries) {
		Value value;
		value.type = "Dictionary";
		for (const auto &[key, item] : p_entries) {
			value.items.push_back(text(key));
			value.items.push_back(item);
		}
		return value;
	};
	// On the root of a scene with the class, or on an instance of one, which
	// the scene's check reports; the script compiles.
	const auto stored = [&](const std::string &p_name, const Value &p_value, bool p_instance = false) {
		FakeProject project;
		compiler::Scene scene;
		scene.path = "res://enemy.tscn";
		compiler::SceneNode node{ ".", "Node", "res://test.pet" };
		if (p_instance) {
			node = { "Enemy", "Node", "", true, "res://test.pet" };
		}
		node.properties = { { p_name, p_value } };
		scene.nodes = { node };
		project.scenes = { scene };
		CHECK(compile_in(source, project).errors.empty());
		return check_scenes_in(source, project);
	};
	const auto errors = [&](const std::string &p_name, const Value &p_value) { return messages(stored(p_name, p_value)); };
	// A field with a default may be left out, as by a scene saved before it
	// was added, and a T? field; an int also comes from a whole float.
	CHECK(errors("hit", entries({ { "damage", of("int", 5) } })).empty());
	CHECK(errors("order", entries({ { "$variant", text("AIM") } })).empty());
	CHECK(errors("hit", entries({ { "damage", of("float", 5) }, { "tags", Value{ "Array", "", "", 0, false, "", { text("a") } } } })).empty());
	CHECK(errors("hit", of("Nil")).empty());
	CHECK(errors("order", entries({ { "$variant", text("MOVE") }, { "target", of("Vector3i") } })).empty());
	// One without a default may not, as when it was added after the scene.
	const compiler::CompileResult missing = stored("hit", entries({ { "crit", of("bool") } }));
	CHECK(contains(messages(missing), "res://enemy.tscn: the value stored for \"hit\" of . does not fit its type: Loadout.Hit.damage is missing."));
	REQUIRE(!missing.errors.empty());
	CHECK((missing.errors[0].scene == "res://enemy.tscn" && missing.errors[0].node == "."));
	CHECK(contains(errors("hit", entries({ { "damage", text("5") } })), "Loadout.Hit.damage: expected int, found String."));
	CHECK(contains(errors("hit", entries({ { "damage", of("int", 1) }, { "tags", Value{ "Array", "", "", 0, false, "", { of("int", 2) } } } })), "Loadout.Hit.tags[0]: expected String, found int."));
	CHECK(contains(errors("order", entries({ { "$variant", text("RUN") } })), "Loadout.Order: expected Loadout.Order, found a Dictionary whose \"$variant\" names none of its variants."));
	CHECK(contains(errors("order", entries({ { "$variant", text("MOVE") } })), "Loadout.Order.MOVE.target is missing."));
	CHECK(contains(errors("order", of("int", 1)), "does not fit its type: expected Loadout.Order, found int."));
	CHECK(contains(messages(stored("hit", entries({ { "crit", of("bool") } }), true)), "the value stored for \"hit\" of Enemy does not fit its type: Loadout.Hit.damage is missing."));
	// Or on a node with it inside an instanced scene, which the scene changes.
	FakeProject project;
	compiler::Scene enemy;
	enemy.path = "res://enemy.tscn";
	enemy.nodes = { { ".", "Node", "" }, { "Brain", "Node", "res://test.pet" } };
	compiler::Scene level;
	level.path = "res://level.tscn";
	level.nodes = { { ".", "Node", "" }, { "Enemy", "Node", "", true }, { "Enemy/Brain", "", "" } };
	level.nodes[1].instance = enemy.path;
	level.nodes[2].properties = { { "hit", entries({ { "crit", of("bool") } }) } };
	project.scenes = { enemy, level };
	const compiler::CompileResult changed = check_scenes_in(source, project);
	REQUIRE(changed.errors.size() == 1);
	CHECK(changed.errors[0].message == "res://level.tscn: the value stored for \"hit\" of Enemy/Brain does not fit its type: Loadout.Hit.damage is missing.");
	CHECK((changed.errors[0].scene == "res://level.tscn" && changed.errors[0].node == "Enemy/Brain"));
}

// As instance_set() writes them: what the entry check takes, after the
// engine converts a container to the type of the one the member holds
// (scene/resources/packed_scene.cpp:450-482, 597-640; ed1daf0).
TEST_CASE("the value stored for any export is checked as the engine writes it into the member (spec §4.3, §3.8)") {
	const std::string source = "extends Node\n\n@export pub var cooldown := 1.0\n@export pub var label: String\n@export pub var shots: int\n@export pub var ids: Array[int]\n"
							   "@export pub var rows: Array[Array[int]]\n@export pub var later: Array[int]?\n@export pub var target: Node2D\n";
	using Value = compiler::SceneValue;
	const auto of = [](const std::string &p_type, const std::vector<Value> &p_items = {}, const std::string &p_typed = std::string()) {
		Value value{ p_type };
		value.items = p_items;
		value.whole = p_type == "float";
		if (!p_typed.empty()) {
			value.typed = { Value{ p_typed } };
		}
		return value;
	};
	const auto path = [](const std::string &p_path, bool p_to_nodes) {
		Value value{ "NodePath" };
		value.text = p_path;
		value.to_nodes = p_to_nodes;
		return value;
	};
	const auto checked = [&](const std::string &p_name, const Value &p_value) {
		FakeProject project;
		compiler::Scene scene;
		scene.path = "res://turret.tscn";
		scene.nodes = { { ".", "Node", "res://test.pet" }, { "Gun", "Node2D" }, { "Label", "Label" } };
		scene.nodes[0].properties = { { p_name, p_value } };
		project.scenes = { scene };
		return check_scenes_in(source, project);
	};
	const auto errors = [&](const std::string &p_name, const Value &p_value) { return messages(checked(p_name, p_value)); };
	CHECK(contains(errors("cooldown", of("String")), "res://turret.tscn: the value stored for \"cooldown\" of . does not fit its type: expected float, found String."));
	// As the entry check converts (spec §3.14); an int not from a float.
	CHECK(errors("cooldown", of("int")).empty());
	CHECK(errors("label", of("StringName")).empty());
	CHECK(contains(errors("shots", of("float")), "expected int, found float"));
	// The engine converts the elements of a container, which fit as in it.
	CHECK(errors("ids", of("Array", { of("int"), of("float") })).empty());
	CHECK(errors("ids", of("Array", { of("float") }, "float")).empty());
	CHECK(contains(errors("ids", of("Array", { of("String") })), "Array[int][0]: expected int, found String"));
	// Not an inner one, nor one for a T? that is None.
	CHECK(contains(errors("rows", of("Array", { of("Array", { of("int") }) }, "Array")), "Array[Array[int]][0]: expected Array[int], found Array"));
	CHECK(errors("rows", of("Array", { of("Array", { of("int") }, "int") }, "Array")).empty());
	CHECK(contains(errors("later", of("Array", { of("int") })), "expected Array[int], found Array"));
	CHECK(errors("later", of("Nil")).empty());
	// A node by the path the scene stores for it.
	CHECK(errors("target", path("Gun", true)).empty());
	CHECK(contains(errors("target", path("Label", true)), "expected Node2D, found Label"));
	CHECK(contains(errors("target", path("Gun", false)), "expected Node2D, found NodePath"));
	// One that is missing, after which the game runs (spec §7.5).
	const compiler::CompileResult missing = checked("target", path("Nowhere", true));
	CHECK(contains(messages(missing), "expected Node2D, found null"));
	CHECK(missing.ok());
	// A resource file's.
	FakeProject held;
	compiler::Scene file;
	file.path = "res://turret.tres";
	file.resources = { { file.path, "res://test.pet", { { "cooldown", of("String") } } } };
	held.scenes = { file };
	CHECK(contains(messages(check_scenes_in("extends Resource\n\n@export pub var cooldown := 1.0\n", held)), "res://turret.tres: the value stored for \"cooldown\" of the resource does not fit its type"));
}

TEST_CASE("a resource file and a scene check what their resources with the class store, as on nodes (spec §4.3, §7.4)") {
	const std::string source = "class_name Loadout\nextends Resource\n\npub struct Hit:\n\tvar damage: int\n\npub enum Order { WAIT, MOVE(target: Vector3) }\n\n@export pub var hit: Hit?\n@export pub var order := Order.WAIT\n@export pub var model: PackedScene[Node3D]\n";
	const auto text = [](const std::string &p_text) {
		compiler::SceneValue value{ "String" };
		value.text = p_text;
		return value;
	};
	compiler::SceneValue hit{ "Dictionary" };
	hit.items = { text("damage"), text("5") };
	compiler::SceneValue order{ "Dictionary" };
	order.items = { text("$variant"), text("RUN") };
	compiler::SceneValue model{ "Object", "PackedScene" };
	model.path = "res://enemy.tscn";
	FakeProject project;
	project.add_script("res://enemy.pet", "extends Node3D\n\npub var target: Node3D\n");
	compiler::Scene enemy;
	enemy.path = model.path;
	enemy.nodes = { { ".", "Node3D", "res://enemy.pet" } };
	compiler::Scene loadout;
	loadout.path = "res://loadout.tres";
	loadout.resources = { { loadout.path, "res://test.pet", { { "hit", hit }, { "order", order }, { "model", model } } } };
	compiler::Scene level;
	level.path = "res://level.tscn";
	level.nodes = { { ".", "Node", "" } };
	level.resources = { { "res://level.tscn::Resource_a1", "res://test.pet", { { "hit", hit }, { "model", model } } } };
	project.scenes = { enemy, loadout, level };
	const compiler::CompileResult checked = check_scenes_in(source, project);
	const std::string errors = messages(checked);
	CHECK(contains(errors, "res://loadout.tres: the value stored for \"hit\" of the resource does not fit its type: Loadout.Hit.damage: expected int, found String."));
	CHECK(contains(errors, "res://loadout.tres: the value stored for \"order\" of the resource does not fit its type: Loadout.Order: expected Loadout.Order, found a Dictionary whose \"$variant\" names none of its variants."));
	CHECK(contains(errors, "res://loadout.tres: \"model\" of the resource holds res://enemy.tscn, which leaves \"target\" to instantiate(), but a PackedScene[Node3D] does not ask for it."));
	CHECK(contains(errors, "res://level.tscn: the value stored for \"hit\" of the resource Resource_a1 does not fit its type"));
	REQUIRE(!checked.errors.empty());
	// Marked on the file, which has no node to mark.
	CHECK((checked.errors[0].scene == "res://loadout.tres" && checked.errors[0].node.empty()));
	CHECK(compile_in(source, project).errors.empty());
}

TEST_CASE("a resource file gives a resource with the class each member that must be assigned and that its _init does not assign (spec §6.2)") {
	const std::string source = "extends Resource\n\n@export pub var icon: Texture2D\n@export pub var cost := 1\npub var holder: RefCounted\n";
	compiler::SceneValue icon{ "Object", "CompressedTexture2D" };
	compiler::SceneValue null{ "Nil" };
	const auto errors = [&](const std::string &p_script, const std::vector<std::pair<std::string, compiler::SceneValue>> &p_stored, bool p_unread = false) {
		FakeProject project;
		project.add_script("res://knife.pet", "extends \"res://test.pet\"\n\nfunc _init():\n\tholder = RefCounted.new()\n");
		compiler::Scene file;
		file.path = "res://knife.tres";
		file.resources = { { file.path, p_script, p_stored, p_unread } };
		project.scenes = { file };
		const compiler::CompileResult checked = check_scenes_in(source, project);
		// The game runs all the same, as reading one without a value fails.
		CHECK(checked.ok());
		std::vector<std::string> found;
		for (const compiler::Diagnostic &error : checked.errors) {
			found.push_back(error.message);
		}
		return found;
	};
	const std::string lacking_icon = "res://knife.tres: the resource has no value for \"icon\", an export that must be assigned.";
	const std::string lacking_holder = "res://knife.tres: the resource has no value for \"holder\", which must be assigned and is not exported.";
	CHECK(errors("res://test.pet", {}) == std::vector<std::string>{ lacking_icon, lacking_holder });
	CHECK(errors("res://test.pet", { { "icon", null } }) == std::vector<std::string>{ lacking_icon, lacking_holder });
	CHECK(errors("res://test.pet", { { "icon", icon } }) == std::vector<std::string>{ lacking_holder });
	CHECK(errors("res://knife.pet", { { "icon", icon } }).empty());
	CHECK(errors("res://knife.pet", {}) == std::vector<std::string>{ lacking_icon });
	// Not while the script fails to compile in the editor, which then reads
	// no value: its own check reports why.
	CHECK(errors("res://test.pet", {}, true).empty());
}
