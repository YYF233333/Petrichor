#include "bytecode/opcodes.h"
#include "helpers.h"

#include "doctest.h"

#include <map>

using namespace petrichor;
using namespace petrichor::tests;
using bytecode::ValueType;

namespace {

bytecode::Module module_of(const std::string &p_source) {
	compiler::CompileResult result = compile(p_source);
	REQUIRE_MESSAGE(result.ok(), joined(errors_of(p_source)));
	return result.module;
}

std::string listing(const bytecode::Module &p_module, const std::string &p_function) {
	const int index = p_module.find_function(p_function);
	REQUIRE(index >= 0);
	return bytecode::disassemble(p_module, p_module.functions[index]);
}

bool contains(const std::string &p_text, const std::string &p_part) {
	return p_text.find(p_part) != std::string::npos;
}

// Follows the control flow of a function whose branches test its bool
// parameters, and records the int constants it stores into members. Returns
// false when the code does not return within a bounded number of steps, or
// uses an instruction this does not follow.
bool trace(const bytecode::Module &p_module, const std::string &p_function, const std::vector<bool> &p_args, std::vector<int64_t> &r_stores) {
	using bytecode::Op;
	const bytecode::Function &fn = p_module.functions[p_module.find_function(p_function)];
	std::map<uint32_t, uint64_t> slots;
	for (size_t i = 0; i < p_args.size(); i++) {
		slots[fn.params[i].offset] = p_args[i] ? 1 : 0;
	}
	const std::vector<uint32_t> &code = fn.code;
	size_t pc = 0;
	for (int steps = 0; steps < 1000 && pc < code.size(); steps++) {
		switch (Op(code[pc])) {
			case Op::LINE:
				pc += 2;
				break;
			case Op::CONST_INT:
				slots[code[pc + 1]] = uint64_t(p_module.constants[code[pc + 2]].i);
				pc += 3;
				break;
			case Op::CONST_BOOL:
				slots[code[pc + 1]] = code[pc + 2];
				pc += 3;
				break;
			case Op::MOVE_8:
				slots[code[pc + 1]] = slots[code[pc + 2]];
				pc += 3;
				break;
			case Op::STORE_MEMBER_8:
				r_stores.push_back(int64_t(slots[code[pc + 2]]));
				pc += 3;
				break;
			case Op::JUMP:
				pc = code[pc + 1];
				break;
			case Op::JUMP_IF_FALSE:
				pc = slots[code[pc + 1]] ? pc + 3 : code[pc + 2];
				break;
			case Op::JUMP_IF_TRUE:
				pc = slots[code[pc + 1]] ? code[pc + 2] : pc + 3;
				break;
			case Op::RETURN_VOID:
				return true;
			default:
				return false;
		}
	}
	return false;
}

std::vector<int64_t> stores(const bytecode::Module &p_module, const std::string &p_function, const std::vector<bool> &p_args) {
	std::vector<int64_t> result;
	CHECK_MESSAGE(trace(p_module, p_function, p_args, result), "the function returns");
	return result;
}

} // namespace

TEST_CASE("class metadata") {
	const bytecode::Module module = module_of(R"(class_name Counter
extends Node
@export pub var speed: float = 2
var count := -3
var label := "n"
)");
	CHECK(module.global_name == "Counter");
	CHECK(module.base_class == "Node");
	REQUIRE(module.members.size() == 3);
	CHECK(module.members[0].type == ValueType::Float);
	CHECK(module.members[0].is_pub);
	CHECK(module.members[0].is_export);
	CHECK(module.members[0].initial.f == 2.0);
	CHECK(module.members[1].type == ValueType::Int);
	CHECK(module.members[1].initial.i == -3);
	CHECK(module.members[2].initial.s == "n");
	CHECK(module.members[2].offset == 16);
	CHECK(module.instance_size == 24);
}

TEST_CASE("signatures") {
	const bytecode::Module module = module_of("extends Node\n\nfunc _process(delta):\n\tpass\n\npub func add(a: int, b: int) -> int:\n\treturn a + b\n");
	const bytecode::Function &process = module.functions[module.find_function("_process")];
	CHECK(process.is_override);
	REQUIRE(process.params.size() == 1);
	CHECK(process.params[0].type == ValueType::Float);
	const bytecode::Function &add = module.functions[module.find_function("add")];
	CHECK(add.is_pub);
	CHECK(add.return_type == ValueType::Int);
	CHECK(contains(listing(module, "add"), "ADD_INT 16 0 8"));
}

