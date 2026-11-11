#include "adapter/editor/script_compiler.h"

#include "adapter/editor/class_db_api.h"
#include "adapter/editor/project_files.h"
#include "adapter/editor/scene_index.h"
#include "adapter/pet_compiled.h"
#include "adapter/pet_script.h"
#include "compiler/analysis.h"
#include "compiler/ast.h"
#include "compiler/parser.h"
#include "compiler/script_classes.h"
#include "runtime/text.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/godot.hpp>

#include <algorithm>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

using namespace godot;

namespace petrichor {

namespace {

// The project's scripts as the editor and the headless build check them,
// each checked again only when what it read changes (architecture §2.4).
compiler::Analysis &analysis() {
	static compiler::Analysis analysis(ClassDBApi::get(), SceneIndex::get());
	return analysis;
}

} // namespace

std::shared_ptr<const ClassImage> compile_script(const String &p_path, const String &p_source, std::vector<compiler::Diagnostic> *r_errors, bool p_report, CompileMode p_mode, std::vector<compiler::Diagnostic> *r_warnings) {
	std::shared_ptr<const compiler::CompileResult> result;
	// Loads, which compile declarations, take no part in the analysis, whose
	// passes take a lock while they load scenes.
	std::optional<compiler::Analysis::Pass> analysis_pass;
	if (p_mode == CompileMode::Full) {
		analysis_pass.emplace(analysis());
		result = analysis().compile(to_utf8(p_path), to_utf8(p_source));
	} else {
		compiler::CompileOptions options;
		options.path = to_utf8(p_path);
		options.declarations_only = true;
		// While loading, the compiler asks for no scenes, only classes and sources.
		options.project = &SceneIndex::get();
		result = std::make_shared<const compiler::CompileResult>(compiler::compile(to_utf8(p_source), ClassDBApi::get(), options));
	}
	if (p_report) {
		const CharString file = p_path.utf8();
		for (const compiler::Diagnostic &error : result->errors) {
			// The compiler's messages are UTF-8 already.
			gdextension_interface::print_script_error(error.message.c_str(), "", file.get_data(), error.line, true);
		}
	}
	if (r_errors) {
		*r_errors = result->errors;
	}
	if (r_warnings) {
		*r_warnings = result->warnings;
	}
	if (!result->errors.empty()) {
		return nullptr;
	}
	String load_error;
	std::shared_ptr<const ClassImage> image;
	// The game verifies what it reads; the editor checks the compiler's output
	// the same way, so that a compiler error is reported instead of run.
	std::string problem;
	if (!bytecode::verify(result->module, problem)) {
		load_error = "internal compiler error: " + to_godot(problem);
	} else {
		// A class of another script, also an inner class (spec §4.1), from its
		// file compiled as this one is. Inner classes of two files can extend
		// each other's classes, which no image can be loaded for.
		thread_local std::vector<String> extending;
		const ClassImage::BaseLoader compiled = [&](const String &p_class) -> std::shared_ptr<const ClassImage> {
			const String file = p_class.get_slice("::", 0);
			if (std::find(extending.begin(), extending.end(), file) != extending.end()) {
				load_error = "The classes of this script and of " + file + " extend each other's.";
				return nullptr;
			}
			extending.push_back(p_path);
			const std::shared_ptr<const ClassImage> image = compile_script(file, FileAccess::get_file_as_string(file), nullptr, p_report, p_mode);
			extending.pop_back();
			return image ? image->find_class(p_class) : nullptr;
		};
		std::shared_ptr<const ClassImage> base;
		if (!result->module.base_script.empty()) {
			const String base_path = to_godot(result->module.base_script);
			base = compiled(base_path);
			if (!base) {
				load_error = "Base script " + base_path + " could not be compiled.";
			}
		}
		if (load_error.is_empty()) {
			image = ClassImage::load(bytecode::Module(result->module), p_path, load_error, base, compiled);
		}
	}
	if (!image && p_report) {
		const CharString message = load_error.utf8();
		const CharString file = p_path.utf8();
		godot::gdextension_interface::print_script_error(message.get_data(), "", file.get_data(), 0, true);
	}
	return image;
}

std::shared_ptr<const compiler::CompileResult> check_scene(const String &p_path, bool p_report) {
	const compiler::Analysis::Pass analysis_pass(analysis());
	const std::shared_ptr<const compiler::CompileResult> result = analysis().check_scene(to_utf8(p_path));
	for (size_t i = 0; p_report && i < result->errors.size(); i++) {
		const compiler::Diagnostic &error = result->errors[i];
		gdextension_interface::print_script_error(error.message.c_str(), "", error.file.c_str(), error.line, true);
	}
	return result;
}

int build_project(int *r_script_count, int *r_scene_count, int *r_resource_count, bool p_command) {
	const compiler::Analysis::Pass analysis_pass(analysis());
	const PackedStringArray paths = project_files(PackedStringArray({ "pet" }));
	int failed = 0;
	for (const String &path : paths) {
		const String source = FileAccess::get_file_as_string(path);
		std::vector<compiler::Diagnostic> warnings;
		const std::shared_ptr<const ClassImage> image = compile_script(path, source, nullptr, true, CompileMode::Full, &warnings);
		if (!image || !write_compiled(cache_path(path), *image)) {
			failed++;
		}
		const CharString file = path.utf8();
		for (size_t i = 0; p_command && i < warnings.size(); i++) {
			const std::string message = warnings[i].message + " (" + warnings[i].warning + ")";
			gdextension_interface::print_warning(message.c_str(), "", file.get_data(), warnings[i].line, false);
		}
	}
	int scenes = 0;
	const std::vector<std::string> checked = SceneIndex::get().scene_paths();
	for (const std::string &path : checked) {
		const std::shared_ptr<const compiler::CompileResult> result = check_scene(to_godot(path), true);
		if (p_command ? !result->errors.empty() : !result->ok()) {
			failed++;
		}
		const String extension = to_godot(path).get_extension().to_lower();
		scenes += extension == "tscn" || extension == "scn";
	}
	if (r_script_count) {
		*r_script_count = int(paths.size());
	}
	if (r_scene_count) {
		*r_scene_count = scenes;
	}
	if (r_resource_count) {
		*r_resource_count = int(checked.size()) - scenes;
	}
	return failed;
}

void engine_classes_changed() {
	ClassDBApi::extensions_changed();
	analysis().clear();
	project_changed();
}

namespace {

struct SceneErrors {
	uint64_t change = 0;
	uint64_t source_hash = 0;
	std::vector<compiler::Diagnostic> errors;
};

std::mutex scene_errors_mutex;
uint64_t project_change = 1;
std::map<std::string, SceneErrors> scene_errors_of; // By the path of the script, or of the scene checked.

// What `p_check` gives for the file at the path, from a source with that hash,
// as of the last change to the project.
std::vector<compiler::Diagnostic> as_of_change(const std::string &p_path, uint64_t p_source_hash, const std::function<std::vector<compiler::Diagnostic>()> &p_check) {
	uint64_t change;
	{
		std::lock_guard<std::mutex> lock(scene_errors_mutex);
		change = project_change;
		const auto found = scene_errors_of.find(p_path);
		if (found != scene_errors_of.end() && found->second.change == change && found->second.source_hash == p_source_hash) {
			return found->second.errors;
		}
	}
	const std::vector<compiler::Diagnostic> errors = p_check();
	std::lock_guard<std::mutex> lock(scene_errors_mutex);
	scene_errors_of[p_path] = { change, p_source_hash, errors };
	return errors;
}

} // namespace

std::vector<compiler::Diagnostic> scene_errors(const String &p_path, const String &p_source) {
	const std::string path = to_utf8(p_path);
	const std::string source = to_utf8(p_source);
	return as_of_change(path, bytecode::hash_source(source), [&] {
		const compiler::Analysis::Pass analysis_pass(analysis());
		return analysis().compile(path, source)->errors;
	});
}

std::vector<compiler::Diagnostic> resource_errors(const String &p_path) {
	return as_of_change(to_utf8(p_path), 0, [&] { return check_scene(p_path, false)->errors; });
}

std::vector<compiler::Diagnostic> placed_errors(const String &p_path) {
	const compiler::Analysis::Pass analysis_pass(analysis());
	compiler::ScriptClasses &classes = analysis().pass_classes();
	const std::string path = to_utf8(p_path);
	std::vector<compiler::Diagnostic> placed;
	// The scenes that place it, and those that instance them, which check the
	// roots of the scenes they instance and what their tracks name; and the
	// files whose resources have scripts of this language, which may extend it.
	std::vector<compiler::Scene> pending = classes.scenes_with_script(path);
	const compiler::ProjectApi &project = *classes.get_project();
	for (const std::string &file : project.scene_paths()) {
		const std::optional<compiler::Scene> held = project.scene(file);
		if (held && std::any_of(held->resources.begin(), held->resources.end(), [](const compiler::SceneResource &p_resource) { return compiler::is_script_path(p_resource.script); })) {
			pending.push_back(*held);
		}
	}
	std::set<std::string> seen;
	while (!pending.empty()) {
		const std::string scene = pending.back().path;
		pending.pop_back();
		if (!seen.insert(scene).second) {
			continue;
		}
		for (const compiler::Diagnostic &error : resource_errors(to_godot(scene))) {
			if (error.file == path) {
				placed.push_back(error);
			}
		}
		const std::vector<compiler::Scene> outer = classes.scenes_instancing(scene);
		pending.insert(pending.end(), outer.begin(), outer.end());
	}
	return placed;
}

void project_changed() {
	{
		std::lock_guard<std::mutex> lock(scene_errors_mutex);
		project_change++;
	}
	PetScript::update_warnings();
}

namespace {

// The name GDScript gives the documentation of a script file's class
// (compiler::DocName), a script of this language's too.
std::string file_doc_name(const std::string &p_file) {
	const String path = to_godot(p_file);
	ProjectSettings *settings = ProjectSettings::get_singleton();
	const TypedArray<Dictionary> classes = settings->get_global_class_list();
	for (int64_t i = 0; i < classes.size(); i++) {
		const Dictionary entry = classes[i];
		if (String(entry.get("path", String())) == path) {
			return to_utf8(entry.get("class", String()));
		}
	}
	const TypedArray<Dictionary> properties = settings->get_property_list();
	for (int64_t i = 0; i < properties.size(); i++) {
		const String name = Dictionary(properties[i]).get("name", String());
		if (name.begins_with("autoload/") && String(settings->get_setting(name)) == "*" + path) {
			return to_utf8(name.trim_prefix("autoload/"));
		}
	}
	return to_utf8("\"" + path.trim_prefix("res://") + "\"");
}

// As the documentation takes a comment (core/doc_data.h:163-185): a tag is
// there when its key is.
void add_comment(Dictionary &r_doc, const compiler::DocComment &p_comment) {
	r_doc["description"] = to_godot(p_comment.description);
	if (p_comment.deprecated) {
		r_doc["deprecated"] = to_godot(*p_comment.deprecated);
	}
	if (p_comment.experimental) {
		r_doc["experimental"] = to_godot(*p_comment.experimental);
	}
}

Dictionary doc_member(const compiler::DocMember &p_member, const char *p_type_key, const char *p_enum_key) {
	Dictionary result;
	add_comment(result, p_member.comment);
	result["name"] = to_godot(p_member.name);
	result[p_type_key] = to_godot(p_member.type);
	result[p_enum_key] = to_godot(p_member.enumeration);
	if (!p_member.qualifiers.empty()) {
		result["qualifiers"] = to_godot(p_member.qualifiers);
	}
	Array arguments;
	for (const compiler::DocMember &argument : p_member.arguments) {
		Dictionary documented = doc_member(argument, "type", "enumeration");
		documented["default_value"] = to_godot(argument.value);
		arguments.push_back(documented);
	}
	result["arguments"] = arguments;
	return result;
}

} // namespace

std::optional<compiler::Lookup> lookup_name(const String &p_path, const std::string &p_source, uint32_t p_offset) {
	const compiler::Analysis::Pass analysis_pass(analysis());
	return compiler::lookup(analysis(), ClassDBApi::get(), SceneIndex::get(), file_doc_name, to_utf8(p_path), p_source, p_offset);
}

compiler::Completion completion(const String &p_path, const std::string &p_source) {
	return compiler::complete(analysis(), ClassDBApi::get(), to_utf8(p_path), p_source);
}

namespace {

// The headers of a script and of its bases. A file's own is read from its
// source, also unsaved; an inner class's from its file.
struct Header {
	compiler::ScriptClasses classes{ ClassDBApi::get(), &SceneIndex::get(), true };
	std::string path;
	compiler::ClassDecl own;
	const compiler::ClassDecl *decl = &own;

