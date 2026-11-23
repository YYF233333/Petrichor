#include "compiler/migration.h"
#include "doctest.h"
#include "helpers.h"

using namespace petrichor;
using namespace petrichor::tests;

// The migration tool (architecture §6.6, spec §13.4) in memory: the files it
// takes, the project as they will be once renamed, and the loop of fixes.

namespace {

using Pairs = std::vector<std::pair<std::string, std::string>>;

// A base with a static function, an enum and a constant; its subclasses, by
// class name and by path; GDScript files that use it at class level, and one
// that only reads its constant; a @tool script and its subclass.
FakeProject gdscript_project() {
	FakeProject project;
	project.add_script("res://base.gd", "class_name Base\nextends Node\nenum Kind { A, B }\nconst N := 1\n\n\nstatic func make() -> Node:\n\treturn null\n", "Base");
	project.add_script("res://child.gd", "extends Base\n");
	project.add_script("res://other.gd", "extends \"child.gd\"\n");
	project.add_script("res://maker.gd", "extends Node\n\n\nfunc f() -> void:\n\tBase.make()\n");
	project.add_script("res://typer.gd", "extends Node\n\n\nfunc f(kind: Base.Kind) -> void:\n\tpass\n");
	project.add_script("res://counter.gd", "extends Node\nconst M = Base.N + 1\n");
	project.add_script("res://reader.gd", "extends Node\n\n\nfunc f() -> int:\n\treturn Base.N + Base.Kind.A\n");
	project.add_script("res://tool.gd", "@tool\nextends Node\n");
	project.add_script("res://tool_child.gd", "extends \"res://tool.gd\"\n");
	return project;
}

} // namespace

TEST_CASE("a migration takes the inheritance tree and the files that GDScript could no longer keep") {
	const FakeProject project = gdscript_project();
	const compiler::MigrationSet taken = compiler::migration_set(fake_engine(), project, { "res://child.gd" });
	CHECK(taken.files == std::set<std::string>{ "res://base.gd", "res://child.gd", "res://counter.gd", "res://maker.gd", "res://other.gd", "res://typer.gd" });
	CHECK(taken.added == Pairs{
								 { "res://base.gd", "in the inheritance tree of res://child.gd" },
								 { "res://counter.gd", "uses Base.N of res://base.gd in a constant expression" },
								 { "res://maker.gd", "calls the static method make() of res://base.gd" },
								 { "res://other.gd", "in the inheritance tree of res://child.gd" },
								 { "res://typer.gd", "uses Base.Kind of res://base.gd as a type" },
						 });
	CHECK(taken.left_out.empty());
	CHECK(taken.unknown.empty());
}

TEST_CASE("a migration leaves out the trees with @tool, and what a file left out uses at class level") {
	FakeProject project = gdscript_project();
	const compiler::MigrationSet tool = compiler::migration_set(fake_engine(), project, { "res://tool_child.gd", "res://none" });
	CHECK(tool.files.empty());
	CHECK(tool.left_out == Pairs{
								   { "res://tool.gd", "it has @tool" },
								   { "res://tool_child.gd", "in the inheritance tree of res://tool.gd, which has @tool" },
						   });
	CHECK(tool.unknown == std::vector<std::string>{ "res://none" });

	project.add_script("res://inspector.gd", "@tool\nextends Node\nvar kind: Base.Kind\n");
	const compiler::MigrationSet base = compiler::migration_set(fake_engine(), project, { "res://maker.gd", "res://other.gd" });
	CHECK(base.files == std::set<std::string>{ "res://maker.gd" });
	CHECK(base.left_out == Pairs{
								   { "res://base.gd", "res://inspector.gd, left out, uses Base.Kind of it as a type" },
								   { "res://child.gd", "in the inheritance tree of res://base.gd, left out" },
								   { "res://other.gd", "in the inheritance tree of res://base.gd, left out" },
						   });
}

