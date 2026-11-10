#pragma once

#include "bytecode/module.h"
#include "runtime/text.h"

#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace petrichor {

// How values of each slot type are made, copied, destroyed and exchanged with
// the engine (architecture §2.2).

// Fills the table of the engine's functions for each type; once the library is
// initialized, before any value is made.
void init_values();

// The engine's Variant: its type first, then 8-byte aligned data, which for an
// object is its ObjectID and pointer (core/variant/variant.h:167-262); the
// engine's VariantInternal reads it the same way (core/variant/variant_internal.h:225-228).
// All zero bytes are NIL. godot-cpp asks the engine for each of these.
inline godot::Variant::Type type_of(const godot::Variant &p_value) {
	return godot::Variant::Type(*reinterpret_cast<const int32_t *>(&p_value));
}
inline const uint8_t *data_of(const godot::Variant &p_value) {
	return reinterpret_cast<const uint8_t *>(&p_value) + 8;
}
inline uint64_t object_id_of(const godot::Variant &p_value) {
	return *reinterpret_cast<const uint64_t *>(data_of(p_value));
}
// The engine's pointer to the object, without the wrapper godot-cpp would
// look up for it.
inline GDExtensionObjectPtr object_pointer(const godot::Variant &p_value) {
	return *reinterpret_cast<const GDExtensionObjectPtr *>(data_of(p_value) + 8);
}
// Without the call through a MethodBind that godot-cpp's get_instance_id() makes.
inline uint64_t instance_id(const godot::Object *p_object) {
	return uint64_t(godot::gdextension_interface::object_get_instance_id(p_object->_owner));
}

void construct_value(bytecode::ValueType p_type, uint8_t *p_slot);
void destroy_value(bytecode::ValueType p_type, uint8_t *p_slot);

inline bool is_scalar(bytecode::ValueType p_type) {
	return p_type == bytecode::ValueType::Bool || p_type == bytecode::ValueType::Int || p_type == bytecode::ValueType::Float;
}
// A Variant of a scalar's slot, made in place as the engine lays it out,
// with the engine's number of the type; it owns nothing to destroy
// (core/variant/variant.h:268-272).
inline void make_scalar_variant(bytecode::ValueType p_type, const uint8_t *p_slot, uint8_t *r_variant) {
	std::memset(r_variant, 0, sizeof(godot::Variant));
	*reinterpret_cast<int32_t *>(r_variant) = int32_t(p_type);
	std::memcpy(r_variant + 8, p_slot, 8);
}
// A scalar's slot from a Variant as from_variant() fills it: an int also
// into a float. False when the Variant holds another type.
inline bool scalar_from_variant(bytecode::ValueType p_type, const godot::Variant &p_value, uint8_t *p_slot) {
	const godot::Variant::Type have = type_of(p_value);
	if (p_type == bytecode::ValueType::Bool && have == godot::Variant::BOOL) {
		// The 8-byte slot stays 0 or 1 above the byte the engine keeps.
		*reinterpret_cast<uint64_t *>(p_slot) = *data_of(p_value) ? 1 : 0;
		return true;
	}
	if (p_type == bytecode::ValueType::Float && have == godot::Variant::INT) {
		*reinterpret_cast<double *>(p_slot) = double(*reinterpret_cast<const int64_t *>(data_of(p_value)));
		return true;
	}
	if ((p_type == bytecode::ValueType::Int && have == godot::Variant::INT) || (p_type == bytecode::ValueType::Float && have == godot::Variant::FLOAT)) {
		std::memcpy(p_slot, data_of(p_value), 8);
		return true;
	}
	return false;
}

void copy_value(bytecode::ValueType p_type, uint8_t *p_dst, const uint8_t *p_src);
godot::Variant to_variant(bytecode::ValueType p_type, const uint8_t *p_slot);
// An element of a packed array, by an index within its size, in the layout of
// the element's slot, as the engine's pointer getter and setter take it
// (core/variant/variant_setget.cpp:348-354, 387-394, 421-428, 465-472).
void get_packed_element(bytecode::ValueType p_type, const uint8_t *p_array, int64_t p_index, uint8_t *p_element);
void set_packed_element(bytecode::ValueType p_type, uint8_t *p_array, int64_t p_index, const uint8_t *p_element);

