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

bool contains(const std::string &p_text, const std::string &p_part) {
	return p_text.find(p_part) != std::string::npos;
}

const bytecode::Function &function(const compiler::CompileResult &p_result, const char *p_name) {
	const int index = p_result.module.find_function(p_name);
	REQUIRE(index >= 0);
	return p_result.module.functions[index];
}

} // namespace

TEST_CASE("a method connected in a scene is visible to the engine (spec §10.5)") {
	const char *source = "extends Node3D\n\nfunc _on_timer_timeout():\n\tpass\n\nfunc other():\n\tpass\n";
	const compiler::CompileResult result = compile_in(source, timer_scene("_on_timer_timeout"));
	REQUIRE_MESSAGE(result.ok(), messages(result));
	CHECK(function(result, "_on_timer_timeout").is_exposed);
	CHECK(!function(result, "other").is_exposed);
}

TEST_CASE("scene connections are checked against the class (spec §7.7)") {
	const char *source = "extends Node3D\n\nfunc _on_timer_timeout():\n\tpass\n";
	CHECK(messages(compile_in(source, timer_scene("_on_missing"))).find("res://enemy.tscn: the signal \"timeout\" of Timer is connected to _on_missing(), which this class does not declare.") != std::string::npos);
	CHECK(messages(compile_in(source, timer_scene("_on_timer_timeout", 1))).find("which passes 1 argument(s); _on_timer_timeout() takes 0.") != std::string::npos);
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
	CHECK(messages(compile_in("extends Node\n\nfunc _on_child(node: int):\n\tpass\n", project)).find("which passes Node here") != std::string::npos);
}

TEST_CASE("$X has the class of the node at that path in the scenes that use the script (spec §7.1)") {
	// scripts/hud.gd of the FPS starter kit, with the parameter annotated.
	const char *hud = "extends CanvasLayer\n\nfunc _on_health_updated(health: int):\n\t$Health.text = str(health) + \"%\"\n";
	const compiler::CompileResult result = compile_in(hud, hud_scene());
	REQUIRE_MESSAGE(result.ok(), messages(result));
	// Checked again at run time, in case the tree has changed since.
	CHECK(result.module.classes == std::vector<std::string>{ "Label" });

	CHECK(hud_errors("$\"Health\".text = \"\"").empty());
	CHECK(contains(hud_errors("$Crosshair.text = \"\""), "TextureRect has no property \"text\""));
	CHECK(contains(hud_errors("var n: Node3D = $Health"), "Cannot assign a value of type Label as Node3D"));
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

TEST_CASE("while loading, only declarations are compiled") {
	// The body has a type error, and the scene's method does not exist; neither
	// is checked without bodies and scenes.
	const char *source = "extends Node3D\n@export var speed := 2.0\n\npub func f(x: int) -> String:\n\tvar s: String = x\n\treturn s\n";
	const compiler::CompileResult result = compile_in(source, timer_scene("_on_missing"), true);
	REQUIRE_MESSAGE(result.ok(), messages(result));
	CHECK(result.module.declarations_only);
	const bytecode::Function &f = function(result, "f");
	CHECK(f.params.size() == 1);
	CHECK(f.return_type == bytecode::ValueType::String);
	CHECK(result.module.members[0].initial.f == 2.0);
	CHECK(!compile_in(source, timer_scene("_on_missing")).ok());
	// Nor are node paths, which need the scenes.
	CHECK(compile_in("extends Node3D\n\nfunc f():\n\t$Missing.text = \"\"\n", FakeProject(), true).ok());
}
