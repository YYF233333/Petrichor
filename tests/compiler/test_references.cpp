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
		if (reference.begin == offset && reference.end == offset + p_text.size()) {
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

// Spec §2.6: as in GDScript, a method of the class or of its base needs a
// receiver.
TEST_CASE("a direct call of a utility function's name calls the utility function") {
	const std::string source = "extends RandomNumberGenerator\n\nfunc lerp(a: float, b: float, t: float) -> float:\n\treturn a\n\nfunc range(n: int) -> int:\n\treturn n\n\n"
							   "func f() -> void:\n\tprint(lerp(1.0, 2.0, 0.5), self.lerp(1.0, 2.0, 0.5), range(3), self.range(3), randi(), self.randi())\n";
	const auto result = compile_in(source, FakeProject());
	REQUIRE_MESSAGE(result.ok(), messages(result));
	CHECK(has(at(result, source, "lerp", 1), { Kind::Engine, "@GlobalScope", "lerp" }));
	CHECK(has(at(result, source, "lerp", 2), { Kind::Script, "res://test.pet", "lerp" }));
	CHECK(has(at(result, source, "range", 1), { Kind::Engine, "@GDScript", "range" }));
	CHECK(has(at(result, source, "range", 2), { Kind::Script, "res://test.pet", "range" }));
	CHECK(has(at(result, source, "randi", 0), { Kind::Engine, "@GlobalScope", "randi" }));
	CHECK(has(at(result, source, "randi", 1), { Kind::Engine, "RandomNumberGenerator", "randi" }));
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
		scene.nodes = { { ".", "Node3D", "res://test.pet" }, { "Head", "Node3D", "" }, { "Head/Camera", "Timer", "", false, "", true } };
		project.scenes.push_back(scene);
	}
	const std::string source = "extends Node3D\n\nfunc f():\n\tvar a = $Head/Camera\n\tvar b = $\"Head/Camera\"\n\tvar c = %Camera\n\tvar d = $\"%Camera\"\n";
	const auto result = compile_in(source, project);
	REQUIRE_MESSAGE(result.ok(), messages(result));
	for (int nth : { 0, 1 }) {
		const auto head = at(result, source, "Head", nth);
		CHECK(head.size() == 2);
		CHECK(has(head, { Kind::Node, "res://a.tscn", "Head" }));
		CHECK(has(head, { Kind::Node, "res://b.tscn", "Head" }));
	}
	for (int nth : { 0, 1, 2, 3 }) {
		CHECK(has(at(result, source, "Camera", nth), { Kind::Node, "res://b.tscn", "Head/Camera" }));
	}
}

TEST_CASE("a method a scene connects is referred to from the scene, and one named by a literal inside the quotes") {
	FakeProject project;
	compiler::Scene scene;
	scene.path = "res://enemy.tscn";
	scene.nodes = { { ".", "Node3D", "res://test.pet" }, { "Timer", "Timer", "" } };
	scene.connections = { { "timeout", "Timer", ".", "_on_timeout" } };
	project.scenes = { scene };
	const std::string source = "extends Node3D\n\npub func _on_timeout():\n\tcall_deferred(\"reset\")\n\tcall_deferred(&'''reset''')\n\npub func reset():\n\tpass\n";
	const auto result = compile_in(source, project);
	REQUIRE_MESSAGE(result.ok(), messages(result));
	// The scene's check has it (architecture §2.1).
	const auto checked = check_scenes_in(source, project);
	REQUIRE(checked.references.size() == 1);
	CHECK((checked.references[0].scene == "res://enemy.tscn" && checked.references[0].node == "." && checked.references[0].connection.source == "Timer" && checked.references[0].target == DeclId{ Kind::Script, "res://test.pet", "_on_timeout" }));
	CHECK(has(at(result, source, "reset"), { Kind::Script, "res://test.pet", "reset" }));
	CHECK(has(at(result, source, "reset", 1), { Kind::Script, "res://test.pet", "reset" }));
}

