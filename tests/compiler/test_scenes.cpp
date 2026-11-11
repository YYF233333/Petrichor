#include "bytecode/opcodes.h"
#include "doctest.h"
#include "helpers.h"

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

TEST_CASE("a method connected in a scene must be pub, since the engine finds it by name (spec §10.5)") {
	CHECK(check_scenes_in("extends Node3D\n\npub func _on_timer_timeout():\n\tpass\n", timer_scene("_on_timer_timeout")).errors.empty());
	CHECK(contains(messages(check_scenes_in("extends Node3D\n\nfunc _on_timer_timeout():\n\tpass\n", timer_scene("_on_timer_timeout"))), "is connected to _on_timer_timeout(), which is not pub; the engine calls only pub methods by name."));
}

TEST_CASE("a method the engine is to call by a literal name must be pub (spec §10.4)") {
	const auto errors = [](const std::string &p_visibility) {
		return messages(compile_in("extends Node\n\nfunc _ready():\n\tcall_deferred(\"reset\")\n\tvar me := self\n\tme.call_deferred(\"stop\")\n\n" + p_visibility + "func reset():\n\tpass\n\n" + p_visibility + "func stop():\n\tpass\n", FakeProject()));
	};
	CHECK(errors("pub ").empty());
	CHECK(contains(errors(""), "\"reset()\" is not pub, so the engine cannot find it by name; pass the method itself, as reset."));
	// Through an object of this class too.
	CHECK(contains(errors(""), "\"stop()\" is not pub"));
}

TEST_CASE("scene connections are checked against the class (spec §7.7)") {
	const char *source = "extends Node3D\n\npub func _on_timer_timeout():\n\tpass\n";
	CHECK(contains(messages(check_scenes_in(source, timer_scene("_on_missing"))), "res://enemy.tscn: the signal \"timeout\" of Timer is connected to _on_missing(), which this class does not declare."));
	CHECK(contains(messages(check_scenes_in(source, timer_scene("_on_timer_timeout", 1))), "which passes 1 argument(s); _on_timer_timeout() takes 0."));
	CHECK(check_scenes_in("extends Node3D\n\npub func _on_timer_timeout(extra: int):\n\tpass\n", timer_scene("_on_timer_timeout", 1)).errors.empty());
}

TEST_CASE("signal arguments must fit the connected method's parameters") {
	FakeProject project;
	compiler::Scene scene;
	scene.path = "res://main.tscn";
	scene.nodes = { { ".", "Node", "res://test.pet" } };
	scene.connections = { { "child_entered_tree", ".", ".", "_on_child", 0, 0 } };
	project.scenes = { scene };
	CHECK(check_scenes_in("extends Node\n\npub func _on_child(node: Node):\n\tpass\n", project).errors.empty());
	// A handler may accept None where the engine might pass it (spec §10.2).
	CHECK(check_scenes_in("extends Node\n\npub func _on_child(node: Node?):\n\tpass\n", project).errors.empty());
	CHECK(contains(messages(check_scenes_in("extends Node\n\npub func _on_child(node: int):\n\tpass\n", project)), "which passes Node here"));
}

TEST_CASE("a signal declared by a script of this language is checked like the engine's (spec §4.7, §7.7)") {
	// main.tscn of the FPS starter kit connects the player's health_updated to the HUD.
	const auto connected = [](const std::string &p_signal, const std::string &p_handler, const std::string &p_visibility = "pub ") {
		FakeProject project = hud_scene();
		project.add_script("res://player.pet", "extends Node3D\n" + p_visibility + "signal health_updated(health: int)\n");
		project.scenes[0].nodes[1].instance_script = "res://player.pet";
		project.scenes[0].connections = { { p_signal, "Player", "HUD", "_on_health_updated", 0, 0 } };
		return messages(check_scenes_in("extends CanvasLayer\n\npub func _on_health_updated(" + p_handler + "):\n\tpass\n", project));
	};
	CHECK(connected("health_updated", "health: int").empty());
	// The engine connects it by name (spec §10.5).
	CHECK(contains(connected("health_updated", "health: int", ""), "is connected to _on_health_updated(), but that signal is not pub; the engine connects only pub signals by name."));
	CHECK(contains(connected("health_updated", "health: String"), "which passes int here"));
	CHECK(contains(connected("health_updated", ""), "which passes 1 argument(s); _on_health_updated() takes 0."));
	CHECK(contains(connected("health_changed", "health: int"), "res://main.tscn: the signal \"health_changed\" of Player is connected to _on_health_updated(), but Player has no such signal."));
	// The engine class's own signals.
	CHECK(connected("ready", "").empty());
	CHECK(contains(messages(check_scenes_in("extends Node3D\n\npub func _on_timer_timeout():\n\tpass\n", [] {
		FakeProject project = timer_scene("_on_timer_timeout");
		project.scenes[0].connections[0].signal = "timed_out";
		return project;
	}())),
			"but Timer has no such signal."));
}

