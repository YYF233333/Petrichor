#pragma once

#include <godot_cpp/classes/script_language.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>

namespace petrichor::debugger {

// Connects the virtual machine to the engine's script debugger (spec §12.5):
// breakpoints and stepping at each line, and a break on program errors. The
// engine's debugger then reads this language's call stack.

void set_language(godot::ScriptLanguage *p_language);

bool is_active();

// Keeps the stepping depth of the engine's debugger, as GDScript does.
void enter_function();
void exit_function();

// At the start of each line.
void line(const godot::StringName &p_source, int p_line);

// Before a program error unwinds the stack.
void error(const godot::String &p_message);

godot::String last_error();

} // namespace petrichor::debugger
