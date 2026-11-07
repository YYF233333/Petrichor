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
	project.add_script("res://typing.pet", "class_name Typing\nextends Node\npub var hp: int = 3\npub func hit(n: int) -> void:\n\thp -= n +\npub func he\npub var x:\n"
										   "pub func empty() -> void:\npub var after: int = 2:\n\tget:\n\t\treturn after +\n",
			"Typing");
	project.add_script("res://partly.pet",
			"class_name Partly\nextends Node\npub var good: int = 1\npub var bad: Missing\npub func ok() -> int:\n\treturn good\npub func takes(m: Missing) -> void:\n\tpass\n"
			"pub const LIMIT: int = \"many\"\npub struct Hit:\n\tvar what: Missing\n",
			"Partly");
	project.add_script("res://half.pet", "extends Node3D implements Combat.Damageable\nclass_name Half\n", "Half");
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
	compiler::EngineMethod count;
	count.name = "count";
	count.owner = "res://legacy.gd";
	count.is_static = true;
	count.return_type = Type::of(TypeKind::Int);
	legacy.methods = { hit, count };
	legacy.constants = { { "MAX_SPEED", Type::of(TypeKind::Float), false, "res://legacy.gd" } };
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

	// Scripts without a class_name, referred to through preload (spec §2.1);
	// tanks-of-freedom-ii's unit.gd, cut down.
	compiler::ForeignClass unit;
	unit.native = "Node2D";
	compiler::EngineMethod make;
	make.name = "make";
	make.owner = "res://units/unit.gd";
	make.is_static = true;
	make.params = { { "level", Type::of(TypeKind::Int) } };
	make.return_type = Type::object("Node2D", true);
	compiler::EngineMethod move;
	move.name = "move";
	move.owner = "res://units/unit.gd";
	move.return_type = Type::of(TypeKind::Void);
	unit.methods = { make, move };
	unit.constants = { { "TILE_SIZE", Type::of(TypeKind::Float), false, "res://units/unit.gd" } };
	project.foreign["res://units/unit.gd"] = unit;
	project.resources["res://units/unit.gd"] = { "GDScript", "" };
	compiler::ForeignClass squad;
	squad.native = "RefCounted";
	compiler::EngineMethod init;
	init.name = "_init";
	init.owner = "res://units/squad.gd";
	init.params = { { "size", Type::of(TypeKind::Int) } };
	init.return_type = Type::of(TypeKind::Void);
	squad.methods = { init };
	project.foreign["res://units/squad.gd"] = squad;
	project.add_script("res://tools/grid.pet", "extends Node\n\npub const SIZE = 16\nconst HIDDEN = 1\n\npub static func cell(x: int) -> int:\n\treturn x / SIZE\n\npub func clear():\n\tpass\n");
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

TEST_CASE("members without pub are visible in their own file and subclasses (spec §4.2)") {
	FakeProject classes = project();
	classes.add_script("res://door.pet", "class_name Door\nextends Node\n\nsignal opened\npub signal closed\nvar code := 1\n\nfunc knock() -> void:\n\tpass\n\ninterface Lock:\n\tfunc open()\n", "Door");
	const auto with_door = [&](const std::string &p_body) {
		return messages(compile_in("extends Node\n\nfunc f(d: Door) -> void:\n\t" + p_body + "\n", classes));
	};
	CHECK(with_door("var s: Signal = d.closed").empty());
	CHECK(contains(with_door("var s: Signal = d.opened"), "The signal \"opened\" of Door is not pub."));
	CHECK(contains(with_door("var l: Door.Lock"), "Door.Lock is not pub"));
	CHECK(contains(with_door("print(d.code)"), "\"code\" of Door is not pub."));
	// Of other instances too, here in a subclass.
	const compiler::CompileResult sub = compile_in("extends Door\n\nfunc f(other: Door) -> int:\n\tother.knock()\n\tvar s: Signal = other.opened\n\treturn other.code\n", classes);
	CHECK_MESSAGE(sub.ok(), messages(sub));
}

