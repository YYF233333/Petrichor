#include "doctest.h"
#include "helpers.h"

using namespace petrichor;
using namespace petrichor::tests;

TEST_CASE("a well-formed class compiles") {
	const char *source = R"(class_name Counter
extends Node

@export pub var speed: float = 2.0
var count := 0
var label = "n"

func _process(delta):
	count += 1
	var step: float = delta * speed

pub func add(a: int, b: int) -> int:
	return a + b

pub func clamp_step(x: float) -> float:
	var y := x * 2
	if y > 10.0:
		return 10.0
	elif y < 0:
		return 0
	while y < 1.0:
		y += 1
	return y

func _ready():
	print("ready ", count, " ", label)
	set_process(true)
	var children := get_child_count()
	var half: int = 7 / 2
)";
	CHECK(joined(errors_of(source)) == "");
}

TEST_CASE("a class annotation is reported once, and the header after it still reads") {
	CHECK(joined(errors_of("@tool\nclass_name X\nextends Node\n")) == "1:2: @tool is not in the first version: scripts of this language do not run in the editor.\n");
	CHECK(joined(errors_of("@icon(\"res://a#b.svg\") class_name X extends Node\n")) == "");
	// GDScript takes a string literal, and only once (modules/gdscript/gdscript_parser.cpp:4422-4440, 4465-4473).
	CHECK(joined(errors_of("@icon(\"\")\nextends Node\n")) == "1:7: @icon takes the path to the icon.\n");
	CHECK(contains(joined(errors_of("const ICON := \"a.svg\"\n@icon(ICON)\nextends Node\n")), "belongs to the class header"));
	CHECK(contains(joined(errors_of("@icon(\"a.svg\")\n@icon(\"b.svg\")\nextends Node\n")), "The class already has @icon."));
	CHECK(joined(errors_of("@static_unload\nextends Node\n")) == "");
	CHECK(joined(errors_of("extends Node\n@export pub var x := 1\n")) == "");
}

// modules/gdscript/gdscript_parser.cpp:798-806, 1182-1190, 2177-2182.
TEST_CASE("a string on a line of its own is a comment") {
	CHECK(errors_of("\"What the class is for.\"\nclass_name X\nextends Node\n'Its state.'\nvar x := 1\nr\"a\\b\"\n\nfunc f() -> int:\n\t\"What f gives.\"\n\treturn x\n").empty());
	CHECK(contains(joined(errors_of("extends Node\n\"a\" var x := 1\n")), "Expected a line break after a comment string."));
}

// Spec §2.2: the keywords this language adds are keywords only where their syntax begins.
TEST_CASE("pub, struct, interface, implements, async and None are names elsewhere") {
	CHECK(errors_of("extends Node\nenum TranslationSource { None, Guess }\npub var pub := 1\nvar interface := TranslationSource.None\n\n\n"
					"pub func struct(implements: int) -> int:\n\tvar async := implements + pub\n\treturn async\n")
					.empty());
	// A bare None is always the empty Option, so nothing its bare name reaches is named None.
	CHECK(has_error("extends Node\nvar None := 1\n", "None cannot be a variable name: a bare None is always the empty Option."));
	CHECK(has_error("extends Node\n\n\nfunc None():\n\tpass\n", "None cannot be a function name"));
	CHECK(has_error("extends Node\nenum { None, Guess }\n", "None cannot be the name of a value"));
	CHECK(has_error("extends Node\n\n\nstruct Hit:\n\tvar None := 0\n", "None cannot be a field name"));
	// No class could implement an interface member named None.
	CHECK(has_error("extends Node\n\n\ninterface Named:\n\tfunc None() -> void\n", "None cannot be a method name"));
}