TEST_CASE("a migration of the whole project, and of a directory, takes every GDScript file there") {
	FakeProject project;
	project.add_script("res://a/one.gd", "extends Node\n");
	project.add_script("res://a/two.gd", "extends Node\n");
	project.add_script("res://ab.gd", "extends Node\n");
	project.add_script("res://kept.pet", "extends Node\n");
	CHECK(compiler::migration_set(fake_engine(), project, {}).files == std::set<std::string>{ "res://a/one.gd", "res://a/two.gd", "res://ab.gd" });
	CHECK(compiler::migration_set(fake_engine(), project, { "res://a" }).files == std::set<std::string>{ "res://a/one.gd", "res://a/two.gd" });
}

TEST_CASE("a migration leaves out the script templates, from which the editor makes new scripts") {
	FakeProject project;
	project.add_script("res://templates/Node/greet.gd", "extends _BASE_\n");
	project.add_script("res://main.gd", "extends Node\n");
	const Pairs left_out = { { "res://templates/Node/greet.gd", "a script template, from which the editor makes new scripts" } };
	const compiler::MigrationSet whole = compiler::migration_set(fake_engine(), project, {}, "res://templates");
	CHECK(whole.files == std::set<std::string>{ "res://main.gd" });
	CHECK(whole.left_out == left_out);
	const compiler::MigrationSet asked = compiler::migration_set(fake_engine(), project, { "res://templates" }, "res://templates");
	CHECK(asked.files.empty());
	CHECK(asked.left_out == left_out);
	CHECK(asked.unknown.empty());
}

TEST_CASE("the paths of files migrated are renamed in scripts and in scenes") {
	const std::map<std::string, std::string> renamed = { { "res://lib/a.gd", "res://lib/a.pet" } };
	int count = 0;
	const std::string script = "const A = preload(\"res://lib/a.gd\")\nconst B = preload('a.gd')\nconst C = \"res://lib/a.gd.bak\"\n# \"res://lib/a.gd\"\n";
	CHECK(compiler::renamed_in_script(script, "res://lib/b.gd", renamed, count) == "const A = preload(\"res://lib/a.pet\")\nconst B = preload('a.pet')\nconst C = \"res://lib/a.gd.bak\"\n# \"res://lib/a.gd\"\n");
	CHECK(count == 2);
	count = 0;
	const std::string scene = "[ext_resource type=\"Script\" uid=\"uid://x\" path=\"res://lib/a.gd\" id=\"1\"]\n[node name=\"A\" type=\"Node\"]\nscript = ExtResource(\"1\")\ntext = \"say \\\"res://lib/a.gd\\\"\"\n";
	CHECK(compiler::renamed_in_resource(scene, renamed, count) == "[ext_resource type=\"Script\" uid=\"uid://x\" path=\"res://lib/a.pet\" id=\"1\"]\n[node name=\"A\" type=\"Node\"]\nscript = ExtResource(\"1\")\ntext = \"say \\\"res://lib/a.gd\\\"\"\n");
	CHECK(count == 1);
	count = 0;
	CHECK(compiler::renamed_in_resource("[autoload]\n\nGame=\"*res://lib/a.gd\"\n", renamed, count) == "[autoload]\n\nGame=\"*res://lib/a.pet\"\n");
	CHECK(count == 1);
}

