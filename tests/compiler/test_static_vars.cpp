#include "helpers.h"

using namespace petrichor;
using namespace petrichor::tests;
using bytecode::Op;

// Static variables (spec §4.8): of the class, given their first values once
// when it loads, in declaration order, then _static_init().

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

FakeProject registry_project() {
	FakeProject project;
	project.add_script("res://registry.pet", R"(class_name Registry extends RefCounted
pub static var count := 0
static var hidden := 1
pub static var label := "none":
	set(value):
		label = value.to_upper()
pub static func bump() -> void:
	count += 1
)",
			"Registry");
	project.add_script("res://a.pet", "class_name A extends RefCounted\npub static var x: int = B.y + 1\n", "A");
	project.add_script("res://b.pet", "class_name B extends RefCounted\npub static var y: int = A.x + 1\n", "B");
	return project;
}

std::vector<std::string> errors_in(const std::string &p_source) {
	std::vector<std::string> result;
	for (const compiler::Diagnostic &d : compile_in(p_source, registry_project()).errors) {
		result.push_back(std::to_string(d.line) + ":" + std::to_string(d.column) + ": " + d.message);
	}
	return result;
}

bool has(const std::vector<std::string> &p_errors, const std::string &p_text) {
	return std::any_of(p_errors.begin(), p_errors.end(), [&](const std::string &p_error) { return contains(p_error, p_text); });
}

} // namespace

TEST_CASE("static variables get their first values in declaration order, then _static_init() runs (spec §4.8)") {
	CHECK(has_error(R"(extends Node
static var instance: Node
)",
			"needs an initial value: Node has no default value. Give it one, or the type Node? (spec §4.8)."));
	const auto valid = compile(R"(extends Node
static var names: Array[String]
static var total := 3
static var instance: Node?
static var doubled := total * 2
static func _static_init() -> void:
	total += 1
)");
	require_verified(valid);
	const bytecode::Module &module = valid.module;
	REQUIRE(module.statics.size() == 4);
	CHECK(module.statics[0].name == "names");
	CHECK(module.statics[0].has_initial);
	CHECK_FALSE(module.statics[1].has_initial);
	CHECK(module.statics[2].has_initial);
	REQUIRE(module.static_init_function >= 0);
	const bytecode::Function &initializer = module.functions[module.static_init_function];
	CHECK(initializer.is_static);
	// The typed Array first, then the initializers, then _static_init().
	std::vector<uint32_t> order;
	size_t call = 0;
	for (size_t pc = 0; pc < initializer.code.size(); pc += bytecode::instruction_length(initializer.code.data() + pc)) {
		if (Op(initializer.code[pc]) == Op::INIT_STATIC) {
			order.push_back(initializer.code[pc + 2]);
		} else if (Op(initializer.code[pc]) == Op::CALL) {
			call = pc;
		}
	}
	CHECK(order == std::vector<uint32_t>{ 0, 1, 3 });
	CHECK(call > 0);
	CHECK(uses(function_named(module, "_static_init"), Op::STORE_STATIC));
}

TEST_CASE("a static initializer reads only static variables declared before it, and no instance (spec §4.8)") {
	CHECK(has_error("extends Node\nstatic var a := b\nstatic var b := 1\n", "\"b\" is not initialized yet: static variables are initialized in declaration order (spec §4.8)."));
	CHECK(has_error("extends Node\nstatic var a := a\n", "\"a\" is not initialized yet"));
	CHECK(has_error("extends Node\nvar hp := 3\nstatic var a := hp\n", "\"hp\" belongs to an instance, which a static variable's initializer does not have."));
	CHECK(has_error("extends Node\nstatic var a := self\n", "\"self\" belongs to an instance, which a static variable's initializer does not have."));
	CHECK(has_error("extends Node\nstatic var a := func(): return name\n", "\"name\" belongs to an instance, which a static variable's initializer does not have."));
	CHECK(has_error("extends Node\nconst C := count\nstatic var count := 1\n", "\"count\" is a variable, not a constant (spec §4.5)."));
	// A function the initializer calls runs then, and is checked when it runs.
	require_verified(compile("extends Node\nstatic var a := read_b()\nstatic var b := 1\nstatic func read_b() -> int:\n\treturn b\n"));
	// A lambda runs later; the type of a later variable is known when written.
	require_verified(compile("extends Node\nstatic var get_b := func() -> int: return b\nstatic var b: int = 1\n"));
	CHECK(has_error("extends Node\nstatic var get_b := func() -> int: return b\nstatic var b := 1\n", "declare it, as in `static var b: T`."));
}

