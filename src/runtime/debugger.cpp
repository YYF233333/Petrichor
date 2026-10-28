#include "runtime/debugger.h"

#include "runtime/text.h"

#include <godot_cpp/classes/engine_debugger.hpp>

#include <string>

using namespace godot;

namespace petrichor::debugger {

namespace {

ScriptLanguage *language = nullptr;
// Not a godot::String: globals are constructed when the library loads, before
// godot-cpp can create engine values.
thread_local std::string break_reason;

void debug_break(const String &p_reason, bool p_can_continue) {
	// On any thread, as GDScript does (modules/gdscript/gdscript_editor.cpp:273-285):
	// the engine's debugger keeps its stepping state per thread
	// (core/debugger/script_debugger.h:46-47), and so does this call stack.
	if (!language) {
		return;
	}
	break_reason = to_utf8(p_reason);
	EngineDebugger::get_singleton()->script_debug(language, p_can_continue, p_reason != "Breakpoint");
}

} // namespace

void set_language(ScriptLanguage *p_language) {
	language = p_language;
}

bool is_active() {
	EngineDebugger *engine_debugger = EngineDebugger::get_singleton();
	return engine_debugger && engine_debugger->is_active();
}

void enter_function() {
	if (!is_active()) {
		return;
	}
	EngineDebugger *engine_debugger = EngineDebugger::get_singleton();
	if (engine_debugger->get_lines_left() > 0 && engine_debugger->get_depth() >= 0) {
		engine_debugger->set_depth(engine_debugger->get_depth() + 1);
	}
}

void exit_function() {
	if (!is_active()) {
		return;
	}
	EngineDebugger *engine_debugger = EngineDebugger::get_singleton();
	if (engine_debugger->get_lines_left() > 0 && engine_debugger->get_depth() >= 0) {
		engine_debugger->set_depth(engine_debugger->get_depth() - 1);
	}
}

void line(const StringName &p_source, int p_line) {
	EngineDebugger *engine_debugger = EngineDebugger::get_singleton();
	bool do_break = false;
	if (engine_debugger->get_lines_left() > 0) {
		if (engine_debugger->get_depth() <= 0) {
			engine_debugger->set_lines_left(engine_debugger->get_lines_left() - 1);
		}
		if (engine_debugger->get_lines_left() <= 0) {
			do_break = true;
		}
	}
	if (!engine_debugger->is_skipping_breakpoints() && engine_debugger->is_breakpoint(p_line, p_source)) {
		do_break = true;
	}
	if (do_break) {
		debug_break("Breakpoint", true);
	}
	engine_debugger->line_poll();
}

void error(const String &p_message) {
	if (is_active()) {
		debug_break(p_message, false);
	}
}

String last_error() {
	return to_godot(break_reason);
}

} // namespace petrichor::debugger
