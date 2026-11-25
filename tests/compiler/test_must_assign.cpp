#include "compiler/analysis.h"
#include "doctest.h"
#include "helpers.h"

using namespace petrichor;
using namespace petrichor::tests;

// Members that must be assigned (spec §4.3, §6.2, §6.4).

TEST_CASE("a member whose type has no default must be assigned, which is checked before it is read") {
	const char *source = "extends Node3D\n\nstruct Hit:\n\tvar damage: int\n\nenum Mode { AIM = 1, FIRE = 2 }\nenum Order { HOLD, MOVE(to: Vector3) }\n\nvar target: Node3D\nvar last: Hit\nvar mode: Mode\nvar order: Order\nvar on_fire: func(int)\nvar aimed: Node3D?\n\nfunc aim() -> float:\n\ttarget = Node3D.new()\n\treturn target.position.x\n\nfunc configure(hit: Hit, m: Mode, o: Order, f: func(int)) -> void:\n\tlast = hit\n\tmode = m\n\torder = o\n\ton_fire = f\n";
	const compiler::CompileResult result = compile(source);
	REQUIRE_MESSAGE(result.ok(), messages(result));
	for (const char *name : { "target", "last", "mode", "order", "on_fire" }) {
		CHECK_MESSAGE(member_named(result.module, name).must_assign, name);
	}
	CHECK(!member_named(result.module, "aimed").must_assign);
	// Marked when written, checked when read.
	const std::string aim = bytecode::disassemble(result.module, function_named(result.module, "aim"));
	REQUIRE(aim.find("MARK_ASSIGNED") != std::string::npos);
	CHECK(aim.find("CHECK_MUST") > aim.find("MARK_ASSIGNED"));
	// The node checks them when it becomes ready; a resource only when read.
	REQUIRE(result.module.ready_function >= 0);
	CHECK(contains(bytecode::disassemble(result.module, result.module.functions[result.module.ready_function]), "CHECK_PROVIDED"));
	CHECK(compile("extends Resource\nvar target: Node3D\n").module.ready_function < 0);
}

TEST_CASE("an @late member or static variable is checked when read, not when the node becomes ready (spec §6.3)") {
	const compiler::CompileResult result = compile("extends Node3D\n\n@late var target: Node3D\n@late static var shared: Node3D\n\nfunc aim() -> float:\n\ttarget = Node3D.new()\n\tshared = target\n\ttarget = shared\n\treturn target.position.x + shared.position.x\n");
	REQUIRE_MESSAGE(result.ok(), messages(result));
	const bytecode::Member &target = member_named(result.module, "target");
	CHECK((target.is_late && target.tracks_assignment() && !target.must_assign && !target.is_pub && !target.has_initial));
	REQUIRE(result.module.statics.size() == 1);
	CHECK(result.module.statics[0].is_late);
	const std::string aim = bytecode::disassemble(result.module, function_named(result.module, "aim"));
	CHECK(aim.find("CHECK_MUST") > aim.find("MARK_ASSIGNED"));
	CHECK(result.module.ready_function < 0);
	const auto errors = [](const std::string &p_members) { return messages(compile("extends Node\n\n" + p_members + "\n")); };
	CHECK(contains(errors("@late var target: Node3D\n\nfunc _init():\n\tprint(target)"), "_init reads \"target\" before assigning it: an @late member has a value only once code assigns it."));
	CHECK(errors("@late var target: Node3D\n\nfunc _init():\n\ttarget = Node3D.new()\n\tprint(target)").empty());
	CHECK_FALSE(errors("@late var target: Node3D\n\nfunc clear() -> void:\n\ttarget = None").empty());
	// A static variable without @late still needs a value.
	CHECK(contains(errors("static var shared: Node3D"), "needs an initial value"));
}

