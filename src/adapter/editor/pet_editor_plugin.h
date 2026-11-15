#pragma once

#include "compiler/navigation.h"

#include <godot_cpp/classes/editor_context_menu_plugin.hpp>
#include <godot_cpp/classes/editor_export_plugin.hpp>
#include <godot_cpp/classes/editor_plugin.hpp>
#include <godot_cpp/classes/editor_syntax_highlighter.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/shortcut.hpp>
#include <godot_cpp/classes/tree.hpp>
#include <godot_cpp/classes/v_box_container.hpp>

#include <cstdint>
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
// it, and finding the references of the name at the caret (spec §13.2).
class PetCodeMenu : public godot::EditorContextMenuPlugin {
	GDCLASS(PetCodeMenu, godot::EditorContextMenuPlugin);

	void format(godot::Object *p_code_edit);

protected:
	static void _bind_methods() {}

public:
	godot::Ref<godot::Shortcut> find_references;

	void _popup_menu(const godot::PackedStringArray &p_paths) override;
};

// The places that finding references lists, in the bottom panel by file, as
// the results of Find in Files show (editor/script/find_in_files.cpp:791-924;
// ed1daf0). Selecting one opens it.
class PetReferences : public godot::VBoxContainer {
	GDCLASS(PetReferences, godot::VBoxContainer);

	godot::Tree *tree = nullptr;
	std::vector<compiler::Occurrence> found;

	void fill();
	void open();
	void highlight(godot::Object *p_item, const godot::Rect2 &p_rect);

protected:
	static void _bind_methods() {}
	void _notification(int p_what);

public:
	PetReferences();
	void set_found(std::vector<compiler::Occurrence> p_found);
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

// Compiles the project before it runs (architecture §2.4), provides the
// headless commands (spec §13.1, §13.2):
//   godot --headless -e --path <project> -- --petrichor-build
//   godot --headless -e --path <project> -- --petrichor-format [--check]
// has the nodes ask again for their errors when a file changes, and shows
// the references found in its panel.
class PetEditorPlugin : public godot::EditorPlugin {
	GDCLASS(PetEditorPlugin, godot::EditorPlugin);

	godot::Ref<PetExportPlugin> export_plugin;
	godot::Ref<PetCodeMenu> code_menu;
	godot::Ref<PetHighlighter> highlighter;
	PetReferences *references = nullptr;

	void show_references(godot::Object *p_code_edit);
	bool waits_for_scan(void (PetEditorPlugin::*p_command)());
	void run_build_command();
	void run_format_command();
	void resource_saved(godot::Object *p_resource);
	void scene_saved(const godot::String &p_path);
	void files_or_settings_changed();

protected:
	static void _bind_methods() {}

public:
	void _enter_tree() override;
	void _exit_tree() override;
	void _shortcut_input(const godot::Ref<godot::InputEvent> &p_event) override;
	bool _build() override;
};

} // namespace petrichor
