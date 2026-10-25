#include "adapter/editor/pet_editor_plugin.h"

#include "adapter/editor/script_compiler.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cstring>

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
	const std::shared_ptr<const ClassImage> image = compile_script(p_path, source, nullptr, true);
	ERR_FAIL_COND_MSG(!image, vformat("%s has errors and is left out of the export.", p_path));

	const std::vector<uint8_t> data = bytecode::serialize(image->module);
	PackedByteArray bytes;
	bytes.resize(int64_t(data.size()));
	std::memcpy(bytes.ptrw(), data.data(), data.size());
	// With remapping, the engine leaves out the source and loads the bytecode
	// when the game asks for the `.pet` path.
	add_file(p_path.get_basename() + ".petc", bytes, true);
}

void PetEditorPlugin::_enter_tree() {
	export_plugin.instantiate();
	add_export_plugin(export_plugin);
	if (OS::get_singleton()->get_cmdline_user_args().has("--petrichor-build")) {
		callable_mp(this, &PetEditorPlugin::run_build_command).call_deferred();
	}
}

void PetEditorPlugin::_exit_tree() {
	remove_export_plugin(export_plugin);
	export_plugin.unref();
}

bool PetEditorPlugin::_build() {
	// Compile errors keep the project from running (spec §1.4).
	return build_project() == 0;
}

void PetEditorPlugin::run_build_command() {
	int count = 0;
	const int failed = build_project(&count);
	UtilityFunctions::print(vformat("petrichor build: %d script(s), %d with errors", count, failed));
	// A non-zero exit code on compile errors (spec §13.1).
	get_tree()->quit(failed > 0 ? 1 : 0);
}

} // namespace petrichor
