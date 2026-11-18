#pragma once

#include "adapter/editor/scene_index.h"
#include "compiler/diagnostic.h"
#include "compiler/navigation.h"
#include "compiler/rename.h"
#include "runtime/class_image.h"

#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <memory>
#include <optional>
#include <vector>

namespace petrichor {

// The project's scripts as the editor and the headless build check them,
// each checked again only when what it read changes (architecture §2.4). It
// reads workspace(), the build and the export SceneIndex.
compiler::Analysis &analysis();

// Compiles a script in the editor; CheckMode::Declarations while it is being
// loaded, when the scenes that use it cannot be loaded
// (core/io/resource_loader.cpp:1049-1056). Returns null when there are
// errors; with `p_report`, each error is printed as a script error at its line.
std::shared_ptr<const ClassImage> compile_script(const godot::String &p_path, const godot::String &p_source, std::vector<compiler::Diagnostic> *r_errors, bool p_report, compiler::CheckMode p_mode = compiler::CheckMode::Program, std::vector<compiler::Diagnostic> *r_warnings = nullptr);

// Compiles every .pet file of the project and writes the bytecode cache
// (architecture §2.4), and checks every scene and resource file (§6.4).
// Returns the number of files with errors that keep the game from running.
// For the headless command (spec §13.1, §7.5), `p_command` counts every error
// and prints the warnings and hints too.
int build_project(int *r_script_count = nullptr, int *r_scene_count = nullptr, int *r_resource_count = nullptr, bool p_command = false);

// The check of the scene or resource file at the path (architecture §6.4);
// with `p_report`, each error is printed as a script error at its line.
std::shared_ptr<const compiler::CompileResult> check_scene(const godot::String &p_path, bool p_report);

// When an extension loads, unloads or reloads, ClassDB changes, and every
// compiled script may change with it.
void engine_classes_changed();
// Files changed on disk, or the project settings, as the editor's events tell
// (architecture §2.1).
void disk_changed();

// The errors of the script, for the nodes that the scene dock asks at each of
// its updates (spec §13.2).
std::vector<compiler::Diagnostic> scene_errors(const godot::String &p_path);
// Those that the checks of the scenes placing the script, or instancing such a
// scene, and of the files whose resources have scripts of this language,
// report in its source, for the script editor (architecture §6.4).
std::vector<compiler::Diagnostic> placed_errors(const godot::String &p_path);

// What the name at byte `p_offset` of the script's text refers to, for the
// script editor's lookup (spec §13.2).
std::optional<compiler::Lookup> lookup_name(const godot::String &p_path, const std::string &p_source, uint32_t p_offset);

// Where what the name at byte `p_offset` of the script's text refers to is
// declared and referred to in the project, with the candidates in GDScript
// and C# files (spec §13.2); nullopt where it refers to nothing.
std::optional<std::vector<compiler::Occurrence>> find_references(const std::string &p_path, uint32_t p_offset);

// What renaming the name at byte `p_offset` of the script's text to `p_new`
// changes (spec §13.3).
compiler::Rename rename(const std::string &p_path, uint32_t p_offset, const std::string &p_new);

// Where the script declares `p_id`; nullopt if it does not.
std::optional<uint32_t> declaration_offset(const std::string &p_path, const compiler::DeclId &p_id);

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
