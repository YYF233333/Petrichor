#pragma once

#include "bytecode/opcodes.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
	bool is_math; // Filed under math, which constant expressions may call (spec §4.5).
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

// A parameter that names a member of an object (spec §10.4).
struct MemberName {
	enum class Kind {
		Method,
		Property,
		Signal,
	};
	uint32_t index;
	int target; // The argument that holds the object, or -1 for the receiver.
	Kind kind;
	bool query; // Whether the member exists is what the method asks.
	int value; // The argument the property is given, or -1.
	bool path; // A NodePath, whose subnames after ":" index the property.
};

struct MemberNameEntry {
	const char *method; // "Class.method".
	MemberName name;
};

std::optional<MemberName> member_name_param(std::string_view p_class, std::string_view p_method);

// An entry of the generated metadata that has a value.
struct Keyed {
	const char *key; // "Class.method:index" or "Class.property".
	const char *value;
};

// The range of an engine integer parameter, or of the value a property's
// setter takes, when it is narrower than int64 or unsigned (spec §10.6).
std::optional<bytecode::IntRange> param_int_range(std::string_view p_class, std::string_view p_method, uint32_t p_index);
std::optional<bytecode::IntRange> property_int_range(std::string_view p_class, std::string_view p_property);

// Of `try_m`, a try_ version of the static method m of an engine class (spec
// §10.3): the static getter of the Error that m sets; null for another name.
const char *error_getter(std::string_view p_class, std::string_view p_name);
// The engine method that `try_m` calls, m, and the reverse.
std::string_view tried_method(std::string_view p_name);
std::string try_version(std::string_view p_method);

struct KeyedIndex {
	const char *key; // "Class.property".
	int32_t index;
};

// The index that an engine property of a family passes its getter and setter
// first, such as a BaseMaterial3D texture, or -1; the class is the one that
// declares it.
int property_index(std::string_view p_class, std::string_view p_property);

struct GlobalConstant {
	const char *name;
	int64_t value;
	int32_t enum_index; // In ENUMS, or -1 for a constant of no enum.
};

// A value of the engine's global enums, such as KEY_ESCAPE, or one of its
// global constants, such as INT32_MAX, which GDScript has in scope.
std::optional<int64_t> global_constant(std::string_view p_name);

struct EnumValue {
	const char *name;
	int64_t value;
};

struct Enum {
	const char *name; // "Key", "Variant.Type", "Vector3.Axis".
	bool is_bitfield;
	const EnumValue *values;
	uint32_t value_count;
};

// One of the engine's global enums, or an enum of a built-in type; null if none.
const Enum *find_enum(std::string_view p_name);
// The global enum a global constant is a value of, such as Key of KEY_ESCAPE; null for another constant.
const Enum *global_constant_enum(std::string_view p_name);

// For completion (spec §13.2): the built-in types and the global enums, such
// as Vector3 and Key, and the utility functions and the global constants.
void global_names(std::vector<std::string> &r_types, std::vector<std::string> &r_values);

} // namespace petrichor::compiler::builtin
