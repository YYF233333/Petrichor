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
	CHECK(contains(errors("func set_y(value: String):\n\tpass"), "As the setter of \"y\", set_y() takes one argument of type int and returns nothing (spec §4.4)."));
	CHECK(contains(errors(""), "The setter set_y() is not a function of this class."));
	CHECK(contains(errors("async func set_y(value: int):\n\tpass"), "A setter cannot be async or static (spec §4.4)."));
	CHECK(contains(messages(compile("extends Node\nvar y: int = 0:\n\tset(a):\n\t\tpass\n\tset(b):\n\t\tpass\n")), "The property already has a setter."));
	CHECK(contains(messages(compile("extends Node\nvar y: int: get = get_y\n\nfunc get_y() -> String:\n\treturn \"\"\n")), "takes no argument and returns int"));
}
