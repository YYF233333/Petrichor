#include "compiler/workspace.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace petrichor::compiler {

// A pass of Analysis reads it as one state (architecture §2.1).
void Workspace::write() const {
	if (passes > 0) {
		std::fputs("petrichor: the workspace was written while a pass of the analysis read it.\n", stderr);
		std::abort();
	}
}

void Workspace::touch(const std::string &p_path) {
	versions[p_path] = ++current;
}

void Workspace::set_script(const std::string &p_path, std::string p_text) {
	write();
	if (script_source(p_path) != p_text) {
		touch(p_path);
	}
	scripts[p_path] = std::move(p_text);
}

void Workspace::drop_script(const std::string &p_path) {
	write();
	const auto found = scripts.find(p_path);
	if (found == scripts.end()) {
		return;
	}
	const std::string text = std::move(found->second);
	scripts.erase(found);
	if (script_source(p_path) != text) {
		touch(p_path);
	}
}

void Workspace::set_scene(const std::string &p_path, Scene p_scene) {
	write();
	if (!(scene(p_path) == p_scene)) {
		touch(p_path);
	}
	scenes[p_path] = std::move(p_scene);
}

void Workspace::drop_scene(const std::string &p_path) {
	write();
	const auto found = scenes.find(p_path);
	if (found == scenes.end()) {
		return;
	}
	const Scene unsaved = std::move(found->second);
	scenes.erase(found);
	if (!(scene(p_path) == unsaved)) {
		touch(p_path);
	}
}

void Workspace::disk_changed() {
	write();
	disk_revision = ++current;
}

uint64_t Workspace::version(const std::string &p_path) const {
	const auto found = versions.find(p_path);
	const uint64_t own = found == versions.end() ? 0 : found->second;
	return scripts.count(p_path) || scenes.count(p_path) ? own : std::max(own, disk_revision);
}

bool Workspace::changed(const Stamp &p_stamp) const {
	return p_stamp.revision == 0 || std::any_of(p_stamp.files.begin(), p_stamp.files.end(), [&](const std::string &p_file) { return version(p_file) > p_stamp.revision; });
}

std::optional<Scene> Workspace::scene(const std::string &p_path) const {
	const auto found = scenes.find(p_path);
	return found == scenes.end() ? disk.scene(p_path) : std::optional<Scene>(found->second);
}

std::optional<std::string> Workspace::script_source(const std::string &p_path) const {
	const auto found = scripts.find(p_path);
	return found == scripts.end() ? disk.script_source(p_path) : std::optional<std::string>(found->second);
}

std::vector<std::pair<bool, std::string>> Workspace::literal_loads(const std::string &p_path) const {
	return scripts.count(p_path) ? ProjectApi::literal_loads(p_path) : disk.literal_loads(p_path);
}

} // namespace petrichor::compiler
