#include "adapter/editor/project_files.h"

#include <godot_cpp/classes/dir_access.hpp>

using namespace godot;

namespace petrichor {

static void collect(const String &p_dir, const PackedStringArray &p_extensions, PackedStringArray &r_paths) {
	const Ref<DirAccess> dir = DirAccess::open(p_dir);
	if (dir.is_null() || dir->file_exists(".gdignore")) {
		return;
	}
	dir->list_dir_begin();
	for (String name = dir->get_next(); !name.is_empty(); name = dir->get_next()) {
		if (name.begins_with(".")) {
			continue;
		}
		const String path = p_dir.path_join(name);
		if (dir->current_is_dir()) {
			collect(path, p_extensions, r_paths);
		} else if (p_extensions.has(name.get_extension().to_lower())) {
			r_paths.push_back(path);
		}
	}
	dir->list_dir_end();
}

PackedStringArray project_files(const PackedStringArray &p_extensions) {
	PackedStringArray paths;
	collect("res://", p_extensions, paths);
	return paths;
}

} // namespace petrichor
