#include "compiler/navigation.h"
#include "doctest.h"
#include "helpers.h"

using namespace petrichor;
using namespace petrichor::tests;

// The script editor's lookup, which going to definitions and hover use
// (spec §13.2).

namespace {

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
)";

// The lookup at the second byte of the nth `p_text` of SOURCE, as
// "kind class.member type path:line".
std::string lookup(const std::string &p_text, int p_nth = 0) {
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
	const std::optional<compiler::Lookup> found = compiler::lookup(analysis, fake_engine(), classes, "res://test.pet", SOURCE, uint32_t(offset + 1));
	if (!found) {
		return "none";
	}
	static const char *KINDS[] = { "location", "class", "constant", "property", "method", "signal", "enum", "local", "local constant" };
	std::string text = KINDS[int(found->kind)];
	if (!found->class_name.empty()) {
		text += " " + found->class_name;
	}
	if (!found->member.empty()) {
		text += (found->class_name.empty() ? " " : ".") + found->member;
	}
	if (!found->type.empty()) {
		text += " " + found->type;
	}
	if (!found->path.empty()) {
		text += " " + found->path + ":" + std::to_string(found->line);
	}
	return text;
}

} // namespace

TEST_CASE("a local or parameter shows its type and leads to its declaration") {
	CHECK(lookup("total", 1) == "local int res://test.pet:7");
	CHECK(lookup("MAX", 1) == "local constant int res://test.pet:6");
	CHECK(lookup("hit", 1) == "local Hit res://test.pet:5");
}

TEST_CASE("a declaration of this language leads to its file and line, also in an inner class") {
	CHECK(lookup("LIMIT", 1) == "location res://test.pet:3");
	CHECK(lookup("damage") == "location res://hit.pet:4");
	CHECK(lookup("Crit") == "location res://hit.pet:6");
	CHECK(lookup("FACTOR") == "location res://hit.pet:7");
}

TEST_CASE("a member of a GDScript class leads to the script of its base that declares it") {
	CHECK(lookup("speed") == "property speed res://legacy_base.gd:3");
	CHECK(lookup("Legacy") == "class res://legacy.gd:1");
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
