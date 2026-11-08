#include "helpers.h"

using namespace petrichor;
using namespace petrichor::tests;
using bytecode::Op;

// Lambdas and function values (spec §3.11, §4.6).

namespace {

void require_verified(const compiler::CompileResult &p_result) {
	REQUIRE_MESSAGE(p_result.ok(), messages(p_result));
	std::string error;
	CHECK_MESSAGE(bytecode::verify(p_result.module, error), error);
}

const bytecode::Function *only_lambda(const bytecode::Module &p_module) {
	const bytecode::Function *found = nullptr;
	for (const bytecode::Function &function : p_module.functions) {
		if (function.is_lambda) {
			REQUIRE_MESSAGE(!found, "more than one lambda");
			found = &function;
		}
	}
	REQUIRE_MESSAGE(found, "no lambda");
	return found;
}

} // namespace

TEST_CASE("a lambda's types: parameters written or from where it goes, the return type from its body (spec §4.6)") {
	CHECK(type_of("f.call(3)", "var f := func(x: int): return x * 2") == "int");
	CHECK(type_of("f", "var f := func(x: int): return x * 2") == "func(int) -> int");
	CHECK(type_of("f", "var f := func(): print(1)") == "func()");
	// Returns of int and float give float, as the branches of a conditional do (§5.5).
	CHECK(type_of("f.call(true)", "var f := func(up: bool):\n\tif up:\n\t\treturn 1\n\treturn 2.5") == "float");
	CHECK(type_of("g.call(1)", "var g: func(int) -> int = func(x): return x + 1") == "int");
	CHECK(type_of("h", "var h: (func(int) -> bool)? = None") == "(func(int) -> bool)?");
	CHECK(type_of("f.call(2)", "var f := func(x: int) -> float: return x") == "float");

	CHECK(has_error(in_function("var f := func(x): return x"), "Parameter \"x\" needs a type: nothing the lambda goes to gives one."));
	CHECK(has_error(in_function("var f := func(up: bool):\n\tif up:\n\t\treturn 1\n\treturn \"a\""), "The lambda returns int and String; declare its return type."));
	CHECK(has_error(in_function("var f := func(up: bool):\n\tif up:\n\t\treturn 1"), "Not all paths of the lambda return a value."));
	CHECK(has_error(in_function("var f := func(up: bool):\n\tif up:\n\t\treturn 1\n\treturn"), "The lambda returns a value on some paths and none on others."));
	CHECK(has_error(in_function("var f := func(x: int = 1): return x"), "Lambda parameters have no default values."));
	CHECK(has_error("extends Node\nconst F = func(): pass\n", "A lambda is not a constant expression."));
}

TEST_CASE("function values are called with call(), checked against their types (spec §3.11)") {
	CHECK(has_error(in_function("var f := func(x: int): return x\nprint(f(1))"), "\"f\" is a function value; call it as f.call(...)."));
	CHECK(has_error(in_function("var f := func(x: int): return x\nprint(f.call(\"a\"))"), "Cannot pass a value of type String as int."));
	CHECK(has_error(in_function("var f := func(x: int): return x\nprint(f.call())"), "takes 1 argument(s), 0 given."));
	CHECK(type_of("f.callv([1])", "var f := func(x: int): return x") == "int");
	CHECK(type_of("f.bind(1).call()", "var f := func(x: int): return x") == "int");
	CHECK(has_error(in_function("var f: func(int) = func(x: String): pass"), "Cannot assign a value of type func(String) as func(int)."));
	CHECK(has_error("extends Node\nvar f: func(int)? = null\n", "A function type that may be None is written in parentheses, as (func(int))?."));
	const compiler::CompileResult result = compile(in_function("var f := func(x: int) -> int: return x * 2\nprint(f.call(3))\nprint(f.callv([3]))"));
	require_verified(result);
	// Not a call of the engine's Callable.call, so that a program error in it aborts the caller (§8.1).
	const bytecode::Function &caller = function_named(result.module, "f");
	const size_t pc = find_instruction(caller, Op::CALL_VALUE);
	CHECK(result.module.method_calls[caller.code[pc + 1]].method == "call");
	CHECK(result.module.method_calls[caller.code[pc + 1]].params.size() == 1);
}

TEST_CASE("a lambda captures by value: assigning a capture is a compile error, a container it shares is not (spec §3.11)") {
	CHECK(has_error(in_function("var hits := 0\nchild_entered_tree.connect(func(node): hits += 1)"), "Cannot assign to \"hits\", which the lambda captures as a copy; keep what it shares in a member or a container."));
	CHECK(has_error(in_function("var v := Vector2()\nvar f := func(): v.x = 1"), "Cannot assign to \"v\", which the lambda captures as a copy"));
	CHECK(has_error(in_function("const K = 3\nvar f := func(): K = 4"), "Cannot assign to the constant \"K\"."));
	require_verified(compile(in_function("var counter: Array[int] = [0]\nchild_entered_tree.connect(func(node): counter[0] += 1)")));
	// Its own locals, and those of a lambda in it, it may assign.
	require_verified(compile(in_function("var f := func():\n\tvar n := 1\n\tn += 1\n\tvar g := func(): return n\n\treturn g.call()")));
}

