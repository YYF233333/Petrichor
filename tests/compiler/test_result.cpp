#include "doctest.h"
#include "helpers.h"

using namespace petrichor;
using namespace petrichor::tests;

// Result (spec §3.5), its patterns (§5.6) and postfix ? (§8.2).

namespace {

const char *const HEAD = "class_name Parse\nextends Node\n\nfunc parsed(s: String) -> Result[int, String]:\n\treturn Ok(s.to_int()) if s.is_valid_int() else Err(s)\n\n";

std::string errors(const std::string &p_body, const std::string &p_signature = "func f(r: Result[int, String], o: int?) -> Result[int, String]:") {
	return messages(compile(std::string(HEAD) + p_signature + "\n" + p_body + "\n"));
}

} // namespace

TEST_CASE("Ok and Err take their Result type from where they go, and a Result is not discarded") {
	CHECK(errors("\tvar a: Result[int, String] = Ok(1)\n\tvar b: Array[Result[int, String]] = [a, Err(\"x\")]\n\tprint(a == Ok(1), a != b[1])\n\tparsed(\"1\").ok()\n\treturn Ok(2) if a == b[0] else Err(\"y\")").empty());
	CHECK(contains(errors("\tvar a := Ok(1)\n\treturn r"), "Ok(...) takes its type from where it goes, as in `var r: Result[int, String] = Ok(...)`; nothing here gives one."));
	CHECK(contains(errors("\tvar a: int = Err(\"x\")\n\treturn r"), "Err(...) makes a Result, but a value of type int goes here."));
	CHECK(contains(errors("\tparsed(\"1\")\n\treturn r"), "A Result cannot be discarded: handle its error, or ignore it with .ok()."));
	CHECK(contains(errors("\tpass", "func g(r: Result[int]) -> void:"), "Result takes a value type and an error type, as in Result[int, String]."));
}

TEST_CASE("the methods of Result take the value or the error apart") {
	const std::string body = "\tvar a: int = r.unwrap() + r.expect(\"m\") + r.unwrap_or(0) + r.unwrap_or_else(func(e): return e.length())\n\tvar b: Result[String, String] = r.map(func(v): return str(v))\n\tvar c: Result[int, int] = r.map_err(func(e): return e.length())\n\tvar d: Result[float, String] = r.and_then(func(v) -> Result[float, String]: return Ok(v * 0.5))\n\tvar k: Result[int, bool] = r.or_else(func(e) -> Result[int, bool]: return Err(e.is_empty()))\n\tvar g: int? = r.ok()\n\tvar h: String? = r.err()\n\treturn r";
	const compiler::CompileResult result = compile(std::string(HEAD) + "func f(r: Result[int, String], o: int?) -> Result[int, String]:\n" + body + "\n");
	REQUIRE_MESSAGE(result.ok(), messages(result));
	std::string error;
	CHECK_MESSAGE(bytecode::verify(result.module, error), error);
	CHECK(contains(errors("\tvar x := r.and_then(func(v) -> Result[int, bool]: return Ok(v))\n\treturn r"), "The function passed to and_then() returns Result[int, bool], not Result[U, String]."));
	CHECK(contains(errors("\tvar x := r.map(func(v: String): return v)\n\treturn r"), "\"map()\" takes a function of one parameter, which takes int; found func(String) -> String."));
	CHECK(contains(errors("\tvar x := r.flatten()\n\treturn r"), "Result[int, String] has no method \"flatten()\"."));
	CHECK(contains(errors("\tprint(q.ok())", "func g(q: Result[int?, String]) -> void:"), "ok() of Result[int?, String] would give int??, and nested Options are not supported yet."));
}

// Spec §4.6: a lambda's return type is what its body returns, also the side
// of a Result that and_then() or or_else() leaves open, and a None.
TEST_CASE("a lambda passed to and_then() or or_else() returns Ok, Err, None and ? without a return type") {
	const std::string body = "\tvar a: Result[float, String] = r.and_then(func(v): return Ok(v * 0.5) if v > 0 else Err(\"neg\"))\n"
							 "\tvar b: Result[int, bool] = r.or_else(func(e):\n\t\tif e.is_empty():\n\t\t\treturn Err(true)\n\t\treturn Ok(0))\n"
							 "\tvar c: Result[int, String] = r.and_then(func(v): return Ok(parsed(str(v))? + 1))\n"
							 "\tvar d: int? = o.and_then(func(x):\n\t\tif x > 5:\n\t\t\treturn None\n\t\treturn x + 1)\n"
							 "\tvar e: String? = o.and_then(func(x): return str(o?))\n\treturn r";
	const compiler::CompileResult result = compile(std::string(HEAD) + "func f(r: Result[int, String], o: int?) -> Result[int, String]:\n" + body + "\n");
	REQUIRE_MESSAGE(result.ok(), messages(result));
	std::string error;
	CHECK_MESSAGE(bytecode::verify(result.module, error), error);
	CHECK(contains(errors("\tvar x := r.and_then(func(v): return Err(\"no\"))\n\treturn r"), "needs a return type: what it returns does not say what Ok(...) holds."));
	CHECK(contains(errors("\tvar x := o.and_then(func(v): return None)\n\treturn r"), "needs a return type: what it returns does not say which T? None is."));
	CHECK(contains(errors("\tvar x := r.or_else(func(e): return Ok(1))\n\treturn r"), "what Err(...) holds."));
}