TEST_CASE("a method of another object is a value, which does not keep the object (spec §3.11)") {
	const compiler::CompileResult result = compile_in("extends Node\n\nfunc f(w: Weapon, e: Enemy) -> String:\n\tvar hit: Combat.Damageable = e\n\tvar a := e.damage\n\tvar b := hit.damage\n\tvar c := e.rotate_y\n\tvar describe := w.describe\n\treturn describe.call()\n", project());
	REQUIRE_MESSAGE(result.ok(), messages(result));
	std::string error;
	CHECK_MESSAGE(bytecode::verify(result.module, error), error);
	CHECK(contains(errors("var x := w.hidden"), "\"hidden()\" of Weapon is not pub."));
	CHECK(contains(errors("var x := Weapon.new().describe"), "nothing else holds this new Weapon (spec §3.11)."));
	CHECK(contains(errors("var x := RandomNumberGenerator.new().randi"), "nothing else holds this new RandomNumberGenerator"));
	CHECK(!contains(errors("var x := Node.new().queue_free"), "nothing else holds"));
}

TEST_CASE("a pub declaration shows only pub types (spec §4.2)") {
	const auto errors_of_class = [](const std::string &p_members) {
		return messages(compile_in("class_name Vault\nextends Node\n\nstruct Coin:\n\tvar value: int = 1\n\nenum Hidden { A, B }\n\ninterface Lock:\n\tfunc open()\n\nclass Badge:\n\tvar rank := 3\n\n" + p_members, FakeProject()));
	};
	CHECK(errors_of_class("var coins: Array[Coin] = []\n\nfunc take(lock: Lock) -> Coin:\n\treturn Coin()\n").empty());
	CHECK(contains(errors_of_class("pub var coins: Array[Coin] = []\n"), "Vault.Coin is not pub, but the pub \"coins\" shows it (spec §4.2)."));
	CHECK(contains(errors_of_class("pub func pick(lock: Lock?) -> void:\n\tpass\n"), "Vault.Lock is not pub, but the pub \"pick()\" shows it"));
	CHECK(contains(errors_of_class("pub signal turned(state: Hidden)\n"), "is not pub, but the pub signal \"turned\" shows it"));
	CHECK(contains(errors_of_class("pub func make() -> Badge:\n\treturn Badge.new()\n"), "Vault.Badge is not pub, but the pub \"make()\" shows it"));
	CHECK(contains(errors_of_class("pub struct Purse:\n\tvar coin: Coin\n"), "Vault.Coin is not pub, but the field \"coin\" of the pub Purse shows it"));
	CHECK(contains(errors_of_class("pub var count := Coin()\n"), "the pub \"count\" shows it"));
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
	CHECK(storage(module, method(0).result.type) == bytecode::ValueType::String);
	// The signature as declared, also the parameter the call leaves out.
	CHECK(method(1).method == "hit");
	CHECK(method(1).params.size() == 2);
	CHECK(method(1).defaults == 1);
	CHECK(bytecode::type_key(module.types, method(1).result.type) == "a(v" + std::to_string(int(bytecode::ValueType::Int)) + ")");
	CHECK(method(2).method == "damage");
	CHECK(storage(module, method(2).result.type) == bytecode::ValueType::Void);
	CHECK(method(3).method == "id");
	CHECK(bytecode::type_key(module.types, method(3).result.type) == "?(v" + std::to_string(int(bytecode::ValueType::Int)) + ")");
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
	const std::vector<bytecode::TypeDesc> &types = result.module.types;
	const auto param = [&](const char *p_function) -> const bytecode::Slot & { return function_named(result.module, p_function).params[0]; };
	const bytecode::ClassRef &a = types[param("a").type].object;
	CHECK((a.name == "Object" && a.script == "res://combat.pet" && a.interface_name == "Damageable"));
	const bytecode::ClassRef &b = types[param("b").type].object;
	CHECK((b.script == "res://heal.pet" && b.interface_name == "Healable"));
	const bytecode::TypeDesc &c = types[param("c").type];
	CHECK((c.kind == bytecode::TypeKind::Array && types[c.element].object == b));
	const bytecode::Slot none;
	CHECK(bytecode::signature_key(types, { param("a") }, 0, none) != bytecode::signature_key(types, { param("b") }, 0, none));
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
	CHECK(contains(errors("var x: Broken? = None\n\tif x != None:\n\t\tprint(x.x)"), "Broken.x has errors (res://broken.pet, line 3)."));
	CHECK(contains(errors("var x: Combat.Nope"), "Combat declares no class, interface, struct or enum \"Nope\""));
}

