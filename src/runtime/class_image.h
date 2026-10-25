#pragma once

#include "bytecode/module.h"

#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <memory>
#include <vector>

namespace petrichor {

// The engine utility functions the compiler may call; the adapter offers the
// same list to the compiler through its EngineApi.
enum class Utility {
	Print,
	Prints,
	Printerr,
	PushError,
	PushWarning,
};

const std::vector<std::pair<const char *, Utility>> &utility_functions();

// A loaded module: immutable, shared by the script, its instances and every
// running frame, so that a hot reload can replace it while old frames finish
// with the old code (spec §12.4).
class ClassImage {
public:
	bytecode::Module module;
	godot::String path;
	godot::StringName global_name;
	godot::StringName base_class;

	std::vector<godot::StringName> function_names;
	godot::HashMap<godot::StringName, int> function_lookup;
	std::vector<godot::StringName> member_names;
	std::vector<bytecode::Slot> member_slots; // For constructing and destroying instance storage.
	godot::HashMap<godot::StringName, int> member_lookup;

	std::vector<godot::String> strings; // String constants, by constant index.
	std::vector<godot::StringName> engine_methods;
	std::vector<Utility> utilities;

	// Returns null and sets r_error when the module refers to something this
	// runtime cannot provide.
	static std::shared_ptr<const ClassImage> load(bytecode::Module &&p_module, const godot::String &p_path, godot::String &r_error);

	int find_function(const godot::StringName &p_name) const;
	int find_member(const godot::StringName &p_name) const;

	// Whether instances of `p_other` can keep their members under this image.
	bool same_layout(const ClassImage &p_other) const;
};

/* Values in frames and instances */

// Default-constructs the slots of a frame or an instance (strings empty,
// numbers zero) and destroys them.
void construct_slots(uint8_t *p_base, uint32_t p_size, const std::vector<bytecode::Slot> &p_slots);
void destroy_slots(uint8_t *p_base, const std::vector<bytecode::Slot> &p_slots);

void copy_value(bytecode::ValueType p_type, uint8_t *p_dst, const uint8_t *p_src);
godot::Variant to_variant(bytecode::ValueType p_type, const uint8_t *p_slot);
// Entry check (spec §10.1): stores the value when its type fits.
bool from_variant(bytecode::ValueType p_type, const godot::Variant &p_value, uint8_t *p_slot);
godot::Variant default_variant(bytecode::ValueType p_type);
godot::Variant::Type variant_type(bytecode::ValueType p_type);

} // namespace petrichor
