#include "compiler/formatter.h"
#include "conventions.h"
#include "doctest.h"

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace petrichor;

namespace {

std::string formatted(const std::string &p_source) {
	std::string error;
	const std::optional<std::string> result = compiler::format(p_source, &error);
	REQUIRE_MESSAGE(result, error);
	CHECK_MESSAGE(compiler::format(*result) == result, "formatting again changes:\n", *result);
	return *result;
}

} // namespace

// Spec §13.2.
TEST_CASE("formatting indents with tabs and spaces operators, blank lines and comments") {
	const std::string source = "extends CharacterBody3D\n"
							   "var speed : float=5.0\n"
							   "const LIMITS := {\"min\" : 0, \"max\" : 10} # range\n"
							   "func move(delta:float)->void:\n"
							   "    var dir := Vector3( 1,0,0 )\n"
							   "    if dir.x>0: position.x+=speed*delta\n"
							   "\n"
							   "\n"
							   "    print(position,\n"
							   "          delta)\n"
							   "func _ready() -> void:\n"
							   "    move(0.1)\n";
	CHECK(formatted(source) == "extends CharacterBody3D\n"
							   "var speed: float = 5.0\n"
							   "const LIMITS := {\"min\": 0, \"max\": 10}  # range\n"
							   "\n"
							   "\n"
							   "func move(delta: float) -> void:\n"
							   "\tvar dir := Vector3(1, 0, 0)\n"
							   "\tif dir.x > 0: position.x += speed * delta\n"
							   "\n"
							   "\tprint(position,\n"
							   "\t\tdelta)\n"
							   "\n"
							   "\n"
							   "func _ready() -> void:\n"
							   "\tmove(0.1)\n");
}

TEST_CASE("a continuation line is one level deeper than its bracket's line or its statement's first line") {
	CHECK(formatted("func f() -> void:\n"
					"\tvar a := [\n"
					"\t\t\t1,\n"
					"\t2, [3,\n"
					"  4],\n"
					"\t\t]\n"
					"\tvar b := 1 + \\\n"
					"2 + \\\n"
					"\t\t\t3\n") == "func f() -> void:\n"
									"\tvar a := [\n"
									"\t\t1,\n"
									"\t\t2, [3,\n"
									"\t\t\t4],\n"
									"\t]\n"
									"\tvar b := 1 + \\\n"
									"\t\t2 + \\\n"
									"\t\t3\n");
}

TEST_CASE("the block of a lambda inside brackets is one level deeper than its header's line") {
	CHECK(formatted("func f() -> void:\n"
					"\tvar g := func():\n"
					"\t\t\tpass\n"
					"\tconnect(\"a\", func():\n"
					"\t\t\t\tif true:\n"
					"\t\t\t\t\tprint(1)\n"
					"\t\t\t\tprint(2)\n"
					"\t)\n"
					"\tcall(1,\n"
					"\t\t\tfunc():\n"
					"\t\t\t\t\tprint(3),\n"
					"\t\t\t2)\n"
					"\tcall(func():\n"
					"\t\tprint(4)\n"
					"\t, 5)\n") == "func f() -> void:\n"
								   "\tvar g := func():\n"
								   "\t\tpass\n"
								   "\tconnect(\"a\", func():\n"
								   "\t\tif true:\n"
								   "\t\t\tprint(1)\n"
								   "\t\tprint(2)\n"
								   "\t)\n"
								   "\tcall(1,\n"
								   "\t\tfunc():\n"
								   "\t\t\tprint(3),\n"
								   "\t\t2)\n"
								   "\tcall(func():\n"
								   "\t\tprint(4)\n"
								   "\t, 5)\n");
}

TEST_CASE("formatting spaces binary operators but not unary ones, calls, subscripts, members or node paths") {
	CHECK(formatted("var a:int=-1\n"
					"var b := {\"k\" : [ 1,2, ], \"m\":-a,}\n"
					"var c = $Head / Camera.position+%Name . position+$\"../Level\".position\n"
					"@export_range(0,10) var r := 1\n"
					"func g(x : int=3, cb : (func(int) -> bool)? = null)->int :\n"
					"\tassert (x>0 && x!=1);var s := r\n"
					"\tsuper .g(x)\n"
					"\treturn x%2+b [\"k\"] [0] if not x else g (- x) ** -2 - - x\n") ==
			"var a: int = -1\n"
			"var b := {\"k\": [1, 2,], \"m\": -a,}\n"
			"var c = $Head/Camera.position + %Name.position + $\"../Level\".position\n"
			"@export_range(0, 10) var r := 1\n"
			"\n"
			"\n"
			"func g(x: int = 3, cb: (func(int) -> bool)? = null) -> int:\n"
			"\tassert(x > 0 && x != 1); var s := r\n"
			"\tsuper.g(x)\n"
			"\treturn x % 2 + b[\"k\"][0] if not x else g(-x) ** -2 - -x\n");
}

