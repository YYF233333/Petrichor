#pragma once

#include "adapter/editor/lsp_server.h"
#include "adapter/editor/pet_export_plugin.h"
#include "adapter/editor/pet_highlighter.h"
#include "adapter/editor/pet_references.h"
#include "adapter/editor/scene_index.h"
#include "adapter/editor/script_compiler.h"
#include "compiler/diagnostic.h"
#include "compiler/rename.h"

#include <godot_cpp/classes/editor_context_menu_plugin.hpp>
#include <godot_cpp/classes/editor_plugin.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/popup_menu.hpp>
#include <godot_cpp/classes/popup_panel.hpp>
#include <godot_cpp/classes/shortcut.hpp>
#include <godot_cpp/classes/timer.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace petrichor {

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

// Compiles the project before it runs (architecture §2.4), provides the
// commands of pet, also headless (spec §13.1, §13.2, architecture §6.5):
//   godot --headless -e --path <project> -- --petrichor-check
//   godot --headless -e --path <project> -- --petrichor-build
//   godot --headless -e --path <project> -- --petrichor-format [--check]
//   godot --headless -e --path <project> -- --petrichor-migrate [--reverse] [--dry-run] [--allow-dirty] [<path>...]
// has the nodes ask again for their errors when a file or an open scene
// changes, shows the references found in its panel, offers quick fixes and
// renames (spec §13.2, §13.3). It runs the language server, headless only
// with `-- --petrichor-lsp`.
class PetEditorPlugin : public godot::EditorPlugin {
	GDCLASS(PetEditorPlugin, godot::EditorPlugin);

	std::unique_ptr<LspServer> lsp;
	godot::Ref<godot::EditorDebuggerPlugin> sessions; // Of the running games, told to reload a script the client saved.
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
	// The refactorings in the editor's global history, by their index there:
	// the action's name, the files it changed, `elsewhere` when it was made,
	// and the calls that redo and undo it.
	struct Recorded {
		godot::String name;
		std::set<std::string> files;
		uint64_t elsewhere = 0;
		godot::Array redo;
		godot::Array undo;
	};
	std::map<int32_t, Recorded> recorded;
	// The steps in the editor's other histories since the first rename, which
	// may come before a rename in its undo (architecture §2.3).
	uint64_t elsewhere = 0;
	uint64_t global_version = 0;
	// The tabs' versions after a refactoring added a step to them, and after a
	// step that undoing a refactoring took off, each with the id of the tab's
	// CodeEdit.
	std::set<std::pair<uint64_t, uint32_t>> refactored;
	std::set<std::pair<uint64_t, uint32_t>> taken_off;
	// The paths from the edited scene's root of the nodes it owns, by id, as
	// they were after the last step; the root's id. The nodes renamed or added
	// while an action is committed (spec §13.3).
	std::map<uint64_t, godot::String> scene_nodes;
	uint64_t scene_root = 0;
	std::set<uint64_t> touched;
	bool merging = false;
	// While open scenes are saved before a rename: the tab shown before, and
	// the scenes saved.
	int32_t shown_before = -1;
	std::set<std::string> saving;
	godot::Timer *marks = nullptr; // Asks the nodes for their errors once scenes stop changing.
	uint64_t checked = 0; // The workspace's revision when the nodes last asked.
	std::map<uint64_t, uint64_t> tab_checks; // The workspace's revision at each tab's last check, by its CodeEdit.
	// The quick fixes of the script in the script editor's current tab: its
	// errors with fixes at the workspace's `revision`, each edit naming its
	// file, the stamp of the check they come from, and the line of the caret
	// when it has one of them.
	struct Fixing {
		uint64_t code_edit = 0;
		std::string path;
		uint64_t revision = 0;
		compiler::Stamp stamp;
		std::vector<compiler::Diagnostic> errors;
		int bulb = -1;
	};
	Fixing fixing;
	godot::PopupMenu *fix_list = nullptr;
	// Its fixes, each with its error, by id. The ids after them fix all of a
	// kind, by its number.
	std::vector<std::pair<compiler::Diagnostic, compiler::Fix>> listed;

	void history_changed();
	void take_scene_nodes();
	void node_touched(godot::Node *p_node);
	void nodes_committed();
	void move_nodes(const godot::String &p_scene, const godot::Dictionary &p_moves, bool p_undo);
	void script_changed();
	void validated(godot::Object *p_code_edit);
	godot::CodeEdit *fixing_code_edit() const;
	void refresh_fixes();
	void show_bulb();
	void draw_bulb(int p_line, int p_gutter, const godot::Rect2 &p_region, godot::Object *p_code_edit);
	void size_gutter(uint64_t p_code_edit);
	void bulb_clicked(int p_line, int p_gutter);
	void list_fixes(godot::Object *p_code_edit);
	void list_fixes_at(godot::CodeEdit *p_code_edit, const godot::Vector2 &p_position);
	void apply_listed(int p_id);
	godot::String change_scripts(const FileEdits &p_changes, bool p_undo, bool p_apply);
	godot::String change_scripts_again(const godot::Array &p_edits, bool p_undo, bool p_apply);
	bool change_files(const FileEdits &p_changes, bool p_undo);
	enum class Saving {
		Done,
		Waiting, // For the next frame.
		Failed,
	};
	Saving scenes_saved(const std::set<std::string> &p_files);
	void reload_written(const std::set<std::string> &p_files);
	std::set<std::string> scanned_for; // Written, read back once the editor's scan ends (reload_written()).
	void read_back_scanned();
	void read_back(const std::set<std::string> &p_files);
	void record(const godot::String &p_action, const std::set<std::string> &p_files, const godot::Array &p_do, const godot::Array &p_undo);
	void show_references(godot::Object *p_code_edit);
	void ask_name(godot::Object *p_code_edit);
	void rename_to(const godot::String &p_name);
	void apply_previewed();
	void apply(const std::string &p_path, uint32_t p_offset, const std::string &p_name, const std::vector<compiler::RenameEdit> &p_edits, bool p_record, bool p_undo);
	godot::String rename_declaration(int p_kind, const godot::String &p_scope, const godot::String &p_name, const godot::String &p_new, const godot::Array &p_candidates, bool p_undo, bool p_apply);
	void files_written(const std::set<std::string> &p_files);
	std::set<std::string> saved_after; // Written by the editor, taken as saved once read back (files_written()).
	void saved_written(const std::set<std::string> &p_files);
	void run_headless(const godot::String &p_command);
	std::pair<std::string, int> migrate(const std::vector<std::string> &p_args);
	std::pair<std::string, int> migrate_back(const std::vector<std::string> &p_paths, bool p_dry_run, bool p_allow_dirty);
	std::pair<std::string, int> write_migrated(const std::map<std::string, std::string> &p_renamed, const std::map<std::string, std::string> &p_texts, std::string p_output, const std::function<std::string(int)> &p_summary, bool p_dry_run, bool p_allow_dirty, bool p_failed);
	std::optional<std::pair<std::string, int>> run_command(const std::string &p_command, const std::vector<std::string> &p_args, const std::set<std::string> &p_conflicts);
	void files_changed();
	void scene_saved(const godot::String &p_path);
	void scenes_paused();
	void changed();
	void check_shown();
	void poll_lsp();

protected:
	static void _bind_methods();

public:
	void _enter_tree() override;
	void _exit_tree() override;
	void _input(const godot::Ref<godot::InputEvent> &p_event) override;
	void _shortcut_input(const godot::Ref<godot::InputEvent> &p_event) override;
	bool _build() override;
	void _save_external_data() override;
};

} // namespace petrichor
