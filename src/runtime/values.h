#pragma once

#include "bytecode/module.h"
#include "runtime/text.h"

#include <godot_cpp/variant/array.hpp>
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

void construct_value(bytecode::ValueType p_type, uint8_t *p_slot);
void destroy_value(bytecode::ValueType p_type, uint8_t *p_slot);

void copy_value(bytecode::ValueType p_type, uint8_t *p_dst, const uint8_t *p_src);
godot::Variant to_variant(bytecode::ValueType p_type, const uint8_t *p_slot);

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
	godot::String name; // Struct: "script::Name".
	std::vector<FieldImage> fields; // Struct.
	// A type that containers keep boxed (bytecode::is_boxed): its boxes' type,
	// the same for the same type in every image.
	const BoxType *box = nullptr;
};

// The images of a module's types; `element` points into the vector returned,
// which must not be copied.
std::vector<TypeImage> type_images(const std::vector<bytecode::TypeDesc> &p_types);

void construct_value(const TypeImage &p_type, uint8_t *p_slot);
void destroy_value(const TypeImage &p_type, uint8_t *p_slot);
void copy_value(const TypeImage &p_type, uint8_t *p_dst, const uint8_t *p_src);
// A struct is encoded (spec §3.6, §10.1); a container is itself, also one
// that keeps boxes.
godot::Variant to_variant(const TypeImage &p_type, const uint8_t *p_slot);
// Stores the value when it fits the type, and with `p_check` the type's
// entry check too, which looks at the elements of a container whose type the
// engine does not keep: boxes, or containers typed one level deep (spec §3.8).
// A box of exactly the type is opened. A struct is decoded from a Dictionary
// that has each field, as `T.try_from` decodes the field (spec §3.6, §5.3),
// containers in it into new ones when they are not of the field's type.
bool from_variant(const TypeImage &p_type, const godot::Variant &p_value, uint8_t *p_slot, bool p_check);
// The value as spec §10.1 encodes it: a struct as a Dictionary, a container
// that keeps boxes as an untyped copy with its keys and elements encoded.
godot::Variant encode(const TypeImage &p_type, const uint8_t *p_slot);

// Boxes (architecture §2.2): a container keeps a value of a boxed type in a
// Callable of this library that cannot be called, compared by the value as
// containers match keys (spec §5.2), and printed as the value. A key's box
// hashes the value when it is made.
godot::Variant make_box(const TypeImage &p_type, const uint8_t *p_value, bool p_key);
// The value in a box of exactly the type, or null.
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
godot::String entry_mismatch(const TypeImage &p_type, const godot::Variant &p_value);
godot::Variant default_variant(const TypeImage &p_type);
// The type as the source writes it: "int?", "Array[Enemy]"…
godot::String type_description(const TypeImage &p_type);
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

// The slots of a frame or of an instance's members: zeroed as a whole, then
// those whose default value is not all zero bytes, such as Basis or Array, are
// constructed, and only those that own something are destroyed.
struct SlotLayout {
	uint32_t size = 0;
	std::vector<std::pair<uint32_t, bytecode::ValueType>> constructed; // Offset and type.
	std::vector<std::pair<uint32_t, bytecode::ValueType>> destroyed;

	template <typename T>
	static SlotLayout of(uint32_t p_size, const std::vector<T> &p_slots, const std::vector<TypeImage> &p_types);
	void construct(uint8_t *p_base) const;
	void destroy(uint8_t *p_base) const;

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
bool is_none_object(const uint8_t *p_slot);
// An object slot that holds a live object; freed objects read as None (spec §3.3).
bool is_live_object(const uint8_t *p_slot);
// A class of a script is the script and those that extend it; with no script
// asked for, any will do.
bool has_script(const godot::Variant &p_script, const godot::String &p_path);
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
