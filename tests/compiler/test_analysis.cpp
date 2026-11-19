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

// The analysis of a project's files, through a workspace with nothing open.
struct Checked {
	compiler::Workspace workspace;
	compiler::Analysis analysis;

	explicit Checked(const compiler::ProjectApi &p_project) :
			workspace(p_project), analysis(fake_engine(), workspace) {}
};

// Compiles each script from the workspace's sources, in one pass.
std::vector<Result> compile_all(compiler::Analysis &r_analysis, const std::vector<std::string> &p_paths) {
	const compiler::Analysis::Pass pass(r_analysis);
	std::vector<Result> results;
	for (const std::string &path : p_paths) {
		results.push_back(r_analysis.compile(path, *r_analysis.project_api().script_source(path)));
	}
	return results;
}

// The same after the files changed on disk, which the editor learns from its
// events (architecture §2.1).
std::vector<Result> compile_all(Checked &r_checked, const std::vector<std::string> &p_paths) {
	r_checked.workspace.disk_changed();
	return compile_all(r_checked.analysis, p_paths);
}

} // namespace

TEST_CASE("a script is checked again only when what it read changes") {
	FakeProject project;
	project.add_script("res://base.pet", BASE, "Base");
	project.add_script("res://user.pet", USER);
	Checked checked(project);
	compiler::Analysis &analysis = checked.analysis;
	const std::vector<std::string> paths = { "res://base.pet", "res://user.pet" };
	const std::vector<Result> before = compile_all(checked, paths);
	REQUIRE(before[0]->ok());
	REQUIRE(before[1]->ok());
	REQUIRE(analysis.checks_run() == 2);

	SUBCASE("nothing") {
		const std::vector<Result> after = compile_all(checked, paths);
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
		const std::vector<Result> after = compile_all(checked, paths);
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
		const std::vector<Result> after = compile_all(checked, paths);
		CHECK((after[1] != before[1]));
		CHECK_FALSE(after[1]->ok());
	}
	SUBCASE("a syntax error in a function body of a script it uses") {
		project.sources["res://base.pet"] = R"(class_name Base
extends Node
pub func damage(amount: int) -> int:
	return amount *
)";
		const std::vector<Result> after = compile_all(checked, paths);
		CHECK((after[1] != before[1]));
		CHECK_FALSE(after[0]->ok());
		CHECK(after[1]->ok());
	}
	SUBCASE("its own function body") {
		project.sources["res://user.pet"] = R"(extends Node
func hit(base: Base) -> int:
	return base.damage(4)
)";
		const std::vector<Result> after = compile_all(checked, paths);
		CHECK((after[0] == before[0]));
		CHECK((after[1] != before[1]));
		CHECK(analysis.checks_run() == 3);
	}
	SUBCASE("the class a global name refers to") {
		project.add_script("res://other.pet", "class_name Base\nextends Node\n", "Base");
		const std::vector<Result> after = compile_all(checked, paths);
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
	Checked checked(project);
	compiler::Analysis &analysis = checked.analysis;
	const Result before = compile_all(checked, { "res://hud.pet" })[0];
	REQUIRE(before->ok());
	REQUIRE(analysis.checks_run() == 1);

	SUBCASE("what it and the scenes it instances store or connect") {
		compiler::SceneValue value;
		value.type = "int";
		value.integer = 3;
		project.scenes[0].nodes[0].properties = { { "shown", value } };
		project.scenes[0].connections = { { "ready", "Health", ".", "show_health" } };
		project.scenes[1].nodes[1].properties = { { "color", value } };
		CHECK((compile_all(checked, { "res://hud.pet" })[0] == before));
		CHECK(analysis.checks_run() == 1);
	}
	SUBCASE("its nodes") {
		project.scenes[0].nodes[1].type = "Node3D";
		const Result after = compile_all(checked, { "res://hud.pet" })[0];
		CHECK((after != before));
		CHECK_FALSE(after->ok());
	}
	SUBCASE("the nodes of a scene it instances") {
		project.scenes[1].nodes[1].type = "Node3D";
		const Result after = compile_all(checked, { "res://hud.pet" })[0];
		CHECK((after != before));
		CHECK_FALSE(after->ok());
	}
}