TEST_CASE("the project once migrated names the new paths") {
	FakeProject project;
	project.add_script("res://player.gd", "class_name Player\nextends Node\nconst Gun = preload(\"gun.gd\")\n", "Player");
	project.add_script("res://gun.gd", "extends Node\n");
	project.add_script("res://hud.gd", "extends Node\nconst P = preload(\"res://player.gd\")\n");
	project.autoloads["Game"] = "res://gun.gd";
	project.foreign["res://hud.gd"] = compiler::ForeignClass{};
	project.foreign["res://player.gd"] = compiler::ForeignClass{};
	project.loads["res://level.tscn"] = { "res://player.gd" };
	compiler::Scene level;
	level.path = "res://level.tscn";
	level.nodes.push_back({ ".", "Node", "res://player.gd::Inner" });
	project.scenes.push_back(level);
	const compiler::Migrated migrated(project, { "res://player.gd", "res://gun.gd" });
	CHECK(migrated.script_source("res://player.pet") == "class_name Player\nextends Node\nconst Gun = preload(\"gun.pet\")\n");
	CHECK(migrated.script_source("res://player.gd") == std::nullopt);
	CHECK(migrated.script_source("res://hud.gd") == "extends Node\nconst P = preload(\"res://player.pet\")\n");
	CHECK(migrated.global_class_path("Player") == "res://player.pet");
	CHECK(migrated.autoload_path("Game") == "res://gun.pet");
	CHECK(migrated.scene("res://level.tscn")->nodes[0].script == "res://player.pet::Inner");
	CHECK(migrated.dependencies("res://level.tscn") == std::vector<std::string>{ "res://player.pet" });
	CHECK(migrated.foreign_class("res://player.gd", false) == std::nullopt);
	CHECK(migrated.foreign_class("res://hud.gd", false).has_value());
	const std::vector<std::string> scripts = migrated.script_paths();
	CHECK(std::set<std::string>(scripts.begin(), scripts.end()) == std::set<std::string>{ "res://gun.pet", "res://hud.gd", "res://player.pet" });
}

TEST_CASE("a string in single or double quotes that spans lines is written in triple quotes, which GDScript reads the same") {
	FakeProject project;
	project.add_script("res://say.gd", "extends Node\n\n\nfunc _ready() -> void:\n\tprint(\"a\nb\", 'c\\'\nd', r\"e\\\"\nf\", &\"g\nh\", \"i\\\nj\")  # \"k\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://say.gd" }, 10);
	CHECK(migration.texts.at("res://say.pet") == "extends Node\n\n\nfunc _ready() -> void:\n\tprint(\"\"\"a\nb\"\"\", '''c\\'\nd''', r\"\"\"e\\\"\nf\"\"\", &\"\"\"g\nh\"\"\", \"i\\\nj\")  # \"k\n");
	CHECK(migration.quoted == 4);
	CHECK(migration.errors.empty());
}

TEST_CASE("the loop of fixes applies the meaning-keeping ones until none is left, but none outside the files migrated") {
	FakeProject project;
	project.add_script("res://unit.gd", "class_name Unit\nextends Node\n\n\nfunc heal() -> void:\n\tpass\n\n\nfunc wait() -> void:\n\tawait ready\n\tvar parent: Node = get_parent()\n\tprint(parent)\n", "Unit");
	project.add_script("res://user.gd", "extends Node\n\n\nfunc f(unit: Unit, other: Outside) -> void:\n\tunit.heal()\n\tother.help()\n");
	project.add_script("res://outside.pet", "class_name Outside\nextends Node\n\n\nfunc help() -> void:\n\tpass\n", "Outside");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://unit.gd", "res://user.gd" }, 10);
	CHECK(migration.settled);
	CHECK(migration.rounds == 1);
	CHECK(migration.texts.at("res://unit.pet") == "class_name Unit\nextends Node\n\n\npub func heal() -> void:\n\tpass\n\n\nasync func wait() -> void:\n\tawait ready\n\tvar parent: Node = get_parent().unwrap()\n\tprint(parent)\n");
	CHECK(migration.fixes == std::map<compiler::FixKind, int>{ { compiler::FixKind::Unwrap, 1 }, { compiler::FixKind::Async, 1 }, { compiler::FixKind::Pub, 1 } });
	REQUIRE(migration.errors.size() == 1);
	CHECK(migration.errors[0].file == "res://user.pet");
	CHECK(contains(migration.errors[0].message, "help"));
}

