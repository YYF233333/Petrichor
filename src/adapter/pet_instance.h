#pragma once

#include "adapter/pet_script.h"
#include "runtime/method_callable.h"

#include <gdextension_interface.h>

#include <atomic>
#include <memory>
#include <thread>

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
	// may run the instance (spec §12.3), so each use takes one snapshot. The
	// lock is the instance's own: MSVC's std::atomic_load() of a shared_ptr
	// takes one lock for the whole process (MSVC 14.44, crt/src/stl/atomic.cpp:10-18).
	std::shared_ptr<const ClassImage> current_image() const {
		lock_image();
		std::shared_ptr<const ClassImage> snapshot = image;
		image_lock.clear(std::memory_order_release);
		return snapshot;
	}
	void replace_image(std::shared_ptr<const ClassImage> p_image) {
		lock_image();
		image.swap(p_image); // The old image goes with `p_image`, after the lock.
		image_lock.clear(std::memory_order_release);
	}

private:
	std::shared_ptr<const ClassImage> image; // Replaced on hot reload when only function bodies change.
	mutable std::atomic_flag image_lock = ATOMIC_FLAG_INIT; // Held only to copy or swap `image`.

	void lock_image() const {
		while (image_lock.test_and_set(std::memory_order_acquire)) {
			std::this_thread::yield();
		}
	}

public:
	static const GDExtensionScriptInstanceInfo3 info;
};

// The instance of this language on an object, for method values (runtime/method_callable.h).
bool resolve_instance(GDExtensionObjectPtr p_object, ScriptedObject &r_instance);
// The image of a script of this language, for static methods (runtime/method_callable.h).
std::shared_ptr<const ClassImage> resolve_script(const godot::String &p_path);
// The script of an inner class in a script, and the path of a script, for
// classes named by their paths (runtime/values.h).
godot::Variant resolve_inner_script(const godot::Variant &p_script, const godot::String &p_name);
godot::String resolve_script_path(const godot::Ref<godot::Script> &p_script);

// Converts the dictionaries used by Script reflection into the C structures of
// the script instance interface, and frees them again.
GDExtensionPropertyInfo *make_property_list(const godot::TypedArray<godot::Dictionary> &p_list);
void free_property_list(const GDExtensionPropertyInfo *p_list, uint32_t p_count);
GDExtensionMethodInfo *make_method_list(const godot::TypedArray<godot::Dictionary> &p_list);
void free_method_list(const GDExtensionMethodInfo *p_list, uint32_t p_count);

} // namespace petrichor
