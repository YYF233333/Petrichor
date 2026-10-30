#include "adapter/pet_compiled.h"

#include "adapter/pet_script.h"
#include "runtime/text.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/core/error_macros.hpp>

#include <algorithm>
#include <cstring>

using namespace godot;

namespace petrichor {

String cache_path(const String &p_script_path) {
	if (!p_script_path.begins_with("res://")) {
		return String();
	}
	return "res://.godot/petrichor/" + p_script_path.trim_prefix("res://") + "c";
}

std::shared_ptr<const ClassImage> load_compiled(const String &p_compiled_path, const String &p_script_path, const String *p_source, String &r_error, Ref<PetScript> *r_base) {
	thread_local std::vector<String> loading;
	if (std::find(loading.begin(), loading.end(), p_script_path) != loading.end()) {
		r_error = "Inheritance cycle in compiled scripts.";
		return nullptr;
	}
	loading.push_back(p_script_path);
	struct Pop {
		std::vector<String> &paths;
		~Pop() { paths.pop_back(); }
	} pop{ loading };
	const PackedByteArray bytes = FileAccess::get_file_as_bytes(p_compiled_path);
	if (bytes.is_empty()) {
		r_error = "has not been compiled. Run the project from the editor, or build it with: godot --headless -e --path <project> -- --petrichor-build";
		return nullptr;
	}
	bytecode::Module module;
	std::string error;
	if (!bytecode::deserialize(bytes.ptr(), size_t(bytes.size()), module, error)) {
		r_error = to_godot(error) + ". Rebuild the project.";
		return nullptr;
	}
	if (p_source && module.source_hash != bytecode::hash_source(to_utf8(*p_source))) {
		r_error = "has changed since it was compiled. Run the project from the editor, or rebuild it.";
		return nullptr;
	}
	if (module.declarations_only) {
		r_error = "was compiled without its function bodies. Rebuild the project.";
		return nullptr;
	}
	Ref<PetScript> base;
	if (!module.base_script.empty()) {
		base = ResourceLoader::get_singleton()->load(to_godot(module.base_script), "Script");
		if (base.is_null() || !base->get_image()) {
			r_error = "Base script could not be loaded. Rebuild the project.";
			return nullptr;
		}
	}
	if (r_base) {
		*r_base = base;
	}
	return ClassImage::load(std::move(module), p_script_path, r_error, base.is_valid() ? base->get_image() : nullptr);
}

bool write_compiled(const String &p_compiled_path, const ClassImage &p_image) {
	ERR_FAIL_COND_V_MSG(p_image.module.declarations_only, false, "Only a full compilation is written to " + p_compiled_path);
	const Error dir_error = DirAccess::make_dir_recursive_absolute(p_compiled_path.get_base_dir());
	ERR_FAIL_COND_V_MSG(dir_error != OK, false, "Cannot create " + p_compiled_path.get_base_dir());
	const Ref<FileAccess> file = FileAccess::open(p_compiled_path, FileAccess::WRITE);
	ERR_FAIL_COND_V_MSG(file.is_null(), false, "Cannot write " + p_compiled_path);
	const std::vector<uint8_t> data = bytecode::serialize(p_image.module);
	PackedByteArray bytes;
	bytes.resize(int64_t(data.size()));
	std::memcpy(bytes.ptrw(), data.data(), data.size());
	file->store_buffer(bytes);
	return true;
}

} // namespace petrichor
