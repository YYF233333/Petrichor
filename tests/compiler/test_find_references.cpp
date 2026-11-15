#include "compiler/navigation.h"
#include "doctest.h"
#include "helpers.h"

using namespace petrichor;
using namespace petrichor::tests;

// Finding references (spec §13.2): the declaration and every place that
// refers to it, in scripts and scenes, literal names included (§10.4), then
// the candidates that renaming leaves unchecked (§13.3).

namespace {

FakeProject project() {
	FakeProject project;
	project.add_script("res://base.pet", "class_name Base\nextends Node\n\npub signal hurt(amount: int)\n\n@export pub var speed := 1.0\n\n\npub func hit() -> int:\n\treturn 1\n", "Base");
	project.add_script("res://derived.pet", "class_name Derived\nextends Base\n\n\npub func hit() -> int:\n\treturn 2\n", "Derived");
	project.add_script("res://other.pet", "extends Base\n\n\npub func hit() -> int:\n\treturn 3\n");
	project.add_script(
			"res://user.pet",
			"extends Node\n\n\n"
			"pub func on_hurt(amount: int):\n"
			"\tpass\n\n\n"
			"func f(b: Base, d: Derived, o: Object):\n"
			"\tprint(b.hit() + d.hit(), b.speed)\n"
			"\tb.connect(\"hurt\", on_hurt)\n"
			"\tb.set_deferred(\"speed\", 2.0)\n"
			"\to.call_deferred(\"hit\")\n"
			"\tvar hit := 1\n"
			"\tprint(hit)\n");
	project.sources["res://legacy.gd"] = "extends Node\n\nfunc hit():\n\tpass\n\nfunc hitter():\n\thit()\n";
	compiler::Scene level;
	level.path = "res://level.tscn";
	compiler::SceneValue fast{ "float" };
	fast.integer = 3;
	fast.whole = true;
	level.nodes = {
		{ ".", "Node", "res://user.pet" },
		{ "Enemy", "Node", "res://base.pet", false, "", false, { { "speed", fast } } },
		{ "AnimationPlayer", "AnimationPlayer" },
	};
	level.connections = { { "hurt", "Enemy", ".", "on_hurt", 0, 0 } };
	level.named_members = { { "AnimationPlayer", "run", "../Enemy", "speed", false } };
	project.scenes = { level };
	return project;
}

// What finding references at the nth `p_text` in the script at `p_path`
// lists: a place in a script as "file:line:column", in a scene as its file
// and node with the property, signal or track, then whether it is the
// declaration or a candidate.
std::string found(const FakeProject &p_project, const std::string &p_path, const std::string &p_text, int p_nth = 0) {
	const std::string &source = p_project.sources.at(p_path);
	size_t offset = source.find(p_text);
	for (int i = 0; i < p_nth; i++) {
		offset = source.find(p_text, offset + 1);
	}
	REQUIRE(offset != std::string::npos);
	compiler::Analysis analysis(fake_engine(), p_project);
	const compiler::Analysis::Pass pass(analysis);
	for (const auto &[path, text] : p_project.sources) {
		if (compiler::is_script_path(path)) {
			const auto compiled = analysis.compile(path, text);
			REQUIRE_MESSAGE(compiled->errors.empty(), path, "\n", messages(*compiled));
		}
	}
	for (const compiler::Scene &scene : p_project.scenes) {
		const auto checked = analysis.check_scene(scene.path);
		REQUIRE_MESSAGE(checked->errors.empty(), scene.path, "\n", messages(*checked));
	}
	const auto result = compiler::find_references(analysis, p_path, source, uint32_t(offset), { "res://legacy.gd" });
	REQUIRE(result);
	std::vector<std::string> shown;
	for (const compiler::Occurrence &each : *result) {
		const compiler::Reference &reference = each.reference;
		std::string place = each.file;
		if (each.line > 0) {
			place += ":" + std::to_string(each.line) + ":" + std::to_string(each.column);
			CHECK(each.text.substr(each.column - 1, each.end_column - each.column) == p_project.sources.at(each.file).substr(reference.begin, reference.end - reference.begin));
		} else {
			place += " " + reference.node;
			place += reference.connection.signal.empty() ? std::string() : " " + reference.connection.source + "." + reference.connection.signal;
			place += reference.named.from.empty() ? std::string() : " " + reference.named.from + "/" + reference.named.animation;
			place += reference.property.empty() ? std::string() : ":" + reference.property;
		}
		shown.push_back(place + (each.declaration ? " declaration" : "") + (each.candidate ? " candidate" : ""));
	}
	return joined(shown);
}

} // namespace

