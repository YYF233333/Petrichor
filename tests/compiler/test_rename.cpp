#include "compiler/rename.h"
#include "doctest.h"
#include "helpers.h"

using namespace petrichor;
using namespace petrichor::tests;

// Renaming (spec §13.3): every place that finding references lists changes,
// in scripts and in the text of scenes and resource files, where the engine
// stores the name (architecture §6.4); or nothing does, and the places that
// keep it from it are listed. The scene texts are trimmed from real files.

namespace {

compiler::SceneValue value(const std::string &p_type, std::vector<compiler::SceneValue> p_items = {}) {
	compiler::SceneValue made;
	made.type = p_type;
	made.items = std::move(p_items);
	return made;
}

compiler::SceneValue text(const std::string &p_text) {
	compiler::SceneValue made = value("String");
	made.text = p_text;
	return made;
}

compiler::SceneValue number(int64_t p_value) {
	compiler::SceneValue made = value("int");
	made.integer = p_value;
	return made;
}

// A project whose scenes and resource files have their texts too.
struct Project : FakeProject {
	std::map<std::string, std::string> texts;
};

compiler::Rename renamed(const Project &p_project, const std::string &p_path, const std::string &p_name, const std::string &p_new, int p_nth = 0) {
	const std::string &source = p_project.sources.at(p_path);
	size_t offset = source.find(p_name);
	for (int i = 0; i < p_nth; i++) {
		offset = source.find(p_name, offset + 1);
	}
	REQUIRE(offset != std::string::npos);
	compiler::Workspace workspace(p_project);
	compiler::Analysis analysis(fake_engine(), workspace);
	std::vector<std::string> others;
	for (const auto &[path, text] : p_project.sources) {
		if (!compiler::is_script_path(path)) {
			others.push_back(path);
		}
	}
	return compiler::rename(analysis, p_path, uint32_t(offset), p_new, others, [&](const std::string &p_file) {
		const auto found = p_project.texts.find(p_file);
		return found == p_project.texts.end() ? std::nullopt : std::optional<std::string>(found->second);
	});
}

// The file after the edits that start checked, each checked against the
// text it replaces.
std::string after(const Project &p_project, const compiler::Rename &p_rename, const std::string &p_file) {
	const auto text = p_project.texts.find(p_file);
	std::string result = text != p_project.texts.end() ? text->second : p_project.sources.at(p_file);
	for (auto edit = p_rename.edits.rbegin(); edit != p_rename.edits.rend(); ++edit) {
		if (edit->occurrence.file == p_file && !edit->occurrence.candidate) {
			REQUIRE(result.substr(edit->edit.begin, edit->edit.end - edit->edit.begin) == edit->edit.old_text);
			result.replace(edit->edit.begin, edit->edit.end - edit->edit.begin, edit->edit.new_text);
		}
	}
	return result;
}

// Each edit as "file:line", a candidate marked.
std::string places(const compiler::Rename &p_rename) {
	std::vector<std::string> shown;
	for (const compiler::RenameEdit &edit : p_rename.edits) {
		shown.push_back(edit.occurrence.file + ":" + std::to_string(edit.occurrence.line) + (edit.occurrence.candidate ? " candidate" : ""));
	}
	return joined(shown);
}

std::string refusals(const compiler::Rename &p_rename) {
	std::vector<std::string> shown;
	for (const compiler::Diagnostic &refusal : p_rename.refusals) {
		shown.push_back(refusal.file + ":" + std::to_string(refusal.line) + ": " + refusal.message);
	}
	return joined(shown);
}

// From godot-demo-projects/networking/multiplayer_bomber/player.tscn: the
// resource name and the library key "stunned" name the animation, not the
// member.
Project player() {
	Project project;
	project.add_script("res://player.pet",
			"extends Node2D\n\n"
			"@export pub var stunned := false\n"
			"pub var synced_position := Vector2.ZERO\n"
			"pub var recoil := Vector2.ZERO\n\n\n"
			"func stun():\n"
			"\tstunned = true\n"
			"\tset_deferred(\"stunned\", false)\n");
	project.texts["res://player.tscn"] = R"([gd_scene format=3 uid="uid://df8f82vhvgtfm"]

[ext_resource type="Script" uid="uid://ddalk1r2gq7u5" path="res://player.pet" id="1"]

[sub_resource type="Animation" id="3"]
resource_name = "stunned"
length = 1.2
tracks/0/type = "value"
tracks/0/imported = false
tracks/0/enabled = true
tracks/0/path = NodePath("sprite:frame")
tracks/0/interp = 1
tracks/0/loop_wrap = true
tracks/0/keys = {
"times": PackedFloat32Array(0),
"transitions": PackedFloat32Array(1),
"update": 1,
"values": [0]
}
tracks/1/type = "value"
tracks/1/imported = false
tracks/1/enabled = true
tracks/1/path = NodePath(".:stunned")
tracks/1/interp = 1
tracks/1/loop_wrap = true
tracks/1/keys = {
"times": PackedFloat32Array(1),
"transitions": PackedFloat32Array(1),
"update": 1,
"values": [false]
}
tracks/2/type = "bezier"
tracks/2/imported = false
tracks/2/enabled = true
tracks/2/path = NodePath(".:recoil:y")
tracks/2/interp = 1
tracks/2/loop_wrap = true
tracks/2/keys = {
"handle_modes": PackedInt32Array(0),
"points": PackedFloat32Array(0, -0.25, 0, 0.25, 0),
"times": PackedFloat32Array(0)
}

[sub_resource type="AnimationLibrary" id="AnimationLibrary_yb83i"]
_data = {
&"stunned": SubResource("3")
}

[sub_resource type="SceneReplicationConfig" id="SceneReplicationConfig_sh64w"]
properties/0/path = NodePath(".:synced_position")
properties/0/spawn = true
properties/0/replication_mode = 1
properties/1/path = NodePath("label:text")
properties/1/spawn = true
properties/1/replication_mode = 0

[node name="player" type="CharacterBody2D" unique_id=1262329161]
z_index = 10
motion_mode = 1
script = ExtResource("1")
stunned = true

[node name="anim" type="AnimationPlayer" parent="." unique_id=365298786]
libraries/ = SubResource("AnimationLibrary_yb83i")

[node name="label" type="Label" parent="." unique_id=1237995107]
text = "Player 1"

[node name="MultiplayerSynchronizer" type="MultiplayerSynchronizer" parent="." unique_id=1095522834]
replication_config = SubResource("SceneReplicationConfig_sh64w")
)";
	compiler::Scene scene;
	scene.path = "res://player.tscn";
	scene.nodes = {
		{ ".", "Node2D", "res://player.pet", false, "", false, { { "stunned", value("bool") } } },
		{ "anim", "AnimationPlayer" },
		{ "label", "Label" },
		{ "MultiplayerSynchronizer", "Node" },
	};
	scene.named_members = {
		{ "anim", "stunned", "../.", "stunned", false, { { "res://player.tscn::3", 1 } } },
		{ "anim", "stunned", "../.", "recoil", false, { { "res://player.tscn::3", 2 } } },
		{ "MultiplayerSynchronizer", "", "../.", "synced_position", false, { { "res://player.tscn::SceneReplicationConfig_sh64w", 0 } } },
	};
	project.scenes = { scene };
	return project;
}

} // namespace

