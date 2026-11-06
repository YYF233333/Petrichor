#include "compiler/parser.h"
#include "compiler/warnings.h"
#include "doctest.h"
#include "helpers.h"

using namespace petrichor;
using namespace petrichor::tests;

// Warnings, hints and @warning_ignore (spec §2.3, §13.2).

namespace {

// Each warning or hint as "line: name: message".
std::vector<std::string> warnings_of(const std::string &p_source) {
	const compiler::CompileResult result = compile(p_source);
	INFO(p_source, messages(result));
	REQUIRE(result.ok());
	std::vector<std::string> warnings;
	for (const compiler::Diagnostic &warning : result.warnings) {
		CHECK(warning.severity == (warning.warning == "unused_ignore" ? compiler::Severity::Hint : compiler::Severity::Warning));
		warnings.push_back(std::to_string(warning.line) + ": " + warning.warning + ": " + warning.message);
	}
	return warnings;
}

// What the file's @warning_ignore annotations leave of a modified_copy
// warning at the start of line `p_line`, as "line: name" with their hints.
std::vector<std::string> left(const std::string &p_source, int p_line) {
	std::vector<compiler::Diagnostic> errors;
	const compiler::ClassDecl decl = compiler::parse(p_source, errors);
	INFO(p_source);
	REQUIRE(errors.empty());
	size_t at = 0;
	for (int line = 1; line < p_line; line++) {
		at = p_source.find('\n', at) + 1;
	}
	at = p_source.find_first_not_of(" \t", at);
	compiler::Diagnostic warning{ uint32_t(at), uint32_t(at + 1) };
	warning.severity = compiler::Severity::Warning;
	warning.warning = "modified_copy";
	std::vector<compiler::Diagnostic> diagnostics = { warning };
	compiler::apply_warning_ignores(decl, diagnostics);
	std::vector<std::string> result;
	for (const compiler::Diagnostic &diagnostic : diagnostics) {
		result.push_back(std::to_string(decl.source->position(diagnostic.begin).line) + ": " + diagnostic.warning);
	}
	return result;
}

// A loop's body starts on line 5.
const std::string LOOP = "extends Node\n\nfunc f(hits: Array[int]):\n\tfor h in hits:\n";

} // namespace

TEST_CASE("@warning_ignore reaches the head of the next statement or declaration, as in GDScript") {
	CHECK(left(LOOP + "\t\t@warning_ignore(\"modified_copy\")\n\t\tprint(h)\n", 6).empty());
	CHECK(left(LOOP + "\t\t@warning_ignore(\"modified_copy\") print(h)\n", 5).empty());
	// Not a function's body.
	CHECK(left("extends Node\n\n@warning_ignore(\"modified_copy\")\nfunc f(hits: Array[int]):\n\tfor h in hits:\n\t\tprint(h)\n", 6) == std::vector<std::string>{ "6: modified_copy", "3: unused_ignore" });
	// A region, to its restore or to the end of the file.
	CHECK(left(LOOP + "\t\t@warning_ignore_start(\"modified_copy\")\n\t\tprint(h)\n\t\t@warning_ignore_restore(\"modified_copy\")\n", 6).empty());
	CHECK(left(LOOP + "\t\t@warning_ignore_start(\"modified_copy\")\n\t\tprint(h)\n", 6).empty());
	CHECK(left(LOOP + "\t\t@warning_ignore_start(\"modified_copy\")\n\t\t@warning_ignore_restore(\"modified_copy\")\n\t\tprint(h)\n", 7) == std::vector<std::string>{ "7: modified_copy", "5: unused_ignore" });
	// Also from before the class header.
	CHECK(left("@warning_ignore_start(\"modified_copy\")\n" + LOOP + "\t\tprint(h)\n", 6).empty());
}

TEST_CASE("a name of @warning_ignore that ignores nothing gets a hint, which its own name ignores") {
	const std::string gdscript_ignore = "@warning_ignore(\"unused_parameter\")\nfunc f(x: int):\n\tpass\n";
	const std::vector<std::string> gdscript = warnings_of("extends Node\n\n" + gdscript_ignore);
	REQUIRE(gdscript.size() == 1);
	CHECK(gdscript[0] == "3: unused_ignore: \"unused_parameter\" is not a warning or hint of this language, so this ignores nothing (spec §2.3).");
	CHECK(warnings_of("extends Node\n\n@warning_ignore(\"modified_copy\")\nvar x := 1\n") == std::vector<std::string>{ "3: unused_ignore: This ignores no \"modified_copy\" warning." });
	// As the editor's [Ignore] writes it above.
	CHECK(warnings_of("extends Node\n\n@warning_ignore(\"unused_ignore\")\n" + gdscript_ignore).empty());
	CHECK(warnings_of("extends Node\n\n@warning_ignore(\"unused_ignore\")\nvar x := 1\n") == std::vector<std::string>{ "3: unused_ignore: This ignores no \"unused_ignore\" hint." });
	// With syntax errors there are none.
	compiler::CompileResult broken = compiler::compile("extends Node\n\n@warning_ignore(\"unused_parameter\")\nfunc f(:\n", fake_engine());
	CHECK_FALSE(broken.ok());
	CHECK(broken.warnings.empty());
}
