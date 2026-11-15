#include "adapter/editor/project_files.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>

using namespace godot;

namespace petrichor {

// As the editor's file system scans
// (editor/file_system/editor_file_system.cpp:1174-1185, 3488-3508; ed1daf0).
// get_next() leaves out hidden entries
// (core/io/dir_access.cpp:608-614), and the editor puts a .gdignore file in
// the project's data directory (editor/file_system/editor_paths.cpp:269-279).
static void collect(const String &p_dir, const PackedStringArray &p_extensions, PackedStringArray &r_paths) {
	const Ref<DirAccess> dir = DirAccess::open(p_dir);
	if (dir.is_null()) {
		return;
	}
	dir->list_dir_begin();
	for (String name = dir->get_next(); !name.is_empty(); name = dir->get_next()) {
		const String path = p_dir.path_join(name);
		if (!dir->current_is_dir()) {
			if (p_extensions.has(name.get_extension().to_lower())) {
				r_paths.push_back(path);
			}
		} else if (!name.begins_with(".") && !FileAccess::file_exists(path.path_join("project.godot")) && !FileAccess::file_exists(path.path_join(".gdignore"))) {
			collect(path, p_extensions, r_paths);
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
