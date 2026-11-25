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

// GDScript runs no branch when none matches (modules/gdscript/gdscript_compiler.cpp:1938-1943, 1983, 1999-2002; ed1daf0).
TEST_CASE("the loop of fixes ends a match over an enum that leaves out values with _: pass") {
	FakeProject project;
	project.add_script("res://room.gd", "extends Node\nenum Type { BATTLE, SHOP, EVENT }\n\n\nfunc enter(type: Type, ease: Tween.EaseType) -> void:\n\tmatch type:\n\t\tType.BATTLE:\n\t\t\tprint(1)\n\t\tType.SHOP: print(2)  # shop\n\tmatch ease:\n\t\tTween.EASE_IN:\n\t\t\tprint(3)\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://room.gd" }, 10);
	CHECK(migration.texts.at("res://room.pet") == "extends Node\nenum Type { BATTLE, SHOP, EVENT }\n\n\nfunc enter(type: Type, ease: Tween.EaseType) -> void:\n\tmatch type:\n\t\tType.BATTLE:\n\t\t\tprint(1)\n\t\tType.SHOP: print(2)  # shop\n\t\t_: pass\n\tmatch ease:\n\t\tTween.EASE_IN:\n\t\t\tprint(3)\n\t\t_: pass\n");
	CHECK(migration.fixes == std::map<compiler::FixKind, int>{ { compiler::FixKind::Wildcard, 2 } });
	CHECK(migration.errors.empty());
}

TEST_CASE("an override with an await is made async with the method it overrides, and so are the other overrides") {
	FakeProject project;
	project.add_script("res://state.gd", "class_name CardState\nextends Node\n\n\nfunc enter() -> void:\n\tpass\n\n\nfunc change() -> void:\n\tenter()\n", "CardState");
	project.add_script("res://base_state.gd", "extends CardState\n\n\nfunc enter() -> void:\n\tawait ready\n");
	project.add_script("res://drag_state.gd", "extends CardState\n\n\nfunc enter() -> void:\n\tprint(\"drag\")\n");
	// An engine virtual method that returns a value cannot be async.
	project.add_script("res://store.gd", "extends Node\n\n\nfunc _get(property):\n\treturn null\n");
	project.add_script("res://slow_store.gd", "extends \"store.gd\"\n\n\nfunc _get(property):\n\tawait ready\n\treturn null\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://state.gd", "res://base_state.gd", "res://drag_state.gd", "res://store.gd", "res://slow_store.gd" }, 10);
	CHECK(migration.texts.at("res://state.pet") == "class_name CardState\nextends Node\n\n\nasync func enter() -> void:\n\tpass\n\n\nfunc change() -> void:\n\tenter().detach()\n");
	CHECK(migration.texts.at("res://base_state.pet") == "extends CardState\n\n\nasync func enter() -> void:\n\tawait ready\n");
	CHECK(migration.texts.at("res://drag_state.pet") == "extends CardState\n\n\nasync func enter() -> void:\n\tprint(\"drag\")\n");
	CHECK(migration.fixes == std::map<compiler::FixKind, int>{ { compiler::FixKind::Detach, 1 }, { compiler::FixKind::Async, 2 } });
	REQUIRE(migration.errors.size() == 1);
	CHECK(migration.errors[0].file == "res://slow_store.pet");
	CHECK(migration.errors[0].message == "await requires an async function and cannot appear in an initializer.");
}

TEST_CASE("a function with an await is made async though its parameter is left without a type") {
	FakeProject project;
	project.add_script("res://w.gd", "extends Node\n\n\nfunc later(x):\n\tawait ready\n\tprint(x + x)\n\n\nfunc _ready() -> void:\n\tlater(1)\n\tlater(\"a\")\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://w.gd" }, 10);
	CHECK(migration.texts.at("res://w.pet") == "extends Node\n\n\nasync func later(x):\n\tawait ready\n\tprint(x + x)\n\n\nfunc _ready() -> void:\n\tlater(1).detach()\n\tlater(\"a\").detach()\n");
	REQUIRE(migration.errors.size() == 1);
	CHECK(migration.errors[0].message == "Parameter \"x\" needs a type.");
}

// GDScript calls the setter and drops what it returns, so a write goes on
// where the setter first waits (spec §4.4).
TEST_CASE("a setter with an await is made async, and a member made T? makes its setter take T?") {
	FakeProject project;
	project.add_script("res://relic_ui.gd", "extends Node\nvar relic: Node : set = set_relic\nvar label := \"\":\n\tset(value):\n\t\tif not is_node_ready():\n\t\t\tawait ready\n\t\tlabel = value\n\n\nfunc set_relic(new_relic: Node) -> void:\n\tif not is_node_ready():\n\t\tawait ready\n\trelic = new_relic\n\n\nfunc put(node: Node) -> void:\n\trelic = node\n\tlabel = \"put\"\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://relic_ui.gd" }, 10);
	CHECK(migration.texts.at("res://relic_ui.pet") == "extends Node\nvar relic: Node? : set = set_relic\nvar label := \"\":\n\tasync set(value):\n\t\tif not is_node_ready():\n\t\t\tawait ready\n\t\tlabel = value\n\n\nasync func set_relic(new_relic: Node?) -> void:\n\tif not is_node_ready():\n\t\tawait ready\n\trelic = new_relic\n\n\nfunc put(node: Node) -> void:\n\trelic = node\n\tlabel = \"put\"\n");
	CHECK(migration.errors.empty());
}

// A node below a scene's root has no creator (spec §6.2).
TEST_CASE("a member that only an ancestor assigns in its _ready becomes T?") {
	FakeProject project;
	project.add_script("res://bar.gd", "extends Node\nvar target: Node\n\n\nfunc show_it() -> void:\n\tprint(target.name)\n");
	project.add_script("res://hud.gd", "extends Node\n\n\nfunc _ready() -> void:\n\t$Bar.target = self\n");
	compiler::Scene main;
	main.path = "res://main.tscn";
	main.nodes = { { ".", "Node", "res://hud.gd" }, { "Bar", "Node", "res://bar.gd" } };
	project.scenes = { main };
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://bar.gd", "res://hud.gd" }, 10);
	CHECK(migration.texts.at("res://bar.pet") == "extends Node\npub var target: Node?\n\n\nfunc show_it() -> void:\n\tprint(target.unwrap().name)\n");
	CHECK(migration.errors.empty());
}

