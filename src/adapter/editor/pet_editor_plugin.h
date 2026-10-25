#pragma once

#include <godot_cpp/classes/editor_export_plugin.hpp>
#include <godot_cpp/classes/editor_plugin.hpp>

namespace petrichor {

// Replaces `.pet` sources with compiled scripts in exported games (spec §1.3).
class PetExportPlugin : public godot::EditorExportPlugin {
	GDCLASS(PetExportPlugin, godot::EditorExportPlugin);

protected:
	static void _bind_methods() {}

public:
	godot::String _get_name() const override;
	void _export_file(const godot::String &p_path, const godot::String &p_type, const godot::PackedStringArray &p_features) override;
};

class PetEditorPlugin : public godot::EditorPlugin {
	GDCLASS(PetEditorPlugin, godot::EditorPlugin);

	godot::Ref<PetExportPlugin> export_plugin;

protected:
	static void _bind_methods() {}

public:
	void _enter_tree() override;
	void _exit_tree() override;
};

} // namespace petrichor