TEST_CASE("what is narrowed where a lambda is made stays narrowed in it (spec §5.4)") {
	require_verified(compile(in_function("var parent: Node? = get_parent()\nif parent != None:\n\tvar f := func(): print(parent.name)\n\tf.call()")));
	CHECK(has_error(in_function("var parent: Node? = get_parent()\nvar f := func(): print(parent.name)"), "may be None"));
}

TEST_CASE("a lambda's parameters take what the signal it is connected to passes (spec §4.7)") {
	require_verified(compile(in_function("child_entered_tree.connect(func(node):\n\tprint(node.name)\n)")));
	CHECK(has_error(in_function("child_entered_tree.connect(func(node: int): pass)"), "passes Node as argument 1"));
	CHECK(has_error(in_function("child_entered_tree.connect(func(): pass)"), "passes 1 argument(s)"));
}

TEST_CASE("the names of lambdas: locals of the function around it are taken, parameters may reuse them, and a lambda's name is not in its body (spec §3.11, §4.6)") {
	CHECK(has_error(in_function("var n := 1\nvar f := func():\n\tvar n := 2\n\treturn n"), "\"n\" is already declared in the function around this lambda."));
	require_verified(compile(in_function("var node := get_parent()\nchild_entered_tree.connect(func(node): print(node.name))")));
	CHECK(has_error(in_function("var f := func fact(n: int) -> int: return n * fact.call(n - 1)"), "Identifier \"fact\" is not declared in the current scope."));
	const compiler::CompileResult named = compile(in_function("var f := func on_timeout(): pass\nf.call()"));
	require_verified(named);
	CHECK(only_lambda(named.module)->name == "on_timeout");
}

TEST_CASE("a lambda is a function of the module after the class's, made by MAKE_LAMBDA with what it captures (spec §3.11)") {
	const compiler::CompileResult result = compile("extends Node\n\nvar speed := 2.0\n\nfunc scale(by: int) -> Callable:\n\tvar factor := by * 2\n\treturn func(x: float): return x * factor * speed\n\nfunc plain() -> Callable:\n\treturn func(): return 1\n\nfunc after() -> void:\n\tpass\n");
	require_verified(result);
	const bytecode::Module &module = result.module;
	REQUIRE(module.functions.size() >= 5);
	// The class's functions first, so that hot reload compares them alone (§12.4).
	CHECK(module.functions[2].name == "after");
	const bytecode::Function &scaling = module.functions[3];
	const bytecode::Function &plain = module.functions[4];
	CHECK(scaling.is_lambda);
	CHECK(scaling.params.size() == 2);
	CHECK(scaling.captures == 1);
	CHECK(scaling.params[1].name == "factor");
	CHECK(!scaling.is_static); // It reads a member.
	CHECK(plain.is_lambda);
	CHECK(plain.captures == 0);
	CHECK(plain.is_static);
	const bytecode::Function &scale = function_named(module, "scale");
	const size_t pc = find_instruction(scale, Op::MAKE_LAMBDA);
	CHECK(scale.code[pc + 1] == 3);
	CHECK(scale.code[pc + 4] == 1);
}

TEST_CASE("a lambda in a lambda captures through it, and one that uses self makes both use it (spec §3.11)") {
	const compiler::CompileResult result = compile("extends Node\n\nvar total := 0\n\nfunc f() -> void:\n\tvar n := 3\n\tvar outer := func():\n\t\tvar inner := func(): total = n\n\t\tinner.call()\n\touter.call()\n");
	require_verified(result);
	int lambdas = 0;
	for (const bytecode::Function &function : result.module.functions) {
		if (function.is_lambda) {
			lambdas++;
			CHECK(function.captures == 1);
			CHECK(!function.is_static);
		}
	}
	CHECK(lambdas == 2);
}

TEST_CASE("a lambda in a static function is static (spec §4.8)") {
	CHECK(has_error("extends Node\nvar total := 0\nstatic func f() -> void:\n\tvar g := func(): total = 1\n", "\"total\" belongs to an instance, which the static function \"f()\" does not have."));
	require_verified(compile("extends Node\nstatic func f() -> int:\n\tvar n := 2\n\tvar g := func(): return n\n\treturn g.call()\n"));
}

TEST_CASE("lambdas over several lines inside brackets, and several on one line (spec §3.11)") {
	require_verified(compile(in_function("var fs: Array[Callable] = [\n\tfunc():\n\t\treturn 1,\n\tfunc():\n\t\treturn 2,\n]\nprint(fs.size())")));
	require_verified(compile(in_function("var f := func():\n\tprint(1)\n\tprint(2)\nf.call()")));
	require_verified(compile(in_function("var f := func(): print(1); print(2)\nf.call()")));
	require_verified(compile(in_function("child_entered_tree.connect(func(node):\n\tif node.name == &\"a\":\n\t\tprint(1)\n\tchild_entered_tree.connect(func(other):\n\t\tprint(other.name)\n\t)\n)\nprint(2)")));
	require_verified(compile(in_function("child_entered_tree.connect(func(node):\n\tprint(node.name))\nprint(2)")));
	require_verified(compile("extends Node\nvar on_hit := func(amount: int): print(amount)\nfunc f() -> void:\n\ton_hit.call(3)\n"));
}