// GDScript checks a value of a class where a subclass is expected as the
// program runs; the migration narrows the declaration written for it, or
// writes `as`.
TEST_CASE("a class written wider than what flows in becomes the subclass that a read expects") {
	const auto migrated = [](const std::string &p_art) {
		FakeProject project;
		project.add_script("res://stats.gd", "class_name Stats\nextends Resource\n@export var art: Resource\n", "Stats");
		project.add_script("res://enemy_stats.gd", "class_name EnemyStats\nextends Stats\n", "EnemyStats");
		project.add_script("res://enemy.gd", "extends Node\n\n\nfunc show(stats: EnemyStats) -> void:\n\tuse(stats.art)\n\n\nfunc use(texture: Texture2D) -> void:\n\tpass\n");
		compiler::Scene bat;
		bat.path = "res://bat.tres";
		bat.resources = { { bat.path, "res://enemy_stats.gd", { { "art", compiler::SceneValue{ "Object", p_art } } } } };
		project.scenes = { bat };
		return compiler::migrate(fake_engine(), project, { "res://stats.gd", "res://enemy_stats.gd", "res://enemy.gd" }, 10);
	};
	const compiler::Migration stored = migrated("CompressedTexture2D");
	CHECK(stored.texts.at("res://stats.pet") == "class_name Stats\nextends Resource\n@export var art: CompressedTexture2D\n");
	CHECK(stored.errors.empty());
	// What a resource stores may be wider; the read then checks it.
	const compiler::Migration wider = migrated("AudioStream");
	CHECK(wider.texts.at("res://stats.pet") == "class_name Stats\nextends Resource\n@export var art: Resource\n");
	CHECK(wider.texts.at("res://enemy.pet") == "extends Node\n\n\nfunc show(stats: EnemyStats) -> void:\n\tuse((stats.art as Texture2D).unwrap())\n\n\nfunc use(texture: Texture2D) -> void:\n\tpass\n");
}

TEST_CASE("an export written PackedScene takes the class of the roots of the scenes stored for it and assigned to it, or Node") {
	FakeProject project;
	// The scenes leave it to the code, so GDScript has it null.
	project.add_script("res://enemy.gd", "class_name Enemy\nextends Node2D\n@export var target: Node2D\n", "Enemy");
	project.add_script("res://boss.gd", "class_name Boss\nextends Enemy\n", "Boss");
	project.add_script("res://stats.gd", "class_name Stats\nextends Resource\n@export var one: PackedScene\n@export var two: PackedScene\n@export var none: PackedScene\n", "Stats");
	project.add_script("res://spawner.gd", "extends Node\n\n\nfunc arm(stats: Stats) -> void:\n\tstats.two = preload(\"res://boss.tscn\")\n");
	compiler::Scene enemy;
	enemy.path = "res://enemy.tscn";
	enemy.nodes = { { ".", "Node2D", "res://enemy.gd" } };
	compiler::Scene boss;
	boss.path = "res://boss.tscn";
	boss.nodes = { { ".", "Node2D", "res://boss.gd" } };
	compiler::SceneValue stored{ "Object", "PackedScene" };
	stored.path = enemy.path;
	compiler::Scene bat;
	bat.path = "res://bat.tres";
	bat.resources = { { bat.path, "res://stats.gd", { { "one", stored }, { "two", stored } } } };
	project.scenes = { enemy, boss, bat };
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://enemy.gd", "res://boss.gd", "res://stats.gd", "res://spawner.gd" }, 10);
	CHECK(migration.texts.at("res://stats.pet") == "class_name Stats\nextends Resource\n@export var one: PackedScene[Enemy]\n@export var two: PackedScene[Enemy]\n@export var none: PackedScene[Node]?\n");
	CHECK(migration.texts.at("res://enemy.pet") == "class_name Enemy\nextends Node2D\n@export var target: Node2D?\n");
	CHECK(migration.errors.empty());
}

// deck_builder_tutorial's EventRoomPool (custom_resources/event_room_pool.gd,
// scenes/event_rooms/event_room_pool.tres) and Room.event_scene
// (custom_resources/room.gd:15, scenes/map/map_generator.gd:208).
TEST_CASE("an export written Array[PackedScene] takes the class of the roots of the scenes stored in it, and passes it on to a scene written PackedScene") {
	FakeProject project;
	project.add_script("res://event.gd", "class_name EventRoom\nextends Control\n@export var stats: Resource\n", "EventRoom");
	project.add_script("res://gamble.gd", "extends EventRoom\n");
	project.add_script("res://pool.gd", "class_name Pool\nextends Resource\n@export var rooms: Array[PackedScene]\n@export var spare: Array[PackedScene]\n\n\nfunc pick() -> PackedScene:\n\treturn rooms.pick_random()\n", "Pool");
	project.add_script("res://room.gd", "class_name Room\nextends Resource\n@export var scene: PackedScene\n", "Room");
	project.add_script("res://map.gd", "extends Node\n\n\nfunc set_up(room: Room, pool: Pool) -> void:\n\troom.scene = pool.pick()\n");
	compiler::Scene gamble;
	gamble.path = "res://gamble.tscn";
	gamble.nodes = { { ".", "Control", "res://gamble.gd" } };
	compiler::Scene help;
	help.path = "res://help.tscn";
	help.nodes = { { ".", "Control", "res://event.gd" } };
	compiler::SceneValue rooms{ "Array" };
	rooms.typed = { { "Object", "PackedScene" } };
	for (const std::string &path : { gamble.path, help.path }) {
		rooms.items.push_back({ "Object", "PackedScene" });
		rooms.items.back().path = path;
	}
	compiler::Scene pool;
	pool.path = "res://pool.tres";
	pool.resources = { { pool.path, "res://pool.gd", { { "rooms", rooms } } } };
	project.scenes = { gamble, help, pool };
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://event.gd", "res://gamble.gd", "res://pool.gd", "res://room.gd", "res://map.gd" }, 10);
	CHECK(migration.texts.at("res://pool.pet") == "class_name Pool\nextends Resource\n@export var rooms: Array[PackedScene[EventRoom]]\n@export var spare: Array[PackedScene]\n\n\npub func pick() -> PackedScene[EventRoom]:\n\treturn rooms.pick_random()\n");
	CHECK(migration.texts.at("res://room.pet") == "class_name Room\nextends Resource\n@export var scene: PackedScene[EventRoom]\n");
	CHECK(migration.texts.at("res://event.pet") == "class_name EventRoom\nextends Control\n@export var stats: Resource?\n");
	CHECK(migration.errors.empty());
}