TEST_CASE("the loop of fixes makes an export that a scene or a resource leaves without a value a T?, as GDScript gives it null") {
	FakeProject project;
	project.add_script("res://stats.gd", "extends Resource\n@export var portrait: Texture2D\nvar holder: RefCounted\n\n\nfunc copy() -> Texture2D:\n\treturn portrait.duplicate()\n");
	project.add_script("res://hud.gd", "extends Node\n@export var icon: Texture2D\n");
	compiler::Scene hero;
	hero.path = "res://hero.tres";
	hero.resources = { { hero.path, "res://stats.gd", {} } };
	compiler::Scene main;
	main.path = "res://main.tscn";
	main.nodes = { { ".", "Node", "res://hud.gd" } };
	project.scenes = { hero, main };
	project.main = main.path;
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://stats.gd", "res://hud.gd" }, 10);
	CHECK(migration.texts.at("res://stats.pet") == "extends Resource\n@export var portrait: Texture2D?\nvar holder: RefCounted\n\n\nfunc copy() -> Texture2D:\n\treturn portrait.unwrap().duplicate()\n");
	CHECK(migration.texts.at("res://hud.pet") == "extends Node\n@export var icon: Texture2D?\n");
	CHECK(migration.fixes == std::map<compiler::FixKind, int>{ { compiler::FixKind::Unwrap, 1 }, { compiler::FixKind::Optional, 2 } });
	REQUIRE(migration.errors.size() == 1);
	CHECK(migration.errors[0].message == "res://hero.tres: the resource has no value for \"holder\", which must be assigned and is not exported.");
}

TEST_CASE("a literal path from the root to an autoload becomes its name, whose class the inference reads") {
	FakeProject project;
	project.add_script("res://audio.gd", "extends Node\n\n\nfunc play(sound):\n\tprint(sound)\n");
	project.autoloads["Audio"] = "res://audio.gd";
	project.add_script("res://hud.gd", "extends Node\n@onready var audio = $\"/root/Audio\"\n\n\nfunc _ready() -> void:\n\taudio.play(\"click\")\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://audio.gd", "res://hud.gd" }, 10);
	CHECK(migration.texts.at("res://audio.pet") == "extends Node\n\n\npub func play(sound: String):\n\tprint(sound)\n");
	CHECK(migration.texts.at("res://hud.pet") == "extends Node\n@onready var audio = Audio\n\n\nfunc _ready() -> void:\n\taudio.play(\"click\")\n");
	CHECK(migration.by_name == 0);
	CHECK(migration.fixes == std::map<compiler::FixKind, int>{ { compiler::FixKind::Pub, 1 }, { compiler::FixKind::Autoload, 1 } });
	CHECK(migration.errors.empty());
}

TEST_CASE("a migration types each slot without a type by the values that flow into it") {
	FakeProject project;
	project.add_script("res://unit.gd", "class_name Unit\nextends Node\nsignal hit(amount)\nvar label = null\n\n\nfunc heal(amount, times = 2):\n\treturn amount * times\n\n\nfunc _ready() -> void:\n\tvar count\n\tcount = heal(3)\n\thit.emit(count)\n\tlabel = Label.new()\n", "Unit");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://unit.gd" }, 10);
	CHECK(migration.texts.at("res://unit.pet") == "class_name Unit\nextends Node\nsignal hit(amount: int)\nvar label: Label? = null\n\n\nfunc heal(amount: int, times: int = 2) -> int:\n\treturn amount * times\n\n\nfunc _ready() -> void:\n\tvar count: int\n\tcount = heal(3)\n\thit.emit(count)\n\tlabel = Label.new()\n");
	CHECK(migration.typed == 6);
	CHECK(migration.errors.empty());
}

TEST_CASE("a migration merges what flows into a slot: classes to their nearest common base, None to T?, and the rest to Variant") {
	FakeProject project;
	project.add_script("res://ui.gd", "extends Node\n\n\nfunc show_it(control, tag, value):\n\tprint(control, tag, value)\n\n\nfunc _ready() -> void:\n\tshow_it(Label.new(), null, 1)\n\tshow_it(TextureRect.new(), \"b\", 2.5)\n\tvar named\n\tnamed = \"x\"\n\tnamed = &\"y\"\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://ui.gd" }, 10);
	CHECK(migration.texts.at("res://ui.pet") == "extends Node\n\n\nfunc show_it(control: Control, tag: String?, value: Variant):\n\tprint(control, tag, value)\n\n\nfunc _ready() -> void:\n\tshow_it(Label.new(), null, 1)\n\tshow_it(TextureRect.new(), \"b\", 2.5)\n\tvar named: Variant\n\tnamed = \"x\"\n\tnamed = &\"y\"\n");
	CHECK(migration.numbers == 1);
	CHECK(migration.errors.empty());
}