// modules/gdscript/gdscript_parser.cpp:187, 5238-5298.
TEST_CASE("@rpc configures how other peers call a method, which they find by name") {
	const compiler::CompileResult result = compile("extends Node\n@rpc(\"any_peer\", \"call_local\", \"unreliable\", 1)\npub func hit(amount: int):\n\tpass\n\n@rpc\npub func sync_state():\n\tpass\n");
	REQUIRE_MESSAGE(result.ok(), messages(result));
	const bytecode::Function &hit = function_named(result.module, "hit");
	CHECK((hit.rpc.mode == 1 && hit.rpc.call_local && hit.rpc.transfer_mode == 0 && hit.rpc.channel == 1));
	const bytecode::Function &sync = function_named(result.module, "sync_state");
	CHECK((sync.rpc.mode == 2 && !sync.rpc.call_local && sync.rpc.transfer_mode == 2 && sync.rpc.channel == 0));
	CHECK(function_named(compile("extends Node\nfunc f():\n\tpass\n").module, "f").rpc.mode == 0);

	const auto rpc_errors = [](const std::string &p_annotation) { return joined(errors_of("extends Node\n" + p_annotation + "\nfunc f():\n\tpass\n")); };
	CHECK(contains(rpc_errors("@rpc(\"server\")"), "@rpc takes \"call_local\" or \"call_remote\""));
	CHECK(contains(rpc_errors("@rpc(\"any_peer\", \"authority\")"), "@rpc takes one of \"any_peer\" and \"authority\"."));
	CHECK(contains(rpc_errors("@rpc(\"any_peer\", \"call_local\", \"reliable\", \"x\")"), "the channel, an int"));
	CHECK(contains(rpc_errors("@rpc\n@rpc"), "A function takes @rpc once."));
	CHECK(contains(rpc_errors("@rpc"), "An @rpc function must be pub: other peers call it by name."));
	CHECK(contains(joined(errors_of("extends Node\n@rpc\nvar x := 1\n")), "@rpc applies to functions."));
	CHECK(contains(rpc_errors("@export"), "@export applies to variables."));
}

// The compiler has no warnings yet; which names suppress one is for the lints
// to say (spec §13.2). GDScript lets them annotate members and statements, and
// open and close regions (modules/gdscript/gdscript_parser.cpp:182-184).
TEST_CASE("@warning_ignore takes the names of warnings") {
	CHECK(errors_of("extends Node\n@warning_ignore_start(\"unused_variable\")\n@warning_ignore(\"unused_parameter\")\nfunc f(x: int):\n\t@warning_ignore(\"integer_division\")\n\tvar y := x / 2\n\t@warning_ignore_start(\"narrowing_conversion\")\n\tprint(y)\n\t@warning_ignore_restore(\"narrowing_conversion\")\n@warning_ignore_restore(\"unused_variable\")\n@warning_ignore(\"x\")\nvar z := 1\n").empty());
	CHECK(contains(joined(errors_of(in_function("@warning_ignore\nprint(1)"))), "@warning_ignore takes the names of warnings."));
	CHECK(contains(joined(errors_of(in_function("@warning_ignore(1)\nprint(1)"))), "takes the names of warnings, as strings"));
	CHECK(contains(joined(errors_of(in_function("@export pub var y := 1"))), "A statement takes only @warning_ignore."));
}

// Spec §3.14, implicit conversions.
TEST_CASE("float to int needs an explicit conversion") {
	CHECK(has_error(in_function("var hp: int = 10\nhp = 2.5"), "Convert explicitly with int(x)."));
	CHECK(errors_of(in_function("var hp: int = int(2.5)")).empty());
	CHECK(errors_of(in_function("var x: float = 1")).empty());
	// The other explicit conversions are the engine's constructors.
	CHECK(errors_of(in_function("var b := bool(2)\nvar i: int = int(\"5\") + int()\nvar f := float(\"1.5\")\nvar s := String(&\"a\")")).empty());
	CHECK(has_error(in_function("var s := String(5)"), "No constructor of String takes (int)."));
}

TEST_CASE("an int argument is accepted for a float parameter") {
	const char *source = "extends Node\n\nfunc half(x: float) -> float:\n\treturn x / 2\n\nfunc f() -> void:\n\thalf(3)\n";
	CHECK(errors_of(source).empty());
}

TEST_CASE("the integer vectors and Rect2i widen to their float counterparts, and nothing narrows") {
	CHECK(errors_of(in_function("var p: Vector2 = Vector2i(1, 2)\nvar q: Vector3 = Vector3i.ONE\nvar r: Rect2 = Rect2i()\nvar s: Vector4 = Vector4i()")).empty());
	CHECK(has_error(in_function("var p: Vector2i = Vector2(1, 2)"), "Cannot assign a value of type Vector2 as Vector2i."));
	CHECK(has_error(in_function("var p: Vector2 = Vector3i()"), "Cannot assign a value of type Vector3i as Vector2."));
}