TEST_CASE("a member is narrowed with its setter's parameter, and an override's result apart from the method it overrides") {
	FakeProject project;
	project.add_script("res://button.gd", "class_name RewardButton\nextends Node\nvar icon: Resource = Texture2D.new() : set = set_icon\n\n\nfunc set_icon(new_icon: Resource) -> void:\n\ticon = new_icon\n\tshow(icon)\n\n\nfunc show(texture: Texture2D) -> void:\n\tpass\n", "RewardButton");
	project.add_script("res://reward.gd", "extends Node\n\n\nfunc give(button: RewardButton) -> void:\n\tbutton.icon = Texture2D.new()\n");
	project.add_script("res://stats.gd", "class_name Stats\nextends Resource\n\n\nfunc create_instance() -> Resource:\n\treturn Resource.new()\n", "Stats");
	project.add_script("res://hero_stats.gd", "class_name HeroStats\nextends Stats\n\n\nfunc create_instance() -> Resource:\n\tvar instance: HeroStats = HeroStats.new()\n\treturn instance\n", "HeroStats");
	project.add_script("res://run.gd", "extends Node\nvar hero: HeroStats\n\n\nfunc start(picked: HeroStats) -> void:\n\thero = picked.create_instance()\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://button.gd", "res://reward.gd", "res://stats.gd", "res://hero_stats.gd", "res://run.gd" }, 10);
	CHECK(migration.texts.at("res://button.pet") == "class_name RewardButton\nextends Node\npub var icon: Texture2D = Texture2D.new() : set = set_icon\n\n\nfunc set_icon(new_icon: Texture2D) -> void:\n\ticon = new_icon\n\tshow(icon)\n\n\nfunc show(texture: Texture2D) -> void:\n\tpass\n");
	CHECK(migration.texts.at("res://stats.pet") == "class_name Stats\nextends Resource\n\n\npub func create_instance() -> Resource:\n\treturn Resource.new()\n");
	CHECK(migration.texts.at("res://hero_stats.pet") == "class_name HeroStats\nextends Stats\n\n\npub func create_instance() -> HeroStats:\n\tvar instance: HeroStats = HeroStats.new()\n\treturn instance\n");
	CHECK(migration.errors.empty());
}

TEST_CASE("where no declaration narrows, a value of a class goes as a subclass through as") {
	FakeProject project;
	project.add_script("res://picker.gd", "extends Node\nvar target: Node2D\n\n\nfunc pick(found: Node) -> void:\n\ttarget = found\n\tvar other: Node2D = get_node_or_null(\"Other\")\n\tprint(other)\n\n\nfunc tip(icon: Resource) -> void:\n\tvar shown: Texture2D = icon\n\tprint(shown)\n");
	project.add_script("res://handler.gd", "extends Node\n\n\nfunc _ready() -> void:\n\tchild_exiting_tree.connect(_on_exiting)\n\n\nfunc _on_exiting(node: Node2D) -> void:\n\tpass\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://picker.gd", "res://handler.gd" }, 10);
	CHECK(migration.texts.at("res://picker.pet") == "extends Node\nvar target: Node2D?\n\n\nfunc pick(found: Node) -> void:\n\ttarget = (found as Node2D).unwrap()\n\tvar other: Node2D = (get_node_or_null(\"Other\") as Node2D).unwrap()\n\tprint(other)\n\n\nfunc tip(icon: Resource) -> void:\n\tvar shown: Texture2D = (icon as Texture2D).unwrap()\n\tprint(shown)\n");
	REQUIRE(migration.errors.size() == 1);
	CHECK(migration.errors[0].file == "res://handler.pet");
}

TEST_CASE("the loop of fixes makes an export that a scene or a resource leaves without a value a T?, as GDScript gives it null") {
	FakeProject project;
	project.add_script("res://stats.gd", "extends Resource\n@export var portrait: Texture2D\n\n\nfunc copy() -> Texture2D:\n\treturn portrait.duplicate()\n");
	project.add_script("res://hud.gd", "extends Node\n@export var icon: Texture2D\n");
	// Reported where the scene is made.
	project.add_script("res://badge.gd", "extends Node\n@export var art: Texture2D\n");
	project.add_script("res://shelf.gd", "extends Node\n\n\nfunc add() -> void:\n\tadd_child(preload(\"res://badge.tscn\").instantiate())\n");
	compiler::Scene hero;
	hero.path = "res://hero.tres";
	hero.resources = { { hero.path, "res://stats.gd", {} } };
	compiler::Scene main;
	main.path = "res://main.tscn";
	main.nodes = { { ".", "Node", "res://hud.gd" } };
	compiler::Scene badge;
	badge.path = "res://badge.tscn";
	badge.nodes = { { ".", "Node", "res://badge.gd" } };
	project.scenes = { hero, main, badge };
	project.main = main.path;
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://stats.gd", "res://hud.gd", "res://badge.gd", "res://shelf.gd" }, 10);
	CHECK(migration.texts.at("res://stats.pet") == "extends Resource\n@export var portrait: Texture2D?\n\n\nfunc copy() -> Texture2D:\n\treturn portrait.unwrap().duplicate()\n");
	CHECK(migration.texts.at("res://hud.pet") == "extends Node\n@export var icon: Texture2D?\n");
	CHECK(migration.texts.at("res://badge.pet") == "extends Node\n@export var art: Texture2D?\n");
	CHECK(migration.fixes == std::map<compiler::FixKind, int>{ { compiler::FixKind::Unwrap, 1 }, { compiler::FixKind::Optional, 3 } });
	CHECK(migration.errors.empty());
}