TEST_CASE("a member is found where other scripts use it, by name in a literal too, and where scenes store and animate it") {
	const std::string expected = joined({
			"res://base.pet:6:17 declaration",
			"res://level.tscn Enemy AnimationPlayer/run",
			"res://level.tscn Enemy:speed",
			"res://user.pet:9:29",
			"res://user.pet:11:18",
	});
	CHECK(found(project(), "res://user.pet", "speed") == expected);
}

TEST_CASE("a method and a signal a scene connects are found in the scene, and a signal in a literal") {
	const std::string method = joined({
			"res://user.pet:4:10 declaration",
			"res://user.pet:10:20",
			"res://level.tscn . Enemy.hurt",
	});
	CHECK(found(project(), "res://user.pet", "on_hurt", 1) == method);
	const std::string signal = joined({
			"res://base.pet:4:12 declaration",
			"res://level.tscn Enemy Enemy.hurt",
			"res://user.pet:10:13",
	});
	CHECK(found(project(), "res://user.pet", "hurt\"") == signal);
}

TEST_CASE("a property path names the member in its first subname") {
	FakeProject paths;
	paths.add_script("res://shake.pet", "extends Node\n\npub var offset := Vector2.ZERO\n\n\nfunc shake(tween: Tween):\n\ttween.tween_property(self, \"offset:x\", 2.0, 1.0)\n");
	const std::string expected = joined({
			"res://shake.pet:3:9 declaration",
			"res://shake.pet:7:30",
	});
	CHECK(found(paths, "res://shake.pet", "offset") == expected);
}

TEST_CASE("a method is found with those it overrides and that override it, then the names that may call it") {
	const std::string expected = joined({
			"res://derived.pet:5:10 declaration",
			"res://base.pet:9:10 declaration",
			"res://other.pet:4:10 declaration",
			"res://user.pet:9:10",
			"res://user.pet:9:20",
			"res://legacy.gd:3:6 candidate",
			"res://legacy.gd:7:2 candidate",
			"res://user.pet:12:19 candidate",
	});
	CHECK(found(project(), "res://derived.pet", "hit") == expected);
}

TEST_CASE("a method of an interface is found with its implementations") {
	FakeProject combat;
	combat.add_script("res://combat.pet", "class_name Combat\n\npub interface Damageable:\n\tfunc damage(amount: int)\n", "Combat");
	for (const char *path : { "res://crate.pet", "res://wall.pet" }) {
		combat.add_script(path, "extends Node implements Combat.Damageable\n\n\npub func damage(amount: int):\n\tpass\n");
	}
	combat.add_script("res://user.pet", "extends Node\n\n\nfunc f(target: Combat.Damageable):\n\ttarget.damage(1)\n");
	const std::string expected = joined({
			"res://wall.pet:4:10 declaration",
			"res://combat.pet:4:7 declaration",
			"res://crate.pet:4:10 declaration",
			"res://user.pet:5:9",
	});
	CHECK(found(combat, "res://wall.pet", "damage") == expected);
}

TEST_CASE("a node path finds the node in its scene and every script that names it") {
	FakeProject nodes;
	nodes.add_script("res://player.pet", "extends Node3D\n\n\nfunc _ready():\n\tprint($Head)\n");
	nodes.add_script("res://gun.pet", "extends Node3D\n\n\nfunc _ready():\n\tprint($\"../Head\")\n");
	compiler::Scene player;
	player.path = "res://player.tscn";
	player.nodes = { { ".", "Node3D", "res://player.pet" }, { "Head", "Node3D" }, { "Gun", "Node3D", "res://gun.pet" } };
	nodes.scenes = { player };
	const std::string expected = joined({
			"res://player.tscn Head declaration",
			"res://gun.pet:5:13",
			"res://player.pet:5:9",
	});
	CHECK(found(nodes, "res://player.pet", "Head") == expected);
}

TEST_CASE("a local is found only in its function, without candidates") {
	const std::string expected = joined({
			"res://user.pet:13:6 declaration",
			"res://user.pet:14:8",
	});
	CHECK(found(project(), "res://user.pet", "hit", 3) == expected);
}