TEST_CASE("an export is renamed where scripts use it and where a scene stores and animates it") {
	const Project project = player();
	const compiler::Rename rename = renamed(project, "res://player.pet", "stunned", "dazed");
	REQUIRE_MESSAGE(rename.refusals.empty(), refusals(rename));
	CHECK(after(project, rename, "res://player.pet") == "extends Node2D\n\n@export pub var dazed := false\npub var synced_position := Vector2.ZERO\npub var recoil := Vector2.ZERO\n\n\nfunc stun():\n\tdazed = true\n\tset_deferred(\"dazed\", false)\n");
	std::string scene = project.texts.at("res://player.tscn");
	scene.replace(scene.find("NodePath(\".:stunned\")"), 21, "NodePath(\".:dazed\")");
	scene.replace(scene.find("stunned = true"), 7, "dazed");
	CHECK(after(project, rename, "res://player.tscn") == scene);
}

TEST_CASE("a bezier track and a synchronized property name the member in the first subname of their path") {
	const Project project = player();
	const compiler::Rename recoil = renamed(project, "res://player.pet", "recoil", "kick");
	REQUIRE_MESSAGE(recoil.refusals.empty(), refusals(recoil));
	CHECK(contains(after(project, recoil, "res://player.tscn"), "tracks/2/path = NodePath(\".:kick:y\")\n"));
	const compiler::Rename synced = renamed(project, "res://player.pet", "synced_position", "position_synced");
	REQUIRE_MESSAGE(synced.refusals.empty(), refusals(synced));
	CHECK(contains(after(project, synced, "res://player.tscn"), "properties/0/path = NodePath(\".:position_synced\")\nproperties/0/spawn"));
}

