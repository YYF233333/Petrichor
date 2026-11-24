#include "doctest.h"
#include "helpers.h"

using namespace petrichor;
using namespace petrichor::tests;

// Property accessors (spec §4.4).

TEST_CASE("a property's setter and getter, inline or named, take and give its type") {
	const auto result = compile(R"(extends Node
var x: int = 1:
	set(value):
		x = max(value, 0)
	get:
		return x * 2
var z := 2:
	set(value):
		z = value + 1
var y: int: set = set_y, get = get_y

func set_y(value: int):
	y = value

func get_y() -> int:
	return y

func f() -> int:
	x = -5
	x += 3
	z = 1
	return x + y
)");
	REQUIRE_MESSAGE(result.ok(), messages(result));
	for (const auto &member : result.module.members) {
		if (member.name == "x") {
			REQUIRE(member.setter >= 0);
			CHECK(result.module.functions[member.setter].name == "@set_x");
			CHECK(result.module.functions[member.getter].name == "@get_x");
		}
		if (member.name == "y") {
			CHECK(result.module.functions[member.setter].name == "set_y");
		}
	}
}

TEST_CASE("named accessors are functions of the class with the property's type") {
	const auto errors = [](const std::string &p_functions) {
		return messages(compile("extends Node\nvar y: int: set = set_y\n\n" + p_functions + "\n"));
	};
	CHECK(contains(errors("func set_y(value: String):\n\tpass"), "As the setter of \"y\", set_y() takes one argument of type int and returns nothing."));
	CHECK(contains(errors(""), "The setter set_y() is not a function of this class."));
	CHECK(contains(errors("static func set_y(value: int):\n\tpass"), "A setter runs on the object, so it is not static."));
	// A member made T? whose setter still takes T, as a migration leaves it.
	CHECK(fixed("extends Node\nvar y: Node?: set = set_y\n\nfunc set_y(value: Node):\n\ty = value\n", "Write Node?") == "extends Node\nvar y: Node?: set = set_y\n\nfunc set_y(value: Node?):\n\ty = value\n");
	CHECK(contains(messages(compile("extends Node\nvar y: int: get = get_y\n\nfunc get_y() -> String:\n\treturn \"\"\n")), "takes no argument and returns int"));
	CHECK(contains(messages(compile("extends Node\nvar y: int = 0:\n\tset(a):\n\t\tpass\n\tset(b):\n\t\tpass\n")), "The property already has a setter."));
}

// A setter may be a coroutine, which a write starts; a getter may not (spec
// §4.4, §9.1).
TEST_CASE("an async setter, inline or named, and no async getter") {
	FakeProject project;
	project.add_script("res://held.pet", "class_name Held extends Node\npub var z: int = 0:\n\tasync set(value):\n\t\tawait ready\n\t\tz = value\n", "Held");
	const auto result = compile_in(R"(extends Node
var x: int = 1:
	async set(value):
		await ready
		x = value
var y: int: set = set_y

async func set_y(value: int):
	await ready
	y = value

func write(held: Held) -> void:
	x = 2
	y += 1
	held.z = 3
)",
			project);
	REQUIRE_MESSAGE(result.ok(), messages(result));
	const std::string named = "extends Node\nvar y: int: set = set_y\n\nfunc set_y(value: int):\n\tawait ready\n";
	CHECK(fixed(named, "Write async func") == "extends Node\nvar y: int: set = set_y\n\nasync func set_y(value: int):\n\tawait ready\n");
	const std::string inline_setter = "extends Node\nvar y: int = 0:\n\tset(value):\n\t\tawait ready\n";
	CHECK(contains(messages(compile(inline_setter)), "await requires an async function"));
	CHECK(fixed(inline_setter, "Write async set") == "extends Node\nvar y: int = 0:\n\tasync set(value):\n\t\tawait ready\n");
	CHECK(contains(messages(compile("extends Node\nvar y: int = 0:\n\tasync get:\n\t\treturn 1\n")), "A getter is not async: reading the member does not wait for it."));
	CHECK(fixed("extends Node\nvar y: int = 0:\n\tget:\n\t\tawait ready\n\t\treturn 1\n", "Write async set") == "no such fix");
}
