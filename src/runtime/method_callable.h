#pragma once

#include "runtime/vm.h"

#include <godot_cpp/variant/callable_custom.hpp>

#include <functional>

namespace petrichor {

// The script instance of this language on an object: its image and members.
struct ScriptedObject {
	std::shared_ptr<const ClassImage> image;
	InstanceContext context;
};

// The adapter finds the script instance on an object, since only it knows how
// instances are stored; false when the object has none of this language.
using InstanceResolver = bool (*)(godot::Object *p_object, ScriptedObject &r_instance);
void set_instance_resolver(InstanceResolver p_resolver);
bool scripted_object(godot::Object *p_object, ScriptedObject &r_instance);

// Likewise the current image of a script of this language by its path, which
// a static method is called on (spec §4.8); null when it has none.
using ScriptResolver = std::shared_ptr<const ClassImage> (*)(const godot::String &p_path);
void set_script_resolver(ScriptResolver p_resolver);
std::shared_ptr<const ClassImage> script_image(const godot::String &p_path);
// A file whose script is loading on this thread, which script_image() finds
// before the loader hands the script to the engine: its static
// initialization runs then (spec §4.8). Nested loads end in reverse order.
void begin_loading(const std::shared_ptr<const ClassImage> &p_image);
void end_loading();

// Whether the object's script of this language implements the interface,
// named "script::Name" (spec §3.9).
bool implements_interface(godot::Object *p_object, const godot::String &p_interface);

// A call of a function value from this language (spec §8.1), which the engine
// passes on, also through bind(): the first call of a lambda or a method value
// that the engine makes for it is its callee, whose program error aborts the
// caller too. Per thread; the caller keeps the one it may be inside of.
struct ValueCall {
	bool pending = false; // Made, and no callee has run yet.
	bool entered = false;
	bool failed = false;
};
ValueCall &value_call();

// A method value (spec §3.11): a Callable that calls a function of the script
// instance on an object directly, whether or not reflection shows the function
// (spec §10.5). It finds the function by name on each call, so that it calls
// the new code after a hot reload (spec §12.4).
class MethodCallable : public godot::CallableCustom {
	godot::ObjectID owner;
	godot::StringName method;

	static bool compare_equal(const godot::CallableCustom *p_a, const godot::CallableCustom *p_b);
	static bool compare_less(const godot::CallableCustom *p_a, const godot::CallableCustom *p_b);

public:
	MethodCallable(godot::Object *p_owner, const godot::StringName &p_method);

	uint32_t hash() const override;
	godot::String get_as_text() const override;
	CompareEqualFunc get_compare_equal_func() const override { return compare_equal; }
	CompareLessFunc get_compare_less_func() const override { return compare_less; }
	godot::ObjectID get_object() const override { return owner; }
	int get_argument_count(bool &r_is_valid) const override;
	void call(const godot::Variant **p_arguments, int p_argcount, godot::Variant &r_return_value, GDExtensionCallError &r_call_error) const override;
};

// A lambda (spec §3.11): a Callable that runs the lambda's function in the
// image that made it, also after a hot reload (§12.4), with copies of what it
// captured, and on the instance it was made on when it uses self, which it
// keeps when RefCounted, as GDScript does
// (modules/gdscript/gdscript_lambda_callable.cpp:285-291). Equal only to
// itself, as GDScript's (:37-45).
class LambdaCallable : public godot::CallableCustom {
	std::shared_ptr<const ClassImage> image;
	int function;
	InstanceContext context; // Without storage when it does not use self.
	godot::ObjectID owner;
	godot::Variant kept; // Holds a RefCounted owner.
	std::vector<std::unique_ptr<ValueBuffer>> captures;
	std::vector<const uint8_t *> capture_data;

	static bool compare_equal(const godot::CallableCustom *p_a, const godot::CallableCustom *p_b) { return p_a == p_b; }
	static bool compare_less(const godot::CallableCustom *p_a, const godot::CallableCustom *p_b) { return std::less<const godot::CallableCustom *>()(p_a, p_b); }

public:
	// The captures are the (type, offset) pairs of MAKE_LAMBDA in `p_frame`.
	LambdaCallable(const std::shared_ptr<const ClassImage> &p_image, int p_function, const InstanceContext &p_context, const uint8_t *p_frame, const uint32_t *p_captures);

	uint32_t hash() const override;
	godot::String get_as_text() const override;
	CompareEqualFunc get_compare_equal_func() const override { return compare_equal; }
	CompareLessFunc get_compare_less_func() const override { return compare_less; }
	bool is_valid() const override;
	godot::ObjectID get_object() const override { return owner; }
	int get_argument_count(bool &r_is_valid) const override;
	void call(const godot::Variant **p_arguments, int p_argcount, godot::Variant &r_return_value, GDExtensionCallError &r_call_error) const override;
};

} // namespace petrichor