TEST_CASE("a member of a foreign class refers to the script that declares it, also in a literal") {
	FakeProject project;
	compiler::ForeignClass base;
	base.native = "Node";
	base.properties = { { "speed", compiler::Type::of(compiler::TypeKind::Float), true, "res://legacy_base.gd" } };
	compiler::EngineMethod step{ "step", "res://legacy_base.gd" };
	compiler::EngineMethod hit{ "hit", "res://legacy_base.gd" };
	compiler::EngineMethod count{ "count", "res://legacy_base.gd", {}, 0, compiler::Type::of(compiler::TypeKind::Int), false, true };
	compiler::EngineMethod init{ "_init", "res://legacy_base.gd" };
	compiler::EngineMethod scored{ "scored", "res://legacy_base.gd", { { "points", compiler::Type::of(compiler::TypeKind::Int) } } };
	base.methods = { step, hit, count, init };
	base.signals = { scored };
	base.constants = { { "MAX_SPEED", compiler::Type::of(compiler::TypeKind::Float), false, "res://legacy_base.gd" } };
	project.foreign["res://legacy_base.gd"] = base;

	compiler::ForeignClass derived = base;
	derived.name = "Legacy";
	derived.base = "res://legacy_base.gd";
	compiler::EngineMethod overridden = hit;
	overridden.owner = "res://legacy.gd";
	derived.methods.insert(derived.methods.begin(), overridden);
	project.foreign["res://legacy.gd"] = derived;
	project.global_classes["Legacy"] = "res://legacy.gd";

	const std::string source =
			"extends Node\n\n"
			"func on_scored(points: int):\n"
			"\tpass\n\n"
			"func f(l: Legacy) -> float:\n"
			"\tvar copy := Legacy.new()\n"
			"\tl.step()\n"
			"\tvar fn := l.step\n"
			"\tl.hit()\n"
			"\tvar s := l.scored\n"
			"\tl.set_deferred(\"speed\", 1.0)\n"
			"\tl.call_deferred(\"step\")\n"
			"\tl.connect(\"scored\", on_scored)\n"
			"\tprint(copy, fn, s, Legacy.count())\n"
			"\treturn l.speed + Legacy.MAX_SPEED\n";
	const auto result = compile_in(source, project);
	REQUIRE_MESSAGE(result.ok(), messages(result));
	CHECK(has(at(result, source, "new"), { Kind::Foreign, "res://legacy.gd", "new" }));
	CHECK(has(at(result, source, "step", 0), { Kind::Foreign, "res://legacy_base.gd", "step" }));
	CHECK(has(at(result, source, "step", 1), { Kind::Foreign, "res://legacy_base.gd", "step" }));
	CHECK(has(at(result, source, "step", 2), { Kind::Foreign, "res://legacy_base.gd", "step" }));
	CHECK(has(at(result, source, "hit"), { Kind::Foreign, "res://legacy.gd", "hit" }));
	CHECK(has(at(result, source, "scored", 1), { Kind::Foreign, "res://legacy_base.gd", "scored" }));
	CHECK(has(at(result, source, "scored", 2), { Kind::Foreign, "res://legacy_base.gd", "scored" }));
	CHECK(has(at(result, source, "speed", 0), { Kind::Foreign, "res://legacy_base.gd", "speed" }));
	CHECK(has(at(result, source, "speed", 1), { Kind::Foreign, "res://legacy_base.gd", "speed" }));
	CHECK(has(at(result, source, "count"), { Kind::Foreign, "res://legacy_base.gd", "count" }));
	CHECK(has(at(result, source, "MAX_SPEED"), { Kind::Foreign, "res://legacy_base.gd", "MAX_SPEED" }));
}

