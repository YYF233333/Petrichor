#include "adapter/pet_language.h"

#include "adapter/pet_script.h"
#include "runtime/debugger.h"
#include "runtime/task.h"
#include "runtime/text.h"
#include "runtime/vm.h"

#ifdef TOOLS_ENABLED
#include "adapter/editor/script_compiler.h"

#include <godot_cpp/classes/resource_loader.hpp>
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
	debugger::set_language(nullptr);
	std::lock_guard<std::mutex> lock(kept_mutex);
	kept_scripts.clear();
}

void PetLanguage::keep_loaded(const Ref<Script> &p_script) {
	std::lock_guard<std::mutex> lock(kept_mutex);
	kept_scripts.push_back(p_script);
}

/* Editor */

PackedStringArray PetLanguage::_get_reserved_words() const {
	static const char *const words[] = {
		// GDScript keywords (spec §2.2).
		"and",
		"as",
		"assert",
		"await",
		"break",
		"breakpoint",
		"class",
		"class_name",
		"const",
		"continue",
		"elif",
		"else",
		"enum",
		"extends",
		"for",
		"func",
		"if",
		"in",
		"is",
		"match",
		"namespace",
		"not",
		"or",
		"pass",
		"preload",
		"return",
		"self",
		"signal",
		"static",
		"super",
		"trait",
		"var",
		"void",
		"when",
		"while",
		"yield",
		// Added by this language (spec §2.2).
		"pub",
		"struct",
		"interface",
		"implements",
		"async",
		"None",
		// Literals and built-in constants.
		"true",
		"false",
		"null",
		"PI",
		"TAU",
		"INF",
		"NAN",
	};
	PackedStringArray result;
	for (const char *word : words) {
		result.push_back(word);
	}
	return result;
}

