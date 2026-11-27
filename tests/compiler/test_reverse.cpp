#include "compiler/migration.h"
#include "compiler/reverse.h"
#include "doctest.h"
#include "helpers.h"

using namespace petrichor;
using namespace petrichor::tests;

// Reverse conversion (spec §13.4, architecture §6.6): what it writes back,
// the files it takes and refuses, and the re-parse check.

namespace {

using Pairs = std::vector<std::pair<std::string, std::string>>;

// The GDScript of res://unit.pet, the only file of a project; or else, each
// place that keeps it, as "line:column: message".
std::string reversed(const std::string &p_source) {
	FakeProject project;
	project.add_script("res://unit.pet", p_source);
	const compiler::Reversal reversal = compiler::reverse(fake_engine(), project, {});
	const std::string defect = reversal.defects.empty() ? std::string() : reversal.defects[0].message;
	REQUIRE_MESSAGE(defect.empty(), defect);
	if (reversal.refused.empty()) {
		return reversal.texts.at("res://unit.pet");
	}
	std::string listed;
	for (const compiler::Diagnostic &refusal : reversal.refused) {
		listed += std::to_string(refusal.line) + ":" + std::to_string(refusal.column) + ": " + refusal.message + "\n";
	}
	return listed;
}

} // namespace

TEST_CASE("reverse conversion deletes pub, async, .detach(), .unwrap(), @late, @owner and @parent, and the ? of a class") {
	CHECK(reversed("@owner(Node2D)\nclass_name Unit\nextends Node\n\nsignal hit(target: Node?)\n@late var target: Node\n@late\npub var other: Node\nvar best: Array[Node?] = []\n\n\npub static func make() -> Unit:\n\treturn Unit.new()\n\n\npub async func wait(node: Node?) -> Node?:\n\tawait ready\n\tvar parent: Node = (node as Node2D).unwrap()\n\treturn get_parent().unwrap()\n\n\nfunc _ready() -> void:\n\twait(null).detach()\n\thit.emit(owner)\n") ==
			"class_name Unit\nextends Node\n\nsignal hit(target: Node)\nvar target: Node\nvar other: Node\nvar best: Array[Node] = []\n\n\nstatic func make() -> Unit:\n\treturn Unit.new()\n\n\nfunc wait(node: Node) -> Node:\n\tawait ready\n\tvar parent: Node = (node as Node2D)\n\treturn get_parent()\n\n\nfunc _ready() -> void:\n\twait(null)\n\thit.emit(owner)\n");
}

// GDScript holds a value type's T? as an untyped Variant, its None as null
// (spec §10.1).
TEST_CASE("reverse conversion leaves a value type's T? untyped, writes None as null and Some(v) as v, in parentheses where it is an operand") {
	CHECK(reversed("extends Node\nvar hp: int? = None\nvar armor := Some(2)\n\n\nfunc heal(by: int?, times := 1) -> int?:\n\tvar total: int? = Some(by.unwrap() * times)\n\tif total == None:\n\t\treturn None\n\treturn Some(by.unwrap() + 1).unwrap() * 2\n") ==
			"extends Node\nvar hp = null\nvar armor = 2\n\n\nfunc heal(by, times := 1):\n\tvar total = by * times\n\tif total == null:\n\t\treturn null\n\treturn (by + 1) * 2\n");
}

TEST_CASE("reverse conversion writes PackedScene[T] as PackedScene and drops this language's names from @warning_ignore") {
	CHECK(reversed("extends Node\n@export var scene: PackedScene[Node2D]\n@export var scenes: Array[PackedScene[Node2D]] = []\n\n\n@warning_ignore(\"await_in_assignment\")\nfunc f() -> void:\n\t@warning_ignore(\"unused_variable\", \"modified_copy\")\n\tvar text := \"\"\"a\nb\"\"\"\n") ==
			"extends Node\n@export var scene: PackedScene\n@export var scenes: Array[PackedScene] = []\n\n\nfunc f() -> void:\n\t@warning_ignore(\"unused_variable\")\n\tvar text := \"\"\"a\nb\"\"\"\n");
}

