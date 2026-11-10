#include "compiler/ast.h"
#include "compiler/navigation.h"
#include "compiler/script_classes.h"
#include "doctest.h"
#include "helpers.h"

using namespace petrichor;
using namespace petrichor::tests;

// The script editor's lookup, which going to definitions and hover use, and
// the documentation hover shows (spec §13.2).

namespace {

// As the engine names the documentation of a script's class: by its
// class_name, else by its quoted path.
compiler::DocName doc_names(const FakeProject &p_project) {
	return [&p_project](const std::string &p_file) {
		for (const auto &[name, path] : p_project.global_classes) {
			if (path == p_file) {
				return name;
			}
		}
		return "\"" + p_file.substr(6) + "\"";
	};
}

FakeProject project() {
	FakeProject project;
	project.add_script("res://hit.pet", "class_name Hit\nextends RefCounted\n\npub var damage := 1\n\npub class Crit:\n\tpub const FACTOR := 2\n", "Hit");
	project.sources["res://legacy_base.gd"] = "extends Node\n\n@export_range(0, 10) var speed := 2.0\n";
	project.sources["res://legacy.gd"] = "extends \"res://legacy_base.gd\"\nclass_name Legacy\n";
	compiler::ForeignClass base;
	base.native = "Node";
	project.foreign["res://legacy_base.gd"] = base;
	compiler::ForeignClass legacy;
	legacy.native = "Node";
	legacy.base = "res://legacy_base.gd";
	legacy.properties = { { "speed", compiler::Type::of(compiler::TypeKind::Float), true, "res://legacy.gd" } };
	project.foreign["res://legacy.gd"] = legacy;
	project.global_classes["Legacy"] = "res://legacy.gd";
	return project;
}

const std::string SOURCE = R"(extends Node3D

const LIMIT := 3

func f(hit: Hit, l: Legacy, timer: Timer) -> void:
	const MAX := 2
	var total := hit.damage + Hit.Crit.FACTOR + LIMIT + MAX
	print(total, l.speed, timer.timeout)
	position = Vector3.ZERO
	var e: Tween.EaseType = Tween.EASE_IN
	var a: Vector3.Axis = Vector3.Axis(0)
	var k: Key = KEY_ESCAPE
	## Which way.
	@warning_ignore("unused_variable")
	var along := a
)";

// The lookup at the second byte of the nth `p_text` of SOURCE.
std::optional<compiler::Lookup> find(const std::string &p_text, int p_nth = 0) {
	size_t offset = SOURCE.find(p_text);
	for (int i = 0; i < p_nth; i++) {
		offset = SOURCE.find(p_text, offset + 1);
	}
	REQUIRE(offset != std::string::npos);
	const FakeProject classes = project();
	compiler::Analysis analysis(fake_engine(), classes);
	const compiler::Analysis::Pass pass(analysis);
	const auto compiled = analysis.compile("res://test.pet", SOURCE);
	REQUIRE_MESSAGE(compiled->ok(), messages(*compiled));
	return compiler::lookup(analysis, fake_engine(), classes, doc_names(classes), "res://test.pet", SOURCE, uint32_t(offset + 1));
}

// The lookup as "kind class.member type path:line".
std::string lookup(const std::string &p_text, int p_nth = 0) {
	const std::optional<compiler::Lookup> found = find(p_text, p_nth);
	if (!found) {
		return "none";
	}
	static const char *KINDS[] = { "class", "constant", "property", "method", "signal", "enum", "local", "local constant" };
	const std::string name = found->member.empty() ? found->class_name : found->class_name + "." + found->member;
	const std::string where = found->path.empty() ? std::string() : found->path + ":" + std::to_string(found->line);
	std::string text = KINDS[int(found->kind)];
	for (const std::string &part : { name, found->type, where }) {
		if (!part.empty()) {
			text += " " + part;
		}
	}
	return text;
}

// Each member as "name(arguments): type [enum] = value qualifiers".
std::vector<std::string> described(const std::vector<compiler::DocMember> &p_members) {
	std::vector<std::string> result;
	for (const compiler::DocMember &member : p_members) {
		std::string text = member.name;
		if (!member.arguments.empty()) {
			const std::vector<std::string> arguments = described(member.arguments);
			text += "(";
			for (size_t i = 0; i < arguments.size(); i++) {
				text += (i > 0 ? ", " : "") + arguments[i];
			}
			text += ")";
		}
		text += member.type.empty() ? "" : ": " + member.type;
		text += member.enumeration.empty() ? "" : " [" + member.enumeration + "]";
		text += member.value.empty() ? "" : " = " + member.value;
		text += member.qualifiers.empty() ? "" : " " + member.qualifiers;
		result.push_back(text);
	}
	return result;
}