TEST_CASE("a scene is checked again only when it, a script on it or its use as a root change") {
	FakeProject project;
	compiler::Scene scene;
	scene.path = "res://enemies.tscn";
	scene.nodes = { { ".", "Node3D", "" }, { "Unit", "Node3D", "res://unit.pet" } };
	project.scenes = { scene };
	project.add_script("res://unit.pet", "@abstract\nextends Node3D\n\nfunc aim() -> float:\n\treturn 1.0\n");
	Checked checked(project);
	compiler::Analysis &analysis = checked.analysis;
	const auto check = [&] {
		checked.workspace.disk_changed();
		const compiler::Analysis::Pass pass(analysis);
		return analysis.check_scene("res://enemies.tscn");
	};
	const Result before = check();
	REQUIRE(before->errors.size() == 1);
	REQUIRE(analysis.checks_run() == 1);

	SUBCASE("nothing") {
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

TEST_CASE("a scene's errors in a script on it move with its lines; a script using its declarations is not checked again (architecture §6.4)") {
	FakeProject project;
	compiler::Scene main;
	main.path = "res://main.tscn";
	main.nodes = { { ".", "Node", "" }, { "HUD", "Node", "res://hud.pet" } };
	compiler::Scene menu;
	menu.path = "res://menu.tscn";
	menu.nodes = { { ".", "Node", "res://hud.pet" }, { "Score", "Label", "" }, { "Ammo", "Label", "" } };
	project.scenes = { main, menu };
	const auto hud = [](const std::string &p_before, const std::string &p_path) {
		return "class_name Hud\nextends Node\n\nfunc aim() -> void:\n\t" + p_before + "\n\npub func show() -> void:\n\tprint(" + p_path + ")\n";
	};
	project.add_script("res://hud.pet", hud("pass", "$Score"), "Hud");
	project.add_script("res://user.pet", "extends Node\n\nfunc f(hud: Hud) -> void:\n\thud.show()\n");
	Checked checked(project);
	compiler::Analysis &analysis = checked.analysis;
	const auto check = [&] {
		checked.workspace.disk_changed();
		const compiler::Analysis::Pass pass(analysis);
		const Result user = analysis.compile("res://user.pet", *project.script_source("res://user.pet"));
		REQUIRE(user->ok());
		return analysis.check_scene("res://main.tscn");
	};
	const Result before = check();
	REQUIRE(before->errors.size() == 1);
	CHECK(before->errors[0].line == 8);
	REQUIRE(analysis.checks_run() == 2);

	SUBCASE("lines before the path") {
		project.sources["res://hud.pet"] = hud("pass\n\tpass", "$Score");
		const Result after = check();
		REQUIRE(after->errors.size() == 1);
		CHECK(after->errors[0].line == 9);
		CHECK(analysis.checks_run() == 3);
	}
	SUBCASE("the path") {
		project.sources["res://hud.pet"] = hud("pass", "get_node(\"Ammo\")");
		const Result after = check();
		REQUIRE(after->errors.size() == 1);
		CHECK(after->errors[0].message == "res://main.tscn: HUD has no node at \"Ammo\".");
		CHECK(analysis.checks_run() == 3);
	}
}

TEST_CASE("a resource file is checked again when it or a script of its resources changes") {
	FakeProject project;
	compiler::Scene file;
	file.path = "res://sword.tres";
	file.resources = { { file.path, "res://item.pet" } };
	project.scenes = { file };
	project.add_script("res://item.pet", "extends Resource\n\n@export pub var icon: Texture2D\n");
	Checked checked(project);
	compiler::Analysis &analysis = checked.analysis;
	const auto check = [&] {
		checked.workspace.disk_changed();
		const compiler::Analysis::Pass pass(analysis);
		return analysis.check_scene("res://sword.tres");
	};
	const Result before = check();
	REQUIRE(before->errors.size() == 1);
	REQUIRE(analysis.checks_run() == 1);

	SUBCASE("nothing") {
		CHECK((check() == before));
		CHECK(analysis.checks_run() == 1);
	}
	SUBCASE("the file") {
		project.scenes[0].resources[0].properties = { { "icon", compiler::SceneValue{ "Object", "CompressedTexture2D" } } };
		CHECK(check()->errors.empty());
	}
	SUBCASE("the script's declarations") {
		project.sources["res://item.pet"] = "extends Resource\n\n@export pub var icon: Texture2D?\n";
		CHECK(check()->errors.empty());
	}
}

TEST_CASE("a script is checked again when the _init of a class it extends changes, not another function body (architecture §6.4)") {
	FakeProject project;
	const std::string base = "class_name Base\nextends Node\n\npub var target: Node\n\nfunc _init():\n";
	project.add_script("res://base.pet", base + "\ttarget = Node.new()\n\nfunc aim() -> void:\n\tpass\n", "Base");
	project.add_script("res://unit.pet", "extends Base\n\nfunc spawn(s: PackedScene[Base]) -> void:\n\tadd_child(s.instantiate())\n");
	Checked checked(project);
	compiler::Analysis &analysis = checked.analysis;
	const Result before = compile_all(checked, { "res://unit.pet" })[0];
	REQUIRE(before->ok());

	SUBCASE("another function body") {
		project.sources["res://base.pet"] = base + "\ttarget = Node.new()\n\nfunc aim() -> void:\n\tprint(1)\n";
		CHECK((compile_all(checked, { "res://unit.pet" })[0] == before));
		CHECK(analysis.checks_run() == 1);
	}
	SUBCASE("_init") {
		project.sources["res://base.pet"] = base + "\tpass\n\nfunc aim() -> void:\n\tpass\n";
		CHECK(contains(messages(*compile_all(checked, { "res://unit.pet" })[0]), "instantiate() needs target = ..., a member of Base that must be assigned and the scene cannot."));
	}
}

TEST_CASE("a script is checked again when a file it preloads adds a preload of it in a function body") {
	FakeProject project;
	project.add_script("res://a.pet", "extends Node\n\nconst B = preload(\"res://b.pet\")\n");
	project.add_script("res://b.pet", "extends Node\n\nfunc make() -> void:\n\tpass\n");
	Checked checked(project);
	REQUIRE(compile_all(checked, { "res://a.pet" })[0]->ok());
	project.sources["res://b.pet"] = "extends Node\n\nfunc make() -> void:\n\tprint(preload(\"res://a.pet\"))\n";
	CHECK(contains(messages(*compile_all(checked, { "res://a.pet" })[0]), "This preload loads this script again (res://b.pet -> res://a.pet)"));
}

// The editor's buffers over the files (architecture §2.1). Versions go into
// the stamps of results used later, so one given twice would let a result of
// another text through.
TEST_CASE("the workspace's revision goes up only when what it gives changes; a version is never given twice") {
	FakeProject project;
	project.add_script("res://a.pet", "extends Node\n");
	compiler::Scene scene;
	scene.path = "res://s.tscn";
	scene.nodes = { { ".", "Node", "" } };
	project.scenes = { scene };
	compiler::Workspace workspace(project);
	const uint64_t start = workspace.revision();
	workspace.set_script("res://a.pet", "extends Node\n");
	workspace.set_scene("res://s.tscn", scene);
	CHECK(workspace.revision() == start);

	workspace.set_script("res://a.pet", "extends Node2D\n");
	const uint64_t edited = workspace.version("res://a.pet");
	CHECK(edited > start);
	workspace.set_script("res://a.pet", "extends Node2D\n");
	CHECK(workspace.revision() == edited);
	workspace.drop_script("res://a.pet");
	const uint64_t closed = workspace.version("res://a.pet");
	CHECK(closed > edited);
	workspace.set_script("res://a.pet", "extends Node2D\n");
	CHECK(workspace.version("res://a.pet") > closed);

	scene.nodes[0].type = "Node2D";
	workspace.set_scene("res://s.tscn", scene);
	const uint64_t unsaved = workspace.version("res://s.tscn");
	CHECK(unsaved > closed);
	workspace.drop_scene("res://s.tscn");
	CHECK(workspace.version("res://s.tscn") > unsaved);

	// Any file not open may have changed on disk; an open one keeps its text.
	workspace.disk_changed();
	CHECK(workspace.version("res://s.tscn") == workspace.revision());
	CHECK(workspace.version("res://a.pet") < workspace.revision());
	CHECK(workspace.script_source("res://a.pet") == "extends Node2D\n");
}

// A file opened with its text on disk after that text changed.
TEST_CASE("a file taken into the workspace keeps the version a change on disk gave it") {
	FakeProject project;
	project.add_script("res://a.pet", "extends Node\n");
	compiler::Workspace workspace(project);
	const compiler::Stamp stamp{ workspace.revision(), { "res://a.pet" } };
	workspace.disk_changed();
	workspace.set_script("res://a.pet", "extends Node\n");
	CHECK(workspace.changed(stamp));
}

TEST_CASE("a result is not read again until the workspace's revision changes") {
	FakeProject project;
	project.add_script("res://base.pet", BASE, "Base");
	project.add_script("res://user.pet", USER);
	Checked checked(project);
	const std::vector<std::string> paths = { "res://user.pet" };
	const Result before = compile_all(checked.analysis, paths)[0];
	REQUIRE(before->ok());

	// A change on disk is seen at its event.
	project.sources["res://base.pet"] = "class_name Base\nextends Node\npub func damage() -> int:\n\treturn 2\n";
	CHECK((compile_all(checked.analysis, paths)[0] == before));
	CHECK(checked.analysis.checks_run() == 1);
	CHECK_FALSE(compile_all(checked, paths)[0]->ok());

	// A buffer's change is seen at once.
	checked.workspace.set_script("res://base.pet", BASE);
	CHECK(compile_all(checked.analysis, paths)[0]->ok());
}

TEST_CASE("the results of a script with no file are forgotten with its text") {
	FakeProject project;
	Checked checked(project);
	checked.workspace.set_script("res://new.pet", "extends Node\n");
	compile_all(checked.analysis, { "res://new.pet" });
	CHECK(checked.analysis.computed("res://new.pet") != 0);
	checked.workspace.drop_script("res://new.pet");
	checked.analysis.forget("res://new.pet");
	CHECK(checked.analysis.computed("res://new.pet") == 0);
}

// A fix of a reused check edits other files at the places they had when it
// was computed.
TEST_CASE("a result reused after the declarations it read moved has a stale stamp") {
	FakeProject project;
	project.add_script("res://base.pet", BASE, "Base");
	project.add_script("res://user.pet", USER);
	Checked checked(project);
	const Result before = compile_all(checked.analysis, { "res://user.pet" })[0];
	const compiler::Stamp stamp{ checked.analysis.computed("res://user.pet"), { "res://user.pet", "res://base.pet" } };
	CHECK_FALSE(checked.workspace.changed(stamp));
	checked.workspace.set_script("res://base.pet", std::string("# Moved down.\n") + BASE);
	CHECK((compile_all(checked.analysis, { "res://user.pet" })[0] == before));
	CHECK(checked.workspace.changed(stamp));
}

namespace {

// Each script compiled in one pass, in order, is as it is compiled alone.
void check_as_alone(const FakeProject &p_project, const std::vector<std::string> &p_paths) {
	Checked checked(p_project);
	const std::vector<Result> together = compile_all(checked.analysis, p_paths);
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