TEST_CASE("@late goes only on a member or static variable without an initializer whose type has no default value (spec §6.3)") {
	const auto errors = [](const std::string &p_members) { return messages(compile("extends Node\n\n" + p_members + "\n")); };
	CHECK(contains(errors("@late var target: Node = Node.new()"), "An @late member takes no initializer: later code assigns it."));
	CHECK(contains(errors("@late var count: int"), "int has a default value, which \"count\" starts at; @late is for a type without one."));
	CHECK(contains(errors("@late var target: Node?"), "Node? has a default value"));
	CHECK(contains(errors("@late static var count: int"), "int has a default value"));
	CHECK(contains(errors("@late @onready var target: Node = get_child(0)"), "@late and @onready do not go together"));
	CHECK(contains(errors("@late @onready var target: Node"), "@late and @onready do not go together"));
	CHECK_FALSE(errors("func f() -> void:\n\t@late var target: Node").empty());
	CHECK_FALSE(errors("struct Hit:\n\t@late var target: Node").empty());
	CHECK_FALSE(errors("interface Aimed:\n\t@late\n\tvar target: Node").empty());
	// A class implements an interface's property with one.
	CHECK(messages(compile("extends Node implements Aimed\n\ninterface Aimed:\n\tvar target: Node\n\n@late pub var target: Node\n")).empty());
}

TEST_CASE("an @late export is pub, and a scene that stores none and instantiate(...) leave it unassigned (spec §6.3, §7.5)") {
	const char *source = "extends Node3D\n@late @export var player: Node3D\n";
	const compiler::CompileResult result = compile(source);
	REQUIRE_MESSAGE(result.ok(), messages(result));
	CHECK((member_named(result.module, "player").is_pub && result.module.ready_function < 0));
	FakeProject project;
	compiler::Scene enemy;
	enemy.path = "res://enemy.tscn";
	enemy.nodes = { { ".", "Node3D", "res://test.pet" }, { "Gun", "Node3D", "res://test.pet" } };
	project.scenes = { enemy };
	project.main = enemy.path;
	CHECK(check_scenes_in(source, project).errors.empty());
	compiler::SceneValue null;
	null.type = "Nil";
	project.scenes[0].nodes[1].properties = { { "player", null } };
	CHECK(check_scenes_in(source, project).errors.empty());
	project.add_script("res://enemy.pet", source);
	project.scenes[0].nodes[0].script = "res://enemy.pet";
	CHECK(messages(compile_in("extends Node\n\nconst ENEMY = preload(\"res://enemy.tscn\")\n\nfunc spawn() -> void:\n\tadd_child(ENEMY.instantiate())\n", project)).empty());
}

TEST_CASE("a node below a scene's root, an instance of another scene too, gets from _init each member that must be assigned and is not exported (spec §6.2)") {
	FakeProject project;
	compiler::Scene bar;
	bar.path = "res://bar.tscn";
	bar.nodes = { { ".", "Node", "res://test.pet" } };
	compiler::Scene hud;
	hud.path = "res://hud.tscn";
	hud.nodes = { { ".", "Node", "" }, { "Bar", "Node", "res://test.pet" }, { "Placed", "Node", "", true, "res://test.pet" } };
	hud.nodes[2].instance = bar.path;
	project.scenes = { bar, hud };
	const compiler::CompileResult result = check_scenes_in("extends Node\n\nvar target: Node3D\n", project);
	REQUIRE_MESSAGE(result.errors.size() == 2, messages(result));
	CHECK(result.errors[0].message == "res://hud.tscn: Bar has no value for \"target\", which must be assigned and is not exported. Mark it @late if later code assigns it.");
	CHECK((result.errors[0].scene == "res://hud.tscn" && result.errors[0].node == "Bar"));
	CHECK(contains(result.errors[1].message, "res://hud.tscn: Placed has no value for \"target\""));
	// The game runs all the same: the node fails the check as it becomes ready.
	CHECK(result.ok());
	CHECK(check_scenes_in("extends Node\n\n@late var target: Node3D\n", project).errors.empty());
	CHECK(check_scenes_in("extends Node\n\nvar target: Node3D\n\nfunc _init():\n\ttarget = Node3D.new()\n", project).errors.empty());
}

