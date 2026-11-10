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
		std::string error;
		CHECK_MESSAGE(bytecode::verify(result.module, error), error);
		const auto &defaults = function_named(result.module, "values").defaults;
		CHECK(defaults.size() == 10);
		const auto data = bytecode::serialize(result.module);
		bytecode::Module copy;
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
