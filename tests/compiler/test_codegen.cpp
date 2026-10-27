#include "bytecode/opcodes.h"
#include "compiler/lower.h"
#include "compiler/parser.h"
#include "helpers.h"

#include "doctest.h"

#include <map>

using namespace petrichor;
using namespace petrichor::tests;
using bytecode::Op;
using bytecode::ValueType;

namespace {

bytecode::Module module_of(const std::string &p_source) {
	compiler::CompileResult result = compile(p_source);
	REQUIRE_MESSAGE(result.ok(), joined(errors_of(p_source)));
	// What the compiler writes is what the runtime accepts.
	std::string problem;
	CHECK_MESSAGE(bytecode::verify(result.module, problem), problem);
	return result.module;
}

// The opcodes of a function in order, without operands.
std::vector<Op> opcodes(const bytecode::Module &p_module, const std::string &p_function) {
	const bytecode::Function &fn = p_module.functions[p_module.find_function(p_function)];
	std::vector<Op> result;
	for (size_t pc = 0; pc < fn.code.size(); pc += bytecode::instruction_length(fn.code.data() + pc)) {
		result.push_back(Op(fn.code[pc]));
	}
	return result;
}

// Whether `p_sequence` appears in `p_ops` in order, not necessarily together.
bool in_order(const std::vector<Op> &p_ops, const std::vector<Op> &p_sequence) {
	size_t next = 0;
	for (Op op : p_ops) {
		if (next < p_sequence.size() && op == p_sequence[next]) {
			next++;
		}
	}
	return next == p_sequence.size();
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
	CHECK(add.result.type == ValueType::Int);
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

TEST_CASE("a member of a property is changed in a copy and written back") {
	const bytecode::Module module = module_of("extends Node3D\n\nfunc _process(delta):\n\tposition.y += delta\n");
	CHECK(in_order(opcodes(module, "_process"), { Op::GET_PROPERTY, Op::GET_FIELD, Op::ADD_FLOAT, Op::SET_FIELD, Op::SET_PROPERTY }));
	REQUIRE(module.properties.size() == 1);
	CHECK(module.properties[0].name == "position");
	CHECK(module.properties[0].type == ValueType::Vector3);
	REQUIRE(module.fields.size() == 1);
	CHECK(module.fields[0].base == ValueType::Vector3);
	CHECK(module.fields[0].name == "y");
	CHECK(module.fields[0].type == ValueType::Float);
}

// Spec §6.1: literals among them, as GDScript runs them
// (modules/gdscript/gdscript_compiler.cpp:2398-2435); a literal is also the
// editor's default value.
TEST_CASE("member initializers run in the init function in declaration order") {
	const bytecode::Module module = module_of(R"(extends Node
var time = 0.0
var rng = RandomNumberGenerator.new()
var first := rng.randi()
)");
	CHECK(module.members[0].has_initial);
	CHECK(!module.members[1].has_initial);
	CHECK(module.members[1].type == ValueType::Object);
	CHECK(module.members[1].class_name == "RandomNumberGenerator");
	REQUIRE(module.init_function >= 0);
	const bytecode::Function &init = module.functions[module.init_function];
	CHECK(init.params.empty());
	CHECK(!init.is_pub);
	CHECK(in_order(opcodes(module, init.name), { Op::CONST_FLOAT, Op::STORE_MEMBER_8, Op::NEW_OBJECT, Op::STORE_MEMBER_VALUE, Op::LOAD_MEMBER_VALUE, Op::CALL_ENGINE, Op::STORE_MEMBER_8 }));
	REQUIRE(module.classes.size() == 1);
	CHECK(module.classes[0].name == "RandomNumberGenerator");
	CHECK(module_of("extends Node\nvar a: int\n").init_function == -1);
}

// What GDScript reduces to a constant for these forms is the editor's default
// (modules/gdscript/gdscript_analyzer.cpp:5652-5681): a built-in constant, and
// a constructor of a value type that is not shared whose arguments are
// constants (:3286-3328); the init function still writes them all.
TEST_CASE("a constant initializer is the member's initial value") {
	const bytecode::Module module = module_of(R"(extends Node
var offset := Vector2(400.0, 0.0)
var tint := Color("000000b0")
var up: Vector3 = Vector3.UP
var placed := Transform2D(0.5, Vector2(-1, 2))
var path: NodePath = "a/b"
var doubled := Vector2(1.0, 2.0) * 2.0
var random := Vector2(randf(), 0.0)
var names := PackedStringArray()
var items: Array[int] = [1, 2]
var points: Array[Vector2] = [Vector2(1, 2), Vector2.ZERO]
var nodes: Array[Node] = []
var plain: Array = [1, "x"]
var mixed: Array[float] = [1, randf()]
)");
	auto initial = [&](const char *p_name) { return module.members[module.find_member(p_name)].has_initial; };
	CHECK(initial("offset"));
	CHECK(initial("tint"));
	CHECK(initial("up"));
	CHECK(initial("placed"));
	CHECK(initial("path"));
	CHECK(!initial("doubled"));
	CHECK(!initial("random"));
	CHECK(!initial("names"));
	CHECK(initial("items"));
	CHECK(initial("points"));
	CHECK(initial("nodes"));
	CHECK(initial("plain"));
	CHECK(!initial("mixed"));

	const bytecode::Constant &offset = module.members[module.find_member("offset")].initial;
	CHECK(offset.type == ValueType::Vector2);
	CHECK(offset.constructor >= 0);
	REQUIRE(offset.args.size() == 2);
	CHECK((offset.args[0].type == ValueType::Float && offset.args[0].f == 400.0));
	CHECK(module.members[module.find_member("up")].initial.builtin == "UP");
	CHECK(module.members[module.find_member("path")].initial.type == ValueType::NodePath);
	// An int literal is converted where the constructor takes a float, as written.
	const bytecode::Constant &placed = module.members[module.find_member("placed")].initial;
	REQUIRE(placed.args.size() == 2);
	REQUIRE(placed.args[1].args.size() == 2);
	CHECK((placed.args[1].type == ValueType::Vector2 && placed.args[1].args[0].type == ValueType::Float && placed.args[1].args[0].f == -1.0));
	// An array is typed as the member; an element of an untyped one keeps its type.
	const bytecode::Constant &items = module.members[module.find_member("items")].initial;
	CHECK((items.type == ValueType::Array && items.element == ValueType::Int && items.args.size() == 2 && items.args[1].i == 2));
	const bytecode::Constant &nodes = module.members[module.find_member("nodes")].initial;
	CHECK((nodes.element == ValueType::Object && nodes.class_name == "Node" && nodes.args.empty()));
	const bytecode::Constant &plain = module.members[module.find_member("plain")].initial;
	REQUIRE(plain.args.size() == 2);
	CHECK((plain.element == ValueType::Void && plain.args[0].type == ValueType::Int && plain.args[1].type == ValueType::String));

	const std::vector<uint8_t> data = bytecode::serialize(module);
	bytecode::Module copy;
	std::string error;
	REQUIRE(bytecode::deserialize(data.data(), data.size(), copy, error));
	CHECK(bytecode::serialize(copy) == data);
}

// Hot reload compares the init function and the entries it uses (spec §12.4).
TEST_CASE("the init function and its table entries do not change with a function body") {
	const char *before = "extends Node\nvar v := Vector3(1, 2, 3)\n\nfunc f() -> float:\n\treturn 1.0\n";
	const char *body_changed = "extends Node\nvar v := Vector3(1, 2, 3)\n\nfunc f() -> float:\n\treturn cos(2.0) + 3.0\n";
	const char *initializer_changed = "extends Node\nvar v := Vector3(1, 2, 4)\n\nfunc f() -> float:\n\treturn 1.0\n";
	const bytecode::Module a = module_of(before);
	const bytecode::Module b = module_of(body_changed);
	const bytecode::Module c = module_of(initializer_changed);
	REQUIRE(a.init_function >= 0);
	auto init_code = [](const bytecode::Module &p_module) { return p_module.functions[p_module.init_function].code; };
	auto init_constants = [](const bytecode::Module &p_module) {
		std::vector<int64_t> values;
		for (uint32_t i = 0; i < p_module.init_tables.constants; i++) {
			values.push_back(p_module.constants[i].i);
		}
		return values;
	};
	CHECK(init_code(a) == init_code(b));
	CHECK(init_constants(a) == init_constants(b));
	CHECK(a.init_tables.constructors == b.init_tables.constructors);
	// The code can stay the same when a constant changes, so the entries count too.
	CHECK(init_code(a) == init_code(c));
	CHECK(init_constants(a) != init_constants(c));
}

TEST_CASE("built-in values take their own size in the frame") {
	const bytecode::Module module = module_of("extends Node\n\nfunc f(v: Vector3, t: Transform3D, n: int) -> void:\n\tpass\n");
	const bytecode::Function &f = module.functions[module.find_function("f")];
	REQUIRE(f.params.size() == 3);
	CHECK(f.params[0].offset == 0);
	CHECK(f.params[1].offset == 16);
	CHECK(f.params[2].offset == 64);
	CHECK(f.frame_size >= 72);
}

TEST_CASE("operators, utilities and constructors carry the engine's signatures") {
	const bytecode::Module module = module_of(R"(extends Node

func f(x: float) -> Vector3:
	var v := Vector3(x, 0, 1) * x
	var c := cos(x)
	return lerp(v, Vector3.UP, c)
)");
	REQUIRE(module.operators.size() == 1);
	CHECK(module.operators[0].op == 8); // OP_MULTIPLY
	CHECK(module.operators[0].left == ValueType::Vector3);
	CHECK(module.operators[0].right == ValueType::Float);
	CHECK(module.operators[0].result == ValueType::Vector3);
	REQUIRE(module.constructors.size() == 1);
	CHECK(module.constructors[0].type == ValueType::Vector3);
	CHECK(module.constructors[0].params == std::vector<ValueType>{ ValueType::Float, ValueType::Float, ValueType::Float });
	REQUIRE(module.utility_calls.size() == 2);
	CHECK(module.utility_calls[0].function == "cos");
	CHECK(module.utility_calls[0].hash == 2140049587u);
	CHECK(module.utility_calls[0].params == std::vector<ValueType>{ ValueType::Float });
	CHECK(module.utility_calls[0].return_type == ValueType::Float);
	CHECK(module.utility_calls[1].function == "lerp");
	CHECK(module.utility_calls[1].return_type == ValueType::Variant);
	CHECK(module.utility_calls[1].result_type == ValueType::Vector3);
	CHECK(module.constants.back().builtin == "UP");
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
	bytecode::Module copy;
	std::string error;
	const std::vector<uint8_t> truncated(data.begin(), data.begin() + data.size() / 2);
	CHECK(!bytecode::deserialize(truncated.data(), truncated.size(), copy, error));
	// The format's identifier follows the magic.
	data[4] ^= 1;
	CHECK(!bytecode::deserialize(data.data(), data.size(), copy, error));
	CHECK(error == "compiled by a build of Petrichor with another bytecode format");
}

TEST_CASE("what lowering has no code for is an internal error, not wrong code") {
	// Parsed but not checked: no call or name has a target.
	std::vector<compiler::Diagnostic> errors;
	const compiler::ClassDecl cls = compiler::parse(compiler::tokenize("extends Node\n\nfunc f() -> void:\n\tg()\n\tx = 1\n", errors), errors);
	REQUIRE(errors.empty());
	compiler::lower(cls, false, errors);
	REQUIRE(errors.size() == 2);
	CHECK(errors[0].line == 4);
	CHECK(errors[0].message.find("Internal compiler error") == 0);
	CHECK(errors[1].line == 5);
}

// Spec §10.6: the widths come from the engine API (Node.get_child takes an
// int32 index, CanvasItem.z_index an int32, RandomNumberGenerator.seed a uint64).
TEST_CASE("an engine integer narrower than int64 or unsigned is checked before it goes to the engine") {
	const bytecode::Module module = module_of("extends Node\n\nfunc f(n: Node, c: CanvasItem, r: RandomNumberGenerator, i: int) -> void:\n\tvar child := n.get_child(i)\n\tc.z_index = i\n\tc.z_index += 1\n\tr.seed = i\n\tvar count := n.get_child_count()\n");
	const bytecode::Function &f = module.functions[module.find_function("f")];
	std::vector<bytecode::IntRange> ranges;
	for (size_t pc = 0; pc < f.code.size(); pc += bytecode::instruction_length(f.code.data() + pc)) {
		if (Op(f.code[pc]) == Op::CHECK_RANGE) {
			ranges.push_back(bytecode::IntRange(f.code[pc + 1]));
		}
	}
	const std::vector<bytecode::IntRange> expected = { bytecode::IntRange::INT32, bytecode::IntRange::INT32, bytecode::IntRange::INT32, bytecode::IntRange::UINT64 };
	CHECK(ranges == expected);
	CHECK(bytecode::int_in_range(bytecode::IntRange::INT32, 2147483647));
	CHECK(!bytecode::int_in_range(bytecode::IntRange::INT32, 2147483648));
	CHECK(!bytecode::int_in_range(bytecode::IntRange::UINT32, -1));
	CHECK(bytecode::int_in_range(bytecode::IntRange::UINT32, 4294967295));
	CHECK(!bytecode::int_in_range(bytecode::IntRange::UINT64, -1));
	CHECK(!bytecode::int_in_range(bytecode::IntRange::INT8, 128));
}