TEST_CASE("a variable declared without a value starts as null in GDScript, which makes a class T? unless _init assigns it") {
	FakeProject project;
	project.add_script("res://deck.gd", "class_name Deck\nextends Resource\nvar pile: Node\nvar keeper: Node\nvar count\nvar picked\n\n\nfunc _init() -> void:\n\tkeeper = Node.new()\n\n\nfunc fill() -> void:\n\tpile = Node.new()\n\tcount = 0\n\tpicked = Node.new()\n\n\nfunc show() -> void:\n\tvar shown := pile.name\n\tprint(shown, keeper.name, count)\n\n\nfunc pick(nodes: Array[Node], at: int):\n\tvar left: Node\n\tif at > 0:\n\t\tleft = nodes[at - 1]\n\tif left and left.name != \"\":\n\t\treturn left\n", "Deck");
	// Read through a member that is not pub yet, and passed unwrapped to a call on another.
	project.add_script("res://table.gd", "extends Node\n\n\nfunc clear(deck: Deck, other: Deck) -> Array[Node]:\n\tdeck.pile.add_child(other.pile)\n\treturn [deck.pile]\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://deck.gd", "res://table.gd" }, 10);
	CHECK(migration.texts.at("res://table.pet") == "extends Node\n\n\nfunc clear(deck: Deck, other: Deck) -> Array[Node]:\n\tdeck.pile.unwrap().add_child(other.pile.unwrap())\n\treturn [deck.pile.unwrap()]\n");
	CHECK(migration.texts.at("res://deck.pet") == "class_name Deck\nextends Resource\npub var pile: Node?\nvar keeper: Node\nvar count: int\nvar picked: Node?\n\n\nfunc _init() -> void:\n\tkeeper = Node.new()\n\n\nfunc fill() -> void:\n\tpile = Node.new()\n\tcount = 0\n\tpicked = Node.new()\n\n\nfunc show() -> void:\n\tvar shown := pile.unwrap().name\n\tprint(shown, keeper.name, count)\n\n\nfunc pick(nodes: Array[Node], at: int) -> Node?:\n\tvar left: Node?\n\tif at > 0:\n\t\tleft = nodes[at - 1]\n\tif left != None and left.name != \"\":\n\t\treturn left\n\treturn None\n");
	CHECK(migration.errors.empty());
}

// The read stays an error, which the loop of fixes unwraps after the
// inference. tanks-of-freedom-ii's camera angle takes a float so
// (scenes/camera.gd:101-102).
TEST_CASE("a read or a call through a T? receiver gives what it gives unwrapped to the slot it flows into") {
	FakeProject project;
	project.add_script("res://camera.gd", "extends Node\nvar pivot\nvar rng\nvar angle\nvar spin\n\n\nfunc _ready() -> void:\n\tpivot = Node3D.new()\n\trng = RandomNumberGenerator.new()\n\n\nfunc aim() -> void:\n\tangle = pivot.rotation.y\n\tspin = rng.randf_range(0.0, 1.0)\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://camera.gd" }, 10);
	CHECK(migration.texts.at("res://camera.pet") == "extends Node\nvar pivot: Node3D?\nvar rng: RandomNumberGenerator?\nvar angle: float\nvar spin: float\n\n\nfunc _ready() -> void:\n\tpivot = Node3D.new()\n\trng = RandomNumberGenerator.new()\n\n\nfunc aim() -> void:\n\tangle = pivot.unwrap().rotation.y\n\tspin = rng.unwrap().randf_range(0.0, 1.0)\n");
	CHECK(migration.errors.empty());
}

TEST_CASE("the loop of fixes makes a type T? where the original assigns or returns null") {
	FakeProject project;
	project.add_script("res://picker.gd", "extends Node\nvar selected: Node = self\n\n\nfunc pick(nodes: Array[Node]) -> Node:\n\tfor node in nodes:\n\t\treturn node\n\treturn null\n\n\nfunc last(nodes: Array[Node]) -> Node:\n\tif nodes.is_empty():\n\t\treturn\n\treturn nodes.back()\n\n\nfunc choose(nodes: Array[Node]) -> void:\n\tselected = null\n\tvar chosen: Node = pick(nodes)\n\tprint(chosen.name)\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://picker.gd" }, 10);
	CHECK(migration.texts.at("res://picker.pet") == "extends Node\nvar selected: Node? = self\n\n\nfunc pick(nodes: Array[Node]) -> Node?:\n\tfor node in nodes:\n\t\treturn node\n\treturn null\n\n\nfunc last(nodes: Array[Node]) -> Node?:\n\tif nodes.is_empty():\n\t\treturn None\n\treturn nodes.back()\n\n\nfunc choose(nodes: Array[Node]) -> void:\n\tselected = null\n\tvar chosen: Node = pick(nodes).unwrap()\n\tprint(chosen.name)\n");
	CHECK(migration.fixes == std::map<compiler::FixKind, int>{ { compiler::FixKind::Unwrap, 1 }, { compiler::FixKind::Optional, 3 }, { compiler::FixKind::ReturnNone, 1 } });
	CHECK(migration.errors.empty());
}

// GDScript passes the null to the connected functions, whose parameters take
// it (modules/gdscript/gdscript_function.cpp:103-106; ed1daf0).
TEST_CASE("the loop of fixes makes a signal's parameter T? where the original emits null, and the parameters of the functions connected to it") {
	FakeProject project;
	project.add_script("res://rewards.gd", "class_name Rewards\nextends Node\nsignal picked(card: Node)\n\n\nfunc _ready() -> void:\n\tpicked.connect(_on_picked)\n\tpicked.connect(func(card: Node): print(card))\n\n\nfunc _on_picked(card: Node) -> void:\n\tif not card:\n\t\treturn\n\tprint(card.name)\n", "Rewards");
	project.add_script("res://skip.gd", "extends Node\n\n\nfunc skip(rewards: Rewards) -> void:\n\trewards.picked.emit(null)\n");
	project.add_script("res://hud.gd", "extends Node\n\n\nfunc _on_rewards_picked(card: Node) -> void:\n\tprint(card)\n");
	compiler::Scene hud;
	hud.path = "res://hud.tscn";
	hud.nodes = { { ".", "Node", "res://hud.gd" }, { "Rewards", "Node", "res://rewards.gd" } };
	hud.connections = { { "picked", "Rewards", ".", "_on_rewards_picked" } };
	project.scenes.push_back(hud);
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://rewards.gd", "res://skip.gd", "res://hud.gd" }, 10);
	CHECK(migration.texts.at("res://hud.pet") == "extends Node\n\n\npub func _on_rewards_picked(card: Node?) -> void:\n\tprint(card)\n");
	CHECK(migration.texts.at("res://rewards.pet") == "class_name Rewards\nextends Node\npub signal picked(card: Node?)\n\n\nfunc _ready() -> void:\n\tpicked.connect(_on_picked)\n\tpicked.connect(func(card: Node?): print(card))\n\n\nfunc _on_picked(card: Node?) -> void:\n\tif not is_instance_valid(card):\n\t\treturn\n\tprint(card.unwrap().name)\n");
	CHECK(migration.errors.empty());
}