TEST_CASE("a method track's key names the method, a key apart from the others") {
	// From FRU-Sim/scenes/p5/exawave/light_wave.tscn.
	Project project;
	project.add_script("res://wave.pet", "extends Node3D\n\n\npub func snapshot_hit():\n\tpass\n\n\npub func move_wave():\n\tpass\n");
	project.texts["res://light_wave.tscn"] = R"([gd_scene load_steps=6 format=3 uid="uid://bc70djyqj0lrc"]

[ext_resource type="Script" path="res://wave.pet" id="1_fbixp"]

[sub_resource type="Animation" id="Animation_ng2vc"]
resource_local_to_scene = true
resource_name = "wave_hit"
length = 1.6
step = 0.05
tracks/0/type = "method"
tracks/0/imported = false
tracks/0/enabled = true
tracks/0/path = NodePath(".")
tracks/0/interp = 1
tracks/0/loop_wrap = true
tracks/0/keys = {
"times": PackedFloat32Array(0, 1.6),
"transitions": PackedFloat32Array(1, 1),
"values": [{
"args": [],
"method": &"snapshot_hit"
}, {
"args": [],
"method": &"move_wave"
}]
}

[sub_resource type="AnimationLibrary" id="AnimationLibrary_x"]
_data = {
&"wave_hit": SubResource("Animation_ng2vc")
}

[node name="LightWave" type="Node3D"]
script = ExtResource("1_fbixp")

[node name="WaveAnim" type="AnimationPlayer" parent="."]
libraries = {
&"": SubResource("AnimationLibrary_x")
}
)";
	compiler::Scene scene;
	scene.path = "res://light_wave.tscn";
	scene.nodes = { { ".", "Node3D", "res://wave.pet" }, { "WaveAnim", "AnimationPlayer" } };
	scene.named_members = {
		{ "WaveAnim", "wave_hit", "../.", "snapshot_hit", true, { { "res://light_wave.tscn::Animation_ng2vc", 0, 0 } } },
		{ "WaveAnim", "wave_hit", "../.", "move_wave", true, { { "res://light_wave.tscn::Animation_ng2vc", 0, 1 } } },
	};
	project.scenes = { scene };
	const compiler::Rename rename = renamed(project, "res://wave.pet", "move_wave", "advance_wave");
	REQUIRE_MESSAGE(rename.refusals.empty(), refusals(rename));
	std::string expected = project.texts.at("res://light_wave.tscn");
	expected.replace(expected.find("&\"move_wave\""), 12, "&\"advance_wave\"");
	CHECK(after(project, rename, "res://light_wave.tscn") == expected);
}