TEST_CASE("static variables are declared as GDScript's, with accessors and without exports") {
	CHECK(has_error("extends Node\n@export static var a := 1\n", "@export cannot be applied to a static variable (spec §4.8)."));
	CHECK(has_error("extends Node\n@onready static var a := 1\n", "@onready cannot be applied to a static variable (spec §4.8)."));
	CHECK(has_error("extends Node\nvar a := 1\nstatic var a := 2\n", "The member \"a\" is already declared."));
	CHECK(has_error("extends Node\nstatic var a := 2\nfunc a() -> void:\n\tpass\n", "\"a\" is already declared."));
	CHECK(has_error("extends Node\nstatic var a\n", "\"a\" needs a type or an initial value."));
	CHECK(has_error("extends Node\nstatic const A := 1\n", "Only variables and functions can be static."));
	CHECK(has_error("extends Node\nstatic var a := 1:\n\tset = put\nfunc put(value: int) -> void:\n\tpass\n", "A setter of a static variable is static and not async (spec §4.4, §4.8)."));
	CHECK(has_error("extends Node\nfunc _static_init() -> void:\n\tpass\n", "_static_init() is static, takes no parameters, returns nothing and cannot await (spec §4.8)."));
	CHECK(has_error("extends Node\nstatic func _static_init(x: int) -> void:\n\tpass\n", "_static_init() is static"));
	const auto result = compile(R"(extends Node
static var label := "a":
	set(value):
		label = value.to_upper()
	get:
		return label + "!"
static func rename() -> String:
	label = "b"
	return label
)");
	require_verified(result);
	const bytecode::Function &rename = function_named(result.module, "rename");
	CHECK(uses(rename, Op::CALL)); // Through the accessors.
	CHECK_FALSE(uses(rename, Op::STORE_STATIC));
	CHECK(uses(function_named(result.module, "@set_label"), Op::STORE_STATIC)); // Its own field.
}

TEST_CASE("a static variable is named in its class, subclasses and inner classes, through its class and through objects (spec §4.8)") {
	require_verified(compile_in(R"(extends Registry
static var mine := count + 1
class Inner:
	static var copy := mine
	func f() -> int:
		mine += copy
		return mine
func f(r: Registry) -> void:
	count = 2
	Registry.count += 1
	r.count = Registry.count
	Registry.label = "x"
	print(Inner.copy, hidden)
)",
			registry_project()));
	const auto result = compile_in("extends Node\nfunc f(r: Registry) -> int:\n\tRegistry.label = \"a\"\n\treturn r.count + Registry.count\n", registry_project());
	require_verified(result);
	const bytecode::Function &f = function_named(result.module, "f");
	CHECK(uses(f, Op::LOAD_STATIC));
	CHECK(uses(f, Op::CALL_METHOD)); // Registry's setter of label.
	CHECK(has(errors_in("extends Node\nfunc f() -> void:\n\tprint(Registry.hidden)\n"), "The static variable \"hidden\" of Registry is not pub."));
	CHECK(has(errors_in("extends Node\nfunc f() -> void:\n\tprint(Registry.nothing)\n"), "Registry has no constant, static variable or static function \"nothing\"."));
}

TEST_CASE("a cycle between the static initializations of classes is found when they run, and both compile (spec §4.8)") {
	// Without bodies, an initializer of a declared type is not checked, so
	// compiling either class does not need the other's while it is compiled.
	const FakeProject project = registry_project();
	for (const char *path : { "res://a.pet", "res://b.pet" }) {
		compiler::CompileOptions options;
		options.path = path;
		options.project = &project;
		const auto result = compiler::compile(*project.script_source(path), fake_engine(), options);
		require_verified(result);
	}
}
