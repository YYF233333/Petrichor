#pragma once

#include <godot_cpp/classes/editor_export_plugin.hpp>
#include <godot_cpp/classes/editor_plugin.hpp>

namespace petrichor {

// Replaces `.pet` sources with bytecode in exported games (spec §1.3).
class PetExportPlugin : public godot::EditorExportPlugin {
	GDCLASS(PetExportPlugin, godot::EditorExportPlugin);

protected:
	static void _bind_methods() {}

public:
	godot::String _get_name() const override;
	void _export_file(const godot::String &p_path, const godot::String &p_type, const godot::PackedStringArray &p_features) override;
};

// Compiles the project before it runs (architecture §2.4), and provides the
// headless build command (spec §13.1):
//   godot --headless -e --path <project> -- --petrichor-build
class PetEditorPlugin : public godot::EditorPlugin {
	GDCLASS(PetEditorPlugin, godot::EditorPlugin);

	godot::Ref<PetExportPlugin> export_plugin;

	void run_build_command();

protected:
	static void _bind_methods() {}

public:
	void _enter_tree() override;
	void _exit_tree() override;
	bool _build() override;
};

} // namespace petrichor