TEST_CASE("an animation library in a resource file of its own is changed there, once for the scenes that share it") {
	// From sourceofmana/presets/animations/overrides/Ponytail.tres.
	Project project;
	project.add_script("res://hair.pet", "extends Node2D\n\npub var sway := 0.0\n");
	project.texts["res://ponytail.tres"] = R"([gd_resource type="AnimationLibrary" format=3 uid="uid://db3rdpuvcwng"]

[sub_resource type="Animation" id="Animation_attackdown"]
resource_name = "AttackDown"
length = 0.4
loop_mode = 1
tracks/0/type = "value"
tracks/0/imported = false
tracks/0/enabled = true
tracks/0/path = NodePath("Hair:sway")
tracks/0/interp = 1
tracks/0/loop_wrap = true
tracks/0/keys = {
"times": PackedFloat32Array(0, 0.133, 0.267),
"transitions": PackedFloat32Array(1, 1, 1),
"update": 1,
"values": [0, 1, 2]
}

[resource]
_data = {
&"AttackDown": SubResource("Animation_attackdown")
}
)";
	for (const char *path : { "res://player.tscn", "res://npc.tscn" }) {
		compiler::Scene scene;
		scene.path = path;
		scene.nodes = { { ".", "Node2D" }, { "Hair", "Node2D", "res://hair.pet" }, { "AnimationPlayer", "AnimationPlayer" } };
		scene.named_members = { { "AnimationPlayer", "ponytail/AttackDown", "../Hair", "sway", false, { { "res://ponytail.tres::Animation_attackdown", 0 } } } };
		project.scenes.push_back(scene);
	}
	const compiler::Rename rename = renamed(project, "res://hair.pet", "sway", "swing");
	REQUIRE_MESSAGE(rename.refusals.empty(), refusals(rename));
	CHECK(places(rename) == joined({ "res://hair.pet:3", "res://ponytail.tres:10" }));
	CHECK(contains(after(project, rename, "res://ponytail.tres"), "NodePath(\"Hair:swing\")"));
}

namespace {

// From tests/engine/project/language/export_base.tscn and
// export_derived.tscn, which inherits it, and a struct inside another.
Project exports() {
	Project project;
	project.add_script("res://export_check.pet",
			"class_name ExportCheck\nextends Node\n\n"
			"pub struct Hit:\n\tvar damage: int\n\tvar crit: bool = false\n\n"
			"pub struct Shot:\n\tvar count: int\n\tvar hit: Hit\n\n"
			"pub enum Order { WAIT, MOVE(target: Vector3) }\n\n"
			"@export pub var hit := Hit(damage = 1)\n"
			"@export pub var shot: Shot\n"
			"@export pub var order := Order.WAIT\n\n\n"
			"pub func described() -> String:\n"
			"\tmatch order:\n"
			"\t\tOrder.MOVE(var target):\n"
			"\t\t\treturn str(target)\n"
			"\treturn str(hit.damage)\n",
			"ExportCheck");
	project.texts["res://export_base.tscn"] = R"([gd_scene format=3]

[ext_resource type="Script" path="res://export_check.pet" id="1"]

[node name="Exported" type="Node"]
script = ExtResource("1")
hit = {"crit": true, "damage": 5}
shot = {
"count": 2,
"hit": {
"damage": 3
}
}
order = {"$variant": "MOVE", "target": Vector3(1, 2, 3)}
)";
	project.texts["res://export_derived.tscn"] = R"([gd_scene format=3]

[ext_resource type="PackedScene" path="res://export_base.tscn" id="1"]

[node name="Exported" instance=ExtResource("1")]
hit = {"damage": 7}
)";
	compiler::SceneValue vector = value("Vector3");
	compiler::Scene base;
	base.path = "res://export_base.tscn";
	const compiler::SceneValue shot = value("Dictionary", { text("count"), number(2), text("hit"), value("Dictionary", { text("damage"), number(3) }) });
	const compiler::SceneValue order = value("Dictionary", { text("$variant"), text("MOVE"), text("target"), vector });
	base.nodes = { { ".", "Node", "res://export_check.pet", false, "", false, { { "hit", value("Dictionary", { text("crit"), value("bool"), text("damage"), number(5) }) }, { "shot", shot }, { "order", order } } } };
	compiler::Scene derived;
	derived.path = "res://export_derived.tscn";
	derived.base = base.path;
	derived.nodes = { { ".", "Node", "res://export_check.pet", false, "", false, { { "hit", value("Dictionary", { text("damage"), number(7) }) } } } };
	project.scenes = { base, derived };
	return project;
}

} // namespace