TEST_CASE("Ok(...) and Err(...) take a Result apart, and a match over one handles both") {
	CHECK(errors("\tmatch r:\n\t\tOk(var v):\n\t\t\treturn Ok(v + 1)\n\t\tErr(var e):\n\t\t\treturn Err(e)").empty());
	CHECK(contains(errors("\tmatch r:\n\t\tOk(var v):\n\t\t\treturn Ok(v)\n\treturn r"), "This match over Result[int, String] does not handle Err(..);"));
	CHECK(contains(errors("\tmatch r:\n\t\tOk(0), Err(_):\n\t\t\treturn r\n\treturn r"), "does not handle other Ok(..);"));
	CHECK(contains(errors("\tmatch o:\n\t\tOk(var v):\n\t\t\treturn r\n\treturn r"), "Ok(...) matches a value of a type Result[T, E], not int?."));
}

TEST_CASE("postfix ? returns the Err or the None it finds, which the function returns") {
	// A lambda returns what the function type it goes to returns.
	CHECK(errors("\tvar l: func(int?) -> int? = func(p): return p? + 1\n\tvar m: func() -> Result[int, String] = func(): return Ok(1)\n\tparsed(\"1\")?\n\treturn Ok(parsed(\"2\")? + r?)\n\nfunc g(o: int?) -> int?:\n\treturn -o? + 1\n\nasync func h() -> Result[int, String]:\n\treturn Ok(1)\n\nasync func i() -> Result[int, String]:\n\treturn Ok(await h()? + 1)").empty());
	CHECK(contains(errors("\treturn Ok(q?)", "func g(q: Result[int, int]) -> Result[int, String]:"), "convert the error with map_err()."));
	CHECK(contains(errors("\treturn Ok(o?)"), "convert it with ok_or()."));
	CHECK(contains(errors("\treturn r?", "func g(r: Result[int, String]) -> int?:"), "convert it with ok()."));
	CHECK(contains(errors("\tprint(r?)", "func g(r: Result[int, String]) -> void:"), "returns void."));
	CHECK(contains(errors("\tvar l := func(): return r?\n\treturn r"), "from the lambda, whose return type is then needed"));
	CHECK(contains(errors("\tassert(o? > 0)\n\treturn r"), "? cannot appear in an assert"));
	CHECK(contains(errors("\tvar x := 3?\n\treturn r"), "? takes apart a Result or a T?, not int."));
}

TEST_CASE("T.try_from(v) gives the value as a T, or a ConversionError") {
	const std::string body = "\tvar a: Result[int, ConversionError] = int.try_from(3.0)\n\tvar b: Result[Array[int], ConversionError] = Array[int].try_from([1])\n\tvar c: Result[Dictionary[String, int?], ConversionError] = Dictionary[String, int?].try_from({})\n\tvar d: Result[Node, ConversionError] = Node.try_from(self)\n\tvar e := ConversionError(message = \"x\")\n\tvar m: String = c.err().unwrap_or(e).message\n\treturn r";
	const compiler::CompileResult result = compile(std::string(HEAD) + "func f(r: Result[int, String], o: int?) -> Result[int, String]:\n" + body + "\n");
	REQUIRE_MESSAGE(result.ok(), messages(result));
	std::string error;
	CHECK_MESSAGE(bytecode::verify(result.module, error), error);
	// A value named so is not a type.
	CHECK(contains(errors("\tvar x := r.try_from(1)\n\treturn r"), "Result[int, String] has no method \"try_from()\"."));
}

TEST_CASE("a Result holds its values in its slot, and is not exported") {
	CHECK(contains(messages(compile("extends Node\n\nstruct S:\n\tvar r: Result[S, int]\n")), "S contains itself through S.r"));
	CHECK(contains(messages(compile("extends Node\n\n@export pub var r: Result[int, String] = Ok(1)\n")), "A Result is an enum with payloads, which cannot be exported in the first version."));
}
