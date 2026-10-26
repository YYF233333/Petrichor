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
	// By function index: its frame, the entry checks of its parameters, and the
	// values of the parameters a caller from the engine may leave out.
	std::vector<SlotLayout> frames;
	std::vector<std::vector<ValueCheck>> param_checks;
	std::vector<std::vector<godot::Variant>> param_defaults;
	int user_init = -1; // The class's own _init(), run after the member initializers.
	// _notification(what: int) and _to_string() -> String, which the engine
	// reaches through the instance rather than by calling them; -1 when absent.
	int notification_function = -1;
	int to_string_function = -1;
	// "_ready", which runs the @onready initializers first (spec §6.3); kept
	// here because godot-cpp values cannot be globals.
	godot::StringName ready_method = "_ready";
	std::vector<godot::StringName> member_names;
	std::vector<ValueCheck> member_checks; // Entry checks of members, by member index.
	SlotLayout members_layout; // An instance's storage.
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
	std::vector<godot::StringName> classes; // Engine classes, by class index.
	std::vector<godot::String> class_scripts; // Classes of this language: their scripts.
	std::vector<godot::String> class_interfaces; // Interfaces: "script::Name".
	// By method call index: the method's name, and the signature the call was
	// compiled against; by function index, each function's own. Equal numbers
	// are equal signatures in every image.
	std::vector<godot::StringName> method_names;
	std::vector<uint32_t> call_signatures;
	std::vector<uint32_t> function_signatures;

	// Whether a live object is of the class at `p_index`: its engine class,
	// and its script or the interface it implements.
	bool fits_class(const godot::Object *p_object, uint32_t p_index) const;

	// Returns null and sets r_error when the module refers to something this
	// runtime or this engine cannot provide.
	static std::shared_ptr<const ClassImage> load(bytecode::Module &&p_module, const godot::String &p_path, godot::String &r_error);

	int find_function(const godot::StringName &p_name) const;
	int find_member(const godot::StringName &p_name) const;

	// Whether the two differ only in function bodies, so that instances of
	// `p_other` can switch to this image (spec §12.4).
	bool same_declarations(const ClassImage &p_other) const;
};

// A literal of the module, such as a parameter's default value; None for an
// Object or Variant constant.
godot::Variant literal_value(const bytecode::Constant &p_constant);

} // namespace petrichor
