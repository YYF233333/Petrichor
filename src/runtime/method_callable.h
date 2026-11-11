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

// The adapter finds the script instance on an object, by the engine's pointer
// to it, since only it knows how instances are stored; false when the object
// has none of this language.
using InstanceResolver = bool (*)(GDExtensionObjectPtr p_object, ScriptedObject &r_instance);
void set_instance_resolver(InstanceResolver p_resolver);
bool scripted_object(GDExtensionObjectPtr p_object, ScriptedObject &r_instance);

// Sets the script of this language on a new object, whose instance runs the
// member initializers and then _init() without arguments; with
// `p_without_init` it leaves _init() to the caller, which passes the arguments
// of new(...) (spec §6.1). False when the object gets no instance of this
// language; `r_initialized` is false when a program error ended its
// initializers, and _init() is not to run then (§8.1). The instance asks
// init_left_to_caller() as it is made and calls constructed() last.
bool attach_script(godot::Object *p_object, const godot::Variant &p_script, bool p_without_init, ScriptedObject &r_instance, bool &r_initialized);
bool init_left_to_caller();
void constructed(bool p_initialized);

// An object of the class of `p_image`, whose script is `p_script`, made as
// new(...) makes it: the member initializers, then _init() with the arguments
// (spec §6.1). Null when they do not fit or _init() ends in a program error,
// which is then the error of a pending call of a function value (§8.1).
godot::Variant construct(const std::shared_ptr<const ClassImage> &p_image, const godot::Variant &p_script, const godot::Variant **p_args, int p_argcount, GDExtensionCallError &r_error);

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
	bool task = false; // The caller takes an async callee's Task (spec §3.11).
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
	bool is_valid() const override;
	godot::ObjectID get_object() const override { return owner; }
	int get_argument_count(bool &r_is_valid) const override;
	void call(const godot::Variant **p_arguments, int p_argcount, godot::Variant &r_return_value, GDExtensionCallError &r_call_error) const override;

	const godot::StringName &get_method() const { return method; }
	// The script instance on the object and the function's index in its image;
	// false when the object or the function is gone.
	bool resolve(ScriptedObject &r_instance, int &r_function) const;
};

// A static method of an engine class, by name (core/core_bind.cpp:1722-1737).
void call_static(const godot::StringName &p_class, const godot::StringName &p_method, const godot::Variant **p_args, uint32_t p_count, godot::Variant &r_result, GDExtensionCallError &r_error);

// The script of the image's class, held, as GDScript's lambdas and static
// function values hold theirs as their object
// (modules/gdscript/gdscript_lambda_callable.h:45, gdscript_lambda_callable.cpp:75-77,
// gdscript.cpp:993; ed1daf0). UndoRedo of 4.7 runs a Callable only when its
// object is alive (core/object/undo_redo.cpp:357-360). NIL once it is gone.
godot::Variant held_script(const ClassImage &p_image);

// A static function as a value (spec §3.11): of a class of this language,
// found by name on its current image at each call (§12.4), or of an engine
// class. Without an object it is valid, which connect() and emit() ask
// (core/object/object.cpp:1255, 1528-1530; ed1daf0). `new` of a class of this
// language constructs it with its current image.
class StaticCallable : public godot::CallableCustom {
	godot::String owner; // The script's path, or the engine class.
	godot::StringName method;
	bool of_engine;
	bool constructs;
	godot::Variant script; // held_script(), for one of this language.

	static bool compare_equal(const godot::CallableCustom *p_a, const godot::CallableCustom *p_b);
	static bool compare_less(const godot::CallableCustom *p_a, const godot::CallableCustom *p_b);

public:
	StaticCallable(const godot::String &p_owner, const godot::StringName &p_method, bool p_of_engine);