TEST_CASE("scenes and resources refer to the exported keys, tracks and synchronized properties they store (spec §7.5, §7.7)") {
	const auto text = [](const std::string &p_text) {
		compiler::SceneValue value{ "String" };
		value.text = p_text;
		return value;
	};
	compiler::SceneValue count{ "int" };
	count.integer = 3;
	compiler::SceneValue hit{ "Dictionary" };
	hit.items = { text("damage"), count };
	compiler::SceneValue order{ "Dictionary" };
	order.items = { text("$variant"), text("MOVE"), text("target"), compiler::SceneValue{ "Vector3" } };

	FakeProject project;
	project.add_script(
			"res://base_enemy.pet",
			"class_name BaseEnemy\nextends Node2D\n\n"
			"pub struct Hit:\n\tvar damage: int\n\n"
			"pub enum Order { WAIT, MOVE(target: Vector3) }\n\n"
			"@export pub var speed := 1.0\n"
			"@export pub var hit: Hit?\n"
			"@export pub var order := Order.WAIT\n"
			"pub var health := 3\n\n"
			"pub func flash():\n\tpass\n",
			"BaseEnemy");
	project.add_script(
			"res://loadout.pet",
			"class_name Loadout\nextends Resource\n\n"
			"@export pub var ammo := 5\n",
			"Loadout");

	compiler::Scene enemy;
	enemy.path = "res://enemy.tscn";
	enemy.nodes = {
		{ ".", "Node2D", "res://test.pet", false, "", false, { { "speed", count }, { "hit", hit }, { "order", order } } },
		{ "AnimationPlayer", "AnimationPlayer" },
		{ "MultiplayerSynchronizer", "MultiplayerSynchronizer" },
	};
	enemy.named_members = {
		{ "AnimationPlayer", "hurt", "..", "speed", false },
		{ "AnimationPlayer", "hurt", "..", "flash", true },
		{ "MultiplayerSynchronizer", "", "..", "health", false },
	};
	enemy.resources = { { "res://enemy.tscn::Resource_1", "res://loadout.pet", { { "ammo", count } } } };

	compiler::Scene level;
	level.path = "res://level.tscn";
	level.nodes = {
		{ ".", "Node2D" },
		{ "Enemy", "Node2D", "", true, "res://test.pet", false, { { "extra", count } } },
		{ "Squad", "Node2D", "", true },
		{ "Squad/Member", "", "", false, "", false, { { "speed", count } } },
	};
	level.nodes[1].instance = enemy.path;
	level.nodes[2].instance = "res://squad.tscn";

	compiler::Scene squad;
	squad.path = "res://squad.tscn";
	squad.nodes = { { ".", "Node2D" }, { "Member", "Node2D", "res://test.pet" } };

	compiler::Scene pack;
	pack.path = "res://pack.tres";
	pack.resources = { { pack.path, "res://loadout.pet", { { "ammo", count } } } };

	project.scenes = { enemy, squad, level, pack };
	const std::string source = "extends BaseEnemy\n\n@export pub var extra := 0\n";
	const auto checked = check_scenes_in(source, project);
	REQUIRE_MESSAGE(checked.errors.empty(), messages(checked));

	const auto count_matching = [&](const std::function<bool(const Reference &)> &p_pred) {
		return std::count_if(checked.references.begin(), checked.references.end(), p_pred);
	};
	// Each inherited member is attributed to BaseEnemy once, not duplicated for the subclass.
	CHECK(count_matching([](const Reference &r) {
		return r.scene == "res://enemy.tscn" && r.node == "." && r.property == "speed" && r.target == DeclId{ Kind::Script, "res://base_enemy.pet", "speed" };
	}) == 1);
	CHECK(count_matching([](const Reference &r) {
		return r.scene == "res://enemy.tscn" && r.node == "." && r.property == "hit" && r.target == DeclId{ Kind::Script, "res://base_enemy.pet", "hit" };
	}) == 1);
	CHECK(count_matching([](const Reference &r) {
		return r.scene == "res://enemy.tscn" && r.node == "." && r.property == "hit" && r.target == DeclId{ Kind::Script, "res://base_enemy.pet", "Hit.damage" };
	}) == 1);
	CHECK(count_matching([](const Reference &r) {
		return r.scene == "res://enemy.tscn" && r.node == "." && r.property == "order" && r.target == DeclId{ Kind::Script, "res://base_enemy.pet", "Order.MOVE" };
	}) == 1);
	CHECK(count_matching([](const Reference &r) {
		return r.scene == "res://enemy.tscn" && r.node == "." && r.property == "order" && r.target == DeclId{ Kind::Script, "res://base_enemy.pet", "Order.MOVE.target" };
	}) == 1);
	CHECK(count_matching([](const Reference &r) {
		return r.scene == "res://enemy.tscn" && r.node == "." && r.named.from == "AnimationPlayer" && r.named.animation == "hurt" && !r.named.method &&
				r.target == DeclId{ Kind::Script, "res://base_enemy.pet", "speed" };
	}) == 1);
	CHECK(count_matching([](const Reference &r) {
		return r.scene == "res://enemy.tscn" && r.node == "." && r.named.from == "AnimationPlayer" && r.named.animation == "hurt" && r.named.method &&
				r.target == DeclId{ Kind::Script, "res://base_enemy.pet", "flash" };
	}) == 1);
	CHECK(count_matching([](const Reference &r) {
		return r.scene == "res://enemy.tscn" && r.node == "." && r.named.from == "MultiplayerSynchronizer" && r.named.animation.empty() &&
				r.target == DeclId{ Kind::Script, "res://base_enemy.pet", "health" };
	}) == 1);
	CHECK(count_matching([](const Reference &r) {
		return r.scene == "res://enemy.tscn" && r.node.empty() && r.resource == "res://enemy.tscn::Resource_1" && r.property == "ammo" &&
				r.target == DeclId{ Kind::Script, "res://loadout.pet", "ammo" };
	}) == 1);
	CHECK(count_matching([](const Reference &r) {
		return r.scene == "res://level.tscn" && r.node == "Enemy" && r.property == "extra" && r.target == DeclId{ Kind::Script, "res://test.pet", "extra" };
	}) == 1);
	CHECK(count_matching([](const Reference &r) {
		return r.scene == "res://level.tscn" && r.node == "Squad/Member" && r.property == "speed" && r.target == DeclId{ Kind::Script, "res://base_enemy.pet", "speed" };
	}) == 1);
	CHECK(count_matching([](const Reference &r) {
		return r.scene == "res://pack.tres" && r.node.empty() && r.resource == "res://pack.tres" && r.property == "ammo" &&
				r.target == DeclId{ Kind::Script, "res://loadout.pet", "ammo" };
	}) == 1);
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
