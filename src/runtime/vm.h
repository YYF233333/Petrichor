#pragma once

#include "runtime/class_image.h"

#include <gdextension_interface.h>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <atomic>
#include <memory>
#include <vector>

namespace petrichor {

// The members of an instance of this language. The instance and every call
// running on it share them, so that they outlive an instance that the engine
// deletes while one of its methods runs: the deletion marks them freed, and
// whoever lets go of them last destroys them. Code that then uses self or a
// member stops with a program error (spec §3.3).
class InstanceStorage {
	std::shared_ptr<const ClassImage> image; // Whose layout the members have, which a reload keeps.

public:
	uint8_t *const members;
	std::atomic<bool> freed{ false };
	std::atomic<bool> coroutines_cancelled{ false };

	explicit InstanceStorage(const std::shared_ptr<const ClassImage> &p_image);
	~InstanceStorage();
	InstanceStorage(const InstanceStorage &) = delete;
	InstanceStorage &operator=(const InstanceStorage &) = delete;
};

// What a running function needs from the instance it runs on; holding it keeps
// the members.
struct InstanceContext {
	std::shared_ptr<InstanceStorage> storage; // None for a static function.
	godot::Object *owner = nullptr;
	std::shared_ptr<const ClassImage> self_image;
};

// An entry of this language's call stack, kept per thread for error messages
// and the debugger.
struct StackFrame {
	const ClassImage *image = nullptr;
	const ClassImage *self_image = nullptr;
	const bytecode::Function *function = nullptr;
	uint8_t *frame = nullptr;
	uint8_t *members = nullptr;
	int line = 0;
};

namespace vm {

// Calls a function from the engine or another language. The arguments pass the
// entry checks (spec §10.1); after a program error the engine gets the default
// value of the return type (spec §8.1), and `r_failed`, when given, is set. A
// lambda's captures, its last parameters, are copied from `p_captures`, each
// in its parameter's layout (spec §3.11). An async function gives the engine
// its Task's `completed` signal, or the result when it ended at once; with
// `p_task`, a caller of this language gets the Task, whose failure is not the
// caller's (§9.1). A struct's method value passes its copy of the struct,
// the first parameter, as `p_bound_self` in that parameter's layout.
godot::Variant call(std::shared_ptr<const ClassImage> p_image, int p_function, InstanceContext p_context, const godot::Variant **p_args, int64_t p_argc, GDExtensionCallError &r_error, bool *r_failed = nullptr, const uint8_t *const *p_captures = nullptr, bool p_task = false, const uint8_t *p_bound_self = nullptr);

// Initializes the static variables of a class that has loaded (spec §4.8):
// its base's first, unless done, then its own and its inner classes'. False
// when any initialization ended in a program error, which was reported.
bool initialize_statics(const std::shared_ptr<const ClassImage> &p_image);

// A pub static variable that the class declares or inherits (spec §4.8), as
// the engine and GDScript read and write it by name (§10.5): through its
// accessors, once its class is initialized; false when there is none. A
// value that fails the entry check is a program error, reported at the
// variable, which keeps its value. In the editor, where scripts do not run,
// the variable's storage is read and written directly.
bool get_static(const std::shared_ptr<const ClassImage> &p_image, const godot::StringName &p_name, godot::Variant &r_value);
bool set_static(const std::shared_ptr<const ClassImage> &p_image, const godot::StringName &p_name, const godot::Variant &p_value);

// The call stack of the current thread, innermost last.
const std::vector<StackFrame> &stack();

// Reports a program error at a line of the module's source (spec §8.1): the
// debugger breaks, and the engine logs it with the call stack.
void report_error(const ClassImage &p_image, const char *p_function, int p_line, const godot::String &p_message);

} // namespace vm

} // namespace petrichor