TEST_CASE("% requires int operands") {
	CHECK(has_error(in_function("var x := 5.0 % 2.0"), "fmod"));
}

// Spec §5.2, equality.
TEST_CASE("comparisons") {
	CHECK(errors_of(in_function("var a := 1 == 1.0")).empty());
	CHECK(has_error(in_function("var hp := 3\nvar b := hp == \"3\""), "Cannot compare int with String"));
}

// Spec §5.4, narrowing: conditions must be bool.
TEST_CASE("a condition must be bool") {
	CHECK(has_error(in_function("if 1:\n\tpass"), "Compare explicitly, for example x != 0."));
	CHECK(has_error(in_function("var s := \"\"\nwhile s:\n\tpass"), "x != \"\""));
	CHECK(has_error(in_function("var b := not 1"), "requires bool"));
	CHECK(has_error(in_function("var b := 1 and true"), "requires bool operands"));
	// The fix for `not x` by x's type.
	CHECK(has_error(in_function("var s := \"\"\nif not s:\n\tpass"), "\"not\" requires bool, found String. Compare explicitly, for example x == \"\"."));
	CHECK(has_error(in_function("var n: Node? = None\nif not n:\n\tpass"), "x == None."));
	CHECK(has_error(in_function("var a: Array[int] = []\nif not a:\n\tpass"), "Test it explicitly, for example x.is_empty()."));
	CHECK(has_error(in_function("var a: Array[int] = []\nif a:\n\tpass"), "not x.is_empty()."));
	CHECK(has_error(in_function("var v: Variant = 1\nif not v:\n\tpass"), "x == None."));
}

// Spec §4.6, signatures.
TEST_CASE("overrides take their types from the engine") {
	CHECK(errors_of("extends Node\n\nfunc _process(delta):\n\tvar d: float = delta\n").empty());
	CHECK(has_error("extends Node\n\nfunc _process(delta):\n\tvar d: int = delta\n", "Convert explicitly"));
	CHECK(has_error("extends Node\n\nfunc _process(delta: int):\n\tpass\n", "declares this parameter as float"));
	CHECK(has_error("extends Node\n\nfunc _process():\n\tpass\n", "1 parameter"));
	CHECK(has_error("extends Node\n\nfunc get_child_count() -> int:\n\treturn 0\n", "cannot be overridden"));
}

TEST_CASE("a function without a return type returns void") {
	CHECK(has_error("extends Node\n\nfunc f():\n\treturn 1\n", "returns void"));
	CHECK(has_error("extends Node\n\nfunc f(x):\n\tpass\n", "needs a type"));
}

TEST_CASE("every path of a function with a result returns") {
	CHECK(has_error("extends Node\n\nfunc f(x: int) -> int:\n\tif x > 0:\n\t\treturn 1\n", "Not all paths"));
	CHECK(errors_of("extends Node\n\nfunc f(x: int) -> int:\n\tif x > 0:\n\t\treturn 1\n\telse:\n\t\treturn 2\n").empty());
	CHECK(has_error("extends Node\n\nfunc f() -> int:\n\treturn\n", "must return a value"));
}

TEST_CASE("names") {
	CHECK(has_error(in_function("x = 1"), "\"x\" is not declared"));
	CHECK(has_error(in_function("var x"), "needs a type or an initial value"));
	CHECK(has_error(in_function("var x := 1\nvar x := 2"), "already declared"));
	CHECK(has_error(in_function("var x := 1\nif x > 0:\n\tvar x := 2"), "already declared in an enclosing block"));
	CHECK(has_error(in_function("var i := 1\nfor i in 3:\n\tpass"), "already declared in an enclosing block"));
	CHECK(has_error(in_function("if n > 0:\n\tvar n := 2", "func f(n: int) -> void:"), "already a parameter"));
	CHECK(errors_of(in_function("if true:\n\tvar x := 1\nelse:\n\tvar x := 2\nvar x := 3")).empty());
	CHECK(has_error(in_function("break"), "only valid in a loop"));
	CHECK(has_error(in_function("nope()"), "not found"));
	CHECK(has_error("extends Missing\n", "Unknown base class"));
}

