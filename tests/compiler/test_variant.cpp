#include "helpers.h"

using namespace petrichor;
using namespace petrichor::tests;
using bytecode::Op;

// Variant (spec §3.13, §3.14): static values go in implicitly, and come out only
// through as, T(v) and is; == and != compare by the engine's table.

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

const char *const HOLDS_INT = "var v: Variant = 1";

} // namespace

TEST_CASE("a Variant is used only after a conversion (spec §3.14)") {
	CHECK(has_error(in_function(std::string(HOLDS_INT) + "\nvar n = v.hp"), "Cannot read \"hp\" of a Variant. Convert it first with T(v), or narrow it with `if v is T:`."));
	CHECK(has_error(in_function(std::string(HOLDS_INT) + "\nvar n = v + 1"), "Cannot use \"+\" on a Variant."));
	CHECK(has_error(in_function(std::string(HOLDS_INT) + "\nvar n = v[0]"), "Cannot index a Variant."));
	CHECK(has_error(in_function(std::string(HOLDS_INT) + "\nfor x in v:\n\tpass"), "Cannot iterate over a Variant."));
	CHECK(has_error(in_function(std::string(HOLDS_INT) + "\nv.queue_free()"), "Cannot call \"queue_free()\" on a Variant."));
	CHECK(has_error(in_function(std::string(HOLDS_INT) + "\nvar n = -v"), "Cannot use \"-\" on a Variant."));
	CHECK(has_error(in_function(std::string(HOLDS_INT) + "\nvar b = v < 2"), "Cannot use \"<\" on a Variant."));
	CHECK(has_error(in_function(std::string(HOLDS_INT) + "\nvar b = 1 in v"), "Cannot look for a value in a Variant."));
	CHECK(has_error(in_function(std::string(HOLDS_INT) + "\nif v:\n\tpass"), "A condition must be bool, found Variant. Compare explicitly, for example x != None."));
	CHECK(has_error(in_function(std::string(HOLDS_INT) + "\nvar n: int = v"), "Cannot assign a value of type Variant as int. Convert it first with int(v), or narrow it with `if v is int:`."));
	CHECK(has_error(in_function(std::string(HOLDS_INT) + "\nvar n: Node = v"), "Convert it first with v as Node, or narrow it with `if v is Node:`."));
	CHECK(has_error(in_function(std::string(HOLDS_INT) + "\nvar a: Array[int] = v"), "Narrow it first with `if v is Array[int]:`."));
}

TEST_CASE("static values enter a Variant implicitly, and leave by as, T(v) and is") {
	CHECK(errors_of(in_function("var v: Variant = 1\nv = \"a\"\nv = Vector2()\nv = self\nv = None\nvar w = v\nw = [1, 2]")).empty());
	CHECK(type_of("v as Node", HOLDS_INT) == "Node?");
	CHECK(type_of("int(v)", HOLDS_INT) == "int");
	CHECK(type_of("float(v)", HOLDS_INT) == "float");
	CHECK(type_of("bool(v)", HOLDS_INT) == "bool");
	CHECK(type_of("String(v)", HOLDS_INT) == "String");
	CHECK(type_of("Vector2(v)", HOLDS_INT) == "Vector2");
	CHECK(type_of("Array(v)", HOLDS_INT) == "Array");
	CHECK(type_of("v == 1 and v != \"a\" and v == None", HOLDS_INT) == "bool");
	CHECK(has_error(in_function(std::string(HOLDS_INT) + "\nvar n = v as int"), "as only converts objects"));
	CHECK(has_error("extends Node\n\nstruct Hit:\n\tvar damage: int\n\nfunc g(v: Variant) -> bool:\n\treturn v is Hit\n", "is tests classes and built-in types, not"));
	CHECK(has_error(in_function(std::string(HOLDS_INT) + "\nvar b = v is Array[Array[int]]"), "is tests classes and built-in types"));
}

TEST_CASE("is narrows a Variant to the type it tests (spec §5.4)") {
	CHECK(errors_of(in_function("var v: Variant = 1\nif v is int:\n\tvar n: int = v + 1\n\tv += 1\nif v is Node:\n\tv.queue_free()\nif v is Dictionary:\n\tvar hp = v.get(\"hp\", 0)\nif not (v is String):\n\treturn\nvar s: String = v")).empty());
	CHECK(errors_of(in_function("var v: Variant? = None\nif v != None and v is Vector2:\n\tvar x: float = v.x")).empty());
	CHECK(has_error(in_function("var v: Variant = 1\nif v is int:\n\tv = \"a\"\n\tvar n: int = v"), "Cannot assign a value of type Variant as int."));
	const compiler::CompileResult result = compile(in_function("var v: Variant = 1\nif v is int:\n\tprint(v + 1)\nvar n = v as Node\nprint(int(v) == 2)"));
	require_verified(result);
	const bytecode::Function &f = function_named(result.module, "f");
	CHECK(uses(f, Op::IS_TYPE));
	CHECK(uses(f, Op::FROM_VARIANT));
	CHECK(uses(f, Op::VARIANT_OBJECT));
	CHECK(uses(f, Op::CONSTRUCT_FROM));
}

