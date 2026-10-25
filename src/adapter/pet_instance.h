#pragma once

#include "adapter/pet_script.h"

#include <gdextension_interface.h>

namespace petrichor {

// The data behind one engine script instance; the engine reaches it through
// the callbacks in `info`.
struct PetInstance {
	GDExtensionObjectPtr owner = nullptr;
	godot::Ref<PetScript> script;
	godot::String message;

	static const GDExtensionScriptInstanceInfo3 info;
};

// Converts the dictionaries used by Script reflection into the C structures of
// the script instance interface, and frees them again.
GDExtensionPropertyInfo *make_property_list(const godot::TypedArray<godot::Dictionary> &p_list);
void free_property_list(const GDExtensionPropertyInfo *p_list, uint32_t p_count);
GDExtensionMethodInfo *make_method_list(const godot::TypedArray<godot::Dictionary> &p_list);
void free_method_list(const GDExtensionMethodInfo *p_list, uint32_t p_count);

} // namespace petrichor