// What an entry check asks of a value beyond how its slot holds it (spec §10.1).
struct ValueCheck {
	godot::StringName class_name; // Object: its class; Array: the class of object elements.
	godot::String script; // As class_name, for a class of this language: its script.
	godot::String interface; // As class_name, an interface the object implements: "script::Name".
	std::string task_result;
	bool nullable = true; // Object: may be None.
	bytecode::ValueType element = bytecode::ValueType::Void; // Array, Dictionary: typed with this element or value type.
	// Dictionary: typed with this key type, and as class_name, script and
	// interface say for the values, for keys of an object type.
	bytecode::ValueType key = bytecode::ValueType::Void;
	godot::StringName key_class_name;
	godot::String key_script;
	godot::String key_interface;
};

struct TypeImage;
struct BoxType;

struct FieldImage {
	godot::String name;
	const TypeImage *type = nullptr; // In the same table.
	uint32_t offset = 0;
};

// A type of a module's table as the runtime uses it (architecture §2.2): how
// its values are made, copied, destroyed and exchanged with the engine.
struct TypeImage {
	bytecode::TypeKind kind = bytecode::TypeKind::Value;
	// How a slot holds it in the engine's layout; Void for T?, whose slot is
	// an 8-byte tag, 1 for Some, then the value, which is there also for None,
	// and for a struct, whose slot holds its fields.
	bytecode::ValueType storage = bytecode::ValueType::Void;
	const TypeImage *element = nullptr; // T?: T, in the same table.
	// Array[T]: T; Dictionary[K, V]: K and V; in the same table.
	const TypeImage *item = nullptr;
	const TypeImage *key = nullptr;
	uint32_t size = 0; // Of its slot.
	uint32_t layout = 0; // Equal for types laid out alike, in every image.
	ValueCheck check;
	std::string result_key; // The signature key of a Task[T] whose T it is.
	godot::String name; // Struct and the enums: "script::Name"; an engine enum as the engine names it.
	std::vector<FieldImage> fields; // Struct and PayloadEnum, whose first field is the variant's index.
	std::vector<int64_t> enum_values; // Enum, sorted; PayloadEnum: each variant's first field.
	bool enum_checked = false; // An enum of this language, whose values entry checks check.
	// Enum: the names and values in declaration order; PayloadEnum: the names
	// of the variants and how many fields each has.
	std::vector<std::pair<godot::String, int64_t>> enum_declared;
	// A type that containers keep boxed (bytecode::is_boxed): its boxes' type,
	// the same for the same type in every image.
	const BoxType *box = nullptr;
};

// The images of a module's types; `element` points into the vector returned,
// which must not be copied.
std::vector<TypeImage> type_images(const std::vector<bytecode::TypeDesc> &p_types);

void construct_value(const TypeImage &p_type, uint8_t *p_slot);
void destroy_value(const TypeImage &p_type, uint8_t *p_slot);
// The type's default in the slot, releasing what it held (spec §6.8).
void clear_value(const TypeImage &p_type, uint8_t *p_slot);
void copy_value(const TypeImage &p_type, uint8_t *p_dst, const uint8_t *p_src);
// A struct is encoded (spec §3.6, §10.1); a container is itself, also one
// that keeps boxes.
godot::Variant to_variant(const TypeImage &p_type, const uint8_t *p_slot);
// Stores the value when it fits the type, and with `p_check` the type's
// entry check too, which looks at the elements of a container whose type the
// engine does not keep: boxes, or containers typed one level deep (spec §3.8).
// A box of exactly the type is opened. A struct is decoded from a Dictionary
// whose fields are there or have defaults, as `T.try_from` decodes the field
// (spec §3.6, §5.3), containers in it into new ones when they are not of the
// field's type.
bool from_variant(const TypeImage &p_type, const godot::Variant &p_value, uint8_t *p_slot, bool p_check);
// from_variant() with the entry check, for a value whose mismatch is a
// program error (spec §8.1): false with why in `r_mismatch`. That stays empty
// when a default computed for a field left out ended in a program error
// instead, which was reported (§3.6).
bool entry_check(const TypeImage &p_type, const godot::Variant &p_value, uint8_t *p_slot, godot::String &r_mismatch);
// `T.try_from(v)` into the slot of a Result[T, ConversionError] (spec §5.3):
// Ok of the value decoded as a struct's field of type T is (§3.6), or Err
// naming where it does not fit. False after a default's program error.
bool try_from(const TypeImage &p_result, const godot::Variant &p_value, uint8_t *p_slot);
// Whether an int is one of an enum's values (spec §3.7).
bool is_enum_value(const TypeImage &p_type, int64_t p_value);
// The value as spec §10.1 encodes it: a struct as a Dictionary, a container
// that keeps boxes as an untyped copy with its keys and elements encoded.
godot::Variant encode(const TypeImage &p_type, const uint8_t *p_slot);
// A variant of an enum with payloads as encode() gives it, from its fields
// encoded so.
godot::Variant encode_variant(const TypeImage &p_type, size_t p_variant, const std::vector<godot::Variant> &p_fields);

