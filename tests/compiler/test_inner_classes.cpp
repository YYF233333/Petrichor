#include "helpers.h"

using namespace petrichor;
using namespace petrichor::tests;
using bytecode::Op;

// Inner classes (spec §2.1, §4.1): classes declared in a file's class, with
// the rules of a class, which see the declarations of the classes around them
// that need no instance.

namespace {

void require_verified(const compiler::CompileResult &p_result) {
	REQUIRE_MESSAGE(p_result.ok(), messages(p_result));
	std::string error;
	CHECK_MESSAGE(bytecode::verify(p_result.module, error), error);
}

bool uses(const bytecode::Function &p_function, Op p_op) {
	for (size_t pc = 0; pc < p_function.code.size(); pc += bytecode::instruction_length(p_function.code.data() + pc)) {
		if (Op(p_function.code[pc]) == p_op) {
			return true;
		}
	}
	return false;
}

const bytecode::Module &inner(const bytecode::Module &p_module, const std::string &p_name) {
	const auto found = std::find_if(p_module.inner_classes.begin(), p_module.inner_classes.end(), [&](const bytecode::Module &p_inner) { return p_inner.name == p_name; });
	REQUIRE_MESSAGE(found != p_module.inner_classes.end(), p_name);
	return *found;
}

// open-rts's Constants, an autoload whose inner classes hold constants, one of
// them inheriting those of another script (Constants.gd, MatchConstants.gd).
FakeProject constants_project() {
	FakeProject project;
	project.add_script("res://match_constants.pet", R"(extends RefCounted
pub class Units:
	pub const PROJECTILES := 3
pub enum Team { BLUE, RED }
)");
	project.add_script("res://constants.pet", R"(extends Node
pub class Match:
	extends "res://match_constants.pet"
	pub const MAX_PLAYERS := 8
	pub class Player:
		pub var team := Team.RED
		pub static func limit() -> int:
			return MAX_PLAYERS
class Hidden:
	pass
)");
	project.autoloads["Constants"] = "res://constants.pet";
	project.add_script("res://combat.pet", R"(class_name Combat extends RefCounted
pub class Shot:
	pub const SPEED := 2.5
	pub var damage := 1
	pub static func make() -> Shot:
		return Shot.new()
	pub class Trail extends Node3D:
		pub const LENGTH := 4
class Secret:
	pass
)",
			"Combat");
	project.add_script("res://base.pet", "class_name Base extends RefCounted\npub const MAX := 1\n", "Base");
	return project;
}

std::vector<std::string> errors_in(const std::string &p_source) {
	std::vector<std::string> result;
	for (const compiler::Diagnostic &d : compile_in(p_source, constants_project()).errors) {
		result.push_back(std::to_string(d.line) + ":" + std::to_string(d.column) + ": " + d.message);
	}
	return result;
}

bool has(const std::vector<std::string> &p_errors, const std::string &p_text) {
	return std::any_of(p_errors.begin(), p_errors.end(), [&](const std::string &p_error) { return contains(p_error, p_text); });
}

} // namespace

TEST_CASE("an inner class extends RefCounted unless it says otherwise, on its line or the first of its body") {
	const auto result = compile_in(R"(extends Node
class Plain:
	pass
class Shot extends Node3D:
	pass
class Flash:
	extends Node3D
	var brightness := 1.0
)",
			FakeProject());
	require_verified(result);
	CHECK(inner(result.module, "Plain").base_class == "RefCounted");
	CHECK(inner(result.module, "Shot").base_class == "Node3D");
	CHECK(inner(result.module, "Flash").base_class == "Node3D");
	CHECK(inner(result.module, "Flash").members.size() == 1);
	CHECK_FALSE(inner(result.module, "Plain").is_pub);
	const auto implementing = compile_in(R"(extends Node
implements Hurtable
pub interface Hurtable:
	func hurt()
pub func hurt():
	pass
class Target:
	extends Node3D
	implements Hurtable
	pub func hurt():
		pass
)",
			FakeProject());
	require_verified(implementing);
	CHECK(inner(implementing.module, "Target").interfaces == std::vector<std::string>{ "res://test.pet::Hurtable" });
}

