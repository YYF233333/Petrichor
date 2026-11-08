#include "helpers.h"

using namespace petrichor;
using namespace petrichor::tests;
using bytecode::Op;

// Calls through a type name (spec §4.6, §4.8): static methods of engine
// classes, of built-in types and of classes of this language, and new().

namespace {

void require_verified(const compiler::CompileResult &p_result) {
	REQUIRE_MESSAGE(p_result.ok(), messages(p_result));
	std::string error;
	CHECK_MESSAGE(bytecode::verify(p_result.module, error), error);
}

FakeProject spawner_project() {
	FakeProject project;
	project.add_script("res://spawner.pet", R"(class_name Spawner extends Node
pub static func make(count: int) -> int:
	return count * 2
static func hidden() -> int:
	return 1
pub func tick() -> void:
	pass
)",
			"Spawner");
	project.add_script("res://counter.pet", "class_name Counter extends RefCounted\npub var count := 0\n", "Counter");
	project.add_script("res://plain.pet", "class_name Plain extends Object\n", "Plain");
	return project;
}

} // namespace

TEST_CASE("a static engine method is called through its class") {
	const compiler::CompileResult result = compile(in_function("var file: FileAccess? = FileAccess.open(\"res://a.txt\", FileAccess.READ)\nif file != None:\n\tprint(file.get_as_text())"));
	require_verified(result);
	const auto opened = std::find_if(result.module.engine_calls.begin(), result.module.engine_calls.end(), [](const bytecode::EngineCall &p_call) { return p_call.method == "open"; });
	REQUIRE(opened != result.module.engine_calls.end());
	CHECK(opened->static_class == "FileAccess");
	CHECK(has_error(in_function("print(FileAccess.get_as_text())"), "\"get_as_text()\" of FileAccess is not static; call it on an object."));
	CHECK(has_error(in_function("FileAccess.nope()"), "FileAccess has no method \"nope()\"."));
}

TEST_CASE("a static engine method called on an object is its class's, after the object (spec §4.8)") {
	const compiler::CompileResult result = compile(in_function("var again: FileAccess? = opened().open(\"res://b.txt\", FileAccess.READ)") + "\nfunc opened() -> FileAccess:\n\treturn FileAccess.open(\"res://a.txt\", FileAccess.WRITE).unwrap()\n");
	require_verified(result);
	const bytecode::Function &f = function_named(result.module, "f");
	const size_t engine = find_instruction(f, Op::CALL_ENGINE);
	CHECK(find_instruction(f, Op::CALL) < engine);
	CHECK(result.module.engine_calls[f.code[engine + 1]].static_class == "FileAccess");
}

TEST_CASE("a static method of a built-in type is called through the type") {
	const compiler::CompileResult result = compile(in_function("var tint := Color.from_hsv(0.5, 1.0, 1.0)\nprint(tint)"));
	require_verified(result);
	const auto from_hsv = std::find_if(result.module.builtin_methods.begin(), result.module.builtin_methods.end(), [](const bytecode::BuiltinMethodCall &p_call) { return p_call.method == "from_hsv"; });
	REQUIRE(from_hsv != result.module.builtin_methods.end());
	CHECK(from_hsv->is_static);
	CHECK(from_hsv->base == bytecode::ValueType::Color);
	CHECK(type_of("Color.from_hsv(0.5, 1.0, 1.0)") == "Color");
	CHECK(has_error(in_function("print(Vector2.length())"), "\"length()\" of Vector2 is not static; call it on a value."));
	// On a value, the type's own (spec §4.8).
	CHECK(type_of("c.from_hsv(0.5, 1.0, 1.0)", "var c := Color.RED") == "Color");
	require_verified(compile(in_function("var c := Color.RED\nprint(c.from_hsv(0.5, 1.0, 1.0))")));
}

TEST_CASE("a static method of a class of this language is called on the class") {
	const FakeProject project = spawner_project();
	const compiler::CompileResult result = compile_in("extends Node\nstatic func twice() -> int:\n\treturn Spawner.make(1)\nfunc f() -> int:\n\treturn Spawner.make(3) + twice()\n", project);
	require_verified(result);
	REQUIRE(result.module.method_calls.size() == 1);
	const bytecode::MethodCall &make = result.module.method_calls[0];
	CHECK(make.method == "make");
	REQUIRE(make.static_class >= 0);
	CHECK(result.module.classes[make.static_class].script == "res://spawner.pet");
	CHECK(contains(messages(compile_in("extends Node\nfunc f() -> int:\n\treturn Spawner.hidden()\n", project)), "\"hidden()\" of Spawner is not pub."));
	CHECK(compile_in("extends Spawner\nfunc f() -> int:\n\treturn Spawner.hidden()\n", project).ok());
	CHECK(contains(messages(compile_in("extends Node\nfunc f() -> void:\n\tSpawner.tick()\n", project)), "\"tick()\" of Spawner is not static; call it on an object."));
	CHECK(contains(messages(compile_in("extends Node\nfunc f() -> void:\n\tSpawner.nope()\n", project)), "Spawner has no method \"nope()\"."));
	CHECK(contains(messages(compile_in("extends Node\nfunc f() -> void:\n\tSpawner.make(\"x\")\n", project)), "Cannot pass a value of type String as int"));
	// On an object, found by name from its class up, as an instance method is (spec §4.8).
	const compiler::CompileResult on_object = compile_in("extends Node\nfunc f(s: Spawner) -> int:\n\treturn s.make(2)\n", project);
	require_verified(on_object);
	REQUIRE(on_object.module.method_calls.size() == 1);
	CHECK(on_object.module.method_calls[0].static_class < 0);
}

