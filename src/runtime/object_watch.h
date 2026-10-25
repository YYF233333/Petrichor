#pragma once

#include <gdextension_interface.h>

#include <functional>

namespace petrichor {

// Tells the runtime when an object is freed (spec §9.7). It uses an instance
// binding with the runtime's own token: an object can hold bindings for
// several tokens (core/object/object.cpp:2167-2201), so this one does not
// interfere with the bindings godot-cpp keeps for its wrappers.
//
// Callbacks run while the engine destroys the object; they must not touch it.
namespace object_watch {

using Callback = std::function<void(uint64_t p_object_id)>;

void watch(GDExtensionObjectPtr p_object, Callback p_callback);

} // namespace object_watch

} // namespace petrichor
