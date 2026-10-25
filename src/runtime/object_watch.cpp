#include "runtime/object_watch.h"

#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/godot.hpp>

#include <mutex>
#include <utility>
#include <vector>

namespace petrichor {
namespace object_watch {

namespace {

struct Binding {
	uint64_t object_id = 0;
	std::vector<Callback> callbacks;
};

// The address of this variable is the runtime's binding token.
char token = 0;
std::mutex mutex;

void *binding_create(void *p_token, void *p_instance) {
	Binding *binding = memnew(Binding);
	binding->object_id = godot::gdextension_interface::object_get_instance_id(p_instance);
	return binding;
}

void binding_free(void *p_token, void *p_instance, void *p_binding) {
	Binding *binding = static_cast<Binding *>(p_binding);
	std::vector<Callback> callbacks;
	{
		std::lock_guard<std::mutex> lock(mutex);
		callbacks = std::move(binding->callbacks);
	}
	for (const Callback &callback : callbacks) {
		callback(binding->object_id);
	}
	godot::memdelete(binding);
}

GDExtensionBool binding_reference(void *p_token, void *p_binding, GDExtensionBool p_reference) {
	return true;
}

const GDExtensionInstanceBindingCallbacks callbacks = {
	binding_create,
	binding_free,
	binding_reference,
};

} // namespace

void watch(GDExtensionObjectPtr p_object, Callback p_callback) {
	Binding *binding = static_cast<Binding *>(godot::gdextension_interface::object_get_instance_binding(p_object, &token, &callbacks));
	std::lock_guard<std::mutex> lock(mutex);
	binding->callbacks.push_back(std::move(p_callback));
}

} // namespace object_watch
} // namespace petrichor