bool PetLanguage::_is_control_flow_keyword(const String &p_keyword) const {
	static const char *const words[] = {
		"break",
		"continue",
		"elif",
		"else",
		"for",
		"if",
		"match",
		"pass",
		"return",
		"when",
		"while",
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
	Array warnings;
#ifdef TOOLS_ENABLED
	std::vector<compiler::Diagnostic> diagnostics;
	std::vector<compiler::Diagnostic> warning_diagnostics;
	compile_script(p_path, p_script, &diagnostics, false, CompileMode::Full, &warning_diagnostics);
	// Those of the scenes are listed, but the script stays valid (architecture §6.4).
	result["valid"] = diagnostics.empty();
	const std::vector<compiler::Diagnostic> placed = placed_errors(p_path);
	diagnostics.insert(diagnostics.end(), placed.begin(), placed.end());
	for (const compiler::Diagnostic &diagnostic : diagnostics) {
		Dictionary error;
		error["path"] = p_path;
		error["line"] = diagnostic.line;
		error["column"] = diagnostic.column;
		error["message"] = to_godot(diagnostic.message);
		errors.push_back(error);
	}
	// Hints too, the engine having no other place for them; the editor's
	// [Ignore] puts the name into a @warning_ignore above the line
	// (editor/script/script_text_editor.cpp:380-404).
	for (const compiler::Diagnostic &diagnostic : warning_diagnostics) {
		Dictionary warning;
		warning["start_line"] = diagnostic.line;
		warning["end_line"] = diagnostic.line;
		warning["leftmost_column"] = diagnostic.column;
		warning["rightmost_column"] = diagnostic.column;
		warning["code"] = 0;
		warning["string_code"] = to_godot(diagnostic.warning);
		warning["message"] = to_godot(diagnostic.message);
		warnings.push_back(warning);
	}
#else
	result["valid"] = true;
#endif
	result["errors"] = errors;
	result["warnings"] = warnings;
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
	return true;
}

bool PetLanguage::_can_inherit_from_file() const {
	return true;
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
	Array options;
	String call_hint;
	bool force = false;
#ifdef TOOLS_ENABLED
	// By compiler::CompletionKind.
	static const CodeCompletionKind KINDS[] = {
		CODE_COMPLETION_KIND_CLASS,
		CODE_COMPLETION_KIND_FUNCTION,
		CODE_COMPLETION_KIND_SIGNAL,
		CODE_COMPLETION_KIND_VARIABLE,
		CODE_COMPLETION_KIND_MEMBER,
		CODE_COMPLETION_KIND_ENUM,
		CODE_COMPLETION_KIND_CONSTANT,
		CODE_COMPLETION_KIND_NODE_PATH,
		CODE_COMPLETION_KIND_FILE_PATH,
		CODE_COMPLETION_KIND_PLAIN_TEXT,
		CODE_COMPLETION_KIND_KEYWORD,
	};
	// By compiler::CompletionItem::Place.
	static const int LOCATIONS[] = { LOCATION_LOCAL, LOCATION_PARENT_MASK, LOCATION_OTHER_USER_CODE, LOCATION_OTHER };
	const compiler::Completion found = completion(p_path, to_utf8(p_code));
	call_hint = to_godot(found.call_hint);
	force = found.force;
	for (const compiler::CompletionItem &item : found.items) {
		Dictionary option;
		option["kind"] = KINDS[int(item.kind)];
		const String name = to_godot(item.name);
		const String insert = to_godot(item.insert);
		// As GDScript shows a function, and an override as it goes in
		// (modules/gdscript/gdscript_editor.cpp:1376-1384, 3709-3711).
		option["display"] = item.kind != compiler::CompletionKind::Function ? name : (insert.ends_with("(") ? name + String::utf8("(…)") : insert);
		option["insert_text"] = insert;
		option["font_color"] = Color();
		option["icon"] = Variant();
		option["default_value"] = Variant();
		option["location"] = LOCATIONS[int(item.place)] | (item.place == compiler::CompletionItem::Place::Member ? item.depth : 0);
		options.push_back(option);
	}
#endif
	Dictionary result;
	result["result"] = OK;
	result["options"] = options;
	result["force"] = force;
	result["call_hint"] = call_hint;
	return result;
}

Dictionary PetLanguage::_lookup_code(const String &p_code, const String &p_symbol, const String &p_path, Object *p_owner) const {
	Dictionary result;
	result["result"] = ERR_UNAVAILABLE;
	result["type"] = LOOKUP_RESULT_SCRIPT_LOCATION;
#ifdef TOOLS_ENABLED
	// The editor marks the name with U+FFFF (scene/gui/code_edit.cpp:2702).
	const int64_t mark = p_code.find(String::chr(0xFFFF));
	if (mark < 0) {
		return result;
	}
	const std::string before = to_utf8(p_code.substr(0, mark));
	const std::optional<compiler::Lookup> found = lookup_name(p_path, before + to_utf8(p_code.substr(mark + 1)), uint32_t(before.size()));
	if (!found) {
		return result;
	}
	// By compiler::SymbolKind.
	static const LookupResultType TYPES[] = {
		LOOKUP_RESULT_CLASS,
		LOOKUP_RESULT_CLASS_CONSTANT,
		LOOKUP_RESULT_CLASS_PROPERTY,
		LOOKUP_RESULT_CLASS_METHOD,
		LOOKUP_RESULT_CLASS_SIGNAL,
		LOOKUP_RESULT_CLASS_ENUM,
		LOOKUP_RESULT_LOCAL_VARIABLE,
		LOOKUP_RESULT_LOCAL_CONSTANT,
	};
	result["result"] = OK;
	result["type"] = TYPES[int(found->kind)];
	result["class_name"] = to_godot(found->class_name);
	result["class_member"] = to_godot(found->member);
	result["doc_type"] = to_godot(found->type);
	result["description"] = to_godot(found->comment.description);
	result["is_deprecated"] = found->comment.deprecated.has_value();
	result["deprecated_message"] = to_godot(found->comment.deprecated.value_or(std::string()));
	result["is_experimental"] = found->comment.experimental.has_value();
	result["experimental_message"] = to_godot(found->comment.experimental.value_or(std::string()));
	if (!found->path.empty()) {
		const String path = to_godot(found->path);
		result["script"] = ResourceLoader::get_singleton()->load(path, "Script");
		result["script_path"] = path;
		result["location"] = found->line;
	}
#endif
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

// A value encoded as far as walks go; one nested deeper is no error of the
// program being inspected (spec §3.6).
static Variant shown(const TypeImage &p_type, const uint8_t *p_slot) {
	const Variant value = encode(p_type, p_slot);
	walked_too_deep();
	return value;
}

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
	return frame ? to_godot(frame->function->name) : String();
}

String PetLanguage::_debug_get_stack_level_source(int32_t p_level) const {
	const StackFrame *frame = frame_at(p_level);
	return frame ? String(frame->image->source) : String();
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
			names.push_back(to_godot(slot.name));
			values.push_back(shown(frame->image->types[slot.type], frame->frame + slot.offset));
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
	const ClassImage &image = *(frame->self_image ? frame->self_image : frame->image);
	for (const bytecode::Member &member : image.module.members) {
		names.push_back(to_godot(member.name));
		values.push_back(shown(image.types[member.type], frame->members + member.offset));
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
	tasks::frame();
}

/* Global classes */

bool PetLanguage::_handles_global_class_type(const String &p_type) const {
	return p_type == PetScript::get_class_static();
}

// Only the editor asks; exported games read the list it saved.
Dictionary PetLanguage::_get_global_class_name(const String &p_path) const {
	Dictionary result;
#ifdef TOOLS_ENABLED
	const String source = FileAccess::get_file_as_string(p_path);
	const PetHeader header = parse_header(source);
	if (header.class_name.is_empty()) {
		return result;
	}
	result["name"] = header.class_name;
	result["base_type"] = script_global_base(p_path, source);
	result["icon_path"] = header_icon_path(header, p_path);
	result["is_abstract"] = header.is_abstract;
	result["is_tool"] = false;
#endif
	return result;
}

} // namespace petrichor
