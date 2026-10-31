#pragma once

#include "bytecode/module.h"
#include "runtime/text.h"

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <cstring>
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
	bytecode::ValueType option = bytecode::ValueType::Void; // Variant: None or a value of this type.
	bytecode::ValueType element = bytecode::ValueType::Void; // Array: typed with this element type.
};

// A type of a module's table as the runtime uses it (architecture §2.2).
struct TypeImage {
	bytecode::ValueType storage = bytecode::ValueType::Void; // How a slot holds it.
	ValueCheck check;
	std::string result_key; // The signature key of a Task[T] whose T it is.
};

std::vector<TypeImage> type_images(const std::vector<bytecode::TypeDesc> &p_types);

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
	void add(uint32_t p_offset, bytecode::ValueType p_type);
};

template <typename T>
SlotLayout SlotLayout::of(uint32_t p_size, const std::vector<T> &p_slots, const std::vector<TypeImage> &p_types) {
	SlotLayout layout;
	layout.size = p_size;
	for (const bytecode::Slot &slot : p_slots) {
		layout.add(slot.offset, p_types[slot.type].storage);
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

// None: NIL, or an OBJECT Variant that has no object, as the engine makes from
// a null pointer (core/variant/variant.cpp:2507-2511). A freed object is not
// None (spec §3.3).
inline bool is_none(const godot::Variant &p_value) {
	const godot::Variant::Type type = type_of(p_value);
	return type == godot::Variant::NIL || (type == godot::Variant::OBJECT && object_id_of(p_value) == 0);
}
// An Object slot that holds a live object; freed objects read as None (spec §3.3).
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
