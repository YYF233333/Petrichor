#include "bytecode/opcodes.h"
#include "helpers.h"

#include "doctest.h"

using namespace petrichor;
using namespace petrichor::tests;

// The scene model (spec §7) and the compilation that runs while a script is
// being loaded in the editor.

namespace {

// A Timer under the root, which has the script; `p_method` is connected to
// the timer's timeout, with `p_binds` bound arguments.
FakeProject timer_scene(const std::string &p_method, int p_binds = 0) {
	FakeProject project;
	compiler::Scene scene;
	scene.path = "res://enemy.tscn";
	scene.nodes = { { ".", "Node3D", "res://test.pet" }, { "Timer", "Timer", "" } };
	compiler::SceneConnection connection;
	connection.signal = "timeout";
	connection.source = "Timer";
	connection.target = ".";
	connection.method = p_method;
	connection.binds = p_binds;
	scene.connections = { connection };
	project.scenes = { scene };
	return project;
}

// main.tscn of the FPS starter kit, cut down: the HUD has the script, and the
// player is an instanced scene.
FakeProject hud_scene() {
	FakeProject project;
	compiler::Scene scene;
	scene.path = "res://main.tscn";
	scene.nodes = {
		{ ".", "Node3D", "" },
		{ "Player", "Node3D", "", true },
		{ "HUD", "CanvasLayer", "res://test.pet" },
		{ "HUD/Crosshair", "TextureRect", "" },
		{ "HUD/Health", "Label", "" },
	};
	project.scenes = { scene };
	return project;
}

std::string hud_errors(const std::string &p_body, const FakeProject &p_project = hud_scene()) {
	return messages(compile_in("extends CanvasLayer\n\nfunc f():\n\t" + p_body + "\n", p_project));
}

} // namespace

TEST_CASE("a method connected in a scene is visible to the engine (spec §10.5)") {
	const char *source = "extends Node3D\n\nfunc _on_timer_timeout():\n\tpass\n\nfunc other():\n\tpass\n";
	const compiler::CompileResult result = compile_in(source, timer_scene("_on_timer_timeout"));
	REQUIRE_MESSAGE(result.ok(), messages(result));
	CHECK(function_named(result.module, "_on_timer_timeout").is_exposed);
	CHECK(!function_named(result.module, "other").is_exposed);
}

TEST_CASE("scene connections are checked against the class (spec §7.7)") {
	const char *source = "extends Node3D\n\nfunc _on_timer_timeout():\n\tpass\n";
	CHECK(contains(messages(compile_in(source, timer_scene("_on_missing"))), "res://enemy.tscn: the signal \"timeout\" of Timer is connected to _on_missing(), which this class does not declare."));
	CHECK(contains(messages(compile_in(source, timer_scene("_on_timer_timeout", 1))), "which passes 1 argument(s); _on_timer_timeout() takes 0."));
	CHECK(compile_in("extends Node3D\n\nfunc _on_timer_timeout(extra: int):\n\tpass\n", timer_scene("_on_timer_timeout", 1)).ok());
}

TEST_CASE("signal arguments must fit the connected method's parameters") {
	FakeProject project;
	compiler::Scene scene;
	scene.path = "res://main.tscn";
	scene.nodes = { { ".", "Node", "res://test.pet" } };
	scene.connections = { { "child_entered_tree", ".", ".", "_on_child", 0, 0 } };
	project.scenes = { scene };
	CHECK(compile_in("extends Node\n\nfunc _on_child(node: Node):\n\tpass\n", project).ok());
	// A handler may accept None where the engine might pass it (spec §10.2).
	CHECK(compile_in("extends Node\n\nfunc _on_child(node: Node?):\n\tpass\n", project).ok());
	CHECK(contains(messages(compile_in("extends Node\n\nfunc _on_child(node: int):\n\tpass\n", project)), "which passes Node here"));
}