TEST_CASE("an inner class uses what the classes around it declare that needs no instance (spec §4.1)") {
	const auto result = compile_in(R"(extends Node
const LIMIT := 3
enum Mode { SLOW, FAST }
enum { NONE, SOME }
struct Hit:
	var damage: int
static func double(x: int) -> int:
	return x * 2
class Shot:
	var mode := Mode.FAST
	var hit := Hit(damage = LIMIT)
	func power() -> int:
		var add := func(x: int) -> int: return x + SOME
		return double(LIMIT) + add.call(NONE)
	func trail() -> Trail:
		return Trail.new()
class Trail:
	var length := LIMIT
)",
			FakeProject());
	require_verified(result);
	const bytecode::Module &shot = inner(result.module, "Shot");
	const bytecode::Function &power = function_named(shot, "power");
	CHECK(uses(power, Op::CALL_METHOD)); // double(), static, on the class's script.
}

TEST_CASE("an inner class has no object of the class around it (spec §4.1)") {
	const std::string around = "extends Node\nvar hp := 3\nsignal hurt\nfunc heal() -> void:\n\tpass\nclass Shot:\n\tfunc f() -> void:\n";
	CHECK(has(errors_in(around + "\t\tprint(hp)\n"), "\"hp\" belongs to an object of test.pet, which test.pet.Shot does not have: an inner class has no object of the class around it."));
	CHECK(has(errors_in(around + "\t\theal()\n"), "\"heal()\" belongs to an object of test.pet"));
	CHECK(has(errors_in(around + "\t\thurt.emit()\n"), "\"hurt\" belongs to an object of test.pet"));
	CHECK(has(errors_in(around + "\t\tprint(name)\n"), "\"name\" belongs to an object of test.pet"));
}

TEST_CASE("what an inner class declares or inherits comes before what is around it") {
	// The String around them would not be returned as an int.
	require_verified(compile_in(R"(extends Node
const MAX := "ten"
class Own:
	const MAX := 2
	func f() -> int:
		return MAX
class Inherited extends Base:
	func f() -> int:
		return MAX
)",
			constants_project()));
}

TEST_CASE("an inner class is named through its class from other files, when pub") {
	require_verified(compile_in(R"(extends Node
func f() -> void:
	var shot: Combat.Shot = Combat.Shot.make()
	var other := Combat.Shot.new()
	var trail: Combat.Shot.Trail = Combat.Shot.Trail.new()
	print(shot.damage + other.damage, Combat.Shot.SPEED, Combat.Shot.Trail.LENGTH, trail is Combat.Shot.Trail)
)",
			constants_project()));
	CHECK(has(errors_in("extends Node\nfunc f() -> void:\n\tvar s := Combat.Secret.new()\n"), "Combat.Secret is not pub."));
	CHECK(has(errors_in("extends Node\nvar s: Combat.Secret\n"), "Combat.Secret is not pub."));
	CHECK(has(errors_in("extends Node\nvar s: Combat.Nope.X\n"), "Combat declares no class \"Nope\"."));
}

TEST_CASE("an inner class is reached through an object of its class, as constants are") {
	const auto result = compile_in(R"(extends Node
func f() -> int:
	var player := Constants.Match.Player.new()
	print(Constants.Match.Units.PROJECTILES, Constants.Match.Team.RED, Constants.Match.Team.keys(), player.team)
	return Constants.Match.MAX_PLAYERS + Constants.Match.Player.limit()
)",
			constants_project());
	require_verified(result);
	// Nothing is read from the autoload: it only leads to the class.
	CHECK_FALSE(uses(function_named(result.module, "f"), Op::GET_AUTOLOAD));
	CHECK(has(errors_in("extends Node\nfunc f() -> void:\n\tprint(Constants.Hidden)\n"), "constants.pet.Hidden is not pub."));
}

TEST_CASE("an inner class is a script as a value") {
	const auto result = compile_in("extends Node\nclass Shot:\n\tpass\nfunc f(node: Node) -> void:\n\tnode.set_script(Shot)\n", FakeProject());
	require_verified(result);
	const bytecode::Function &f = function_named(result.module, "f");
	const size_t get = find_instruction(f, Op::GET_SCRIPT);
	REQUIRE(get < f.code.size());
	CHECK(result.module.classes[f.code[get + 1]].script == "res://test.pet::Shot");
}

