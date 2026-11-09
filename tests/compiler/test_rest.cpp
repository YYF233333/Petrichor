#include "helpers.h"

using namespace petrichor;
using namespace petrichor::tests;

// Rest parameters (spec §4.6).

TEST_CASE("a rest parameter takes the arguments after the others, each checked") {
	const std::string declared = "extends Node\nfunc joined(sep: String, ...parts: Array[String]) -> int:\n\treturn parts.size()\n";
	const auto result = compile(declared + "func f() -> void:\n\tjoined(\"-\")\n\tjoined(\"-\", \"a\", \"b\")\n");
	REQUIRE_MESSAGE(result.ok(), messages(result));
	const auto &joined = function_named(result.module, "joined");
	CHECK(joined.is_vararg);
	CHECK(joined.fixed_params() == 1);
	std::string error;
	CHECK_MESSAGE(bytecode::verify(result.module, error), error);
	CHECK(has_error(declared + "func f() -> void:\n\tjoined(\"-\", \"a\", 2)\n", "Cannot pass"));
	CHECK(has_error(declared + "func f() -> void:\n\tjoined()\n", "at least 1"));
	CHECK(has_error(declared + "func f(names: Array[String]) -> void:\n\tjoined(\"-\", ...names)\n", "use callv()"));
	// An untyped Array takes any value, an Array too.
	CHECK(compile("func count(...values: Array) -> int:\n\treturn values.size()\nfunc f() -> void:\n\tcount(1, \"a\", [2])\n").ok());
}

TEST_CASE("a rest parameter is a trailing Array without a default, written with its type") {
	CHECK(has_error("func f(...rest) -> void:\n\tpass\n", "rest parameter \"rest\" needs a type"));
	CHECK(has_error("func f(...rest: int) -> void:\n\tpass\n", "as an Array or an Array[T]"));
	CHECK(has_error("func f(...rest: Array = []) -> void:\n\tpass\n", "cannot have a default value"));
	CHECK(has_error("func f(...rest: Array, last: int) -> void:\n\tpass\n", "must be the last"));
	CHECK(has_error("signal said(...words: Array)\n", "A signal cannot have a rest parameter"));
	CHECK(has_error("func f(...rest: Array) -> void:\n\tpass\nvar g: func(int) = f\n", "Callable"));
}

TEST_CASE("an override keeps every argument count of its base, a rest parameter too") {
	FakeProject p;
	p.add_script("res://base.pet", "class_name Base\nextends Node\npub func say(level: int, ...parts: Array[String]) -> void:\n\tpass\npub func one(a: int) -> int:\n\treturn a\n", "Base");
	const auto errors = [&](const std::string &p_body) { return messages(compile_in("extends Base\n" + p_body, p)); };
	CHECK(errors("pub func say(level, ...parts) -> void:\n\tvar s: Array[String] = parts\n").empty());
	CHECK(errors("pub func say(level: int, first: String = \"\", ...parts: Array) -> void:\n\tpass\n").empty());
	CHECK(errors("pub func one(a: int, ...more: Array[int]) -> int:\n\treturn a + more.size()\n").empty());
	CHECK(errors("pub func one(...all: Array[int]) -> int:\n\treturn all.size()\n").empty());
	CHECK(contains(errors("pub func say(level: int) -> void:\n\tpass\n"), "keep the rest parameter"));
	CHECK(contains(errors("pub func say(level: int, ...parts: Array[int]) -> void:\n\tpass\n"), "narrows"));
	CHECK(contains(errors("pub func one(...all: Array[float]) -> int:\n\treturn 0\n"), "narrows"));
	CHECK(contains(errors("pub func one(a: int, b: int, ...more: Array) -> int:\n\treturn 0\n"), "argument count"));
	// An engine virtual method, to which the engine passes one argument.
	CHECK(compile("extends Node\nfunc _process(delta: float, scale: float = 1.0) -> void:\n\tpass\nfunc _physics_process(...rest: Array) -> void:\n\tpass\n").ok());
	CHECK(has_error("extends Node\nfunc _process(delta: float, scale: float) -> void:\n\tpass\n", "with 1 parameter(s)"));
	CHECK(has_error("extends Node\nfunc _process(...rest: Array[int]) -> void:\n\tpass\n", "passes float in this rest parameter"));
}

TEST_CASE("a function with a rest parameter is a Callable, checked by its declaration where a signal is connected") {
	const std::string declared = "extends Node\npub signal told(level: int, text: String)\nfunc hear(level: int, ...rest: Array[String]) -> void:\n\tpass\nfunc deaf(...rest: Array[int]) -> void:\n\tpass\n";
	CHECK(compile(declared + "func f() -> void:\n\tvar g: Callable = hear\n\tg.call(1, \"a\")\n\ttold.connect(hear)\n\ttold.connect(func(...all: Array): pass)\n").ok());
	CHECK(has_error(declared + "func f() -> void:\n\ttold.connect(deaf)\n", "passes String as argument 2"));
	FakeProject p;
	CHECK(contains(messages(compile_in(declared + "func f() -> void:\n\tconnect(\"told\", deaf)\n", p)), "passes String as argument 2"));
	const std::string by_name = messages(compile_in("extends Node\nfunc f() -> void:\n\tcall_deferred(\"g\", 1, 2, 3)\n\tcall_deferred(\"g\", 1, \"x\")\npub func g(a: int, ...rest: Array[int]) -> void:\n\tpass\n", p));
	CHECK(contains(by_name, "takes int as argument 2"));
	CHECK_FALSE(contains(by_name, "follow"));
	p.add_script("res://shape.pet", "class_name Shape\n\npub interface Named:\n\tfunc name(...parts: Array[String]) -> String\n", "Shape");
	CHECK(contains(messages(compile_in("extends Node implements Shape.Named\npub func name(parts: Array[String]) -> String:\n\treturn \"\"\n", p)), "func(...Array[String]) -> String"));
}
