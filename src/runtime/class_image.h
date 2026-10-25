#pragma once

#include "bytecode/module.h"
#include "runtime/values.h"

#include <godot_cpp/godot.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <memory>
#include <vector>

namespace petrichor {

// A loaded module: immutable, shared by the script, its instances and every
// running frame, so that a hot reload can replace it while old frames finish
// with the old code (spec §12.4). Loading resolves the module's tables into
// the engine's functions.
class ClassImage {
public:
	bytecode::Module module;
	godot::String path;
	godot::StringName source; // The path as the debugger names breakpoints.
	godot::StringName global_name;
	godot::StringName base_class;

	std::vector<godot::StringName> function_names;
	godot::HashMap<godot::StringName, int> function_lookup;
	int user_init = -1; // The class's own _init(), run after the member initializers.
	std::vector<godot::StringName> member_names;
	std::vector<godot::StringName> member_classes; // Entry checks of Object members.
	std::vector<bytecode::Slot> member_slots; // For constructing and destroying instance storage.
	godot::HashMap<godot::StringName, int> member_lookup;

	std::vector<godot::String> strings; // String constants, by constant index.
	std::vector<godot::Variant> values; // Other constants, by constant index.
	std::vector<godot::StringName> engine_methods;
	std::vector<godot::StringName> properties;
	std::vector<GDExtensionPtrUtilityFunction> utilities;
	std::vector<GDExtensionPtrOperatorEvaluator> evaluators;
	std::vector<GDExtensionPtrGetter> getters;
	std::vector<GDExtensionPtrSetter> setters;
	std::vector<GDExtensionPtrConstructor> constructors;
	std::vector<GDExtensionPtrBuiltInMethod> builtin_methods;
	std::vector<godot::StringName> classes;

	// Returns null and sets r_error when the module refers to something this
	// runtime or this engine cannot provide.
	static std::shared_ptr<const ClassImage> load(bytecode::Module &&p_module, const godot::String &p_path, godot::String &r_error);

	int find_function(const godot::StringName &p_name) const;
	int find_member(const godot::StringName &p_name) const;

	// Whether the two differ only in function bodies, so that instances of
	// `p_other` can switch to this image (spec §12.4).
	bool same_declarations(const ClassImage &p_other) const;
};

} // namespace petrichor