TEST_CASE("a member inherited without pub is seen as where it is declared, whichever class names it") {
	FakeProject project;
	project.add_script("res://base.pet", "class_name Base extends RefCounted\nconst SECRET := 1\nenum { HIDDEN }\nenum Mode { A }\nstatic func secret() -> int:\n\treturn SECRET\n", "Base");
	project.add_script("res://heir.pet", "class_name Heir extends Base\n", "Heir");
	const auto through = [](const std::string &p_class) {
		return "func f() -> void:\n\tprint(" + p_class + ".secret(), " + p_class + ".SECRET, " + p_class + ".HIDDEN, " + p_class + ".Mode.A)\n";
	};
	// A subclass of the declaring class, through another subclass (spec §4.2).
	require_verified(compile_in("extends Base\n" + through("Heir"), project));
	// Not a subclass, through a subclass of its own file.
	const std::string leak = messages(compile_in("class_name Other extends RefCounted\nclass Sub extends Base:\n\tpass\n" + through("Sub"), project));
	CHECK(contains(leak, "\"secret()\" of Other.Sub is not pub."));
	CHECK(contains(leak, "The constant \"SECRET\" of Other.Sub is not pub."));
	CHECK(contains(leak, "The constant \"HIDDEN\" of Other.Sub is not pub."));
	CHECK(contains(leak, "The enum Other.Sub.Mode is not pub."));
}

TEST_CASE("a static function is a value, of this language or of the engine (spec §3.11)") {
	const FakeProject project = spawner_project();
	const compiler::CompileResult result = compile_in("extends Node\nstatic func twice(n: int) -> int:\n\treturn n * 2\nstatic func f(s: Spawner) -> int:\n\tvar own := twice\n\tvar other := Spawner.make\n\tvar on_object := s.make\n\tvar open := FileAccess.open\n\treturn own.call(1) + other.call(2) + on_object.call(3)\nclass Inner:\n\tfunc g() -> int:\n\t\tvar around := twice\n\t\treturn around.call(4)\n", project);
	require_verified(result);
	find_instruction(function_named(result.module, "f"), Op::STATIC_VALUE);
	CHECK(std::any_of(result.module.classes.begin(), result.module.classes.end(), [](const bytecode::ClassRef &p_class) { return p_class.script == "res://spawner.pet"; }));
	CHECK(type_of("FileAccess.open") == "func(String, FileAccess.ModeFlags) -> FileAccess?");
	const auto errors = [&](const std::string &p_body) { return messages(compile_in("extends Node\nfunc f() -> void:\n\t" + p_body + "\n", project)); };
	CHECK(contains(errors("var x := Spawner.hidden"), "\"hidden()\" of Spawner is not pub."));
	CHECK(contains(errors("var x := Spawner.tick"), "\"tick()\" of Spawner is not static; take it from an object."));
	CHECK(contains(errors("var x := FileAccess.get_as_text"), "\"get_as_text()\" of FileAccess is not static; take it from an object."));
	CHECK(contains(errors("var x := Spawner.new"), "new() as a value is not supported yet"));
}

TEST_CASE("a class calls its own static methods through its class_name") {
	const compiler::CompileResult result = compile_in("class_name Grid extends Node\nstatic func cell() -> int:\n\treturn 8\nfunc f() -> int:\n\treturn Grid.cell()\n", FakeProject());
	require_verified(result);
	CHECK(result.module.method_calls.empty());
	find_instruction(function_named(result.module, "f"), Op::CALL);
}

