#include "bytecode/opcodes.h"
#include "doctest.h"
#include "helpers.h"

using namespace petrichor;
using namespace petrichor::tests;
using bytecode::Op;

// A cache file is read in the game without being compiled again
// (architecture §2.4), so whatever its bytes say must not let the virtual
// machine reach memory outside its frames and instances.

namespace {

const char *SOURCE = R"(extends Node
var count := 0
var label := "x"

func pick(s: String, a: int, flag: bool) -> int:
	if flag:
		return a + a
	return get_child_count()

func touch():
	count += 1
	pick(label, count, true)
)";

bytecode::Module compiled() {
	compiler::CompileResult result = compile(SOURCE);
	REQUIRE_MESSAGE(result.ok(), joined(errors_of(SOURCE)));
	return result.module;
}

std::string problem(const bytecode::Module &p_module) {
	std::string error;
	return bytecode::verify(p_module, error) ? std::string() : error;
}

bool rejected(const bytecode::Module &p_module, const std::string &p_part) {
	const std::string error = problem(p_module);
	INFO(error);
	return !error.empty() && contains(error, p_part);
}

} // namespace

TEST_CASE("compiled modules pass verification") {
	CHECK(problem(compiled()).empty());
}

TEST_CASE("instructions must be whole and jumps must land on them") {
	bytecode::Module module = compiled();
	bytecode::Function &pick = function_named(module, "pick");
	SUBCASE("unknown instruction") {
		pick.code[0] = uint32_t(Op::COUNT) + 7;
		CHECK(rejected(module, "unknown instruction"));
	}
	SUBCASE("jump into an instruction") {
		const size_t branch = find_instruction(pick, Op::JUMP_IF_FALSE);
		pick.code[branch + 2] += 1;
		CHECK(rejected(module, "not an instruction"));
	}
	SUBCASE("jump past the end") {
		const size_t branch = find_instruction(pick, Op::JUMP_IF_FALSE);
		pick.code[branch + 2] = uint32_t(pick.code.size());
		CHECK(rejected(module, "not an instruction"));
	}
	SUBCASE("cut off") {
		pick.code.pop_back();
		CHECK(!problem(module).empty());
	}
	SUBCASE("running past the end") {
		bytecode::Function &touch = function_named(module, "touch");
		touch.code.pop_back(); // The closing RETURN_VOID.
		CHECK(rejected(module, "past its end"));
	}
	SUBCASE("a call whose argument count runs past the code") {
		bytecode::Function &touch = function_named(module, "touch");
		touch.code[find_instruction(touch, Op::CALL) + 4] = 0x40000000;
		CHECK(rejected(module, "cut off"));
	}
}

TEST_CASE("operands must name slots of the instruction's type") {
	bytecode::Module module = compiled();
	bytecode::Function &pick = function_named(module, "pick");
	const uint32_t string_slot = pick.params[0].offset;
	SUBCASE("int arithmetic on a String") {
		pick.code[find_instruction(pick, Op::ADD_INT) + 2] = string_slot;
		CHECK(rejected(module, "is not a slot of type int"));
	}
	SUBCASE("outside the frame") {
		pick.code[find_instruction(pick, Op::ADD_INT) + 1] = pick.frame_size;
		CHECK(rejected(module, "is not a slot of type int"));
	}
	SUBCASE("inside another slot") {
		pick.code[find_instruction(pick, Op::ADD_INT) + 1] = pick.params[1].offset + 4;
		CHECK(rejected(module, "is not a slot of type int"));
	}
	SUBCASE("a branch on a non-bool") {
		pick.code[find_instruction(pick, Op::JUMP_IF_FALSE) + 1] = pick.params[1].offset;
		CHECK(rejected(module, "is not a slot of type bool"));
	}
	SUBCASE("a member of another type") {
		bytecode::Function &touch = function_named(module, "touch");
		touch.code[find_instruction(touch, Op::LOAD_MEMBER_8) + 2] = module.members[1].offset;
		CHECK(rejected(module, "member"));
	}
	SUBCASE("a call argument of another type") {
		bytecode::Function &touch = function_named(module, "touch");
		const size_t call = find_instruction(touch, Op::CALL);
		touch.code[call + 5] = uint32_t(bytecode::ValueType::Int); // The String argument.
		CHECK(rejected(module, "parameter's type"));
	}
	SUBCASE("a result slot of another type") {
		bytecode::Function &touch = function_named(module, "touch");
		const size_t call = find_instruction(touch, Op::CALL);
		touch.code[call + 2] = touch.code[call + 6]; // The String argument's slot.
		CHECK(rejected(module, "is not a slot of type int"));
	}
}

TEST_CASE("constants must have the type their instruction stores") {
	bytecode::Module module = compiled();
	bytecode::Function &touch = function_named(module, "touch");
	SUBCASE("a bool other than 0 and 1") {
		touch.code[find_instruction(touch, Op::CONST_BOOL) + 2] = 2;
		CHECK(rejected(module, "a bool other than 0 and 1"));
	}
	SUBCASE("an int instruction with a float constant") {
		module.constants[touch.code[find_instruction(touch, Op::CONST_INT) + 2]].type = bytecode::ValueType::Float;
		CHECK(rejected(module, "the constant does not have the slot's type"));
	}
}

TEST_CASE("a static function does not use the instance") {
	bytecode::Module module = compiled();
	SUBCASE("members") {
		function_named(module, "touch").is_static = true;
		CHECK(rejected(module, "a static function uses the instance"));
	}
	SUBCASE("self as the base of an engine call") {
		function_named(module, "pick").is_static = true;
		CHECK(rejected(module, "a static function uses the instance"));
	}
}