// GDScript calls a bare new() on the script that runs it: in static code the
// class, in a method the script of self, which may be a subclass
// (modules/gdscript/gdscript_compiler.cpp:654-661, gdscript_vm.cpp:656-666;
// ed1daf0).
TEST_CASE("a bare new() in static code constructs the class by its name, and one in a method stays an error") {
	FakeProject project;
	project.add_script("res://modifier.gd", "class_name Modifier\nextends Node\nvar source := \"\"\n\n\nstatic func create(from: String) -> Modifier:\n\tvar made := new()\n\tmade.source = from\n\treturn made\n\n\nfunc copy() -> Modifier:\n\treturn new()\n\n\nclass Part:\n\tstatic func make() -> Part:\n\t\treturn new()\n", "Modifier");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://modifier.gd" }, 10);
	CHECK(migration.texts.at("res://modifier.pet") == "class_name Modifier\nextends Node\nvar source := \"\"\n\n\nstatic func create(from: String) -> Modifier:\n\tvar made := Modifier.new()\n\tmade.source = from\n\treturn made\n\n\nfunc copy() -> Modifier:\n\treturn new()\n\n\nclass Part:\n\tstatic func make() -> Part:\n\t\treturn Part.new()\n");
	REQUIRE(migration.errors.size() == 1);
	CHECK(migration.errors[0].line == 13);
}

TEST_CASE("a literal path from the root to an autoload becomes its name, whose class the inference reads") {
	FakeProject project;
	project.add_script("res://audio.gd", "extends Node\n\n\nfunc play(sound):\n\tprint(sound)\n");
	project.autoloads["Audio"] = "res://audio.gd";
	project.add_script("res://hud.gd", "extends Node\n@onready var audio = $\"/root/Audio\"\n\n\nfunc _ready() -> void:\n\taudio.play(\"click\")\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://audio.gd", "res://hud.gd" }, 10);
	CHECK(migration.texts.at("res://audio.pet") == "extends Node\n\n\npub func play(sound: String):\n\tprint(sound)\n");
	CHECK(migration.texts.at("res://hud.pet") == "extends Node\n@onready var audio = Audio\n\n\nfunc _ready() -> void:\n\taudio.play(\"click\")\n");
	CHECK(migration.partial == 0);
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

// GDScript converts a float that goes where it wrote int, as int(x) does
// (modules/gdscript/gdscript_analyzer.cpp:3062-3064, 6370-6374,
// gdscript_vm.cpp:1433-1438; ed1daf0), and the result of a compound
// assignment, not its right side (gdscript_compiler.cpp:1346-1381). A
// variable declared with an int value and no type holds the float.
// tanks-of-freedom-ii reads its camera's angles negated and through float()
// (scenes/camera.gd:50, 293). A negated read is not kept: GDScript's -0 is
// the int 0, which prints as 0, while -0.0 prints as -0 (spec §1.2).
TEST_CASE("a float into an int that GDScript wrote is converted, and a float joins an int value where every read keeps") {
	FakeProject project;
	project.add_script("res://base.gd", "extends Node\nvar delay = 0\nvar steps = 0\nvar angle = 0\nvar turns = 0\n\n\nfunc run() -> void:\n\tvar t := 0\n\tt = 1.5\n\tt += 0.5\n\tsteps = 2.5\n\tprint(t, delay > 0, Vector2(delay, 0), steps / 2, deg_to_rad(-angle), float(turns))\n");
	project.add_script("res://lock.gd", "extends \"res://base.gd\"\n\n\nfunc _init() -> void:\n\tself.delay = 0.3\n\tself.angle = 0.5\n\tself.turns = 1.5\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://base.gd", "res://lock.gd" }, 10);
	CHECK(migration.texts.at("res://base.pet") == "extends Node\nvar delay: float = 0\nvar steps = 0\nvar angle = 0\nvar turns: float = 0\n\n\nfunc run() -> void:\n\tvar t := 0\n\tt = int(1.5)\n\tt += 0.5\n\tsteps = 2.5\n\tprint(t, delay > 0, Vector2(delay, 0), steps / 2, deg_to_rad(-angle), float(turns))\n");
	CHECK(migration.fixes == std::map<compiler::FixKind, int>{ { compiler::FixKind::IntConversion, 1 } });
	CHECK(migration.numbers == 2);
	REQUIRE(migration.errors.size() == 3);
	for (const compiler::Diagnostic &error : migration.errors) {
		CHECK(contains(error.message, "Cannot assign a value of type float as int"));
	}
}

TEST_CASE("an int joins a float by the reads of the last round only, not one beside a float that a later round makes an int") {
	FakeProject project;
	project.add_script("res://d.gd", "extends Node\n\n\nfunc f(x, y):\n\tprint(x / y)\n\n\nfunc g(k):\n\tf(1, k)\n\n\nfunc _ready() -> void:\n\tf(1.5, 2.0)\n\tg(2)\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://d.gd" }, 10);
	CHECK(migration.texts.at("res://d.pet") == "extends Node\n\n\nfunc f(x, y):\n\tprint(x / y)\n\n\nfunc g(k: int):\n\tf(1, k)\n\n\nfunc _ready() -> void:\n\tf(1.5, 2.0)\n\tg(2)\n");
	CHECK(migration.numbers == 2);
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

// In the shape of tanks-of-freedom-ii, whose board passes itself to its
// abilities (spec §2.1, §7.6).
TEST_CASE("a class without a class_name is named in types by a constant that preloads it, which the file has or gets") {
	FakeProject project;
	project.add_script("res://board/board.gd", "extends Node\nvar abilities = preload(\"res://abilities/abilities.gd\").new(self)\n");
	project.add_script("res://abilities/abilities.gd", "extends RefCounted\nvar board\n\n\nfunc _init(p_board):\n\tboard = p_board\n");
	project.add_script("res://unit.gd", "extends RefCounted\n");
	project.add_script("res://outcomes/camera.gd", "extends Node\n");
	project.add_script("res://camera.gd", "class_name Camera\nextends Node\n", "Camera");
	// A directory named as the file adds nothing to its name.
	project.add_script("res://ui/online/online.gd", "extends Node\n");
	project.add_script("res://online.gd", "class_name Online\nextends Node\n", "Online");
	project.add_script("res://hud.gd", "extends Node\nconst Unit = preload(\"res://unit.gd\")\nvar picked\nvar watched\nvar lobby\n\n\nfunc _init() -> void:\n\tpicked = Unit.new()\n\twatched = preload(\"res://outcomes/camera.gd\").new()\n\tlobby = preload(\"res://ui/online/online.gd\").new()\n\tadd_child(watched)\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://board/board.gd", "res://abilities/abilities.gd", "res://unit.gd", "res://outcomes/camera.gd", "res://camera.gd", "res://hud.gd", "res://ui/online/online.gd", "res://online.gd" }, 10);
	CHECK(migration.texts.at("res://abilities/abilities.pet") == "extends RefCounted\n\nconst Board = preload(\"res://board/board.pet\")\nvar board: Board\n\n\npub func _init(p_board: Board):\n\tboard = p_board\n");
	CHECK(migration.texts.at("res://hud.pet") == "extends Node\n\nconst OutcomesCamera = preload(\"res://outcomes/camera.pet\")\nconst UiOnline = preload(\"res://ui/online/online.pet\")\nconst Unit = preload(\"res://unit.pet\")\nvar picked: Unit\nvar watched: OutcomesCamera\nvar lobby: UiOnline\n\n\nfunc _init() -> void:\n\tpicked = Unit.new()\n\twatched = preload(\"res://outcomes/camera.pet\").new()\n\tlobby = preload(\"res://ui/online/online.pet\").new()\n\tadd_child(watched)\n");
	CHECK(migration.errors.empty());
	CHECK(std::any_of(migration.notes.begin(), migration.notes.end(), [](const compiler::Diagnostic &p_note) { return p_note.file == "res://hud.pet" && p_note.message == "Added const OutcomesCamera = preload(\"res://outcomes/camera.pet\"), const UiOnline = preload(\"res://ui/online/online.pet\") to name classes without a class_name in types."; }));
}