TEST_CASE("a file that uses what GDScript lacks stays, with each place listed") {
	CHECK(reversed("extends Node\nstruct Hit:\n\tvar damage: int\n\nenum Command { IDLE, MOVE(target: Vector2) }\nvar twice: Node??\n\n\nfunc parse(text: String) -> Result[int, ConversionError]:\n\tvar n := int.try_from(text)?\n\treturn Ok(n)\n") ==
			"2:8: GDScript has no struct.\n5:6: GDScript has no enum with payloads.\n6:12: GDScript has no T?? or Variant?, whose two Nones are one null there.\n9:29: GDScript has no Result.\n10:11: GDScript has no Result.\n10:15: GDScript has no try_from().\n10:29: GDScript has no ? operator.\n11:9: GDScript has no Result.\n");
	CHECK(reversed("extends Node\n\n\nasync func load_all() -> int:\n\tawait ready\n\treturn 1\n\n\nfunc f() -> void:\n\tvar task := load_all()\n\ttask.cancel()\n") == "10:6: GDScript has no Task value.\n11:7: GDScript has no Task value.\n");
}

// Architecture §6.6: on a name, each is a conditional expression; expect()
// that a variable or a name takes is an assert after it.
TEST_CASE("reverse conversion writes the methods of T? on a name as conditional expressions, and expect() as an assert") {
	CHECK(reversed("extends Node\nvar saved: Node?\n\n\nfunc f(hp: int?, target: Node?, pick: func(Node) -> String) -> void:\n\tvar a: int = hp.unwrap_or(0) + 1\n\tvar b: Node = target.unwrap_or(self)\n\tvar c = target.map(pick)\n\tvar d = target.filter(func(n: Node) -> bool: return n.name == \"a\")\n\tprint(hp.unwrap_or_else(func() -> int: return 2), a, b, c, d)\n\tvar node := target.expect(\"no node\")\n\tsaved = target.expect(\"none saved\")\n\tprint(node)\n") ==
			"extends Node\nvar saved: Node\n\n\nfunc f(hp, target: Node, pick: Callable) -> void:\n\tvar a: int = (hp if hp != null else 0) + 1\n\tvar b: Node = target if target != null else self\n\tvar c = pick.call(target) if target != null else null\n\tvar d = target if target != null and (func(n: Node) -> bool: return n.name == \"a\").call(target) else null\n\tprint(hp if hp != null else (func() -> int: return 2).call(), a, b, c, d)\n\tvar node := target\n\tassert(node != null, \"no node\")\n\tsaved = target\n\tassert(saved != null, \"none saved\")\n\tprint(node)\n");
	CHECK(reversed("extends Node\n\n\nfunc f(n: Node?, hp: int?) -> void:\n\tprint(n.expect(\"none\"), get_parent().unwrap_or(n.unwrap()))\n\tvar total := hp.unwrap() + 1\n") ==
			"5:10: This is not converted back yet: expect() but as the whole value of a variable declared or a name assigned.\n5:39: This is not converted back yet: unwrap_or() of T? on more than a name.\n6:6: This is not converted back yet: `:=` of a value that GDScript reads untyped, here int.\n");
}

// Spec §9.6: GDScript gives an Array.
TEST_CASE("reverse conversion reads what awaiting a signal with several parameters gives by index") {
	CHECK(reversed("extends Node\nsignal done(code: int, body: String)\n\n\nasync func f() -> Array:\n\tvar args = await done\n\tprint(args.body)\n\treturn Array(args)\n") == "extends Node\nsignal done(code: int, body: String)\n\n\nfunc f() -> Array:\n\tvar args = await done\n\tprint(args[1])\n\treturn args\n");
}

