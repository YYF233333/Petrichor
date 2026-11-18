#include "compiler/rename.h"
#include "doctest.h"
#include "helpers.h"

using namespace petrichor;
using namespace petrichor::tests;

// Renaming or moving nodes in the scene editor (spec §13.3): the literal node
// paths of scripts to or from the nodes moved are written anew, as the editor
// writes NodePath properties (editor/docks/scene_tree_dock.cpp:2077-2120;
// ed1daf0), or listed when they cannot be.

namespace {

// What moving nodes of `p_scene` changes, with the project holding the scene
// after the move.
std::string moved(const FakeProject &p_project, const std::string &p_scene, const std::map<std::string, std::string> &p_moves) {
	compiler::Workspace workspace(p_project);
	compiler::Analysis analysis(fake_engine(), workspace);
	const compiler::MovedNodes moves = compiler::move_nodes(analysis, p_scene, p_moves);
	std::vector<std::string> lines;
	for (const compiler::RenameEdit &edit : moves.edits) {
		lines.push_back(edit.occurrence.file + ":" + std::to_string(edit.occurrence.line) + " " + edit.edit.old_text + " -> " + edit.edit.new_text);
	}
	for (const compiler::Diagnostic &report : moves.reports) {
		lines.push_back(report.file + ":" + std::to_string(report.line) + ": " + report.message);
	}
	return joined(lines);
}

compiler::SceneNode node(const std::string &p_path, const std::string &p_type, const std::string &p_script = std::string()) {
	return { p_path, p_type, p_script };
}

compiler::SceneNode instance(const std::string &p_path, const std::string &p_scene) {
	compiler::SceneNode made{ p_path, "Node3D", "", true };
	made.instance = p_scene;
	return made;
}

} // namespace

TEST_CASE("a node renamed is renamed in $X and get_node()") {
	FakeProject project;
	project.add_script("res://player.pet", "extends Node2D\n\n@onready var sprite: Node2D = $Sprite\n@onready var arm: Node2D = get_node('Sprite/Arm')\n@onready var shown: Node2D = ($Sprite)\n");
	project.scenes = { { "res://player.tscn", { node(".", "Node2D", "res://player.pet"), node("Body", "Node2D"), node("Body/Arm", "Node2D") } } };
	CHECK(moved(project, "res://player.tscn", { { "Sprite", "Body" }, { "Sprite/Arm", "Body/Arm" } }) == "res://player.pet:3 $Sprite -> $Body\nres://player.pet:4 'Sprite/Arm' -> 'Body/Arm'\nres://player.pet:5 $Sprite -> $Body\n");
}

TEST_CASE("a node moved is reached by its new path, also with \"..\", from a node moved too") {
	FakeProject project;
	project.add_script("res://level.pet", "extends Node2D\n\n@onready var b: Node2D = $A/B\n");
	project.add_script("res://a.pet", "extends Node2D\n\n@onready var d: Node2D = $\"../D\"\n@onready var b: Node2D = $B\n");
	project.scenes = { { "res://level.tscn", { node(".", "Node2D", "res://level.pet"), node("C", "Node2D"), node("C/A", "Node2D", "res://a.pet"), node("C/A/B", "Node2D"), node("D", "Node2D") } } };
	CHECK(moved(project, "res://level.tscn", { { "A", "C/A" }, { "A/B", "C/A/B" } }) == "res://a.pet:3 $\"../D\" -> $\"../../D\"\nres://level.pet:3 $A/B -> $C/A/B\n");
}

TEST_CASE("%Name stays while its node keeps its name, and takes the new one") {
	FakeProject project;
	project.add_script("res://ui.pet", "extends Control\n\n@onready var hp: Control = %HealthBar\n@onready var icon: TextureRect = %HealthBar/Icon\n@onready var bar: Control = $Box/%HealthBar\n");
	compiler::Scene ui{ "res://ui.tscn", { node(".", "Control", "res://ui.pet"), node("Box", "Control"), node("Panel", "Control"), node("Panel/HealthBar", "Control"), node("Panel/HealthBar/Icon", "TextureRect") } };
	ui.nodes[3].unique = true;
	project.scenes = { ui };
	CHECK(moved(project, "res://ui.tscn", { { "Box/HealthBar", "Panel/HealthBar" }, { "Box/HealthBar/Icon", "Panel/HealthBar/Icon" } }).empty());
	project.scenes[0].nodes[3].path = "Box/HpBar";
	project.scenes[0].nodes[4].path = "Box/HpBar/Icon";
	CHECK(moved(project, "res://ui.tscn", { { "Box/HealthBar", "Box/HpBar" }, { "Box/HealthBar/Icon", "Box/HpBar/Icon" } }) == "res://ui.pet:3 %HealthBar -> %HpBar\nres://ui.pet:4 %HealthBar/Icon -> %HpBar/Icon\nres://ui.pet:5 $Box/%HealthBar -> $Box/%HpBar\n");
	// A path that the move breaks, though it is written as before.
	project.sources["res://ui.pet"] = "extends Control\n\n@onready var bar: Control = $Box/%HpBar\n";
	for (const int i : { 1, 3, 4 }) {
		project.scenes[0].nodes[i].path.replace(0, 3, "Frame");
	}
	CHECK(moved(project, "res://ui.tscn", { { "Box", "Frame" } }) == "res://ui.pet:3: $Box/%HpBar now leads to no node in res://ui.tscn.\n");
}