TEST_CASE("new() of a class of this language makes the engine object with the script") {
	const FakeProject project = spawner_project();
	const compiler::CompileResult result = compile_in("extends Node\nfunc f() -> int:\n\tvar s := Spawner.new()\n\tadd_child(s)\n\tvar c: Counter = Counter.new()\n\treturn c.count\n", project);
	require_verified(result);
	const bytecode::Function &f = function_named(result.module, "f");
	const uint32_t made = f.code[find_instruction(f, Op::NEW_OBJECT) + 1];
	CHECK(result.module.classes[made].name == "Node");
	CHECK(result.module.classes[made].script == "res://spawner.pet");
	CHECK(compile_in("class_name Link extends RefCounted\nfunc next() -> Link:\n\treturn Link.new()\n", FakeProject()).ok());
	CHECK(contains(messages(compile_in("extends Node\nfunc f() -> void:\n\tadd_child(Spawner.new(3))\n", project)), "Function \"new()\" takes 0 argument(s), 1 given."));
	CHECK(contains(messages(compile_in("extends Node\nfunc f() -> void:\n\tprint(Plain.new())\n", project)), "Plain cannot be instantiated: the engine cannot instantiate Object, which it extends."));
}

TEST_CASE("no function of a class or an interface is named new, which constructs the class (spec §4.1)") {
	const std::string refused = "A function cannot be named \"new\"";
	CHECK(contains(messages(compile("extends RefCounted\nstatic func new() -> int:\n\treturn 1\n")), refused));
	CHECK(contains(messages(compile("extends RefCounted\nfunc new() -> void:\n\tpass\n")), refused));
	CHECK(contains(messages(compile("extends RefCounted\ninterface Maker:\n\tfunc new()\n")), refused));
}

TEST_CASE("new(...) passes its arguments to the nearest _init(), whose visibility it has (spec §4.2, §6.1)") {
	FakeProject project;
	project.add_script("res://pair.pet", "class_name Pair extends RefCounted\npub var left := 0\npub var right := \"\"\npub func _init(a: int, b: String = \"b\"):\n\tleft = a\n\tright = b\n", "Pair");
	project.add_script("res://triple.pet", "class_name Triple extends Pair\n", "Triple");
	project.add_script("res://pool.pet", "class_name Pool extends RefCounted\nvar size := 0\nfunc _init(n: int):\n\tsize = n\npub static func make() -> Pool:\n\treturn Pool.new(8)\n", "Pool");
	project.add_script("res://quiet.pet", "class_name Quiet extends RefCounted\nfunc _init():\n\tpass\n", "Quiet");
	const compiler::CompileResult result = compile_in("extends Node\nfunc f() -> int:\n\tvar p := Pair.new(order(1), \"x\")\n\treturn p.left + Triple.new(2).left\nfunc order(n: int) -> int:\n\treturn n\n", project);
	require_verified(result);
	const bytecode::Function &f = function_named(result.module, "f");
	// The arguments, the object, then its _init() with them.
	const size_t made = find_instruction(f, Op::NEW_WITHOUT_INIT);
	CHECK(find_instruction(f, Op::CALL) < made);
	const size_t init = find_instruction(f, Op::CALL_METHOD);
	CHECK(made < init);
	CHECK(f.code[init + 3] == f.code[made + 2]);
	const bytecode::MethodCall &call = result.module.method_calls[f.code[init + 1]];
	CHECK(call.method == "_init");
	CHECK(call.params.size() == 2);
	CHECK(call.defaults == 1);

	const auto errors = [&](const std::string &p_source) { return messages(compile_in(p_source, project)); };
	CHECK(contains(errors("extends Node\nfunc f() -> void:\n\tprint(Pair.new())\n"), "Function \"new()\" takes 1 to 2 argument(s), 0 given."));
	CHECK(contains(errors("extends Node\nfunc f() -> void:\n\tprint(Pair.new(\"x\"))\n"), "Cannot pass a value of type String as int"));
	CHECK(contains(errors("extends Node\nfunc f() -> void:\n\tprint(Pool.new(1))\n"), "new() of Pool cannot be called here: its _init() is not pub."));
	CHECK(contains(errors("extends Node\nfunc f() -> void:\n\tprint(Quiet.new())\n"), "new() of Quiet cannot be called here"));
	CHECK(compile_in("extends Node\nfunc f() -> void:\n\tprint(Pool.make())\n", project).ok());
	CHECK(compile_in("extends Pool\nfunc f() -> void:\n\tprint(Pool.new(1))\n", project).ok());
	// Each class's _init() has its own signature and visibility; a base's runs through super.
	const compiler::CompileResult named = compile_in("class_name Named extends Pair\nfunc _init(name: String):\n\tsuper._init(1, name)\nfunc again() -> Named:\n\treturn Named.new(\"n\")\n", project);
	require_verified(named);
	CHECK(contains(errors("extends Pair\nfunc _init():\n\tsuper._init(\"x\")\n"), "Cannot pass a value of type String as int"));
	CHECK(contains(errors("extends Pair\nfunc _init(name):\n\tpass\n"), "Parameter \"name\" needs a type."));
	CHECK(compile_in("class_name Counted extends Object\nfunc _init(n: int):\n\tpass\n", project).ok());
}