TEST_CASE("_init reads a member that must be assigned only once it has assigned it on every path") {
	const auto init = [](const std::string &p_body) { return messages(compile("extends Node\n\nvar target: Node3D\n\nfunc _init():\n" + p_body + "\n\nfunc helper() -> void:\n\tprint(target)\n")); };
	const std::string read_first = init("\tprint(target)");
	CHECK(contains(read_first, "_init reads \"target\" before assigning it: a member that must be assigned gets its value from the scene or the creator only after _init."));
	// After both branches assign it; in a lambda, which runs later; through a call, at run time.
	CHECK(init("\tif randi() > 0:\n\t\ttarget = Node3D.new()\n\telse:\n\t\ttarget = Node3D.new()\n\tprint(target.position)").empty());
	CHECK(init("\tvar later := func(): print(target)\n\thelper()\n\ttarget = Node3D.new()").empty());
	CHECK(contains(init("\tif randi() > 0:\n\t\ttarget = Node3D.new()\n\tprint(target)"), "_init reads \"target\" before assigning it"));
	CHECK(contains(init("\twhile randi() > 0:\n\t\ttarget = Node3D.new()\n\tprint(target)"), "_init reads \"target\" before assigning it"));
	CHECK(contains(init("\ttarget.position = Vector3.ZERO"), "_init reads \"target\" before assigning it"));
}

TEST_CASE("super() in _init assigns what the base's _init assigns, from there on (spec §6.2)") {
	FakeProject project;
	project.add_script("res://base.pet", "class_name Base\nextends Node\n\npub var target: Node\n\nfunc _init():\n\ttarget = Node.new()\n", "Base");
	const auto errors = [&](const std::string &p_init) {
		return messages(compile_in("extends Base\n\nfunc _init():\n" + p_init + "\n", project));
	};
	CHECK(errors("\tsuper()\n\tprint(target)").empty());
	CHECK(errors("\tsuper._init()\n\tprint(target)").empty());
	CHECK(contains(errors("\tprint(target)\n\tsuper()"), "_init reads \"target\" before assigning it"));
	// Through classes of the same file, a subclass before its base.
	CHECK(errors("\tsuper()\n\nclass Sub extends Inner:\n\tfunc _init():\n\t\tsuper()\n\t\tprint(target)\n\nclass Inner extends Base:\n\tfunc _init():\n\t\tsuper()").empty());
}

TEST_CASE("the _init of two files that make each other's objects counts what each assigns (spec §6.2)") {
	FakeProject project;
	const auto source = [](const std::string &p_name, const std::string &p_other) {
		return "class_name " + p_name + "\nextends Node\n\npub var partner: Node\n\npub func _init():\n\tpartner = " + p_other + ".new()\n\tprint(" + p_other + ".new().partner)\n";
	};
	project.add_script("res://a.pet", source("A", "B"), "A");
	project.add_script("res://b.pet", source("B", "A"), "B");
	CHECK(messages(compile_in("extends Node\n\nfunc f(a: PackedScene[A], b: PackedScene[B]) -> void:\n\tadd_child(a.instantiate())\n\tadd_child(b.instantiate())\n", project)).empty());
	compiler::Workspace workspace(project);
	compiler::Analysis analysis(fake_engine(), workspace);
	const compiler::Analysis::Pass pass(analysis);
	for (const char *path : { "res://a.pet", "res://b.pet" }) {
		const std::string errors = messages(*analysis.compile(path, *project.script_source(path)));
		CHECK_MESSAGE(errors.empty(), errors);
	}
}

