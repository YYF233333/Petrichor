#include "compiler/constant.h"
#include "doctest.h"
#include "helpers.h"

using namespace petrichor;
using namespace petrichor::tests;
using bytecode::ValueType;

// Constants (spec §4.5): the compiler computes scalars, the engine the rest
// when the class loads, and what loading could fail on is reported here.

namespace {

using Errors = std::vector<std::string>;

const bytecode::Constant &named(const bytecode::Module &p_module, const std::string &p_name) {
	for (const bytecode::NamedConstant &constant : p_module.named_constants) {
		if (constant.name == p_name) {
			return p_module.constants[constant.constant];
		}
	}
	FAIL("no constant " << p_name);
	static const bytecode::Constant none;
	return none;
}

bytecode::Module constants_of(const std::string &p_source) {
	const compiler::CompileResult result = compile(p_source);
	REQUIRE_MESSAGE(result.ok(), messages(result));
	return result.module;
}

} // namespace

TEST_CASE("the compiler computes constants of scalars") {
	const bytecode::Module module = constants_of(R"(extends Node
const DAY_MS = 24 * 60 * 60 * 1000
const MASK = 1 << 3 | 1
const RATE = 1.0 / 60.0 * 30.0
const PREFIX = "game/"
const KEY = PREFIX + "speed"
const TWICE = HALF * 2
const HALF = 3
const ROUNDED = roundi(2.5) + posmod(-7, 3) + clampi(9, 0, 5)
const SMALL = -9223372036854775807 - 1
const WRAPPED = SMALL - 1
const CHOSEN = 1 if DAY_MS > 0 else 2
const LIMITS = [10, 20, 30]
const SECOND = LIMITS[1] + LIMITS[-1]
const LEFT = 2 ** 3 ** 2
const SIGNED = -2 ** 2 + 2 ** -1 + (-1) ** -3
const EXACT = 3 ** 39
const WRAPS = 2 ** 64
)");
	CHECK(named(module, "DAY_MS").i == 86400000);
	CHECK(named(module, "MASK").i == 9);
	CHECK(named(module, "RATE").f == 1.0 / 60.0 * 30.0);
	CHECK(named(module, "KEY").s == "game/speed");
	CHECK(named(module, "TWICE").i == 6);
	CHECK(named(module, "ROUNDED").i == 3 + 2 + 5);
	CHECK(named(module, "WRAPPED").i == INT64_MAX);
	CHECK(named(module, "CHOSEN").i == 1);
	CHECK(named(module, "SECOND").i == 50);
	// ** is left-associative and binds tighter than the signs, as in GDScript;
	// of ints exact, wrapping as * does (spec §5.1).
	CHECK(named(module, "LEFT").i == 64);
	CHECK(named(module, "SIGNED").i == -4 + 0 - 1);
	CHECK(named(module, "EXACT").i == 4052555153018976267);
	CHECK(named(module, "WRAPS").i == 0);
	CHECK(storage(module, named(module, "LIMITS").type) == ValueType::Array);
	CHECK(named(module, "LIMITS").args.size() == 3);
}

TEST_CASE("the engine computes what has engine types when the class loads") {
	const bytecode::Module module = constants_of(R"(extends Node
const EXTENTS := Vector3.ONE / 2.0
const TINT = Color.WHITE * Color(1, 1, 1, 0.5)
const ANGLE = deg_to_rad(30)
const SLOPE = tan(ANGLE)
const CORNERS: Array[Vector2i] = [Vector2i.ONE, -Vector2i.ONE]
const STATIC = Color("#82010b")
const SPLIT = Vector2i(4, 2).x / 2
const ROOT = 2.0 ** 0.5
)");
	CHECK(named(module, "SPLIT").op == compiler::OP_DIVIDE);
	CHECK(named(module, "ROOT").op == compiler::OP_POWER);
	CHECK(has_error("const SPLIT = 4 / Vector2i(4, 2).x\n", "The divisor of an int must be known at compile time (spec §4.5)."));
	const bytecode::Constant &extents = named(module, "EXTENTS");
	CHECK(extents.op == compiler::OP_DIVIDE);
	REQUIRE(extents.args.size() == 2);
	CHECK(extents.args[0].builtin == "ONE");
	CHECK(extents.args[1].f == 2.0);
	const bytecode::Constant &angle = named(module, "ANGLE");
	REQUIRE(angle.utility >= 0);
	CHECK(module.utility_calls[angle.utility].function == "deg_to_rad");
	// The int is converted where the engine takes a float, as written.
	REQUIRE(angle.args.size() == 1);
	CHECK(storage(module, angle.args[0].type) == ValueType::Float);
	CHECK(angle.args[0].f == 30.0);
	CHECK(named(module, "SLOPE").args[0].utility >= 0);
	CHECK(named(module, "CORNERS").args[1].op == compiler::OP_NEGATE);
	CHECK(named(module, "STATIC").constructor >= 0);
}

