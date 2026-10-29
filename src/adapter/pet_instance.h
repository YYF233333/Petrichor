#pragma once

#include "adapter/pet_script.h"
#include "runtime/method_callable.h"

#include <gdextension_interface.h>

#include <memory>

namespace petrichor {

// The data behind one engine script instance; the engine reaches it through
// the callbacks in `info`. Members are stored by type in one block laid out as
// the image's members (architecture §2.2), which running calls share, so that
// they outlive the instance when the engine deletes it during one.
struct PetInstance {
	GDExtensionObjectPtr owner = nullptr;
	godot::Object *owner_object = nullptr;
	godot::Ref<PetScript> script;
	std::shared_ptr<InstanceStorage> storage;

	PetInstance(PetScript *p_script, const std::shared_ptr<const ClassImage> &p_image, godot::Object *p_owner);
	~PetInstance();

	InstanceContext context() const { return InstanceContext{ storage, owner_object }; }

	// A hot reload replaces the image on the main thread while other threads
	// may run the instance (spec §12.3), so each use takes one snapshot.
	std::shared_ptr<const ClassImage> current_image() const { return std::atomic_load(&image); }
	void replace_image(const std::shared_ptr<const ClassImage> &p_image) { std::atomic_store(&image, p_image); }

private:
	std::shared_ptr<const ClassImage> image; // Replaced on hot reload when only function bodies change.

public:
	static const GDExtensionScriptInstanceInfo3 info;
};

// The instance of this language on an object, for method values (runtime/method_callable.h).
bool resolve_instance(godot::Object *p_object, ScriptedObject &r_instance);

// Converts the dictionaries used by Script reflection into the C structures of
// the script instance interface, and frees them again.
GDExtensionPropertyInfo *make_property_list(const godot::TypedArray<godot::Dictionary> &p_list);
void free_property_list(const GDExtensionPropertyInfo *p_list, uint32_t p_count);
GDExtensionMethodInfo *make_method_list(const godot::TypedArray<godot::Dictionary> &p_list);
void free_method_list(const GDExtensionMethodInfo *p_list, uint32_t p_count);

} // namespace petrichor