TEST_CASE("a path into an instanced scene follows a node moved inside it, at any depth") {
	FakeProject project;
	project.add_script("res://level.pet", "extends Node3D\n\n@onready var body: Node3D = $Player/Body\n@onready var muzzle: Node3D = $Player/Gun/Muzzle\n");
	project.scenes = {
		{ "res://level.tscn", { node(".", "Node3D", "res://level.pet"), instance("Player", "res://player.tscn") } },
		{ "res://player.tscn", { node(".", "Node3D"), node("Mesh", "Node3D"), node("Hand", "Node3D"), instance("Hand/Gun", "res://gun.tscn") } },
		{ "res://gun.tscn", { node(".", "Node3D"), node("Muzzle", "Node3D") } },
	};
	CHECK(moved(project, "res://player.tscn", { { "Body", "Mesh" }, { "Gun", "Hand/Gun" } }) == "res://level.pet:3 $Player/Body -> $Player/Mesh\nres://level.pet:4 $Player/Gun/Muzzle -> $Player/Hand/Gun/Muzzle\n");
}

TEST_CASE("a scene that inherits the one edited has its nodes moved too") {
	FakeProject project;
	project.add_script("res://slime.pet", "extends Node3D\n\n@onready var hitbox: Area3D = $Hitbox\n");
	compiler::Scene slime{ "res://slime.tscn", { node(".", "Node3D", "res://slime.pet"), node("Hitbox", "Area3D") } };
	slime.base = "res://enemy.tscn";
	project.scenes = { { "res://enemy.tscn", { node(".", "Node3D"), node("HurtBox", "Area3D") } }, slime };
	CHECK(moved(project, "res://enemy.tscn", { { "Hitbox", "HurtBox" } }) == "res://slime.pet:3 $Hitbox -> $HurtBox\n");
}

TEST_CASE("a path that would lead elsewhere in another scene with the script is listed, not written anew") {
	FakeProject project;
	project.add_script("res://enemy.pet", "extends Node3D\n\n@onready var hitbox: Area3D = $Hitbox\n");
	project.scenes = {
		{ "res://enemy_a.tscn", { node(".", "Node3D", "res://enemy.pet"), node("HurtBox", "Area3D") } },
		{ "res://enemy_b.tscn", { node(".", "Node3D", "res://enemy.pet"), node("Hitbox", "Area3D") } },
	};
	CHECK(moved(project, "res://enemy_a.tscn", { { "Hitbox", "HurtBox" } }) == "res://enemy.pet:3: $Hitbox is not updated: it would be $Hitbox in res://enemy_b.tscn and $HurtBox in res://enemy_a.tscn.\n");
	// A scene without the node, where the new path finds another.
	project.sources["res://enemy.pet"] = "extends Node3D\n\n@onready var hitbox := get_node_or_null(\"Hitbox\")\n";
	project.scenes[1].nodes[1].path = "HurtBox";
	CHECK(moved(project, "res://enemy_a.tscn", { { "Hitbox", "HurtBox" } }) == "res://enemy.pet:3: \"Hitbox\" is not updated: \"HurtBox\" would lead to another node in res://enemy_b.tscn.\n");
}

TEST_CASE("a get_node() of a path built at run time is listed when its Strings name a node moved") {
	FakeProject project;
	project.add_script("res://spawner.pet", "extends Node\n\nfunc kill(n: String) -> Node:\n\treturn get_node(\"Enemies/\" + n)\n\nfunc boss() -> Node:\n\treturn get_node(\"Enemies/Boss\")\n");
	project.scenes = { { "res://spawner.tscn", { node(".", "Node", "res://spawner.pet"), node("Foes", "Node"), node("Foes/Boss", "Node") } } };
	CHECK(moved(project, "res://spawner.tscn", { { "Enemies", "Foes" }, { "Enemies/Boss", "Foes/Boss" } }) == "res://spawner.pet:7 \"Enemies/Boss\" -> \"Foes/Boss\"\nres://spawner.pet:4: This get_node() is not updated, though \"Enemies/\" names a node renamed or moved.\n");
}
