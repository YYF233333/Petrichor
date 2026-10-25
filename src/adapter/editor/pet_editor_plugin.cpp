#include "adapter/editor/pet_editor_plugin.h"

#include "adapter/pet_compiled.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/error_macros.hpp>

using namespace godot;

namespace petrichor {

String PetExportPlugin::_get_name() const {
	return "Petrichor";
}

void PetExportPlugin::_export_file(const String &p_path, const String &p_type, const PackedStringArray &p_features) {
	if (p_path.get_extension().to_lower() != "pet") {
		return;
	}
	const String source = FileAccess::get_file_as_string(p_path);
	ERR_FAIL_COND_MSG(FileAccess::get_open_error() != OK, vformat("Cannot read %s for export.", p_path));
	// With remapping, the engine leaves out the source and loads the compiled
	// file when the game asks for the `.pet` path.
	add_file(p_path.get_basename() + ".petc", encode_compiled(source), true);
}

void PetEditorPlugin::_enter_tree() {
	export_plugin.instantiate();
	add_export_plugin(export_plugin);
}

void PetEditorPlugin::_exit_tree() {
	remove_export_plugin(export_plugin);
	export_plugin.unref();
}

} // namespace petrichor