TEST_CASE("instantiate(...) and the scene leave out what the _init of the root's class assigns, also in another file (spec §6.2, §7.4)") {
	FakeProject project;
	project.add_script("res://armed.pet", "class_name Armed\nextends Node3D\n\npub var target: Node3D\n\nfunc _init():\n\ttarget = Node3D.new()\n", "Armed");
	project.add_script("res://kept.pet", "class_name Kept\nextends Armed\n", "Kept");
	project.add_script("res://redone.pet", "class_name Redone\nextends Armed\n\nfunc _init():\n\tpass\n", "Redone");
	compiler::Scene armed;
	armed.path = "res://armed.tscn";
	armed.nodes = { { ".", "Node3D", "res://armed.pet" } };
	compiler::Scene redone;
	redone.path = "res://redone.tscn";
	redone.nodes = { { ".", "Node3D", "res://redone.pet" } };
	project.scenes = { armed, redone };
	const auto errors = [&](const std::string &p_body) {
		return messages(compile_in("extends Node\n\nfunc f(a: PackedScene[Armed], k: PackedScene[Kept], r: PackedScene[Redone]) -> void:\n" + p_body + "\n", project));
	};
	CHECK(errors("\tadd_child(a.instantiate())\n\tadd_child(k.instantiate())\n\tvar s: PackedScene[Node3D] = preload(\"res://armed.tscn\")").empty());
	// An _init of its own runs instead.
	CHECK(contains(errors("\tadd_child(r.instantiate())"), "instantiate() needs target = ..., a member of Redone that must be assigned and the scene cannot."));
	CHECK(contains(errors("\tvar s: PackedScene[Armed] = preload(\"res://redone.tscn\")"), "res://redone.tscn leaves \"target\" to instantiate(), but a PackedScene[Armed] does not ask for it."));
	// A scene that a scene stores for an export.
	compiler::Scene level;
	level.path = "res://level.tscn";
	level.nodes = { { ".", "Node", "" }, { "Spawner", "Node", "res://test.pet" } };
	compiler::SceneValue stored;
	stored.type = "Object";
	stored.class_name = "PackedScene";
	stored.path = armed.path;
	level.nodes[1].properties = { { "wave", stored } };
	project.scenes.push_back(level);
	CHECK(check_scenes_in("extends Node\n@export pub var wave: PackedScene[Node3D]\n", project).errors.empty());
}

TEST_CASE("_init assigns what every path that leaves it has assigned, those that return too (spec §6.2)") {
	FakeProject project;
	const std::string head = "extends Node3D\n\npub var target: Node3D\n\nfunc _init():\n";
	project.add_script("res://early.pet", "class_name Early\n" + head + "\tif randi() > 0:\n\t\treturn\n\ttarget = Node3D.new()\n", "Early");
	project.add_script("res://last.pet", "class_name Last\n" + head + "\ttarget = Node3D.new()\n\treturn\n", "Last");
	const std::string errors = messages(compile_in("extends Node\n\nfunc f(e: PackedScene[Early], l: PackedScene[Last]) -> void:\n\tadd_child(e.instantiate())\n\tadd_child(l.instantiate())\n", project));
	CHECK(contains(errors, "instantiate() needs target = ..., a member of Early"));
	CHECK_FALSE(contains(errors, "Last"));
}

TEST_CASE("an export that the _init of a base in another file assigns needs no value from the scene (spec §6.2, §7.5)") {
	FakeProject project;
	project.add_script("res://archer.pet", "class_name Archer\nextends Node3D\n\n@export pub var bow: Node3D\n\nfunc _init():\n\tbow = Node3D.new()\n", "Archer");
	compiler::Scene level;
	level.path = "res://level.tscn";
	level.nodes = { { ".", "Node3D", "" }, { "Archer", "Node3D", "res://test.pet" } };
	project.scenes = { level };
	project.main = level.path;
	CHECK(check_scenes_in("extends Archer\n", project).errors.empty());
	CHECK(contains(messages(check_scenes_in("extends Archer\n\nfunc _init():\n\tpass\n", project)), "res://level.tscn: Archer has no value for \"bow\", an export that must be assigned."));
}

