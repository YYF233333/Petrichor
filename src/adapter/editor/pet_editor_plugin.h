#pragma once

#include "adapter/editor/scene_index.h"
#include "compiler/diagnostic.h"
#include "compiler/rename.h"

#include <godot_cpp/classes/editor_context_menu_plugin.hpp>
#include <godot_cpp/classes/editor_export_plugin.hpp>
#include <godot_cpp/classes/editor_plugin.hpp>
#include <godot_cpp/classes/editor_syntax_highlighter.hpp>
#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/popup_menu.hpp>
#include <godot_cpp/classes/popup_panel.hpp>
#include <godot_cpp/classes/shortcut.hpp>
#include <godot_cpp/classes/timer.hpp>
#include <godot_cpp/classes/tree.hpp>
#include <godot_cpp/classes/v_box_container.hpp>

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace petrichor {

// Replaces `.pet` sources with bytecode in exported games (spec §1.3), and
// fails the export on the errors of the checks of scenes and resource files
// (architecture §6.4), and where the exported game could not load this
// library (architecture §5).
class PetExportPlugin : public godot::EditorExportPlugin {
	GDCLASS(PetExportPlugin, godot::EditorExportPlugin);

protected:
	static void _bind_methods() {}

public:
	godot::String _get_name() const override;
	void _export_begin(const godot::PackedStringArray &p_features, bool p_is_debug, const godot::String &p_path, uint32_t p_flags) override;
	void _export_file(const godot::String &p_path, const godot::String &p_type, const godot::PackedStringArray &p_features) override;
};

// The script editor's context menu on a script of this language: formatting
// it, finding the references of the name at the caret, its quick fixes (spec
// §13.2) and renaming it (§13.3).
class PetCodeMenu : public godot::EditorContextMenuPlugin {
	GDCLASS(PetCodeMenu, godot::EditorContextMenuPlugin);

	void format(godot::Object *p_code_edit);

protected:
	static void _bind_methods() {}

public:
	godot::Ref<godot::Shortcut> find_references;
	godot::Ref<godot::Shortcut> rename;
	godot::Ref<godot::Shortcut> quick_fix;

	void _popup_menu(const godot::PackedStringArray &p_paths) override;
};

// The places that finding references lists, in the bottom panel by file, as
// the results of Find in Files show (editor/script/find_in_files.cpp:791-924;
// ed1daf0). Selecting one opens it. It also previews the changes of a rename,
// or shows why it cannot rename.
class PetReferences : public godot::VBoxContainer {
	GDCLASS(PetReferences, godot::VBoxContainer);

	enum class Shown {
		References,
		Changes,
		Refusals, // Their messages are the texts.
	};

	godot::Tree *tree = nullptr;
	godot::HBoxContainer *actions = nullptr;
	Shown shown = Shown::References;
	std::vector<compiler::Occurrence> found;
	std::vector<bool> ticked; // Of the changes.
	godot::Callable apply;

	void fill();
	void open();
	void tick();
	void finish(bool p_apply);
	void highlight(godot::Object *p_item, const godot::Rect2 &p_rect);

protected:
	static void _bind_methods() {}
	void _notification(int p_what);

public:
	PetReferences();
	void set_found(std::vector<compiler::Occurrence> p_found);
	// Each with a check box: candidates start unchecked, and the rest cannot
	// be unchecked. Apply calls `p_apply`.
	void set_changes(const std::vector<compiler::RenameEdit> &p_changes, const godot::Callable &p_apply);
	void set_refusals(const std::vector<compiler::Diagnostic> &p_refusals);
	const std::vector<bool> &checked() const { return ticked; }
};

// Colors scripts in the script editor by the lexer's tokens, in the colors of
// the editor settings that GDScript's highlighter reads
// (modules/gdscript/editor/gdscript_highlighter.cpp:753-980; ed1daf0).
class PetHighlighter : public godot::EditorSyntaxHighlighter {
	GDCLASS(PetHighlighter, godot::EditorSyntaxHighlighter);

	godot::Color text, symbol, number, string, string_name, node_path, node_reference, annotation, comment, doc_comment, function, function_definition, global_function, member;
	// Keywords and type names, by their color where they are not members.
	std::unordered_map<std::string, godot::Color> names;

	// The text at `version` with the input method's, by line, and the colors
	// of each line from the columns they begin at.
	mutable int64_t version = -1;
	mutable godot::PackedStringArray lines;
	mutable std::vector<std::vector<std::pair<int, godot::Color>>> line_colors;

	void highlight() const;

protected:
	static void _bind_methods() {}

public:
	godot::String _get_name() const override;
	godot::PackedStringArray _get_supported_languages() const override;
	godot::Ref<godot::EditorSyntaxHighlighter> _create() const override;
	void _update_cache() override;
	godot::Dictionary _get_line_syntax_highlighting(int32_t p_line) const override;
};

// Edits, each in the text of the file it changes, by its path.
using FileEdits = std::map<std::string, std::vector<compiler::CheckedEdit>>;

// Compiles the project before it runs (architecture §2.4), provides the
// headless commands (spec §13.1, §13.2):
//   godot --headless -e --path <project> -- --petrichor-build
//   godot --headless -e --path <project> -- --petrichor-format [--check]
// has the nodes ask again for their errors when a file or an open scene
// changes, shows the references found in its panel, offers quick fixes and
// renames (spec §13.2, §13.3).
class PetEditorPlugin : public godot::EditorPlugin {
	GDCLASS(PetEditorPlugin, godot::EditorPlugin);

	godot::Ref<PetExportPlugin> export_plugin;
	godot::Ref<PetCodeMenu> code_menu;
	godot::Ref<PetHighlighter> highlighter;
	PetReferences *references = nullptr;
	godot::PopupPanel *name_popup = nullptr;
	godot::LineEdit *name_edit = nullptr;
	// The rename asked for: the script, the name's offset in its text and the
	// new name, and what renaming changes, which the References panel may
	// preview.
	struct {
		std::string path;
		uint32_t offset = 0;
		std::string name;
		compiler::Rename changes;
	} renaming;
	// The renames in the editor's global history, by their index there: the
	// action's name, the files it changed and `elsewhere` when it was made.
	struct Recorded {
		godot::String name;
		std::set<std::string> files;
		uint64_t elsewhere = 0;
	};
	std::map<int32_t, Recorded> recorded;
	// The steps in the editor's other histories since the first rename, which
	// may come before a rename in its undo (architecture §2.3).
	uint64_t elsewhere = 0;
	uint64_t global_version = 0;
	godot::Timer *marks = nullptr; // Asks the nodes for their errors once scenes stop changing.
	// The quick fixes of the script in the script editor's current tab: its
	// errors with fixes, of its text at `version`, each edit naming its file,
	// and the line of the caret when it has one of them.
	struct Fixing {
		uint64_t code_edit = 0;
		std::string path;
		uint32_t version = 0;
		std::vector<compiler::Diagnostic> errors;
		int bulb = -1;
	};
	Fixing fixing;
	godot::PopupMenu *fix_list = nullptr;
	// Its fixes, each with its error, by id. The ids after them fix all of a
	// kind, by its number.
	std::vector<std::pair<compiler::Diagnostic, compiler::Fix>> listed;

	void history_changed();
	void script_changed();
	void validated(godot::Object *p_code_edit);
	godot::CodeEdit *fixing_code_edit() const;
	void refresh_fixes();
	void show_bulb();
	void draw_bulb(int p_line, int p_gutter, const godot::Rect2 &p_region, godot::Object *p_code_edit);
	void size_gutter(godot::Object *p_code_edit);
	void bulb_clicked(int p_line, int p_gutter);
	void list_fixes(godot::Object *p_code_edit);
	void list_fixes_at(godot::CodeEdit *p_code_edit, const godot::Vector2 &p_position);
	void apply_listed(int p_id);
	bool change_scripts(const OpenProject &p_project, const FileEdits &p_changes);
	void change_scripts_again(const godot::Array &p_edits);
	void change_files(const OpenProject &p_project, const FileEdits &p_changes, bool p_save);
	void record(const godot::String &p_action, const std::set<std::string> &p_files, const godot::Array &p_do, const godot::Array &p_undo);
	void show_references(godot::Object *p_code_edit);
	void ask_name(godot::Object *p_code_edit);
	void rename_to(const godot::String &p_name);
	void apply_previewed();
	void apply(const OpenProject &p_project, const std::string &p_path, uint32_t p_offset, const std::string &p_name, const std::vector<compiler::RenameEdit> &p_edits, bool p_record);
	void rename_declaration(int p_kind, const godot::String &p_scope, const godot::String &p_name, const godot::String &p_new, const godot::Array &p_candidates);
	bool waits_for_scan(void (PetEditorPlugin::*p_command)());
	void run_build_command();
	void run_format_command();
	void resource_saved(godot::Object *p_resource);
	void scene_saved(const godot::String &p_path);
	void files_or_settings_changed();

protected:
	static void _bind_methods();

public:
	void _enter_tree() override;
	void _exit_tree() override;
	void _input(const godot::Ref<godot::InputEvent> &p_event) override;
	void _shortcut_input(const godot::Ref<godot::InputEvent> &p_event) override;
	bool _build() override;
};

} // namespace petrichor
