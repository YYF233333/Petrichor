#include "doctest.h"
#include "helpers.h"

using namespace petrichor;
using namespace petrichor::tests;

// Enums without payloads (spec §3.7).

namespace {

const char *const STATE = "enum State { IDLE, RUN, JUMP = 5, FALL }\nenum Other { A, B }\nenum { LOW, HIGH = 4, TOP }\nenum Level { EASY = 1, HARD = 2 }\n\n";

std::string errors(const std::string &p_body, const std::string &p_members = "") {
	return joined(errors_of(std::string("class_name Moves\nextends Node\n\n") + STATE + p_members + "func f(s: State, n: int) -> void:\n" + indented(p_body)));
}

FakeProject project() {
	FakeProject project;
	project.add_script("res://player.pet", "class_name Player\nextends Node\n\npub enum State { IDLE, RUN }\nenum Secret { HIDDEN }\npub enum { MAX_LIVES = 3 }\n", "Player");
	return project;
}

} // namespace

TEST_CASE("an enum's values count on from the one before, and its values are ints") {
	CHECK(errors("var i: int = State.FALL + 1\nvar f: float = State.RUN\nn = s\nvar a: State = State(6)\nvar t: State = State(HIGH + 1)").empty());
	CHECK(contains(errors("var a := State(4)"), "4 is none of the values of State."));
	CHECK(contains(errors("var a := State(HIGH)"), "4 is none of the values of State."));
	CHECK(errors("var x: bool = s == State.RUN and s != 1 and s < State.FALL and State.RUN in [s]\nvar m: int = -s + ~State.RUN").empty());
	// Written as a constant int known at compile time, also from earlier values (spec §4.5).
	CHECK(errors("pass", "enum Flags { ONE = 1, TWO = ONE << 1, FOUR = TWO << 1 }\n\n").empty());
	CHECK(contains(errors("pass", "enum Bad { HALF = 0.5 }\n\n"), "The values of an enum are ints, not float."));
	CHECK(contains(errors("pass", "enum Bad { X, X }\n\n"), "\"X\" is already a value of Bad."));
	CHECK(contains(errors("pass", "enum Bad { X = Y, Y }\n\n"), "Identifier \"Y\" is not declared"));
	CHECK(contains(errors("pass", "enum { LOW }\n\n"), "\"LOW\" is already declared."));
	CHECK(contains(errors("pass", "enum State { X }\n\n"), "\"State\" is already declared."));
}

TEST_CASE("an int becomes an enum only explicitly, and two enums do not mix (spec §3.7)") {
	CHECK(contains(errors("s = 1"), "Cannot assign a value of type int as Moves.State. Convert explicitly with State(x), which checks that it is one of the values."));
	CHECK(contains(errors("s += 1"), "Cannot assign a value of type int as Moves.State."));
	CHECK(contains(errors("s = Other.A"), "Cannot assign a value of type Moves.Other as Moves.State."));
	CHECK(contains(errors("var t := n as State"), "as only converts objects; convert to Moves.State with Moves.State(x) or Moves.State.try_from(x)."));
	CHECK(contains(errors("var t := State(1.5)"), "State() converts an int, not float. Convert it with int(x) first."));
	CHECK(errors("s = State(n)\ns = State(Other.B)\nvar t: State = State((s + 1) % State.size())\nvar u: Result[State, ConversionError] = State.try_from(n)").empty());
	CHECK(contains(errors("var t := State.WALK"), "State has no value \"WALK\"."));
}

TEST_CASE("an enum's name as a value is a read-only Dictionary typed by the enum (spec §3.7)") {
	CHECK(errors("var names: Array[String] = State.keys()\nvar values: Array[State] = State.values()\nvar count: int = State.size()\n"
				 "var key: String? = State.find_key(s)\nvar maybe: State? = State.get(\"RUN\")\nvar run: State = State[\"RUN\"]\nvar has: bool = State.has(\"RUN\")\n"
				 "for name in State:\n\tprint(name)")
					.empty());
	CHECK(contains(errors("var values: Array[int] = State.values()"), "Cannot assign a value of type Array[Moves.State] as Array[int]."));
	CHECK(contains(errors("State.clear()"), "is a constant, whose containers are read-only"));
}