// Spec §3.11, §10.1, §3.8; GDScript casts an int to an enum unchecked.
TEST_CASE("reverse conversion writes the types GDScript has for nested containers and function types, and an int as an enum by a cast") {
	CHECK(reversed("extends Node\nenum State { IDLE, RUN }\nvar grid: Array[Array[int]] = []\nvar calls: Array[func()] = []\nvar scores: Dictionary[String, int?] = {}\n\n\nfunc f(n: int, pick: func(int) -> bool) -> void:\n\tvar state := State(n + 1)\n\tvar row: Array[int] = grid[0]\n\tprint(State(n) == state, Array(calls), row, pick, Dictionary(scores))\n") ==
			"extends Node\nenum State { IDLE, RUN }\nvar grid: Array[Array] = []\nvar calls: Array[Callable] = []\nvar scores: Dictionary = {}\n\n\nfunc f(n: int, pick: Callable) -> void:\n\tvar state := (n + 1) as State\n\tvar row: Array[int] = grid[0]\n\tprint((n as State) == state, Array(calls, TYPE_NIL, &\"\", null), row, pick, Dictionary(scores, TYPE_NIL, &\"\", null, TYPE_NIL, &\"\", null))\n");
	CHECK(reversed("extends Node\nvar grid: Array[Array[int]] = []\nvar nodes: Array[Node] = []\n\n\nfunc f() -> void:\n\tgrid.append([1])\n\tvar other = self\n\tother.nodes = []\n") == "extends Node\nvar grid: Array[Array] = []\nvar nodes: Array[Node] = []\n\n\nfunc f() -> void:\n\tgrid.append([1] as Array[int])\n\tvar other = self\n\tother.nodes = [] as Array[Node]\n");
	CHECK(reversed("extends Node\nvar grid: Array[Array[int]] = []\n\n\nfunc f() -> void:\n\tvar cell := grid[0][0]\n\tfor row in grid:\n\t\tvar first := row[0]\n") ==
			"6:6: This is not converted back yet: `:=` of a value that GDScript reads untyped, here int.\n8:7: This is not converted back yet: `:=` of a value that GDScript reads untyped, here int.\n");
}

// Spec §6.4, §5.6, §10.1.
TEST_CASE("reverse conversion assigns members passed by name after the call, matches Some(p) as p after None, and exports a value type's T? as Variant") {
	CHECK(reversed("class_name Unit\nextends Node\n@export var hp: int?\nvar target: Node\n\n\nfunc f(other: Node?) -> void:\n\tvar unit := Unit.new(target = self)\n\tmatch other:\n\t\tNone:\n\t\t\tpass\n\t\tSome(var o):\n\t\t\tprint(o, unit)\n\tmatch hp:\n\t\tSome(3):\n\t\t\tpass\n\t\t_:\n\t\t\tpass\n") ==
			"class_name Unit\nextends Node\n@export var hp: Variant\nvar target: Node\n\n\nfunc f(other: Node) -> void:\n\tvar unit := Unit.new()\n\tunit.target = self\n\tmatch other:\n\t\tvar _none when not is_instance_valid(_none):\n\t\t\tpass\n\t\tvar o:\n\t\t\tprint(o, unit)\n\tmatch hp:\n\t\t3:\n\t\t\tpass\n\t\t_:\n\t\t\tpass\n");
	CHECK(reversed("extends Node\n\n\nfunc f(other: Node?) -> void:\n\tmatch other:\n\t\tSome(_):\n\t\t\tpass\n\t\tNone:\n\t\t\tpass\n") == "6:3: This is not converted back yet: Some(_) or Some(None), which would take null too, before a branch of None.\n");
}

// GDScript counts a match as returning only with a branch that takes any
// value, this language also when it is exhaustive (spec §5.6).
TEST_CASE("a function that returns through an exhaustive match without _ stays") {
	CHECK(reversed("extends Node\nenum Mode { CALM, ANGRY }\n\n\nfunc name_of(mode: Mode) -> String:\n\tmatch mode:\n\t\tMode.CALM:\n\t\t\treturn \"calm\"\n\t\tMode.ANGRY:\n\t\t\treturn \"angry\"\n") == "6:2: This is not converted back yet: a match that returns on every path without _, which GDScript does not count as returning.\n");
}