TEST_CASE("a value of one slot flows into another, and a slot that nothing flows into tries Variant") {
	FakeProject project;
	project.add_script("res://a.gd", "class_name A\nextends Node\n\n\nfunc first(x):\n\treturn B.second(x)\n\n\nfunc _ready() -> void:\n\tfirst(Vector2.ZERO)\n\tprint(B.unused(null))\n", "A");
	project.add_script("res://b.gd", "class_name B\nextends Node\n\n\nstatic func second(y):\n\treturn y\n\n\nstatic func unused(z):\n\treturn z\n", "B");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://a.gd", "res://b.gd" }, 10);
	CHECK(migration.texts.at("res://a.pet") == "class_name A\nextends Node\n\n\nfunc first(x: Vector2) -> Vector2:\n\treturn B.second(x)\n\n\nfunc _ready() -> void:\n\tfirst(Vector2.ZERO)\n\tprint(B.unused(null))\n");
	CHECK(migration.texts.at("res://b.pet") == "class_name B\nextends Node\n\n\npub static func second(y: Vector2) -> Vector2:\n\treturn y\n\n\npub static func unused(z: Variant) -> Variant:\n\treturn z\n");
	CHECK(migration.errors.empty());
}

TEST_CASE("an int joins a float where each read of the slot gives for an int what the same float gives") {
	FakeProject project;
	project.add_script("res://h.gd", "extends Node\nvar total := 0.0\n\n\nfunc add(amount, times):\n\ttotal += amount\n\tvar scaled = amount\n\tif scaled > 1:\n\t\ttotal -= scaled * 0.5\n\tmatch times:\n\t\t2:\n\t\t\ttotal += 1\n\n\nfunc _ready() -> void:\n\tadd(1, 2)\n\tadd(1.5, 2.5)\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://h.gd" }, 10);
	CHECK(migration.texts.at("res://h.pet") == "extends Node\nvar total := 0.0\n\n\nfunc add(amount: float, times: Variant):\n\ttotal += amount\n\tvar scaled = amount\n\tif scaled > 1:\n\t\ttotal -= scaled * 0.5\n\tmatch times:\n\t\t2:\n\t\t\ttotal += 1\n\n\nfunc _ready() -> void:\n\tadd(1, 2)\n\tadd(1.5, 2.5)\n");
	CHECK(migration.numbers == 1);
}

TEST_CASE("an int joins a float in a member by its reads in every file, but not one that flows on into a slot") {
	FakeProject project;
	project.add_script("res://meter.gd", "class_name Meter\nextends Node\nvar level\nvar peak\n\n\nfunc _ready() -> void:\n\tlevel = 1\n\tlevel = 0.5\n\tpeak = 2\n\tpeak = 2.5\n\tshow_it(peak)\n\n\nfunc show_it(value):\n\tprint(value)\n", "Meter");
	project.add_script("res://gauge.gd", "extends Node\n\n\nfunc read(meter: Meter) -> float:\n\treturn meter.level * 2.0\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://meter.gd", "res://gauge.gd" }, 10);
	CHECK(migration.texts.at("res://meter.pet") == "class_name Meter\nextends Node\npub var level: float\nvar peak: Variant\n\n\nfunc _ready() -> void:\n\tlevel = 1\n\tlevel = 0.5\n\tpeak = 2\n\tpeak = 2.5\n\tshow_it(peak)\n\n\nfunc show_it(value: Variant):\n\tprint(value)\n");
	CHECK(migration.numbers == 1);
	CHECK(migration.errors.empty());

	// No check sees how a GDScript file left out uses what it reads.
	project.add_script("res://legacy.gd", "extends Node\n\n\nfunc report(meter: Meter) -> void:\n\tprint(meter.level)\n");
	CHECK(compiler::migrate(fake_engine(), project, { "res://meter.gd", "res://gauge.gd" }, 10).numbers == 2);
}