// Spec §5.7.
TEST_CASE("for loops") {
	CHECK(errors_of(in_function("var total := 0\nfor i in 3:\n\ttotal += i\n\tif i == 1:\n\t\tbreak")).empty());
	CHECK(errors_of(in_function("for i: int in 3:\n\tpass")).empty());
	CHECK(has_error(in_function("for i: float in 3:\n\tpass"), "The elements are int, which the loop variable of type float cannot hold."));
	CHECK(has_error(in_function("for i in 3:\n\tpass\nvar j := i"), "\"i\" is not declared"));
	CHECK(has_error(in_function("for x in 2.5:\n\tpass"), "iterate over range(...) instead"));
	CHECK(has_error(in_function("for x in Vector2(2, 3):\n\tpass"), "iterate over range(...) instead"));
	CHECK(has_error(in_function("for x in true:\n\tpass"), "Cannot iterate over bool."));
	// GDScript's range(), of ints only, whose step is not 0.
	CHECK(errors_of(in_function("for i: int in range(1, 9, 2):\n\tpass\nvar all: Array[int] = range(3)")).empty());
	CHECK(has_error(in_function("for i in range(2.5):\n\tpass"), "Cannot pass a value of type float as int"));
	CHECK(has_error(in_function("for i in range(0, 9, 0):\n\tpass"), "The step of range() is 0."));
	CHECK(has_error(in_function("var r := range()"), "takes 1 to 3 argument(s), 0 given"));
}

// Spec §2.6.
TEST_CASE("GDScript's own functions") {
	CHECK(type_of("len(s) + len(a) + len(d) + len(p) + ord(s)", "var s := \"a\"\nvar a: Array[Node] = []\nvar d := {}\nvar p := PackedVector2Array()") == "int");
	CHECK(type_of("char(65)") == "String");
	CHECK(type_of("get_stack()") == "Array[Dictionary]");
	CHECK(has_error(in_function("var n := len(3)"), "len() takes a String, a StringName, an Array, a Dictionary or a packed array, not int."));
	CHECK(has_error(in_function("var c := Color8(1, 2, 3)"), "use Color.from_rgba8() instead"));
	// As `v is T`, which narrows.
	CHECK(errors_of(in_function("var n: Node = Node.new()\nif is_instance_of(n, Node3D):\n\tvar m: Node3D = n")).empty());
	CHECK(type_of("is_instance_of(v, TYPE_INT)", "var v: Variant = 1") == "bool");
	// Of a Script value, a bool that does not narrow.
	CHECK(type_of("is_instance_of(n, s) and is_instance_of(v, s) and is_instance_of(n, g)", "var n: Node = Node.new()\nvar v: Variant = n\nvar s: Script = GDScript.new()\nvar g := GDScript.new()") == "bool");
	CHECK(has_error(in_function("var n: Node = Node.new()\nvar s: Script = GDScript.new()\nif is_instance_of(n, s):\n\tvar m: Node3D = n"), "Cannot assign a value of type Node as Node3D"));
	CHECK(has_error(in_function("var s: Script? = null\nvar b := is_instance_of(self, s)"), "The value may be None (Script?); unwrap it first."));
	CHECK(has_error(in_function("var t: Variant = 3\nvar b := is_instance_of(self, t)"), "is_instance_of() takes a class name, a TYPE_* constant or a Script, not Variant; test it with is Script first."));
	CHECK(has_error(in_function("var t := 3\nvar b := is_instance_of(self, t)"), "is_instance_of() takes a class name, a TYPE_* constant or a Script, not int."));
	CHECK(has_error(in_function("var b := is_instance_of(3, GDScript.new())"), "is_instance_of() tests objects against a Script, not int."));
}

// The countdown of the documentation of _iter_init (doc/classes/Object.xml:217-240; ed1daf0).
static const char *const COUNTDOWN = "extends RefCounted\n\nvar from := 3\n\n"
									 "func _iter_init(iter):\n\titer[0] = from\n\treturn from > 0\n\n"
									 "func _iter_next(iter):\n\tvar n := int(iter[0]) - 1\n\titer[0] = n\n\treturn n > 0\n\n"
									 "func _iter_get(iter) -> int:\n\treturn int(iter)\n\n";