TEST_CASE("what loading could fail on is a compile error, where it is") {
	CHECK(errors_of("const N = 10 / 0\n") == Errors{ "1:14: Division by zero." });
	CHECK(errors_of("const N = 10 % (5 - 5)\n") == Errors{ "1:14: Modulo by zero." });
	CHECK(errors_of("const N = posmod(10, 0)\n") == Errors{ "1:11: Modulo by zero." });
	CHECK(errors_of("const N = 1 << -1\n") == Errors{ "1:13: A shift takes operands that are not negative (spec §4.5)." });
	CHECK(errors_of("const N = 1 << 64\n") == Errors{ "1:13: A shift by 64 or more bits is undefined (spec §4.5)." });
	CHECK(errors_of("const N = 0 ** -1\n") == Errors{ "1:13: 0 ** -1 divides by zero." });
	CHECK(errors_of("const N = Vector2i(4, 2).x ** 2\n") == Errors{ "1:28: Both sides of ** of two ints must be known at compile time (spec §4.5)." });
	CHECK(errors_of("const N = int(1e300)\n") == Errors{ "1:11: Cannot convert 1.0000000000000001e+300 to int: out of range." });
	CHECK(errors_of("const L = [1, 2]\nconst E = L[5]\n") == Errors{ "2:11: Index 5 is out of bounds (size 2)." });
	CHECK(errors_of("const D = {\"a\": 1, \"a\": 2}\n") == Errors{ "1:20: The key \"a\" is already in this dictionary." });
	CHECK(errors_of("const V = Vector2i(4, 4) / Vector2i(1, 0)\n") == Errors{ "1:26: The divisor of an integer vector must be known at compile time and not zero (spec §4.5)." });
	CHECK(errors_of("const C = Color(\"dark green\")\n") == Errors{ "1:11: A color name is looked up when the class loads, so constants write the constant instead: Color.DARK_GREEN (spec §4.5)." });
	CHECK(errors_of("const Q = Quaternion(Vector3.UP, 1.0)\n") == Errors{ "1:11: This constructor of Quaternion checks its arguments in debug builds, so constant expressions cannot use it (spec §4.5)." });
	CHECK(errors_of("const S = \"%d\" % [3]\n") == Errors{ "1:16: Formatting with % is not supported in constant expressions (spec §4.5)." });
	CHECK(errors_of("const K = 100 / roundi(sin(0.5))\n") == Errors{ "1:17: The float that roundi() rounds to an int must be known at compile time (spec §4.5)." });
}

TEST_CASE("an initializer that is not a constant expression is reported at what is not") {
	CHECK(errors_of("const R = randi() + 1\n") == Errors{ "1:11: \"randi()\" is not a constant expression: of calls, only constructors of built-in types and math functions are (spec §4.5)." });
	CHECK_MESSAGE(errors_of("var hp = 3\nconst H = hp * 2\n") == Errors{ "2:11: \"hp\" is a variable, not a constant (spec §4.5)." }, joined(errors_of("var hp = 3\nconst H = hp * 2\n")));
	CHECK(errors_of("const A = B\nconst B = A\n") == Errors{ "2:11: The constant \"A\" depends on its own value." });
}

TEST_CASE("a constant of T? is used as Some of its value") {
	const compiler::CompileResult result = compile(R"(extends Node
const LIMIT: int? = 3
const RATE: float? = 2
const NOTHING: int? = None
const FALLBACK: Node? = None
const ORIGIN: Vector2? = Vector2.ZERO
func f() -> float:
	var total := LIMIT.unwrap() + RATE.unwrap()
	if NOTHING == None and FALLBACK == None and ORIGIN != None and LIMIT == 3:
		total += 1
	return total
)");
	REQUIRE_MESSAGE(result.ok(), messages(result));
	std::string error;
	CHECK_MESSAGE(bytecode::verify(result.module, error), error);
	CHECK(named(result.module, "LIMIT").i == 3);
	CHECK(named(result.module, "RATE").f == 2.0);
	find_instruction(function_named(result.module, "f"), bytecode::Op::SOME);
	CHECK(errors_of("const N = None\n") == Errors{ "1:11: The constant needs a type: None does not say which T? it is (spec §4.5)." });
}