// GDScript finds an outer class's static members only through its name, and
// a Callable's call() gives Variant.
TEST_CASE("a file stays whose GDScript would not find an outer class's statics, or could not infer what a function value returns") {
	CHECK(reversed("extends Node\nstatic var log: Array[String] = []\n\n\nstatic func doubled(x: int) -> int:\n\treturn x * 2\n\n\nclass Inner:\n\tstatic func f(make: func(int) -> int) -> void:\n\t\tlog.append(\"a\")\n\t\tprint(doubled(1))\n\t\tvar made := make.call(1)\n") ==
			"11:3: This is not converted back yet: a static member of an outer class by its name alone, which GDScript does not find.\n12:9: This is not converted back yet: a static member of an outer class by its name alone, which GDScript does not find.\n13:7: This is not converted back yet: `:=` of a value that GDScript reads untyped, here int.\n");
}

// GDScript resolves what a static variable's initializer reads as it
// resolves the variable, and cannot where that comes back to it; this
// language reports the cycle as it runs (spec §4.8).
TEST_CASE("the files stay whose static initializers read each other") {
	FakeProject project;
	project.add_script("res://a.pet", "class_name A\nextends RefCounted\npub static var x: int = B.y + 1\npub static var z: int = B.y\n", "A");
	project.add_script("res://b.pet", "class_name B\nextends RefCounted\npub static var y: int = A.x + 1\n", "B");
	const compiler::Reversal reversal = compiler::reverse(fake_engine(), project, {});
	CHECK(reversal.texts.empty());
	std::string listed;
	for (const compiler::Diagnostic &refusal : reversal.refused) {
		listed += refusal.file + ":" + std::to_string(refusal.line) + ":" + std::to_string(refusal.column) + ": " + refusal.message + "\n";
	}
	CHECK(listed == "res://a.pet:3:27: GDScript has no cycle of static initializers, as through B.y here.\nres://b.pet:3:27: GDScript has no cycle of static initializers, as through A.x here.\n");
}

// GDScript tries the guard after the pattern and goes on to the next branch
// when it fails, as this language goes past Some(var x) for None.
TEST_CASE("reverse conversion guards Some(var x) that comes before None as var x when x != null") {
	CHECK(reversed("extends Node\n\n\nfunc f(other: Node?, hp: int?) -> void:\n\tmatch other:\n\t\tSome(var o):\n\t\t\tprint(o)\n\t\tNone:\n\t\t\tpass\n\tmatch hp:\n\t\tSome(var h) when h > 2 or h < 0:\n\t\t\tprint(h)\n\t\t_:\n\t\t\tpass\n") ==
			"extends Node\n\n\nfunc f(other: Node, hp) -> void:\n\tmatch other:\n\t\tvar o when o != null:\n\t\t\tprint(o)\n\t\tvar _none when not is_instance_valid(_none):\n\t\t\tpass\n\tmatch hp:\n\t\tvar h when h != null and (h > 2 or h < 0):\n\t\t\tprint(h)\n\t\t_:\n\t\t\tpass\n");
}

// Spec §3.3: a freed object reads as None, but GDScript's null pattern tests
// the type first.
TEST_CASE("reverse conversion matches None of an object as a bind that is no valid instance, and refuses it inside a pattern") {
	CHECK(reversed("extends Node\n\n\nfunc f(n: Node?, hp: int?) -> void:\n\tmatch n:\n\t\tnull when hp == None:\n\t\t\tpass\n\t\t_:\n\t\t\tpass\n\tmatch hp:\n\t\tNone:\n\t\t\tpass\n\t\t_:\n\t\t\tpass\n") ==
			"extends Node\n\n\nfunc f(n: Node, hp) -> void:\n\tmatch n:\n\t\tvar _none when not is_instance_valid(_none) and (hp == null):\n\t\t\tpass\n\t\t_:\n\t\t\tpass\n\tmatch hp:\n\t\tnull:\n\t\t\tpass\n\t\t_:\n\t\t\tpass\n");
	CHECK(reversed("extends Node\n\n\nfunc f(pair: Array[Node?]) -> void:\n\tmatch pair:\n\t\t[None, _]:\n\t\t\tpass\n\t\t_:\n\t\t\tpass\n") == "6:4: This is not converted back yet: None of an object inside a pattern or beside another, which GDScript would not match to a freed object.\n");
}

