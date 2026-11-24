#pragma once

#include "adapter/editor/scene_index.h"
#include "compiler/diagnostic.h"
#include "compiler/navigation.h"
#include "compiler/rename.h"
#include "runtime/class_image.h"

#include <godot_cpp/classes/editor_debugger_plugin.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace petrichor {

// The project's scripts as the editor and the headless build check them,
// each checked again only when what it read changes (architecture §2.4). It
// reads workspace(), the build and the export SceneIndex, and an LSP session
// what it sees (compiler::Analysis::View).
compiler::Analysis &analysis();

// Compiles a script in the editor; CheckMode::Declarations while it is being
// loaded, when the scenes that use it cannot be loaded
// (core/io/resource_loader.cpp:1049-1056). Returns null when there are
// errors; with `p_report`, each error is printed as a script error at its line.
// `r_load_error` gets why a script without errors gives no class.
std::shared_ptr<const ClassImage> compile_script(const godot::String &p_path, const godot::String &p_source, std::vector<compiler::Diagnostic> *r_errors, bool p_report, compiler::CheckMode p_mode = compiler::CheckMode::Program, std::vector<compiler::Diagnostic> *r_warnings = nullptr, godot::String *r_load_error = nullptr);

// What build_project() found: the files checked, and the files with errors
// that keep the game from running, or with any error for a command.
struct ProjectReport {
	int scripts = 0;
	int scenes = 0;
	int resources = 0;
	int failed = 0;
	// Each naming its file, in the order checked.
	std::vector<compiler::Diagnostic> diagnostics;
};

// Compiles every .pet file of the project as it is on disk, and checks every
// scene and resource file (§6.4). `p_write` writes the bytecode cache
// (architecture §2.4). For the commands (spec §13.1, §7.5), `p_command`
// counts every error and gives the warnings and hints too.
ProjectReport build_project(bool p_write, bool p_command);

// Writes the bytecode of the script at the path as its file compiles against
// the project's files, as the build compiles it (architecture §2.4), so that
// a game reads what the files on disk give. Called outside any pass; whether
// it compiled.
bool write_bytecode(const godot::String &p_path);
// After the script's file was written: its bytecode, and a running game
// reloads it, as the script editor has a saved script without errors reloaded
// while Debug > Synchronize Script Changes is on
// (editor/script/script_editor_plugin.cpp:776-801,
// editor/debugger/debugger_editor_plugin.cpp:238; ed1daf0).
void script_written(const godot::String &p_path, const godot::Ref<godot::EditorDebuggerPlugin> &p_sessions);

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

// What the name at byte `p_offset` of the script's text refers to, for the
// script editor's lookup (spec §13.2).
std::optional<compiler::Lookup> lookup_name(const godot::String &p_path, const std::string &p_source, uint32_t p_offset);
// Where the types of what it holds are declared (compiler::type_definition()).
std::vector<compiler::Lookup> type_definition(const std::string &p_path, const std::string &p_source, uint32_t p_offset);

// Where what the name at byte `p_offset` of the script's text refers to is
// declared and referred to in the project, with the candidates in GDScript
// and C# files (spec §13.2); nullopt where it refers to nothing.
std::optional<std::vector<compiler::Occurrence>> find_references(const std::string &p_path, uint32_t p_offset);

// What renaming the name at byte `p_offset` of the script's text to `p_new`
// changes (spec §13.3).
compiler::Rename rename(const std::string &p_path, uint32_t p_offset, const std::string &p_new);
// The name it renames there to any new name, or why none
// (compiler::renamed_name()).
compiler::RenamedName renamed_name(const std::string &p_path, uint32_t p_offset);

// Where the script declares `p_id`; nullopt if it does not.
std::optional<uint32_t> declaration_offset(const std::string &p_path, const compiler::DeclId &p_id);

// What can be written at the cursor, which U+FFFF marks in `p_source`, for
// the script editor's completion (spec §13.2).
compiler::Completion completion(const godot::String &p_path, const std::string &p_source);

// The errors with quick fixes of the script at `p_path` (spec §13.2), from
// `p_errors` of its own check and from the checks of its scenes, by
// position, each edit naming its file. Those of the scenes count with fixes
// in this script only.
std::vector<compiler::Diagnostic> fixable(std::vector<compiler::Diagnostic> p_errors, const std::string &p_path);
// The quick fixes that the script editor and the LSP server list for
// `p_errors`, those of `p_fixable` on the lines asked for (architecture
// §2.4): first the fixes of the errors that meet the range from `p_from` to
// `p_to`, then of the others by position, each once. Apart, each automatic
// kind among them where the script's own check has more than one fix of it
// that changes only this script, as fixing all takes them (§2.1).
struct QuickFixes {
	std::vector<std::pair<const compiler::Diagnostic *, const compiler::Fix *>> fixes;
	std::vector<compiler::FixKind> all;
};
QuickFixes quick_fixes(std::vector<const compiler::Diagnostic *> p_errors, uint32_t p_from, uint32_t p_to, const std::vector<compiler::Diagnostic> &p_fixable, const std::string &p_path);

// Edits, each in the text of the file it changes, by its path.
using FileEdits = std::map<std::string, std::vector<compiler::CheckedEdit>>;

// A quick fix listed from the check of the script at `p_path` that `p_stamp`
// stamps, found again (architecture §2.4): the fix titled `p_title` of the
// error with the range, the message and the file of `p_error`, or without
// `p_error`, fixing all of `p_kind`. It is found in that check while the
// files its fixes edit are unchanged since. Otherwise it is found in a check
// that keeps nothing from earlier ones, as the analysis keeps a check while
// the other scripts' declarations stay the same, though their offsets may
// move. Its edits in order; none where it is not found.
FileEdits fix_again(const std::string &p_path, const compiler::Stamp &p_stamp, const compiler::Diagnostic *p_error, const std::string &p_title, compiler::FixKind p_kind);

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
