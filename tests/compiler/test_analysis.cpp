#include "bytecode/module.h"
#include "compiler/analysis.h"
#include "doctest.h"
#include "helpers.h"

using namespace petrichor;
using namespace petrichor::tests;

// The analysis checks a script again only when its source, the declarations of
// the scripts it uses or the project's answers change (architecture §2.1).

namespace {

using Result = std::shared_ptr<const compiler::CompileResult>;

const char *const BASE = R"(class_name Base
extends Node
pub func damage(amount: int) -> int:
	return amount * 2
)";

const char *const USER = R"(extends Node
func hit(base: Base) -> int:
	return base.damage(3)
)";

// Compiles each script from the project's sources, in one pass.
std::vector<Result> compile_all(compiler::Analysis &r_analysis, const FakeProject &p_project, const std::vector<std::string> &p_paths) {
	const compiler::Analysis::Pass pass(r_analysis);
	std::vector<Result> results;
	for (const std::string &path : p_paths) {
		results.push_back(r_analysis.compile(path, *p_project.script_source(path)));
	}
	return results;
}

} // namespace

TEST_CASE("a script is checked again only when what it read changes") {
	FakeProject project;
	project.add_script("res://base.pet", BASE, "Base");
	project.add_script("res://user.pet", USER);
	compiler::Analysis analysis(fake_engine(), project);
	const std::vector<std::string> paths = { "res://base.pet", "res://user.pet" };
	const std::vector<Result> before = compile_all(analysis, project, paths);
	REQUIRE(before[0]->ok());
	REQUIRE(before[1]->ok());
	REQUIRE(analysis.checks_run() == 2);

	SUBCASE("nothing") {
		const std::vector<Result> after = compile_all(analysis, project, paths);
		CHECK((after == before));
		CHECK(analysis.checks_run() == 2);
	}
	SUBCASE("a function body of a script it uses, also with a deeper block at its end") {
		project.sources["res://base.pet"] = R"(class_name Base
extends Node
pub func damage(amount: int) -> int:
	if amount > 0:
		# Doubled.
		return amount * 2
	return 0
)";
		const std::vector<Result> after = compile_all(analysis, project, paths);
		CHECK((after[0] != before[0]));
		CHECK((after[1] == before[1]));
		CHECK(analysis.checks_run() == 3);
	}
	SUBCASE("a declaration it uses") {
		project.sources["res://base.pet"] = R"(class_name Base
extends Node
pub func damage(amount: int, scale: int) -> int:
	return amount * scale
)";
		const std::vector<Result> after = compile_all(analysis, project, paths);
		CHECK((after[1] != before[1]));
		CHECK_FALSE(after[1]->ok());
	}
	SUBCASE("a syntax error in a function body of a script it uses") {
		project.sources["res://base.pet"] = R"(class_name Base
extends Node
pub func damage(amount: int) -> int:
	return amount *
)";
		const std::vector<Result> after = compile_all(analysis, project, paths);
		CHECK((after[1] != before[1]));
		CHECK_FALSE(after[0]->ok());
		CHECK(after[1]->ok());
	}
	SUBCASE("its own function body") {
		project.sources["res://user.pet"] = R"(extends Node
func hit(base: Base) -> int:
	return base.damage(4)
)";
		const std::vector<Result> after = compile_all(analysis, project, paths);
		CHECK((after[0] == before[0]));
		CHECK((after[1] != before[1]));
		CHECK(analysis.checks_run() == 3);
	}
	SUBCASE("the class a global name refers to") {
		project.add_script("res://other.pet", "class_name Base\nextends Node\n", "Base");
		const std::vector<Result> after = compile_all(analysis, project, paths);
		CHECK((after[1] != before[1]));
		CHECK_FALSE(after[1]->ok());
	}
}

