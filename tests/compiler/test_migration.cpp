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

TEST_CASE("the loop of fixes stops after its rounds") {
	FakeProject project;
	project.add_script("res://unit.gd", "extends Node\n\n\nfunc wait() -> void:\n\tawait ready\n");
	const compiler::Migration migration = compiler::migrate(fake_engine(), project, { "res://unit.gd" }, 0);
	CHECK(!migration.settled);
	CHECK(migration.rounds == 0);
	CHECK(migration.errors.size() == 1);
}
