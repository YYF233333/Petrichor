#pragma once

#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

namespace petrichor {

// The project's files with one of `p_extensions` (lower case, without the
// dot), as the editor's file system sees them: without hidden entries, other
// projects and directories with a .gdignore file.
godot::PackedStringArray project_files(const godot::PackedStringArray &p_extensions);

} // namespace petrichor