TEST_CASE("the members that animation tracks and synchronized properties name must be pub (spec §7.7, §10.5)") {
	// The player and the synchronizer start from their parent, the root.
	FakeProject project;
	compiler::Scene scene;
	scene.path = "res://enemy.tscn";
	scene.nodes = { { ".", "Node2D", "res://test.pet" }, { "AnimationPlayer", "AnimationPlayer", "" }, { "MultiplayerSynchronizer", "MultiplayerSynchronizer", "" } };
	scene.named_members = {
		{ "AnimationPlayer", "hit", "..", "flash_amount", false },
		{ "AnimationPlayer", "hit", "..", "flash", true },
		{ "MultiplayerSynchronizer", "", "../.", "health", false },
		// The engine's own property and method, and an override, which it finds by name.
		{ "AnimationPlayer", "hit", "..", "position", false },
		{ "AnimationPlayer", "hit", "..", "queue_free", true },
		{ "AnimationPlayer", "hit", "..", "_process", true },
	};
	project.scenes = { scene };
	const auto checked = [&](const std::string &p_visibility) {
		return check_scenes_in("extends Node2D\n\n" + p_visibility + "var flash_amount := 0.0\n" + p_visibility + "var health := 3\n\n" + p_visibility + "func flash():\n\tpass\n\nfunc _process(delta: float):\n\tpass\n", project);
	};
	CHECK(checked("pub ").errors.empty());
	const compiler::CompileResult hidden = checked("");
	REQUIRE(hidden.errors.size() == 3);
	CHECK(hidden.errors[0].message == "res://enemy.tscn: the animation \"hit\" of AnimationPlayer animates \"flash_amount\" of the root, which is not pub; the engine finds only pub properties by name.");
	CHECK(hidden.errors[1].message == "res://enemy.tscn: MultiplayerSynchronizer synchronizes \"health\" of the root, which is not pub; the engine finds only pub properties by name.");
	CHECK(hidden.errors[2].message == "res://enemy.tscn: the animation \"hit\" of AnimationPlayer calls flash() on the root, which is not pub; the engine calls only pub methods by name.");
	CHECK((hidden.errors[0].scene == "res://enemy.tscn" && hidden.errors[0].node == "."));

	// A scene that instances the script's scene animates it from outside.
	compiler::Scene level;
	level.path = "res://level.tscn";
	level.nodes = { { ".", "Node2D", "" }, { "Enemy", "Node2D", "", true, "res://test.pet" }, { "Cutscene", "AnimationPlayer", "" } };
	level.nodes[1].instance = "res://enemy.tscn";
	level.named_members = { { "Cutscene", "intro", "../Enemy", "speed", false } };
	project.scenes = { { "res://enemy.tscn", { { ".", "Node2D", "res://test.pet" } } }, level };
	const compiler::CompileResult outer = check_scenes_in("extends Node2D\n\nvar speed := 1.0\n", project);
	REQUIRE(outer.errors.size() == 1);
	CHECK(outer.errors[0].message == "res://level.tscn: the animation \"intro\" of Cutscene animates \"speed\" of Enemy, which is not pub; the engine finds only pub properties by name.");
	CHECK((outer.errors[0].scene == "res://level.tscn" && outer.errors[0].node == "Enemy"));
}