// Spec §5.7.
TEST_CASE("for over an object whose class defines _iter_init, _iter_next and _iter_get") {
	FakeProject project;
	project.add_script("res://countdown.pet", COUNTDOWN, "Countdown");
	const auto errors = [&](const std::string &p_source) { return messages(compile_in(p_source, project)); };
	CHECK(errors(in_function("for n in Countdown.new():\n\tvar m: int = n")) == "");
	CHECK(contains(errors(in_function("for n in Countdown.new():\n\tvar s: String = n")), "Cannot assign a value of type int as String."));
	CHECK(contains(errors("extends Node\n\nfunc _iter_get(iter) -> int:\n\treturn 0\n\nfunc f() -> void:\n\tfor n in self:\n\t\tpass\n"), "whose class lacks _iter_init() and _iter_next()."));
	CHECK(has_error(in_function("for x in Node.new():\n\tpass"), "Cannot iterate over Node, whose class lacks _iter_init(), _iter_next() and _iter_get()."));
	CHECK(has_error(in_function("var n: Node? = None\nfor x in n:\n\tpass"), "The value may be None (Node?); check it before iterating over it."));
	CHECK(has_error("extends RefCounted\n\nfunc _iter_init(iter) -> int:\n\treturn 0\n", "The overridden method returns bool."));

	// Called directly, in the loop's chain (spec §8.1).
	const compiler::CompileResult result = compile_in(in_function("for n in Countdown.new():\n\tprint(n)"), project);
	REQUIRE_MESSAGE(result.ok(), messages(result));
	const std::string code = bytecode::disassemble(result.module, function_named(result.module, "f"));
	INFO(code);
	const auto count = [&](const char *p_op) {
		size_t found = 0;
		for (size_t at = code.find(p_op); at != std::string::npos; at = code.find(p_op, at + 1)) {
			found++;
		}
		return found;
	};
	CHECK(count("CALL_METHOD") == 3);
	CHECK(count("ITER_STATE") == 2);
}

// Spec §8.3.
TEST_CASE("assert takes a bool condition and a String message, and does not narrow") {
	CHECK(joined(errors_of(in_function("var n := randi()\nassert(n >= 0)\nassert(n >= 0, \"negative: \" + str(n))\nassert(n >= 0, \"negative\",)\nassert(\n\tn >= 0,\n\t\"negative\"\n)"))) == "");
	CHECK(has_error(in_function("var save_data: Resource? = None\nassert(save_data, \"no save\")"), "A condition must be bool, found Resource?. Compare explicitly, for example x != None."));
	CHECK(has_error(in_function("assert(true, 3)"), "The message of an assert must be a String, found int. Convert it with str(x)."));
	CHECK(has_error(in_function("assert true"), "Expected \"(\", found \"true\"."));
	CHECK(has_error(in_function("var r: Resource? = None\nassert(r != None)\nvar s: Resource = r"), "Cannot assign a value of type Resource? as Resource."));
	CHECK(has_error("signal done(ok: bool)\nasync func f() -> void:\n\tassert(await done)\n", "await cannot appear in an assert: it would vanish with the assertion in release builds."));
	CHECK(has_error("signal done(ok: bool)\nasync func f() -> void:\n\tassert(true, str(await done))\n", "await cannot appear in an assert"));
}

TEST_CASE("assert evaluates its message only when it fails, and only in debug builds") {
	const compiler::CompileResult result = compile(in_function("assert(n > 0, str(n))", "func f(n: int) -> void:"));
	REQUIRE_MESSAGE(result.ok(), messages(result));
	const std::string code = bytecode::disassemble(result.module, function_named(result.module, "f"));
	INFO(code);
	const size_t debug = code.find("DEBUG_BUILD"), test = code.find("GT_INT"), message = code.find("CALL_UTILITY"), failure = code.find("ASSERT_FAIL");
	REQUIRE(failure != std::string::npos);
	CHECK((debug < test && test < message && message < failure));
}

