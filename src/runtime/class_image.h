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
class ClassImage : public std::enable_shared_from_this<ClassImage> {
public:
	bytecode::Module module;
	std::shared_ptr<const ClassImage> base;
	// A function of the base has the same index in every class that extends it
	// (load() checks it), so a call within a class finds an override by index;
	// an inherited function runs the code of the class that declares it.
	struct FunctionOrigin {
		std::shared_ptr<const ClassImage> image; // Null for a function declared here.
		int index = -1;
	};
	std::vector<FunctionOrigin> function_origins;
	static void resolve_function(std::shared_ptr<const ClassImage> &r_image, int &r_index);
	bool has_signature(const godot::StringName &p_name, uint32_t p_signature) const;
	godot::String path;
	godot::StringName source; // The path as the debugger names breakpoints.
	godot::StringName global_name;
	godot::StringName base_class;

	std::vector<TypeImage> types; // By the module's type index.
	std::vector<godot::StringName> function_names;
	godot::HashMap<godot::StringName, int> function_lookup;
	std::vector<SlotLayout> frames; // By function index.
	int user_init = -1; // The class's own _init(), run after the member initializers.
	// _notification(what: int) and _to_string() -> String, which the engine
	// reaches through the instance rather than by calling them; -1 when absent.
	int notification_function = -1;
	int to_string_function = -1;
	// "_ready", which runs the @onready initializers first (spec §6.3); kept
	// here because godot-cpp values cannot be globals.
	godot::StringName ready_method = "_ready";
	std::vector<godot::StringName> member_names;
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
	// What a method of Dictionary does with keys (spec §3.8): set() and
	// get_or_add() may insert their first argument, keys() and find_key()
	// hand keys out.
	enum class KeyRole : uint8_t {
		None,
		Inserts,
		HandsOut,
	};
	std::vector<KeyRole> key_roles; // By built-in method index.
	// A method of Array or Dictionary that, on a container that keeps boxes,
	// the runtime carries out itself, or finishes, since the engine would hash
	// the boxes or leave them shared (architecture §2.2).
	enum class Replaced : uint8_t {
		None,
		Hash,
		HasAll, // Its keys may be boxes of elements, which are not hashed.
		Duplicate, // With deep, the boxes are copied after the engine.
		DuplicateDeep,
		Slice,
		// The engine has no `<` of boxes (core/variant/variant_op.cpp:731-791):
		// its own algorithm with this language's `<` (spec §3.8).
		Sort,
		Bsearch,
		Min,
		Max,
	};
	std::vector<Replaced> replaced; // By built-in method index.
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
	static std::shared_ptr<const ClassImage> load(bytecode::Module &&p_module, const godot::String &p_path, godot::String &r_error, const std::shared_ptr<const ClassImage> &p_base = {});

	int find_function(const godot::StringName &p_name) const;
	int find_member(const godot::StringName &p_name) const;

	// Whether the two differ only in function bodies, so that instances of
	// `p_other` can switch to this image (spec §12.4).
	bool same_declarations(const ClassImage &p_other) const;
};

// The value of a constant of the module whose types `p_types` are; None for a
// constant of an object type or of T?. Returns false and sets r_error when the
// engine has no such constant or constructor.
bool constant_value(const std::vector<TypeImage> &p_types, const bytecode::Constant &p_constant, godot::Variant &r_value, godot::String &r_error);

} // namespace petrichor
