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
	CHECK(contains(messages(compile_in("extends Node\nfunc f() -> void:\n\tadd_child(Spawner.new(3))\n", project)), "new() of Spawner takes no arguments: _init() with parameters is not supported yet."));
	CHECK(contains(messages(compile_in("extends Node\nfunc f() -> void:\n\tprint(Plain.new())\n", project)), "Plain cannot be instantiated: the engine cannot instantiate Object, which it extends."));
}