TEST_CASE("a class_name is a script as a value, as an inner class is (spec §2.1)") {
	FakeProject project;
	project.add_script("res://weapon.pet", "class_name Weapon extends RefCounted\n", "Weapon");
	const auto result = compile_in("class_name Holder extends Node\nfunc f() -> Array[Script]:\n\treturn [Weapon, Holder]\n", project);
	require_verified(result);
	const bytecode::Function &f = function_named(result.module, "f");
	const size_t get = find_instruction(f, Op::GET_SCRIPT);
	REQUIRE(get < f.code.size());
	CHECK(result.module.classes[f.code[get + 1]].script == "res://weapon.pet");
}

TEST_CASE("an inner class extends one of its file, one of another class, or the class around it") {
	require_verified(compile_in(R"(class_name Weapon extends RefCounted
class Shot:
	pub var damage := 1
class Bolt extends Shot:
	func f() -> int:
		return damage
class Tracer extends Combat.Shot:
	pass
class Copy extends Weapon:
	pass
)",
			constants_project()));
	CHECK(has(errors_in("extends Node\nclass A extends B:\n\tpass\nclass B extends A:\n\tpass\n"), "Inheritance cycle"));
}

TEST_CASE("an inner class's name is declared once, and class_name and the file's annotations stay with the file") {
	CHECK(has(errors_in("extends Node\nvar Shot := 1\nclass Shot:\n\tpass\n"), "\"Shot\" is already declared."));
	CHECK(has(errors_in("extends Node\nclass Node3D:\n\tpass\n"), "\"Node3D\" is already declared."));
	// Every type the class declares, as one rule.
	CHECK(has(errors_in("extends Node\nvar Item := 1\nstruct Item:\n\tvar x := 0\n"), "\"Item\" is already declared."));
	CHECK(has(errors_in("extends Node\nstatic var State := 1\nenum State { A, B }\n"), "\"State\" is already declared."));
	CHECK(has(errors_in("extends Node\nstatic var A := 1\nenum { A, B }\n"), "\"A\" is already declared."));
	CHECK(has(errors_in("extends Node\ninterface Node:\n\tfunc f() -> void\n"), "\"Node\" is already declared."));
	CHECK(has(errors_in("extends Node\ninterface Hit:\n\tfunc f() -> void\nclass Hit:\n\tpass\n"), "\"Hit\" is already declared."));
	CHECK(has(errors_in("extends Node\nclass Shot:\n\tclass_name Shot\n"), "class_name belongs to the file's class."));
	CHECK(has(errors_in("extends Node\nclass Shot:\n\t@icon(\"res://a.svg\")\n\tvar x := 1\n"), "@icon belongs to the file's class."));
	CHECK(has(errors_in("extends Node\nclass Shot:\n\tvar x := 1\n\textends Node\n"), "\"extends\" belongs to the class header, before the members."));
	require_verified(compile_in("extends Node\n@abstract class Shape:\n\t@abstract func area() -> float\nclass Square extends Shape:\n\tfunc area() -> float:\n\t\treturn 1.0\n", FakeProject()));
	CHECK(has(errors_in("extends Node\n@abstract class Shape:\n\tpass\nfunc f() -> void:\n\tvar s := Shape.new()\n"), "is abstract and cannot be instantiated"));
}

TEST_CASE("an inner class's module is its own, in its file's") {
	const auto result = compile_in(R"(extends Node
pub class Shot:
	pub var damage := 1
	class Trail:
		pass
)",
			FakeProject());
	require_verified(result);
	const bytecode::Module &shot = inner(result.module, "Shot");
	CHECK(shot.is_pub);
	CHECK(inner(shot, "Trail").name == "Trail");
	CHECK(shot.global_name.empty());
	bytecode::Module read;
	std::string error;
	const std::vector<uint8_t> data = bytecode::serialize(result.module);
	REQUIRE_MESSAGE(bytecode::deserialize(data.data(), data.size(), read, error), error);
	CHECK(inner(inner(read, "Shot"), "Trail").name == "Trail");
}