TEST_CASE("an enum without the value 0 has no default value (spec §3.7, §4.3)") {
	CHECK(errors("var t: State\nvar o: Other", "var state: State\n\n").empty());
	CHECK(contains(errors("var l: Level"), "\"l\" needs an initial value: Level has no value 0 to start at."));
	CHECK(errors("level = Level.HARD", "var level: Level\n\n").empty()); // A member that must be assigned (spec §6.2).
	CHECK(errors("pass", "var level := Level.EASY\n\n").empty());
}

TEST_CASE("a match over an enum handles every value or has _ (spec §5.6)") {
	CHECK(errors("match s:\n\tState.IDLE, State.RUN:\n\t\tpass\n\tState.JUMP:\n\t\tpass\n\tState.FALL:\n\t\tpass").empty());
	CHECK(errors("match s:\n\tState.IDLE:\n\t\tpass\n\t_:\n\t\tpass").empty());
	CHECK(contains(errors("match s:\n\tState.IDLE:\n\t\tpass\n\tState.RUN:\n\t\tpass"), "This match over State does not handle JUMP and FALL; handle them or add a `_` branch."));
	// Branches with `when` do not count.
	CHECK(contains(errors("match s:\n\tState.IDLE, State.RUN, State.JUMP:\n\t\tpass\n\tState.FALL when n > 0:\n\t\tpass"), "does not handle FALL;"));
	CHECK(errors("match n:\n\t1:\n\t\tpass").empty());
	// Closed, it takes every path, and only a value from the engine misses every branch.
	const char *name = "func name(s: State) -> String:\n\tmatch s:\n\t\tState.IDLE, State.RUN:\n\t\t\treturn \"ground\"\n\t\tState.JUMP:\n\t\t\treturn \"up\"\n\t\tState.FALL:\n\t\t\treturn \"down\"\n\n";
	CHECK(errors("pass", name).empty());
	CHECK(contains(errors("pass", "func name(s: State) -> String:\n\tmatch s:\n\t\tState.IDLE:\n\t\t\treturn \"idle\"\n\t\t_ when s == State.RUN:\n\t\t\treturn \"run\"\n\n"), "Not all paths of \"name()\" return a value."));
	const compiler::CompileResult result = compile(std::string("extends Node\n\n") + STATE + name);
	REQUIRE_MESSAGE(result.ok(), messages(result));
	CHECK(contains(bytecode::disassemble(result.module, function_named(result.module, "name")), "CHECK_ENUM"));
}

TEST_CASE("an enum of another class is written through its class, if pub (spec §3.7, §4.2)") {
	const auto errors_in = [](const std::string &p_body) {
		return messages(compile_in("extends Node\n\nfunc f(p: Player.State) -> Player.State:\n" + indented(p_body), project()));
	};
	CHECK(errors_in("var lives: int = Player.MAX_LIVES\nvar names: Array[String] = Player.State.keys()\nreturn Player.State.RUN if p == Player.State.IDLE else Player.State(0)").empty());
	CHECK(contains(errors_in("return 1"), "Cannot return a value of type int as Player.State."));
	CHECK(contains(errors_in("var s: Player.Secret = Player.Secret.HIDDEN\nreturn p"), "Player.Secret is not pub."));
}

TEST_CASE("a constant initialized with an enum's name names the same enum (spec §3.7)") {
	CHECK(errors("var t: ALIAS = ALIAS.RUN\nt = ALIAS(n)\nvar u: State = t\nvar names: Array[String] = ALIAS.keys()\nmatch t:\n\tALIAS.IDLE, ALIAS.RUN, ALIAS.JUMP, ALIAS.FALL:\n\t\tpass", "const ALIAS = State\n\n").empty());
	// gdUnit4's `const STATE = GdUnitInspectorTreeConstants.STATE`.
	const compiler::CompileResult result = compile_in("extends Node\nconst STATE = Player.State\n\nfunc f(s: STATE) -> Player.State:\n\treturn STATE.RUN if s == STATE.IDLE else STATE(0)\n", project());
	CHECK_MESSAGE(result.ok(), messages(result));
	// An engine's enum too, whose values its name also qualifies.
	const char *const ease = "const EASE = Tween.EaseType\n\n";
	CHECK(errors("var e: EASE = EASE.EASE_IN\ne = EASE(n)\nvar t: Tween.EaseType = e\nvar names: Array[String] = EASE.keys()\n"
				 "var o := Tween.EaseType.EASE_OUT\nt = o\nvar b: MouseButton = MouseButton.MOUSE_BUTTON_LEFT\nvar a: Vector3.Axis = Vector3.Axis.AXIS_Y",
			ease)
					.empty());
	CHECK(contains(errors("var e := EASE.EASE_SIDEWAYS", ease), "Tween.EaseType has no value \"EASE_SIDEWAYS\"."));
}

