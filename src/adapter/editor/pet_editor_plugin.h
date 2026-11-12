#pragma once

#include <godot_cpp/classes/editor_context_menu_plugin.hpp>
#include <godot_cpp/classes/editor_export_plugin.hpp>
#include <godot_cpp/classes/editor_plugin.hpp>

#include <cstdint>

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

// Compiles the project before it runs (architecture §2.4), provides the
// headless commands (spec §13.1, §13.2):
//   godot --headless -e --path <project> -- --petrichor-build
//   godot --headless -e --path <project> -- --petrichor-format [--check]
// and has the nodes ask again for their errors when a file changes.
class PetEditorPlugin : public godot::EditorPlugin {
	GDCLASS(PetEditorPlugin, godot::EditorPlugin);

	godot::Ref<PetExportPlugin> export_plugin;
	godot::Ref<PetFormatMenu> format_menu;

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