TEST_CASE("top-level functions and classes have two blank lines around them, those in inner classes one") {
	CHECK(formatted("\n\nextends Node\n"
					"var a := 1\n"
					"func f() -> void:\n"
					"\tpass\n"
					"# About g.\n"
					"@rpc\n"
					"func g() -> void:\n"
					"\n"
					"\n"
					"\tpass\n"
					"\n"
					"\n"
					"\n"
					"\n"
					"var b := 2\n"
					"class Inner:\n"
					"\n"
					"\tvar x := 1\n"
					"\tfunc h() -> void:\n"
					"\t\tpass\n"
					"\n"
					"\n"
					"\tfunc i() -> void: pass\n"
					"\tvar y := 2\n"
					"interface Damageable:\n"
					"\tfunc take(d: int)\n"
					"\tfunc heal(h: int)\n\n\n") == "extends Node\n"
													"var a := 1\n"
													"\n"
													"\n"
													"func f() -> void:\n"
													"\tpass\n"
													"\n"
													"\n"
													"# About g.\n"
													"@rpc\n"
													"func g() -> void:\n"
													"\n"
													"\tpass\n"
													"\n"
													"\n"
													"var b := 2\n"
													"\n"
													"\n"
													"class Inner:\n"
													"\n"
													"\tvar x := 1\n"
													"\n"
													"\tfunc h() -> void:\n"
													"\t\tpass\n"
													"\n"
													"\tfunc i() -> void: pass\n"
													"\n"
													"\tvar y := 2\n"
													"\n"
													"\n"
													"interface Damageable:\n"
													"\tfunc take(d: int)\n"
													"\tfunc heal(h: int)\n");
}

TEST_CASE("a comment line goes with its block, by its column where blocks end") {
	CHECK(formatted("func f() -> void:\n"
					"\tif true:\n"
					"\t\tpass\n"
					"\t\t# Inner.\n"
					"\t# Outer.\n"
					"# Left of f's block.\n"
					"\tvar a := [\n"
					"# First.\n"
					"\t1,\n"
					"\t\t\t# Last.\n"
					"]\n"
					"\t# End of f.   \n"
					"# Between.\n"
					"\n"
					"func g() -> void:\n"
					"#\tprint(\"commented out\")\n"
					"\tpass\n") == "func f() -> void:\n"
								   "\tif true:\n"
								   "\t\tpass\n"
								   "\t\t# Inner.\n"
								   "\t# Outer.\n"
								   "\t# Left of f's block.\n"
								   "\tvar a := [\n"
								   "\t\t# First.\n"
								   "\t\t1,\n"
								   "\t\t# Last.\n"
								   "\t]\n"
								   "\t# End of f.\n"
								   "\n"
								   "\n"
								   "# Between.\n"
								   "\n"
								   "\n"
								   "func g() -> void:\n"
								   "\t#\tprint(\"commented out\")\n"
								   "\tpass\n");
}

// editor/gui/code_editor.cpp:1356-1371 (ed1daf0): the editor comments lines
// out after their indentation, and empty ones at the start.
TEST_CASE("comment lines keep the levels they have beyond their block, as code commented out in the editor does") {
	CHECK(formatted("extends Node\n"
					"#var a := [\n"
					"\t#1,\n"
					"#]\n"
					"func h() -> void:\n"
					"\tpass\n"
					"\t#if true:\n"
					"\t\t#print(1)\n"
					"#\n"
					"\t#print(2)\n"
					"var after := 1\n") == "extends Node\n"
										   "\n"
										   "\n"
										   "#var a := [\n"
										   "\t#1,\n"
										   "#]\n"
										   "func h() -> void:\n"
										   "\tpass\n"
										   "\t#if true:\n"
										   "\t\t#print(1)\n"
										   "\t#\n"
										   "\t#print(2)\n"
										   "\n"
										   "\n"
										   "var after := 1\n");
	CHECK(formatted("func f() -> void:\n"
					"    pass\n"
					"            # Two levels deeper.\n") == "func f() -> void:\n"
															 "\tpass\n"
															 "\t\t\t# Two levels deeper.\n");
}

TEST_CASE("formatting keeps line breaks, literals and the file's line breaks, and leaves files with syntax errors") {
	const std::string long_line = "var hit := space.intersect_ray(PhysicsRayQueryParameters3D.create(global_position, target, collision_mask, [self]))\n";
	CHECK(formatted(long_line) == long_line);
	const std::string kept = "func f(x: int) -> void:\n"
							 "\tif x == 0xFFFF: return\n"
							 "\telse: if x == 1_000: print('a', \"\"\"b\n"
							 "    c\"\"\", &\"d\", ^\"e\", r\"\\f\", 1e3)\n"
							 "\tvar y := (x + 1) * 2 if x || x and not x else -(x)\n";
	CHECK(formatted(kept) == kept);
	CHECK(formatted("var a=1\r\nvar b=2\r\n") == "var a = 1\r\nvar b = 2\r\n");
	CHECK(formatted("") == "");
	CHECK(formatted("# Only a comment") == "# Only a comment\n");
	std::string error;
	CHECK_FALSE(compiler::format("func f(:\n\tpass\n", &error));
	CHECK(error == "The file has syntax errors.");
}

// Architecture §2.1, §5: real code. Run from the repository's root.
TEST_CASE("formatting the scripts of the tests and benchmarks keeps their code and changes nothing the second time") {
	size_t count = 0;
	for (const char *root : { "tests/engine", "benchmarks/project" }) {
		for (const auto &entry : std::filesystem::recursive_directory_iterator(root)) {
			if (entry.path().extension() != ".pet") {
				continue;
			}
			std::ifstream file(entry.path(), std::ios::binary);
			std::stringstream text;
			text << file.rdbuf();
			INFO(entry.path().string());
			tests::check_formatting(text.str());
			count++;
		}
	}
	CHECK(count > 90);
}
