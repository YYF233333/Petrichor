#pragma once

#include "runtime/class_image.h"

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/string.hpp>

#include <memory>

namespace petrichor {
class PetScript;

// Where the editor writes the bytecode of a script, and the game reads it
// (architecture §2.4): res://.godot/petrichor/<path>c.
godot::String cache_path(const godot::String &p_script_path);

// Reads a compiled script. With `p_source`, the bytecode must have been
// compiled from exactly that source.
std::shared_ptr<const ClassImage> load_compiled(const godot::String &p_compiled_path, const godot::String &p_script_path, const godot::String *p_source, godot::String &r_error, godot::Ref<PetScript> *r_base = nullptr);

bool write_compiled(const godot::String &p_compiled_path, const ClassImage &p_image);

} // namespace petrichor
