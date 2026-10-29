#include "bytecode/opcodes.h"
#include "doctest.h"
#include "helpers.h"

using namespace petrichor;
using namespace petrichor::tests;
using compiler::Type;
using compiler::TypeKind;

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
	project.autoloads["Audio"] = "res://audio.pet";
	project.global_classes["Gone"] = "res://gone.gd";

	// GDScript classes as reflection shows them (spec §11.1).
	compiler::ForeignClass legacy;
	legacy.native = "Node3D";
	legacy.properties = {
		{ "target", Type::object("Node3D", true), true, "res://legacy.gd" },
		{ "speed", Type::of(TypeKind::Float), true, "res://legacy.gd" },
		{ "anything", Type::of(TypeKind::Variant), true, "res://legacy.gd" },
		{ "buddy", Type::object("Legacy", true), true, "res://legacy.gd" },
	};
	compiler::EngineMethod hit;
	hit.name = "hit";
	hit.owner = "res://legacy.gd";
	hit.params = { { "amount", Type::of(TypeKind::Int) } };
	hit.return_type = Type::of(TypeKind::Bool);
	legacy.methods = { hit };
	compiler::EngineMethod scored;
	scored.name = "scored";
	scored.params = { { "points", Type::of(TypeKind::Int) } };
	legacy.signals = { scored };
	project.foreign["res://legacy.gd"] = legacy;
	project.global_classes["Legacy"] = "res://legacy.gd";

	compiler::ForeignClass old_legacy;
	old_legacy.native = "Node3D";
	old_legacy.base = "res://legacy.gd";
	old_legacy.properties = legacy.properties;
	project.foreign["res://old_legacy.gd"] = old_legacy;
	project.global_classes["OldLegacy"] = "res://old_legacy.gd";

	// An autoload without a class_name.
	compiler::ForeignClass values;
	values.native = "Node";
	values.properties = { { "drop_percent", Type::of(TypeKind::Float), true, "res://value_manager.gd" } };
	compiler::EngineMethod spawn;
	spawn.name = "spawn";
	spawn.owner = "res://value_manager.gd";
	spawn.params = { { "count", Type::of(TypeKind::Int) } };
	spawn.return_type = Type::of(TypeKind::Void);
	values.methods = { spawn };
	project.foreign["res://value_manager.gd"] = values;
	project.autoloads["ValueManager"] = "res://value_manager.gd";
	return project;
}