// Architecture §6.6: what the migration writes for an implicit downcast goes
// back as the value where GDScript checks it, as the original did.
TEST_CASE("reverse conversion writes a downcast back as the value where it goes to a place of its type") {
	CHECK(reversed("extends Node\nvar label: Label = (get_child(0) as Label).unwrap()\n\n\nfunc take(n: Node2D) -> void:\n\tpass\n\n\nfunc f(n: Node) -> Node2D:\n\tvar shown: Label = (n as Label).unwrap()\n\tlabel = (n as Label).unwrap()\n\ttake((n as Node2D).unwrap())\n\tvar other: Control = (n as Label).unwrap()\n\tvar inferred := (n as Node2D).unwrap()\n\tprint((n as Node2D).unwrap(), shown, other, inferred)\n\treturn (n as Node2D).unwrap()\n\n\nfunc g(o: Node?) -> void:\n\tvar near: Node2D? = (o as Node2D).unwrap() if o != None else None\n\tprint(near)\n") ==
			"extends Node\nvar label: Label = get_child(0)\n\n\nfunc take(n: Node2D) -> void:\n\tpass\n\n\nfunc f(n: Node) -> Node2D:\n\tvar shown: Label = n\n\tlabel = n\n\ttake(n)\n\tvar other: Control = (n as Label)\n\tvar inferred := (n as Node2D)\n\tprint((n as Node2D), shown, other, inferred)\n\treturn n\n\n\nfunc g(o: Node) -> void:\n\tvar near: Node2D = o\n\tprint(near)\n");
	// As godot-tiny-mmo's arrow sets its arc's source (source/common/gameplay/items/weapons/bow/arrow.gd:199).
	CHECK(reversed("extends Node\n\n\nfunc f(n: Node, arc: Arc) -> void:\n\tarc.source = (n as Label).unwrap()\n\tarc.kept = (n as Label).unwrap()\n\n\nclass Arc:\n\tvar source: Label\n\tvar kept = Label.new()\n") ==
			"extends Node\n\n\nfunc f(n: Node, arc: Arc) -> void:\n\tarc.source = n\n\tarc.kept = (n as Label)\n\n\nclass Arc:\n\tvar source: Label\n\tvar kept = Label.new()\n");
}

// Spec §3.4: the argument of a method is evaluated whether the receiver holds
// a value or not, and GDScript's conditional expression evaluates one side.
TEST_CASE("the methods of T? stay when GDScript would skip an argument that may have effects") {
	CHECK(reversed("extends Node\nconst MAX = 9\nenum State { IDLE, RUN }\n\n\nfunc spawn() -> Node:\n\treturn Node.new()\n\n\nfunc named(n: Node) -> String:\n\treturn n.name\n\n\nfunc f(hp: int?, n: Node?, by: int, items: Array[int]) -> void:\n\tprint(hp.unwrap_or(-by * MAX), hp.unwrap_or(State.RUN), hp.or(Some(by)), n.map(named))\n\tprint(n.unwrap_or(spawn()))\n\tprint(hp.unwrap_or(by / 2))\n\tprint(hp.unwrap_or(items[0]))\n") ==
			"16:10: This is not converted back yet: unwrap_or() of T? whose argument may have effects, which GDScript would evaluate only where it uses it.\n17:11: This is not converted back yet: unwrap_or() of T? whose argument may have effects, which GDScript would evaluate only where it uses it.\n18:11: This is not converted back yet: unwrap_or() of T? whose argument may have effects, which GDScript would evaluate only where it uses it.\n");
}