TEST_CASE("a script is checked again only when the nodes of a scene that uses it change") {
	FakeProject project;
	compiler::Scene scene;
	scene.path = "res://hud.tscn";
	scene.nodes = { { ".", "CanvasLayer", "res://hud.pet" }, { "Health", "Label", "" }, { "Bar", "Control", "", true, "", false, {}, "res://bar.tscn" } };
	compiler::Scene bar;
	bar.path = "res://bar.tscn";
	bar.nodes = { { ".", "Control", "" }, { "Fill", "TextureRect", "" } };
	project.scenes = { scene, bar };
	project.add_script("res://hud.pet", "extends CanvasLayer\n\npub var shown := 0\n\npub func show_health():\n\t$Health.text = \"full\"\n\t$Bar/Fill.z_index = 1\n");
	compiler::Analysis analysis(fake_engine(), project);
	const Result before = compile_all(analysis, project, { "res://hud.pet" })[0];
	REQUIRE(before->ok());
	REQUIRE(analysis.checks_run() == 1);

	SUBCASE("what it and the scenes it instances store or connect") {
		compiler::SceneValue value;
		value.type = "int";
		value.integer = 3;
		project.scenes[0].nodes[0].properties = { { "shown", value } };
		project.scenes[0].connections = { { "ready", "Health", ".", "show_health" } };
		project.scenes[1].nodes[1].properties = { { "color", value } };
		CHECK((compile_all(analysis, project, { "res://hud.pet" })[0] == before));
		CHECK(analysis.checks_run() == 1);
	}
	SUBCASE("its nodes") {
		project.scenes[0].nodes[1].type = "Node3D";
		const Result after = compile_all(analysis, project, { "res://hud.pet" })[0];
		CHECK((after != before));
		CHECK_FALSE(after->ok());
	}
	SUBCASE("the nodes of a scene it instances") {
		project.scenes[1].nodes[1].type = "Node3D";
		const Result after = compile_all(analysis, project, { "res://hud.pet" })[0];
		CHECK((after != before));
		CHECK_FALSE(after->ok());
	}
}

TEST_CASE("a scene is checked again only when it, the declarations of a script on it or its use as a root change") {
	FakeProject project;
	compiler::Scene scene;
	scene.path = "res://enemies.tscn";
	scene.nodes = { { ".", "Node3D", "" }, { "Unit", "Node3D", "res://unit.pet" } };
	project.scenes = { scene };
	project.add_script("res://unit.pet", "@abstract\nextends Node3D\n\nfunc aim() -> float:\n\treturn 1.0\n");
	compiler::Analysis analysis(fake_engine(), project);
	const auto check = [&] {
		const compiler::Analysis::Pass pass(analysis);
		return analysis.check_scene("res://enemies.tscn");
	};
	const Result before = check();
	REQUIRE(before->errors.size() == 1);
	REQUIRE(analysis.checks_run() == 1);

	SUBCASE("nothing, or a function body of the script") {
		project.sources["res://unit.pet"] = "@abstract\nextends Node3D\n\nfunc aim() -> float:\n\treturn 2.0\n";
		CHECK((check() == before));
		CHECK(analysis.checks_run() == 1);
	}
	SUBCASE("the script's declarations") {
		project.sources["res://unit.pet"] = "extends Node3D\n\nfunc aim() -> float:\n\treturn 1.0\n";
		CHECK(check()->errors.empty());
	}
	SUBCASE("the scene") {
		project.scenes[0].nodes[1].script.clear();
		CHECK(check()->errors.empty());
	}
	SUBCASE("its use as a root") {
		project.sources["res://spawner.gd"] = "extends Node\n\nfunc f():\n\tload(\"res://enemies.tscn\")\n";
		check();
		CHECK(analysis.checks_run() == 2);
	}
}

