#pragma once

#include <gdextension_interface.h>

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace petrichor::probe_methods {

// In probe builds every script also has `echo_later(value, frames)`, an async
// method that returns the signal of a task, for the cross-language await probe
// (spec §9.8) until the language has coroutines.
bool has(const godot::StringName &p_method);
godot::Dictionary info(const godot::StringName &p_method);
void append_to(godot::TypedArray<godot::Dictionary> &r_list);
// Returns false when the method is not a probe method.
bool call(const godot::StringName &p_method, const godot::Variant **p_args, int64_t p_argc, godot::Variant &r_result, GDExtensionCallError &r_error);

} // namespace petrichor::probe_methods
