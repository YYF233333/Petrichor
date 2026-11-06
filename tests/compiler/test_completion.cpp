#include "compiler/navigation.h"
#include "doctest.h"
#include "helpers.h"

using namespace petrichor;
using namespace petrichor::tests;

// Completion in the script editor (spec §13.2): what checks at the cursor.

namespace {

using compiler::CompletionItem;
using compiler::CompletionKind;
using Place = compiler::CompletionItem::Place;

FakeProject project() {
	FakeProject project;
	project.add_script("res://enemy.pet", R"(class_name Enemy
extends Node2D

pub var hp := 3
var target: Node2D?
pub const KIND := 1
pub signal died

pub func hit(amount: int) -> void:
	hp -= amount

func think() -> void:
	pass

func shove(force: int, far: bool = false) -> void:
	pass

pub struct Shot:
	var damage := 1
)",
			"Enemy");
	project.sources["res://legacy.gd"] = "extends Node\nclass_name Legacy\n";
	compiler::ForeignClass legacy;
	legacy.name = "Legacy";
	legacy.native = "Node";
	project.foreign["res://legacy.gd"] = legacy;
	project.global_classes["Legacy"] = "res://legacy.gd";
	// res://test.pet at the root of one scene, and below it in another.
	compiler::Scene level;
	level.path = "res://level.tscn";
	level.nodes = { { ".", "Node2D", "res://test.pet" }, { "Head", "Node3D" }, { "Head/Eye", "Timer" }, { "My Label", "Label" }, { "Hud", "Control" }, { "Hud/Score", "Label" } };
	compiler::Scene arena;
	arena.path = "res://arena.tscn";
	arena.nodes = { { ".", "Node" }, { "Player", "Node2D", "res://test.pet" }, { "Player/Head", "Node3D" }, { "Player/Head/Eye", "Timer" }, { "Player/My Label", "Label" }, { "Player/Gun", "Node2D" }, { "Hud", "Control" }, { "Hud/Score", "Label" } };
	level.nodes[4].unique = arena.nodes[6].unique = true;
	project.scenes = { level, arena };
	return project;
}

// Completion at ‸ in `p_source`, the script res://test.pet.
compiler::Completion completion(const std::string &p_source) {
	const std::string mark = "‸";
	std::string source = p_source;
	const size_t at = source.find(mark);
	REQUIRE(at != std::string::npos);
	source.replace(at, mark.size(), "\xEF\xBF\xBF");
	const FakeProject classes = project();
	compiler::Analysis analysis(fake_engine(), classes);
	return compiler::complete(analysis, fake_engine(), "res://test.pet", source);
}

std::vector<CompletionItem> complete(const std::string &p_source) {
	return completion(p_source).items;
}

// The signature hinted at ‸, the parameter at it in [brackets].
std::string hint(const std::string &p_source) {
	std::string text = completion(p_source).call_hint;
	for (size_t at = text.find("\xEF\xBF\xBF"), count = 0; at != std::string::npos; at = text.find("\xEF\xBF\xBF"), count++) {
		text.replace(at, 3, count % 2 == 0 ? "[" : "]");
	}
	return text;
}

const CompletionItem *find(const std::vector<CompletionItem> &p_items, const std::string &p_name) {
	const auto found = std::find_if(p_items.begin(), p_items.end(), [&](const CompletionItem &p_item) { return p_item.name == p_name; });
	return found == p_items.end() ? nullptr : &*found;
}

// Whether each name is offered or not, as "name" or "-name".
bool offers(const std::vector<CompletionItem> &p_items, const std::vector<std::string> &p_names) {
	std::vector<std::string> wrong;
	for (const std::string &name : p_names) {
		const bool absent = name[0] == '-';
		if ((find(p_items, absent ? name.substr(1) : name) != nullptr) == absent) {
			wrong.push_back(name);
		}
	}
	if (!wrong.empty()) {
		MESSAGE("wrong: " << joined(wrong));
	}
	return wrong.empty();
}

} // namespace