// Boxes (architecture §2.2): a container keeps a value of a boxed type in a
// Callable of this library that cannot be called, compared by the value as
// containers match keys (spec §5.2), and printed as the value. A key's box
// hashes the value when it is made.
godot::Variant make_box(const TypeImage &p_type, const uint8_t *p_value, bool p_key);
// The value in a box of exactly the type, or for Task[T]? also of Task[T]; or
// null.
const uint8_t *opened(const TypeImage &p_type, const godot::Variant &p_value);
// Whether the container keeps boxes, as its elements, keys or values.
bool keeps_boxes(const godot::Variant &p_container);
// Frees the types of boxes, when the library is unloaded.
void clear_box_types();
// The hash of a value as keys are hashed, boxes by their values (spec §3.6).
uint32_t content_hash(const godot::Variant &p_value);
// A copy with every container inside copied too, also inside boxes (spec
// §3.8): -1 as duplicate(true) copies, or the mode of duplicate_deep().
godot::Variant deep_copy(const godot::Variant &p_value, int64_t p_mode);
// `<` of a struct, field by field in order, of a T?, None first, or of a
// container that keeps boxes, element by element (spec §5.2).
bool less_values(const TypeImage &p_type, const uint8_t *p_a, const uint8_t *p_b);
// `<` of two keys or elements: boxes by their values, others by the engine's
// operator table, which takes a pair it cannot order as not less (spec §3.8).
bool variant_less(const godot::Variant &p_a, const godot::Variant &p_b);
// variant_less() as a Callable, for sort_custom() and bsearch_custom().
godot::Callable less_callable();
godot::String entry_mismatch(const TypeImage &p_type, const godot::Variant &p_value);
godot::Variant default_variant(const TypeImage &p_type);
// The type as the source writes it: "int?", "Array[Enemy]"…
godot::String type_description(const TypeImage &p_type);
// A value as a program error shows it: "Hit(damage = 3, crit = false)",
// "Command.IDLE", "Err(\"missing\")".
godot::String value_text(const TypeImage &p_type, const uint8_t *p_slot);
// What the engine sees: NIL for T?, Dictionary for a struct (spec §10.1).
godot::Variant::Type variant_type(const TypeImage &p_type);
// `==` of the type: of a struct, field by field (spec §3.6); of an object, by
// its identity (§5.2).
bool equal_values(const TypeImage &p_type, const uint8_t *p_a, const uint8_t *p_b);

// A value of a type outside any frame, such as a result on its way, made and
// destroyed by its type; on the C++ stack when it is small.
class ValueBuffer {
	const TypeImage &type;
	alignas(16) uint8_t small[80];
	std::unique_ptr<uint64_t[]> large;

public:
	uint8_t *const data;

	explicit ValueBuffer(const TypeImage &p_type) :
			type(p_type), large(p_type.size > sizeof(small) ? new uint64_t[p_type.size / 8 + 1] : nullptr), data(large ? reinterpret_cast<uint8_t *>(large.get()) : small) {
		construct_value(type, data);
	}
	~ValueBuffer() { destroy_value(type, data); }
	ValueBuffer(const ValueBuffer &) = delete;
	ValueBuffer &operator=(const ValueBuffer &) = delete;
};