// Spec §5.1.
TEST_CASE("% of a String formats a value of any type, which enters the engine as anywhere else") {
	CHECK(type_of("\"%d\" % 3") == "String");
	CHECK(type_of("&\"%s\" % Vector2()") == "String");
	CHECK(joined(errors_of(in_function("var maybe: int? = None\nvar text := \"%s %s\" % [maybe, Node.new()]\ntext %= maybe\ntext = \"%s\" % maybe\ntext %= [1, \"a\"]\ntext = \"%s\" % \"a\""))) == "");
	CHECK(has_error(in_function("var hits: Array[Vector2?] = []\nvar text := \"%s\" % hits"), "Cannot format a value of type Array[Vector2?]. Its elements exist only in this language; convert it explicitly with Array(x)"));
	const compiler::CompileResult result = compile(in_function("var text := \"%d\" % n\ntext %= n", "func f(n: int) -> void:"));
	REQUIRE_MESSAGE(result.ok(), messages(result));
	const std::string code = bytecode::disassemble(result.module, function_named(result.module, "f"));
	CHECK(code.find("FORMAT") != code.rfind("FORMAT"));
}

TEST_CASE("! is not, and ; separates statements, as in GDScript") {
	CHECK(errors_of(in_function("var a := !true; var b := not a")).empty());
	CHECK(has_error(in_function("var b := !1"), "requires bool"));
}

// Spec §5.5.
TEST_CASE("conditional expressions") {
	CHECK(type_of("-1 if randi() % 2 == 0 else 1") == "int");
	CHECK(type_of("1 if true else 2.5") == "float");
	CHECK(type_of("Node3D.new() if true else Node.new()") == "Node");
	CHECK(type_of("Node3D.new() if true else Timer.new()") == "Node");
	CHECK(contains(type_of("\"a\" if true else 1"), "which have no common type; declare the type."));
	CHECK(has_error(in_function("var x := 1 if 0 else 2"), "A condition must be bool"));
	// None and a T give T?, unless one fits the other.
	CHECK(type_of("None if true else 1") == "int?");
	CHECK(type_of("Node.new() if true else None") == "Node?");
	CHECK(type_of("None if true else maybe", "var maybe: int? = 2") == "int?");
	CHECK(type_of("None if true else v", "var v: Variant = 2") == "Variant");
	CHECK(contains(type_of("None if true else [1]"), "Array[int]? is not supported yet"));
	// With a type to go to, both branches go to it.
	CHECK(errors_of(in_function("var v: Variant = 1 if randf() > 0.5 else \"a\"")).empty());
	const compiler::CompileResult result = compile(in_function("var x := None if randf() > 0.5 else 1"));
	REQUIRE_MESSAGE(result.ok(), messages(result));
	std::string error;
	CHECK_MESSAGE(bytecode::verify(result.module, error), error);
}

TEST_CASE("a call that fails still reports the errors of its arguments") {
	const std::string undeclared = "Identifier \"nope\" is not declared in the current scope.";
	for (const std::string call : { "Node.new(nope)", "Node.instantiate(nope)", "Vector3().missing(nope)", "_process(nope)", "missing(nope)", "Ok(nope)", "Task.any(nope, 1)" }) {
		const std::string reported = joined(errors_of(in_function("var x = " + call)));
		INFO(call);
		INFO(reported);
		CHECK(contains(reported, undeclared));
		CHECK(reported.find(undeclared) == reported.rfind(undeclared)); // Once.
		CHECK(reported.find('\n') + 1 < reported.size()); // A second error, the call's.
	}
}

// Spec §4.6.
TEST_CASE("default values of parameters") {
	const std::string source = "extends Node\n\nfunc add(a: int, b: int = 2, c: float = 0.5) -> float:\n\treturn a + b + c\n\nfunc f() -> void:\n\t";
	CHECK(errors_of(source + "var x := add(1)\n\tvar y := add(1, 2, 3)\n").empty());
	CHECK(has_error(source + "var x := add()\n", "takes 1 to 3 argument(s), 0 given"));
	CHECK(has_error("extends Node\n\nfunc g(a: int = 1, b: int):\n\tpass\n", "needs one too"));
	CHECK(has_error("extends Node\n\nfunc g(a: int = \"x\"):\n\tpass\n", "Cannot use a value of type String as int"));
	// Each sees self and the parameters before it, as in GDScript.
	CHECK(errors_of("extends Node\n\nvar hp := 1\n\nfunc g(a: int, b: int = a + hp, c: int = randi() + b) -> void:\n\tpass\n").empty());
	CHECK(has_error("extends Node\n\nfunc g(a: int = b, b: int = 1) -> void:\n\tpass\n", "Identifier \"b\" is not declared"));
	CHECK(has_error("extends Node\n\nvar hp := 1\n\nstatic func g(a: int = hp) -> void:\n\tpass\n", "\"hp\" belongs to an instance, which the static function \"g()\" does not have."));
}

