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
};

// An entry of this language's call stack, kept per thread for error messages
// and the debugger.
struct StackFrame {
	const ClassImage *image = nullptr;
	const bytecode::Function *function = nullptr;
	uint8_t *frame = nullptr;
	uint8_t *members = nullptr;
	int line = 0;
};

namespace vm {

// Calls a function from the engine or another language. The arguments pass the
// entry checks (spec §10.1); after a program error the engine gets the default
// value of the return type (spec §8.1), and `r_failed`, when given, is set.
godot::Variant call(const std::shared_ptr<const ClassImage> &p_image, int p_function, const InstanceContext &p_context, const godot::Variant **p_args, int64_t p_argc, GDExtensionCallError &r_error, bool *r_failed = nullptr);

// The call stack of the current thread, innermost last.
const std::vector<StackFrame> &stack();

// Reports a program error at a line of the module's source (spec §8.1): the
// debugger breaks, and the engine logs it with the call stack.
void report_error(const ClassImage &p_image, const char *p_function, int p_line, const godot::String &p_message);

} // namespace vm

} // namespace petrichor
