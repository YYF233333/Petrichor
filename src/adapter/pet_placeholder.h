#pragma once

#include "adapter/pet_script.h"

#include <gdextension_interface.h>
#include <godot_cpp/templates/hash_map.hpp>

namespace petrichor {

// A script's instance on an object in the editor, where scripts do not run
// (spec §2.3). As the engine's placeholder (core/object/script_language.cpp:680-923;
// ed1daf0), it shows and stores the exported properties, and keeps every value
// a scene sets while the script has errors. A node's also gives the scene dock
// the errors marked on that node (spec §13.2), which the engine's cannot: it
// answers no call (core/object/script_language.cpp:793-804), and the dock asks
// for them by calling _get_configuration_warnings (scene/main/node.cpp:3522-3532).
struct PetPlaceholder {
	godot::Object *owner = nullptr;
	godot::Ref<PetScript> script;
	godot::TypedArray<godot::Dictionary> properties;
	godot::HashMap<godot::StringName, godot::Variant> values; // Those that differ from the defaults.
	godot::HashMap<godot::StringName, godot::Variant> constants;

	PetPlaceholder(PetScript *p_script, godot::Object *p_owner);

	// Takes the script's properties after it compiled; values of properties it
	// no longer has, or equal to their defaults, are dropped.
	void update(bool p_notify);
	bool default_value(const godot::StringName &p_name, godot::Variant &r_value) const;

	static const GDExtensionScriptInstanceInfo3 info;
};

} // namespace petrichor