TEST_CASE("the engine's enums are enum types too, and its bitfields stay int (spec §3.7)") {
	const auto errors_in = [](const std::string &p_body) {
		return joined(errors_of(in_function(p_body, "func f(n: int, e: Tween.EaseType) -> void:")));
	};
	CHECK(errors_in("Input.mouse_mode = Input.MOUSE_MODE_CAPTURED\nvar m: Input.MouseMode = Input.mouse_mode\nvar i: int = m + e\n"
					"var k: Key = KEY_ESCAPE\nvar b: MouseButton = MouseButton(n)\nvar t := Tween.EaseType(n)\nvar a: Vector3.Axis = Vector3.Axis(0)\nvar v: Variant.Type = TYPE_INT")
					.empty());
	CHECK(type_of("KEY_ESCAPE") == "Key");
	CHECK(type_of("KEY_MASK_SHIFT") == "int"); // A value of the bitfield KeyModifierMask.
	CHECK(contains(errors_in("Input.mouse_mode = 2"), "Cannot assign a value of type int as Input.MouseMode. Convert explicitly with Input.MouseMode(x)"));
	// Open: the engine gives values outside them, and later versions add more.
	CHECK(errors_in("var b := MouseButton(100)\nmatch e:\n\tTween.EASE_IN:\n\t\tpass\n\t_:\n\t\tpass").empty());
	CHECK(contains(errors_in("match e:\n\tTween.EASE_IN, Tween.EASE_OUT, Tween.EASE_IN_OUT, Tween.EASE_OUT_IN:\n\t\tpass"), "A match over Tween.EaseType needs a `_` branch: the engine's enums are open"));
	const compiler::CompileResult result = compile(in_function("var b := MouseButton(n)", "func f(n: int) -> void:"));
	REQUIRE_MESSAGE(result.ok(), messages(result));
	CHECK(!contains(bytecode::disassemble(result.module, function_named(result.module, "f")), "CHECK_ENUM"));
}

TEST_CASE("the name of an engine's enum as a value is typed as one of this language's (spec §3.7)") {
	const auto errors_in = [](const std::string &p_body) { return joined(errors_of(in_function(p_body))); };
	CHECK(errors_in("var names: Array[String] = Tween.EaseType.keys()\nvar values: Array[Tween.EaseType] = Tween.EaseType.values()\n"
					"var b: MouseButton? = MouseButton.get(\"MOUSE_BUTTON_LEFT\")\nvar axes: int = Vector3.Axis.size()\nvar mode: ProcessMode = ProcessMode[\"PROCESS_MODE_ALWAYS\"]")
					.empty());
	CHECK(contains(errors_in("var values: Array[int] = Tween.EaseType.values()"), "Cannot assign a value of type Array[Tween.EaseType] as Array[int]."));
}

TEST_CASE("an enum compiles to an int checked where it is made from one") {
	const compiler::CompileResult result = compile_in(std::string("extends Node\n\n") + STATE + "func f(n: int) -> State:\n\treturn State(n)\n", FakeProject());
	REQUIRE_MESSAGE(result.ok(), messages(result));
	bool found = false;
	std::string names;
	for (const bytecode::TypeDesc &type : result.module.types) {
		names += type.name + ";";
		if (type.kind == bytecode::TypeKind::Enum && type.name == "res://test.pet::State") {
			found = true;
			CHECK(type.value == bytecode::ValueType::Int);
			REQUIRE(type.values.size() == 4);
			CHECK(type.values[3].name == "FALL");
			CHECK(type.values[3].value == 6);
		}
	}
	CHECK_MESSAGE(found, names);
	CHECK(contains(bytecode::disassemble(result.module, function_named(result.module, "f")), "CHECK_ENUM"));
}
