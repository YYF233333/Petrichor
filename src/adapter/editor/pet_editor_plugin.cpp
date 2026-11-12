#include "adapter/editor/pet_editor_plugin.h"

#include "adapter/editor/project_files.h"
#include "adapter/editor/scene_index.h"
#include "adapter/editor/script_compiler.h"
#include "adapter/pet_script.h"
#include "compiler/formatter.h"
#include "runtime/text.h"

#include <godot_cpp/classes/code_edit.hpp>
#include <godot_cpp/classes/config_file.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/editor_export_platform.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_toaster.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/gd_extension_manager.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/script_editor.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cstring>

using namespace godot;

namespace petrichor {

namespace {

// The features that the engine's export of extensions takes as architectures
// (editor/export/gdextension_export_plugin.h:68-77; ed1daf0).
bool is_architecture(const String &p_tag) {
	for (const char *architecture : { "x86_32", "x86_64", "arm32", "arm64", "rv64", "ppc64", "wasm32", "loongarch64", "universal" }) {
		if (p_tag == architecture) {
			return true;
		}
	}
	return false;
}

// The key of [libraries] whose library the engine exports for an architecture:
// the one with the most tags, each the architecture or a feature of the export
// that is none, and the first of equals
// (core/extension/gdextension_library_loader.cpp:82-97,
// editor/export/gdextension_export_plugin.h:79-110; ed1daf0).
String library_key(const PackedStringArray &p_keys, const PackedStringArray &p_features, const String &p_architecture) {
	String best;
	int64_t most = 0;
	for (const String &key : p_keys) {
		const PackedStringArray tags = key.split(".");
		bool met = true;
		for (const String &tag : tags) {
			const String name = tag.strip_edges();
			met = met && (name == p_architecture || (p_features.has(name) && !is_architecture(name)));
		}
		if (met && tags.size() > most) {
			best = key;
			most = tags.size();
		}
	}
	return best;
}

} // namespace

String PetExportPlugin::_get_name() const {
	return "Petrichor";
}

void PetExportPlugin::_export_begin(const PackedStringArray &p_features, bool p_is_debug, const String &p_path, uint32_t p_flags) {
	const Ref<EditorExportPlatform> platform = get_export_platform();
	const auto fail = [&](const String &p_what, const String &p_remedy) {
		platform->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, "Petrichor", vformat("%s The exported game cannot load Petrichor, so its .pet scripts will not run. %s", p_what, p_remedy));
	};
	// platform/web/export/export_plugin.cpp:362-366 (ed1daf0).
	if (p_features.has("web_noextensions")) {
		fail("This Web export has \"Extensions Support\" turned off.", "Turn it on in the options of the export preset.");
	}
	// Without a library for the target, the engine exports with a warning, and
	// the game fails when it runs
	// (editor/export/gdextension_export_plugin.h:161-165; ed1daf0).
	for (const String &extension : GDExtensionManager::get_singleton()->get_loaded_extensions()) {
		// This library's own file names its entry point, wherever the project
		// keeps it (src/adapter/register_types.cpp).
		Ref<ConfigFile> config;
		config.instantiate();
		config->load(extension);
		if (String(config->get_value("configuration", "entry_symbol", String())) != "petrichor_library_init") {
			continue;
		}
		const PackedStringArray keys = config->get_section_keys("libraries");
		PackedStringArray architectures;
		for (const String &feature : p_features) {
			if (is_architecture(feature)) {
				architectures.push_back(feature);
			}
		}
		if (architectures.is_empty()) {
			architectures.push_back("unknown_arch");
		}
		for (const String &architecture : architectures) {
			// A universal library serves each architecture of the export.
			if (architecture == "universal") {
				continue;
			}
			String key = library_key(keys, p_features, architecture);
			if (key.is_empty() && architectures.has("universal")) {
				key = library_key(keys, p_features, "universal");
			}
			const String target = vformat("a %s export to %s %s", p_is_debug ? "debug" : "release", platform->get_os_name(), architecture);
			if (key.is_empty()) {
				fail(vformat("%s has no library for %s.", extension, target), "Install a release of Petrichor that supports this target.");
				continue;
			}
			// core/extension/gdextension_library_loader.cpp:100-102 (ed1daf0).
			String library = config->get_value("libraries", key);
			if (library.is_relative_path()) {
				library = extension.get_base_dir().path_join(library);
			}
			// On macOS a library may be a framework, a directory.
			if (!FileAccess::file_exists(library) && !DirAccess::dir_exists_absolute(library)) {
				fail(vformat("%s names %s as the library for %s, and that file does not exist.", extension, library, target), "Install Petrichor again, with every file of its release.");
			}
		}
	}
}

