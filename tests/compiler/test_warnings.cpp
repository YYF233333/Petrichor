#include "doctest.h"
#include "helpers.h"

using namespace petrichor;
using namespace petrichor::tests;

// Warnings, hints and @warning_ignore (spec §2.3, §13.2).

namespace {

const std::string HIT = "extends Node\n\nstruct Hit:\n\tvar damage: int\n\tvar crit: bool = false\n\tvar pos: Vector2\n\tvar items: Array[int]\n\n";

// Each warning or hint as "line: name: message", the name empty for a hint.
std::vector<std::string> warnings_of(const std::string &p_source) {
	const compiler::CompileResult result = compile(p_source);
	INFO(p_source, messages(result));
	REQUIRE(result.ok());
	std::vector<std::string> warnings;
	for (const compiler::Diagnostic &warning : result.warnings) {
		CHECK(warning.severity == (warning.warning.empty() ? compiler::Severity::Hint : compiler::Severity::Warning));
		warnings.push_back(std::to_string(warning.line) + ": " + warning.warning + ": " + warning.message);
	}
	return warnings;
}

std::string in_loop(const std::string &p_body) {
	return HIT + "func f(hits: Array[Hit]):\n\tfor h in hits:\n" + p_body;
}

} // namespace

TEST_CASE("a change to a field of a struct loop variable that nothing reads modifies a copy") {
	const std::vector<std::string> copy = warnings_of(in_loop("\t\th.damage = 0\n"));
	REQUIRE(copy.size() == 1);
	CHECK(copy[0] == "11: modified_copy: This modifies a copy: \"h\" holds a copy of the element, and nothing reads it afterwards (spec §3.6).");
	// Through a field of a value type, also compound; a read before does not count.
	CHECK(warnings_of(in_loop("\t\th.pos.x = 1.0\n")).size() == 1);
	CHECK(warnings_of(in_loop("\t\tvar before := h.damage\n\t\th.damage += before\n")).size() == 1);
	CHECK(warnings_of(in_loop("\t\th.damage = 0\n\t\th.crit = true\n")).size() == 2);
	// A read afterwards, a container field, which is shared, or another variable.
	CHECK(warnings_of(in_loop("\t\th.damage = 0\n\t\tvar kept := h\n")).empty());
	CHECK(warnings_of(in_loop("\t\th.damage = 0\n\t\tif h.crit:\n\t\t\tpass\n")).empty());
	CHECK(warnings_of(in_loop("\t\th.items[0] = 1\n")).empty());
	CHECK(warnings_of(HIT + "func f(hit: Hit) -> Hit:\n\tvar h := hit\n\th.damage = 0\n\treturn hit\n").empty());
}

TEST_CASE("@warning_ignore reaches the head of the next statement or declaration, as in GDScript") {
	CHECK(warnings_of(in_loop("\t\t@warning_ignore(\"modified_copy\")\n\t\th.damage = 0\n")).empty());
	CHECK(warnings_of(in_loop("\t\t@warning_ignore(\"modified_copy\") h.damage = 0\n")).empty());
	// Not a function's body.
	const std::vector<std::string> function = warnings_of(HIT + "@warning_ignore(\"modified_copy\")\nfunc f(hits: Array[Hit]):\n\tfor h in hits:\n\t\th.damage = 0\n");
	REQUIRE(function.size() == 2);
	CHECK(function[0] == "9: : This ignores no \"modified_copy\" warning.");
	CHECK(contains(function[1], "12: modified_copy: "));
	// A region, to its restore or to the end of the file.
	CHECK(warnings_of(in_loop("\t\t@warning_ignore_start(\"modified_copy\")\n\t\th.damage = 0\n\t\t@warning_ignore_restore(\"modified_copy\")\n")).empty());
	CHECK(warnings_of(in_loop("\t\t@warning_ignore_start(\"modified_copy\")\n\t\th.damage = 0\n")).empty());
	CHECK(warnings_of(in_loop("\t\t@warning_ignore_start(\"modified_copy\")\n\t\t@warning_ignore_restore(\"modified_copy\")\n\t\th.damage = 0\n")).size() == 2);
}

TEST_CASE("a name of @warning_ignore that ignores nothing gets a hint") {
	const std::vector<std::string> gdscript = warnings_of("extends Node\n\n@warning_ignore(\"unused_parameter\")\nfunc f(x: int):\n\tpass\n");
	REQUIRE(gdscript.size() == 1);
	CHECK(gdscript[0] == "3: : \"unused_parameter\" is not a warning of this language, so this ignores nothing (spec §2.3).");
	// With syntax errors there are none.
	compiler::CompileResult broken = compiler::compile("extends Node\n\n@warning_ignore(\"unused_parameter\")\nfunc f(:\n", fake_engine());
	CHECK_FALSE(broken.ok());
	CHECK(broken.warnings.empty());
}
