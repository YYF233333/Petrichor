#pragma once

#include "compiler/workspace.h"

#include <godot_cpp/classes/code_edit.hpp>
#include <godot_cpp/variant/array.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace petrichor {

// The project as the compiler sees it. Scenes for the scene model (spec §7)
// are read from the engine's scene states, and resource files from what the
// engine loads, so that text and binary files and instances read as the
// engine reads them, and read again when a file they are read from changes;
// loading a scene loads what it refers to, so scenes are only asked for
// outside of resource loading (CheckMode::Program). Global classes and
// autoloads come from the project settings, and sources from the files.
class SceneIndex : public compiler::ProjectApi {
	// What was read from files, with when they changed then.
	struct Read {
		std::vector<std::pair<std::string, uint64_t>> files;
		// One changed less than 2 s before it was read, so its time, in whole
		// seconds, may not tell a later change apart.
		bool recent = false;
		mutable uint64_t checked = 0; // The count of changes on disk when it was last found current.
	};
	// A scene's files: its own; the scenes it instances, at any depth, as an
	// instance takes the class and the script of its scene's root; animation
	// libraries stored apart; and the scripts of this language of its
	// resources, which give their properties.
	struct Entry : Read {
		compiler::Scene scene;
	};
	struct Loads : Read {
		std::vector<std::pair<bool, std::string>> found;
	};
	mutable std::mutex mutex; // Not around loading, which takes the engine's locks.
	mutable uint64_t changes = 0; // On disk, as the editor reports them.
	mutable std::map<std::string, std::shared_ptr<const Entry>> scenes;
	mutable std::map<std::string, std::shared_ptr<const Loads>> loads; // literal_loads() of each script read.
	// The global classes by name, and the list of the project settings they
	// were read from, which the editor replaces when they change.
	mutable std::shared_ptr<const std::map<std::string, std::string>> global_classes;
	mutable std::optional<godot::Array> global_list;

	// The scene, read again unless it is up to date.
	std::shared_ptr<const Entry> read(const std::string &p_path) const;
	bool current(const Read &p_read) const;
	// Notes when the files changed, read from at `p_time` and after.
	void stamp(Read &r_read, const std::set<std::string> &p_files, double p_time) const;
	std::shared_ptr<const std::map<std::string, std::string>> listed_classes() const;

public:
	std::vector<std::string> scene_paths() const override;
	std::optional<compiler::Scene> scene(const std::string &p_path) const override;
	std::string main_scene() const override;
	std::optional<compiler::ResourceInfo> resource(const std::string &p_path) const override;
	std::string uid_path(const std::string &p_uid) const override;
	std::vector<std::string> dependencies(const std::string &p_path) const override;
	std::string global_class_path(const std::string &p_name) const override;
	std::string autoload_path(const std::string &p_name) const override;
	std::map<std::string, std::string> global_class_paths() const override;
	std::vector<std::string> autoload_names() const override;
	std::vector<std::string> resource_files() const override;
	std::vector<std::string> input_actions() const override;
	std::vector<std::string> script_paths() const override;
	std::optional<std::string> script_source(const std::string &p_path) const override;
	std::vector<std::pair<bool, std::string>> literal_loads(const std::string &p_path) const override;
	std::optional<compiler::ForeignClass> foreign_class(const std::string &p_path, bool p_members) const override;

	// Files changed on disk, which ones unknown: what was read from them is
	// checked again, by their modification times.
	void disk_changed() const;

	static const SceneIndex &get();
};

// The project as the editor has it (architecture §2.1), over SceneIndex.
// Outside a pass of the analysis, it first takes in the text of each script
// open in the script editor that changed.
compiler::Workspace &workspace();
// Takes the scenes open with unsaved changes into the workspace, packed as
// saving packs them (editor/editor_node.cpp:2505; ed1daf0), and drops the
// others.
void pack_open_scenes();
// The open script's editor, or null.
godot::CodeEdit *open_code_edit(const std::string &p_path);
// The bytes of a file, an unsaved scene's as the text saver writes it;
// nullopt if it cannot be read.
std::optional<std::string> file_text(const std::string &p_path);

} // namespace petrichor