bool has(const std::vector<compiler::DocMember> &p_members, const std::string &p_described) {
	const std::vector<std::string> all = described(p_members);
	INFO(joined(all));
	return std::find(all.begin(), all.end(), p_described) != all.end();
}

// The documentation of the script res://doc.pet of `p_source`, whose
// declarations have no errors.
std::vector<compiler::ClassDoc> documentation(const std::string &p_source, const std::string &p_class_name = std::string()) {
	FakeProject classes = project();
	classes.add_script("res://doc.pet", p_source, p_class_name);
	compiler::CompileOptions options;
	options.path = "res://doc.pet";
	options.project = &classes;
	options.declarations_only = true;
	compiler::ScriptClasses scripts(fake_engine(), &classes, true);
	compiler::ClassDecl decl;
	const compiler::CompileResult result = compiler::compile(p_source, fake_engine(), options, scripts, decl);
	REQUIRE_MESSAGE(result.ok(), messages(result));
	return compiler::document(decl, "res://doc.pet", doc_names(classes));
}

const compiler::DocComment &comment_of(const std::vector<compiler::DocMember> &p_members, const std::string &p_name) {
	const auto member = std::find_if(p_members.begin(), p_members.end(), [&](const compiler::DocMember &p_member) {
		return p_member.name == p_name;
	});
	REQUIRE(member != p_members.end());
	return member->comment;
}

} // namespace

TEST_CASE("a local or parameter shows its type and leads to its declaration") {
	CHECK(lookup("total", 1) == "local int res://test.pet:7");
	CHECK(lookup("MAX", 1) == "local constant int res://test.pet:6");
	CHECK(lookup("hit", 1) == "local Hit res://test.pet:5");
	// With its comment, which may be above its annotations (spec §2.2).
	CHECK(find("along")->comment.description == "Which way.");
}

TEST_CASE("a declaration of this language leads to its file and line and to its documentation, also in an inner class") {
	CHECK(lookup("LIMIT", 1) == "constant \"test.pet\".LIMIT res://test.pet:3");
	CHECK(lookup("damage") == "property Hit.damage res://hit.pet:4");
	CHECK(lookup("Crit") == "class Hit.Crit res://hit.pet:6");
	CHECK(lookup("FACTOR") == "constant Hit.Crit.FACTOR res://hit.pet:7");
}

TEST_CASE("a member of a GDScript class leads to the script of its base that declares it") {
	CHECK(lookup("speed") == "property \"legacy_base.gd\".speed res://legacy_base.gd:3");
	CHECK(lookup("Legacy") == "class Legacy res://legacy.gd:1");
}

TEST_CASE("the documentation has every member of every class of a script, the script's class last") {
	const std::string source = R"(class_name Doc
extends Node

pub signal hit(amount: int)
const LIMIT := 3
enum State { IDLE, RUN = 4 }
@export pub var speed := 2.5
var target: Node3D?
var states: Array[State] = []
static var count := 0
var hp: int:
	get:
		return 1

pub static func make(x: int = 1) -> Doc:
	return null

async func wait() -> int:
	return 1

struct Shot:
	var damage := 1

class Inner extends Node3D:
	pub var depth := 2
)";
	const std::vector<compiler::ClassDoc> docs = documentation(source, "Doc");
	REQUIRE(docs.size() == 3);
	CHECK((docs[0].name == "Doc.Inner" && docs[0].inherits == "Node3D" && has(docs[0].properties, "depth: int = 2")));
	CHECK((docs[1].name == "Doc.Shot" && has(docs[1].properties, "damage: int = 1")));
	const compiler::ClassDoc &doc = docs[2];
	CHECK((doc.name == "Doc" && doc.inherits == "Node"));
	CHECK(has(doc.signals, "hit(amount: int)"));
	CHECK(has(doc.constants, "LIMIT: int = 3"));
	CHECK(described(doc.enums) == std::vector<std::string>{ "State" });
	CHECK(has(doc.constants, "RUN: int [Doc.State] = 4"));
	CHECK(has(doc.properties, "speed: float = 2.5"));
	CHECK(has(doc.properties, "target: Node3D?"));
	CHECK(has(doc.properties, "states: Doc.State[] = []"));
	CHECK(has(doc.properties, "count: int = 0"));
	CHECK(has(doc.properties, "hp: int"));
	// Its inline getter is not a method.
	CHECK(described(doc.methods) == std::vector<std::string>{ "make(x: int = 1): Doc static", "wait: int async" });
}