TEST_CASE("a type that the check refuses is withdrawn, and the slot stays an error with what it received") {
	FakeProject project;
	project.add_script("res://c.gd", "extends Node\n\n\nfunc twice(x):\n\treturn x + x\n\n\nfunc _ready() -> void:\n\tprint(twice(1), twice(\"a\"))\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://c.gd" }, 10);
	CHECK(migration.texts.at("res://c.pet") == "extends Node\n\n\nfunc twice(x) -> Variant:\n\treturn x + x\n\n\nfunc _ready() -> void:\n\tprint(twice(1), twice(\"a\"))\n");
	CHECK(migration.withdrawn == 1);
	REQUIRE(migration.notes.size() == 1);
	CHECK(migration.notes[0].line == 4);
	CHECK(migration.notes[0].message == "\"x\" is left without a type: Variant does not check. It receives int (res://c.pet:9), String (res://c.pet:9).");
	REQUIRE(migration.errors.size() == 1);
	CHECK(contains(migration.errors[0].message, "\"x\" needs a type"));
}

TEST_CASE("an empty container takes the types of the elements and keys put in it, or else stays untyped") {
	FakeProject project;
	project.add_script("res://e.gd", "extends Node\nvar names = []\nvar scores = {}\n\n\nfunc gather(nodes: Array[Node]) -> void:\n\tvar picked = []\n\tnames.append(\"a\")\n\tscores[\"a\"] = 1\n\tfor child in nodes:\n\t\tpicked.push_back(child)\n\tvar mixed = []\n\tmixed.append(1)\n\tmixed.append(\"x\")\n\tvar unused = {}\n\tprint(picked, mixed, unused)\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://e.gd" }, 10);
	CHECK(migration.texts.at("res://e.pet") == "extends Node\nvar names: Array[String] = []\nvar scores: Dictionary[String, int] = {}\n\n\nfunc gather(nodes: Array[Node]) -> void:\n\tvar picked: Array[Node] = []\n\tnames.append(\"a\")\n\tscores[\"a\"] = 1\n\tfor child in nodes:\n\t\tpicked.push_back(child)\n\tvar mixed: Array = []\n\tmixed.append(1)\n\tmixed.append(\"x\")\n\tvar unused: Dictionary = {}\n\tprint(picked, mixed, unused)\n");
	CHECK(migration.errors.empty());
}

TEST_CASE("a type written over := keeps the initializer, and an assigned value that does not fit is about its target too") {
	FakeProject project;
	project.add_script("res://f.gd", "extends Node\n\n\nfunc _ready() -> void:\n\tvar points := []\n\tpoints.append(Vector2.ZERO)\n\tvar labels = []\n\tlabels.append(Label.new())\n\tlabels[0].text = 3\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://f.gd" }, 10);
	CHECK(migration.texts.at("res://f.pet") == "extends Node\n\n\nfunc _ready() -> void:\n\tvar points: Array[Vector2] = []\n\tpoints.append(Vector2.ZERO)\n\tvar labels = []\n\tlabels.append(Label.new())\n\tlabels[0].text = 3\n");
	REQUIRE(migration.notes.size() == 1);
	CHECK(migration.notes[0].message == "\"labels\" is left without a type: Array[Label] does not check. It receives an element Label (res://f.pet:8).");
}

TEST_CASE("a signal connected in a scene flows into the parameters of its method") {
	FakeProject project;
	project.add_script("res://hud.gd", "extends Node\n\n\nfunc _on_box_child_entered_tree(node):\n\tprint(node)\n");
	compiler::Scene hud;
	hud.path = "res://hud.tscn";
	hud.nodes = { { ".", "Node", "res://hud.gd" }, { "Box", "Node" } };
	hud.connections = { { "child_entered_tree", "Box", ".", "_on_box_child_entered_tree" } };
	project.scenes.push_back(hud);
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://hud.gd" }, 10);
	CHECK(migration.texts.at("res://hud.pet") == "extends Node\n\n\npub func _on_box_child_entered_tree(node: Node):\n\tprint(node)\n");
	CHECK(migration.errors.empty());
}

