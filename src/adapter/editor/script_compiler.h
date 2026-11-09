#pragma once

#include "compiler/diagnostic.h"
#include "compiler/navigation.h"
#include "runtime/class_image.h"

#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <memory>
#include <optional>
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

// What the name at byte `p_offset` of the script's text refers to, for the
// script editor's lookup (spec §13.2).
std::optional<compiler::Lookup> lookup_name(const godot::String &p_path, const std::string &p_source, uint32_t p_offset);

// What can be written at the cursor, which U+FFFF marks in `p_source`, for
// the script editor's completion (spec §13.2).
compiler::Completion completion(const godot::String &p_path, const std::string &p_source);

// The class header of the script at `p_path`, read from `p_source` as the
// compiler reads it (spec §4.1): the path of the script it extends, empty for
// an engine class or none.
godot::String script_base_path(const godot::String &p_path, const godot::String &p_source);
// The engine class it extends, through its bases.
godot::String script_native_base(const godot::String &p_path, const godot::String &p_source);
// Its base in the global class list: the class it names, or else its nearest
// base with a global name. GDScript gives the class it names
// (modules/gdscript/gdscript.cpp:2810-2812; ed1daf0), and the editor lists a
// global class's subclasses by it (core/object/script_language.cpp:439-454).
godot::String script_global_base(const godot::String &p_path, const godot::String &p_source);
// Whether it extends the script at `p_base`, through its bases.
bool script_extends(const godot::String &p_path, const godot::String &p_source, const godot::String &p_base);

// What the documentation names a class of this language, also an inner one,
// by its path.
godot::String class_doc_name(const godot::String &p_path);

// The documentation of a script's classes (spec §13.2), from its
// declarations, as the engine reads it (core/doc_data.h:724-860).
godot::TypedArray<godot::Dictionary> script_documentation(const godot::String &p_path, const godot::String &p_source);

} // namespace petrichor