TEST_CASE("a struct's field is renamed in the values that scenes store, nested ones too, in an inherited scene and its base") {
	const Project project = exports();
	const compiler::Rename rename = renamed(project, "res://export_check.pet", "damage", "amount");
	REQUIRE_MESSAGE(rename.refusals.empty(), refusals(rename));
	std::string base = project.texts.at("res://export_base.tscn");
	for (size_t at = base.find("\"damage\""); at != std::string::npos; at = base.find("\"damage\"")) {
		base.replace(at, 8, "\"amount\"");
	}
	CHECK(after(project, rename, "res://export_base.tscn") == base);
	CHECK(contains(after(project, rename, "res://export_derived.tscn"), "hit = {\"amount\": 7}\n"));
	CHECK(contains(after(project, rename, "res://export_check.pet"), "\tvar amount: int\n"));
}

TEST_CASE("renaming a field or a local that a pattern names in short writes the pattern out") {
	const Project project = exports();
	compiler::Rename rename = renamed(project, "res://export_check.pet", "target", "goal");
	REQUIRE_MESSAGE(rename.refusals.empty(), refusals(rename));
	CHECK(contains(after(project, rename, "res://export_check.pet"), "\t\tOrder.MOVE(goal = var target):\n\t\t\treturn str(target)\n"));
	rename = renamed(project, "res://export_check.pet", "target", "aim", 2);
	REQUIRE_MESSAGE(rename.refusals.empty(), refusals(rename));
	CHECK(contains(after(project, rename, "res://export_check.pet"), "\t\tOrder.MOVE(target = var aim):\n\t\t\treturn str(aim)\n"));
}

TEST_CASE("an enum's variant is renamed where a stored value names it") {
	const Project project = exports();
	const compiler::Rename rename = renamed(project, "res://export_check.pet", "MOVE", "GO");
	REQUIRE_MESSAGE(rename.refusals.empty(), refusals(rename));
	CHECK(contains(after(project, rename, "res://export_base.tscn"), "order = {\"$variant\": \"GO\", \"target\": Vector3(1, 2, 3)}\n"));
}

namespace {

// From tests/engine/project/language/inherit_base.tscn and
// inherit_leaf.tscn, which inherits it and so has its connection too.
Project connections() {
	Project project;
	project.add_script("res://inherit_base.pet", "class_name InheritBase\nextends Node\n\npub signal tick()\n@export pub var target: Node3D\n\n\npub func _entered():\n\tpass\n", "InheritBase");
	project.add_script("res://inherit_leaf.pet", "extends InheritBase\n\n\npub func _leaf_tick():\n\ttick.emit()\n");
	project.texts["res://inherit_base.tscn"] = R"([gd_scene format=3]
[ext_resource type="Script" path="res://inherit_base.pet" id="1"]
[node name="Inherited" type="Node" node_paths=PackedStringArray("target")]
script = ExtResource("1")
target = NodePath("Target")
[node name="Target" type="Node3D" parent="."]
[connection signal="tree_entered" from="Target" to="." method="_entered"]
)";
	project.texts["res://inherit_leaf.tscn"] = R"([gd_scene format=3]
[ext_resource type="PackedScene" path="res://inherit_base.tscn" id="1"]
[ext_resource type="Script" path="res://inherit_leaf.pet" id="2"]
[node name="Inherited" instance=ExtResource("1")]
script = ExtResource("2")
[node name="Target" parent="."]
visible = false
[connection signal="tick" from="." to="." method="_leaf_tick"]
)";
	compiler::Scene base;
	base.path = "res://inherit_base.tscn";
	base.nodes = { { ".", "Node", "res://inherit_base.pet", false, "", false, { { "target", value("NodePath") } } }, { "Target", "Node3D" } };
	base.connections = { { "tree_entered", "Target", ".", "_entered", 0, 0, base.path } };
	compiler::Scene leaf = base;
	leaf.path = "res://inherit_leaf.tscn";
	leaf.base = base.path;
	leaf.nodes[0].script = "res://inherit_leaf.pet";
	leaf.nodes[0].properties.clear();
	leaf.connections.push_back({ "tick", ".", ".", "_leaf_tick", 0, 0, leaf.path });
	project.scenes = { base, leaf };
	return project;
}

} // namespace

