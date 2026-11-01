#include "doctest.h"
#include "helpers.h"

using namespace petrichor;
using namespace petrichor::tests;
using compiler::DeclId;
using compiler::Reference;

// The reference table (architecture §2.1). The helpers check on every source
// that compiles that each reference spells its name and that every resolved
// name has one; these are the cases it does not reach.

namespace {

using Kind = DeclId::Kind;

// What the references at the nth `p_text` of the source refer to, with what
// a declaration there overrides.
std::vector<std::pair<DeclId, std::optional<DeclId>>> at(const compiler::CompileResult &p_result, const std::string &p_source, const std::string &p_text, int p_nth = 0) {
	size_t offset = p_source.find(p_text);
	for (int i = 0; i < p_nth; i++) {
		offset = p_source.find(p_text, offset + 1);
	}
	REQUIRE(offset != std::string::npos);
	std::vector<std::pair<DeclId, std::optional<DeclId>>> found;
	for (const Reference &reference : p_result.references) {
		if (reference.scene.empty() && reference.begin == offset && reference.end == offset + p_text.size()) {
			found.push_back({ reference.target, reference.overrides });
		}
	}
	return found;
}

bool has(const std::vector<std::pair<DeclId, std::optional<DeclId>>> &p_found, const DeclId &p_target, const std::optional<DeclId> &p_overrides = std::nullopt) {
	for (const auto &[target, overrides] : p_found) {
		if (target == p_target && overrides == p_overrides) {
			return true;
		}
	}
	return false;
}

FakeProject classes() {
	FakeProject project;
	project.add_script("res://base.pet", "class_name Base\nextends Node\n\npub func hit() -> int:\n\treturn 1\n", "Base");
	project.add_script("res://derived.pet", "class_name Derived\nextends Base\n\npub func hit() -> int:\n\treturn 2\n", "Derived");
	project.add_script("res://combat.pet", "class_name Combat\n\npub interface Damageable:\n\tfunc damage(amount: int)\n", "Combat");
	return project;
}

} // namespace

TEST_CASE("a call refers to the method of the class it is made on") {
	const std::string source = "extends Node\n\nfunc f(d: Derived, b: Base) -> int:\n\treturn d.hit() + b.hit()\n";
	const auto result = compile_in(source, classes());
	REQUIRE_MESSAGE(result.ok(), messages(result));
	CHECK(has(at(result, source, "hit", 0), { Kind::Script, "res://derived.pet", "hit" }));
	CHECK(has(at(result, source, "hit", 1), { Kind::Script, "res://base.pet", "hit" }));
	CHECK(has(at(result, source, "Derived"), { Kind::Script, "res://derived.pet", "" }));
}

TEST_CASE("an override records what it overrides, and an implementation what it implements") {
	const std::string overriding = "extends Base\n\npub func hit() -> int:\n\treturn 2\n\nfunc _ready():\n\tpass\n";
	const auto result = compile_in(overriding, classes());
	REQUIRE_MESSAGE(result.ok(), messages(result));
	CHECK(has(at(result, overriding, "hit"), { Kind::Script, "res://test.pet", "hit" }, DeclId{ Kind::Script, "res://base.pet", "hit" }));
	CHECK(has(at(result, overriding, "_ready"), { Kind::Script, "res://test.pet", "_ready" }, DeclId{ Kind::Engine, "Node", "_ready" }));

	const std::string implementing = "extends Node implements Combat.Damageable\n\npub func damage(amount: int):\n\tpass\n";
	const auto implemented = compile_in(implementing, classes());
	REQUIRE_MESSAGE(implemented.ok(), messages(implemented));
	CHECK(has(at(implemented, implementing, "damage"), { Kind::Script, "res://test.pet", "damage" }, DeclId{ Kind::Script, "res://combat.pet", "Damageable.damage" }));
	CHECK(has(at(implemented, implementing, "Combat"), { Kind::Script, "res://combat.pet", "" }));
	CHECK(has(at(implemented, implementing, "Damageable"), { Kind::Script, "res://combat.pet", "Damageable" }));
}

TEST_CASE("a node path refers to the node of each scene the script is attached in") {
	FakeProject project;
	for (const char *path : { "res://a.tscn", "res://b.tscn" }) {
		compiler::Scene scene;
		scene.path = path;
		scene.nodes = { { ".", "Node3D", "res://test.pet" }, { "Head", "Node3D", "" }, { "Head/Camera", "Timer", "" } };
		project.scenes.push_back(scene);
	}
	const std::string source = "extends Node3D\n\nfunc f():\n\tvar a = $Head/Camera\n\tvar b = $\"Head/Camera\"\n";
	const auto result = compile_in(source, project);
	REQUIRE_MESSAGE(result.ok(), messages(result));
	for (int nth : { 0, 1 }) {
		const auto head = at(result, source, "Head", nth);
		CHECK(head.size() == 2);
		CHECK(has(head, { Kind::Node, "res://a.tscn", "Head" }));
		CHECK(has(head, { Kind::Node, "res://b.tscn", "Head" }));
		CHECK(has(at(result, source, "Camera", nth), { Kind::Node, "res://b.tscn", "Head/Camera" }));
	}
}

TEST_CASE("a method a scene connects is referred to from the scene, and one named by a literal inside the quotes") {
	FakeProject project;
	compiler::Scene scene;
	scene.path = "res://enemy.tscn";
	scene.nodes = { { ".", "Node3D", "res://test.pet" }, { "Timer", "Timer", "" } };
	scene.connections = { { "timeout", "Timer", ".", "_on_timeout", 0, 0 } };
	project.scenes = { scene };
	const std::string source = "extends Node3D\n\nfunc _on_timeout():\n\tcall_deferred(\"reset\")\n\nfunc reset():\n\tpass\n";
	const auto result = compile_in(source, project);
	REQUIRE_MESSAGE(result.ok(), messages(result));
	bool connected = false;
	for (const Reference &reference : result.references) {
		connected = connected || (reference.scene == "res://enemy.tscn" && reference.connection.source == "Timer" && reference.target == DeclId{ Kind::Script, "res://test.pet", "_on_timeout" });
	}
	CHECK(connected);
	CHECK(has(at(result, source, "reset"), { Kind::Script, "res://test.pet", "reset" }));
}

TEST_CASE("a local is the same where it is declared and where it is used") {
	const std::string source = "extends Node\n\nfunc f(count: int) -> int:\n\tvar total := count\n\treturn total\n";
	const auto result = compile_in(source, FakeProject());
	REQUIRE_MESSAGE(result.ok(), messages(result));
	const auto declared = at(result, source, "total", 0);
	REQUIRE(declared.size() == 1);
	CHECK(declared[0].first.kind == Kind::Local);
	CHECK(has(at(result, source, "total", 1), declared[0].first));
	CHECK(has(at(result, source, "count", 1), at(result, source, "count", 0)[0].first));
}