// Spec §4.8.
TEST_CASE("static functions have no instance") {
	const std::string head = "extends Node\nvar hp := 1\n\nfunc instance_only():\n\tpass\n\nstatic func twice(x: int) -> int:\n\treturn x * 2\n\n";
	CHECK(errors_of(head + "func f() -> void:\n\tvar y := twice(3)\n").empty());
	CHECK(errors_of(head + "static func g() -> int:\n\treturn twice(2)\n").empty());
	CHECK(has_error(head + "static func g() -> int:\n\treturn hp\n", "\"hp\" belongs to an instance, which the static function \"g()\" does not have."));
	CHECK(has_error(head + "static func g():\n\tinstance_only()\n", "\"instance_only()\" belongs to an instance"));
	CHECK(has_error(head + "static func g():\n\tqueue_free()\n", "\"queue_free()\" belongs to an instance"));
	CHECK(has_error("extends Node\n\nstatic func _ready():\n\tpass\n", "cannot be static"));
}

TEST_CASE("engine calls") {
	CHECK(errors_of(in_function("set_process(false)\nvar n: int = get_child_count(true)")).empty());
	CHECK(has_error(in_function("set_process(1)"), "Cannot pass a value of type int as bool"));
	CHECK(has_error(in_function("set_owner(1)"), "Cannot pass a value of type int as Node?"));
	CHECK(has_error(in_function("_ready()"), "virtual engine method"));
}

TEST_CASE("objects the engine returns are nullable until audited (spec §10.2)") {
	CHECK(errors_of(in_function("var p = get_parent()\nset_owner(p)")).empty());
	CHECK(has_error(in_function("var p = get_parent()\nprint(p.name)"), "may be None (Node?)"));
	CHECK(has_error(in_function("var p = get_parent()\np.queue_free()"), "may be None (Node?)"));
}

TEST_CASE("the smallest int can be written in decimal") {
	CHECK(errors_of(in_function("var x := -9223372036854775808")).empty());
	CHECK(has_error(in_function("var x := 9223372036854775808"), "too large for int"));
}

// Spec §2.1.
TEST_CASE("the class header comes before the members") {
	CHECK(has_error("class_name A\nclass_name B\nextends Node\n", "already has a name"));
	CHECK(has_error("extends Node\nvar a := 1\nclass_name A\n", "class header"));
	CHECK(has_error("extends Node\n\nfunc f():\n\tpass\n\nextends Node2D\n", "class header"));
}

// Spec §6.2: Node and Resource scripts take parameters through new(...) too.
TEST_CASE("_init") {
	CHECK(errors_of("extends Node\nvar ready_count := 0\n\nfunc _init():\n\tready_count = 1\n").empty());
	CHECK(errors_of("extends Node\nvar target: Node\n\nfunc _init(a: Node, b: int = 1):\n\ttarget = a\n").empty());
	CHECK(errors_of("extends Resource\nvar size := 0\n\nfunc _init(a: int = 4):\n\tsize = a\n").empty());
	CHECK(has_error("extends Node\n\nfunc _init() -> int:\n\treturn 1\n", "returns nothing"));
}

TEST_CASE("syntax errors are reported at their position") {
	const auto errors = errors_of("extends Node\n\nfunc f() -> void:\n\tvar x := 1 +\n\tpass\n");
	REQUIRE(errors.size() >= 1);
	CHECK(errors[0].rfind("4:", 0) == 0);
}

TEST_CASE("member initializers run in declaration order (spec §6.1)") {
	CHECK(errors_of("extends Node\nvar a := 1\nvar b := a + 1\n").empty());
	CHECK(errors_of("extends Node\nvar a: float = -1\n").empty());
	CHECK(has_error("extends Node\nvar b := a\nvar a := 1\n", "\"a\" is not initialized yet"));
	CHECK(has_error("extends Node\nvar a: int = a\n", "\"a\" is not initialized yet"));
	CHECK(errors_of("extends Node\n\nvar rng = RandomNumberGenerator.new()\nvar r := rng.randf_range(0, 1)\n").empty());
}