TEST_CASE("a signal declared by a script of this language is checked like the engine's (spec §4.7, §7.7)") {
	// main.tscn of the FPS starter kit connects the player's health_updated to the HUD.
	const auto connected = [](const std::string &p_signal, const std::string &p_handler) {
		FakeProject project = hud_scene();
		project.add_script("res://player.pet", "extends Node3D\nsignal health_updated(health: int)\n");
		project.scenes[0].nodes[1].instance_script = "res://player.pet";
		project.scenes[0].connections = { { p_signal, "Player", "HUD", "_on_health_updated", 0, 0 } };
		return messages(compile_in("extends CanvasLayer\n\nfunc _on_health_updated(" + p_handler + "):\n\tpass\n", project));
	};
	CHECK(connected("health_updated", "health: int").empty());
	CHECK(contains(connected("health_updated", "health: String"), "which passes int here"));
	CHECK(contains(connected("health_updated", ""), "which passes 1 argument(s); _on_health_updated() takes 0."));
	CHECK(contains(connected("health_changed", "health: int"), "res://main.tscn: the signal \"health_changed\" of Player is connected to _on_health_updated(), but Player has no such signal."));
	// The engine class's own signals.
	CHECK(connected("ready", "").empty());
	CHECK(contains(messages(compile_in("extends Node3D\n\nfunc _on_timer_timeout():\n\tpass\n", [] {
		FakeProject project = timer_scene("_on_timer_timeout");
		project.scenes[0].connections[0].signal = "timed_out";
		return project;
	}())), "but Timer has no such signal."));
}

TEST_CASE("$X has the class of the node at that path in the scenes that use the script (spec §7.1)") {
	// scripts/hud.gd of the FPS starter kit, with the parameter annotated.
	const char *hud = "extends CanvasLayer\n\nfunc _on_health_updated(health: int):\n\t$Health.text = str(health) + \"%\"\n";
	const compiler::CompileResult result = compile_in(hud, hud_scene());
	REQUIRE_MESSAGE(result.ok(), messages(result));
	// Checked again at run time, in case the tree has changed since.
	REQUIRE(result.module.classes.size() == 1);
	CHECK(result.module.classes[0].name == "Label");

	CHECK(hud_errors("$\"Health\".text = \"\"").empty());
	CHECK(contains(hud_errors("$Crosshair.text = \"\""), "TextureRect has no property \"text\""));
	CHECK(contains(hud_errors("var n: Node3D = $Health"), "Cannot assign a value of type Label as Node3D"));
}

TEST_CASE("$X of a node with a script of this language has the script's class") {
	FakeProject project = hud_scene();
	project.add_script("res://bar.pet", "extends Control\nclass_name Bar\n\npub func fill(x: float):\n\tpass\n", "Bar");
	project.scenes[0].nodes.push_back({ "HUD/Bar", "Control", "res://bar.pet" });
	const compiler::CompileResult result = compile_in("extends CanvasLayer\n\nfunc f():\n\t$Bar.fill(0.5)\n", project);
	REQUIRE_MESSAGE(result.ok(), messages(result));
	REQUIRE(result.module.classes.size() == 1);
	CHECK(result.module.classes[0].script == "res://bar.pet");
	CHECK(contains(hud_errors("$Bar.fill(\"full\")", project), "Cannot pass a value of type String as float"));
}