TEST_CASE("a value saved in a scene flows into the exported member") {
	FakeProject project;
	project.add_script("res://bag.gd", "extends Node\n@export var tags := []\n\n\nfunc _ready() -> void:\n\ttags.append(\"new\")\n");
	compiler::Scene level;
	level.path = "res://level.tscn";
	level.nodes = { { ".", "Node", "res://bag.gd" } };
	compiler::SceneValue saved{ "Array" };
	saved.items = { compiler::SceneValue{ "int", "", "", 3 } };
	level.nodes[0].properties = { { "tags", saved } };
	project.scenes.push_back(level);
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://bag.gd" }, 10);
	CHECK(migration.texts.at("res://bag.pet") == "extends Node\n@export var tags: Array = []\n\n\nfunc _ready() -> void:\n\ttags.append(\"new\")\n");
	CHECK(migration.errors.empty());
}

TEST_CASE("a member that a scene types gets its type written when another file reads it") {
	FakeProject project;
	project.add_script("res://map.gd", "class_name Map\nextends Node\n@onready var title = $Title\n@onready var unread = $Unread\n", "Map");
	project.add_script("res://hud.gd", "extends Node\n\n\nfunc show_on(map: Map) -> void:\n\tmap.title.text = \"a\"\n");
	compiler::Scene scene;
	scene.path = "res://map.tscn";
	scene.nodes = { { ".", "Node", "res://map.gd" }, { "Title", "Label" }, { "Unread", "Label" } };
	project.scenes.push_back(scene);
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://map.gd", "res://hud.gd" }, 10);
	CHECK(migration.texts.at("res://map.pet") == "class_name Map\nextends Node\n@onready pub var title: Label = $Title\n@onready var unread = $Unread\n");
	CHECK(migration.errors.empty());
}

TEST_CASE("the inference reads a declaration through an error that the loop of fixes then fixes") {
	FakeProject project;
	project.add_script("res://state.gd", "class_name CardState\nextends Node\nenum State { BASE, CLICKED }\n@export var state: State\n", "CardState");
	project.add_script("res://machine.gd", "extends Node\nvar states := {}\n\n\nfunc add(child: CardState) -> void:\n\tstates[child.state] = child\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://state.gd", "res://machine.gd" }, 10);
	CHECK(migration.texts.at("res://state.pet") == "class_name CardState\nextends Node\npub enum State { BASE, CLICKED }\n@export var state: State\n");
	CHECK(migration.texts.at("res://machine.pet") == "extends Node\nvar states: Dictionary[CardState.State, CardState] = {}\n\n\nfunc add(child: CardState) -> void:\n\tstates[child.state] = child\n");
	CHECK(migration.errors.empty());
}

TEST_CASE("a type is withdrawn for an error that reaches it through the locals and loops it types") {
	FakeProject project;
	project.add_script("res://d.gd", "extends Node\n\n\nfunc first(values):\n\tfor each in values:\n\t\tvar copy = each\n\t\tprint(copy + 1)\n\n\nfunc _ready() -> void:\n\tfirst([1])\n\tfirst([\"a\"])\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://d.gd" }, 10);
	CHECK(migration.texts.at("res://d.pet") == "extends Node\n\n\nfunc first(values):\n\tfor each in values:\n\t\tvar copy = each\n\t\tprint(copy + 1)\n\n\nfunc _ready() -> void:\n\tfirst([1])\n\tfirst([\"a\"])\n");
	CHECK(migration.withdrawn == 1);
}

