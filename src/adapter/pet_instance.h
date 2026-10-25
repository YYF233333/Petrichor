#pragma once

#include "adapter/pet_script.h"

#include <gdextension_interface.h>

#include <memory>

namespace petrichor {

// The data behind one engine script instance; the engine reaches it through
// the callbacks in `info`. Members are stored by type in one block laid out as
// the image's members (architecture §2.2).
struct PetInstance {
	GDExtensionObjectPtr owner = nullptr;
	godot::Object *owner_object = nullptr;
	godot::Ref<PetScript> script;
	std::shared_ptr<const ClassImage> image; // Replaced on hot reload when the layout is unchanged.
	uint8_t *members = nullptr;

	PetInstance(PetScript *p_script, const std::shared_ptr<const ClassImage> &p_image, godot::Object *p_owner);
	~PetInstance();

	static const GDExtensionScriptInstanceInfo3 info;
};

// Converts the dictionaries used by Script reflection into the C structures of
// the script instance interface, and frees them again.
GDExtensionPropertyInfo *make_property_list(const godot::TypedArray<godot::Dictionary> &p_list);
void free_property_list(const GDExtensionPropertyInfo *p_list, uint32_t p_count);
GDExtensionMethodInfo *make_method_list(const godot::TypedArray<godot::Dictionary> &p_list);
void free_method_list(const GDExtensionMethodInfo *p_list, uint32_t p_count);

} // namespace petrichor
