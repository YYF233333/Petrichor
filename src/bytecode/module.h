#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace petrichor::bytecode {

// The compiled form of one source file: the contract between the compiler and
// the runtime. It has no version number: a file is read only by builds whose
// definition of the format, the files of this directory, is the same
// (FORMAT_ID, which SConstruct generates). The meaning of each instruction's
// operands is written in opcodes.h, so that a change to it changes the id.

// How a value is stored in a frame or an instance. Up to Variant the numbers
// are the engine's Variant::Type, so the runtime can hand them to the engine's
// functions for each type; values are stored in the engine's own layout, except
// Bool, which takes 8 bytes like Int and Float, and Object, which is held in a
// Variant for now (architecture §2.2 plans an object ID and a pointer). Variant
// is a slot that holds any engine value.
enum class ValueType : uint8_t {
	Void,
	Bool,
	Int,
	Float,
	String,
	Vector2,
	Vector2i,
	Rect2,
	Rect2i,
	Vector3,
	Vector3i,
	Transform2D,
	Vector4,
	Vector4i,
	Plane,
	Quaternion,
	AABB,
	Basis,
	Transform3D,
	Projection,
	Color,
	StringName,
	NodePath,
	RID,
	Object,
	Callable,
	Signal,
	Dictionary,
	Array,
	PackedByteArray,
	PackedInt32Array,
	PackedInt64Array,
	PackedFloat32Array,
	PackedFloat64Array,
	PackedStringArray,
	PackedVector2Array,
	PackedVector3Array,
	PackedColorArray,
	PackedVector4Array,
	Variant,
	Count,
};

// The bytes a slot of the type takes in a frame or an instance, for 64-bit
// targets with single-precision `real_t` (the engine's float_64 sizes). Slots
// are 8-byte aligned.
uint32_t value_size(ValueType p_type);
uint32_t slot_size(ValueType p_type);
const char *value_type_name(ValueType p_type);
// ValueType::Count when the name is not a value type.
ValueType value_type_from_name(const std::string &p_name);

// A class that code constructs, tests or casts to: an engine class, a class
// of this language (spec §11.1), or an interface (§3.9).
struct ClassRef {
	std::string name; // The engine class its objects have.
	std::string script; // A class of this language: its script; an interface: the script that declares it.
	std::string interface_name; // An interface: its name in that script.

	bool operator==(const ClassRef &p_other) const { return name == p_other.name && script == p_other.script && interface_name == p_other.interface_name; }
};

// What a type is (architecture §2.2). A module lists each type that its slots,
// members, signatures, constants and instructions use once, and they refer to
// it by its index; a type inside another comes before it.
enum class TypeKind : uint8_t {
	Value, // An engine value type in its own layout, or Variant: `value`.
	Object, // An object of the class `object`, or also None when `nullable` (spec §3.3).
	Option, // T? of a value type: None or a value of the type `element` (spec §3.4).
	Array, // Array[T], T being the type `element` (spec §3.8); an untyped Array is a Value.
	Count,
};

struct TypeDesc {
	TypeKind kind = TypeKind::Value;
	ValueType value = ValueType::Void;
	uint32_t element = 0;
	ClassRef object;
	bool nullable = false;
	std::string task_result; // Object: a Task[T] handle, with T's signature key.

	bool operator==(const TypeDesc &p_other) const {
		return kind == p_other.kind && value == p_other.value && element == p_other.element && object == p_other.object && nullable == p_other.nullable && task_result == p_other.task_result;
	}
};

// How a slot of the type holds it in the engine's layout: an object in a
// Variant for now. T? has no such layout: its slot is an 8-byte tag, 1 for
// Some, then the value, which is there also for None.
ValueType storage_of(const std::vector<TypeDesc> &p_types, uint32_t p_type);
uint32_t type_size(const std::vector<TypeDesc> &p_types, uint32_t p_type);
// The type as text, equal exactly for equal types in any module.
std::string type_key(const std::vector<TypeDesc> &p_types, uint32_t p_type);
// How a slot of the type is laid out, as text equal for types whose values can
// be copied from one slot to the other as they are.
std::string layout_key(const std::vector<TypeDesc> &p_types, uint32_t p_type);