// A value in its type's own layout outside any frame, such as the default of
// an engine method's parameter.
class NativeValue {
	const bytecode::ValueType type;
	std::unique_ptr<uint64_t[]> storage;

public:
	explicit NativeValue(bytecode::ValueType p_type) :
			type(p_type), storage(new uint64_t[bytecode::slot_size(p_type) / 8 + 1]) {
		construct_value(type, data());
	}
	~NativeValue() { destroy_value(type, data()); }
	NativeValue(const NativeValue &) = delete;
	NativeValue &operator=(const NativeValue &) = delete;
	uint8_t *data() const { return reinterpret_cast<uint8_t *>(storage.get()); }
};

// The slots of a frame or of an instance's members: zeroed as a whole, then
// those whose default value is not all zero bytes, such as Basis or Array, are
// constructed, and only those that own something are destroyed.
struct SlotLayout {
	uint32_t size = 0;
	std::vector<std::pair<uint32_t, bytecode::ValueType>> constructed; // Offset and type.
	std::vector<std::pair<uint32_t, bytecode::ValueType>> destroyed;
	std::vector<bytecode::Preset> presets; // A frame's constants.

	template <typename T>
	static SlotLayout of(uint32_t p_size, const std::vector<T> &p_slots, const std::vector<TypeImage> &p_types);
	void construct(uint8_t *p_base) const;
	void destroy(uint8_t *p_base) const;
	// Lets go of the references the slots hold to the RefCounted object with
	// the ObjectID, noting where; hold_again() puts the object back there.
	void let_go(uint8_t *p_base, uint64_t p_id, std::vector<std::pair<uint32_t, bytecode::ValueType>> &r_slots) const;
	static void hold_again(uint8_t *p_base, const godot::Variant &p_object, const std::vector<std::pair<uint32_t, bytecode::ValueType>> &p_slots);

private:
	void add(uint32_t p_offset, const TypeImage &p_type);
};

template <typename T>
SlotLayout SlotLayout::of(uint32_t p_size, const std::vector<T> &p_slots, const std::vector<TypeImage> &p_types) {
	SlotLayout layout;
	layout.size = p_size;
	for (const bytecode::Slot &slot : p_slots) {
		layout.add(slot.offset, p_types[slot.type]);
	}
	return layout;
}

// Entry check: stores the value when its type fits the slot and `p_check`, if
// given. A freed object is accepted as it is (spec §3.3).
bool from_variant(bytecode::ValueType p_type, const godot::Variant &p_value, uint8_t *p_slot, const ValueCheck *p_check = nullptr);
// Why a value failed the entry check, as its diagnostic says it (spec §8.4):
// "expected Array[int], found Array[float]", with classes of this language by
// their class_name.
godot::String entry_mismatch(bytecode::ValueType p_type, const godot::Variant &p_value, const ValueCheck &p_check);
godot::Variant default_variant(bytecode::ValueType p_type);
godot::Variant::Type variant_type(bytecode::ValueType p_type);
// The resource at a path, a script or another, as ResourceLoader loads it;
// null for an empty path. Made from the Ref that godot-cpp's load() returns,
// a Variant holding a script of this language released it once too often,
// and the next destruction of it crashed. An inner class's path, its file's
// followed by "::" and its names (spec §4.1), gives the class's script.
godot::Variant load_resource(const godot::String &p_path);
// The adapter knows the scripts of inner classes, which have no resource path
// (spec §4.1): the one named so in a script, and the path that names a script
// as the bytecode does, for an inner class its file's followed by its names.
using InnerScriptResolver = godot::Variant (*)(const godot::Variant &p_script, const godot::String &p_name);
using ScriptPathResolver = godot::String (*)(const godot::Ref<godot::Script> &p_script);
void set_inner_script_resolvers(InnerScriptResolver p_inner, ScriptPathResolver p_path);
godot::String script_path(const godot::Ref<godot::Script> &p_script);
// A new empty array, typed with the element type and the class of object
// elements, and for a class of this language with its script too (spec §3.8);
// untyped for Void.
godot::Array typed_array(bytecode::ValueType p_element, const godot::StringName &p_class, const godot::String &p_script);
// A new empty Dictionary, typed as the check says, or untyped (spec §3.8).
godot::Dictionary typed_dictionary(const ValueCheck &p_check);