TEST_CASE("an interface's documentation inherits its parent, as the engine shows one parent") {
	const std::vector<compiler::ClassDoc> docs = documentation("class_name Doc\nextends Node\n\npub interface Named:\n\tsignal named\n\npub interface Timed:\n\tsignal timeout\n\npub interface Mortal extends Named:\n\tsignal died\n\npub interface Both extends Named, Timed:\n\tpass\n", "Doc");
	REQUIRE(docs.size() == 5);
	CHECK((docs[2].name == "Doc.Mortal" && docs[2].inherits == "Doc.Named"));
	CHECK((docs[3].name == "Doc.Both" && docs[3].inherits.empty()));
}

TEST_CASE("a ## comment documents the declaration it is on or directly above, read as GDScript reads it, and one of ### is ordinary") {
	const std::string source = R"(## A unit.
##
## Moves and [b]fights[/b].
## @tutorial(Moving): https://example.com/move
## @experimental
extends Node

### Signals ###
signal hit
signal died  ## When it dies.

## Its speed,
## in meters.[br][br]Per second.
## @deprecated: Use [member pace].
@export pub var speed := 2.5

## Not of pace.
### Section
var pace := 1.0

## Usage:
## [codeblock]
## if x:
##     go()
## [/codeblock]
func go() -> void:
	pass

## States.
enum State {
	IDLE,  ## Waiting.
	## Moving.
	RUN,
	A, B,  ## Of B only.
}

## A hit.
struct Shot:
	var damage := 1  ## Before armor.

## Inner.
class Inner:
	pass
)";
	const std::vector<compiler::ClassDoc> docs = documentation(source);
	REQUIRE(docs.size() == 3);
	CHECK(docs[0].comment.brief == "Inner.");
	CHECK(docs[1].comment.brief == "A hit.");
	CHECK(comment_of(docs[1].properties, "damage").description == "Before armor.");
	const compiler::ClassDoc &doc = docs[2];
	CHECK(doc.comment.brief == "A unit.");
	CHECK(doc.comment.description == "Moves and [b]fights[/b].");
	CHECK(doc.comment.tutorials == std::vector<std::pair<std::string, std::string>>{ { "Moving", "https://example.com/move" } });
	CHECK(doc.comment.experimental == std::string());
	CHECK(comment_of(doc.signals, "hit").description.empty());
	CHECK(comment_of(doc.signals, "died").description == "When it dies.");
	const compiler::DocComment &speed = comment_of(doc.properties, "speed");
	CHECK(speed.description == "Its speed, in meters.\nPer second.");
	CHECK(speed.deprecated == std::string("Use [member pace]."));
	CHECK(comment_of(doc.properties, "pace").description.empty());
	CHECK(comment_of(doc.methods, "go").description == "Usage:\n[codeblock]\nif x:\n    go()\n[/codeblock]");
	CHECK(comment_of(doc.enums, "State").description == "States.");
	CHECK(comment_of(doc.constants, "IDLE").description == "Waiting.");
	CHECK(comment_of(doc.constants, "RUN").description == "Moving.");
	CHECK(comment_of(doc.constants, "A").description.empty());
	CHECK(comment_of(doc.constants, "B").description == "Of B only.");
	// The first comment documents the class only when it is not directly
	// above the first member.
	const std::vector<compiler::ClassDoc> member = documentation("extends Node\n## Of x.\nvar x := 1\n");
	CHECK(member[0].comment.brief.empty());
	CHECK(comment_of(member[0].properties, "x").description == "Of x.");
}

TEST_CASE("a declaration of the engine is filed as its documentation files it") {
	CHECK(lookup("Timer") == "class Timer");
	CHECK(lookup("timeout") == "signal Timer.timeout");
	CHECK(lookup("position") == "property Node3D.position");
	CHECK(lookup("print") == "method @GlobalScope.print");
	CHECK(lookup("EaseType") == "enum Tween.EaseType");
	CHECK(lookup("EASE_IN") == "constant Tween.EASE_IN");
	CHECK(lookup("ZERO") == "constant Vector3.ZERO");
	CHECK(lookup("Axis") == "enum Vector3.Axis");
	// The global enums are under @GlobalScope (doc/classes/@GlobalScope.xml).
	CHECK(lookup("Key") == "enum @GlobalScope.Key");
	CHECK(lookup("KEY_ESCAPE") == "constant @GlobalScope.KEY_ESCAPE");
}
