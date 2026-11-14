#pragma once

#include <godot_cpp/classes/editor_context_menu_plugin.hpp>
#include <godot_cpp/classes/editor_export_plugin.hpp>
#include <godot_cpp/classes/editor_plugin.hpp>
#include <godot_cpp/classes/editor_syntax_highlighter.hpp>

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

// Formats the script in the script editor from its context menu (spec §13.2).
class PetFormatMenu : public godot::EditorContextMenuPlugin {
	GDCLASS(PetFormatMenu, godot::EditorContextMenuPlugin);

	void format(godot::Object *p_code_edit);

protected:
	static void _bind_methods() {}

public:
	void _popup_menu(const godot::PackedStringArray &p_paths) override;
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
// and has the nodes ask again for their errors when a file changes.
class PetEditorPlugin : public godot::EditorPlugin {
	GDCLASS(PetEditorPlugin, godot::EditorPlugin);

	godot::Ref<PetExportPlugin> export_plugin;
	godot::Ref<PetFormatMenu> format_menu;
	godot::Ref<PetHighlighter> highlighter;

	bool waits_for_scan(void (PetEditorPlugin::*p_command)());
	void run_build_command();
	void run_format_command();
	void resource_saved(godot::Object *p_resource);
	void scene_saved(const godot::String &p_path);
	void filesystem_changed();

protected:
	static void _bind_methods() {}

public:
	void _enter_tree() override;
	void _exit_tree() override;
	bool _build() override;
};

} // namespace petrichor
