#include "compiler/lower.h"
#include "compiler/parser.h"
#include "doctest.h"
#include "helpers.h"

using namespace petrichor;
using namespace petrichor::tests;
using bytecode::ValueType;

// What the compiler hands the runtime: the class's metadata, initial values
// and the bytecode format. What the code does is tested by running it, in the
// engine tests (architecture §3).

namespace {

bytecode::Module module_of(const std::string &p_source) {
	compiler::CompileResult result = compile(p_source);
	REQUIRE_MESSAGE(result.ok(), joined(errors_of(p_source)));
	// What the compiler writes is what the runtime accepts.
	std::string problem;
	CHECK_MESSAGE(bytecode::verify(result.module, problem), problem);
	return result.module;
}

std::string listing(const bytecode::Module &p_module, const std::string &p_function) {
	return bytecode::disassemble(p_module, function_named(p_module, p_function));
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
	CHECK(storage(module, module.members[0].type) == ValueType::Float);
	CHECK(module.members[0].is_pub);
	CHECK(module.members[0].is_export);
	CHECK(module.members[0].initial.f == 2.0);
	CHECK(storage(module, module.members[1].type) == ValueType::Int);
	CHECK(module.members[1].initial.i == -3);
	CHECK(storage(module, module.members[2].type) == ValueType::String);
	CHECK(module.members[2].initial.s == "n");
}

TEST_CASE("signatures") {
	const bytecode::Module module = module_of("extends Node\n\nfunc _process(delta):\n\tpass\n\npub func add(a: int, b: int) -> int:\n\treturn a + b\n");
	const bytecode::Function &process = function_named(module, "_process");
	CHECK(process.is_override);
	REQUIRE(process.params.size() == 1);
	CHECK(storage(module, process.params[0].type) == ValueType::Float);
	const bytecode::Function &add = function_named(module, "add");
	CHECK(add.is_pub);
	CHECK(storage(module, add.result.type) == ValueType::Int);
}

// An initializer that is a constant expression is the editor's default, as
// what GDScript reduces to a constant is (modules/gdscript/gdscript_analyzer.cpp:5652-5681;
// spec §4.5); the init function still writes them all.
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
	auto initial = [&](const char *p_name) { return member_named(module, p_name).has_initial; };
	CHECK(initial("offset"));
	CHECK(initial("tint"));
	CHECK(initial("up"));
	CHECK(initial("placed"));
	CHECK(initial("path"));
	CHECK(initial("doubled"));
	CHECK(!initial("random"));
	CHECK(initial("names"));
	CHECK(initial("items"));
	CHECK(initial("points"));
	CHECK(initial("nodes"));
	CHECK(initial("plain"));
	CHECK(!initial("mixed"));

	const bytecode::Constant &offset = member_named(module, "offset").initial;
	CHECK(storage(module, offset.type) == ValueType::Vector2);
	CHECK(offset.constructor >= 0);
	REQUIRE(offset.args.size() == 2);
	CHECK((storage(module, offset.args[0].type) == ValueType::Float && offset.args[0].f == 400.0));
	CHECK(member_named(module, "up").initial.builtin == "UP");
	CHECK(storage(module, member_named(module, "path").initial.type) == ValueType::NodePath);
	// An int literal is converted where the constructor takes a float, as written.
	const bytecode::Constant &placed = member_named(module, "placed").initial;
	REQUIRE(placed.args.size() == 2);
	REQUIRE(placed.args[1].args.size() == 2);
	CHECK((storage(module, placed.args[1].type) == ValueType::Vector2 && storage(module, placed.args[1].args[0].type) == ValueType::Float && placed.args[1].args[0].f == -1.0));
	// An array is typed as the member; an element of an untyped one keeps its type.
	const bytecode::Constant &items = member_named(module, "items").initial;
	CHECK((module.types[items.type].kind == bytecode::TypeKind::Array && storage(module, module.types[items.type].element) == ValueType::Int && items.args.size() == 2 && items.args[1].i == 2));
	const bytecode::Constant &nodes = member_named(module, "nodes").initial;
	CHECK((module.types[module.types[nodes.type].element].object.name == "Node" && nodes.args.empty()));
	const bytecode::Constant &plain = member_named(module, "plain").initial;
	REQUIRE(plain.args.size() == 2);
	CHECK((module.types[plain.type].kind == bytecode::TypeKind::Value && storage(module, plain.args[0].type) == ValueType::Int && storage(module, plain.args[1].type) == ValueType::String));

	const std::vector<uint8_t> data = bytecode::serialize(module);
	bytecode::Module copy;
	std::string error;
	REQUIRE(bytecode::deserialize(data.data(), data.size(), copy, error));
	CHECK(bytecode::serialize(copy) == data);
}

// Spec §4.3: a member without an initializer starts at its type's default,
// which is then its initial value; one the scene must assign has none (§7.5).
TEST_CASE("a member without an initializer starts at its type's default") {
	const bytecode::Module module = module_of("extends Node\n@export var count: int\n@export var nodes: Array[Node]\n@export var target: Node\nvar maybe: Node?\n");
	const bytecode::Member &count = member_named(module, "count");
	CHECK((count.has_initial && storage(module, count.initial.type) == ValueType::Int && count.initial.i == 0));
	const bytecode::Member &nodes = member_named(module, "nodes");
	CHECK((nodes.has_initial && module.types[module.types[nodes.initial.type].element].object.name == "Node" && nodes.initial.args.empty()));
	CHECK(!member_named(module, "target").has_initial);
	CHECK(member_named(module, "maybe").has_initial);
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
	compiler::ClassDecl cls = compiler::parse("extends Node\n\nfunc f() -> void:\n\tg()\n\tx = 1\n", errors);
	REQUIRE(errors.empty());
	for (const auto &function : cls.functions) {
		cls.all_functions.push_back(&function);
	}
	compiler::lower(cls, false, errors);
	REQUIRE(errors.size() == 2);
	CHECK(cls.source->position(errors[0].begin).line == 4);
	CHECK(errors[0].message.find("Internal compiler error") == 0);
	CHECK(cls.source->position(errors[1].begin).line == 5);
}

// Spec §10.6: the engine's integer types, which the runtime checks values
// against before they go to the engine.
TEST_CASE("the ranges of engine integers narrower than int64 or unsigned") {
	CHECK(bytecode::int_in_range(bytecode::IntRange::INT32, 2147483647));
	CHECK(!bytecode::int_in_range(bytecode::IntRange::INT32, 2147483648));
	CHECK(!bytecode::int_in_range(bytecode::IntRange::UINT32, -1));
	CHECK(bytecode::int_in_range(bytecode::IntRange::UINT32, 4294967295));
	CHECK(!bytecode::int_in_range(bytecode::IntRange::UINT64, -1));
	CHECK(!bytecode::int_in_range(bytecode::IntRange::INT8, 128));
}