void PetExportPlugin::_export_file(const String &p_path, const String &p_type, const PackedStringArray &p_features) {
	// An error message fails the export in the editor's window
	// (editor/export/editor_export_platform.cpp:131-148; ed1daf0).
	const String extension = p_path.get_extension().to_lower();
	if (p_type == "PackedScene" || extension == "tres" || extension == "res") {
		for (const compiler::Diagnostic &error : check_scene(p_path, true)->errors) {
			get_export_platform()->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, "Petrichor", to_godot(error.message));
		}
		return;
	}
	if (extension != "pet") {
		return;
	}
	// Without skip(), the engine would export the source as it is; exported
	// games contain only bytecode (spec §1.3).
	const String source = FileAccess::get_file_as_string(p_path);
	if (FileAccess::get_open_error() != OK) {
		skip();
		get_export_platform()->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, "Petrichor", vformat("Cannot read %s for export.", p_path));
		return;
	}
	const std::shared_ptr<const ClassImage> image = compile_script(p_path, source, nullptr, true);
	if (!image) {
		skip();
		get_export_platform()->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, "Petrichor", vformat("%s has errors and is left out of the export.", p_path));
		return;
	}
	const std::vector<uint8_t> data = bytecode::serialize(image->module);
	PackedByteArray bytes;
	bytes.resize(int64_t(data.size()));
	std::memcpy(bytes.ptrw(), data.data(), data.size());
	// With remapping, the engine leaves out the source and loads the bytecode
	// when the game asks for the `.pet` path.
	add_file(p_path.get_basename() + ".petc", bytes, true);
}

void PetFormatMenu::_popup_menu(const PackedStringArray &p_paths) {
	// The path given is the code editor's node, not the script's
	// (editor/script/script_text_editor.cpp:2600-2601; ed1daf0).
	const Ref<Script> script = EditorInterface::get_singleton()->get_script_editor()->get_current_script();
	if (Object::cast_to<PetScript>(script.ptr())) {
		add_context_menu_item("Format", callable_mp(this, &PetFormatMenu::format));
	}
}

void PetFormatMenu::format(Object *p_code_edit) {
	CodeEdit *code_edit = Object::cast_to<CodeEdit>(p_code_edit);
	ERR_FAIL_NULL(code_edit);
	const String text = code_edit->get_text();
	std::string error;
	const std::optional<std::string> result = compiler::format(to_utf8(text), &error);
	if (!result) {
		EditorInterface::get_singleton()->get_editor_toaster()->push_toast(String::utf8(error.c_str()), EditorToaster::SEVERITY_WARNING);
		return;
	}
	const String formatted = String::utf8(result->data(), int64_t(result->size()));
	if (formatted == text) {
		return;
	}
	const int line = code_edit->get_caret_line();
	const int column = code_edit->get_caret_column();
	const double scroll = code_edit->get_v_scroll();
	// A single step to undo.
	code_edit->begin_complex_operation();
	code_edit->select_all();
	code_edit->insert_text_at_caret(formatted);
	code_edit->end_complex_operation();
	code_edit->set_caret_line(std::min(line, code_edit->get_line_count() - 1));
	code_edit->set_caret_column(column);
	code_edit->set_v_scroll(scroll);
}

