#pragma once

#include "compiler/diagnostic.h"
#include "runtime/class_image.h"

#include <godot_cpp/variant/string.hpp>

#include <memory>
#include <vector>

namespace petrichor {

enum class CompileMode {
	// With function bodies and the scenes that use the script.
	Full,
	// Only what describes the class, without scenes: while the script is
	// being loaded, the scenes that use it cannot be loaded
	// (core/io/resource_loader.cpp:1049-1056).
	Declarations,
};

// Compiles a script in the editor. Returns null when there are errors; with
// `p_report`, each error is printed as a script error at its line.
std::shared_ptr<const ClassImage> compile_script(const godot::String &p_path, const godot::String &p_source, std::vector<compiler::Diagnostic> *r_errors, bool p_report, CompileMode p_mode = CompileMode::Full, std::vector<compiler::Diagnostic> *r_warnings = nullptr);

// Compiles every .pet file of the project and writes the bytecode cache
// (architecture §2.4). Returns the number of scripts with errors. With
// `p_warnings`, prints the warnings and hints too, as the headless command
// does (spec §13.1).
int build_project(int *r_script_count = nullptr, bool p_warnings = false);

// When an extension loads, unloads or reloads, ClassDB changes, and every
// compiled script may change with it.
void engine_classes_changed();

// The errors of the script from `p_source`, as of the last change to the
// project, for the nodes that the scene dock asks at each of its updates
// (spec §13.2).
std::vector<compiler::Diagnostic> scene_errors(const godot::String &p_path, const godot::String &p_source);
// A file of the project was saved or changed: the nodes ask again.
void project_changed();

} // namespace petrichor
