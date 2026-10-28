#pragma once

#include "bytecode/opcodes.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace petrichor::compiler::builtin {

// The engine's built-in types (Vector3, String, Array…) and utility functions
// (lerp, print…), as godot-cpp's extension_api-4-7.json describes them; the
// tables are generated at build time (builtin_api_gen.py). ClassDB does not
// describe these, and the engine offers an extension no way to list them, so
// the editor uses the same tables as the tests (architecture §2.1). Types are
// the JSON's names: "float", "Vector3", "Variant", "enum::Vector3.Axis"…; an
// empty name is void.

struct Argument {
	const char *name;
	const char *type;
	bool has_default;
	// As GDScript source, such as `true`, `""` or `Vector3(0, 1, 0)`.
	const char *default_value;
};

struct Method {
	const char *name;
	const char *return_type;
	uint32_t hash;
	const Argument *arguments;
	uint32_t argument_count;
	bool is_vararg;
	bool is_static;
	bool is_const;
};

struct Operator {
	const char *name; // "+", "==", "unary-", "not"…
	const char *right_type; // Empty for a unary operator.
	const char *return_type;
};

struct Member {
	const char *name;
	const char *type;
};

struct Constant {
	const char *name;
	const char *type;
};

// Constructors in the engine's order: the position is the index the runtime
// asks the engine for.
struct Constructor {
	const Argument *arguments;
	uint32_t argument_count;
};

struct Class {
	const char *name;
	const Member *members;
	uint32_t member_count;
	const Constant *constants;
	uint32_t constant_count;
	const Operator *operators;
	uint32_t operator_count;
	const Method *methods;
	uint32_t method_count;
	const Constructor *constructors;
	uint32_t constructor_count;
};

struct Utility {
	const char *name;
	const char *return_type;
	uint32_t hash;
	const Argument *arguments;
	uint32_t argument_count;
	bool is_vararg;
};

const Class *find_class(std::string_view p_name);
const Utility *find_utility(std::string_view p_name);
const Member *find_member(const Class &p_class, std::string_view p_name);
const Constant *find_constant(const Class &p_class, std::string_view p_name);
const Method *find_method(const Class &p_class, std::string_view p_name);

// Engine API whose objects are never None (spec §10.2): the engine's
// RequiredResult and RequiredParam metadata, and the audit in
// binding_metadata.txt (§10.3). Classes are those that declare the member.
bool returns_non_null(std::string_view p_class, std::string_view p_method);
bool property_non_null(std::string_view p_class, std::string_view p_property);
bool param_non_null(std::string_view p_class, std::string_view p_method, uint32_t p_index);
// The parameter that names the class of the objects the method's array holds
// (spec §10.3), or -1.
int class_filter_param(std::string_view p_class, std::string_view p_method);

// An entry of the generated metadata that has a value.
struct Keyed {
	const char *key; // "Class.method:index" or "Class.property".
	const char *value;
};

// The range of an engine integer parameter, or of the value a property's
// setter takes, when it is narrower than int64 or unsigned (spec §10.6).
std::optional<bytecode::IntRange> param_int_range(std::string_view p_class, std::string_view p_method, uint32_t p_index);
std::optional<bytecode::IntRange> property_int_range(std::string_view p_class, std::string_view p_property);

} // namespace petrichor::compiler::builtin