TEST_CASE("a connection's method and signal are renamed in the scene that stores the connection") {
	const Project project = connections();
	const compiler::Rename method = renamed(project, "res://inherit_base.pet", "_entered", "_on_entered");
	REQUIRE_MESSAGE(method.refusals.empty(), refusals(method));
	CHECK(places(method) == joined({ "res://inherit_base.pet:8", "res://inherit_base.tscn:7" }));
	CHECK(contains(after(project, method, "res://inherit_base.tscn"), "[connection signal=\"tree_entered\" from=\"Target\" to=\".\" method=\"_on_entered\"]"));
	const compiler::Rename signal = renamed(project, "res://inherit_base.pet", "tick", "pulse");
	REQUIRE_MESSAGE(signal.refusals.empty(), refusals(signal));
	CHECK(contains(after(project, signal, "res://inherit_leaf.tscn"), "[connection signal=\"pulse\" from=\".\" to=\".\" method=\"_leaf_tick\"]"));
	CHECK(contains(after(project, signal, "res://inherit_leaf.pet"), "\tpulse.emit()\n"));
}

TEST_CASE("an export of a Node type is renamed in its key and where the node's header lists the key as a node path") {
	const Project project = connections();
	const compiler::Rename rename = renamed(project, "res://inherit_base.pet", "target", "goal");
	REQUIRE_MESSAGE(rename.refusals.empty(), refusals(rename));
	CHECK(contains(after(project, rename, "res://inherit_base.tscn"), "node_paths=PackedStringArray(\"goal\")]\nscript = ExtResource(\"1\")\ngoal = NodePath(\"Target\")\n"));
}

TEST_CASE("a class's global name is renamed in the headers of the resource files whose script it is, by which a preload is typed") {
	Project project;
	project.add_script("res://weapon.pet", "class_name Weapon\nextends Resource\n\n@export pub var damage := 1\n", "Weapon");
	project.add_script("res://shop.pet", "extends Node\n\nconst SWORD = preload(\"res://sword.tres\")\n");
	project.texts["res://sword.tres"] = R"([gd_resource type="Resource" script_class="Weapon" format=3 uid="uid://c4xvb1yyfmxfo"]

[ext_resource type="Script" path="res://weapon.pet" id="1_wpn"]

[resource]
script = ExtResource("1_wpn")
damage = 3
)";
	project.resources["res://sword.tres"] = { "Resource", "Weapon" };
	compiler::Scene sword;
	sword.path = "res://sword.tres";
	sword.resources = { { sword.path, "res://weapon.pet", { { "damage", number(3) } } } };
	project.scenes = { sword };
	const compiler::Rename rename = renamed(project, "res://weapon.pet", "Weapon", "Arm");
	REQUIRE_MESSAGE(rename.refusals.empty(), refusals(rename));
	CHECK(contains(after(project, rename, "res://sword.tres"), "[gd_resource type=\"Resource\" script_class=\"Arm\" format=3"));
}

TEST_CASE("a method is renamed with those that override it, and a scene's connection to an override") {
	Project project;
	project.add_script("res://base.pet", "class_name Base\nextends Node\n\n\npub func on_hit():\n\tpass\n", "Base");
	project.add_script("res://boss.pet", "extends Base\n\n\npub func on_hit():\n\tsuper()\n");
	project.texts["res://arena.tscn"] = R"([gd_scene format=3]

[ext_resource type="Script" path="res://boss.pet" id="1_boss"]

[node name="Arena" type="Node"]

[node name="Boss" type="Node" parent="."]
script = ExtResource("1_boss")

[node name="Clock" type="Timer" parent="."]

[connection signal="timeout" from="Clock" to="Boss" method="on_hit"]
)";
	compiler::Scene arena;
	arena.path = "res://arena.tscn";
	arena.nodes = { { ".", "Node" }, { "Boss", "Node", "res://boss.pet" }, { "Clock", "Timer" } };
	arena.connections = { { "timeout", "Clock", "Boss", "on_hit", 0, 0, arena.path } };
	project.scenes = { arena };
	const compiler::Rename rename = renamed(project, "res://base.pet", "on_hit", "on_damage");
	REQUIRE_MESSAGE(rename.refusals.empty(), refusals(rename));
	CHECK(places(rename) == joined({ "res://base.pet:5", "res://boss.pet:4", "res://arena.tscn:12" }));
}

