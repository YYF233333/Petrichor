#include "adapter/pet_language.h"

#include "adapter/pet_script.h"
#include "runtime/debugger.h"
#include "runtime/vm.h"

#ifdef PETRICHOR_PROBES
#include "probe/task.h"
#endif

#ifdef TOOLS_ENABLED
#include "adapter/editor/script_compiler.h"
#endif

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/error_macros.hpp>

using namespace godot;

namespace petrichor {

PetLanguage *PetLanguage::singleton = nullptr;

PetLanguage::PetLanguage() {
	ERR_FAIL_COND(singleton != nullptr);
	singleton = this;
}

PetLanguage::~PetLanguage() {
	if (singleton == this) {
		singleton = nullptr;
	}
}

String PetLanguage::_get_name() const {
	return "Petrichor";
}

void PetLanguage::_init() {
	debugger::set_language(this);
}

String PetLanguage::_get_type() const {
	return PetScript::get_class_static();
}

String PetLanguage::_get_extension() const {
	return "pet";
}

void PetLanguage::_finish() {
#ifdef PETRICHOR_PROBES
	task_queue::clear();
#endif
	debugger::set_language(nullptr);
}

/* Editor */

PackedStringArray PetLanguage::_get_reserved_words() const {
	static const char *const words[] = {
		// GDScript keywords (spec §2.2).
		"and", "as", "assert", "await", "break", "breakpoint", "class", "class_name", "const", "continue",
		"elif", "else", "enum", "extends", "for", "func", "if", "in", "is", "match", "namespace", "not",
		"or", "pass", "preload", "return", "self", "signal", "static", "super", "trait", "var", "void",
		"when", "while", "yield",
		// Added by this language (spec §2.2).
		"pub", "struct", "interface", "implements", "async", "None",
		// Literals and built-in constants.
		"true", "false", "null", "PI", "TAU", "INF", "NAN",
	};
	PackedStringArray result;
	for (const char *word : words) {
		result.push_back(word);
	}
	return result;
}

bool PetLanguage::_is_control_flow_keyword(const String &p_keyword) const {
	static const char *const words[] = {
		"break", "continue", "elif", "else", "for", "if", "match", "pass", "return", "when", "while",
	};
	for (const char *word : words) {
		if (p_keyword == word) {
			return true;
		}
	}
	return false;
}

PackedStringArray PetLanguage::_get_comment_delimiters() const {
	PackedStringArray result;
	result.push_back("#");
	return result;
}

PackedStringArray PetLanguage::_get_doc_comment_delimiters() const {
	PackedStringArray result;
	result.push_back("##");
	return result;
}

PackedStringArray PetLanguage::_get_string_delimiters() const {
	PackedStringArray result;
	result.push_back("\" \"");
	result.push_back("' '");
	result.push_back("\"\"\" \"\"\"");
	result.push_back("''' '''");
	return result;
}

Ref<Script> PetLanguage::_make_template(const String &p_template, const String &p_class_name, const String &p_base_class_name) const {
	const String source = vformat("extends %s\n", p_base_class_name);
	Ref<PetScript> script;
	script.instantiate();
#ifdef TOOLS_ENABLED
	script->set_loaded(source, compile_script(String(), source, nullptr, false));
#else
	script->set_loaded(source, nullptr);
#endif
	return script;
}

TypedArray<Dictionary> PetLanguage::_get_built_in_templates(const StringName &p_object) const {
	return TypedArray<Dictionary>();
}

bool PetLanguage::_is_using_templates() {
	return false;
}

Dictionary PetLanguage::_validate(const String &p_script, const String &p_path, bool p_validate_functions, bool p_validate_errors, bool p_validate_warnings, bool p_validate_safe_lines) const {
	Dictionary result;
	Array errors;
#ifdef TOOLS_ENABLED
	std::vector<compiler::Diagnostic> diagnostics;
	compile_script(p_path, p_script, &diagnostics, false);
	for (const compiler::Diagnostic &diagnostic : diagnostics) {
		Dictionary error;
		error["path"] = p_path;
		error["line"] = diagnostic.line;
		error["column"] = diagnostic.column;
		error["message"] = String::utf8(diagnostic.message.c_str());
		errors.push_back(error);
	}
#endif
	result["valid"] = errors.is_empty();
	result["errors"] = errors;
	result["warnings"] = Array();
	result["functions"] = PackedStringArray();
	result["safe_lines"] = PackedInt32Array();
	return result;
}

String PetLanguage::_validate_path(const String &p_path) const {
	return String();
}

bool PetLanguage::_supports_builtin_mode() const {
	return false;
}

bool PetLanguage::_supports_documentation() const {
	return false;
}

bool PetLanguage::_can_inherit_from_file() const {
	return false;
}

int32_t PetLanguage::_find_function(const String &p_function, const String &p_code) const {
	return -1;
}

String PetLanguage::_make_function(const String &p_class_name, const String &p_function_name, const PackedStringArray &p_function_args) const {
	return String();
}

bool PetLanguage::_can_make_function() const {
	return false;
}

Error PetLanguage::_open_in_external_editor(const Ref<Script> &p_script, int32_t p_line, int32_t p_column) {
	return ERR_UNAVAILABLE;
}

bool PetLanguage::_overrides_external_editor() {
	return false;
}

ScriptLanguage::ScriptNameCasing PetLanguage::_preferred_file_name_casing() const {
	return ScriptLanguage::SCRIPT_NAME_CASING_SNAKE_CASE;
}

Dictionary PetLanguage::_complete_code(const String &p_code, const String &p_path, Object *p_owner) const {
	Dictionary result;
	result["result"] = OK;
	result["options"] = Array();
	result["force"] = false;
	result["call_hint"] = String();
	return result;
}

Dictionary PetLanguage::_lookup_code(const String &p_code, const String &p_symbol, const String &p_path, Object *p_owner) const {
	Dictionary result;
	result["result"] = ERR_UNAVAILABLE;
	result["type"] = LOOKUP_RESULT_SCRIPT_LOCATION;
	return result;
}

String PetLanguage::_auto_indent_code(const String &p_code, int32_t p_from_line, int32_t p_to_line) const {
	return p_code;
}

void PetLanguage::_add_global_constant(const StringName &p_name, const Variant &p_value) {
}

void PetLanguage::_add_named_global_constant(const StringName &p_name, const Variant &p_value) {
}

void PetLanguage::_remove_named_global_constant(const StringName &p_name) {
}

/* Threads */

void PetLanguage::_thread_enter() {
}

void PetLanguage::_thread_exit() {
}

/* Debugger */

// Level 0 is the innermost frame of this language on the current thread.
static const StackFrame *frame_at(int32_t p_level) {
	const std::vector<StackFrame> &stack = vm::stack();
	if (p_level < 0 || size_t(p_level) >= stack.size()) {
		return nullptr;
	}
	return &stack[stack.size() - 1 - size_t(p_level)];
}

String PetLanguage::_debug_get_error() const {
	return debugger::last_error();
}

int32_t PetLanguage::_debug_get_stack_level_count() const {
	return int32_t(vm::stack().size());
}

int32_t PetLanguage::_debug_get_stack_level_line(int32_t p_level) const {
	const StackFrame *frame = frame_at(p_level);
	return frame ? frame->line : -1;
}

String PetLanguage::_debug_get_stack_level_function(int32_t p_level) const {
	const StackFrame *frame = frame_at(p_level);
	return frame ? String::utf8(frame->function->name.c_str()) : String();
}

String PetLanguage::_debug_get_stack_level_source(int32_t p_level) const {
	const StackFrame *frame = frame_at(p_level);
	return frame ? frame->image->path : String();
}

Dictionary PetLanguage::_debug_get_stack_level_locals(int32_t p_level, int32_t p_max_subitems, int32_t p_max_depth) {
	Dictionary result;
	const StackFrame *frame = frame_at(p_level);
	if (!frame) {
		return result;
	}
	PackedStringArray names;
	Array values;
	for (const bytecode::Slot &slot : frame->function->slots) {
		if (!slot.name.empty()) {
			names.push_back(String::utf8(slot.name.c_str()));
			values.push_back(to_variant(slot.type, frame->frame + slot.offset));
		}
	}
	result["locals"] = names;
	result["values"] = values;
	return result;
}

Dictionary PetLanguage::_debug_get_stack_level_members(int32_t p_level, int32_t p_max_subitems, int32_t p_max_depth) {
	Dictionary result;
	const StackFrame *frame = frame_at(p_level);
	// A static function has no instance (spec §4.8).
	if (!frame || !frame->members) {
		return result;
	}
	PackedStringArray names;
	Array values;
	for (const bytecode::Member &member : frame->image->module.members) {
		names.push_back(String::utf8(member.name.c_str()));
		values.push_back(to_variant(member.type, frame->members + member.offset));
	}
	result["members"] = names;
	result["values"] = values;
	return result;
}

void *PetLanguage::_debug_get_stack_level_instance(int32_t p_level) {
	return nullptr;
}

Dictionary PetLanguage::_debug_get_globals(int32_t p_max_subitems, int32_t p_max_depth) {
	return Dictionary();
}

String PetLanguage::_debug_parse_stack_level_expression(int32_t p_level, const String &p_expression, int32_t p_max_subitems, int32_t p_max_depth) {
	return String();
}

TypedArray<Dictionary> PetLanguage::_debug_get_current_stack_info() {
	TypedArray<Dictionary> result;
	for (int32_t level = 0; level < _debug_get_stack_level_count(); level++) {
		Dictionary info;
		info["file"] = _debug_get_stack_level_source(level);
		info["func"] = _debug_get_stack_level_function(level);
		info["line"] = _debug_get_stack_level_line(level);
		result.push_back(info);
	}
	return result;
}

/* Reloading */

void PetLanguage::_reload_all_scripts() {
}

void PetLanguage::_reload_scripts(const Array &p_scripts, bool p_soft_reload) {
	// Live reload from the editor (spec §12.4).
	for (int64_t i = 0; i < p_scripts.size(); i++) {
		const Ref<PetScript> script = p_scripts[i];
		if (script.is_valid()) {
			script->reload(true);
		}
	}
}

void PetLanguage::_reload_tool_script(const Ref<Script> &p_script, bool p_soft_reload) {
	const Ref<PetScript> script = p_script;
	if (script.is_valid()) {
		script->reload(true);
	}
}

/* Loader */

PackedStringArray PetLanguage::_get_recognized_extensions() const {
	PackedStringArray result;
	result.push_back("pet");
	return result;
}

TypedArray<Dictionary> PetLanguage::_get_public_functions() const {
	return TypedArray<Dictionary>();
}

Dictionary PetLanguage::_get_public_constants() const {
	return Dictionary();
}

TypedArray<Dictionary> PetLanguage::_get_public_annotations() const {
	return TypedArray<Dictionary>();
}

/* Profiling */

void PetLanguage::_profiling_start() {
}

void PetLanguage::_profiling_stop() {
}

void PetLanguage::_profiling_set_save_native_calls(bool p_enable) {
}

int32_t PetLanguage::_profiling_get_accumulated_data(ScriptLanguageExtensionProfilingInfo *p_info_array, int32_t p_info_max) {
	return 0;
}

int32_t PetLanguage::_profiling_get_frame_data(ScriptLanguageExtensionProfilingInfo *p_info_array, int32_t p_info_max) {
	return 0;
}

void PetLanguage::_frame() {
#ifdef PETRICHOR_PROBES
	task_queue::frame();
#endif
}

/* Global classes */

bool PetLanguage::_handles_global_class_type(const String &p_type) const {
	return p_type == PetScript::get_class_static();
}

Dictionary PetLanguage::_get_global_class_name(const String &p_path) const {
	const String source = FileAccess::get_file_as_string(p_path);
	const PetHeader header = parse_header(source);
	Dictionary result;
	if (header.class_name.is_empty()) {
		return result;
	}
	result["name"] = header.class_name;
	result["base_type"] = header.base;
	result["icon_path"] = String();
	result["is_abstract"] = false;
	result["is_tool"] = false;
	return result;
}

} // namespace petrichor
