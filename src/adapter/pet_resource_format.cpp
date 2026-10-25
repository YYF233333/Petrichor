#include "adapter/pet_resource_format.h"

#include "adapter/pet_compiled.h"
#include "adapter/pet_script.h"

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
	if (p_path.get_extension().to_lower() == "petc") {
		const PackedByteArray bytes = FileAccess::get_file_as_bytes(p_path);
		if (bytes.is_empty()) {
			return FileAccess::get_open_error();
		}
		if (!decode_compiled(bytes, source)) {
			ERR_PRINT(vformat("%s: not a compiled Petrichor script, or compiled by another version.", p_path));
			return ERR_FILE_CORRUPT;
		}
	} else {
		source = FileAccess::get_file_as_string(p_path);
		const Error error = FileAccess::get_open_error();
		if (error != OK) {
			return error;
		}
	}

	Ref<PetScript> script;
	script.instantiate();
	script->set_source(source);
	if (!script->_is_valid()) {
		ERR_PRINT(vformat("%s: unknown base class '%s'.", p_original_path, script->_get_instance_base_type()));
	}
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
