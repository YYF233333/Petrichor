#include "helpers.h"

#include "doctest.h"

using namespace petrichor;
using namespace petrichor::tests;

// Other classes of this language (spec §11.1): by class_name, as autoloads
// (§7.3), and their interfaces (§3.9). The scripts are cut down from the FPS
// starter kit's second paper migration.

namespace {

FakeProject project() {
	FakeProject project;
	project.add_script("res://weapon.pet",
			"extends Resource\nclass_name Weapon\n\n@export pub var damage: float = 25\n@export pub var model: PackedScene[Node3D]\nvar secret := 1\n\npub func describe() -> String:\n\treturn \"w\"\n\nfunc hidden():\n\tpass\n",
			"Weapon");
	project.add_script("res://combat.pet", "class_name Combat\n\npub interface Damageable:\n\tfunc damage(amount: float)\n", "Combat");
	project.add_script("res://enemy.pet", "extends Node3D implements Combat.Damageable\nclass_name Enemy\n\npub func damage(amount: float):\n\tpass\n", "Enemy");
	project.add_script("res://audio.pet", "extends Node\n\npub func play(sound_path: String):\n\tpass\n");
	project.add_script("res://broken.pet", "extends Node\nclass_name Broken\npub var x: int = \"no\"\n", "Broken");
	project.add_script("res://a.pet", "class_name A\nextends Node\npub var b: B?\n", "A");
	project.add_script("res://b.pet", "class_name B\nextends Node\npub var a: A?\n", "B");
	project.global_classes["Legacy"] = "res://legacy.gd";
	project.autoloads["Audio"] = "res://audio.pet";
	return project;
}

std::string errors(const std::string &p_body, const std::string &p_header = "extends Node") {
	return messages(compile_in(p_header + "\n\nfunc f(w: Weapon, n: Node, e: Enemy) -> void:\n\t" + p_body + "\n", project()));
}

bool contains(const std::string &p_text, const std::string &p_part) {
	return p_text.find(p_part) != std::string::npos;
}

} // namespace

TEST_CASE("a class_name names a class of another script") {
	CHECK(errors("var d: float = w.damage\n\tvar s: String = w.describe()\n\tvar m: PackedScene[Node3D] = w.model\n\tw.damage = 3").empty());
	CHECK(errors("var weapons: Array[Weapon] = [w]\n\tvar first: Weapon = weapons[0]").empty());
	CHECK(errors("var r: Resource = w").empty());
	CHECK(contains(errors("var x := w.secret"), "\"secret\" of Weapon is not pub."));
	CHECK(contains(errors("w.hidden()"), "\"hidden()\" of Weapon is not pub."));
	CHECK(contains(errors("w.describe(1)"), "takes 0 argument(s), 1 given"));
	CHECK(contains(errors("var x: Weapon = n"), "Cannot assign a value of type Node as Weapon"));
	CHECK(contains(errors("var x: Enemy = w"), "Cannot assign a value of type Weapon as Enemy"));
}

TEST_CASE("members without pub are visible in their own file only (spec §4.2)") {
	FakeProject classes = project();
	classes.add_script("res://door.pet", "class_name Door\nextends Node\n\nsignal opened\npub signal closed\n\ninterface Lock:\n\tfunc open()\n", "Door");
	const auto with_door = [&](const std::string &p_body) {
		return messages(compile_in("extends Node\n\nfunc f(d: Door) -> void:\n\t" + p_body + "\n", classes));
	};
	CHECK(with_door("var s: Signal = d.closed").empty());
	CHECK(contains(with_door("var s: Signal = d.opened"), "The signal \"opened\" of Door is not pub."));
	CHECK(contains(with_door("var l: Door.Lock"), "Door.Lock is not pub"));
	// Other instances of the same class are reached through the engine, which
	// sees only pub members (spec §10.5).
	const std::string own = messages(compile_in("class_name Gate\nextends Node\nvar hp := 1\n\nfunc f(other: Gate) -> int:\n\treturn other.hp\n", classes));
	CHECK(contains(own, "\"hp\" is not pub, and members that are not pub of other instances of this class are not supported yet."));
}