TEST_CASE("after a value, completion offers the members that check there: pub ones of another class and its engine class's") {
	const std::vector<CompletionItem> items = complete(in_function("e.‸", "func f(e: Enemy) -> void:"));
	CHECK(offers(items, { "hp", "hit", "died", "KIND", "position", "add_child", "queue_free", "-target", "-think", "-_ready" }));
	CHECK((find(items, "hp")->kind == CompletionKind::Member && find(items, "hit")->kind == CompletionKind::Function));
	CHECK((find(items, "died")->kind == CompletionKind::Signal && find(items, "KIND")->kind == CompletionKind::Constant));
	// A function goes in with "()" when it takes no arguments.
	CHECK((find(items, "hit")->insert == "hit(" && find(items, "queue_free")->insert == "queue_free()"));
	// Nearest class first: the script's, then Node2D, then Node.
	CHECK((find(items, "hp")->place == Place::Member && find(items, "hp")->depth == 0));
	CHECK(find(items, "position")->depth == 1);
	CHECK(find(items, "add_child")->depth == 2);
	// Each offered value checks where it is offered.
	FakeProject classes = project();
	for (const CompletionItem &item : items) {
		if (item.kind != CompletionKind::Function) {
			compiler::CompileOptions options;
			options.path = "res://test.pet";
			options.project = &classes;
			const compiler::CompileResult result = compiler::compile(in_function("var v = e." + item.name, "func f(e: Enemy) -> void:"), fake_engine(), options);
			CHECK_MESSAGE(result.ok(), (item.name + ": " + messages(result)));
		}
	}
}

TEST_CASE("a value of T? offers its methods until a check narrows it") {
	CHECK(offers(complete(in_function("t.‸", "func f(t: Node2D?) -> void:")), { "unwrap", "expect", "-position", "-map" }));
	CHECK(offers(complete(in_function("if t != None:\n\tt.‸", "func f(t: Node2D?) -> void:")), { "position", "-unwrap" }));
	// Nor does a Variant have members to offer.
	CHECK(complete(in_function("v.‸", "func f(v: Variant) -> void:")).empty());
}

TEST_CASE("bare names are the locals, the class's members and the project's and engine's names; a static function has no instance") {
	const std::string source = R"(extends Node2D

const LIMIT := 3
var speed := 1.0

func f(amount: int) -> void:
	var total := 2
	‸

static func make() -> void:
	pass
)";
	const std::vector<CompletionItem> items = complete(source);
	CHECK(offers(items, { "total", "amount", "speed", "LIMIT", "f", "make", "position", "Enemy", "Node2D", "print", "KEY_ESCAPE", "-hp" }));
	CHECK((find(items, "total")->kind == CompletionKind::Variable && find(items, "total")->place == Place::Local));
	CHECK((find(items, "speed")->place == Place::Member && find(items, "speed")->depth == 0));
	CHECK((find(items, "Enemy")->kind == CompletionKind::Class && find(items, "Enemy")->place == Place::Project));
	CHECK((find(items, "Node2D")->kind == CompletionKind::Class && find(items, "Node2D")->place == Place::Engine));
	CHECK(find(items, "f")->insert == "f(");
	const std::string in_static = source.substr(0, source.find("\tvar total")) + "\tpass\n\nstatic func make() -> void:\n\t‸\n";
	CHECK(offers(complete(in_static), { "LIMIT", "make", "-speed", "-position", "-f", "-self" }));
}

TEST_CASE("keywords are offered where they can be written") {
	const std::vector<CompletionItem> statement = complete(in_function("for i in 3:\n\tif i > 1:\n\t\tpass\n\t‸"));
	CHECK(offers(statement, { "var", "if", "return", "break", "continue", "elif", "else", "self", "true", "-await" }));
	CHECK(find(statement, "var")->insert == "var ");
	CHECK(offers(complete(in_function("var x := ‸")), { "true", "None", "not", "self", "-var", "-break", "-elif" }));
	CHECK(offers(complete(in_function("pass\n‸", "async func f() -> void:")), { "await", "-break", "-elif" }));
}

TEST_CASE("a type is completed with types only, also after a class's name and after extends") {
	CHECK(offers(complete(in_function("var total := 1\nvar x: ‸")), { "Node2D", "int", "Enemy", "Key", "Variant", "-total", "-print", "-void" }));
	CHECK(offers(complete("extends Node\n\nfunc g() -> ‸:\n\tpass\n"), { "void", "Node2D" }));
	const std::vector<CompletionItem> inner = complete(in_function("var s: Enemy.‸"));
	CHECK((inner.size() == 1 && inner[0].name == "Shot"));
	CHECK(offers(complete("extends ‸\n"), { "Node2D", "Enemy", "-Legacy", "-Key" }));
}