TEST_CASE("a scene that changes a node of a scene it instances stores the key under that node") {
	Project project;
	project.add_script("res://enemy.pet", "extends Node\n\n@export pub var speed := 1.0\n");
	project.texts["res://level.tscn"] = R"([gd_scene format=3]

[ext_resource type="PackedScene" path="res://enemy.tscn" id="1_enemy"]

[node name="Level" type="Node"]

[node name="Enemy1" parent="." instance=ExtResource("1_enemy")]

[node name="Enemy2" parent="." instance=ExtResource("1_enemy")]
speed = 5.0
)";
	compiler::SceneValue fast = value("float");
	fast.integer = 5;
	fast.whole = true;
	compiler::Scene level;
	level.path = "res://level.tscn";
	level.nodes = {
		{ ".", "Node" },
		{ "Enemy1", "Node", "", true, "res://enemy.pet", false, {}, "res://enemy.tscn" },
		{ "Enemy2", "Node", "", true, "res://enemy.pet", false, { { "speed", fast } }, "res://enemy.tscn" },
	};
	project.scenes = { level };
	const compiler::Rename rename = renamed(project, "res://enemy.pet", "speed", "move_speed");
	REQUIRE_MESSAGE(rename.refusals.empty(), refusals(rename));
	CHECK(contains(after(project, rename, "res://level.tscn"), "instance=ExtResource(\"1_enemy\")]\nmove_speed = 5.0\n"));
}

TEST_CASE("a key is written in quotes when the new name needs them, as the engine writes it") {
	const Project project = player();
	const compiler::Rename rename = renamed(project, "res://player.pet", "stunned", "眩晕");
	REQUIRE_MESSAGE(rename.refusals.empty(), refusals(rename));
	const std::string scene = after(project, rename, "res://player.tscn");
	CHECK(contains(scene, "script = ExtResource(\"1\")\n\"眩晕\" = true\n"));
	CHECK(contains(scene, "NodePath(\".:眩晕\")"));
}

TEST_CASE("candidates by name come last and start unchecked") {
	Project project;
	project.add_script("res://unit.pet", "extends Node\n\n\npub func hit():\n\tpass\n");
	project.sources["res://legacy.gd"] = "extends Node\n\nfunc f(unit):\n\tunit.hit()\n";
	const compiler::Rename rename = renamed(project, "res://unit.pet", "hit", "strike");
	CHECK(places(rename) == joined({ "res://unit.pet:4", "res://legacy.gd:4 candidate" }));
}

TEST_CASE("renaming is refused when the new name is not a name, or the edits change what the scripts mean") {
	Project project;
	project.add_script("res://unit.pet",
			"extends Node2D\n\n"
			"pub var speed := 1.0\n"
			"pub var pace := 2.0\n\n\n"
			"func f():\n"
			"\tvar step := 3.0\n"
			"\tprint(step + speed)\n\n\n"
			"func _ready():\n"
			"\tpass\n");
	for (const char *name : { "func", "2x", "a b", "" }) {
		CHECK(refusals(renamed(project, "res://unit.pet", "speed", name)) == "res://unit.pet:0: \"" + std::string(name) + "\" is not a name.\n");
	}
	// Two members of one name.
	CHECK(contains(refusals(renamed(project, "res://unit.pet", "speed", "pace")), "res://unit.pet:4: After renaming: "));
	CHECK(contains(refusals(renamed(project, "res://unit.pet", "speed", "position")), "res://unit.pet:3: After renaming: \"position\" is already a property of Node2D"));
	// The local would take the member's place.
	CHECK(refusals(renamed(project, "res://unit.pet", "step", "speed")) == "res://unit.pet:9: After renaming, the name here refers to another declaration.\n");
	CHECK(refusals(renamed(project, "res://unit.pet", "_ready", "_start")) == "res://unit.pet:12: It overrides a method that the engine or another language calls by this name.\n");
}