	Header(const String &p_path, const String &p_source) :
			path(to_utf8(p_path)) {
		if (path != compiler::script_file(path)) {
			decl = classes.header(path);
			return;
		}
		std::vector<compiler::Diagnostic> errors;
		own = compiler::parse(to_utf8(p_source), errors, true);
	}
};

} // namespace

String script_base_path(const String &p_path, const String &p_source) {
	Header header(p_path, p_source);
	return header.decl ? to_godot(header.classes.base_path(*header.decl, header.path)) : String();
}

String script_native_base(const String &p_path, const String &p_source) {
	Header header(p_path, p_source);
	const std::string native = header.decl ? header.classes.native_base(*header.decl, header.path) : std::string();
	return native.empty() ? String("RefCounted") : to_godot(native);
}

String script_global_base(const String &p_path, const String &p_source) {
	Header header(p_path, p_source);
	std::set<std::string> seen;
	for (std::string path = header.path; header.decl && seen.insert(path).second;) {
		if (!header.decl->extends_path && header.decl->extends.find('.') == std::string::npos) {
			return to_godot(header.decl->extends);
		}
		path = header.classes.base_path(*header.decl, path);
		header.decl = path.empty() ? nullptr : header.classes.header(path);
		if (header.decl && !header.decl->class_name.empty() && path == compiler::script_file(path)) {
			return to_godot(header.decl->class_name);
		}
	}
	return "RefCounted";
}

bool script_extends(const String &p_path, const String &p_source, const String &p_base) {
	Header header(p_path, p_source);
	const std::string base = to_utf8(p_base);
	std::set<std::string> seen;
	for (std::string path = header.path; header.decl && seen.insert(path).second; header.decl = header.classes.header(path)) {
		path = header.classes.base_path(*header.decl, path);
		if (path.empty() || path == base) {
			return !path.empty();
		}
	}
	return false;
}

String class_doc_name(const String &p_path) {
	return to_godot(compiler::doc_class_name(file_doc_name, to_utf8(p_path)));
}

TypedArray<Dictionary> script_documentation(const String &p_path, const String &p_source) {
	compiler::CompileOptions options;
	options.path = to_utf8(p_path);
	options.declarations_only = true;
	options.project = &SceneIndex::get();
	compiler::ScriptClasses classes(ClassDBApi::get(), options.project, true);
	compiler::ClassDecl decl;
	compiler::compile(to_utf8(p_source), ClassDBApi::get(), options, classes, decl);
	TypedArray<Dictionary> result;
	for (const compiler::ClassDoc &doc : compiler::document(decl, options.path, file_doc_name)) {
		Dictionary documented;
		documented["name"] = to_godot(doc.name);
		documented["inherits"] = to_godot(doc.inherits);
		documented["is_script_doc"] = true;
		documented["script_path"] = p_path;
		add_comment(documented, doc.comment);
		documented["brief_description"] = to_godot(doc.comment.brief);
		Array tutorials;
		for (const auto &[title, link] : doc.comment.tutorials) {
			Dictionary tutorial;
			tutorial["title"] = to_godot(title);
			tutorial["link"] = to_godot(link);
			tutorials.push_back(tutorial);
		}
		documented["tutorials"] = tutorials;
		Array constants;
		for (const compiler::DocMember &constant : doc.constants) {
			Dictionary member = doc_member(constant, "type", "enumeration");
			member["value"] = to_godot(constant.value);
			member["is_value_valid"] = !constant.value.empty();
			constants.push_back(member);
		}
		documented["constants"] = constants;
		Array properties;
		for (const compiler::DocMember &property : doc.properties) {
			Dictionary member = doc_member(property, "type", "enumeration");
			member["default_value"] = to_godot(property.value);
			properties.push_back(member);
		}
		documented["properties"] = properties;
		Array methods;
		for (const compiler::DocMember &method : doc.methods) {
			methods.push_back(doc_member(method, "return_type", "return_enum"));
		}
		documented["methods"] = methods;
		Array signals;
		for (const compiler::DocMember &signal : doc.signals) {
			signals.push_back(doc_member(signal, "return_type", "return_enum"));
		}
		documented["signals"] = signals;
		Dictionary enums;
		for (const compiler::DocMember &documented_enum : doc.enums) {
			Dictionary comment;
			add_comment(comment, documented_enum.comment);
			enums[to_godot(documented_enum.name)] = comment;
		}
		documented["enums"] = enums;
		result.push_back(documented);
	}
	return result;
}

} // namespace petrichor
