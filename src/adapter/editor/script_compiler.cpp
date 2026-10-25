#include "adapter/editor/script_compiler.h"

#include "adapter/editor/class_db_api.h"
#include "adapter/editor/scene_index.h"
#include "adapter/pet_compiled.h"
#include "compiler/compiler.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

namespace petrichor {

std::shared_ptr<const ClassImage> compile_script(const String &p_path, const String &p_source, std::vector<compiler::Diagnostic> *r_errors, bool p_report, CompileMode p_mode) {
	compiler::CompileOptions options;
	options.path = to_utf8(p_path);
	options.declarations_only = p_mode == CompileMode::Declarations;
	options.project = options.declarations_only ? nullptr : &SceneIndex::get();
	compiler::CompileResult result = compiler::compile(to_utf8(p_source), ClassDBApi::get(), options);
	if (p_report) {
		const CharString file = p_path.utf8();
		for (const compiler::Diagnostic &error : result.errors) {
			const CharString message = String::utf8(error.message.c_str()).utf8();
			godot::gdextension_interface::print_script_error(message.get_data(), "", file.get_data(), error.line, true);
		}
	}
	if (r_errors) {
		*r_errors = result.errors;
	}
	if (!result.ok()) {
		return nullptr;
	}
	String load_error;
	std::shared_ptr<const ClassImage> image;
	// The game verifies what it reads; the editor checks the compiler's output
	// the same way, so that a compiler error is reported instead of run.
	std::string problem;
	if (!bytecode::verify(result.module, problem)) {
		load_error = "internal compiler error: " + String::utf8(problem.c_str());
	} else {
		image = ClassImage::load(std::move(result.module), p_path, load_error);
	}
	if (!image && p_report) {
		const CharString message = load_error.utf8();
		const CharString file = p_path.utf8();
		godot::gdextension_interface::print_script_error(message.get_data(), "", file.get_data(), 0, true);
	}
	return image;
}

static void collect_scripts(const String &p_dir, PackedStringArray &r_paths) {
	const Ref<DirAccess> dir = DirAccess::open(p_dir);
	if (dir.is_null()) {
		return;
	}
	dir->list_dir_begin();
	for (String name = dir->get_next(); !name.is_empty(); name = dir->get_next()) {
		if (name.begins_with(".")) {
			continue; // Also skips .godot.
		}
		const String path = p_dir.path_join(name);
		if (dir->current_is_dir()) {
			collect_scripts(path, r_paths);
		} else if (name.get_extension() == "pet") {
			r_paths.push_back(path);
		}
	}
	dir->list_dir_end();
}

int build_project(int *r_script_count) {
	PackedStringArray paths;
	collect_scripts("res://", paths);
	int failed = 0;
	for (const String &path : paths) {
		const String source = FileAccess::get_file_as_string(path);
		const std::shared_ptr<const ClassImage> image = compile_script(path, source, nullptr, true);
		if (!image || !write_compiled(cache_path(path), *image)) {
			failed++;
		}
	}
	if (r_script_count) {
		*r_script_count = int(paths.size());
	}
	return failed;
}

} // namespace petrichor