TEST_CASE("among the arguments of a call, the signature of the innermost is hinted, with the parameter at the cursor marked") {
	CHECK(hint("extends Node\n\nfunc hit(amount: int, crit: bool = false) -> void:\n\tpass\n\nfunc f() -> void:\n\thit(1, ‸)\n") == "hit(amount: int, [crit: bool = false]) -> void");
	CHECK(hint(in_function("e.hit(‸)", "func f(e: Enemy) -> void:")) == "hit([amount: int]) -> void");
	CHECK(hint(in_function("set_process(‸)")) == "set_process([enable: bool]) -> void");
	CHECK(hint(in_function("print(lerpf(1.0, ‸))")) == "lerpf(from: float, [to: float], weight: float) -> float");
	// Of the constructors, those that take an argument there.
	CHECK(hint(in_function("var v := Vector2(1.0, ‸)")) == "Vector2(x: float, [y: float]) -> Vector2");
	// A struct's fields go by name.
	CHECK(hint(in_function("var s := Enemy.Shot(‸)")) == "Shot(damage: int = 1) -> Enemy.Shot");
	// Not in the name.
	CHECK(hint(in_function("set_pro‸(true)")).empty());
}

TEST_CASE("after $ and %, completion offers the paths that reach a node from the script in every scene, quoted where they must be") {
	const std::vector<CompletionItem> items = complete(in_function("$‸"));
	CHECK(offers(items, { "Head", "Head/Eye", "My Label", "%Hud", "%Hud/Score", "-Gun", "-Hud", "-Player" }));
	CHECK((find(items, "My Label")->insert == "\"My Label\"" && find(items, "Head/Eye")->kind == CompletionKind::NodePath));
	// The editor replaces the word back to "%", or within the string.
	const std::vector<CompletionItem> unique = complete(in_function("%‸"));
	CHECK((unique.size() == 2 && unique[0].name == "Hud" && unique[1].name == "Hud/Score"));
	CHECK(find(complete(in_function("$Head/E‸")), "Head/Eye"));
	CHECK(find(complete(in_function("$\"My‸\"")), "My Label")->insert == "My Label");
	CHECK(complete(in_function("$‸", "static func f() -> void:")).empty());
}

TEST_CASE("after func, completion offers what the function can override, with the parameters and without types") {
	const std::vector<CompletionItem> items = complete("extends Enemy\n\nfunc think() -> void:\n\tpass\n\nfunc ‸\n");
	CHECK(offers(items, { "shove", "_process", "_init", "_notification", "-think", "-hit", "-get_child", "-_static_init", "-_has_point" }));
	CHECK((find(items, "shove")->insert == "shove(force, far = false):" && find(items, "_process")->insert == "_process(delta):"));
	CHECK((find(items, "shove")->depth == 1 && find(items, "_process")->depth == 3));
	// With the modifiers of what it overrides.
	CHECK(offers(complete("extends Enemy\n\npub func ‸\n"), { "hit", "-shove", "-think" }));
	CHECK(offers(complete("extends Enemy\n\nstatic func ‸\n"), { "_static_init", "-shove", "-_process" }));
	CHECK(offers(complete("extends Node\n\nasync func ‸\n"), { "_process", "-_to_string" }));
}

TEST_CASE("after @, completion offers the annotations that the place takes") {
	const std::vector<CompletionItem> header = complete("@‸\nextends Node\n");
	CHECK(offers(header, { "icon", "static_unload", "abstract", "export", "export_range", "onready", "warning_ignore", "-tool", "-parent", "-readonly" }));
	CHECK((find(header, "icon")->insert == "icon(" && find(header, "onready")->insert == "onready" && find(header, "export_range")->insert == "export_range("));
	CHECK(offers(complete("extends Node\n\nvar a := 1\n@‸\nvar b := 2\n"), { "export", "rpc", "export_group", "-icon", "-static_unload" }));
	CHECK(offers(complete(in_function("@‸\nvar x := 1")), { "warning_ignore", "warning_ignore_start", "-export", "-onready" }));
	const std::vector<CompletionItem> property = complete("extends Node\n\ninterface Named:\n\t@‸\n\tvar name: String\n");
	CHECK((property.size() == 1 && property[0].name == "readonly"));
}

TEST_CASE("completion works on the recovered syntax tree, with syntax errors elsewhere") {
	CHECK(offers(complete(in_function("var := 1\ne.‸\nvar y := (", "func f(e: Enemy) -> void:")), { "hp" }));
}