TEST_CASE("mixed int and float arithmetic converts the int") {
	const std::string code = listing(module_of("extends Node\n\nfunc f(x: float) -> float:\n\treturn x * 2\n"), "f");
	CHECK(contains(code, "INT_TO_FLOAT"));
	CHECK(contains(code, "MUL_FLOAT"));
}

TEST_CASE("int division stays int") {
	const std::string code = listing(module_of("extends Node\n\nfunc f(a: int) -> int:\n\treturn a / 2\n"), "f");
	CHECK(contains(code, "DIV_INT"));
	CHECK(!contains(code, "INT_TO_FLOAT"));
}

TEST_CASE("control flow becomes jumps") {
	const std::string code = listing(module_of(R"(extends Node

func f(n: int) -> int:
	var total := 0
	while n > 0:
		if n == 3:
			break
		total += n
		n -= 1
	return total
)"),
			"f");
	CHECK(contains(code, "JUMP_IF_FALSE"));
	CHECK(contains(code, "JUMP "));
	CHECK(contains(code, "RETURN"));
}

TEST_CASE("a function that runs off its end returns from every branch") {
	const bytecode::Module module = module_of(R"(extends Node
var x := 0

func chain(a: bool, b: bool):
	if a:
		x = 1
	elif b:
		x = 2
	else:
		x = 3

func only_if(a: bool):
	if a:
		x = 1

func in_loop(a: bool):
	while a:
		if a:
			x = 1
		a = false
)");
	CHECK(stores(module, "chain", { true, false }) == std::vector<int64_t>{ 1 });
	CHECK(stores(module, "chain", { false, true }) == std::vector<int64_t>{ 2 });
	CHECK(stores(module, "chain", { false, false }) == std::vector<int64_t>{ 3 });
	CHECK(stores(module, "only_if", { true }) == std::vector<int64_t>{ 1 });
	CHECK(stores(module, "only_if", { false }).empty());
	CHECK(stores(module, "in_loop", { true }) == std::vector<int64_t>{ 1 });
	CHECK(stores(module, "in_loop", { false }).empty());
}

TEST_CASE("calls") {
	const bytecode::Module module = module_of(R"(extends Node

func g(x: float) -> float:
	return x

func f() -> void:
	print("n = ", get_child_count())
	g(1)
)");
	const std::string code = listing(module, "f");
	CHECK(contains(code, "CALL_UTILITY"));
	CHECK(contains(code, "CALL_ENGINE"));
	CHECK(contains(code, "CALL 0"));
	REQUIRE(module.utility_calls.size() == 1);
	CHECK(module.utility_calls[0].function == "print");
	REQUIRE(module.engine_calls.size() == 1);
	CHECK(module.engine_calls[0].method == "get_child_count");
	CHECK(module.engine_calls[0].return_type == ValueType::Int);
}

TEST_CASE("serialization round-trips") {
	const bytecode::Module module = module_of("class_name A\nextends Node\nvar s := \"x\"\n\nfunc f(a: int) -> String:\n\tif a > 1:\n\t\treturn s\n\treturn \"y\"\n");
	const std::vector<uint8_t> data = bytecode::serialize(module);
	bytecode::Module copy;
	std::string error;
	REQUIRE(bytecode::deserialize(data.data(), data.size(), copy, error));
	CHECK(copy.global_name == "A");
	CHECK(copy.source_hash == module.source_hash);
	CHECK(copy.members[0].initial.s == "x");
	CHECK(listing(copy, "f") == listing(module, "f"));
	CHECK(bytecode::serialize(copy) == data);
}

TEST_CASE("corrupt data is rejected") {
	const bytecode::Module module = module_of("extends Node\n\nfunc f() -> void:\n\tpass\n");
	std::vector<uint8_t> data = bytecode::serialize(module);
	data.resize(data.size() / 2);
	bytecode::Module copy;
	std::string error;
	CHECK(!bytecode::deserialize(data.data(), data.size(), copy, error));
	data[4] = 99;
	CHECK(!bytecode::deserialize(data.data(), data.size(), copy, error));
}