TEST_CASE("other scripts see what the parser recovers of a script with syntax errors") {
	CHECK(errors("Typing.new().hit(Typing.new().hp)").empty());
	CHECK(contains(errors("Typing.new().heal(1)"), "Typing has no method \"heal()\"; res://typing.pet has syntax errors (line 5)."));
	CHECK(contains(errors("print(Typing.new().armor)"), "Typing has no property \"armor\"; res://typing.pet has syntax errors (line 5)."));
	CHECK(contains(errors("print(w.armor)"), "Weapon has no property \"armor\"."));
	CHECK(contains(errors("print(Typing.new().x)"), "Typing.x has errors (res://typing.pet, line 7)."));
	// A missing block is an error of its header, a getter's body is not the property's.
	CHECK(contains(errors("Typing.new().empty()"), "Typing.empty() has errors (res://typing.pet, line 8)."));
	CHECK(errors("print(Typing.new().after)").empty());
}

TEST_CASE("other scripts use the declarations of a script that have no errors") {
	CHECK(errors("print(Partly.new().good + Partly.new().ok())").empty());
	CHECK(contains(errors("print(Partly.new().bad)"), "Partly.bad has errors (res://partly.pet, line 4)."));
	CHECK(contains(errors("Partly.new().takes(null)"), "Partly.takes() has errors (res://partly.pet, line 7)."));
	CHECK(contains(errors("print(Partly.LIMIT)"), "Partly.LIMIT has errors (res://partly.pet, line 9)."));
	CHECK(contains(errors("var h: Partly.Hit"), "Partly.Hit has errors (res://partly.pet, line 11)."));
	// Implementing an interface is one: the class is usable but not as it.
	CHECK(errors("var h: Node3D = Half.new()").empty());
	CHECK(contains(errors("var d: Combat.Damageable? = Half.new()"), "Half implements Combat.Damageable with errors (res://half.pet, line 1)."));
	CHECK(contains(errors("var all: Array[Combat.Damageable] = [Half.new()]"), "Half implements Combat.Damageable with errors (res://half.pet, line 1)."));
	// A subclass holds all of its base's members.
	CHECK(contains(errors("pass", "extends Partly"), "Base script res://partly.pet has errors (line 4)."));
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

TEST_CASE("a constant of a preloaded script names its class (spec §2.1)") {
	const auto in = [](const std::string &p_body, const std::string &p_members = "") {
		return messages(compile_in("extends Node\n\nconst Unit = preload(\"res://units/unit.gd\")\nconst Grid = preload(\"tools/grid.pet\")\n" + p_members + "\nfunc f(n: Node) -> void:\n\t" + p_body + "\n", project()));
	};
	// As a type, in is and as, and with new(), static methods and constants.
	CHECK(in("var u: Unit = Unit.new()\n\tu.position = Vector2.ONE * Unit.TILE_SIZE\n\tvar made: Node2D? = Unit.make(2)\n\tif n is Unit:\n\t\tpass\n\tvar maybe: Unit? = n as Unit\n\tsquad.append(u)", "var squad: Array[Unit] = []\n").empty());
	CHECK(in("var g: Grid = Grid.new()\n\tg.clear()\n\tvar c: int = Grid.cell(40) + Grid.SIZE").empty());
	CHECK(contains(in("Unit.move()"), "\"move()\" of unit.gd is not static; call it on an object."));
	CHECK(contains(in("Unit.missing()"), "unit.gd has no method \"missing()\"."));
	CHECK(contains(in("Unit.make(\"two\")"), "String"));
	CHECK(contains(in("Unit.TILE_SIZE = 3.0"), "Cannot assign to the constant \"TILE_SIZE\"."));
	CHECK(contains(in("print(Unit.NOPE)"), "unit.gd has no constant \"NOPE\"."));
	CHECK(contains(in("Grid.clear()"), "\"clear()\" of grid.pet is not static"));
	CHECK(contains(in("print(Grid.HIDDEN)"), "The constant \"HIDDEN\" of grid.pet is not pub."));
	// Where a value is expected, it is the script.
	CHECK(in("n.set_script(Unit)\n\tvar s: GDScript = Unit\n\tvar g: Script = Grid").empty());
	CHECK(contains(in("var s: Node = Unit"), "Cannot assign a value of type GDScript as Node"));
	// A local constant, and the preload itself.
	CHECK(in("const Squad = preload(\"res://units/squad.gd\")\n\tvar s: Squad = Squad.new(3)\n\tvar u := preload(\"res://units/unit.gd\").new()\n\tvar c := preload(\"res://tools/grid.pet\").cell(1)").empty());
	// new() takes what _init does.
	CHECK(contains(in("var s := preload(\"res://units/squad.gd\").new()"), "takes 1 argument(s), 0 given"));
}

TEST_CASE("GDScript classes by class_name: new(), static methods and constants (spec §11.1)") {
	CHECK(errors("var l: Legacy = Legacy.new()\n\tvar c: int = Legacy.count()\n\tvar s: float = Legacy.MAX_SPEED").empty());
	CHECK(contains(errors("Legacy.hit(1)"), "\"hit()\" of Legacy is not static"));
	CHECK(errors("var l: Legacy = Legacy.new()\n\tvar c: int = l.count()").empty());
	const compiler::CompileResult result = compile_in("extends Node\n\nconst Unit = preload(\"res://units/unit.gd\")\n\nfunc f() -> float:\n\tvar u := Unit.new()\n\tvar l := Legacy.new()\n\tvar made := Unit.make(1)\n\treturn Unit.TILE_SIZE + Legacy.MAX_SPEED\n", project());
	REQUIRE_MESSAGE(result.ok(), messages(result));
	std::string error;
	CHECK_MESSAGE(bytecode::verify(result.module, error), error);
	// Called by name on the script, which preload() loads.
	std::vector<std::string> calls;
	for (const bytecode::EngineCall &call : result.module.engine_calls) {
		calls.push_back(call.method);
	}
	CHECK(std::count(calls.begin(), calls.end(), "new") == 2);
	CHECK(std::count(calls.begin(), calls.end(), "make") == 1);
}

TEST_CASE("the declarations know the classes that preloads of scripts name (spec §2.1)") {
	// Of this language from its header; of another without a global name not
	// at all, as no script is loaded.
	const compiler::CompileResult result = compile_in("extends Node\n\nconst Grid = preload(\"res://tools/grid.pet\")\nconst Unit = preload(\"res://units/unit.gd\")\n\npub var grid: Grid?\npub var unit: Unit?\n", project(), true);
	CHECK_MESSAGE(result.ok(), messages(result));
	FakeProject classes = project();
	classes.add_script("res://board.pet", "class_name Board\nextends Node\n\nconst Grid = preload(\"res://tools/grid.pet\")\n\npub var grid: Grid?\n", "Board");
	CHECK(messages(compile_in("extends Node\n\nfunc f(b: Board) -> void:\n\tvar g = b.grid\n\tif g != None:\n\t\tg.clear()\n", classes)).empty());
	// A subclass uses the constant by its bare name (spec §4.5).
	CHECK(messages(compile_in("extends Board\n\nfunc f() -> int:\n\treturn Grid.cell(3)\n", classes)).empty());
}

TEST_CASE("the declarations do not load scripts of other languages (spec §11.1)") {
	// The global class list names a class's base; members, and an autoload
	// without a class_name, are known only when the script is loaded.
	const compiler::CompileResult result = compile_in(
			"extends Node\n\npub var buddy: Legacy?\nvar percent = ValueManager.drop_percent\n\npub func take(l: OldLegacy) -> Legacy:\n\treturn l\n", project(), true);
	CHECK_MESSAGE(result.ok(), messages(result));
}

TEST_CASE("implements: the class has every method of the interface, pub, not static and with its signature (spec §3.9)") {
	const auto header = [](const std::string &p_methods) {
		return messages(compile_in("extends Node3D implements Combat.Damageable\n" + p_methods + "\n", project()));
	};
	CHECK(header("\npub func damage(amount: float):\n\tpass").empty());
	CHECK(contains(header(""), "The class does not implement damage() of Combat.Damageable."));
	CHECK(contains(header("\nfunc damage(amount: float):\n\tpass"), "implements Combat.Damageable and must be pub"));
	CHECK(contains(header("\npub static func damage(amount: float):\n\tpass"), "\"damage()\" implements Combat.Damageable, so it cannot be static (spec §3.9)."));
	CHECK(contains(header("\npub func damage(amount: int):\n\tpass"), "does not have the signature that Combat.Damageable declares, func(float)."));
	CHECK(contains(messages(compile_in("extends Node implements Weapon\n", project())), "Weapon is not an interface."));
}

TEST_CASE("implements: interface properties, by a pub member of the same type or an engine property (spec §3.9)") {
	FakeProject p = project();
	p.add_script("res://placed.pet", "class_name Placed\n\npub interface Spot:\n\tvar health: int\n\tvar position: Vector3\n\npub interface Rooted:\n\t@readonly\n\tvar root: Window?\n\npub interface Rerooted:\n\tvar root: Window?\n", "Placed");
	const auto spot = [&](const std::string &p_header, const std::string &p_members) {
		return messages(compile_in(p_header + " implements Placed.Spot\n" + p_members + "\n", p));
	};
	CHECK(spot("extends Node3D", "pub var health: int = 1").empty());
	CHECK(contains(spot("extends Node3D", ""), "The class does not implement the property \"health\" of Placed.Spot."));
	CHECK(contains(spot("extends Node3D", "var health: int = 1"), "\"health\" implements Placed.Spot and must be pub"));
	CHECK(contains(spot("extends Node3D", "pub var health: float = 1.0"), "\"health\" has type float, but Placed.Spot requires int."));
	CHECK(contains(spot("extends Node", "pub var health: int = 1"), "The class does not implement the property \"position\" of Placed.Spot."));
	CHECK(messages(compile_in("extends SceneTree implements Placed.Rooted\n", p)).empty());
	CHECK(contains(messages(compile_in("extends SceneTree implements Placed.Rerooted\n", p)), "The engine property \"root\" is read-only, so it satisfies only a @readonly property of Placed.Rerooted."));

	const auto body = [&](const std::string &p_body) {
		return messages(compile_in("extends Node\n\nfunc f(s: Placed.Spot, r: Placed.Rooted):\n\t" + p_body + "\n", p));
	};
	CHECK(body("s.health += 1\n\tvar at: Vector3 = s.position\n\tvar w: Window? = r.root").empty());
	CHECK(contains(body("r.root = None"), "The property \"root\" of Placed.Rooted is @readonly."));
	CHECK(contains(body("var h: String = s.health"), "Cannot assign a value of type int as String"));
	CHECK(contains(messages(compile_in("extends Node\n@readonly\nvar x: int = 1\n", p)), "@readonly is only for interface properties (spec §3.9)."));
	CHECK(contains(messages(compile_in("pub interface Twice:\n\tfunc a()\n\tvar a: int\n", p)), "\"a\" is already declared in Twice."));
	CHECK(contains(messages(compile_in("pub interface Loud:\n\tsignal hit\n", p)), "Signals of interfaces are not supported yet."));
	CHECK(contains(messages(compile_in("pub interface Kind:\n\tfunc a():\n\t\tpass\n", p)), "Default methods of interfaces are not supported yet."));
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

TEST_CASE("self is a value of this class, except in the PREDELETE branch (spec §6.6)") {
	const std::string head = "extends Node implements Combat.Damageable\nclass_name Turret\n\nvar hp := 1\n\npub func damage(amount: float):\n\tpass\n\n";
	const compiler::CompileResult result = compile_in(head + "func f(n: Node, hp: String) -> void:\n\tvar me: Turret = self\n\tvar d: Combat.Damageable = self\n\tn.add_child(self)\n\tself.hp = 2\n\tvar h: int = self.hp\n", project());
	REQUIRE_MESSAGE(result.ok(), messages(result));
	std::string error;
	CHECK_MESSAGE(bytecode::verify(result.module, error), error);
	find_instruction(function_named(result.module, "f"), bytecode::Op::GET_SELF);

	const auto check = [&](const std::string &p_functions) { return messages(compile_in(head + p_functions, project())); };
	CHECK(contains(check("static func s() -> void:\n\tvar me := self\n"), "\"self\" belongs to an instance"));
	CHECK(contains(check("func f() -> void:\n\tvar x = self.nope\n"), "Turret has no member \"nope\"."));
	const std::string predelete = check("func _notification(what: int) -> void:\n\tif what == Object.NOTIFICATION_PREDELETE:\n\t\tprint(self.hp)\n\t\tprint(self)\n\tmatch what:\n\t\tObject.NOTIFICATION_PREDELETE:\n\t\t\tvar me: Turret = self\n\t\t_:\n\t\t\tprint(self)\n");
	CHECK(predelete == "12:9: self cannot be used as a value in the PREDELETE branch (spec §6.6).\n15:21: self cannot be used as a value in the PREDELETE branch (spec §6.6).\n");
}