TEST_CASE("constants are read-only") {
	CHECK(errors_of(in_function("MAX = 3", "func f() -> void:") + "const MAX = 1\n") == Errors{ "4:2: Cannot assign to the constant \"MAX\"." });
	CHECK(has_error("const L: Array[int] = [1]\nfunc f() -> void:\n\tL.append(2)\n", "\"L\" is a constant, whose containers are read-only (spec §4.5)."));
	CHECK(has_error("const L: Array[int] = [1]\nfunc f() -> void:\n\tL[0] = 2\n", "\"L\" is a constant, whose containers are read-only (spec §4.5)."));
	CHECK(has_error("func f() -> void:\n\tconst X = 3\n\tX = 4\n", "Cannot assign to the constant \"X\"."));
	CHECK(compile("const L: Array[int] = [1]\nfunc f() -> int:\n\treturn L.size() + L[0]\n").ok());
}

TEST_CASE("a constant is used by its name, through self and in static functions") {
	const compiler::CompileResult result = compile(R"(extends Node
const SPEED = 10
const SPEEDS = [1.0, 2.0]
static func limit() -> int:
	return SPEED * 2
func f() -> float:
	const LOCAL = SPEED + 1
	return self.SPEED + LOCAL + SPEEDS[0]
)");
	REQUIRE_MESSAGE(result.ok(), messages(result));
	const bytecode::TypeDesc &speeds = result.module.types[named(result.module, "SPEEDS").type];
	CHECK(speeds.kind == bytecode::TypeKind::Array);
	CHECK(storage(result.module, speeds.element) == ValueType::Float);
}

TEST_CASE("annotations take constants the compiler knows") {
	const compiler::CompileResult result = compile(R"(extends Node
const MAX = 100
const STEP = MAX / 50
const LABEL = "Insert an expression..."
@export_range(0, MAX, STEP) var amount: int
@export_multiline(LABEL) var text: String
)");
	REQUIRE_MESSAGE(result.ok(), messages(result));
	CHECK(member_named(result.module, "amount").hint_string == "0.0,100.0,2.0");
	CHECK(member_named(result.module, "text").hint_string == "Insert an expression...");
	CHECK(has_error("const ANGLE = deg_to_rad(30)\n@export_range(0, ANGLE) var a: float\n", "An annotation argument must be known at compile time"));
}

TEST_CASE("bitwise operators and their assignments take ints at run time") {
	const compiler::CompileResult result = compile(in_function("var flags := x & 3 | ~n ^ 1\nflags <<= n\nflags >>= 1\nflags &= 7\nflags |= 8\nflags ^= 2\nreturn flags", "func f(x: int, n: int) -> int:"));
	REQUIRE_MESSAGE(result.ok(), messages(result));
	std::string error;
	CHECK_MESSAGE(bytecode::verify(result.module, error), error);
	const bytecode::Function &f = function_named(result.module, "f");
	for (const bytecode::Op op : { bytecode::Op::AND_INT, bytecode::Op::OR_INT, bytecode::Op::XOR_INT, bytecode::Op::NOT_INT, bytecode::Op::SHL_INT, bytecode::Op::SHR_INT }) {
		find_instruction(f, op);
	}
	CHECK(has_error(in_function("var flags := 1\nflags |= 1.5"), "Invalid operands int and float for \"|\"."));
	CHECK(has_error(in_function("var on := true\nprint(~on)"), "\"~\" requires int, found bool."));
}

TEST_CASE("constants of other classes, pub to all and the rest to subclasses") {
	FakeProject project;
	project.add_script("res://player.pet", "class_name Player extends Node\npub const MAX = 5\nconst SECRET = 1\n", "Player");
	CHECK(compile_in("extends Node\nconst DOUBLE = Player.MAX * 2\nfunc f(p: Player) -> int:\n\treturn DOUBLE + p.MAX\n", project).ok());
	const compiler::CompileResult hidden = compile_in("extends Node\nconst S = Player.SECRET\n", project);
	CHECK(contains(messages(hidden), "The constant \"SECRET\" of Player is not pub."));
	CHECK(compile_in("extends Player\nconst S = SECRET + Player.SECRET\n", project).ok());
}
