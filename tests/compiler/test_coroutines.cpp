#include "helpers.h"
using namespace petrichor;
using namespace petrichor::tests;
using bytecode::Op;

TEST_CASE("async calls, Task[void] and await ownership survive serialization") {
	const auto result = compile(R"(extends Node
signal tick(value: int)
async func work() -> int:
	return await tick
async func nothing():
	await tick
async func parent() -> int:
	var t: Task[void] = nothing()
	t.cancel()
	await t
	return await work()
func detached():
	work().detach()
)");
	REQUIRE_MESSAGE(result.ok(), messages(result));
	const auto bytes = bytecode::serialize(result.module);
	bytecode::Module module;
	std::string error;
	REQUIRE_MESSAGE(bytecode::deserialize(bytes.data(), bytes.size(), module, error), error);
	auto &parent = function_named(module, "parent");
	CHECK(parent.is_async);
	size_t pc = find_instruction(parent, Op::AWAIT_TASK);
	CHECK(parent.code[pc + 3] == 0);
	pc += bytecode::instruction_length(parent.code.data() + pc);
	while (Op(parent.code[pc]) != Op::AWAIT_TASK) {
		pc += bytecode::instruction_length(parent.code.data() + pc);
	}
	CHECK(parent.code[pc + 3] == 1);
	parent.is_async = false;
	CHECK_FALSE(bytecode::verify(module, error));
	CHECK(contains(error, "await"));
}

TEST_CASE("coroutine declarations enforce their public contracts") {
	CHECK(has_error("extends Node\nfunc f(s: Signal):\n\tawait s\n", "async"));
	CHECK(has_error("async func f():\n\tpass\nfunc g():\n\tf()\n", "detach"));
	CHECK(has_error("func f(t: Task):\n\tpass\n", "Task"));
	CHECK(has_error("func f(t: Array[void]):\n\tpass\n", "void"));
	CHECK(has_error("signal s(a: int, b: int)\nasync func f() -> int:\n\treturn await s\n", "void"));
	CHECK(has_error("async func f() -> int:\n\treturn 1\nfunc g():\n\tvar t: Task[float] = f()\n", "Task"));
	const auto result = compile("async func f() -> int:\n\treturn 3\n");
	REQUIRE(result.ok());
	const auto &fn = result.module.functions[0];
	CHECK(bytecode::signature_key(result.module.types, fn.params, 0, fn.result, true) != bytecode::signature_key(result.module.types, fn.params, 0, fn.result, false));
}

TEST_CASE("nullable Task handles require unwrap before waiting") {
	CHECK(has_error("async func f(t: Task[int]?):\n\treturn await t\n", "Unwrap"));
	const auto result = compile("async func f(t: Task[int]?) -> int:\n\treturn await t.unwrap()\n");
	REQUIRE_MESSAGE(result.ok(), messages(result));
	const auto &fn = result.module.functions[0];
	CHECK(fn.code[find_instruction(fn, Op::AWAIT_TASK) + 3] == 0);
}

TEST_CASE("async engine overrides and Task exports obey their contracts") {
	CHECK(has_error("async func _init():\n\tpass\n", "_init() cannot be async"));
	CHECK(has_error("async func _to_string() -> String:\n\treturn \"ok\"\n", "cannot be async"));
	CHECK(has_error("async func _to_string():\n\treturn \"ok\"\n", "cannot be async"));
	CHECK(has_error("extends Control\nasync func _has_point(point):\n\treturn true\n", "cannot be async"));
	const auto valid = compile("extends Node\nfunc _init():\n\tpass\nfunc _to_string():\n\treturn \"ok\"\nasync func _ready():\n\tpass\nasync func ordinary() -> int:\n\treturn 1\n");
	REQUIRE_MESSAGE(valid.ok(), messages(valid));
	CHECK(has_error("@export var t: Task[int]\n", "cannot be exported"));
	CHECK(has_error("@export var t: Task[int]?\n", "cannot be exported"));
	CHECK_FALSE(compile("@export var t: Array[Task[int]]\n").ok());
	CHECK_FALSE(contains(joined(errors_of("var t: Task[int]\n")), "unless exported"));
	CHECK(has_error("async func f(cb: Callable):\n\tawait cb.get_argument_count()\n", "await expects"));
	CHECK(has_error("async func f(cb: Callable):\n\tawait cb.is_valid()\n", "await expects"));
}