// A constant of a type of the table; one of an Object type or of T? is None.
struct Constant {
	uint32_t type = 0;
	bool b = false;
	int64_t i = 0;
	double f = 0.0;
	std::string s; // String, StringName and NodePath values.
	std::string builtin; // A constant of a built-in type, such as "UP" of Vector3.
	// A value built by a constructor of the type, by its index in the engine's
	// list, from `args`, which have the types the constructor takes; -1 for
	// none.
	int32_t constructor = -1;
	// The constructor's arguments, or the elements of an Array, typed as its type says.
	std::vector<Constant> args;
};

struct Slot {
	std::string name; // Empty for temporaries.
	uint32_t type = 0; // In the module's type table.
	uint32_t offset = 0;
};

// A slot of the instance, and what the class says of it.
struct Member : Slot {
	uint32_t line = 0; // Where it is declared, for diagnostics.
	bool is_pub = false;
	bool is_export = false;
	bool is_onready = false; // Set by the ready function (spec §6.3).
	// The functions that reading and writing it from outside the class call
	// (spec §4.4); -1 without one.
	int32_t setter = -1;
	int32_t getter = -1;
	// For the inspector: PropertyHint as the engine numbers it, and its string.
	uint32_t hint = 0;
	std::string hint_string;
	// The value the member starts at, when it is a constant: a constant
	// initializer, or its type's default without one (spec §4.3). The editor
	// shows it as the default; the module's init function writes it, with the
	// other initializers.
	bool has_initial = false;
	Constant initial;
};

struct Function {
	bool is_inherited = false;
	std::string base_signature;
	bool is_async = false;
	std::string name;
	std::vector<Slot> params;
	// The values of the last parameters when a call leaves them out: one from
	// the engine, or from another class of this language (CALL_METHOD). A call
	// within the class passes every argument.
	std::vector<Constant> defaults;
	bool is_static = false;
	// The return value's type, Void when there is none; name and offset mean
	// nothing.
	Slot result;
	bool is_pub = false;
	bool is_override = false; // Overrides an engine virtual method.
	// The engine must find it by name: a method connected in a scene (spec §10.5).
	bool is_exposed = false;
	uint32_t line = 0;
	uint32_t frame_size = 0;
	std::vector<Slot> slots; // All slots with their types, params first.
	std::vector<uint32_t> code;
};

// A signal the class declares (spec §4.7); the engine finds it by name.
struct SignalInfo {
	std::string name;
	std::vector<Slot> params; // Types and names; the offsets mean nothing.
	bool is_pub = false;
};

// `@export_category`, `@export_group` or `@export_subgroup`: a heading the
// inspector shows before a member.
struct ExportGroup {
	std::string name;
	std::string prefix;
	uint8_t level = 1; // 0: category, 1: group, 2: subgroup.
	uint32_t before_member = 0;
};

// A method of an engine class, called by name.
struct EngineCall {
	std::string method;
	uint32_t result_type = 0; // The slot that receives the result, by type.
};

// A method of a class of this language, called on an object of that class or
// of an interface the class implements (spec §3.9, §11.1), with the signature
// the caller was compiled against. The runtime calls the method only while it
// has exactly this signature, with arguments and result in their slots' own
// layout (spec §8.1).
struct MethodCall {
	bool is_async = false;
	std::string method;
	std::vector<Slot> params; // Names and offsets mean nothing.
	uint32_t defaults = 0; // How many of the last parameters have default values, which a call may leave out.
	Slot result; // As Function::result.
};

// A signature of a method as text that is equal exactly for equal signatures
// in any module: the parameters' and the result's types (`p_types` being the
// table they refer to), and how many parameters have defaults.
std::string signature_key(const std::vector<TypeDesc> &p_types, const std::vector<Slot> &p_params, size_t p_defaults, const Slot &p_result, bool p_async = false);
// The signature key of a Task[T] handle's T.
std::string task_result_key(const std::vector<TypeDesc> &p_types, uint32_t p_type);

// A property of an engine object, read and written by name.
struct PropertyAccess {
	std::string name;
	uint32_t type = 0; // The slot it is read into or written from, by type.
};

// A global utility function of the engine, such as print or lerp. Parameters
// of type Variant, and extra arguments of a vararg function, are passed as
// Variant; others in the slot's own layout.
struct UtilityCall {
	std::string function;
	uint32_t hash = 0;
	bool is_vararg = false;
	std::vector<ValueType> params;
	ValueType return_type = ValueType::Void; // As the engine declares it.
	uint32_t result_type = 0; // The slot that receives it, by type.
};

