#include "bytecode/opcodes.h"
#include "helpers.h"

#include "doctest.h"

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

bytecode::Function &function(bytecode::Module &r_module, const char *p_name) {
	const int index = r_module.find_function(p_name);
	REQUIRE(index >= 0);
	return r_module.functions[index];
}

// The position of the first instruction with this opcode.
size_t find(const bytecode::Function &p_function, Op p_op) {
	size_t pc = 0;
	while (pc < p_function.code.size()) {
		const Op op = Op(p_function.code[pc]);
		if (op == p_op) {
			return pc;
		}
		const int count = bytecode::operand_count(op);
		pc += count < 0 ? 5 + 2 * p_function.code[pc + 4] : 1 + count;
	}
	FAIL("no such instruction");
	return 0;
}

std::string problem(const bytecode::Module &p_module) {
	std::string error;
	return bytecode::verify(p_module, error) ? std::string() : error;
}

bool rejected(const bytecode::Module &p_module, const std::string &p_part) {
	const std::string error = problem(p_module);
	INFO(error);
	return !error.empty() && error.find(p_part) != std::string::npos;
}

} // namespace

TEST_CASE("compiled modules pass verification") {
	CHECK(problem(compiled()).empty());
}

TEST_CASE("instructions must be whole and jumps must land on them") {
	bytecode::Module module = compiled();
	bytecode::Function &pick = function(module, "pick");
	SUBCASE("unknown instruction") {
		pick.code[0] = uint32_t(Op::COUNT) + 7;
		CHECK(rejected(module, "unknown instruction"));
	}
	SUBCASE("jump into an instruction") {
		const size_t branch = find(pick, Op::JUMP_IF_FALSE);
		pick.code[branch + 2] += 1;
		CHECK(rejected(module, "not an instruction"));
	}
	SUBCASE("jump past the end") {
		const size_t branch = find(pick, Op::JUMP_IF_FALSE);
		pick.code[branch + 2] = uint32_t(pick.code.size());
		CHECK(rejected(module, "not an instruction"));
	}
	SUBCASE("cut off") {
		pick.code.pop_back();
		CHECK(!problem(module).empty());
	}
	SUBCASE("running past the end") {
		bytecode::Function &touch = function(module, "touch");
		touch.code.pop_back(); // The closing RETURN_VOID.
		CHECK(rejected(module, "past its end"));
	}
	SUBCASE("a call whose argument count runs past the code") {
		bytecode::Function &touch = function(module, "touch");
		touch.code[find(touch, Op::CALL) + 4] = 0x40000000;
		CHECK(rejected(module, "cut off"));
	}
}

TEST_CASE("operands must name slots of the instruction's type") {
	bytecode::Module module = compiled();
	bytecode::Function &pick = function(module, "pick");
	const uint32_t string_slot = pick.params[0].offset;
	SUBCASE("int arithmetic on a String") {
		pick.code[find(pick, Op::ADD_INT) + 2] = string_slot;
		CHECK(rejected(module, "is not a slot of type int"));
	}
	SUBCASE("outside the frame") {
		pick.code[find(pick, Op::ADD_INT) + 1] = pick.frame_size;
		CHECK(rejected(module, "is not a slot of type int"));
	}
	SUBCASE("inside another slot") {
		pick.code[find(pick, Op::ADD_INT) + 1] = pick.params[1].offset + 4;
		CHECK(rejected(module, "is not a slot of type int"));
	}
	SUBCASE("a branch on a non-bool") {
		pick.code[find(pick, Op::JUMP_IF_FALSE) + 1] = pick.params[1].offset;
		CHECK(rejected(module, "is not a slot of type bool"));
	}
	SUBCASE("a member of another type") {
		bytecode::Function &touch = function(module, "touch");
		touch.code[find(touch, Op::LOAD_MEMBER_8) + 2] = module.members[1].offset;
		CHECK(rejected(module, "member"));
	}
	SUBCASE("a call argument of another type") {
		bytecode::Function &touch = function(module, "touch");
		const size_t call = find(touch, Op::CALL);
		touch.code[call + 5] = uint32_t(bytecode::ValueType::Int); // The String argument.
		CHECK(rejected(module, "parameter's type"));
	}
	SUBCASE("a result slot of another type") {
		bytecode::Function &touch = function(module, "touch");
		const size_t call = find(touch, Op::CALL);
		touch.code[call + 2] = touch.code[call + 6]; // The String argument's slot.
		CHECK(rejected(module, "is not a slot of type int"));
	}
}

TEST_CASE("tables and layouts must be consistent") {
	bytecode::Module module = compiled();
	bytecode::Function &pick = function(module, "pick");
	SUBCASE("engine call index") {
		pick.code[find(pick, Op::CALL_ENGINE) + 1] = uint32_t(module.engine_calls.size());
		CHECK(rejected(module, "outside the engine call table"));
	}
	SUBCASE("function index") {
		bytecode::Function &touch = function(module, "touch");
		touch.code[find(touch, Op::CALL) + 1] = uint32_t(module.functions.size());
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
		module.ready_function = module.find_function("pick");
		CHECK(rejected(module, "initializer function takes or returns a value"));
	}
}

TEST_CASE("deserialize refuses a module that does not verify") {
	bytecode::Module module = compiled();
	bytecode::Function &pick = function(module, "pick");
	pick.code[find(pick, Op::ADD_INT) + 2] = pick.params[0].offset;
	const std::vector<uint8_t> data = bytecode::serialize(module);
	bytecode::Module copy;
	std::string error;
	CHECK(!bytecode::deserialize(data.data(), data.size(), copy, error));
	CHECK(error.find("invalid compiled script") != std::string::npos);
}
