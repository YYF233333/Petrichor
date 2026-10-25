#pragma once

#include "bytecode/module.h"

#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <vector>

namespace petrichor {

// How values of each slot type are made, copied, destroyed and exchanged with
// the engine (architecture §2.2). Built-in values are kept in the engine's own
// layout and handled by the engine's functions for their type; Object slots
// hold a Variant for now.

void construct_value(bytecode::ValueType p_type, uint8_t *p_slot);
void destroy_value(bytecode::ValueType p_type, uint8_t *p_slot);

// Default-constructs the slots of a frame or an instance (numbers zero,
// strings empty, Basis the identity, objects None) and destroys them.
void construct_slots(uint8_t *p_base, uint32_t p_size, const std::vector<bytecode::Slot> &p_slots);
void destroy_slots(uint8_t *p_base, const std::vector<bytecode::Slot> &p_slots);

void copy_value(bytecode::ValueType p_type, uint8_t *p_dst, const uint8_t *p_src);
godot::Variant to_variant(bytecode::ValueType p_type, const uint8_t *p_slot);
// Entry check (spec §10.1): stores the value when its type fits. An object
// must also have `p_class`, when one is given, and may be None only when
// `p_nullable`; a freed object is accepted as it is (spec §3.3).
bool from_variant(bytecode::ValueType p_type, const godot::Variant &p_value, uint8_t *p_slot, const godot::StringName &p_class = godot::StringName(), bool p_nullable = true);
godot::Variant default_variant(bytecode::ValueType p_type);
godot::Variant::Type variant_type(bytecode::ValueType p_type);

// None: NIL, or an OBJECT Variant that has no object, as the engine makes from
// a null pointer. A freed object is not None (spec §3.3).
bool is_none(const godot::Variant &p_value);
// An Object slot that holds a live object; freed objects read as None (spec §3.3).
bool is_live_object(const uint8_t *p_slot);
inline godot::Variant &variant_at(uint8_t *p_slot) {
	return *reinterpret_cast<godot::Variant *>(p_slot);
}
inline const godot::Variant &variant_at(const uint8_t *p_slot) {
	return *reinterpret_cast<const godot::Variant *>(p_slot);
}

// Checks once that the engine's values fit the slot sizes the compiler assumed.
bool check_value_sizes(godot::String &r_error);

} // namespace petrichor