TEST_CASE("an empty container takes the types of the elements and keys put in it, or else stays untyped") {
	FakeProject project;
	project.add_script("res://e.gd", "extends Node\nvar names = []\nvar scores = {}\n\n\nfunc gather(nodes: Array[Node]) -> void:\n\tvar picked = []\n\tnames.append(\"a\")\n\tscores[\"a\"] = 1\n\tfor child in nodes:\n\t\tpicked.push_back(child)\n\tvar mixed = []\n\tmixed.append(1)\n\tmixed.append(\"x\")\n\tvar unused = {}\n\tprint(picked, mixed, unused)\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://e.gd" }, 10);
	CHECK(migration.texts.at("res://e.pet") == "extends Node\nvar names: Array[String] = []\nvar scores: Dictionary[String, int] = {}\n\n\nfunc gather(nodes: Array[Node]) -> void:\n\tvar picked: Array[Node] = []\n\tnames.append(\"a\")\n\tscores[\"a\"] = 1\n\tfor child in nodes:\n\t\tpicked.push_back(child)\n\tvar mixed: Array = []\n\tmixed.append(1)\n\tmixed.append(\"x\")\n\tvar unused: Dictionary = {}\n\tprint(picked, mixed, unused)\n");
	CHECK(migration.errors.empty());
}

// In the shape of deck_builder_tutorial's map (scenes/map/map_generator.gd:25-66,
// scenes/map/map.gd:14-62). GDScript cannot write the inner type.
TEST_CASE("an Array of untyped Arrays takes the type of the Arrays that flow in, with each declaration they pass, unless reads need another") {
	FakeProject project;
	project.add_script("res://map.gd", "extends Node\nvar grid: Array[Array]\n\n\nfunc build() -> Array[Array]:\n\tvar rows: Array[Array] = []\n\tfor i in 2:\n\t\tvar row: Array[Node3D] = []\n\t\trow.append(Node3D.new())\n\t\trows.append(row)\n\treturn rows\n\n\nfunc restore(saved: Array[Array]) -> void:\n\tgrid = saved\n\n\nfunc _ready() -> void:\n\tgrid = build()\n\trestore(grid)\n\tvar first: Node3D = grid[0][0]\n\tfor row: Array in grid:\n\t\tfor node: Node3D in row:\n\t\t\tprint(node, first)\n");
	project.add_script("res://wide.gd", "extends Node\nvar grid: Array[Array]\n\n\nfunc _ready() -> void:\n\tvar row: Array[Node3D] = []\n\trow.append(Node3D.new())\n\tgrid.append(row)\n\tvar first: Node = grid[0][0]\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://map.gd", "res://wide.gd" }, 10);
	CHECK(migration.texts.at("res://map.pet") == "extends Node\nvar grid: Array[Array[Node3D]]\n\n\nfunc build() -> Array[Array[Node3D]]:\n\tvar rows: Array[Array[Node3D]] = []\n\tfor i in 2:\n\t\tvar row: Array[Node3D] = []\n\t\trow.append(Node3D.new())\n\t\trows.append(row)\n\treturn rows\n\n\nfunc restore(saved: Array[Array[Node3D]]) -> void:\n\tgrid = saved\n\n\nfunc _ready() -> void:\n\tgrid = build()\n\trestore(grid)\n\tvar first: Node3D = grid[0][0]\n\tfor row: Array[Node3D] in grid:\n\t\tfor node: Node3D in row:\n\t\t\tprint(node, first)\n");
	// A class that the elements extend is the owner's to choose (BACKLOG.md).
	CHECK(migration.texts.at("res://wide.pet") == "extends Node\nvar grid: Array[Array]\n\n\nfunc _ready() -> void:\n\tvar row: Array[Node3D] = []\n\trow.append(Node3D.new())\n\tgrid.append(row)\n\tvar first: Node = grid[0][0]\n");
	REQUIRE(migration.errors.size() == 1);
	CHECK(migration.errors[0].file == "res://wide.pet");
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

TEST_CASE("a constant that a resource types gets its type written when another file reads it") {
	FakeProject project;
	project.resources["res://style.tres"] = { "Texture2D", "" };
	project.add_script("res://card.gd", "class_name Card\nextends Node\nconst STYLE := preload(\"res://style.tres\")\nconst ICON = preload(\"res://style.tres\")\n", "Card");
	project.add_script("res://hand.gd", "extends Node\n\n\nfunc show_on(card: Card) -> void:\n\tvar t: Texture2D = card.STYLE\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://card.gd", "res://hand.gd" }, 10);
	CHECK(migration.texts.at("res://card.pet") == "class_name Card\nextends Node\npub const STYLE: Texture2D = preload(\"res://style.tres\")\nconst ICON = preload(\"res://style.tres\")\n");
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
	project.add_script("res://d.gd", "extends Node\n\n\nfunc first(values):\n\tfor each in values:\n\t\tvar copy = each\n\t\tprint(copy.length())\n\n\nfunc _ready() -> void:\n\tfirst([1])\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://d.gd" }, 10);
	CHECK(migration.texts.at("res://d.pet") == "extends Node\n\n\nfunc first(values):\n\tfor each in values:\n\t\tvar copy = each\n\t\tprint(copy.length())\n\n\nfunc _ready() -> void:\n\tfirst([1])\n");
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

