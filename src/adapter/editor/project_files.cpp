#include "adapter/editor/project_files.h"

#include "runtime/text.h"

#include <godot_cpp/classes/project_settings.hpp>

#include <algorithm>
#include <vector>

#ifdef _WIN32
#include <cwchar>
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#endif

using namespace godot;

namespace petrichor {

namespace {

struct Entry {
	std::string name;
	bool directory = false;
	bool hidden = false;
	FileStat stat;
};

#ifdef _WIN32
// With the prefix that lifts MAX_PATH, as the engine's paths
// (drivers/windows/dir_access_windows.cpp:71-87; ed1daf0).
std::wstring native(std::string p_path) {
	std::replace(p_path.begin(), p_path.end(), '/', '\\');
	if (p_path.compare(0, 2, "\\\\") != 0) {
		p_path = "\\\\?\\" + p_path;
	}
	std::wstring wide(size_t(MultiByteToWideChar(CP_UTF8, 0, p_path.data(), int(p_path.size()), nullptr, 0)), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, p_path.data(), int(p_path.size()), wide.data(), int(wide.size()));
	return wide;
}

FileStat stat_of(const FILETIME &p_time, DWORD p_high, DWORD p_low) {
	return { int64_t(uint64_t(p_time.dwHighDateTime) << 32 | p_time.dwLowDateTime), int64_t(uint64_t(p_high) << 32 | p_low) };
}
#else
FileStat stat_of(const struct stat &p_info) {
#ifdef __APPLE__
	const timespec &time = p_info.st_mtimespec;
#else
	const timespec &time = p_info.st_mtim;
#endif
	return { int64_t(time.tv_sec) * 1000000000 + time.tv_nsec, int64_t(p_info.st_size) };
}
#endif

// Hidden as the engine's directory listing tells: by the attribute on
// Windows, by a leading dot elsewhere, and on macOS by the flag too
// (drivers/windows/dir_access_windows.cpp:111, drivers/unix/dir_access_unix.cpp:688-690,
// platform/macos/dir_access_macos.mm:83-90; ed1daf0).
std::vector<Entry> entries(const std::string &p_directory) {
	std::vector<Entry> found;
#ifdef _WIN32
	WIN32_FIND_DATAW data;
	const HANDLE handle = FindFirstFileExW(native(p_directory + "*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
	if (handle == INVALID_HANDLE_VALUE) {
		return found;
	}
	do {
		const int length = int(wcslen(data.cFileName));
		std::string name(size_t(WideCharToMultiByte(CP_UTF8, 0, data.cFileName, length, nullptr, 0, nullptr, nullptr)), '\0');
		WideCharToMultiByte(CP_UTF8, 0, data.cFileName, length, name.data(), int(name.size()), nullptr, nullptr);
		found.push_back({ std::move(name), (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, (data.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0, stat_of(data.ftLastWriteTime, data.nFileSizeHigh, data.nFileSizeLow) });
	} while (FindNextFileW(handle, &data));
	FindClose(handle);
#else
	DIR *directory = opendir(p_directory.c_str());
	if (!directory) {
		return found;
	}
	while (const dirent *each = readdir(directory)) {
		struct stat info;
		if (fstatat(dirfd(directory), each->d_name, &info, 0) == 0) {
			bool hidden = each->d_name[0] == '.';
#ifdef __APPLE__
			hidden = hidden || (info.st_flags & UF_HIDDEN) != 0;
#endif
			found.push_back({ each->d_name, S_ISDIR(info.st_mode) != 0, hidden, stat_of(info) });
		}
	}
	closedir(directory);
#endif
	found.erase(std::remove_if(found.begin(), found.end(), [](const Entry &p_entry) { return p_entry.name == "." || p_entry.name == ".."; }), found.end());
	return found;
}

// As the editor's file system scans: it skips hidden entries, directories
// whose names begin with a dot, and directories with another project or a
// .gdignore file. The editor puts a .gdignore file in the project's data
// directory (editor/file_system/editor_file_system.cpp:1174-1185, 3488-3508,
// editor/file_system/editor_paths.cpp:269-279; ed1daf0).
void walk(const std::string &p_directory, const std::string &p_path, bool p_root, std::map<std::string, FileStat> &r_files) {
	const std::vector<Entry> found = entries(p_directory);
	const auto skips = [](const Entry &p_entry) { return !p_entry.directory && (p_entry.name == "project.godot" || p_entry.name == ".gdignore"); };
	if (!p_root && std::any_of(found.begin(), found.end(), skips)) {
		return;
	}
	for (const Entry &entry : found) {
		if (entry.hidden) {
			continue;
		}
		if (!entry.directory) {
			r_files.emplace(p_path + entry.name, entry.stat);
		} else if (entry.name[0] != '.') {
			walk(p_directory + entry.name + "/", p_path + entry.name + "/", false, r_files);
		}
	}
}

} // namespace

std::map<std::string, FileStat> walk_project(const std::string &p_root) {
	std::map<std::string, FileStat> files;
	walk(p_root, "res://", true, files);
	return files;
}

std::string project_directory() {
	const std::string path = to_utf8(ProjectSettings::get_singleton()->globalize_path("res://"));
	return path.back() == '/' ? path : path + "/";
}

PackedStringArray project_files(const PackedStringArray &p_extensions) {
	PackedStringArray paths;
	for (const auto &[path, stat] : walk_project(project_directory())) {
		const String each = to_godot(path);
		if (p_extensions.has(each.get_extension().to_lower())) {
			paths.push_back(each);
		}
	}
	return paths;
}

std::optional<FileStat> file_stat(const std::string &p_path) {
	const std::string path = to_utf8(ProjectSettings::get_singleton()->globalize_path(to_godot(p_path)));
#ifdef _WIN32
	WIN32_FILE_ATTRIBUTE_DATA data;
	if (!GetFileAttributesExW(native(path).c_str(), GetFileExInfoStandard, &data)) {
		return std::nullopt;
	}
	return stat_of(data.ftLastWriteTime, data.nFileSizeHigh, data.nFileSizeLow);
#else
	struct stat info;
	if (stat(path.c_str(), &info) != 0) {
		return std::nullopt;
	}
	return stat_of(info);
#endif
}

bool file_empty(const std::string &p_path) {
	const std::optional<FileStat> stat = file_stat(p_path);
	return stat && stat->size == 0;
}

} // namespace petrichor
