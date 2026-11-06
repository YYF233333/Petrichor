#pragma once

#include "bytecode/module.h"
#include "runtime/values.h"

#include <godot_cpp/godot.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace petrichor {

// The static variables of a class (spec §4.8): made with its image, and
// handed on to the next image of the class by a hot reload that changes only
// function bodies, so that they keep their values.
struct Statics {
	enum class State : uint8_t {
		Uninitialized,
		Initializing,
		Initialized,
		Failed, // Its initialization ended in a program error.
	};
	std::atomic<State> state{ State::Uninitialized };
	std::thread::id thread; // That initializes it.
	// While it initializes, the static variables before this one that have
	// initializers have run them.
	uint32_t next = 0;
	SlotLayout layout;
	std::unique_ptr<uint64_t[]> data;

	explicit Statics(const SlotLayout &p_layout);
	~Statics();
	Statics(const Statics &) = delete;
	Statics &operator=(const Statics &) = delete;
	uint8_t *at(uint32_t p_offset) { return reinterpret_cast<uint8_t *>(data.get()) + p_offset; }
};

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
	int user_init = -1; // The nearest _init(), run after the member initializers (spec §6.1).
	// _notification(what: int) and _to_string() -> String, which the engine
	// reaches through the instance rather than by calling them; -1 when absent.
	int notification_function = -1;
	int to_string_function = -1;
	// "_ready", which runs the @onready initializers first (spec §6.3); kept
	// here because godot-cpp values cannot be globals.
	godot::StringName ready_method = "_ready";
	godot::StringName call_method = "call"; // Of Callable, for CALL_VALUE.
	std::vector<godot::StringName> member_names;
	SlotLayout members_layout; // An instance's storage.
	godot::HashMap<godot::StringName, int> member_lookup;
	// Set when the image loads; a hot reload hands it on (adopt_statics()).
	mutable std::shared_ptr<Statics> statics;
	godot::HashMap<godot::StringName, int> static_lookup;

	std::vector<godot::String> strings; // String constants, by constant index.
	std::vector<godot::Variant> values; // Other constants, by constant index.
	std::vector<godot::StringName> engine_methods;
	std::vector<godot::StringName> engine_static_classes; // Empty for a method called on an object.
	std::vector<godot::StringName> properties;
	std::vector<GDExtensionPtrUtilityFunction> utilities;
	std::vector<GDExtensionPtrOperatorEvaluator> evaluators;
	std::vector<GDExtensionPtrGetter> getters;
	std::vector<GDExtensionPtrSetter> setters;
	std::vector<GDExtensionPtrConstructor> constructors;
	std::vector<GDExtensionPtrBuiltInMethod> builtin_methods;
	using KeyRole = bytecode::KeyRole;
	using Replaced = bytecode::Replaced;
	std::vector<const bytecode::ContainerMethod *> container_methods; // By built-in method index.
	std::vector<godot::StringName> classes; // Engine classes, by class index.
	std::vector<godot::String> class_scripts; // Classes of this language: their scripts.
	std::vector<godot::String> class_interfaces; // Interfaces: "script::Name".
	// By method call index: the method's name, and the signature the call was
	// compiled against; by function index, each function's own. Equal numbers
	// are equal signatures in every image.
	std::vector<godot::StringName> method_names;
	std::vector<uint32_t> call_signatures;
	std::vector<uint32_t> function_signatures;

	// The images of the inner classes, in declaration order (spec §4.1), each
	// at its path: this one's followed by "::" and its name.
	std::vector<std::shared_ptr<const ClassImage>> inner_classes;

	// Whether a live object is of the class at `p_index`: its engine class,
	// and its script or the interface it implements.
	bool fits_class(const godot::Object *p_object, uint32_t p_index) const;

	// Of a member that must be assigned, in an instance's storage: not zero
	// once it has been (bytecode/module.h).
	uint64_t &assigned(uint8_t *p_members, size_t p_member) const {
		const bytecode::Member &member = module.members[p_member];
		return *reinterpret_cast<uint64_t *>(p_members + member.offset + types[member.type].size);
	}

	// The base of an inner class that is not of its file.
	using BaseLoader = std::function<std::shared_ptr<const ClassImage>(const godot::String &p_path)>;

	// Returns null and sets r_error when the module refers to something this
	// runtime or this engine cannot provide. The images of its inner classes
	// are loaded with it.
	static std::shared_ptr<const ClassImage> load(bytecode::Module &&p_module, const godot::String &p_path, godot::String &r_error, const std::shared_ptr<const ClassImage> &p_base = {}, const BaseLoader &p_bases = {});
	// This class or an inner class of it by its path; null when there is none.
	std::shared_ptr<const ClassImage> find_class(const godot::String &p_path) const;

	int find_function(const godot::StringName &p_name) const;
	int find_member(const godot::StringName &p_name) const;
	int find_static(const godot::StringName &p_name) const;
	// This image and its inner classes take the static variables of
	// `p_previous`, an image of the same class with the same declarations.
	void adopt_statics(const ClassImage &p_previous) const;

	// Whether the two differ only in function bodies, so that instances of
	// `p_other` can switch to this image (spec §12.4).
	bool same_declarations(const ClassImage &p_other) const;

private:
	static std::shared_ptr<ClassImage> load_class(bytecode::Module &&p_module, const godot::String &p_path, godot::String &r_error, const std::shared_ptr<const ClassImage> &p_base);
};

// The value of a constant of the image's module, computed now, containers
// new and writable; None for a constant of an object type or of T?, unless it
// loads a resource. Returns false and sets r_error when the engine has no such
// constant, constructor, operator or function.
bool constant_value(const ClassImage &p_image, const bytecode::Constant &p_constant, godot::Variant &r_value, godot::String &r_error);

// The value of a pub constant the class declares or inherits (spec §4.5), as
// the engine and GDScript read it by name (§10.5, §11.3); false if there is none.
bool pub_constant(const ClassImage &p_image, const godot::StringName &p_name, godot::Variant &r_value);

} // namespace petrichor
