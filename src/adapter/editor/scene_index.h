#pragma once

#include "compiler/project_api.h"

#include <godot_cpp/classes/code_edit.hpp>
#include <godot_cpp/classes/packed_scene.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace petrichor {

// The project as the compiler sees it. Scenes for the scene model (spec §7)
// are read from the engine's scene states, and resource files from what the
// engine loads, so that text and binary files and instances read as the
// engine reads them, and read again when a file they are read from changes;
// loading a scene loads what it refers to, so scenes are only asked for
// outside of resource loading (CompileMode::Full). Global classes and
// autoloads come from the project settings, and sources from the files.
class SceneIndex : public compiler::ProjectApi {
	struct Entry {
		uint64_t modified = 0;
		// The other files it is read from, and when they changed: the scenes it
		// instances, at any depth, as an instance takes the class and the script
		// of its scene's root, animation libraries stored apart, and the scripts
		// of this language of its resources, which give their properties.
		std::vector<std::pair<std::string, uint64_t>> files;
		compiler::Scene scene;
	};
	mutable std::mutex mutex; // Only around `scenes` and `loads`: loading takes the engine's locks.
	mutable std::map<std::string, std::shared_ptr<const Entry>> scenes;
	// literal_loads() of each script read, with when it changed.
	mutable std::map<std::string, std::pair<uint64_t, std::vector<std::pair<bool, std::string>>>> loads;

	// The scene, read again unless it is up to date.
	std::shared_ptr<const Entry> read(const std::string &p_path) const;

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

	// The editor saved the file: it and the scenes read from it are read again,
	// also within the second their modification times keep.
	void forget(const std::string &p_path) const;

	static const SceneIndex &get();
};

// The project with what the editor has not saved (architecture §2.3): the text
// of the scripts open in the script editor, and the scenes open with unsaved
// changes, packed as saving packs them (editor/editor_node.cpp:2505; ed1daf0).
// The rest it reads as SceneIndex does.
class OpenProject : public compiler::ProjectApi {
	std::map<std::string, godot::CodeEdit *> code_edits;
	std::map<std::string, std::string> buffers;
	std::map<std::string, godot::Ref<godot::PackedScene>> packed;
	std::map<std::string, compiler::Scene> scenes;

public:
	OpenProject();
	// The open script's editor, or null.
	godot::CodeEdit *code_edit(const std::string &p_path) const;
	bool is_unsaved_scene(const std::string &p_path) const { return packed.count(p_path) > 0; }
	// The bytes of a file, an unsaved scene's as the text saver writes it;
	// nullopt if it cannot be read.
	std::optional<std::string> text(const std::string &p_path) const;

	std::vector<std::string> scene_paths() const override { return SceneIndex::get().scene_paths(); }
	std::optional<compiler::Scene> scene(const std::string &p_path) const override;
	std::string main_scene() const override { return SceneIndex::get().main_scene(); }
	std::optional<compiler::ResourceInfo> resource(const std::string &p_path) const override { return SceneIndex::get().resource(p_path); }
	std::string uid_path(const std::string &p_uid) const override { return SceneIndex::get().uid_path(p_uid); }
	std::vector<std::string> dependencies(const std::string &p_path) const override { return SceneIndex::get().dependencies(p_path); }
	std::string global_class_path(const std::string &p_name) const override { return SceneIndex::get().global_class_path(p_name); }
	std::string autoload_path(const std::string &p_name) const override { return SceneIndex::get().autoload_path(p_name); }
	std::map<std::string, std::string> global_class_paths() const override { return SceneIndex::get().global_class_paths(); }
	std::vector<std::string> autoload_names() const override { return SceneIndex::get().autoload_names(); }
	std::vector<std::string> resource_files() const override { return SceneIndex::get().resource_files(); }
	std::vector<std::string> input_actions() const override { return SceneIndex::get().input_actions(); }
	std::vector<std::string> script_paths() const override { return SceneIndex::get().script_paths(); }
	std::optional<std::string> script_source(const std::string &p_path) const override;
	std::vector<std::pair<bool, std::string>> literal_loads(const std::string &p_path) const override;
	std::optional<compiler::ForeignClass> foreign_class(const std::string &p_path, bool p_members) const override { return SceneIndex::get().foreign_class(p_path, p_members); }
};

} // namespace petrichor
