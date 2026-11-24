#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace petrichor::compiler {

enum class Severity {
	Error,
	Warning,
	Hint,
};

// A change to a file that names the text it replaces, which whoever applies it
// checks first (architecture §2.1).
struct CheckedEdit {
	uint32_t begin = 0;
	uint32_t end = 0;
	std::string old_text;
	std::string new_text;
};

// What a quick fix (spec §13.2) does, which decides whether it is automatic:
// it keeps what the code meant in GDScript, comes first, needs no input,
// and fixing all applies the fixes of one such kind (architecture §2.1).
enum class FixKind {
	TruthTest,
	FloatArguments, // Where a float is expected.
	BothLevels, // Of a T?? or a Variant?, as GDScript's null stands for both.
	InstanceValid,
	Detach,
	Pub,
	MemberItself, // The method or signal itself for its literal name.
	Autoload, // Its name for a literal path from the root to it.
	PackedArray, // The conversion of an Array where a packed array is expected.
	ArgumentField, // The field for a literal index of a signal's arguments.
	// The ones offered for the user to choose.
	FloatResult, // float arguments where the result's type or value then changes.
	Unwrap,
	Optional,
	ReturnNone, // Where a function that may return None returns nothing.
	Async,
	Await,
	NullPattern,
	Receiver, // self.f() for a method that a direct call f() of a utility function's name misses.
	GetNodeOrNull, // For $X that a scene lacks and another has.
	Cast, // `as` where a value of a class goes as a subclass, which GDScript checks as the program runs.
};

// What fixing all of a kind in a file is called: "Fix all '…' in file".
struct FixKindInfo {
	bool automatic;
	const char *name;
};
inline constexpr FixKindInfo FIX_KINDS[] = {
	{ true, "Write the truth test" },
	{ true, "Write the int arguments as float" },
	{ true, "Test both levels for null" },
	{ true, "Write is_instance_valid()" },
	{ true, "Write .detach()" },
	{ true, "Make it pub" },
	{ true, "Use the method or signal itself" },
	{ true, "Reach the autoload by its name" },
	{ true, "Convert to the packed array" },
	{ true, "Read the argument by its name" },
	{ false, "Write the int arguments as float" },
	{ false, "Write .unwrap()" },
	{ false, "Make the type T?" },
	{ false, "Write return None" },
	{ false, "Write async func" },
	{ false, "Write await" },
	{ false, "Match both levels of null" },
	{ false, "Write the receiver" },
	{ false, "Write get_node_or_null()" },
	{ false, "Write as" },
};
static_assert(sizeof(FIX_KINDS) / sizeof(FIX_KINDS[0]) == size_t(FixKind::Cast) + 1);

// A quick fix of a diagnostic, named by `title`.
struct Fix {
	std::string title;
	FixKind kind;
	struct Edit {
		std::string file; // The script it changes; empty for the one of the diagnostic's range.
		CheckedEdit change;
	};
	std::vector<Edit> edits;

	bool automatic() const { return FIX_KINDS[size_t(kind)].automatic; }
};

// A compile error, a warning or a hint (spec §1.4), over a byte range of the
// source. compile() adds the line and column of its start, from 1, columns
// in code points.
struct Diagnostic {
	uint32_t begin = 0;
	uint32_t end = 0;
	std::string message;
	int line = 0;
	int column = 0;
	Severity severity = Severity::Error;
	std::string warning; // A warning's or a hint's name, which @warning_ignore takes (spec §2.3).
	// An error in how a scene uses the script is also marked on the node of
	// that scene that has it (spec §7, §13.2), by its path from the root.
	std::string scene;
	std::string node;
	// The script whose source its range is in, when a scene's check reports it
	// (architecture §6.4); empty for the script compiled.
	std::string file;
	// An error of a scene's check after which the game still runs, as a check
	// at run time reports it too: a value that a scene lacks (spec §7.5).
	// Exports and the headless build fail on it.
	bool runs = false;
	std::vector<Fix> fixes; // The automatic one first.
};

} // namespace petrichor::compiler
