#pragma once

#include <godot_cpp/variant/packed_string_array.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace petrichor {

// When a file last changed, in the file system's precision, and its size.
struct FileStat {
	int64_t time = 0;
	int64_t size = 0;

	bool operator==(const FileStat &p_other) const { return time == p_other.time && size == p_other.size; }
	bool operator!=(const FileStat &p_other) const { return !(*this == p_other); }
};

// The project's files by res:// path, as the editor's file system sees them:
// without hidden entries, other projects and directories with a .gdignore
// file. It calls nothing of the engine, so a thread may walk them.
std::map<std::string, FileStat> walk_project(const std::string &p_root);
// The project's directory in UTF-8, with a "/" at its end, for walk_project().
std::string project_directory();
// The project's files with one of `p_extensions` (lower case, without the dot).
godot::PackedStringArray project_files(const godot::PackedStringArray &p_extensions);
// Of the file at a path of the engine's; nullopt if there is none.
std::optional<FileStat> file_stat(const std::string &p_path);
// Whether the file is there and empty, as a writer that truncates it first
// leaves it until it writes. Such a resource file is not loaded: the engine's
// loader reports an empty one as a parse error
// (core/variant/variant_parser.cpp:1879-1880, scene/resources/resource_format_text.cpp:1143-1147;
// ed1daf0).
bool file_empty(const std::string &p_path);

} // namespace petrichor