TEST_CASE("a track or a synchronized property names a member that its node's class has (spec §7.7)") {
	FakeProject project;
	compiler::Scene scene;
	scene.path = "res://enemy.tscn";
	scene.nodes = { { ".", "Node2D", "res://test.pet" }, { "AnimationPlayer", "AnimationPlayer", "" }, { "MultiplayerSynchronizer", "MultiplayerSynchronizer", "" } };
	scene.named_members = {
		{ "AnimationPlayer", "hit", "..", "flash_amont", false },
		{ "AnimationPlayer", "hit", "..", "flsh", true },
		{ "MultiplayerSynchronizer", "", "..", "helth", false },
		// A name the engine class may take without listing it.
		{ "AnimationPlayer", "hit", "..", "metadata/glow", false },
	};
	project.scenes = { scene };
	const std::string errors = messages(check_scenes_in("extends Node2D\n\npub var flash_amount := 0.0\n", project));
	CHECK(contains(errors, "res://enemy.tscn: the animation \"hit\" of AnimationPlayer animates \"flash_amont\" of the root, which this class does not declare."));
	CHECK(contains(errors, "res://enemy.tscn: the animation \"hit\" of AnimationPlayer calls flsh() on the root, which this class does not declare."));
	CHECK(contains(errors, "res://enemy.tscn: MultiplayerSynchronizer synchronizes \"helth\" of the root, which this class does not declare."));
	CHECK(!contains(errors, "glow"));
	// A class with _set() takes any name.
	const std::string taking = messages(check_scenes_in("extends Node2D\n\nfunc _set(property: StringName, value: Variant) -> bool:\n\treturn false\n", project));
	CHECK((contains(taking, "flsh") && !contains(taking, "flash_amont")));
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

TEST_CASE("$X of a node with a GDScript script has the script's class (spec §11.1)") {
	FakeProject project = hud_scene();
	compiler::ForeignClass meter;
	meter.native = "Control";
	compiler::EngineMethod fill;
	fill.name = "fill";
	fill.owner = "res://meter.gd";
	fill.params = { { "amount", compiler::Type::of(compiler::TypeKind::Float) } };
	fill.return_type = compiler::Type::of(compiler::TypeKind::Void);
	meter.methods = { fill };
	project.foreign["res://meter.gd"] = meter;
	project.global_classes["Meter"] = "res://meter.gd";
	project.scenes[0].nodes.push_back({ "HUD/Meter", "Control", "res://meter.gd" });
	CHECK(hud_errors("$Meter.fill(0.5)\n\tvar m: Meter = $Meter\n\tvar c: Control = $Meter", project).empty());
	CHECK(contains(hud_errors("$Meter.fill(\"full\")", project), "Cannot pass a value of type String as float"));
	// One that cannot be read keeps its engine class.
	project.scenes[0].nodes.push_back({ "HUD/Broken", "Label", "res://broken.gd" });
	CHECK(hud_errors("$Broken.text = \"\"", project).empty());
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
	// Other resources take the class their file loads as, or their script's
	// (modules/gdscript/gdscript_analyzer.cpp:4757-4780).
	project.resources["res://icon.png"] = { "Texture2D", "" };
	project.resources["res://odd.bin"] = { "", "" };
	project.add_script("res://weapon.pet", "class_name Weapon\nextends Resource\n@export pub var damage := 1\n", "Weapon");
	project.resources["res://rifle.tres"] = { "Resource", "Weapon" };
	project.uids["uid://b4icon"] = "res://icon.png";
	CHECK(errors("var t: Texture2D = preload(\"res://icon.png\")\n\tvar w: int = preload(\"res://rifle.tres\").damage\n\tvar u: Texture2D = preload(\"uid://b4icon\")").empty());
	CHECK(contains(errors("var t: Label = preload(\"res://icon.png\")"), "Cannot assign a value of type Texture2D as Label"));
	CHECK(contains(errors("var t := preload(\"res://missing.png\")"), "There is no resource at res://missing.png."));
	CHECK(contains(errors("var t := preload(\"uid://nope\")"), "There is no resource with the uid uid://nope."));
	CHECK(contains(errors("var t := preload(\"res://odd.bin\")"), "The file at res://odd.bin is not a resource the engine knows."));
	CHECK(contains(errors("var t := preload(\"res://missing.pet\")"), "There is no script of this language at res://missing.pet."));
	CHECK(contains(errors("var t := preload(\"res://missing.gd\")"), "There is no script at res://missing.gd, or it cannot be loaded."));
	CHECK(contains(errors("var s := preload(path)"), "preload() takes one literal path, a String."));
	CHECK(contains(errors("var s := preload(&\"res://objects/platform.tscn\")"), "preload() takes one literal path, a String."));

	// A constant that the class loads as it loads, also in a function body.
	const compiler::CompileResult result = compile_in("extends Node\n\nfunc f():\n\tvar s := preload(\"res://objects/platform.tscn\")\n", project);
	REQUIRE_MESSAGE(result.ok(), messages(result));
	CHECK(result.module.engine_calls.empty());
	REQUIRE(result.module.constants.size() == 1);
	CHECK((result.module.constants[0].load && result.module.constants[0].s == "res://objects/platform.tscn"));
	find_instruction(function_named(result.module, "f"), bytecode::Op::CONST_VALUE);

	// Scenes are not read while loading; a constant still holds the preload,
	// which GDScript reads by name (spec §11.3).
	CHECK(compile_in("extends Node\nvar s = preload(\"res://missing.tscn\")\n", project, true).ok());
	const compiler::CompileResult loading = compile_in("extends Node\npub const SCENE = preload(\"res://objects/platform.tscn\")\n", project, true);
	REQUIRE_MESSAGE(loading.ok(), messages(loading));
	REQUIRE(loading.module.named_constants.size() == 1);
	const bytecode::Constant &scene = loading.module.constants[loading.module.named_constants[0].constant];
	CHECK((scene.load && scene.s == "res://objects/platform.tscn"));
	std::string problem;
	CHECK_MESSAGE(bytecode::verify(loading.module, problem), problem);
	// So does one of a container of preloads, untyped there.
	const compiler::CompileResult containers = compile_in("extends Node\npub const SCENES = [preload(\"res://objects/platform.tscn\"), 1]\npub const BY_NAME = {\"p\": preload(\"res://objects/platform.tscn\")}\n", project, true);
	REQUIRE_MESSAGE(containers.ok(), messages(containers));
	REQUIRE(containers.module.named_constants.size() == 2);
	const bytecode::Constant &list = containers.module.constants[containers.module.named_constants[0].constant];
	CHECK((list.args.size() == 2 && list.args[0].load && list.args[0].s == "res://objects/platform.tscn"));
	CHECK(containers.module.constants[containers.module.named_constants[1].constant].args.size() == 2);
	CHECK_MESSAGE(bytecode::verify(containers.module, problem), problem);
}

TEST_CASE("load() of a literal path in the project is typed as preload() types it (spec §7.4)") {
	FakeProject project;
	project.add_script("res://objects/impact.pet", "extends Node3D\n\npub func play_shot():\n\tpass\n");
	compiler::Scene impact;
	impact.path = "res://objects/impact.tscn";
	impact.nodes = { { ".", "Node3D", "res://objects/impact.pet" } };
	project.scenes = { impact };
	project.resources["res://icon.png"] = { "Texture2D", "" };
	project.resources["res://units/unit.gd"] = { "GDScript", "" };
	const auto errors = [&](const std::string &p_body) {
		return messages(compile_in("extends Node\n\nfunc f(path: String):\n\t" + p_body + "\n", project));
	};
	CHECK(errors("load(\"res://objects/impact.tscn\").instantiate().play_shot()\n\tvar t: Texture2D = load(\"icon.png\")\n\tvar s: GDScript = load(\"res://units/unit.gd\")").empty());
	// A path the project does not have, or outside it, or known at run time.
	CHECK(contains(errors("var s: PackedScene = load(\"res://missing.tscn\")"), "Cannot assign a value of type Resource? as PackedScene"));
	CHECK(contains(errors("var s: Resource = load(\"user://save.tres\")"), "Cannot assign a value of type Resource? as Resource"));
	CHECK(contains(errors("var s: PackedScene = load(path)"), "Cannot assign a value of type Resource? as PackedScene"));
	// Checked at run time, which a failed load does not pass.
	const compiler::CompileResult result = compile_in("extends Node\n\nfunc f():\n\tvar t := load(\"res://icon.png\")\n", project);
	REQUIRE_MESSAGE(result.ok(), messages(result));
	find_instruction(function_named(result.module, "f"), bytecode::Op::CHECK_OBJECT);
}

TEST_CASE("instantiate() of a scene known statically takes the exports it does not assign; so may no PackedScene[T] it converts to (spec §7.4)") {
	FakeProject project;
	project.add_script("res://enemy.pet", "class_name Enemy\nextends Node3D\n@export pub var player: Node3D\n@export pub var camera: Node3D\npub var target: Node\n", "Enemy");
	compiler::Scene enemy;
	enemy.path = "res://enemy.tscn";
	enemy.nodes = { { ".", "Node3D", "res://enemy.pet" } };
	compiler::SceneValue path;
	path.type = "NodePath";
	enemy.nodes[0].properties = { { "camera", path } };
	project.scenes = { enemy };
	const auto errors = [&](const std::string &p_body) {
		return messages(compile_in("extends Node\nconst ENEMY := preload(\"res://enemy.tscn\")\n\nfunc f(p: Node3D):\n\t" + p_body + "\n", project));
	};
	CHECK(errors("var e := ENEMY.instantiate(player = p, target = self)").empty());
	const char *needs = "instantiate() needs player = ..., an export of Enemy that must be assigned and res://enemy.tscn does not assign.";
	CHECK(contains(errors("var e := ENEMY.instantiate(target = self)"), needs));
	CHECK(contains(errors("var e := preload(\"res://enemy.tscn\").instantiate(target = self)"), needs));
	CHECK(contains(errors("var e := load(\"res://enemy.tscn\").instantiate(target = self)"), needs));
	// A variable or a parameter is not known statically: instantiate()
	// asks only for the members of T that are not exported.
	CHECK(contains(errors("var s := ENEMY"), "res://enemy.tscn leaves \"player\" to instantiate(), but a PackedScene[Enemy] does not ask for it."));
	project.scenes[0].nodes[0].properties.push_back({ "player", path });
	CHECK(errors("var s: PackedScene[Enemy] = ENEMY\n\tvar e := s.instantiate(target = self)").empty());
	CHECK(contains(errors("g(ENEMY)\n\nfunc g(s: PackedScene[Node3D]):\n\tpass"), "res://enemy.tscn leaves \"target\" to instantiate(), but a PackedScene[Node3D] does not ask for it."));
	// Nor may a scene that a scene stores for an export, marked there.
	compiler::Scene level;
	level.path = "res://level.tscn";
	level.nodes = { { ".", "Node", "" }, { "Spawner", "Node", "res://test.pet" } };
	compiler::SceneValue stored;
	stored.type = "Object";
	stored.class_name = "PackedScene";
	stored.path = enemy.path;
	level.nodes[1].properties = { { "wave", stored } };
	project.scenes.push_back(level);
	const compiler::CompileResult spawner = compile_in("extends Node\n@export pub var wave: PackedScene[Node3D]\n", project);
	REQUIRE(spawner.errors.size() == 1);
	CHECK(spawner.errors[0].message == "res://level.tscn: \"wave\" of Spawner holds res://enemy.tscn, which leaves \"target\" to instantiate(), but a PackedScene[Node3D] does not ask for it.");
	CHECK((spawner.errors[0].scene == level.path && spawner.errors[0].node == "Spawner"));
	CHECK(compile_in("extends Node\n@export pub var wave: PackedScene[Enemy]\n", project).errors.empty());
}

// Spec §7.6.
TEST_CASE("a preload whose file loads the script again is an error") {
	FakeProject project;
	compiler::Scene own;
	own.path = "res://own.tscn";
	own.nodes = { { ".", "Node", "res://test.pet" } };
	compiler::Scene other;
	other.path = "res://other.tscn";
	other.nodes = { { ".", "Node", "res://other.gd" } };
	compiler::Scene plain;
	plain.path = "res://plain.tscn";
	plain.nodes = { { ".", "Node", "" } };
	project.scenes = { own, other, plain };
	project.loads["res://own.tscn"] = { "res://test.pet" };
	project.loads["res://other.tscn"] = { "res://other.gd", "res://plain.tscn" };
	project.sources["res://other.gd"] = "extends Node\n\nconst BACK = preload(\"own.tscn\")\n";
	project.add_script("res://child.pet", "extends \"res://test.pet\"\n");
	const auto errors = [&](const std::string &p_preload) {
		return messages(compile_in("extends Node\n\nconst S = preload(\"" + p_preload + "\")\n", project));
	};
	CHECK(errors("res://plain.tscn").empty());
	CHECK(contains(errors("res://own.tscn"), "This preload loads this script again (res://own.tscn -> res://test.pet); load one of them with load() where it is used."));
	CHECK(contains(errors("res://other.tscn"), "(res://other.tscn -> res://other.gd -> res://own.tscn -> res://test.pet)"));
	CHECK(contains(errors("res://child.pet"), "(res://child.pet -> res://test.pet)"));
	CHECK(contains(errors("res://test.pet"), "(res://test.pet)"));
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

TEST_CASE("@owner(T): owner has type T, and scenes give the script such an owner (spec §7.3)") {
	FakeProject project;
	project.add_script("res://combat.pet", "class_name Combat\n\npub interface Combatant:\n\tvar health: int\n", "Combat");
	project.add_script("res://brick.pet", "class_name Brick\nextends Node3D implements Combat.Combatant\npub var health: int = 3\n", "Brick");
	// The component's own scene, whose root has the script, and a scene that
	// instances it under a root that satisfies the requirement.
	compiler::Scene component;
	component.path = "res://component.tscn";
	component.nodes = { { ".", "Node", "res://test.pet" } };
	compiler::Scene brick;
	brick.path = "res://brick.tscn";
	brick.nodes = { { ".", "Node3D", "res://brick.pet" }, { "Component", "Node", "", true, "res://test.pet" } };
	project.scenes = { component, brick };
	const std::string source = "@owner(Combat.Combatant)\nextends Node\n\nfunc f() -> int:\n\towner.health -= 1\n\treturn owner.health\n";
	CHECK(messages(compile_in(source, project)).empty());
	CHECK(check_scenes_in(source, project).errors.empty());

	// The scenes' checks report the rest (architecture §6.4).
	compiler::Scene plain;
	plain.path = "res://plain.tscn";
	plain.nodes = { { ".", "Node3D", "" }, { "Component", "Node", "", true, "res://test.pet" }, { "Direct", "Node", "res://test.pet" } };
	project.scenes.push_back(plain);
	const std::string errors = messages(check_scenes_in(source, project));
	CHECK(contains(errors, "res://plain.tscn: the owner of Component is Node3D, not Combat.Combatant (@owner)."));
	CHECK(contains(errors, "res://plain.tscn: the owner of Direct is Node3D, not Combat.Combatant (@owner)."));
	// The root of a scene used as the root has no owner.
	project.main = "res://component.tscn";
	CHECK(contains(messages(check_scenes_in(source, project)), "res://component.tscn: the main scene's root has no owner (@owner)."));

	CHECK(contains(messages(compile_in("extends Node\n\nfunc f():\n\tprint(owner.name)\n", project)), "The value may be None (Node?)"));
	CHECK(contains(messages(compile_in("@owner(Node?)\nextends Node\n", FakeProject())), "@owner takes a class or an interface, not Node?."));
	CHECK(contains(messages(compile_in("@owner(Node)\nextends RefCounted\n", FakeProject())), "@owner is for classes that extend Node."));
}

TEST_CASE("@parent(T): get_parent() and $\"..\" have type T, and scenes give the script such a parent (spec §7.3)") {
	// A scene whose root has the script, instanced in another; and a scene
	// with a node that has it.
	FakeProject project;
	compiler::Scene paddle;
	paddle.path = "res://paddle.tscn";
	paddle.nodes = { { ".", "Node2D", "res://test.pet" } };
	compiler::Scene level;
	level.path = "res://level.tscn";
	level.nodes = { { ".", "Node2D", "" }, { "Paddle", "Node2D", "res://test.pet" } };
	compiler::Scene breakout;
	breakout.path = "res://breakout.tscn";
	breakout.nodes = { { ".", "Node2D", "" }, { "Paddle", "Node2D", "", true, "res://test.pet" } };
	project.scenes = { paddle, level, breakout };
	const std::string source = "@parent(Node2D)\nextends Node2D\n\nfunc f() -> float:\n\treturn get_parent().position.x + $\"..\".position.y + self.get_parent().position.x\n";
	const compiler::CompileResult result = compile_in(source, project);
	REQUIRE_MESSAGE(result.ok(), messages(result));
	// Checked as the node gets a parent; get_parent() is checked where it is called.
	REQUIRE(result.module.parent_class >= 0);
	CHECK(result.module.classes[result.module.parent_class].name == "Node2D");
	find_instruction(function_named(result.module, "f"), bytecode::Op::CHECK_OBJECT);
	CHECK(check_scenes_in(source, project).errors.empty());

	// The scenes' checks report the rest (architecture §6.4).
	compiler::Scene menu;
	menu.path = "res://menu.tscn";
	menu.nodes = { { ".", "Control", "" }, { "Paddle", "Node2D", "", true, "res://test.pet" }, { "Direct", "Node2D", "res://test.pet" } };
	project.scenes.push_back(menu);
	project.main = "res://paddle.tscn";
	const std::string errors = messages(check_scenes_in(source, project));
	CHECK(contains(errors, "res://menu.tscn: the parent of Paddle is Control, not Node2D (@parent)."));
	CHECK(contains(errors, "res://menu.tscn: the parent of Direct is Control, not Node2D (@parent)."));
	CHECK(contains(errors, "res://paddle.tscn: the parent of the main scene's root is Window, not Node2D (@parent)."));
	CHECK(!contains(errors, "level.tscn"));
	CHECK(compile_in(source, project).ok());

	CHECK(contains(messages(compile_in("extends Node\n\nfunc f():\n\tprint(get_parent().name)\n", FakeProject())), "The value may be None (Node?)"));
	CHECK(contains(messages(compile_in("@parent(Node?)\nextends Node\n", FakeProject())), "@parent takes a class or an interface, not Node?."));
	CHECK(contains(messages(compile_in("@parent(Node)\n@parent(Node)\nextends Node\n", FakeProject())), "The class already has @parent."));
}

TEST_CASE("a scene gives a subclass what its base requires, which is marked on the subclass's node (spec §7.3)") {
	FakeProject project;
	project.add_script("res://archer.pet", "extends \"res://test.pet\"\n");
	compiler::Scene menu;
	menu.path = "res://menu.tscn";
	menu.nodes = { { ".", "Control", "" }, { "Archer", "Node2D", "res://archer.pet" }, { "Instanced", "Node2D", "", true, "res://archer.pet" } };
	project.scenes = { menu };
	const std::vector<compiler::Diagnostic> errors = check_scenes_in("@parent(Node2D)\nextends Node2D\n", project).errors;
	REQUIRE(errors.size() == 2);
	CHECK(errors[0].message == "res://menu.tscn: the parent of Archer is Control, not Node2D (@parent).");
	CHECK((errors[0].node == "Archer" && errors[0].file == "res://test.pet" && errors[0].line == 1));
	CHECK(errors[1].node == "Instanced");
}

TEST_CASE("an abstract class is set on no node of a scene, which the scene's check reports (spec §4.1)") {
	FakeProject project;
	const std::string source = "@abstract\nextends Node3D\n\n@abstract func aim() -> float\n";
	project.add_script("res://test.pet", source);
	project.add_script("res://archer.pet", "extends \"res://test.pet\"\n\nfunc aim() -> float:\n\treturn 1.0\n");
	compiler::Scene scene;
	scene.path = "res://enemies.tscn";
	scene.nodes = { { ".", "Node3D", "" }, { "Unit", "Node3D", "res://test.pet" }, { "Archer", "Node3D", "res://archer.pet" } };
	project.scenes = { scene };
	const std::vector<compiler::Diagnostic> errors = check_scenes_in(source, project).errors;
	REQUIRE(errors.size() == 1);
	CHECK(errors[0].message == "res://enemies.tscn: Unit has test.pet, which is abstract and cannot be set on a node.");
	CHECK((errors[0].scene == "res://enemies.tscn" && errors[0].node == "Unit"));
	CHECK((errors[0].file == "res://test.pet" && errors[0].line == 2));
	CHECK(compile_in(source, project).errors.empty());
}

TEST_CASE("%Name is the node with that unique name, which the scene's root owns (spec §7.1)") {
	// The engine looks in the unique names the node owns, then in its owner's
	// (scene/main/node.cpp:1942-1950).
	FakeProject project = hud_scene();
	project.scenes[0].nodes[4].unique = true;
	project.scenes[0].nodes.push_back({ "HUD/Panel", "Panel", "" });
	project.scenes[0].nodes.push_back({ "HUD/Panel/Ammo", "Label", "", false, "", true });
	project.scenes[0].nodes.push_back({ "HUD/Panel/Ammo/Icon", "TextureRect", "" });
	CHECK(hud_errors("%Health.text = \"\"\n\t$%Ammo.text = %Health.text\n\t$\"%Ammo\".text = \"\"", project).empty());
	CHECK(contains(hud_errors("var n: Node3D = %Ammo", project), "Cannot assign a value of type Label as Node3D"));
	CHECK(contains(hud_errors("%Crosshair.visible = false", project), "res://main.tscn: no node has the unique name %Crosshair"));
	// On from a unique node, as GDScript writes it
	// (modules/gdscript/gdscript_parser.cpp:3609-3685).
	CHECK(contains(hud_errors("var n: Label = %Ammo/Icon", project), "Cannot assign a value of type TextureRect as Label"));
	CHECK(contains(hud_errors("var n: Node3D = %Ammo/%Health", project), "Cannot assign a value of type Label as Node3D"));
	CHECK(hud_errors("var n := 7 % 2\n\tn %= 2", project).empty());
	// The names an instanced scene's root owns are that scene's.
	compiler::Scene player;
	player.path = "res://player.tscn";
	player.nodes = { { ".", "Node3D", "" }, { "Head", "Node3D", "" }, { "Head/Gun", "MeshInstance3D", "", false, "", true } };
	project.scenes.push_back(player);
	project.scenes[0].nodes[1].instance = "res://player.tscn";
	CHECK(contains(hud_errors("var n: Label = $\"../Player\"/%Gun", project), "Cannot assign a value of type MeshInstance3D as Label"));
	CHECK(contains(hud_errors("var n = %Gun", project), "res://main.tscn: no node has the unique name %Gun."));

	// Ring's paddle.gd uses $%HealthIcon, which breakout.tscn has: the root of
	// paddle.tscn has breakout's root as its owner once instanced there.
	FakeProject paddle;
	compiler::Scene scene;
	scene.path = "res://paddle.tscn";
	scene.nodes = { { ".", "Node2D", "res://test.pet" } };
	paddle.scenes = { scene };
	const char *source = "extends Node2D\n\nfunc f():\n\tvar icon: Label = $%HealthIcon\n";
	CHECK(contains(messages(compile_in(source, paddle)), "res://paddle.tscn: %HealthIcon leads out of the scene, and no scene instances it."));
	compiler::Scene breakout;
	breakout.path = "res://breakout.tscn";
	breakout.nodes = { { ".", "Node2D", "" }, { "Paddle", "Node2D", "", true, "res://test.pet" }, { "UI/HealthIcon", "TextureRect", "", false, "", true } };
	breakout.nodes[1].instance = "res://paddle.tscn";
	paddle.scenes.push_back(breakout);
	CHECK(contains(messages(compile_in(source, paddle)), "Cannot assign a value of type TextureRect as Label"));
	paddle.scenes[1].nodes[2].unique = false;
	const compiler::CompileResult lacking = compile_in(source, paddle);
	REQUIRE(lacking.errors.size() == 1);
	CHECK(lacking.errors[0].message == "res://breakout.tscn: no node has the unique name %HealthIcon.");
	CHECK((lacking.errors[0].scene == "res://breakout.tscn" && lacking.errors[0].node == "Paddle"));
}

TEST_CASE("a path goes into instanced scenes, with the changes the scene stores for them (spec §7.1)") {
	FakeProject project = hud_scene();
	compiler::Scene player;
	player.path = "res://player.tscn";
	player.nodes = { { ".", "Node3D", "" }, { "Head", "Node3D", "" }, { "Head/Mesh", "MeshInstance3D", "" } };
	project.scenes.push_back(player);
	project.add_script("res://bar.pet", "extends Node3D\n\npub func fill(x: float):\n\tpass\n");
	project.scenes[0].nodes[1].instance = "res://player.tscn";
	// The scene gives the instance's Head a script, and adds a node under it.
	project.scenes[0].nodes.push_back({ "Player/Head", "", "res://bar.pet" });
	project.scenes[0].nodes.push_back({ "Player/Head/Sound", "AudioStreamPlayer", "" });
	CHECK(contains(hud_errors("var m: Label = $\"../Player/Head/Mesh\"", project), "Cannot assign a value of type MeshInstance3D as Label"));
	CHECK(hud_errors("$\"../Player/Head\".fill(0.5)\n\tvar s: AudioStreamPlayer = $\"../Player/Head/Sound\"", project).empty());
	CHECK(contains(hud_errors("var n = $\"../Player/Head/Gun\"", project), "res://main.tscn: HUD has no node at \"../Player/Head/Gun\"."));
}

TEST_CASE("a path with .. goes up to the parent in the scene, and $\"..\" is a Node (spec §7.1, §7.3)") {
	// Ring's goal_test_level_manager.gd, beside the level it manages:
	// `@onready var bricks: Node2D = $"../Level/Bricks"`.
	FakeProject project;
	compiler::Scene scene;
	scene.path = "res://goal_test_level.tscn";
	scene.nodes = { { ".", "Node2D", "" }, { "Level", "Node2D", "" }, { "Level/Bricks", "Node2D", "" }, { "Manager", "Node", "res://test.pet" } };
	project.scenes = { scene };
	const auto errors = [&](const std::string &p_body) {
		return messages(compile_in("extends Node\n\nfunc f():\n\t" + p_body + "\n", project));
	};
	CHECK(errors("var bricks: Node2D = $\"../Level/Bricks\"\n\tvar root: Node2D = $\"../Level/..\"").empty());
	CHECK(contains(errors("var n: Label = $\"../Level\""), "Cannot assign a value of type Node2D as Label"));
	CHECK(contains(errors("var n = $\"../Lvl\""), "res://goal_test_level.tscn: Manager has no node at \"../Lvl\""));
	CHECK(contains(errors("var n = $\"../..\""), "res://goal_test_level.tscn: $\"../..\" leads out of the scene, and no scene instances it."));
	// Above the root, in each scene that instances it.
	compiler::Scene game;
	game.path = "res://game.tscn";
	game.nodes = { { ".", "Node", "" }, { "Level", "Node2D", "", true }, { "Score", "Label", "" } };
	game.nodes[1].instance = "res://goal_test_level.tscn";
	project.scenes.push_back(game);
	CHECK(contains(errors("var n: Node2D = $\"../../Score\""), "Cannot assign a value of type Label as Node2D"));
	CHECK(errors("var n: Label = $\"../../Score\"").empty());
	// The parent is any node, as in GDScript, with or without scenes.
	CHECK(errors("var n: Node = $\"..\"").empty());
	CHECK(contains(errors("var n: Node2D = $\"..\""), "Cannot assign a value of type Node as Node2D"));
	CHECK(compile("extends Node\n\nfunc f() -> Node:\n\treturn $\"..\"\n").ok());
}

TEST_CASE("get_node() of a literal path is typed as $X; get_node_or_null() as T?, which a scene may lack (spec §7.1, §7.2)") {
	FakeProject project = hud_scene();
	compiler::Scene menu;
	menu.path = "res://menu.tscn";
	menu.nodes = { { ".", "CanvasLayer", "res://test.pet" }, { "Score", "Label", "" } };
	project.scenes.push_back(menu);
	CHECK(contains(hud_errors("var n: Node3D = get_node(\"Health\")", project), "Cannot assign a value of type Label as Node3D"));
	CHECK(contains(hud_errors("var n = get_node(\"Score\")", project), "res://main.tscn: HUD has no node at \"Score\"."));
	CHECK(contains(hud_errors("var n: Label = get_node_or_null(\"Score\")", project), "Cannot assign a value of type Label? as Label"));
	CHECK(hud_errors("var n: Label? = get_node_or_null(\"Score\")\n\tvar m: Node? = get_node_or_null(\"Ammo\")", project).empty());
	CHECK(contains(hud_errors("var n: Node = get_node_or_null(\"..\")", project), "Cannot assign a value of type Node? as Node"));
	// Checked at run time unless it is None, in case the tree has changed since.
	const compiler::CompileResult result = compile_in("extends CanvasLayer\n\nfunc f() -> Label?:\n\treturn get_node_or_null(\"Score\")\n", project);
	REQUIRE_MESSAGE(result.ok(), messages(result));
	find_instruction(function_named(result.module, "f"), bytecode::Op::IS_NONE);
	find_instruction(function_named(result.module, "f"), bytecode::Op::CHECK_OBJECT);
	CHECK(contains(messages(compile_in("@parent(Node2D)\nextends Node\n\nfunc f():\n\tvar p: Label = get_node(\"..\")\n", FakeProject())), "Cannot assign a value of type Node2D as Label"));
}

TEST_CASE("a scene used only as the base of inherited scenes may lack a node that they have (spec §7.2)") {
	FakeProject project;
	compiler::Scene base;
	base.path = "res://enemy.tscn";
	base.nodes = { { ".", "Node3D", "res://test.pet" } };
	compiler::Scene armed;
	armed.path = "res://armed_enemy.tscn";
	armed.base = "res://enemy.tscn";
	armed.nodes = { { ".", "Node3D", "res://test.pet" }, { "Gun", "MeshInstance3D", "" } };
	project.scenes = { base, armed };
	const char *source = "extends Node3D\n\nfunc f():\n\tvar gun: MeshInstance3D = $Gun\n";
	CHECK(messages(compile_in(source, project)).empty());
	// Used as a root or instanced, it must have the node.
	project.main = "res://enemy.tscn";
	CHECK(contains(messages(compile_in(source, project)), "res://enemy.tscn: the root has no node at \"Gun\"."));
	project.main.clear();
	compiler::Scene level;
	level.path = "res://level.tscn";
	level.nodes = { { ".", "Node3D", "" }, { "Enemy", "Node3D", "", true, "res://test.pet" } };
	level.nodes[1].instance = "res://enemy.tscn";
	project.scenes.push_back(level);
	CHECK(contains(messages(compile_in(source, project)), "res://enemy.tscn: the root has no node at \"Gun\"."));
}

TEST_CASE("a literal path from the root into an autoload names the autoload instead (spec §7.3)") {
	FakeProject project = hud_scene();
	project.autoloads["Music"] = "res://music.pet";
	CHECK(contains(hud_errors("var n: Node = $\"/root/Music\"", project), "$\"/root/Music\" is the autoload Music; write Music."));
	CHECK(contains(hud_errors("var n: Node = $/root/Music/Player", project), "$\"/root/Music/Player\" is in the autoload Music; write Music.get_node(\"Player\")."));
	CHECK(contains(hud_errors("var n := get_node_or_null(\"/root/Music\")", project), "get_node_or_null(\"/root/Music\") is the autoload Music; write Music."));
	CHECK(hud_errors("var n := get_node(\"/root/Main\")", project).empty());
	// Another node under the root is a Node, as get_node() gives it.
	CHECK(hud_errors("var n: Node = $/root/Main", project).empty());
	CHECK(contains(hud_errors("var n: Label = $\"/root/Main\"", project), "Cannot assign a value of type Node as Label"));
	// A relative path that misses names the autoload it may mean.
	CHECK(contains(hud_errors("var n = $\"../Music/Player\"", project), "res://main.tscn: HUD has no node at \"../Music/Player\". Music is an autoload; write Music.get_node(\"Player\")."));
	CHECK(contains(hud_errors("var n = $\"../../Music\"", project), "leads out of the scene, and no scene instances it. Music is an autoload; write Music."));
}

// Spec §6.3.
TEST_CASE("@onready members are set when the node is ready, from the scene") {
	const char *source = "extends CanvasLayer\n@onready var health = $Health\nvar shown := \"\"\n\nfunc _ready():\n\tshown = health.text\n";
	const compiler::CompileResult result = compile_in(source, hud_scene());
	REQUIRE_MESSAGE(result.ok(), messages(result));
	CHECK(result.module.members[0].is_onready);
	CHECK(result.module.types[result.module.members[0].type].object.name == "Label");
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
	CHECK(storage(later.module, later.module.members[0].type) == bytecode::ValueType::String);
	CHECK(contains(errors("@onready var a := 1\nvar b := a"), "\"a\" is set when the node is ready, after this initializer runs"));
	CHECK(contains(errors("@onready var b := a\n@onready var a := 1"), "\"a\" is not initialized yet"));
	CHECK(contains(errors("@onready @export pub var a := 1"), "@onready and @export do not go together"));
	CHECK(contains(errors("@onready var a := 1\n\nfunc _init():\n\tprint(a)"), "after _init() runs"));
	CHECK(contains(errors("@onready var a := 1\n\nfunc _enter_tree():\n\tprint(a)"), "after _enter_tree() runs"));
	CHECK(contains(messages(compile("extends Resource\n@onready var a := 1\n")), "@onready is for Node scripts"));
}

TEST_CASE("while loading, only declarations are compiled") {
	// The body has a type error, and the scene's method does not exist; neither
	// is checked without bodies and scenes.
	const char *source = "extends Node3D\n@export pub var speed := 2.0\n\npub func f(x: int) -> String:\n\tvar s: String = x\n\treturn s\n";
	const compiler::CompileResult result = compile_in(source, timer_scene("_on_missing"), true);
	REQUIRE_MESSAGE(result.ok(), messages(result));
	CHECK(result.module.declarations_only);
	const bytecode::Function &f = function_named(result.module, "f");
	CHECK(f.params.size() == 1);
	CHECK(storage(result.module, f.result.type) == bytecode::ValueType::String);
	CHECK(result.module.members[0].initial.f == 2.0);
	CHECK(!compile_in(source, timer_scene("_on_missing")).ok());
	// Nor are node paths, which need the scenes; a member typed by one is a
	// Variant until the full compile.
	CHECK(compile_in("extends Node3D\n\nfunc f():\n\t$Missing.text = \"\"\n", FakeProject(), true).ok());
	const compiler::CompileResult loading = compile_in("extends CanvasLayer\n@onready var health = $Health\n", hud_scene(), true);
	REQUIRE_MESSAGE(loading.ok(), messages(loading));
	CHECK(storage(loading.module, loading.module.members[0].type) == bytecode::ValueType::Variant);
}

TEST_CASE("an error in how a scene uses the script is marked on the script's node there") {
	const compiler::CompileResult missing = compile_in("extends CanvasLayer\n\nfunc f():\n\tvar health = $Missing\n", hud_scene());
	REQUIRE(missing.errors.size() == 1);
	CHECK(missing.errors[0].scene == "res://main.tscn");
	CHECK(missing.errors[0].node == "HUD");
	// The scene's check reports the scene's own errors (architecture §6.4),
	// which leave the script compiling.
	const compiler::CompileResult connection = check_scenes_in("extends Node3D\n", timer_scene("_on_missing"));
	REQUIRE(connection.errors.size() == 1);
	CHECK(connection.errors[0].scene == "res://enemy.tscn");
	CHECK(connection.errors[0].node == ".");
	CHECK(compile_in("extends Node3D\n", timer_scene("_on_missing")).ok());
	// An error of the script alone is marked on no scene.
	const compiler::CompileResult own = compile_in("extends CanvasLayer\n\nfunc f():\n\tvar health: int = \"full\"\n", hud_scene());
	REQUIRE(own.errors.size() == 1);
	CHECK(own.errors[0].scene.empty());
}
