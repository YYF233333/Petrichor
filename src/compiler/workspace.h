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
class Workspace : public ProjectLayer {
	friend class Analysis;

	std::map<std::string, std::string> scripts;
	std::map<std::string, Scene> scenes;
	std::map<std::string, uint64_t> versions;
	uint64_t current = 1;
	uint64_t disk_revision = 0; // Of the last change on disk, which may be in any file not open.
	int passes = 0; // Of Analysis, which read it as it is.

	void write() const;
	void touch(const std::string &p_path);
	void take_in(const std::string &p_path, bool p_taken);

public:
	explicit Workspace(const ProjectApi &p_disk) :
			ProjectLayer(p_disk) {}

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

	std::optional<Scene> scene(const std::string &p_path) const override;
	std::optional<std::string> script_source(const std::string &p_path) const override;
	std::vector<std::pair<bool, std::string>> literal_loads(const std::string &p_path) const override;
};

// Another project with texts of its own for some scripts, as an LSP session
// lays the documents it has open over its view (architecture §2.4).
class Overlay : public ProjectLayer {
public:
	std::map<std::string, std::string> scripts;

	explicit Overlay(const ProjectApi &p_under) :
			ProjectLayer(p_under) {}

	std::optional<std::string> script_source(const std::string &p_path) const override;
	std::vector<std::pair<bool, std::string>> literal_loads(const std::string &p_path) const override;
};

} // namespace petrichor::compiler