std::string errors(const std::string &p_body, const std::string &p_header = "extends Node") {
	return messages(compile_in(p_header + "\n\nfunc f(w: Weapon, n: Node, e: Enemy) -> void:\n\t" + p_body + "\n", project()));
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

TEST_CASE("a method of another class, another instance or an interface is called directly (spec §8.1)") {
	FakeProject classes = project();
	classes.add_script("res://shield.pet", "class_name Shield\nextends Node\n\npub func hit(amount: float, crit: bool = false) -> Array[int]:\n\treturn []\n\nfunc _ready():\n\tpass\n", "Shield");
	const compiler::CompileResult result = compile_in(
			"class_name Gate\nextends Node\n\npub func id() -> int?:\n\treturn 1\n\n"
			"func f(w: Weapon, s: Shield, d: Combat.Damageable, other: Gate) -> void:\n\tvar text := w.describe()\n\tvar hits := s.hit(2)\n\td.damage(5)\n\tvar i := other.id()\n\ts._ready()\n",
			classes);
	REQUIRE_MESSAGE(result.ok(), messages(result));
	std::string error;
	CHECK_MESSAGE(bytecode::verify(result.module, error), error);
	const bytecode::Module &module = result.module;
	const bytecode::Function &f = function_named(module, "f");
	std::vector<uint32_t> methods;
	int engine_calls = 0;
	for (size_t pc = 0; pc < f.code.size(); pc += bytecode::instruction_length(f.code.data() + pc)) {
		if (bytecode::Op(f.code[pc]) == bytecode::Op::CALL_METHOD) {
			methods.push_back(f.code[pc + 1]);
		}
		engine_calls += bytecode::Op(f.code[pc]) == bytecode::Op::CALL_ENGINE ? 1 : 0;
	}
	REQUIRE(methods.size() == 4);
	const auto method = [&](size_t p_call) -> const bytecode::MethodCall & { return module.method_calls[methods[p_call]]; };
	CHECK(method(0).method == "describe");
	CHECK(method(0).result.type == bytecode::ValueType::String);
	// The signature as declared, also the parameter the call leaves out.
	CHECK(method(1).method == "hit");
	CHECK(method(1).params.size() == 2);
	CHECK(method(1).defaults == 1);
	CHECK(method(1).result.element == bytecode::ValueType::Int);
	CHECK(method(2).method == "damage");
	CHECK(method(2).result.type == bytecode::ValueType::Void);
	CHECK(method(3).method == "id");
	CHECK(method(3).result.option == bytecode::ValueType::Int);
	// An override of an engine virtual method is the engine's to run.
	CHECK(engine_calls == 1);
}

TEST_CASE("an interface is described in slots, for the entry checks and signatures (spec §3.9, §10.1)") {
	FakeProject classes = project();
	classes.add_script("res://heal.pet", "class_name Heal\n\npub interface Healable:\n\tfunc heal(amount: float)\n", "Heal");
	const compiler::CompileResult result = compile_in("extends Node\n\nfunc a(d: Combat.Damageable) -> void:\n\tpass\n\nfunc b(h: Heal.Healable) -> void:\n\tpass\n\nfunc c(all: Array[Heal.Healable]) -> void:\n\tpass\n", classes);
	REQUIRE_MESSAGE(result.ok(), messages(result));
	std::string error;
	CHECK_MESSAGE(bytecode::verify(result.module, error), error);
	const auto param = [&](const char *p_function) -> const bytecode::Slot & { return function_named(result.module, p_function).params[0]; };
	CHECK(param("a").interface_name == "res://combat.pet::Damageable");
	CHECK(param("a").class_name == "Object");
	CHECK(param("a").script.empty());
	CHECK(param("b").interface_name == "res://heal.pet::Healable");
	CHECK(param("c").element == bytecode::ValueType::Object);
	CHECK(param("c").interface_name == "res://heal.pet::Healable");
	const bytecode::Slot none;
	CHECK(bytecode::signature_key({ param("a") }, 0, none) != bytecode::signature_key({ param("b") }, 0, none));
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
	CHECK(contains(errors("var x: Gone"), "Gone (res://gone.gd) cannot be read."));
	CHECK(contains(errors("var x: Legacy.Thing"), "Legacy is a class of another language, which has no interfaces."));
	CHECK(contains(errors("var x: Broken? = None\n\tif x != None:\n\t\tprint(x.x)"), "res://broken.pet has errors"));
	CHECK(contains(errors("var x: Combat.Nope"), "Combat declares no interface \"Nope\""));
}

TEST_CASE("GDScript classes and autoloads as reflection shows them (spec §11.1)") {
	const auto in = [](const std::string &p_body) {
		return messages(compile_in("extends Node\n\nfunc f(l: Legacy, old: OldLegacy, n: Node) -> void:\n\t" + p_body + "\n", project()));
	};
	// Objects are T?, value types as declared, untyped ones Variant.
	CHECK(in("var t: Node3D? = l.target\n\tvar s: float = l.speed\n\tvar b: Legacy? = l.buddy").empty());
	CHECK(contains(in("var t: Node3D = l.target"), "Node3D?"));
	CHECK(contains(in("var s: int = l.speed"), "int(x)"));
	CHECK(contains(in("print(l.anything + 1)"), "Variant"));
	CHECK(in("l.speed = 2.5").empty());
	CHECK(contains(in("print(l.nope)"), "Legacy has no property \"nope\""));
	CHECK(in("print(l.position)").empty());

	// Methods and signals are checked like the engine's.
	CHECK(in("var hit: bool = l.hit(3)\n\tl.scored.emit(10)").empty());
	CHECK(contains(in("l.hit(\"no\")"), "String"));
	CHECK(contains(in("l.scored.emit(\"no\")"), "String"));

	// A class extends the scripts it extends; as and is name them.
	CHECK(in("var as_base: Legacy = old\n\tvar l2: Legacy? = n as Legacy\n\tif n is OldLegacy:\n\t\tpass").empty());
	CHECK(contains(in("var o: OldLegacy = l"), "OldLegacy"));

	// An autoload is typed by its script's class, also without a class_name.
	CHECK(in("var p: float = ValueManager.drop_percent * 2.0\n\tValueManager.spawn(3)").empty());
	CHECK(contains(in("ValueManager.spawn(\"no\")"), "String"));
}

TEST_CASE("the declarations do not load scripts of other languages (spec §11.1)") {
	// The global class list names a class's base; members, and an autoload
	// without a class_name, are known only when the script is loaded.
	const compiler::CompileResult result = compile_in(
			"extends Node\n\npub var buddy: Legacy?\nvar percent = ValueManager.drop_percent\n\npub func take(l: OldLegacy) -> Legacy:\n\treturn l\n", project(), true);
	CHECK_MESSAGE(result.ok(), messages(result));
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