// Spec §7.4.
TEST_CASE("preload() of a scene is a PackedScene of the scene's root") {
	FakeProject project;
	project.add_script("res://objects/impact.pet", "extends Node3D\n\npub func play_shot():\n\tpass\n");
	compiler::Scene impact;
	impact.path = "res://objects/impact.tscn";
	impact.nodes = { { ".", "Node3D", "res://objects/impact.pet" } };
	compiler::Scene platform;
	platform.path = "res://objects/platform.tscn";
	platform.nodes = { { ".", "Node3D", "" } };
	compiler::Scene legacy;
	legacy.path = "res://objects/legacy.tscn";
	legacy.nodes = { { ".", "Node3D", "res://objects/legacy.gd" } };
	project.scenes = { impact, platform, legacy };
	const auto errors = [&](const std::string &p_body) {
		return messages(compile_in("extends Node\n\nfunc f(path: String):\n\t" + p_body + "\n", project));
	};

	// The class of the root's script, when it has one of this language.
	CHECK(errors("var shot := preload(\"res://objects/impact.tscn\").instantiate()\n\tshot.play_shot()").empty());
	CHECK(contains(errors("var n: Label = preload(\"res://objects/impact.tscn\").instantiate()"), "Cannot assign a value of type impact.pet as Label"));
	CHECK(errors("var s: PackedScene[Node3D] = preload(\"res://objects/platform.tscn\")").empty());
	CHECK(errors("var n: Node3D = preload(\"res://objects/legacy.tscn\").instantiate()").empty());
	// Relative to the script's directory, and simplified as the engine does.
	CHECK(errors("var s: PackedScene[Node3D] = preload(\"objects/platform.tscn\")").empty());
	CHECK(errors("var s: PackedScene[Node3D] = preload(\"./objects/../objects/platform.tscn\")").empty());
	CHECK(errors("var s: PackedScene[Node3D] = preload(\"res://objects//platform.tscn\")").empty());

	CHECK(contains(errors("var s := preload(\"res://missing.tscn\")"), "There is no scene at res://missing.tscn."));
	CHECK(contains(errors("var s := preload(\"res://icon.png\")"), "preload() of a resource other than a scene is not supported yet."));
	CHECK(contains(errors("var s := preload(path)"), "preload() takes one literal path."));

	// It loads as load() does, and checks that it got a scene.
	const compiler::CompileResult result = compile_in("extends Node\n\nfunc f():\n\tvar s := preload(\"res://objects/platform.tscn\")\n", project);
	REQUIRE_MESSAGE(result.ok(), messages(result));
	REQUIRE(result.module.engine_calls.size() == 1);
	CHECK(result.module.engine_calls[0].method == "load");
	REQUIRE(result.module.classes.size() >= 1);
	CHECK(result.module.classes.back().name == "PackedScene");

	// Scenes are not read while loading.
	CHECK(compile_in("extends Node\nvar s = preload(\"res://missing.tscn\")\n", project, true).ok());
}

TEST_CASE("a path that a scene using the script lacks is an error marked with that scene (spec §7.1, §7.2)") {
	CHECK(contains(hud_errors("$Score.text = \"\""), "res://main.tscn: HUD has no node at \"Score\""));

	FakeProject two = hud_scene();
	compiler::Scene menu;
	menu.path = "res://menu.tscn";
	menu.nodes = { { ".", "CanvasLayer", "res://test.pet" }, { "Score", "Label", "" }, { "Health", "TextureRect", "" } };
	two.scenes.push_back(menu);
	const std::string score = hud_errors("$Score.text = \"\"", two);
	CHECK(contains(score, "res://main.tscn: HUD has no node at \"Score\""));
	CHECK(!contains(score, "menu.tscn"));
	CHECK(contains(hud_errors("$Health.text = \"\"", two), "$Health is a Label in res://main.tscn but a TextureRect in res://menu.tscn"));

	// A script that no scene uses has no nodes to type $X from.
	CHECK(contains(hud_errors("$Health.text = \"\"", FakeProject()), "no scene does"));
}

TEST_CASE("paths that are not typed yet are refused rather than guessed") {
	FakeProject project = hud_scene();
	project.scenes[0].nodes.push_back({ "HUD/Bar", "Control", "", true });
	CHECK(contains(hud_errors("var n: Node = $Bar/Fill", project), "paths into an instanced scene, such as HUD/Bar, are not supported yet"));
	CHECK(contains(hud_errors("var n: Node = $\"../Player\""), "are supported yet"));
	CHECK(contains(hud_errors("var n: Node = $\"/root/Main\""), "are supported yet"));
}