TEST_CASE("renaming is refused when a binary or imported file stores the name, or a text file not where the references say") {
	Project project = player();
	project.scenes[0].path = "res://player.scn";
	project.scenes[0].named_members[0].places[0].resource = "res://model.glb::Animation_1";
	project.scenes[0].named_members[0].places.push_back({ "res://hit.anim", 0 });
	CHECK(refusals(renamed(project, "res://player.pet", "stunned", "dazed")) ==
			joined({
					"res://hit.anim:0: It is a binary file, whose names cannot be rewritten. Save it as .tres first.",
					"res://model.glb:0: It is imported, and its names cannot be rewritten.",
					"res://player.scn:0: It is a binary file, whose names cannot be rewritten. Save it as .tscn first.",
			}));
	Project moved = player();
	moved.scenes[0].named_members[0].places[0].index = 0;
	CHECK(refusals(renamed(moved, "res://player.pet", "stunned", "dazed")) == "res://player.tscn:11: tracks/0/path of the resource 3 names \"frame\", not \"stunned\".\n");
}

TEST_CASE("renaming is refused where a node or a resource with the class stores the new name already, whose value would then take effect") {
	// As a removed export leaves it, where the renamed one is not stored.
	Project project = player();
	std::string &scene = project.texts["res://player.tscn"];
	scene.replace(scene.find("stunned = true"), 7, "dazed");
	project.scenes[0].nodes[0].properties = { { "dazed", value("bool") } };
	CHECK(refusals(renamed(project, "res://player.pet", "stunned", "dazed")) == "res://player.tscn:61: \"dazed\" of the root is stored already, and its value would take effect after renaming.\n");
	// The engine drops it as it loads the resource, so only the text has it;
	// here the resource has a subclass.
	Project items;
	items.add_script("res://item.pet", "extends Resource\n\n@export pub var price := 1\n");
	items.add_script("res://elixir.pet", "extends \"res://item.pet\"\n");
	items.texts["res://potion.tres"] = "[gd_resource type=\"Resource\" format=3]\n\n[ext_resource type=\"Script\" path=\"res://elixir.pet\" id=\"1\"]\n\n[resource]\nscript = ExtResource(\"1\")\ncost = 5\n";
	compiler::Scene potion;
	potion.path = "res://potion.tres";
	potion.resources = { { potion.path, "res://elixir.pet" } };
	items.scenes = { potion };
	CHECK(refusals(renamed(items, "res://item.pet", "price", "cost")) == "res://potion.tres:7: \"cost\" of the resource is stored already, and its value would take effect after renaming.\n");
	Project fields = exports();
	std::string &derived = fields.texts["res://export_derived.tscn"];
	derived.replace(derived.find("{\"damage\": 7}"), 13, "{\"amount\": 1, \"damage\": 7}");
	fields.scenes[1].nodes[0].properties[0].second = value("Dictionary", { text("amount"), number(1), text("damage"), number(7) });
	CHECK(contains(refusals(renamed(fields, "res://export_check.pet", "damage", "amount")), "res://export_derived.tscn:6: "));
}

TEST_CASE("renaming is refused where another scene uses a place it rewrites for another declaration") {
	// An animation stored apart, which a scene with a script of GDScript
	// animates too.
	Project project;
	project.add_script("res://player.pet", "extends Node2D\n\npub var flash := 0.0\n");
	project.sources["res://enemy.gd"] = "extends Node2D\n\nvar flash := 0.0\n";
	project.texts["res://hurt.tres"] = R"([gd_resource type="Animation" format=3]

[resource]
resource_name = "hurt"
tracks/0/type = "value"
tracks/0/path = NodePath(".:flash")
tracks/0/keys = {
"times": PackedFloat32Array(0),
"transitions": PackedFloat32Array(1),
"update": 0,
"values": [1.0]
}
)";
	for (const std::string &name : { "player", "enemy" }) {
		compiler::Scene scene;
		scene.path = "res://" + name + ".tscn";
		scene.nodes = { { ".", "Node2D", name == "player" ? "res://player.pet" : "res://enemy.gd" }, { "AnimationPlayer", "AnimationPlayer" } };
		scene.named_members = { { "AnimationPlayer", "hurt", "..", "flash", false, { { "res://hurt.tres", 0 } } } };
		project.scenes.push_back(scene);
	}
	CHECK(refusals(renamed(project, "res://player.pet", "flash", "blink")) == "res://enemy.tscn:0: \"flash\" here is another declaration, but renaming changes it in res://hurt.tres.\n");
}
