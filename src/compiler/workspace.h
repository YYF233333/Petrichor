#pragma once

#include "compiler/project_api.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>

namespace petrichor::compiler {

// When a result that is used later was computed (architecture §2.1): the
// workspace's revision then, and the files it holds places in. A file whose
// version is later has changed since.
struct Stamp {
	uint64_t revision = 0; // 0: not read from the workspace.
	std::set<std::string> files;
};

// The project as the editor has it (architecture §2.1): the scripts open in
// it and the scenes with unsaved changes, over the files. Every check in the
// editor reads it, so that all see one text of each file. Its revision goes up
// when what it gives changes. A file's version is the revision of its last
// change, so a version is never given twice, also to a file closed and opened
// again. While a file is open, a change on disk does not reach it.
class Workspace : public ProjectApi {
	friend class Analysis;

	const ProjectApi &disk;
	std::map<std::string, std::string> scripts;
	std::map<std::string, Scene> scenes;
	std::map<std::string, uint64_t> versions;
	uint64_t current = 1;
	uint64_t disk_revision = 0; // Of the last change on disk, which may be in any file not open.
	int passes = 0; // Of Analysis, which read it as it is.

	void write() const;
	void touch(const std::string &p_path);

public:
	explicit Workspace(const ProjectApi &p_disk) :
			disk(p_disk) {}

	void set_script(const std::string &p_path, std::string p_text);
	void drop_script(const std::string &p_path);
	void set_scene(const std::string &p_path, Scene p_scene);
	void drop_scene(const std::string &p_path);
	// Files changed on disk, which ones unknown.
	void disk_changed();

	uint64_t revision() const { return current; }
	uint64_t version(const std::string &p_path) const;
	bool changed(const Stamp &p_stamp) const;
	bool in_pass() const { return passes > 0; }

	std::vector<std::string> scene_paths() const override { return disk.scene_paths(); }
	std::optional<Scene> scene(const std::string &p_path) const override;
	std::string main_scene() const override { return disk.main_scene(); }
	std::optional<ResourceInfo> resource(const std::string &p_path) const override { return disk.resource(p_path); }
	std::string uid_path(const std::string &p_uid) const override { return disk.uid_path(p_uid); }
	std::vector<std::string> dependencies(const std::string &p_path) const override { return disk.dependencies(p_path); }
	std::string global_class_path(const std::string &p_name) const override { return disk.global_class_path(p_name); }
	std::string autoload_path(const std::string &p_name) const override { return disk.autoload_path(p_name); }
	std::map<std::string, std::string> global_class_paths() const override { return disk.global_class_paths(); }
	std::vector<std::string> autoload_names() const override { return disk.autoload_names(); }
	std::vector<std::string> resource_files() const override { return disk.resource_files(); }
	std::vector<std::string> input_actions() const override { return disk.input_actions(); }
	std::vector<std::string> script_paths() const override { return disk.script_paths(); }
	std::optional<std::string> script_source(const std::string &p_path) const override;
	std::vector<std::pair<bool, std::string>> literal_loads(const std::string &p_path) const override;
	std::optional<ForeignClass> foreign_class(const std::string &p_path, bool p_members) const override { return disk.foreign_class(p_path, p_members); }
};

} // namespace petrichor::compiler