// Spec §6.3.
TEST_CASE("@onready members are set when the node is ready, from the scene") {
	const char *source = "extends CanvasLayer\n@onready var health = $Health\nvar shown := \"\"\n\nfunc _ready():\n\tshown = health.text\n";
	const compiler::CompileResult result = compile_in(source, hud_scene());
	REQUIRE_MESSAGE(result.ok(), messages(result));
	CHECK(result.module.members[0].is_onready);
	CHECK(result.module.members[0].class_name == "Label");
	REQUIRE(result.module.ready_function >= 0);
	REQUIRE(result.module.init_function >= 0);
	// shown is written on a new instance, health when the node is ready.
	const auto stores = [&](int32_t p_function, size_t p_member) {
		const bytecode::Function &fn = result.module.functions[p_function];
		const uint32_t offset = result.module.members[p_member].offset;
		for (size_t pc = 0; pc < fn.code.size(); pc += bytecode::instruction_length(fn.code.data() + pc)) {
			const bytecode::Op op = bytecode::Op(fn.code[pc]);
			if (((op == bytecode::Op::STORE_MEMBER_8 || op == bytecode::Op::STORE_MEMBER_STRING) && fn.code[pc + 1] == offset) || (op == bytecode::Op::STORE_MEMBER_VALUE && fn.code[pc + 2] == offset)) {
				return true;
			}
		}
		return false;
	};
	CHECK(stores(result.module.init_function, 1));
	CHECK(!stores(result.module.init_function, 0));
	CHECK(stores(result.module.ready_function, 0));

	const auto errors = [](const std::string &p_members) {
		return messages(compile_in("extends CanvasLayer\n" + p_members + "\n", hud_scene()));
	};
	CHECK(errors("@onready var a := 1\n@onready var b := a + 1").empty());
	// Members declared later are set by then; player.gd of the FPS starter kit.
	const compiler::CompileResult later = compile_in("extends CanvasLayer\n@onready var weapon = weapons[weapon_index]\nvar weapon_index := 0\nvar weapons: Array[String] = []\n\nfunc f() -> String:\n\treturn weapon + \"!\"\n", hud_scene());
	REQUIRE_MESSAGE(later.ok(), messages(later));
	CHECK(later.module.members[0].type == bytecode::ValueType::String);
	CHECK(contains(errors("@onready var a := 1\nvar b := a"), "\"a\" is set when the node is ready, after this initializer runs"));
	CHECK(contains(errors("@onready var b := a\n@onready var a := 1"), "\"a\" is not initialized yet"));
	CHECK(contains(errors("@onready @export var a := 1"), "@onready and @export do not go together"));
	CHECK(contains(errors("@onready var a := 1\n\nfunc _init():\n\tprint(a)"), "after _init() runs"));
	CHECK(contains(errors("@onready var a := 1\n\nfunc _enter_tree():\n\tprint(a)"), "after _enter_tree() runs"));
	CHECK(contains(messages(compile("extends Resource\n@onready var a := 1\n")), "@onready is for Node scripts"));
}

TEST_CASE("while loading, only declarations are compiled") {
	// The body has a type error, and the scene's method does not exist; neither
	// is checked without bodies and scenes.
	const char *source = "extends Node3D\n@export var speed := 2.0\n\npub func f(x: int) -> String:\n\tvar s: String = x\n\treturn s\n";
	const compiler::CompileResult result = compile_in(source, timer_scene("_on_missing"), true);
	REQUIRE_MESSAGE(result.ok(), messages(result));
	CHECK(result.module.declarations_only);
	const bytecode::Function &f = function_named(result.module, "f");
	CHECK(f.params.size() == 1);
	CHECK(f.result.type == bytecode::ValueType::String);
	CHECK(result.module.members[0].initial.f == 2.0);
	CHECK(!compile_in(source, timer_scene("_on_missing")).ok());
	// Nor are node paths, which need the scenes; a member typed by one is a
	// Variant until the full compile.
	CHECK(compile_in("extends Node3D\n\nfunc f():\n\t$Missing.text = \"\"\n", FakeProject(), true).ok());
	const compiler::CompileResult loading = compile_in("extends CanvasLayer\n@onready var health = $Health\n", hud_scene(), true);
	REQUIRE_MESSAGE(loading.ok(), messages(loading));
	CHECK(loading.module.members[0].type == bytecode::ValueType::Variant);
}
