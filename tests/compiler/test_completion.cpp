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
	return project;
}

// What completion offers at ‸ in `p_source`, the script res://test.pet.
std::vector<CompletionItem> complete(const std::string &p_source) {
	const std::string mark = "‸";
	std::string source = p_source;
	const size_t at = source.find(mark);
	REQUIRE(at != std::string::npos);
	source.replace(at, mark.size(), "\xEF\xBF\xBF");
	const FakeProject classes = project();
	compiler::Analysis analysis(fake_engine(), classes);
	return compiler::complete(analysis, fake_engine(), "res://test.pet", source);
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
	INFO("wrong: " << joined(wrong));
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

TEST_CASE("completion works on the recovered syntax tree, with syntax errors elsewhere") {
	CHECK(offers(complete(in_function("var := 1\ne.‸\nvar y := (", "func f(e: Enemy) -> void:")), { "hp" }));
}