// A Dictionary keeps a key as a copy in which every Array and Dictionary is
// read-only, so that the hash it keeps stays right; one this language
// obtains from a Dictionary made elsewhere is made read-only in place
// (spec §3.8). Other values are left as they are.
godot::Variant frozen_copy(const godot::Variant &p_key);
void freeze(const godot::Variant &p_key);
// Whether keys of the Dictionary may hold an Array or a Dictionary.
bool keys_hold_containers(const godot::Dictionary &p_dictionary);

// A value as diagnostics name it: by its type or class, "None", "a freed object".
godot::String describe_value(const godot::Variant &p_value);

// `v is T` of a built-in type: the Variant holds a value of exactly the type,
// and a typed container one typed exactly so, as GDScript's `is` asks
// (modules/gdscript/gdscript_vm.cpp:879-895; ed1daf0); any container is an
// untyped container's (spec §3.8, §5.4).
bool is_exactly(const TypeImage &p_type, const godot::Variant &p_value);

using bytecode::ElementRole;
// Why the value, in the role, does not fit the types the container has, as
// it would be assigned there; empty when it fits.
godot::String untyped_misfit(const godot::Variant &p_container, ElementRole p_role, const godot::Variant &p_value);

// None: NIL, or an OBJECT Variant that has no object, as the engine makes from
// a null pointer (core/variant/variant.cpp:2507-2511). A freed object is not
// None (spec §3.3).
inline bool is_none(const godot::Variant &p_value) {
	const godot::Variant::Type type = type_of(p_value);
	return type == godot::Variant::NIL || (type == godot::Variant::OBJECT && object_id_of(p_value) == 0);
}
// An object slot holds the data of a Variant with the object, its ObjectID and
// the engine's pointer, or zeros for None (architecture §2.2). The ObjectID's
// top bit says whether the object is RefCounted (core/object/object_id.h:45),
// and the slot then holds a reference to it, which Variants take and drop by
// the same bit (core/variant/variant.cpp:1074-1092).

// A Variant with the slot's object that takes no reference, for the engine to
// read while a call runs; it is never destroyed.
class ObjectView {
	alignas(godot::Variant) uint8_t bytes[sizeof(godot::Variant)] = {};

public:
	explicit ObjectView(const uint8_t *p_slot);
	godot::Variant &get() { return *reinterpret_cast<godot::Variant *>(bytes); }
};

// Holds the object of the Variant in the slot, or None for any other value.
void set_object(uint8_t *p_slot, const godot::Variant &p_value);
// Holds the object an engine method returned by its pointer, taking over the
// reference that came with a RefCounted result (core/variant/method_ptrcall.h:285-290),
// or taking one as a Variant would.
void take_returned_object(uint8_t *p_slot, void *p_object, bool p_with_reference);
bool is_none_object(const uint8_t *p_slot);
// An object slot that holds a live object; freed objects read as None (spec §3.3).
bool is_live_object(const uint8_t *p_slot);
// Whether two object slots hold the same object, by ObjectID, so that two
// freed objects are not one; a freed object in a T? reads as None (spec §5.2).
bool same_object(const uint8_t *p_a, bool p_a_optional, const uint8_t *p_b, bool p_b_optional);
// A class of a script is the script and those that extend it; with no script
// asked for, any will do.
bool has_script(const godot::Variant &p_script, const godot::String &p_path);
// Whether the script is the base script itself or extends it, by identity, so
// that a script without a path is only itself (spec §2.6).
bool extends_script(const godot::Variant &p_script, const godot::Variant &p_base);
// The value of a slot as it leaves this language: a T? whose object was
// freed reads as None (spec §3.3).
godot::Variant outward_variant(const TypeImage &p_type, const uint8_t *p_data);
inline godot::Variant &variant_at(uint8_t *p_slot) {
	return *reinterpret_cast<godot::Variant *>(p_slot);
}
inline const godot::Variant &variant_at(const uint8_t *p_slot) {
	return *reinterpret_cast<const godot::Variant *>(p_slot);
}

// Checks once that the engine's values fit the slot sizes the compiler assumed.
bool check_value_sizes(godot::String &r_error);

} // namespace petrichor
