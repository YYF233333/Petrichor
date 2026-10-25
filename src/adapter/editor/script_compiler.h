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
std::shared_ptr<const ClassImage> compile_script(const godot::String &p_path, const godot::String &p_source, std::vector<compiler::Diagnostic> *r_errors, bool p_report, CompileMode p_mode = CompileMode::Full);

// Compiles every .pet file of the project and writes the bytecode cache
// (architecture §2.4). Returns the number of scripts with errors.
int build_project(int *r_script_count = nullptr);

} // namespace petrichor