TEST_CASE("a member typed from scenes is not known to other scripts") {
	FakeProject classes = project();
	classes.add_script("res://spawner.pet", "class_name Spawner\nextends Node\n\npub var enemy_scene = preload(\"res://enemy.tscn\")\n", "Spawner");
	const std::string result = messages(compile_in("extends Node\n\nfunc f(s: Spawner) -> void:\n\tvar scene = s.enemy_scene\n", classes));
	CHECK(contains(result, "The type of Spawner.enemy_scene is not known here; declare it"));
}

TEST_CASE("T? and common types of a class of this language keep its class") {
	CHECK(errors("var weapons: Array[Weapon] = [w]\n\tvar last: Weapon? = weapons.pop_back()\n\tif last != None:\n\t\tprint(last.describe())").empty());
	// Enemy extends Node3D, Timer extends Node: the conditional is a Node (spec §5.5).
	CHECK(errors("var b := true\n\tvar t := Timer.new()\n\tvar x: Node = e if b else t").empty());
}

TEST_CASE("calls to methods of other classes that return arrays") {
	FakeProject classes = project();
	classes.add_script("res://inventory.pet", "class_name Inventory\n\npub func names() -> Array[String]:\n\treturn []\n", "Inventory");
	const compiler::CompileResult result = compile_in("extends Node\n\nfunc f(i: Inventory) -> int:\n\tvar names := i.names()\n\treturn names.size()\n", classes);
	REQUIRE_MESSAGE(result.ok(), messages(result));
	std::string error;
	CHECK_MESSAGE(bytecode::verify(result.module, error), error);
}

TEST_CASE("an autoload is a value of its script's class (spec §7.3)") {
	CHECK(errors("Audio.play(\"sounds/land.ogg\")").empty());
	CHECK(contains(errors("Audio.play(1)"), "Cannot pass a value of type int as String"));
}

TEST_CASE("classes may refer to each other") {
	const compiler::CompileResult result = compile_in("extends Node\n\nfunc f(a: A) -> void:\n\tvar b: B? = a.b\n\tif b != None:\n\t\tvar back: A? = b.a\n", project());
	CHECK_MESSAGE(result.ok(), messages(result));
}

TEST_CASE("names that are not classes of this language") {
	CHECK(contains(errors("var x: Legacy"), "Legacy is a class of another language"));
	CHECK(contains(errors("var x: Broken? = None\n\tif x != None:\n\t\tprint(x.x)"), "res://broken.pet has errors"));
	CHECK(contains(errors("var x: Combat.Nope"), "Combat declares no interface \"Nope\""));
}

TEST_CASE("implements: the class has every method of the interface, pub and with its signature (spec §3.9)") {
	const auto header = [](const std::string &p_methods) {
		return messages(compile_in("extends Node3D implements Combat.Damageable\n" + p_methods + "\n", project()));
	};
	CHECK(header("\npub func damage(amount: float):\n\tpass").empty());
	CHECK(contains(header(""), "The class does not implement damage() of Combat.Damageable."));
	CHECK(contains(header("\nfunc damage(amount: float):\n\tpass"), "implements Combat.Damageable and must be pub"));
	CHECK(contains(header("\npub func damage(amount: int):\n\tpass"), "does not have the signature that Combat.Damageable declares, func(float)."));
	CHECK(contains(messages(compile_in("extends Node implements Weapon\n", project())), "Weapon is not an interface."));
}

TEST_CASE("values of an interface type") {
	CHECK(errors("var d: Combat.Damageable = e\n\td.damage(5)").empty());
	CHECK(contains(errors("var d: Combat.Damageable = w"), "Cannot assign a value of type Weapon as Combat.Damageable"));
	CHECK(errors("if n is Combat.Damageable:\n\t\tn.damage(5)").empty());
	CHECK(contains(errors("if n is Combat.Damageable:\n\t\tn.damage(\"x\")"), "Cannot pass a value of type String as float"));
	CHECK(errors("var d := n as Combat.Damageable\n\tif d != None:\n\t\td.damage(1)").empty());
	CHECK(errors("var as_enemy := n as Enemy").empty());
	CHECK(contains(errors("var as_weapon := n as Weapon"), "Node and Weapon are unrelated classes"));
}
