#include "adapter/pet_resource_format.h"

#include "adapter/pet_compiled.h"
#include "adapter/pet_script.h"

#ifdef TOOLS_ENABLED
#include "adapter/editor/script_compiler.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#endif

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/error_macros.hpp>

using namespace godot;

namespace petrichor {

/* Loader */

PackedStringArray PetResourceLoader::_get_recognized_extensions() const {
	PackedStringArray result;
	result.push_back("pet");
	result.push_back("petc");
	return result;
}

bool PetResourceLoader::_handles_type(const StringName &p_type) const {
	return p_type == StringName("Script") || p_type == PetScript::get_class_static();
}

String PetResourceLoader::_get_resource_type(const String &p_path) const {
	const String extension = p_path.get_extension().to_lower();
	if (extension == "pet" || extension == "petc") {
		return PetScript::get_class_static();
	}
	return String();
}

Variant PetResourceLoader::_load(const String &p_path, const String &p_original_path, bool p_use_sub_threads, int32_t p_cache_mode) const {
	String source;
	String error;
	std::shared_ptr<const ClassImage> image;
	if (p_path.get_extension().to_lower() == "petc") {
		// An exported game: the export plugin put the bytecode in place of the source.
		image = load_compiled(p_path, p_original_path, nullptr, error);
	} else {
		source = FileAccess::get_file_as_string(p_path);
		const Error open_error = FileAccess::get_open_error();
		if (open_error != OK) {
			return open_error;
		}
#ifdef TOOLS_ENABLED
		if (Engine::get_singleton()->is_editor_hint()) {
			// Inside a load, so without the scenes that use the script; the
			// full compilation comes when the script is validated, saved or
			// built.
			image = compile_script(p_original_path, source, nullptr, true, CompileMode::Declarations);
		} else
#endif
		{
			// The game reads what the editor compiled (architecture §2.4).
			image = load_compiled(cache_path(p_original_path), p_original_path, &source, error);
		}
	}
	if (!error.is_empty()) {
		ERR_PRINT(vformat("%s %s", p_original_path, error));
	}

	Ref<PetScript> script;
	script.instantiate();
	script->set_loaded(source, image);
	return script;
}

/* Saver */

Error PetResourceSaver::_save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags) {
	const Ref<PetScript> script = p_resource;
	ERR_FAIL_COND_V(script.is_null(), ERR_INVALID_PARAMETER);

	const Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_null()) {
		return FileAccess::get_open_error();
	}
	file->store_string(script->_get_source_code());
	file->close();
#ifdef TOOLS_ENABLED
	if (Engine::get_singleton()->is_editor_hint()) {
		const std::shared_ptr<const ClassImage> image = compile_script(p_path, script->_get_source_code(), nullptr, false);
		// Keeps the bytecode cache current, so that a running game can reload
		// the script right after the editor saves it (spec §12.4).
		const String compiled_path = cache_path(p_path); // Empty outside res://.
		if (image && !compiled_path.is_empty()) {
			write_compiled(compiled_path, *image);
		}
		// The script takes the new class when the editor reloads scripts on
		// save, as GDScript's saver does (modules/gdscript/gdscript_resource_format.cpp:167-169).
		if (EditorInterface::get_singleton()->get_editor_settings()->get_setting("text_editor/behavior/files/auto_reload_and_parse_scripts_on_save")) {
			script->set_image(image);
		}
	}
#endif
	return OK;
}

bool PetResourceSaver::_recognize(const Ref<Resource> &p_resource) const {
	return Object::cast_to<PetScript>(p_resource.ptr()) != nullptr;
}

PackedStringArray PetResourceSaver::_get_recognized_extensions(const Ref<Resource> &p_resource) const {
	PackedStringArray result;
	if (_recognize(p_resource)) {
		result.push_back("pet");
	}
	return result;
}

} // namespace petrichor