TEST_CASE("an error that a declaration of another file has errors leaves the type that reads it") {
	FakeProject project;
	project.add_script("res://camera.gd", "class_name Camera\nextends Node\nvar speed: int = \"fast\"\n", "Camera");
	project.add_script("res://map.gd", "extends Node\n\n\nfunc follow(camera) -> int:\n\treturn camera.speed\n\n\nfunc _ready() -> void:\n\tprint(follow(Camera.new()))\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://camera.gd", "res://map.gd" }, 10);
	CHECK(migration.texts.at("res://map.pet") == "extends Node\n\n\nfunc follow(camera: Camera) -> int:\n\treturn camera.speed\n\n\nfunc _ready() -> void:\n\tprint(follow(Camera.new()))\n");
	CHECK(migration.withdrawn == 0);
	REQUIRE(migration.errors.size() == 2);
	CHECK(migration.errors[1].message == "Camera.speed has errors (res://camera.pet, line 3).");
}

TEST_CASE("an assigned call that does not fit is about its callee, whatever its arguments are") {
	FakeProject project;
	project.add_script("res://map.gd", "extends Node\nvar cell: Vector2i\n\n\nfunc to_cell(x):\n\treturn Vector2(x, x)\n\n\nfunc place(where) -> void:\n\tcell = to_cell(where.x)\n\n\nfunc _ready() -> void:\n\tplace(Vector2(1, 2))\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://map.gd" }, 10);
	CHECK(contains(migration.texts.at("res://map.pet"), "func place(where: Vector2) -> void:"));
	CHECK(migration.withdrawn == 1);
}

// tanks-of-freedom-ii's camera takes a float of the pivot's rotation into an
// int member, and its loader uses what a call with its parameters gave
// (scenes/camera.gd:101-102, scenes/map/loader.gd:33-34).
TEST_CASE("an error at a value is about the slots that give it its type, not what a member is read on, nor the arguments of the call that typed a local") {
	FakeProject project;
	project.add_script("res://camera.gd", "extends Node\nvar pivot\nvar angle = 0\n\n\nfunc _ready() -> void:\n\tpivot = Node3D.new()\n\taim(\"a\")\n\n\nfunc find(key):\n\tprint(key)\n\treturn Node.new()\n\n\nfunc aim(key) -> void:\n\tvar turned = pivot.rotation\n\tangle = turned.y\n\tprint(angle)\n\tvar found = find(key)\n\tfound.open()\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://camera.gd" }, 10);
	CHECK(migration.texts.at("res://camera.pet") == "extends Node\nvar pivot: Node3D?\nvar angle = 0\n\n\nfunc _ready() -> void:\n\tpivot = Node3D.new()\n\taim(\"a\")\n\n\nfunc find(key: String):\n\tprint(key)\n\treturn Node.new()\n\n\nfunc aim(key: String) -> void:\n\tvar turned = pivot.unwrap().rotation\n\tangle = turned.y\n\tprint(angle)\n\tvar found = find(key)\n\tfound.open()\n");
	CHECK(migration.withdrawn == 1);
	CHECK(std::any_of(migration.errors.begin(), migration.errors.end(), [](const compiler::Diagnostic &p_error) { return p_error.line == 18 && contains(p_error.message, "Cannot assign a value of type float as int"); }));
}

TEST_CASE("an error about a call's receiver inside an assigned value leaves the slots of the arguments and of the target") {
	FakeProject project;
	project.add_script("res://g.gd", "extends Node\n\n\nfunc total(start, items):\n\tvar sum = start\n\tfor each in items:\n\t\tsum = each.added_to(sum)\n\treturn sum\n\n\nfunc _ready() -> void:\n\tprint(total(1, []))\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://g.gd" }, 10);
	CHECK(migration.texts.at("res://g.pet") == "extends Node\n\n\nfunc total(start: int, items: Array) -> int:\n\tvar sum = start\n\tfor each in items:\n\t\tsum = each.added_to(sum)\n\treturn sum\n\n\nfunc _ready() -> void:\n\tprint(total(1, []))\n");
	CHECK(migration.withdrawn == 0);
}

TEST_CASE("an error of an operation with a call's Variant leaves the slots of its arguments") {
	FakeProject project;
	project.add_script("res://g.gd", "extends Node\nvar data: Dictionary = {}\n\n\nfunc lookup(key) -> Variant:\n\treturn data[key]\n\n\nfunc add(total, key):\n\ttotal += lookup(key)\n\treturn total\n\n\nfunc _ready() -> void:\n\tprint(add(1, \"hp\"))\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://g.gd" }, 10);
	CHECK(migration.texts.at("res://g.pet") == "extends Node\nvar data: Dictionary = {}\n\n\nfunc lookup(key: String) -> Variant:\n\treturn data[key]\n\n\nfunc add(total: int, key: String) -> int:\n\ttotal += lookup(key)\n\treturn total\n\n\nfunc _ready() -> void:\n\tprint(add(1, \"hp\"))\n");
	CHECK(migration.withdrawn == 0);
	CHECK(migration.errors.size() == 1);
}

TEST_CASE("an error about an element of an untyped container leaves the slots of the container") {
	FakeProject project;
	project.add_script("res://g.gd", "extends Node\nvar data\n\n\nfunc fill(new_data):\n\tdata = new_data\n\tif data[\"details\"].has(\"who\"):\n\t\tprint(data[\"details\"][\"who\"])\n\tvar step = data.get(\"step\", 0)\n\tprint(step.size())\n\n\nfunc passives():\n\treturn []\n\n\nfunc total():\n\tvar sum = 0\n\tfor ability in passives():\n\t\tsum = ability.added_to(sum)\n\treturn sum\n\n\nfunc _ready() -> void:\n\tfill({})\n\tprint(total())\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://g.gd" }, 10);
	CHECK(migration.texts.at("res://g.pet") == "extends Node\nvar data: Dictionary\n\n\nfunc fill(new_data: Dictionary):\n\tdata = new_data\n\tif data[\"details\"].has(\"who\"):\n\t\tprint(data[\"details\"][\"who\"])\n\tvar step = data.get(\"step\", 0)\n\tprint(step.size())\n\n\nfunc passives() -> Array:\n\treturn []\n\n\nfunc total() -> int:\n\tvar sum = 0\n\tfor ability in passives():\n\t\tsum = ability.added_to(sum)\n\treturn sum\n\n\nfunc _ready() -> void:\n\tfill({})\n\tprint(total())\n");
	CHECK(migration.withdrawn == 0);
	CHECK(migration.errors.size() == 4);
}