TEST_CASE("== with a Variant compares by the engine, a match pattern by type first (spec §5.2, §5.6)") {
	const compiler::CompileResult result = compile(in_function("var v: Variant = 1\nprint(v == 1)\nmatch v:\n\t1:\n\t\tprint(1)\n\t\"a\", &\"b\":\n\t\tprint(2)"));
	require_verified(result);
	const bytecode::Function &f = function_named(result.module, "f");
	CHECK(uses(f, Op::VARIANT_EQUAL));
	CHECK(uses(f, Op::MATCHES));
}

TEST_CASE("the elements of untyped containers are Variant (spec §3.8)") {
	CHECK(type_of("u[0]", "var u: Array = [1]") == "Variant");
	CHECK(type_of("u.pop_back()", "var u: Array = [1]") == "Variant?");
	CHECK(type_of("u.front()", "var u: Array = [1]") == "Variant");
	CHECK(type_of("d.get(\"a\")", "var d: Dictionary = {}") == "Variant?");
	CHECK(type_of("d.get(\"a\", 0)", "var d: Dictionary = {}") == "Variant");
	// Its None would be null in a Variant (spec §3.14).
	CHECK(has_error(in_function("var d: Dictionary = {}\nprint(d.get(\"a\"))"), "Cannot pass a value of type Variant? as Variant. A Variant cannot tell its None from null; pass x.unwrap_or(null)"));
	CHECK(errors_of(in_function("var u: Array = [1, \"a\"]\nu.append(Node.new())\nvar d: Dictionary = {1: \"a\"}\nd[Vector2()] = []\nfor k in d:\n\tprint(k)\nd[1] = {}\nvar w: Variant = [1, \"a\"]")).empty());
	const compiler::CompileResult result = compile(in_function("var u: Array = [1]\nvar x = u.pop_back()\nu.append(2)\nprint(3 in u)"));
	require_verified(result);
	bool checked = false;
	for (const bytecode::BuiltinMethodCall &call : result.module.builtin_methods) {
		checked = checked || (call.method == "append" && call.checks_elements);
	}
	CHECK(checked);
}

TEST_CASE("a typed container goes where an untyped one is expected, the reverse explicitly (spec §3.14)") {
	CHECK(errors_of(in_function("var ids: Array[int] = [1]\nvar any: Array = ids\nvar names: Dictionary[String, int] = {}\nvar all: Dictionary = names\nvar v: Variant = ids")).empty());
	CHECK(errors_of(in_function("var ids: Array[int] = [1]\nids.assign(a)\nids.append_array(a)\nif a is Array[int]:\n\tids = a", "func f(a: Array) -> void:")).empty());
	CHECK(has_error(in_function("var ids: Array[int] = a", "func f(a: Array) -> void:"), "Cannot assign a value of type Array as Array[int]. Its elements may be of any type; narrow it with `if x is Array[int]:`, or copy them in with assign()."));
	CHECK(has_error(in_function("var ids: Array[int] = []\nids.assign(f)", "func g(f: Array[float]) -> void:"), "Cannot pass a value of type Array[float] as Array[int]."));
	CHECK(has_error(in_function("var d: Dictionary[String, int] = {}\nd.merge({1: 2})"), "The keys of Dictionary[String, int] are String, not int."));
	CHECK(has_error(in_function("if a is Array[int]:\n\ta.append(\"x\")", "func f(a: Array) -> void:"), "Cannot pass a value of type String as int"));
	CHECK(has_error(in_function("var hits: Array[Hit] = []\nvar any: Array = hits", "func f() -> void:") + "\n\nstruct Hit:\n\tvar damage: int\n", "Hit] as Array."));
}

TEST_CASE("members, parameters and results may be Variant") {
	CHECK(errors_of("extends Control\n@export pub var any: Variant\nvar kept: Variant\nsignal changed(value: Variant)\n\nfunc pick(v: Variant) -> Variant:\n\tchanged.emit(v)\n\treturn v\n\nfunc _can_drop_data(at: Vector2, data: Variant) -> bool:\n\treturn data is Dictionary\n").empty());
	CHECK(has_error("extends Node\n\nfunc _get(property: StringName) -> Variant:\n\treturn null\n", "Overriding _get() is not supported yet."));
	CHECK(has_error("extends Node\n\nfunc _set(property: StringName, value: Variant) -> bool:\n\treturn false\n", "Overriding _set() is not supported yet."));
	CHECK(has_error(in_function("var n: Nil"), "Unknown type \"Nil\"."));
}