namespace {

std::string made_errors(const std::string &p_body) {
	FakeProject project;
	project.add_script("res://bullet.pet", "class_name Bullet\nextends Node3D\n\npub var target: Node3D\npub var speed := 1.0\n@export pub var trail: Node3D\n@late pub var shooter: Node3D\n", "Bullet");
	project.add_script("res://hidden.pet", "class_name Hidden\nextends Node\n\nvar secret: Node3D\n", "Hidden");
	return messages(compile_in("extends Node\n\nfunc f(scene: PackedScene[Bullet], hidden: PackedScene[Hidden]) -> void:\n" + p_body + "\n", project));
}

} // namespace

TEST_CASE("new(...) and instantiate(...) take the members that must be assigned by name") {
	const std::string made = made_errors("\tvar a := Bullet.new(target = Node3D.new(), trail = Node3D.new())\n\tvar b := scene.instantiate(target = Node3D.new(), trail = a)\n\tadd_child(a)\n\tadd_child(b)");
	CHECK_MESSAGE(made.empty(), made);
	// The scene may assign an export, but nothing else.
	CHECK(contains(made_errors("\tadd_child(scene.instantiate())"), "instantiate() needs target = ..., a member of Bullet that must be assigned and the scene cannot."));
	CHECK(contains(made_errors("\tadd_child(hidden.instantiate())"), "instantiate() cannot give \"secret\", a member of Hidden that must be assigned and is not pub."));
	CHECK(contains(made_errors("\tadd_child(Bullet.new(speed = 2.0))"), "\"speed\" has a value of its own; by name, new(...) takes only members that must be assigned."));
	CHECK(contains(made_errors("\tadd_child(scene.instantiate(target = Node3D.new(), trail = Node3D.new(), shooter = Node3D.new()))"), "\"shooter\" is @late; by name, instantiate(...) takes only members that must be assigned."));
	CHECK(contains(made_errors("\tadd_child(Hidden.new(secret = Node3D.new()))"), "\"secret\" of Hidden is not pub."));
	CHECK(contains(made_errors("\tadd_child(Bullet.new(nope = 1))"), "Bullet has no member \"nope\"."));
	CHECK(contains(made_errors("\tadd_child(Bullet.new(target = Node3D.new(), 1))"), "Arguments by position come before those by name."));
	CHECK(contains(made_errors("\tadd_child(Node.new(name = &\"x\"))"), "Only new(...) of a class of this language and instantiate(...) of a PackedScene[T] take members by name."));
}

TEST_CASE("new(...) of a class known statically gives each member that must be assigned and that _init does not assign (spec §6.2)") {
	FakeProject project;
	project.add_script("res://shot.pet", "class_name Shot\nextends Node3D\n\npub var target: Node3D\n", "Shot");
	project.add_script("res://missile.pet", "class_name Missile\nextends Shot\n\n@export pub var trail: Node3D\n@late pub var shooter: Node3D\nvar aimed: Node3D\n\npub func _init():\n\taimed = Node3D.new()\n", "Missile");
	const auto errors = [&](const std::string &p_body) {
		return messages(compile_in("extends Node\n\nconst MISSILE = preload(\"res://missile.pet\")\n\nfunc f() -> void:\n" + p_body + "\n", project));
	};
	const std::string given = errors("\tadd_child(Missile.new(target = Node3D.new(), trail = Node3D.new()))\n\tadd_child(MISSILE.new(target = Node3D.new(), trail = Node3D.new()))");
	CHECK_MESSAGE(given.empty(), given);
	const std::string left = errors("\tadd_child(MISSILE.new())");
	CHECK(contains(left, "new() needs target = ..., a member of Missile that must be assigned."));
	CHECK(contains(left, "new() needs trail = ..., an export of Missile that must be assigned."));
	CHECK_FALSE(contains(left, "shooter"));
	CHECK_FALSE(contains(left, "aimed"));
}