TEST_CASE("an error of an operation with a Variant operand leaves the slots of the other operand") {
	FakeProject project;
	project.add_script("res://g.gd", "extends Node\nvar data: Dictionary = {}\n\n\nfunc count():\n\treturn 3\n\n\nfunc _ready() -> void:\n\tvar n = count()\n\tn += data[\"k\"]\n\tif count() == 0 and data[\"b\"]:\n\t\tprint(count() >= data[\"n\"])\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://g.gd" }, 10);
	CHECK(migration.texts.at("res://g.pet") == "extends Node\nvar data: Dictionary = {}\n\n\nfunc count() -> int:\n\treturn 3\n\n\nfunc _ready() -> void:\n\tvar n = count()\n\tn += data[\"k\"]\n\tif count() == 0 and data[\"b\"]:\n\t\tprint(count() >= data[\"n\"])\n");
	CHECK(migration.withdrawn == 0);
	CHECK(migration.errors.size() == 3);
}

TEST_CASE("a call by name flows into every function so named, and the report marks the type it gives") {
	FakeProject project;
	project.add_script("res://enemy.gd", "extends Node\n\n\nfunc take_damage(amount):\n\tprint(amount)\n");
	project.add_script("res://bullet.gd", "extends Node\n\n\nfunc hit(body: Node) -> void:\n\tbody.take_damage(5)\n\n\nfunc graze(body: Node?) -> void:\n\tbody.take_damage(1)\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://enemy.gd", "res://bullet.gd" }, 10);
	CHECK(migration.texts.at("res://enemy.pet") == "extends Node\n\n\nfunc take_damage(amount: int):\n\tprint(amount)\n");
	CHECK(migration.partial == 1);
	REQUIRE(migration.notes.size() == 1);
	CHECK(migration.notes[0].message == "The type of \"amount\", int, rests on calls by name: res://bullet.pet:5, res://bullet.pet:9.");
}

TEST_CASE("a Variant says nothing of a slot's type, and one that does not fit is an error where it flows in") {
	FakeProject project;
	project.add_script("res://audio.gd", "extends Node\nvar data: Dictionary = {}\n\n\nfunc play(sample_name, extra):\n\tprint(sample_name, extra)\n\n\nfunc _ready() -> void:\n\tplay(\"click\", data[\"a\"])\n\tplay(data[\"k\"], data[\"b\"])\n\tvar label\n\tlabel = \"a\"\n\tlabel = data[\"k\"]\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://audio.gd" }, 10);
	CHECK(migration.texts.at("res://audio.pet") == "extends Node\nvar data: Dictionary = {}\n\n\nfunc play(sample_name: String, extra: Variant):\n\tprint(sample_name, extra)\n\n\nfunc _ready() -> void:\n\tplay(\"click\", data[\"a\"])\n\tplay(data[\"k\"], data[\"b\"])\n\tvar label: String\n\tlabel = \"a\"\n\tlabel = data[\"k\"]\n");
	CHECK(migration.partial == 2);
	REQUIRE(migration.notes.size() == 2);
	CHECK(migration.notes[0].message == "The type of \"sample_name\", String, leaves out the Variant values at res://audio.pet:11.");
	CHECK(migration.notes[1].message == "The type of \"label\", String, leaves out the Variant values at res://audio.pet:14.");
	REQUIRE(migration.errors.size() == 2);
	CHECK(contains(migration.errors[0].message, "Cannot pass a value of type Variant as String."));
	CHECK(contains(migration.errors[1].message, "Cannot assign a value of type Variant as String."));
}

TEST_CASE("a signal connected to a method flows into its parameters, as do the arguments bound to it") {
	FakeProject project;
	project.add_script("res://score.gd", "extends Node\nsignal scored(points: int)\n\n\nfunc _ready() -> void:\n\tscored.connect(_on_scored)\n\tscored.connect(_on_scored_by.bind(\"me\"))\n\n\nfunc _on_scored(points):\n\tprint(points)\n\n\nfunc _on_scored_by(points, who):\n\tprint(points, who)\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://score.gd" }, 10);
	CHECK(migration.texts.at("res://score.pet") == "extends Node\nsignal scored(points: int)\n\n\nfunc _ready() -> void:\n\tscored.connect(_on_scored)\n\tscored.connect(_on_scored_by.bind(\"me\"))\n\n\nfunc _on_scored(points: int):\n\tprint(points)\n\n\nfunc _on_scored_by(points: int, who: String):\n\tprint(points, who)\n");
	CHECK(migration.errors.empty());
}

// As material-maker reads a request's result (addons/material_maker/engine/loader.gd:81).
TEST_CASE("a literal index of what awaiting a signal with several parameters gives becomes the field it names") {
	FakeProject project;
	project.add_script("res://loader.gd", "extends Node\n\n\nfunc fetch(http: HTTPRequest) -> bool:\n\tvar result = await http.request_completed\n\tif result[0] != 0 or result[1] != 200:\n\t\treturn false\n\tprint(result[3].get_string_from_ascii())\n\treturn true\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://loader.gd" }, 10);
	CHECK(migration.texts.at("res://loader.pet") == "extends Node\n\n\nasync func fetch(http: HTTPRequest) -> bool:\n\tvar result = await http.request_completed\n\tif result.result != 0 or result.response_code != 200:\n\t\treturn false\n\tprint(result.body.get_string_from_ascii())\n\treturn true\n");
	CHECK(migration.fixes.at(compiler::FixKind::ArgumentField) == 3);
	CHECK(migration.errors.empty());
}

// As godot-tiny-mmo reads a request's result (source/client/gateway/gateway.gd:342)
// and godots returns it (src/http_client.gd:29).
TEST_CASE("a local written Array for what awaiting a signal with several parameters gives drops the type, and is an Array() where an Array goes") {
	FakeProject project;
	project.add_script("res://gateway.gd", "extends Node\n\n\nfunc fetch(http: HTTPRequest) -> int:\n\tvar args: Array = await http.request_completed\n\tvar result: int = args[0]\n\tvar body: PackedByteArray = args[3]\n\treturn result + body.size()\n\n\nfunc whole(http: HTTPRequest) -> Array:\n\tvar response: Array = await http.request_completed\n\treturn response\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://gateway.gd" }, 10);
	CHECK(migration.texts.at("res://gateway.pet") == "extends Node\n\n\nasync func fetch(http: HTTPRequest) -> int:\n\tvar args = await http.request_completed\n\tvar result: int = args.result\n\tvar body: PackedByteArray = args.body\n\treturn result + body.size()\n\n\nasync func whole(http: HTTPRequest) -> Array:\n\tvar response = await http.request_completed\n\treturn Array(response)\n");
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