TEST_CASE("converting a class takes its inheritance tree, which a file that cannot convert keeps, and lists the errors it causes in the files that stay, which see GDScript") {
	FakeProject project;
	project.add_script("res://base.pet", "class_name Base\nextends Node\nenum Kind { A, B }\n\n\npub func hit() -> void:\n\tpass\n", "Base");
	project.add_script("res://child.pet", "extends Base\nconst Hud = preload(\"res://hud.pet\")\n");
	project.add_script("res://hud.pet", "extends Node\nvar kind: Base.Kind\n");
	compiler::Reversal reversal = compiler::reverse(fake_engine(), project, { "res://child.pet" });
	CHECK(reversal.texts == std::map<std::string, std::string>{ { "res://base.pet", "class_name Base\nextends Node\nenum Kind { A, B }\n\n\nfunc hit() -> void:\n\tpass\n" }, { "res://child.pet", "extends Base\nconst Hud = preload(\"res://hud.pet\")\n" } });
	CHECK(reversal.added == Pairs{ { "res://base.pet", "in the inheritance tree of res://child.pet" } });
	REQUIRE(reversal.caused.size() == 1);
	CHECK(reversal.caused[0].file == "res://hud.pet");
	CHECK(reversal.caused[0].line == 2);

	project.add_script("res://boss.pet", "extends \"res://child.pet\"\nstruct Loot:\n\tvar gold: int\n");
	reversal = compiler::reverse(fake_engine(), project, { "res://base.pet", "res://none" });
	CHECK(reversal.texts.empty());
	CHECK(reversal.left_out == Pairs{ { "res://base.pet", "in the inheritance tree of res://boss.pet, which stays" }, { "res://boss.pet", "it cannot convert, as listed" }, { "res://child.pet", "in the inheritance tree of res://boss.pet, which stays" } });
	REQUIRE(reversal.refused.size() == 1);
	CHECK(reversal.refused[0].file == "res://boss.pet");
	CHECK(reversal.unknown == std::vector<std::string>{ "res://none" });
}

// As the FPS kit's scenes/main.tscn connects a signal to scripts/hud.pet.
TEST_CASE("an error that a check of a scene reports keeps the file") {
	FakeProject project;
	project.add_script("res://player.pet", "extends Node\npub signal hit(target: Node)\n");
	project.add_script("res://hud.pet", "extends Node\n\n\npub func _on_hit(target: Node, times: int) -> void:\n\tprint(target, times)\n");
	compiler::Scene scene;
	scene.path = "res://main.tscn";
	scene.nodes = { { ".", "Node", "res://hud.pet" }, { "Player", "Node", "res://player.pet" } };
	scene.connections = { { "hit", "Player", ".", "_on_hit" } };
	project.scenes.push_back(scene);
	const compiler::Reversal reversal = compiler::reverse(fake_engine(), project, { "res://hud.pet" });
	CHECK(reversal.texts.empty());
	REQUIRE(reversal.refused.size() == 1);
	CHECK(reversal.refused[0].line == 4);
	CHECK(contains(reversal.refused[0].message, "res://main.tscn"));
}

// GDScript cannot use an enum of a class of this language (spec §11.3).
TEST_CASE("a file converted takes the trees whose enums and static methods it uses, or stays with them") {
	FakeProject project;
	project.add_script("res://base.pet", "class_name Base\nextends Node\npub enum Kind { A, B }\n", "Base");
	project.add_script("res://loot.pet", "class_name Loot\nextends Node\npub enum Rarity { COMMON }\nstruct Drop:\n\tvar gold: int\n", "Loot");
	project.add_script("res://user.pet", "extends Node\nvar kind := Base.Kind.A\n");
	project.add_script("res://shop.pet", "extends Node\nvar rarity: Loot.Rarity\n");
	const compiler::Reversal reversal = compiler::reverse(fake_engine(), project, { "res://user.pet", "res://shop.pet" });
	CHECK(reversal.added == Pairs{ { "res://base.pet", "res://user.pet uses Base.Kind of it" } });
	CHECK(reversal.left_out == Pairs{ { "res://shop.pet", "it uses Loot.Rarity of res://loot.pet, which stays" } });
	CHECK(reversal.texts.size() == 2);
}

