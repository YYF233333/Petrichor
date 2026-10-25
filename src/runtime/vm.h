#pragma once

#include "runtime/class_image.h"

#include <gdextension_interface.h>

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <memory>
#include <vector>

namespace petrichor {

// What a running function needs from the instance it runs on.
struct InstanceContext {
	uint8_t *members = nullptr; // Laid out as the image's members.
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

} // namespace vm

} // namespace petrichor