// An operator of the engine's table (spec §5.1), as Variant::Operator.
struct OperatorCall {
	uint8_t op = 0;
	ValueType left = ValueType::Void;
	ValueType right = ValueType::Void; // Void for a unary operator.
	ValueType result = ValueType::Void;
};

// A member of a built-in type, such as x of Vector3.
struct FieldAccess {
	ValueType base = ValueType::Void;
	std::string name;
	ValueType type = ValueType::Void;
};

// A constructor of a built-in type, by its index in the engine's list.
struct ConstructorCall {
	ValueType type = ValueType::Void;
	uint32_t index = 0;
	std::vector<ValueType> params;
};

// A method of a built-in type, such as lerp of Vector3.
struct BuiltinMethodCall {
	ValueType base = ValueType::Void;
	std::string method;
	uint32_t hash = 0;
	bool is_vararg = false; // Extra arguments are passed as Variant.
	std::vector<ValueType> params;
	ValueType return_type = ValueType::Void;
	uint32_t result_type = 0; // The slot that receives it, by type.
};

// How many entries of each table there were once the init function was
// lowered, before the other functions: the entries it can use.
struct TablePrefix {
	uint32_t types = 0;
	uint32_t constants = 0;
	uint32_t engine_calls = 0;
	uint32_t properties = 0;
	uint32_t utility_calls = 0;
	uint32_t operators = 0;
	uint32_t fields = 0;
	uint32_t constructors = 0;
	uint32_t builtin_methods = 0;
	uint32_t classes = 0;
	uint32_t method_calls = 0;
};

struct Module {
	uint64_t source_hash = 0;
	// Compiled without function bodies, to describe the class while it is
	// being loaded in the editor; it cannot run.
	bool declarations_only = false;
	std::string global_name;
	std::string base_class;
	std::string base_script;
	uint32_t inherited_members = 0;
	std::vector<TypeDesc> types;
	std::vector<Member> members;
	std::vector<SignalInfo> signals;
	std::vector<ExportGroup> groups;
	uint32_t instance_size = 0;
	std::vector<Function> functions;
	// Runs the member initializers in declaration order, before anything else
	// on a new instance (spec §6.1); -1 when there are none. It is lowered
	// first, so that its code and the table entries it uses, which hot reload
	// compares as declarations, do not change when only a function body does.
	int32_t init_function = -1;
	// Runs when the node is ready, before its _ready(): checks the exports the
	// scene must assign (spec §7.5) and runs the @onready initializers (§6.3);
	// -1 when there is nothing to do. Lowered right after the init function,
	// and compared with it as declarations.
	int32_t ready_function = -1;
	TablePrefix init_tables; // What the two use.
	std::vector<Constant> constants;
	std::vector<EngineCall> engine_calls;
	std::vector<PropertyAccess> properties;
	std::vector<UtilityCall> utility_calls;
	std::vector<OperatorCall> operators;
	std::vector<FieldAccess> fields;
	std::vector<ConstructorCall> constructors;
	std::vector<BuiltinMethodCall> builtin_methods;
	std::vector<ClassRef> classes;
	std::vector<MethodCall> method_calls;
	// The interfaces the class implements, as "script::Name" (spec §3.9).
	std::vector<std::string> interfaces;
};

uint64_t hash_source(const std::string &p_utf8);

std::vector<uint8_t> serialize(const Module &p_module);
// Returns false and sets r_error when the data is not a module of this format,
// or does not pass verify().
bool deserialize(const uint8_t *p_data, size_t p_size, Module &r_module, std::string &r_error);

// Checks the module's own structure, so that a damaged or outdated cache file
// is refused instead of run: instructions are whole, jumps land on
// instructions, code cannot run past its end, operands name slots of the
// types their instructions use, slots fit their frames without overlapping,
// and table indices are in range. The engine signatures recorded in the tables
// (operators, fields, constructors, methods, utility functions) are trusted:
// the compiler takes them from the engine's API, and the engine finds methods
// and utility functions by name and hash. It does not defend against a file
// crafted to lie about them.
bool verify(const Module &p_module, std::string &r_error);

// A readable listing, for tests and debugging.
std::string disassemble(const Module &p_module, const Function &p_function);

} // namespace petrichor::bytecode