TEST_CASE("a constructor does not write over its arguments") {
	compiler::CompileResult result = compile("extends Node\n\nfunc v(x: Vector3) -> Vector3:\n\treturn Vector3(x)\n");
	REQUIRE_MESSAGE(result.ok(), messages(result));
	bytecode::Function &v = function_named(result.module, "v");
	const size_t construct = find_instruction(v, Op::CONSTRUCT);
	v.code[construct + 2] = v.code[construct + 6]; // dst is the first argument's slot.
	CHECK(rejected(result.module, "a constructor writes over one of its arguments"));
}

TEST_CASE("only arrays are indexed") {
	// The language does not index strings (spec §3.8), and the runtime's
	// GET_INDEX does not handle them.
	compiler::CompileResult result = compile("extends Node\n\nfunc g(s: String, a: Array[int]) -> int:\n\treturn a[0]\n");
	REQUIRE_MESSAGE(result.ok(), messages(result));
	bytecode::Function &g = function_named(result.module, "g");
	const size_t index = find_instruction(g, Op::GET_INDEX);
	g.code[index + 1] = uint32_t(bytecode::ValueType::String);
	g.code[index + 4] = g.params[0].offset; // The String as the container.
	CHECK(rejected(result.module, "an index into a value that has none"));
}

TEST_CASE("a call to a method of this language passes what the call table says") {
	FakeProject project;
	project.global_classes["Probe"] = "res://test.pet";
	compiler::CompileResult result = compile_in("class_name Probe\nextends Node\n\npub func id(a: int, b: String = \"x\") -> int:\n\treturn a\n\nfunc poke(p: Probe, s: String) -> int:\n\treturn p.id(1, s)\n", project);
	REQUIRE_MESSAGE(result.ok(), messages(result));
	bytecode::Module &module = result.module;
	bytecode::Function &poke = function_named(module, "poke");
	const size_t call = find_instruction(poke, Op::CALL_METHOD);
	CHECK(problem(module).empty());
	SUBCASE("an argument of another type") {
		poke.code[call + 7] = uint32_t(bytecode::ValueType::Int); // The String argument.
		CHECK(rejected(module, "parameter's type"));
	}
	SUBCASE("fewer arguments than the parameters without defaults") {
		poke.code[call + 4] = 0;
		poke.code.erase(poke.code.begin() + call + 5, poke.code.begin() + call + 9);
		CHECK(rejected(module, "wrong number of arguments"));
	}
	SUBCASE("a result slot of another type") {
		poke.code[call + 2] = poke.code[call + 8]; // The String argument's slot.
		CHECK(rejected(module, "is not a slot of type int"));
	}
	SUBCASE("no receiver") {
		poke.code[call + 3] = bytecode::NO_SLOT;
		CHECK(rejected(module, "is not a slot of type Object"));
	}
	SUBCASE("a call table entry with more defaults than parameters") {
		module.method_calls[poke.code[call + 1]].defaults = 3;
		CHECK(rejected(module, "more default values than parameters"));
	}
}

TEST_CASE("tables and layouts must be consistent") {
	bytecode::Module module = compiled();
	bytecode::Function &pick = function_named(module, "pick");
	SUBCASE("engine call index") {
		pick.code[find_instruction(pick, Op::CALL_ENGINE) + 1] = uint32_t(module.engine_calls.size());
		CHECK(rejected(module, "outside the engine call table"));
	}
	SUBCASE("function index") {
		bytecode::Function &touch = function_named(module, "touch");
		touch.code[find_instruction(touch, Op::CALL) + 1] = uint32_t(module.functions.size());
		CHECK(rejected(module, "outside the function table"));
	}
	SUBCASE("overlapping slots") {
		pick.slots[1].offset = pick.slots[0].offset;
		pick.params[1].offset = pick.slots[0].offset;
		CHECK(rejected(module, "overlap"));
	}
	SUBCASE("a slot outside its frame") {
		pick.frame_size = pick.slots.back().offset;
		CHECK(rejected(module, "does not fit"));
	}
	SUBCASE("a member outside the instance") {
		module.members[1].offset = module.instance_size;
		CHECK(rejected(module, "does not fit"));
	}
	SUBCASE("a huge frame") {
		pick.frame_size = 0x7FFFFFFF;
		CHECK(rejected(module, "storage"));
	}
	SUBCASE("a parameter that is not its slot") {
		pick.params[0].type = bytecode::ValueType::Int;
		CHECK(rejected(module, "parameter 0"));
	}
	SUBCASE("an init function outside the module") {
		module.init_function = int32_t(module.functions.size());
		CHECK(rejected(module, "initializer function"));
	}
	SUBCASE("a ready function that takes parameters") {
		module.ready_function = function_index(module, "pick");
		CHECK(rejected(module, "initializer function takes or returns a value"));
	}
}

TEST_CASE("deserialize refuses a module that does not verify") {
	bytecode::Module module = compiled();
	bytecode::Function &pick = function_named(module, "pick");
	pick.code[find_instruction(pick, Op::ADD_INT) + 2] = pick.params[0].offset;
	const std::vector<uint8_t> data = bytecode::serialize(module);
	bytecode::Module copy;
	std::string error;
	CHECK(!bytecode::deserialize(data.data(), data.size(), copy, error));
	CHECK(contains(error, "invalid compiled script"));
}