TEST_CASE("an error about an override's parameter withdraws the type of its base's") {
	FakeProject project;
	project.add_script("res://base.gd", "class_name Base\nextends Node\n\n\nfunc act(target):\n\tprint(target)\n\n\nfunc _ready() -> void:\n\tact(5)\n", "Base");
	project.add_script("res://derived.gd", "extends Base\n\n\nfunc act(target):\n\tprint(target.size())\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://base.gd", "res://derived.gd" }, 10);
	CHECK(migration.texts.at("res://base.pet") == "class_name Base\nextends Node\n\n\nfunc act(target):\n\tprint(target)\n\n\nfunc _ready() -> void:\n\tact(5)\n");
	CHECK(migration.withdrawn == 1);
}

TEST_CASE("an error about a call's receiver inside an assigned value leaves the slots of the arguments and of the target") {
	FakeProject project;
	project.add_script("res://g.gd", "extends Node\n\n\nfunc total(start, items):\n\tvar sum = start\n\tfor each in items:\n\t\tsum = each.added_to(sum)\n\treturn sum\n\n\nfunc _ready() -> void:\n\tprint(total(1, []))\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://g.gd" }, 10);
	CHECK(migration.texts.at("res://g.pet") == "extends Node\n\n\nfunc total(start: int, items) -> int:\n\tvar sum = start\n\tfor each in items:\n\t\tsum = each.added_to(sum)\n\treturn sum\n\n\nfunc _ready() -> void:\n\tprint(total(1, []))\n");
	CHECK(migration.withdrawn == 1);
}

TEST_CASE("a call by name flows into every function so named, and the report marks the type it gives") {
	FakeProject project;
	project.add_script("res://enemy.gd", "extends Node\n\n\nfunc take_damage(amount):\n\tprint(amount)\n");
	project.add_script("res://bullet.gd", "extends Node\n\n\nfunc hit(body: Node) -> void:\n\tbody.take_damage(5)\n\n\nfunc graze(body: Node?) -> void:\n\tbody.take_damage(1)\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://enemy.gd", "res://bullet.gd" }, 10);
	CHECK(migration.texts.at("res://enemy.pet") == "extends Node\n\n\nfunc take_damage(amount: int):\n\tprint(amount)\n");
	CHECK(migration.by_name == 1);
	REQUIRE(migration.notes.size() == 1);
	CHECK(migration.notes[0].message == "The type of \"amount\", int, rests on calls by name: res://bullet.pet:5, res://bullet.pet:9.");
}

TEST_CASE("a signal connected to a method flows into its parameters, as do the arguments bound to it") {
	FakeProject project;
	project.add_script("res://score.gd", "extends Node\nsignal scored(points: int)\n\n\nfunc _ready() -> void:\n\tscored.connect(_on_scored)\n\tscored.connect(_on_scored_by.bind(\"me\"))\n\n\nfunc _on_scored(points):\n\tprint(points)\n\n\nfunc _on_scored_by(points, who):\n\tprint(points, who)\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://score.gd" }, 10);
	CHECK(migration.texts.at("res://score.pet") == "extends Node\nsignal scored(points: int)\n\n\nfunc _ready() -> void:\n\tscored.connect(_on_scored)\n\tscored.connect(_on_scored_by.bind(\"me\"))\n\n\nfunc _on_scored(points: int):\n\tprint(points)\n\n\nfunc _on_scored_by(points: int, who: String):\n\tprint(points, who)\n");
	CHECK(migration.errors.empty());
}

TEST_CASE("the inference and the loop of fixes stop after their rounds") {
	FakeProject project;
	project.add_script("res://unit.gd", "extends Node\n\n\nfunc wait() -> void:\n\tawait ready\n\n\nfunc take(amount):\n\tprint(amount)\n\n\nfunc hit(body: Node) -> void:\n\tbody.take(1)\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://unit.gd" }, 0);
	CHECK(!migration.settled);
	CHECK(migration.rounds == 0);
	CHECK(migration.typed == 0);
	REQUIRE(migration.notes.size() == 1);
	CHECK(migration.notes[0].message == "\"amount\" is left without a type: no type was tried for it in 0 rounds. It receives int (res://unit.pet:13).");
	CHECK(migration.errors.size() == 3);
}
