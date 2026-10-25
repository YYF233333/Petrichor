#pragma once

#include "runtime/vm.h"

#include <godot_cpp/variant/callable_custom.hpp>

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

// Whether the object's script of this language implements the interface,
// named "script::Name" (spec §3.9).
bool implements_interface(godot::Object *p_object, const godot::String &p_interface);

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

} // namespace petrichor