void PetEditorPlugin::_enter_tree() {
	export_plugin.instantiate();
	add_export_plugin(export_plugin);
	format_menu.instantiate();
	add_context_menu_plugin(EditorContextMenuPlugin::CONTEXT_SLOT_SCRIPT_EDITOR_CODE, format_menu);
	// editor/plugins/editor_plugin.cpp:284-289 (ed1daf0).
	connect("resource_saved", callable_mp(this, &PetEditorPlugin::resource_saved));
	connect("scene_saved", callable_mp(this, &PetEditorPlugin::scene_saved));
	EditorInterface::get_singleton()->get_resource_filesystem()->connect("filesystem_changed", callable_mp(this, &PetEditorPlugin::filesystem_changed));
	const PackedStringArray args = OS::get_singleton()->get_cmdline_user_args();
	if (args.has("--petrichor-build")) {
		callable_mp(this, &PetEditorPlugin::run_build_command).call_deferred();
	} else if (args.has("--petrichor-format")) {
		callable_mp(this, &PetEditorPlugin::run_format_command).call_deferred();
	}
}

void PetEditorPlugin::resource_saved(Object *p_resource) {
	if (const Resource *resource = Object::cast_to<Resource>(p_resource)) {
		SceneIndex::get().forget(to_utf8(resource->get_path()));
	}
	project_changed();
}

void PetEditorPlugin::scene_saved(const String &p_path) {
	SceneIndex::get().forget(to_utf8(p_path));
	project_changed();
}

void PetEditorPlugin::filesystem_changed() {
	project_changed();
}

void PetEditorPlugin::_exit_tree() {
	remove_context_menu_plugin(format_menu);
	format_menu.unref();
	remove_export_plugin(export_plugin);
	export_plugin.unref();
}

bool PetEditorPlugin::_build() {
	// Compile errors keep the project from running (spec §1.4).
	return build_project() == 0;
}

// The first scan imports the project, and lasts until its imports are done
// (editor/file_system/editor_file_system.cpp:1131-1137, 1838-1840), so a
// command on a fresh copy runs once it ends.
bool PetEditorPlugin::waits_for_scan(void (PetEditorPlugin::*p_command)()) {
	if (!EditorInterface::get_singleton()->get_resource_filesystem()->is_scanning()) {
		return false;
	}
	get_tree()->connect("process_frame", callable_mp(this, p_command), CONNECT_ONE_SHOT);
	return true;
}

void PetEditorPlugin::run_build_command() {
	if (waits_for_scan(&PetEditorPlugin::run_build_command)) {
		return;
	}
	int scripts = 0;
	int scenes = 0;
	int resources = 0;
	const int failed = build_project(&scripts, &scenes, &resources, true);
	UtilityFunctions::print(vformat("petrichor build: %d script(s), %d scene(s), %d resource file(s), %d with errors", scripts, scenes, resources, failed));
	// A non-zero exit code on compile errors (spec §13.1).
	get_tree()->quit(failed > 0 ? 1 : 0);
}

void PetEditorPlugin::run_format_command() {
	if (waits_for_scan(&PetEditorPlugin::run_format_command)) {
		return;
	}
	const bool check = OS::get_singleton()->get_cmdline_user_args().has("--check");
	const PackedStringArray paths = project_files(PackedStringArray({ "pet" }));
	int changed = 0;
	int failed = 0;
	for (const String &path : paths) {
		// As bytes, which keeps the file's line breaks.
		const PackedByteArray bytes = FileAccess::get_file_as_bytes(path);
		const std::string source = bytes.is_empty() ? std::string() : std::string(reinterpret_cast<const char *>(bytes.ptr()), size_t(bytes.size()));
		std::string error;
		const std::optional<std::string> result = compiler::format(source, &error);
		if (!result) {
			failed++;
			const CharString file = path.utf8();
			gdextension_interface::print_script_error(error.c_str(), "", file.get_data(), 0, false);
			continue;
		}
		if (*result == source) {
			continue;
		}
		changed++;
		if (check) {
			UtilityFunctions::print(vformat("%s is not formatted.", path));
			continue;
		}
		const Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
		if (file.is_null()) {
			failed++;
			ERR_PRINT(vformat("Cannot write %s.", path));
			continue;
		}
		PackedByteArray data;
		data.resize(int64_t(result->size()));
		if (!result->empty()) {
			std::memcpy(data.ptrw(), result->data(), result->size());
		}
		file->store_buffer(data);
	}
	UtilityFunctions::print(vformat("petrichor format: %d script(s), %d %s, %d with errors", int(paths.size()), changed, check ? "not formatted" : "formatted", failed));
	get_tree()->quit(failed > 0 || (check && changed > 0) ? 1 : 0);
}

} // namespace petrichor
