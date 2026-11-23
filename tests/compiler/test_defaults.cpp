#include "helpers.h"
using namespace petrichor;
using namespace petrichor::tests;

TEST_CASE("defaults share member constant construction including nested arrays and options") {
	const std::string source = R"(extends Node
var member: Transform2D = Transform2D(0.5, Vector2(-1, 2))
var optional: Color? = Color.RED
var absent: Color? = None
func values(red: Color = Color(0.66, 0, 0, 0.66), transparent: Color = Color(1, 1, 1, 0.33), placed: Transform2D = Transform2D(0.5, Vector2(-1, 2)), up: Vector3 = Vector3.UP, node: Node? = None, none: Color? = None, color: Color? = Color.RED, points: Array[Vector2] = [Vector2(1, 2), Vector2.ZERO], nested: Array[Array] = [[1, 2]], nodes: Array[Node] = []):
	pass
func local_call():
	values()
)";
	FakeProject project;
	for (const bool declarations : { false, true }) {
		const auto result = compile_in(source, project, declarations);
		REQUIRE_MESSAGE(result.ok(), messages(result));
		const auto &defaults = function_named(result.module, "values").defaults;
		CHECK(defaults.size() == 10);
		const auto data = bytecode::serialize(result.module);
		bytecode::Module copy;
		std::string error;
		REQUIRE_MESSAGE(bytecode::deserialize(data.data(), data.size(), copy, error), error);
		CHECK(bytecode::serialize(copy) == data);
	}
}

// Constant expressions (spec §4.5), which the engine's method list shows.
TEST_CASE("constant defaults are listed with their values") {
	const auto result = compile("extends Node\nconst LIMIT = 10\nfunc f(value: int = LIMIT * 2 + 1, bytes: PackedByteArray = PackedByteArray(), mask: int = 1 << 4):\n\tpass\n");
	REQUIRE_MESSAGE(result.ok(), messages(result));
	const auto &defaults = function_named(result.module, "f").defaults;
	REQUIRE(defaults.size() == 3);
	CHECK(defaults[0].i == 21);
	CHECK(defaults[1].constructor >= 0);
	CHECK(defaults[2].i == 16);
}

// `speed := 5.0` (spec §4.6). The default may read a member or call a
// function declared later, as Pixelorama's `frame_index := current_frame`
// and gdUnit4's `_test_parameters := _parameters_typed()` do.
TEST_CASE("a parameter written := takes the type of its default value, which may read what is declared later") {
	const auto result = compile(R"(extends Node
func move(speed := 5.0, frame := current_frame, label := label_of(frame)) -> String:
	return label
func use() -> String:
	return move(2)
var current_frame := 0
func label_of(frame: int) -> String:
	return str(frame)
)");
	REQUIRE_MESSAGE(result.ok(), messages(result));
	const auto &move = function_named(result.module, "move");
	CHECK(storage(result.module, move.params[0].type) == bytecode::ValueType::Float);
	CHECK(storage(result.module, move.params[1].type) == bytecode::ValueType::Int);
	CHECK(errors_of("extends Node\nfunc f(none := null, empty := []) -> void:\n\tpass\n") == std::vector<std::string>{ "2:8: \"none\" needs a type: None does not say which T? it is.", "2:31: An empty array needs a declared type, for example var a: Array[int] = []." });
	CHECK(errors_of("extends Node\nfunc _process(delta := \"0\") -> void:\n\tpass\n") == std::vector<std::string>{ "2:24: The overridden method declares this parameter as float." });
	CHECK(errors_of("extends Node\nvar total := count()\nfunc count(from := total) -> int:\n\treturn from\n") == std::vector<std::string>{ "3:12: The type of parameter \"from\" is needed before its default value can give it; write the type, as in `from: T = value`." });
}