TEST_CASE("a script is checked again when the _init of a class it extends changes, not another function body (architecture §6.4)") {
	FakeProject project;
	compiler::Scene scene;
	scene.path = "res://squad.tscn";
	scene.nodes = { { ".", "Node", "" }, { "Unit", "Node", "res://unit.pet" } };
	project.scenes = { scene };
	const std::string base = "class_name Base\nextends Node\n\npub var target: Node\n\nfunc _init():\n";
	project.add_script("res://base.pet", base + "\ttarget = Node.new()\n\nfunc aim() -> void:\n\tpass\n", "Base");
	project.add_script("res://unit.pet", "extends Base\n\nfunc _ready():\n\tprint(target)\n");
	compiler::Analysis analysis(fake_engine(), project);
	const Result before = compile_all(analysis, project, { "res://unit.pet" })[0];
	REQUIRE(before->ok());

	SUBCASE("another function body") {
		project.sources["res://base.pet"] = base + "\ttarget = Node.new()\n\nfunc aim() -> void:\n\tprint(1)\n";
		CHECK((compile_all(analysis, project, { "res://unit.pet" })[0] == before));
		CHECK(analysis.checks_run() == 1);
	}
	SUBCASE("_init") {
		project.sources["res://base.pet"] = base + "\tpass\n\nfunc aim() -> void:\n\tpass\n";
		CHECK(contains(messages(*compile_all(analysis, project, { "res://unit.pet" })[0]), "res://squad.tscn: Unit reads \"target\" in _ready"));
	}
}

TEST_CASE("a script is checked again when a file it preloads adds a preload of it in a function body") {
	FakeProject project;
	project.add_script("res://a.pet", "extends Node\n\nconst B = preload(\"res://b.pet\")\n");
	project.add_script("res://b.pet", "extends Node\n\nfunc make() -> void:\n\tpass\n");
	compiler::Analysis analysis(fake_engine(), project);
	REQUIRE(compile_all(analysis, project, { "res://a.pet" })[0]->ok());
	project.sources["res://b.pet"] = "extends Node\n\nfunc make() -> void:\n\tprint(preload(\"res://a.pet\"))\n";
	CHECK(contains(messages(*compile_all(analysis, project, { "res://a.pet" })[0]), "This preload loads this script again (res://b.pet -> res://a.pet)"));
}

namespace {

// Each script compiled in one pass, in order, is as it is compiled alone.
void check_as_alone(const FakeProject &p_project, const std::vector<std::string> &p_paths) {
	compiler::Analysis analysis(fake_engine(), p_project);
	const std::vector<Result> together = compile_all(analysis, p_project, p_paths);
	for (size_t i = 0; i < p_paths.size(); i++) {
		compiler::CompileOptions options;
		options.path = p_paths[i];
		options.project = &p_project;
		const compiler::CompileResult alone = compiler::compile(*p_project.script_source(p_paths[i]), fake_engine(), options);
		INFO(p_paths[i], messages(alone));
		CHECK(alone.ok());
		CHECK(messages(*together[i]) == messages(alone));
		CHECK(bytecode::serialize(together[i]->module) == bytecode::serialize(alone.module));
	}
}

} // namespace

TEST_CASE("a base class has the types its scenes give, also after a script used it without") {
	FakeProject project;
	compiler::Scene scene;
	scene.path = "res://panel.tscn";
	scene.nodes = { { ".", "Node", "res://derived.pet" }, { "Label", "Label", "" } };
	project.scenes = { scene };
	project.add_script("res://base.pet", "class_name Base\nextends Node\npub var count := 0\n@onready pub var label = $Label\n", "Base");
	project.add_script("res://user.pet", "extends Node\nfunc read(base: Base):\n\tvar shown = base.count\n");
	project.add_script("res://derived.pet", "extends Base\nfunc show():\n\tlabel.text = \"shown\"\n");
	check_as_alone(project, { "res://user.pet", "res://derived.pet" });
}

TEST_CASE("scripts that use each other compile in one pass as each does alone") {
	FakeProject project;
	project.add_script("res://a.pet", R"(class_name A
extends Node
pub var count: int = 1
pub func value(other: B) -> int:
	return other.twice() + count
)",
			"A");
	project.add_script("res://b.pet", R"(class_name B
extends A
pub func twice() -> int:
	return count * 2
pub func thrice() -> int:
	return value(self)
)",
			"B");
	check_as_alone(project, { "res://a.pet", "res://b.pet", "res://a.pet" });
}