	uint32_t hash() const override;
	godot::String get_as_text() const override;
	CompareEqualFunc get_compare_equal_func() const override { return compare_equal; }
	CompareLessFunc get_compare_less_func() const override { return compare_less; }
	bool is_valid() const override { return true; }
	godot::ObjectID get_object() const override;
	int get_argument_count(bool &r_is_valid) const override;
	void call(const godot::Variant **p_arguments, int p_argcount, godot::Variant &r_return_value, GDExtensionCallError &r_call_error) const override;

	// The static function of this language it calls, on the class's current
	// image, and its index there; false for one of the engine, or for new.
	bool resolve(std::shared_ptr<const ClassImage> &r_image, int &r_function) const;
};

// A method of a struct as a value (spec §3.11): a Callable that runs the
// method, found as the value is made, with a copy of the struct, as a lambda
// runs with its captures. Equal only to itself.
class StructMethodCallable : public godot::CallableCustom {
	std::shared_ptr<const ClassImage> image;
	int function;
	ValueBuffer self;
	godot::Variant script; // held_script()

	static bool compare_equal(const godot::CallableCustom *p_a, const godot::CallableCustom *p_b) { return p_a == p_b; }
	static bool compare_less(const godot::CallableCustom *p_a, const godot::CallableCustom *p_b) { return std::less<const godot::CallableCustom *>()(p_a, p_b); }

public:
	// `p_self` is the struct in the layout of the method's first parameter.
	StructMethodCallable(const std::shared_ptr<const ClassImage> &p_image, int p_function, const uint8_t *p_self);

	uint32_t hash() const override;
	godot::String get_as_text() const override;
	CompareEqualFunc get_compare_equal_func() const override { return compare_equal; }
	CompareLessFunc get_compare_less_func() const override { return compare_less; }
	bool is_valid() const override { return true; }
	godot::ObjectID get_object() const override;
	int get_argument_count(bool &r_is_valid) const override;
	void call(const godot::Variant **p_arguments, int p_argcount, godot::Variant &r_return_value, GDExtensionCallError &r_call_error) const override;

	const std::shared_ptr<const ClassImage> &get_image() const { return image; }
	int get_function() const { return function; }
	const uint8_t *get_self() const { return self.data; }
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
	godot::ObjectID owner; // The instance, or without self the script.
	godot::Variant kept; // Holds a RefCounted owner, or the script (held_script()).
	// The captures, each in its parameter's layout, one after another; in the
	// object itself when they fit, so that making a lambda allocates no more.
	alignas(16) uint8_t inline_captures[64];
	std::unique_ptr<uint64_t[]> large_captures;
	uint8_t *captures;

	static bool compare_equal(const godot::CallableCustom *p_a, const godot::CallableCustom *p_b) { return p_a == p_b; }
	static bool compare_less(const godot::CallableCustom *p_a, const godot::CallableCustom *p_b) { return std::less<const godot::CallableCustom *>()(p_a, p_b); }

public:
	// The captures are the (type, offset) pairs of MAKE_LAMBDA in `p_frame`.
	LambdaCallable(const std::shared_ptr<const ClassImage> &p_image, int p_function, const InstanceContext &p_context, const uint8_t *p_frame, const uint32_t *p_captures);
	~LambdaCallable() override;
	LambdaCallable(const LambdaCallable &) = delete;
	LambdaCallable &operator=(const LambdaCallable &) = delete;

	uint32_t hash() const override;
	godot::String get_as_text() const override;
	CompareEqualFunc get_compare_equal_func() const override { return compare_equal; }
	CompareLessFunc get_compare_less_func() const override { return compare_less; }
	bool is_valid() const override;
	godot::ObjectID get_object() const override { return owner; }
	int get_argument_count(bool &r_is_valid) const override;
	void call(const godot::Variant **p_arguments, int p_argcount, godot::Variant &r_return_value, GDExtensionCallError &r_call_error) const override;

	const std::shared_ptr<const ClassImage> &get_image() const { return image; }
	int get_function() const { return function; }
	const InstanceContext &get_context() const { return context; }
	const uint8_t *get_captures() const { return captures; }
};

} // namespace petrichor