TEST_CASE("the paths of the files converted are renamed in their scripts") {
	FakeProject project;
	project.add_script("res://a.pet", "extends Node\nconst B = preload(\"b.pet\")\nconst C = preload(\"res://c.pet\")\n");
	project.add_script("res://b.pet", "extends Node\n");
	project.add_script("res://c.pet", "extends Node\nstruct S:\n\tvar x: int\n");
	project.add_script("res://d.pet", "extends \"b.pet\"\n");
	const compiler::Reversal reversal = compiler::reverse(fake_engine(), project, { "res://a.pet", "res://d.pet" });
	CHECK(reversal.texts.at("res://a.pet") == "extends Node\nconst B = preload(\"b.gd\")\nconst C = preload(\"res://c.pet\")\n");
	CHECK(reversal.texts.at("res://d.pet") == "extends \"b.gd\"\n");
	CHECK(reversal.defects.empty());
}

// Erasing tokens may let their neighbours parse anew, as `1 + 2 as number /
// 3` became `1 + 2 / 3` in TypeScript (TypeScript#63533).
TEST_CASE("the re-parse check finds GDScript whose tree differs from the file's without what was erased") {
	FakeProject project;
	const std::string source = "extends Node\n\n\nfunc f(a: int, b: int) -> int?:\n\treturn Some(a + b).unwrap() * 2\n";
	project.add_script("res://unit.pet", source);
	const compiler::Reversal reversal = compiler::reverse(fake_engine(), project, {});
	const std::string written = reversal.texts.at("res://unit.pet");
	CHECK(written == "extends Node\n\n\nfunc f(a: int, b: int):\n\treturn (a + b) * 2\n");
	const compiler::Erased &erased = reversal.erased.at("res://unit.pet");
	CHECK(compiler::reparse_difference("res://unit.pet", source, erased, written, {}).empty());
	const std::string unparenthesized = "extends Node\n\n\nfunc f(a: int, b: int):\n\treturn a + b * 2\n";
	CHECK(contains(compiler::reparse_difference("res://unit.pet", source, erased, unparenthesized, {}), "at line 5"));
	CHECK(contains(compiler::reparse_difference("res://unit.pet", source, erased, "extends Node\n\n\nfunc f(a: int, b: int):\n\treturn (a + b\n", {}), "does not parse"));
}

// What the forward migration writes comes back as the GDScript it came from,
// but for the strings it wrote in triple quotes and the `_: pass` it added
// (architecture §6.6).
TEST_CASE("GDScript migrated and converted back is the original, but for the known rewrites") {
	FakeProject project;
	const std::string unit = "class_name Unit\nextends Node\nenum Type { A, B }\nvar target: Node\n\n\nfunc heal() -> void:\n\tpass\n\n\nfunc wait() -> void:\n\tawait ready\n\tvar parent: Node = get_parent()\n\tprint(parent, \"a\nb\")\n\n\nfunc pick(type: Type) -> void:\n\tmatch type:\n\t\tType.A:\n\t\t\twait()\n";
	const std::string user = "extends Node\n\n\nfunc f(unit: Unit) -> void:\n\tunit.heal()\n\tunit.target = self\n";
	project.add_script("res://unit.gd", unit, "Unit");
	project.add_script("res://user.gd", user);
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://unit.gd", "res://user.gd" }, 10);
	REQUIRE(migration.errors.empty());
	FakeProject migrated;
	migrated.add_script("res://unit.pet", migration.texts.at("res://unit.pet"), "Unit");
	migrated.add_script("res://user.pet", migration.texts.at("res://user.pet"));
	const compiler::Reversal reversal = compiler::reverse(fake_engine(), migrated, {});
	REQUIRE(reversal.refused.empty());
	CHECK(reversal.defects.empty());
	CHECK(reversal.texts.at("res://unit.pet") == "class_name Unit\nextends Node\nenum Type { A, B }\nvar target: Node\n\n\nfunc heal() -> void:\n\tpass\n\n\nfunc wait() -> void:\n\tawait ready\n\tvar parent: Node = get_parent()\n\tprint(parent, \"\"\"a\nb\"\"\")\n\n\nfunc pick(type: Type) -> void:\n\tmatch type:\n\t\tType.A:\n\t\t\twait()\n\t\t_: pass\n");
	CHECK(reversal.texts.at("res://user.pet") == user);
}
