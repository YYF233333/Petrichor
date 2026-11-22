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
#include <godot_cpp/classes/editor_debugger_session.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/godot.hpp>

#include <algorithm>
#include <optional>
#include <set>
#include <vector>

using namespace godot;

namespace petrichor {

compiler::Analysis &analysis() {
	compiler::Workspace &open = workspace();
	static compiler::Analysis analysis(ClassDBApi::get(), open);
	return analysis;
}

std::shared_ptr<const ClassImage> compile_script(const String &p_path, const String &p_source, std::vector<compiler::Diagnostic> *r_errors, bool p_report, compiler::CheckMode p_mode, std::vector<compiler::Diagnostic> *r_warnings, String *r_load_error) {
	std::shared_ptr<const compiler::CompileResult> result;
	// Loads, which compile declarations, take no part in the analysis, whose
	// passes take a lock while they load scenes.
	std::optional<compiler::Analysis::Pass> analysis_pass;
	if (p_mode == compiler::CheckMode::Program) {
		analysis_pass.emplace(analysis());
		result = analysis().compile(to_utf8(p_path), to_utf8(p_source));
	} else {
		compiler::CompileOptions options;
		options.path = to_utf8(p_path);
		options.mode = p_mode;
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
			const compiler::ProjectApi &project = analysis_pass ? *analysis().pass_classes().get_project() : SceneIndex::get();
			const std::shared_ptr<const ClassImage> image = compile_script(file, to_godot(project.script_source(to_utf8(file)).value_or(std::string())), nullptr, p_report, p_mode);
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
	if (r_load_error) {
		*r_load_error = load_error;
	}
	return image;
}

bool write_bytecode(const String &p_path) {
	const String compiled_path = cache_path(p_path); // Empty outside res://.
	if (compiled_path.is_empty() || !FileAccess::file_exists(p_path)) {
		return false;
	}
	const compiler::Analysis::Pass analysis_pass(analysis(), &SceneIndex::get());
	const std::shared_ptr<const ClassImage> image = compile_script(p_path, FileAccess::get_file_as_string(p_path), nullptr, false);
	return image && write_compiled(compiled_path, *image);
}

void script_written(const String &p_path, const Ref<EditorDebuggerPlugin> &p_sessions) {
	if (!write_bytecode(p_path) || p_sessions.is_null() || EditorInterface::get_singleton()->get_editor_settings()->get_project_metadata("debug_options", "run_reload_scripts", true) != Variant(true)) {
		return;
	}
	const Array each = p_sessions->get_sessions();
	for (int64_t i = 0; i < each.size(); i++) {
		const Ref<EditorDebuggerSession> session = each[i];
		if (session->is_active()) {
			session->send_message("reload_scripts", Array::make(p_path));
		}
	}
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

ProjectReport build_project(bool p_write, bool p_command) {
	const compiler::Analysis::Pass analysis_pass(analysis(), &SceneIndex::get());
	const PackedStringArray paths = project_files(PackedStringArray({ "pet" }));
	ProjectReport report;
	for (const String &path : paths) {
		std::vector<compiler::Diagnostic> found;
		std::vector<compiler::Diagnostic> warnings;
		String load_error;
		const std::shared_ptr<const ClassImage> image = compile_script(path, FileAccess::get_file_as_string(path), &found, false, compiler::CheckMode::Program, &warnings, &load_error);
		if (!image && found.empty()) {
			found.emplace_back().message = to_utf8(load_error);
		} else if (image && p_write && !write_compiled(cache_path(path), *image)) {
			found.emplace_back().message = "Its bytecode cannot be written.";
		}
		report.failed += !found.empty();
		if (p_command) {
			found.insert(found.end(), warnings.begin(), warnings.end());
		}
		for (compiler::Diagnostic &each : found) {
			each.file = to_utf8(path);
			report.diagnostics.push_back(std::move(each));
		}
	}
	const std::vector<std::string> checked = SceneIndex::get().scene_paths();
	for (const std::string &path : checked) {
		const std::shared_ptr<const compiler::CompileResult> result = check_scene(to_godot(path), false);
		report.failed += p_command ? !result->errors.empty() : !result->ok();
		report.diagnostics.insert(report.diagnostics.end(), result->errors.begin(), result->errors.end());
		const String extension = to_godot(path).get_extension().to_lower();
		report.scenes += extension == "tscn" || extension == "scn";
	}
	report.scripts = int(paths.size());
	report.resources = int(checked.size()) - report.scenes;
	return report;
}

void engine_classes_changed() {
	ClassDBApi::extensions_changed();
	analysis().clear();
	disk_changed();
	PetScript::update_warnings();
}

void disk_changed() {
	SceneIndex::get().disk_changed();
	workspace().disk_changed();
}

std::vector<compiler::Diagnostic> scene_errors(const String &p_path) {
	const std::string path = to_utf8(p_path);
	return analysis().compile(path, workspace().script_source(path).value_or(std::string()))->errors;
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
	for (const std::string &name : SceneIndex::get().autoload_names()) {
		if (SceneIndex::get().autoload_path(name) == p_file) {
			return name;
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
	return compiler::lookup(analysis(), file_doc_name, to_utf8(p_path), p_source, p_offset);
}

std::vector<compiler::Lookup> type_definition(const std::string &p_path, const std::string &p_source, uint32_t p_offset) {
	return compiler::type_definition(analysis(), file_doc_name, p_path, p_source, p_offset);
}

namespace {

std::vector<std::string> other_scripts() {
	std::vector<std::string> others;
	for (const String &path : project_files(PackedStringArray({ "gd", "cs" }))) {
		others.push_back(to_utf8(path));
	}
	return others;
}

} // namespace

std::optional<std::vector<compiler::Occurrence>> find_references(const std::string &p_path, uint32_t p_offset) {
	pack_open_scenes();
	return compiler::find_references(analysis(), p_path, p_offset, other_scripts());
}

namespace {

// Whether an open scene that has no file yet has a node with one of the
// scripts, or with a script extending one.
bool unnamed_scene_places(const std::set<std::string> &p_scripts) {
	const TypedArray<Node> roots = EditorInterface::get_singleton()->get_open_scene_roots();
	for (int64_t i = 0; i < roots.size(); i++) {
		Node *root = Object::cast_to<Node>(roots[i]);
		if (!root->get_scene_file_path().is_empty()) {
			continue;
		}
		TypedArray<Node> nodes = root->find_children("*", "", true, false);
		nodes.push_back(root);
		for (int64_t n = 0; n < nodes.size(); n++) {
			const Ref<Script> script = Object::cast_to<Node>(nodes[n])->get_script();
			const String path = script.is_valid() ? script->get_path() : String();
			if (!path.is_empty() && std::any_of(p_scripts.begin(), p_scripts.end(), [&](const std::string &p_base) { return to_utf8(path) == p_base || script_extends(path, script->get_source_code(), to_godot(p_base)); })) {
				return true;
			}
		}
	}
	return false;
}

} // namespace

// A scene with no file is not in the workspace (pack_open_scenes()), so a
// value it stores under a member's name would not be renamed: a scene that
// may store one stops renaming the member.
compiler::Rename rename(const std::string &p_path, uint32_t p_offset, const std::string &p_new) {
	pack_open_scenes();
	compiler::Rename found = compiler::rename(analysis(), p_path, p_offset, p_new, other_scripts(), file_text);
	std::set<std::string> declaring;
	for (const compiler::RenameEdit &each : found.edits) {
		const compiler::Reference &reference = each.occurrence.reference;
		if (each.occurrence.declaration && reference.target.kind == compiler::DeclId::Kind::Script) {
			declaring.insert(compiler::script_file(reference.target.scope));
		}
	}
	if (!declaring.empty() && unnamed_scene_places(declaring)) {
		compiler::Diagnostic refusal;
		refusal.file = p_path;
		refusal.message = "An open scene that has no file yet has nodes with this script. Save that scene first.";
		found.edits.clear();
		found.refusals.push_back(refusal);
	}
	return found;
}

compiler::RenamedName renamed_name(const std::string &p_path, uint32_t p_offset) {
	pack_open_scenes();
	return compiler::renamed_name(analysis(), p_path, p_offset, other_scripts());
}

std::optional<uint32_t> declaration_offset(const std::string &p_path, const compiler::DeclId &p_id) {
	const std::optional<std::string> source = workspace().script_source(p_path);
	for (const compiler::Reference &reference : source ? analysis().compile(p_path, *source)->references : std::vector<compiler::Reference>()) {
		if (reference.declares && reference.target == p_id) {
			return reference.begin;
		}
	}
	return std::nullopt;
}

compiler::Completion completion(const String &p_path, const std::string &p_source) {
	return compiler::complete(analysis(), to_utf8(p_path), p_source);
}

std::vector<compiler::Diagnostic> fixable(std::vector<compiler::Diagnostic> p_errors, const std::string &p_path) {
	const std::vector<compiler::Diagnostic> placed = analysis().placed_errors(p_path);
	p_errors.insert(p_errors.end(), placed.begin(), placed.end());
	std::vector<compiler::Diagnostic> found;
	for (compiler::Diagnostic &error : p_errors) {
		for (compiler::Fix &fix : error.fixes) {
			for (compiler::Fix::Edit &edit : fix.edits) {
				edit.file = edit.file.empty() ? p_path : edit.file;
			}
		}
		const auto elsewhere_too = [&](const compiler::Fix &p_fix) {
			return !error.file.empty() && std::any_of(p_fix.edits.begin(), p_fix.edits.end(), [&](const compiler::Fix::Edit &p_edit) { return p_edit.file != p_path; });
		};
		error.fixes.erase(std::remove_if(error.fixes.begin(), error.fixes.end(), elsewhere_too), error.fixes.end());
		if (!error.fixes.empty()) {
			found.push_back(std::move(error));
		}
	}
	std::stable_sort(found.begin(), found.end(), [](const compiler::Diagnostic &p_a, const compiler::Diagnostic &p_b) { return p_a.begin < p_b.begin; });
	return found;
}

bool same_fix(const compiler::Fix &p_a, const compiler::Fix &p_b) {
	return p_a.title == p_b.title && std::equal(p_a.edits.begin(), p_a.edits.end(), p_b.edits.begin(), p_b.edits.end(), [](const compiler::Fix::Edit &p_x, const compiler::Fix::Edit &p_y) {
		return p_x.file == p_y.file && p_x.change.begin == p_y.change.begin && p_x.change.end == p_y.change.end && p_x.change.new_text == p_y.change.new_text;
	});
}

bool fixes_all(const std::vector<compiler::Diagnostic> &p_fixable, const std::string &p_path, compiler::FixKind p_kind) {
	std::vector<const compiler::Fix *> found;
	for (const compiler::Diagnostic &error : p_fixable) {
		for (const compiler::Fix &fix : error.fixes) {
			const bool here = error.file.empty() && std::all_of(fix.edits.begin(), fix.edits.end(), [&](const compiler::Fix::Edit &p_edit) { return p_edit.file == p_path; });
			if (fix.kind == p_kind && here && std::none_of(found.begin(), found.end(), [&](const compiler::Fix *p_found) { return same_fix(*p_found, fix); })) {
				found.push_back(&fix);
			}
		}
	}
	return found.size() > 1;
}

FileEdits fix_again(const std::string &p_path, const compiler::Stamp &p_stamp, const compiler::Diagnostic *p_error, const std::string &p_title, compiler::FixKind p_kind) {
	const compiler::ProjectApi &project = analysis().project_api();
	const std::string source = project.script_source(p_path).value_or(std::string());
	std::shared_ptr<const compiler::CompileResult> result;
	if (workspace().changed(p_stamp)) {
		compiler::CompileOptions options;
		options.path = p_path;
		options.project = &project;
		result = std::make_shared<const compiler::CompileResult>(compiler::compile(source, ClassDBApi::get(), options));
	} else {
		result = analysis().compile(p_path, source);
	}
	FileEdits changes;
	if (!p_error) {
		const std::vector<compiler::CheckedEdit> edits = compiler::fix_all(*result, p_kind);
		if (!edits.empty()) {
			changes[p_path] = edits;
		}
	}
	for (const compiler::Diagnostic &error : p_error ? fixable(result->errors, p_path) : std::vector<compiler::Diagnostic>()) {
		const auto fix = std::find_if(error.fixes.begin(), error.fixes.end(), [&](const compiler::Fix &p_fix) { return p_fix.title == p_title; });
		if (changes.empty() && error.begin == p_error->begin && error.end == p_error->end && error.message == p_error->message && error.file == p_error->file && fix != error.fixes.end()) {
			for (const compiler::Fix::Edit &edit : fix->edits) {
				changes[edit.file].push_back(edit.change);
			}
		}
	}
	for (auto &[file, edits] : changes) {
		std::stable_sort(edits.begin(), edits.end(), [](const compiler::CheckedEdit &p_a, const compiler::CheckedEdit &p_b) { return p_a.begin < p_b.begin; });
	}
	return changes;
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
	options.mode = compiler::CheckMode::Declarations;
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
