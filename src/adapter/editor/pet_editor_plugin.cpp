#include "adapter/editor/pet_editor_plugin.h"

#include "adapter/editor/class_db_api.h"
#include "adapter/editor/project_files.h"
#include "adapter/editor/scene_index.h"
#include "adapter/editor/script_compiler.h"
#include "adapter/pet_script.h"
#include "compiler/ast.h"
#include "compiler/compiler.h"
#include "compiler/formatter.h"
#include "compiler/lexer.h"
#include "compiler/migration.h"
#include "runtime/text.h"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/code_edit.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/editor_toaster.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/popup_menu.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/script_editor.hpp>
#include <godot_cpp/classes/script_editor_base.hpp>
#include <godot_cpp/classes/tab_bar.hpp>
#include <godot_cpp/classes/text_edit.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

using namespace godot;

namespace petrichor {

void PetCodeMenu::_popup_menu(const PackedStringArray &p_paths) {
	// The path given is the code editor's node, not the script's
	// (editor/script/script_text_editor.cpp:2600-2601; ed1daf0).
	const Ref<Script> script = EditorInterface::get_singleton()->get_script_editor()->get_current_script();
	if (Object::cast_to<PetScript>(script.ptr())) {
		add_context_menu_item("Format", callable_mp(this, &PetCodeMenu::format));
		add_context_menu_item_from_shortcut("Find References", find_references);
		add_context_menu_item_from_shortcut("Rename...", rename);
		add_context_menu_item_from_shortcut("Quick Fix...", quick_fix);
	}
}

namespace {

// Applies `p_edits`, in bytes of `p_source`, the text of `p_code_edit`, as one
// step to undo, as the editor trims trailing whitespace
// (editor/gui/code_editor.cpp:1201-1226; ed1daf0). Each removes its range and
// inserts its text there, the last first, so that the offsets of the others
// hold. Carets and the marks of lines, as breakpoints, stay with the text. A
// removal over several lines keeps the marks of its first line only, and the
// lines an insertion adds have none (scene/gui/text_edit.cpp:513-565,
// 4356-4385, 9248-9337).
void apply_edits(CodeEdit *p_code_edit, const std::string &p_source, const std::vector<compiler::Edit> &p_edits) {
	// An empty operation would join the last one to undo to those before it
	// (scene/gui/text_edit.cpp:4722-4742).
	if (p_edits.empty()) {
		return;
	}
	const std::vector<uint32_t> starts = compiler::line_starts_of(p_source);
	p_code_edit->begin_complex_operation();
	for (auto edit = p_edits.rbegin(); edit != p_edits.rend(); ++edit) {
		const compiler::SourcePosition begin = compiler::position_in(p_source, starts, edit->begin);
		if (edit->end > edit->begin) {
			const compiler::SourcePosition end = compiler::position_in(p_source, starts, edit->end);
			p_code_edit->remove_text(begin.line - 1, begin.column - 1, end.line - 1, end.column - 1);
		}
		if (!edit->text.empty()) {
			p_code_edit->insert_text(to_godot(edit->text), begin.line - 1, begin.column - 1);
		}
	}
	p_code_edit->merge_overlapping_carets();
	p_code_edit->end_complex_operation();
}

// Whether the tab of the script at `p_path` has the text that edits were
// computed on; not when the LSP client has it open with another (architecture
// §2.4), which it tells.
bool tab_has(const CodeEdit *p_code_edit, const std::string &p_path, const std::string &p_text) {
	if (to_utf8(p_code_edit->get_text()) == without_cr(p_text)) {
		return true;
	}
	EditorInterface::get_singleton()->get_editor_toaster()->push_toast(vformat("%s: its text in the LSP client differs from its tab in Godot, so nothing was changed.", to_godot(p_path)), EditorToaster::SEVERITY_WARNING);
	return false;
}

} // namespace

void PetCodeMenu::format(Object *p_code_edit) {
	CodeEdit *code_edit = Object::cast_to<CodeEdit>(p_code_edit);
	ERR_FAIL_NULL(code_edit);
	const std::string source = to_utf8(code_edit->get_text());
	std::string error;
	const std::optional<std::string> result = compiler::format(source, &error);
	if (!result) {
		EditorInterface::get_singleton()->get_editor_toaster()->push_toast(String::utf8(error.c_str()), EditorToaster::SEVERITY_WARNING);
		return;
	}
	const std::vector<compiler::Edit> edits = compiler::format_edits(source, *result);
	int64_t operations = 0;
	for (const compiler::Edit &edit : edits) {
		operations += int64_t(edit.end > edit.begin) + int64_t(!edit.text.empty());
	}
	// Beyond what the undo stack holds, which the editor reads when it is made,
	// one undo would restore only part of the text
	// (scene/gui/text_edit.cpp:4785-4796, 8198-8200, 9407; ed1daf0). The whole
	// text is then replaced, and the marks of its lines are lost.
	if (operations <= int64_t(ProjectSettings::get_singleton()->get_setting("gui/common/text_edit_undo_stack_max_size"))) {
		apply_edits(code_edit, source, edits);
		return;
	}
	const int line = code_edit->get_caret_line();
	const int column = code_edit->get_caret_column();
	const double scroll = code_edit->get_v_scroll();
	apply_edits(code_edit, source, { { 0, uint32_t(source.size()), *result } });
	code_edit->set_caret_line(std::min(line, code_edit->get_line_count() - 1));
	code_edit->set_caret_column(column);
	code_edit->set_v_scroll(scroll);
}

void PetEditorPlugin::_bind_methods() {
	// For the editor's undo history, which calls methods by name.
	ClassDB::bind_method(D_METHOD("_rename_declaration", "kind", "scope", "name", "new_name", "candidates", "undo", "apply"), &PetEditorPlugin::rename_declaration, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("_change_scripts", "edits", "undo", "apply"), &PetEditorPlugin::change_scripts_again, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("_move_nodes", "scene", "moves", "undo"), &PetEditorPlugin::move_nodes);
}

static void resource_saved(const Ref<Resource> &p_resource) {
	file_seen(to_utf8(p_resource->get_path()));
}

void PetEditorPlugin::scene_saved(const String &p_path) {
	file_seen(to_utf8(p_path));
	if (lsp) {
		lsp->scene_saving();
	}
}

// After the resources that the editor saves with a scene, scripts too, and
// the external data of the plugins before this one (editor/editor_node.cpp:2521-2526,
// 2374-2408, editor/editor_data.cpp:421-425; ed1daf0).
void PetEditorPlugin::_save_external_data() {
	if (lsp) {
		lsp->resources_saved();
	}
}

void PetEditorPlugin::_enter_tree() {
	export_plugin.instantiate();
	add_export_plugin(export_plugin);
	const auto shortcut = [](const char *p_setting, const char *p_name, Key p_key, bool p_shift, bool p_alt) {
		Ref<InputEventKey> key;
		key.instantiate();
		key->set_keycode(p_key);
		key->set_shift_pressed(p_shift);
		key->set_alt_pressed(p_alt);
		Ref<Shortcut> made;
		made.instantiate();
		made->set_name(p_name);
		made->set_events(Array::make(key));
		// So that users can change it; Godot 4.4 does not bind add_shortcut.
		if (ClassDBSingleton::get_singleton()->class_has_method("EditorSettings", "add_shortcut")) {
			EditorInterface::get_singleton()->get_editor_settings()->call("add_shortcut", p_setting, made);
		}
		return made;
	};
	code_menu.instantiate();
	code_menu->find_references = shortcut("petrichor/find_references", "Find References", KEY_F12, true, true);
	code_menu->add_menu_shortcut(code_menu->find_references, callable_mp(this, &PetEditorPlugin::show_references));
	// F2, as Rename in the scene tree and the FileSystem docks, which take it
	// only where they have focus (editor/docks/scene_tree_dock.cpp:181-184, 5013;
	// editor/docks/filesystem_dock.cpp:2780, 3885, 4449; ed1daf0). The script
	// editor binds no F2.
	code_menu->rename = shortcut("petrichor/rename", "Rename", KEY_F2, false, false);
	code_menu->add_menu_shortcut(code_menu->rename, callable_mp(this, &PetEditorPlugin::ask_name));
	// Alt+Enter, as in JetBrains IDEs; the editor binds none, and Ctrl+. goes
	// to the next breakpoint (editor/script/script_text_editor.cpp:2672; ed1daf0).
	code_menu->quick_fix = shortcut("petrichor/quick_fix", "Quick Fix", KEY_ENTER, false, true);
	code_menu->add_menu_shortcut(code_menu->quick_fix, callable_mp(this, &PetEditorPlugin::list_fixes));
	add_context_menu_plugin(EditorContextMenuPlugin::CONTEXT_SLOT_SCRIPT_EDITOR_CODE, code_menu);
	fix_list = memnew(PopupMenu);
	fix_list->set_name("QuickFixes");
	fix_list->connect("id_pressed", callable_mp(this, &PetEditorPlugin::apply_listed));
	add_child(fix_list);
	ScriptEditor *script_editor = EditorInterface::get_singleton()->get_script_editor();
	script_editor->connect("editor_script_changed", callable_mp(this, &PetEditorPlugin::script_changed).unbind(1));
	// Shown again, as on going back to the Script screen, which does not check
	// the tab (editor/script/script_editor_plugin.cpp:4318-4325; ed1daf0).
	script_editor->connect("visibility_changed", callable_mp(this, &PetEditorPlugin::check_shown), CONNECT_DEFERRED);
	callable_mp(this, &PetEditorPlugin::script_changed).call_deferred();
	references = memnew(PetReferences);
	add_control_to_bottom_panel(references, "References");
	name_popup = memnew(PopupPanel);
	name_edit = memnew(LineEdit);
	name_edit->set_custom_minimum_size(Vector2(240, 0) * EditorInterface::get_singleton()->get_editor_scale());
	name_edit->connect("text_submitted", callable_mp(this, &PetEditorPlugin::rename_to));
	name_popup->add_child(name_edit);
	add_child(name_popup);
	highlighter.instantiate();
	script_editor->register_syntax_highlighter(highlighter);
	// editor/plugins/editor_plugin.cpp:284-289 (ed1daf0).
	connect("resource_saved", callable_mp_static(&resource_saved));
	connect("scene_saved", callable_mp(this, &PetEditorPlugin::scene_saved));
	connect("resource_saved", callable_mp(this, &PetEditorPlugin::files_changed).unbind(1));
	connect("scene_saved", callable_mp(this, &PetEditorPlugin::files_changed).unbind(1));
	EditorInterface::get_singleton()->get_resource_filesystem()->connect("filesystem_changed", callable_mp(this, &PetEditorPlugin::files_changed));
	// Such as the main scene and autoloads (core/config/project_settings.cpp:575, ed1daf0).
	ProjectSettings::get_singleton()->connect("settings_changed", callable_mp(this, &PetEditorPlugin::files_changed));
	// An edit of an open scene, its undo and redo, as the editor learns that a
	// scene changed (editor/editor_node.cpp:8406-8409,
	// editor/editor_undo_redo_manager.cpp:298, 327, 380). Once they pause for
	// as long as the script editor waits before it checks a script
	// (editor/gui/code_editor.cpp:1014, 1166, 1696-1699; ed1daf0).
	marks = memnew(Timer);
	marks->set_one_shot(true);
	marks->set_wait_time(EditorInterface::get_singleton()->get_editor_settings()->get_setting("text_editor/completion/idle_parse_delay"));
	marks->connect("timeout", callable_mp(this, &PetEditorPlugin::scenes_paused));
	add_child(marks);
	get_undo_redo()->connect("history_changed", callable_mp(this, &PetEditorPlugin::history_changed));
	get_undo_redo()->connect("version_changed", callable_mp(this, &PetEditorPlugin::history_changed));
	// Closing a scene drops its history, which adds no step
	// (editor/editor_data.cpp:674-699; ed1daf0).
	connect("scene_closed", callable_mp(marks, &Timer::start).bind(-1.0).unbind(1));
	// A rename or a move in the scene editor, but not an undo or a redo, nor
	// a change outside an action, as @tool code makes
	// (editor/editor_undo_redo_manager.cpp:252-260, 298, 327, 380; ed1daf0).
	get_undo_redo()->connect("history_changed", callable_mp(this, &PetEditorPlugin::nodes_committed));
	get_undo_redo()->connect("version_changed", callable_mp(this, &PetEditorPlugin::take_scene_nodes));
	connect("scene_changed", callable_mp(this, &PetEditorPlugin::take_scene_nodes).unbind(1));
	get_tree()->connect("node_renamed", callable_mp(this, &PetEditorPlugin::node_touched));
	get_tree()->connect("node_added", callable_mp(this, &PetEditorPlugin::node_touched));
	// To tell a running game to reload a script written (script_written()).
	sessions.instantiate();
	add_debugger_plugin(sessions);
	const PackedStringArray args = OS::get_singleton()->get_cmdline_user_args();
	const bool headless = DisplayServer::get_singleton()->get_name() == "headless";
	for (const char *command : { "check", "build", "format", "migrate" }) {
		if (args.has(String("--petrichor-") + command)) {
			callable_mp(this, &PetEditorPlugin::run_headless).call_deferred(String(command));
			return;
		}
	}
	if (args.has("--petrichor-lsp") || !headless) {
		const auto run = [this](const std::string &p_command, const std::vector<std::string> &p_args, const std::set<std::string> &p_conflicts) { return run_command(p_command, p_args, p_conflicts); };
		lsp = std::make_unique<LspServer>(sessions, [this] { changed(); }, [this](const std::set<std::string> &p_files) { reload_written(p_files); }, run);
		get_tree()->connect("process_frame", callable_mp(this, &PetEditorPlugin::poll_lsp));
	}
	// Where pet finds an editor to run headless once none is open
	// (architecture §6.5).
	const String recorded = "res://.godot/petrichor/editor_path";
	const String executable = OS::get_singleton()->get_executable_path();
	if (!headless && (!FileAccess::file_exists(recorded) || FileAccess::get_file_as_string(recorded) != executable)) {
		DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path(recorded.get_base_dir()));
		const Ref<FileAccess> file = FileAccess::open(recorded, FileAccess::WRITE);
		if (file.is_valid()) {
			file->store_string(executable);
		}
	}
}

void PetEditorPlugin::poll_lsp() {
	if (lsp) {
		lsp->poll();
	}
}

void PetEditorPlugin::files_changed() {
	disk_changed();
	pack_open_scenes();
	changed();
}

void PetEditorPlugin::scenes_paused() {
	pack_open_scenes();
	changed();
}

// Once for each revision of the workspace: the nodes ask for their errors
// again, and the tab shown checks its script again (architecture §2.4).
void PetEditorPlugin::changed() {
	const uint64_t revision = workspace().revision();
	if (revision == checked) {
		return;
	}
	checked = revision;
	check_shown();
	PetScript::update_warnings();
	if (lsp) {
		lsp->settled();
	}
}

// The tab of a script of this language, while it shows, if the workspace
// changed since it last checked its script.
void PetEditorPlugin::check_shown() {
	CodeEdit *code_edit = fixing_code_edit();
	if (code_edit && code_edit->is_visible_in_tree() && tab_checks[code_edit->get_instance_id()] != workspace().revision()) {
		code_edit->get_parent()->emit_signal("validate_script");
	}
}

void PetEditorPlugin::history_changed() {
	marks->start();
	if (!recorded.empty()) {
		const uint64_t version = get_undo_redo()->get_history_undo_redo(EditorUndoRedoManager::GLOBAL_HISTORY)->get_version();
		elsewhere += version == global_version;
		global_version = version;
	}
}

void PetEditorPlugin::take_scene_nodes() {
	scene_nodes.clear();
	touched.clear();
	const Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	scene_root = root ? root->get_instance_id() : 0;
	if (root) {
		scene_nodes[scene_root] = ".";
	}
	const TypedArray<Node> nodes = root ? root->find_children("*", "", true, false) : TypedArray<Node>();
	for (int64_t i = 0; i < nodes.size(); i++) {
		Node *node = Object::cast_to<Node>(nodes[i]);
		if (node->get_owner() == root) {
			scene_nodes[node->get_instance_id()] = String(root->get_path_to(node));
		}
	}
}

// A node moves into the tree again (editor/docks/scene_tree_dock.cpp:2562-2563;
// ed1daf0).
void PetEditorPlugin::node_touched(Node *p_node) {
	if (get_undo_redo()->is_committing_action() && !merging) {
		touched.insert(p_node->get_instance_id());
	}
}

// The scripts follow the nodes that the action just committed renamed or
// moved, with their descendants, in a step merged into that action
// (architecture §2.3). A node made the root moves them all
// (editor/docks/scene_tree_dock.cpp:1111-1135; ed1daf0). It runs after the
// undo or the redo returns, as a refactoring does (record()).
void PetEditorPlugin::nodes_committed() {
	Node *root = EditorInterface::get_singleton()->get_edited_scene_root();
	if (merging || (touched.empty() && root && root->get_instance_id() == scene_root)) {
		return;
	}
	if (touched.empty() || !root || !scene_nodes.count(root->get_instance_id()) || root->get_scene_file_path().is_empty()) {
		take_scene_nodes();
		return;
	}
	Dictionary moves;
	Dictionary back;
	for (const auto &[id, was] : scene_nodes) {
		Node *node = Object::cast_to<Node>(ObjectDB::get_instance(id));
		bool moved = false;
		for (const Node *at = node && (node == root || root->is_ancestor_of(node)) ? node : nullptr; at && !moved; at = at == root ? nullptr : at->get_parent()) {
			moved = touched.count(at->get_instance_id());
		}
		const String now = moved ? String(root->get_path_to(node)) : was;
		if (now != was) {
			moves[was] = now;
			back[now] = was;
		}
	}
	take_scene_nodes();
	if (moves.is_empty()) {
		return;
	}
	EditorUndoRedoManager *undo_redo = get_undo_redo();
	const String scene = root->get_scene_file_path();
	merging = true;
	undo_redo->create_action(undo_redo->get_history_undo_redo(undo_redo->get_object_history_id(root))->get_current_action_name(), UndoRedo::MERGE_ALL, root);
	undo_redo->add_do_method(this, "callv", "call_deferred", Array::make("_move_nodes", scene, moves, false));
	undo_redo->add_undo_method(this, "callv", "call_deferred", Array::make("_move_nodes", scene, back, true));
	undo_redo->commit_action();
	merging = false;
}

// From the texts as they are, as an undo of a rename recomputes it
// (rename_declaration()). Each change and each path left is a line of the
// Output panel.
void PetEditorPlugin::move_nodes(const String &p_scene, const Dictionary &p_moves, bool p_undo) {
	pack_open_scenes();
	std::map<std::string, std::string> moves;
	const Array keys = p_moves.keys();
	for (int64_t i = 0; i < keys.size(); i++) {
		moves[to_utf8(keys[i])] = to_utf8(p_moves[keys[i]]);
	}
	const compiler::MovedNodes found = compiler::move_nodes(analysis(), to_utf8(p_scene), moves);
	FileEdits changes;
	for (const compiler::RenameEdit &edit : found.edits) {
		changes[edit.occurrence.file].push_back(edit.edit);
	}
	if (!changes.empty() && !change_files(changes, p_undo)) {
		return;
	}
	for (const compiler::RenameEdit &edit : found.edits) {
		UtilityFunctions::print(to_godot(edit.occurrence.file + ":" + std::to_string(edit.occurrence.line) + " " + edit.edit.old_text + " → " + edit.edit.new_text));
	}
	for (const compiler::Diagnostic &report : found.reports) {
		UtilityFunctions::print(to_godot(report.file + ":" + std::to_string(report.line) + ": " + report.message));
	}
}

// A context menu's shortcut in the code editor acts only while the menu is
// open (editor/inspector/editor_context_menu_plugin.cpp:155-176,
// editor/script/script_editor_plugin.cpp:3111; ed1daf0), so the plugin takes
// it wherever the script editor shows.
void PetEditorPlugin::_shortcut_input(const Ref<InputEvent> &p_event) {
	ScriptEditor *script_editor = EditorInterface::get_singleton()->get_script_editor();
	ScriptEditorBase *current = script_editor->get_current_editor();
	if (!p_event->is_pressed() || p_event->is_echo() || !current) {
		return;
	}
	if (code_menu->find_references->matches_event(p_event) && script_editor->is_visible_in_tree()) {
		show_references(current->get_base_editor());
		get_viewport()->set_input_as_handled();
	} else if (code_menu->rename->matches_event(p_event) && current->get_base_editor()->has_focus()) {
		ask_name(current->get_base_editor());
		get_viewport()->set_input_as_handled();
	} else if (code_menu->quick_fix->matches_event(p_event) && current->get_base_editor()->has_focus() && Object::cast_to<PetScript>(script_editor->get_current_script().ptr())) {
		list_fixes(current->get_base_editor());
		get_viewport()->set_input_as_handled();
	}
}

namespace {

// The script open in the script editor at `p_code_edit`, if of this language,
// and the offset of the caret in the workspace's text, which references and
// renaming read. That text may have the carriage returns that the tab drops
// (without_cr()); they only end lines, so lines and columns agree.
std::optional<std::pair<String, uint32_t>> at_caret(const Object *p_code_edit) {
	const CodeEdit *code_edit = Object::cast_to<CodeEdit>(p_code_edit);
	const Ref<Script> script = EditorInterface::get_singleton()->get_script_editor()->get_current_script();
	if (!code_edit || !Object::cast_to<PetScript>(script.ptr())) {
		return std::nullopt;
	}
	const std::string text = workspace().script_source(to_utf8(script->get_path())).value_or(std::string());
	const std::vector<uint32_t> starts = compiler::line_starts_of(text);
	uint32_t offset = starts[std::min(size_t(code_edit->get_caret_line()), starts.size() - 1)];
	for (int column = code_edit->get_caret_column(); column > 0 && offset < text.size() && text[offset] != '\r' && text[offset] != '\n'; column--) {
		offset++;
		while (offset < text.size() && (static_cast<unsigned char>(text[offset]) & 0xC0) == 0x80) {
			offset++;
		}
	}
	return std::make_pair(script->get_path(), offset);
}

void toast(const String &p_message, EditorToaster::Severity p_severity = EditorToaster::SEVERITY_INFO) {
	EditorInterface::get_singleton()->get_editor_toaster()->push_toast(p_message, p_severity);
}

bool is_script(const std::string &p_file) {
	const String extension = to_godot(p_file).get_extension().to_lower();
	return extension == "pet" || extension == "gd" || extension == "cs";
}

std::string file_bytes(const String &p_path) {
	const PackedByteArray bytes = FileAccess::get_file_as_bytes(p_path);
	return bytes.is_empty() ? std::string() : std::string(reinterpret_cast<const char *>(bytes.ptr()), size_t(bytes.size()));
}

// Whether the file has the bytes then. Saving safely, the editor's default,
// writes another file, which replaces it only as it closes, and fails there
// (drivers/windows/file_access_windows.cpp:197-216, 245-277; ed1daf0).
bool write_file(const String &p_path, const std::string &p_bytes) {
	const Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_valid()) {
		PackedByteArray data;
		data.resize(int64_t(p_bytes.size()));
		if (!p_bytes.empty()) {
			std::memcpy(data.ptrw(), p_bytes.data(), p_bytes.size());
		}
		file->store_buffer(data);
		file->close();
	}
	const bool written = file.is_valid() && file_bytes(p_path) == p_bytes;
	if (!written) {
		ERR_PRINT(vformat("Cannot write %s.", p_path));
	}
	return written;
}

// Gives each file its bytes, or removes it for none: all of them or none, as
// once one cannot be written, each gets back what it had (architecture §2.3).
// The one that failed, or "".
std::string write_all(const std::map<std::string, std::optional<std::string>> &p_files) {
	std::map<std::string, std::optional<std::string>> had;
	for (const auto &[file, bytes] : p_files) {
		const String path = to_godot(file);
		had[file] = FileAccess::file_exists(path) ? std::optional<std::string>(file_bytes(path)) : std::nullopt;
		if (bytes ? write_file(path, *bytes) : DirAccess::remove_absolute(path) == OK) {
			continue;
		}
		for (const auto &[each, before] : had) {
			const String undone = to_godot(each);
			if (before && (!FileAccess::file_exists(undone) || file_bytes(undone) != *before)) {
				write_file(undone, *before);
			} else if (!before && FileAccess::file_exists(undone)) {
				DirAccess::remove_absolute(undone);
			}
		}
		return file;
	}
	return std::string();
}

// A byte order mark is no text, as the build reads a script
// (core/string/ustring.cpp:1756-1759, ed1daf0), and is written back.
std::string byte_order_mark(const std::string &p_bytes) {
	return p_bytes.rfind("\xEF\xBB\xBF", 0) == 0 ? "\xEF\xBB\xBF" : "";
}

// The scenes whose nodes `p_node` and its descendants show: each instance's,
// at any depth, with the scenes it inherits.
void scenes_shown(const Node *p_node, std::set<std::string> &r_scenes) {
	for (std::string scene = to_utf8(p_node->get_scene_file_path()); !scene.empty() && r_scenes.insert(scene).second;) {
		const std::optional<compiler::Scene> read = SceneIndex::get().scene(scene);
		scene = read ? read->base : std::string();
	}
	for (int64_t i = 0; i < p_node->get_child_count(); i++) {
		scenes_shown(p_node->get_child(i), r_scenes);
	}
}

// The paths of the open scenes that show nodes of a scene in `p_files`.
std::vector<std::string> open_scenes_showing(const std::set<std::string> &p_files) {
	std::vector<std::string> showing;
	const TypedArray<Node> roots = EditorInterface::get_singleton()->get_open_scene_roots();
	for (int64_t i = 0; i < roots.size(); i++) {
		const Node *root = Object::cast_to<Node>(roots[i]);
		std::set<std::string> shown;
		scenes_shown(root, shown);
		if (!root->get_scene_file_path().is_empty() && std::any_of(shown.begin(), shown.end(), [&](const std::string &p_scene) { return p_files.count(p_scene) > 0; })) {
			showing.push_back(to_utf8(root->get_scene_file_path()));
		}
	}
	return showing;
}

bool is_unsaved_scene(const std::string &p_file) {
	return EditorInterface::get_singleton()->get_unsaved_scenes().has(to_godot(p_file));
}

// The tabs of the open scenes, in order (editor/scene/editor_scene_tabs.cpp:263-292,
// 463-472, editor/editor_node.cpp:8847; ed1daf0).
TabBar *scene_tabs() {
	const TypedArray<Node> found = EditorInterface::get_singleton()->get_base_control()->find_children("*", "EditorSceneTabs", true, false);
	const TypedArray<Node> bars = found.is_empty() ? TypedArray<Node>() : Object::cast_to<Node>(found[0])->find_children("*", "TabBar", true, false);
	return bars.is_empty() ? nullptr : Object::cast_to<TabBar>(bars[0]);
}

// Whether `p_text` still has the text that `p_edit` replaces.
bool intact(const std::string &p_text, const compiler::CheckedEdit &p_edit) {
	return p_edit.begin <= p_edit.end && p_edit.end <= p_text.size() && p_text.compare(p_edit.begin, p_edit.end - p_edit.begin, p_edit.old_text) == 0;
}

// The gutter that shows the bulb of quick fixes in `p_code_edit`; -1 before
// it is added.
int fix_gutter(const CodeEdit *p_code_edit) {
	for (int32_t i = 0; i < p_code_edit->get_gutter_count(); i++) {
		if (p_code_edit->get_gutter_name(i) == "petrichor_fixes") {
			return i;
		}
	}
	return -1;
}

// As the editor's history keeps them: each [file, begin, end, old text, new text].
Array to_array(const FileEdits &p_changes) {
	Array edits;
	for (const auto &[file, changes] : p_changes) {
		for (const compiler::CheckedEdit &edit : changes) {
			edits.push_back(Array::make(to_godot(file), edit.begin, edit.end, to_godot(edit.old_text), to_godot(edit.new_text)));
		}
	}
	return edits;
}

FileEdits from_array(const Array &p_edits) {
	FileEdits changes;
	for (int64_t i = 0; i < p_edits.size(); i++) {
		const Array edit = p_edits[i];
		changes[to_utf8(edit[0])].push_back({ uint32_t(int64_t(edit[1])), uint32_t(int64_t(edit[2])), to_utf8(edit[3]), to_utf8(edit[4]) });
	}
	return changes;
}

// What undoes `p_changes`, whose edits are in order in each file.
FileEdits inverse(const FileEdits &p_changes) {
	FileEdits undoing;
	for (const auto &[file, changes] : p_changes) {
		int64_t delta = 0;
		for (const compiler::CheckedEdit &edit : changes) {
			const uint32_t begin = uint32_t(edit.begin + delta);
			undoing[file].push_back({ begin, uint32_t(begin + edit.new_text.size()), edit.new_text, edit.old_text });
			delta += int64_t(edit.new_text.size()) - (edit.end - edit.begin);
		}
	}
	return undoing;
}

} // namespace

// The plugin sees a key before the CodeEdit (scene/main/viewport.cpp:3537),
// which takes Undo and Redo also with nothing to undo or redo
// (scene/gui/text_edit.cpp:2737-2746, 4766-4768; ed1daf0). In a script of this
// language, they undo or redo a refactoring that changed it, when the
// editor's Undo or Redo would: Undo once the script has nothing to undo or
// the refactoring's edit last, and Redo once it has nothing to redo or would
// redo the edit that undoing the refactoring took off (change_files()). Only
// redoing tells which edit is next, by the version it restores
// (scene/gui/text_edit.cpp:4840, 4848; ed1daf0), so the script redoes, and
// undoes again for a refactoring's edit, which it never redoes alone. One that
// would be refused is not made, and the histories do not move (architecture
// §2.3).
void PetEditorPlugin::_input(const Ref<InputEvent> &p_event) {
	const bool undo = p_event->is_action_pressed("ui_undo", true, true);
	if (!undo && !p_event->is_action_pressed("ui_redo", true, true)) {
		return;
	}
	ScriptEditor *script_editor = EditorInterface::get_singleton()->get_script_editor();
	ScriptEditorBase *current = script_editor->get_current_editor();
	CodeEdit *code_edit = current ? Object::cast_to<CodeEdit>(current->get_base_editor()) : nullptr;
	const Ref<Script> script = script_editor->get_current_script();
	if (!code_edit || !code_edit->has_focus() || !Object::cast_to<PetScript>(script.ptr())) {
		return;
	}
	const uint64_t id = code_edit->get_instance_id();
	if (undo && code_edit->has_undo() && !refactored.count({ id, code_edit->get_version() })) {
		return;
	}
	if (!undo && code_edit->has_redo()) {
		code_edit->redo();
		get_viewport()->set_input_as_handled();
		if (!taken_off.count({ id, code_edit->get_version() })) {
			return;
		}
		code_edit->undo();
	}
	UndoRedo *global = get_undo_redo()->get_history_undo_redo(EditorUndoRedoManager::GLOBAL_HISTORY);
	const int32_t index = global->get_current_action() + (undo ? 0 : 1);
	const auto found = recorded.find(index);
	if (found == recorded.end() || index >= global->get_history_count() || global->get_action_name(index) != found->second.name || !found->second.files.count(to_utf8(script->get_path())) || found->second.elsewhere != elsewhere) {
		return;
	}
	const Array &call = undo ? found->second.undo : found->second.redo;
	Array tried = call.slice(1);
	tried.push_back(false);
	const String refusal = callv(call[0], tried);
	if (!refusal.is_empty()) {
		toast(refusal, EditorToaster::SEVERITY_WARNING);
		get_viewport()->set_input_as_handled();
		return;
	}
	// The item of the main menu that the key chooses elsewhere
	// (editor/editor_node.cpp:3584-3634, 8036-8037, 8267-8279; ed1daf0).
	const TypedArray<Node> bars = EditorInterface::get_singleton()->get_base_control()->find_children("*", "MenuBar", true, false);
	for (int64_t i = 0; i < bars.size(); i++) {
		const TypedArray<Node> menus = Object::cast_to<Node>(bars[i])->get_children();
		for (int64_t j = 0; j < menus.size(); j++) {
			PopupMenu *menu = Object::cast_to<PopupMenu>(menus[j]);
			if (menu && menu->activate_item_by_event(p_event)) {
				get_viewport()->set_input_as_handled();
				return;
			}
		}
	}
}

// From the declaration, found by its id as undoing a rename finds it
// (architecture §2.3); then the reference chosen, by its order in its file.
// Not once the panel shows another search.
void PetEditorPlugin::show_references(Object *p_code_edit) {
	const std::optional<std::pair<String, uint32_t>> at = at_caret(p_code_edit);
	if (!at) {
		return;
	}
	std::optional<std::vector<compiler::Occurrence>> found = find_references(to_utf8(at->first), at->second);
	if (!found) {
		toast("No declaration at the caret.");
		return;
	}
	references->set_found(std::move(*found));
	make_bottom_panel_item_visible(references);
}

// Not where renaming is refused whatever the new name, as the language
// server's prepareRename refuses it (architecture §2.4).
void PetEditorPlugin::ask_name(Object *p_code_edit) {
	const std::optional<std::pair<String, uint32_t>> at = at_caret(p_code_edit);
	if (!at) {
		return;
	}
	const std::string path = to_utf8(at->first);
	const compiler::RenamedName name = renamed_name(path, at->second);
	if (name.refusal) {
		toast(to_godot(name.refusal->message));
		return;
	}
	renaming.path = path;
	renaming.offset = at->second;
	const CodeEdit *code_edit = Object::cast_to<CodeEdit>(p_code_edit);
	name_edit->set_text(to_godot(workspace().script_source(path).value_or(std::string()).substr(name.begin, name.end - name.begin)));
	name_popup->set_position(Vector2i(code_edit->get_screen_position() + code_edit->get_caret_draw_pos(0)));
	name_popup->reset_size();
	name_popup->popup();
	name_edit->grab_focus();
	name_edit->select_all();
}

// Previews the changes when they reach a scene or a resource file or include
// candidates, as JetBrains IDEs do; applies them at once otherwise.
void PetEditorPlugin::rename_to(const String &p_name) {
	name_popup->hide();
	renaming.name = to_utf8(p_name);
	renaming.changes = rename(renaming.path, renaming.offset, renaming.name);
	const std::vector<compiler::RenameEdit> &edits = renaming.changes.edits;
	if (!renaming.changes.refusals.empty()) {
		references->set_refusals(renaming.changes.refusals);
	} else if (std::any_of(edits.begin(), edits.end(), [](const compiler::RenameEdit &p_edit) { return p_edit.occurrence.candidate || !is_script(p_edit.occurrence.file); })) {
		references->set_changes(edits, callable_mp(this, &PetEditorPlugin::apply_previewed));
	} else {
		references->set_found({});
		apply(renaming.path, renaming.offset, renaming.name, edits, true, false);
		return;
	}
	make_bottom_panel_item_visible(references);
}

// Whether the open scenes that show nodes of a scene in `p_files` are saved.
// Otherwise it saves one with unsaved changes, as Save Scene saves the current
// one: shown while it is saved (editor/editor_interface.cpp:699-704, 775-777,
// editor/editor_node.h:981-987, editor/editor_node.cpp:2466-2560, 4961-4974;
// ed1daf0). Once all are, it shows again the tab shown before, by its place,
// which a scene without a path has too. A scene just opened is the current
// tab from the next frame (editor/editor_node.cpp:884-887, 4983-4987;
// ed1daf0).
//
// It changes tabs at most once, and the caller runs again deferred. Showing a
// scene defers calls that hold items of the scene dock, which the next change
// of tabs or reload of the scene frees (editor/editor_node.cpp:4651-4675,
// editor/scene/scene_tree_editor.cpp:995-1004, 1131-1140; ed1daf0). A scene
// that is still unsaved once saved, as a read-only file, stops it, as the
// rename would lose its changes when it reloads the scene; the user is told.
// So does a scene to save whose file changed on disk, which saving would write
// over (architecture §6.5).
PetEditorPlugin::Saving PetEditorPlugin::scenes_saved(const std::set<std::string> &p_files) {
	EditorInterface *editor = EditorInterface::get_singleton();
	TabBar *tabs = scene_tabs();
	const std::vector<std::string> showing = open_scenes_showing(p_files);
	PackedStringArray conflicting;
	for (const std::string &scene : showing) {
		if (lsp && is_unsaved_scene(scene) && lsp->conflicts_with_disk(scene)) {
			conflicting.push_back(to_godot(scene));
		}
	}
	std::string failed;
	for (const std::string &scene : conflicting.is_empty() ? showing : std::vector<std::string>()) {
		if (!is_unsaved_scene(scene)) {
			continue;
		}
		if (!saving.insert(scene).second) {
			failed = scene;
			break;
		}
		const Node *current = editor->get_edited_scene_root();
		const bool shown = current && to_utf8(current->get_scene_file_path()) == scene;
		if (!shown) {
			if (shown_before < 0 && tabs) {
				shown_before = tabs->get_current_tab();
			}
			editor->open_scene_from_path(to_godot(scene));
		}
		editor->save_scene_as(to_godot(scene), false);
		if (!shown) {
			return Saving::Waiting;
		}
		if (is_unsaved_scene(scene)) {
			failed = scene;
			break;
		}
	}
	saving.clear();
	const bool refused = !failed.empty() || !conflicting.is_empty();
	const int32_t shown = shown_before;
	shown_before = -1;
	if (tabs && shown >= 0 && tabs->get_current_tab() != shown) {
		tabs->set_current_tab(shown);
		if (!refused) {
			return Saving::Waiting;
		}
	}
	if (refused) {
		toast(conflicting.is_empty() ? vformat("%s cannot be saved, so nothing was renamed.", to_godot(failed)) : vformat("Godot has unsaved changes to %s, which changed on disk, so nothing was renamed.", String(", ").join(conflicting)), EditorToaster::SEVERITY_WARNING);
		return Saving::Failed;
	}
	return Saving::Done;
}

// The scenes it changes are saved, and the changes found again. The preview
// is shown again if they differ.
void PetEditorPlugin::apply_previewed() {
	const std::vector<bool> ticked = references->checked();
	std::set<std::string> files;
	for (size_t i = 0; i < ticked.size(); i++) {
		if (ticked[i]) {
			files.insert(renaming.changes.edits[i].occurrence.file);
		}
	}
	const Saving saved = scenes_saved(files);
	if (saved == Saving::Waiting) {
		callable_mp(this, &PetEditorPlugin::apply_previewed).call_deferred();
	}
	if (saved != Saving::Done) {
		return;
	}
	const compiler::Rename again = rename(renaming.path, renaming.offset, renaming.name);
	const auto same = [](const compiler::RenameEdit &p_a, const compiler::RenameEdit &p_b) {
		const compiler::Occurrence &a = p_a.occurrence;
		const compiler::Occurrence &b = p_b.occurrence;
		return a.file == b.file && a.line == b.line && a.column == b.column && a.candidate == b.candidate && p_a.edit.old_text == p_b.edit.old_text;
	};
	const std::vector<compiler::RenameEdit> &previewed = renaming.changes.edits;
	if (!again.refusals.empty() || !std::equal(again.edits.begin(), again.edits.end(), previewed.begin(), previewed.end(), same)) {
		renaming.changes = again;
		if (again.refusals.empty()) {
			references->set_changes(again.edits, callable_mp(this, &PetEditorPlugin::apply_previewed));
		} else {
			references->set_refusals(again.refusals);
		}
		toast("The project changed.");
		return;
	}
	std::vector<compiler::RenameEdit> chosen;
	for (size_t i = 0; i < ticked.size(); i++) {
		if (ticked[i]) {
			chosen.push_back(again.edits[i]);
		}
	}
	references->set_found({});
	apply(renaming.path, renaming.offset, renaming.name, chosen, true, false);
}

// A rename in one open script is a step of its undo; one that changes more
// files, a step of the editor's history (architecture §2.3).
void PetEditorPlugin::apply(const std::string &p_path, uint32_t p_offset, const std::string &p_name, const std::vector<compiler::RenameEdit> &p_edits, bool p_record, bool p_undo) {
	std::map<std::string, std::vector<const compiler::RenameEdit *>> by_file;
	for (const compiler::RenameEdit &edit : p_edits) {
		by_file[edit.occurrence.file].push_back(&edit);
	}
	CodeEdit *one_buffer = p_record && by_file.size() == 1 ? open_code_edit(by_file.begin()->first) : nullptr;
	// Where the name renamed is after the edits, and where the candidates
	// changed are before and after them.
	uint32_t offset = p_offset;
	std::string old_name;
	Array candidates_before;
	Array candidates_after;
	FileEdits changes;
	for (auto &[file, edits] : by_file) {
		std::sort(edits.begin(), edits.end(), [](const compiler::RenameEdit *p_a, const compiler::RenameEdit *p_b) { return p_a->edit.begin < p_b->edit.begin; });
		int64_t delta = 0;
		for (const compiler::RenameEdit *each : edits) {
			const compiler::CheckedEdit &edit = each->edit;
			if (file == p_path && edit.begin <= p_offset && p_offset <= edit.end) {
				offset = uint32_t(edit.begin + delta);
				old_name = edit.old_text;
			}
			if (each->occurrence.candidate) {
				candidates_before.push_back(Array::make(to_godot(file), edit.begin));
				candidates_after.push_back(Array::make(to_godot(file), edit.begin + delta));
			}
			delta += int64_t(edit.new_text.size()) - (edit.end - edit.begin);
			changes[file].push_back(edit);
		}
	}
	const std::string source = one_buffer ? workspace().script_source(by_file.begin()->first).value_or(std::string()) : std::string();
	if (one_buffer && !tab_has(one_buffer, by_file.begin()->first, source)) {
		return;
	}
	if (one_buffer) {
		std::vector<compiler::Edit> plain;
		for (const compiler::CheckedEdit &edit : changes.begin()->second) {
			plain.push_back({ edit.begin, edit.end, edit.new_text });
		}
		apply_edits(one_buffer, source, plain);
	} else if (!change_files(changes, p_undo)) {
		return;
	}
	// What finding references lists of the new name should be what changed: in
	// a script, the places, of which a pattern written out has two edits.
	const std::optional<std::vector<compiler::Occurrence>> found = find_references(p_path, offset);
	using Places = std::map<std::string, std::set<std::pair<int, int>>>;
	const auto add = [](Places &r_places, const compiler::Occurrence &p_each) {
		if (!p_each.candidate) {
			r_places[p_each.file].insert(is_script(p_each.file) ? std::make_pair(p_each.line, p_each.column) : std::make_pair(0, 0));
		}
	};
	Places changed;
	Places listed;
	for (const compiler::RenameEdit &edit : p_edits) {
		add(changed, edit.occurrence);
	}
	for (const compiler::Occurrence &each : found.value_or(std::vector<compiler::Occurrence>())) {
		add(listed, each);
	}
	PackedStringArray differing;
	for (const auto &[file, places] : changed) {
		if (listed[file].size() != places.size()) {
			differing.push_back(to_godot(file));
		}
	}
	for (const auto &[file, places] : listed) {
		if (!changed.count(file)) {
			differing.push_back(to_godot(file));
		}
	}
	if (!differing.is_empty()) {
		UtilityFunctions::push_warning(vformat("After renaming \"%s\" to \"%s\", finding references lists other places than it changed in %s.", to_godot(old_name), to_godot(p_name), String(", ").join(differing)));
	}
	const auto declared = std::find_if(p_edits.begin(), p_edits.end(), [](const compiler::RenameEdit &p_edit) { return p_edit.occurrence.declaration; });
	if (!p_record || one_buffer || !found || found->empty() || declared == p_edits.end()) {
		return;
	}
	const compiler::DeclId &before = declared->occurrence.reference.target;
	const compiler::DeclId &renamed = found->front().reference.target;
	std::set<std::string> files;
	for (const auto &[file, edits] : changes) {
		files.insert(file);
	}
	record(vformat("Rename \"%s\" to \"%s\"", to_godot(old_name), to_godot(p_name)), files, Array::make("_rename_declaration", int(before.kind), to_godot(before.scope), to_godot(before.name), to_godot(p_name), candidates_before, false), Array::make("_rename_declaration", int(renamed.kind), to_godot(renamed.scope), to_godot(renamed.name), to_godot(old_name), candidates_after, true));
}

// Changes all files or none, and fails when a text is not what an edit
// replaces or a file cannot be written (architecture §2.3). Edits all in one
// open script are one step to undo in its tab, unsaved. Otherwise each file is
// written as bytes, a script's with its byte order mark (write_all()). A tab
// then takes its edits as one step to undo, and is saved, as its own unsaved
// changes are written with them. Nothing is written over a change the editor
// has not taken in, which Held tells. A tab's file is looked at first:
// changed on disk, it is why an LSP client's text differs from the tab
// (architecture §6.5). An undo whose text is what the tab had before its last
// step, a refactoring's, takes that step off instead (architecture §2.3). The
// open scenes that show the nodes of a file written are reloaded, which clears
// their undo histories (editor/editor_node.cpp:7124-7160; ed1daf0).
bool PetEditorPlugin::change_files(const FileEdits &p_changes, bool p_undo) {
	const bool one_tab = p_changes.size() == 1 && open_code_edit(p_changes.begin()->first);
	std::set<std::string> changed;
	for (const auto &[file, edits] : p_changes) {
		changed.insert(file);
	}
	Held held = one_tab ? Held() : Held::of(changed);
	held.drop(Held::Unsaved);
	if (!held.empty()) {
		toast(held.reason() + " Nothing was changed.", EditorToaster::SEVERITY_WARNING);
		return false;
	}
	std::map<std::string, std::pair<std::string, std::string>> texts; // Before and after, by file.
	for (const auto &[file, edits] : p_changes) {
		const std::optional<std::string> text = is_script(file) ? workspace().script_source(file) : std::optional<std::string>(file_bytes(to_godot(file)));
		if (!text || !std::all_of(edits.begin(), edits.end(), [&](const compiler::CheckedEdit &p_edit) { return intact(*text, p_edit); })) {
			toast(vformat("%s changed while it was being edited, so nothing was changed.", to_godot(file)), EditorToaster::SEVERITY_WARNING);
			return false;
		}
		if (const CodeEdit *code_edit = is_script(file) ? open_code_edit(file) : nullptr; code_edit && !tab_has(code_edit, file, *text)) {
			return false;
		}
		std::string result;
		uint32_t done = 0;
		for (const compiler::CheckedEdit &edit : edits) {
			result += text->substr(done, edit.begin - done) + edit.new_text;
			done = edit.end;
		}
		texts[file] = { *text, result + text->substr(done) };
	}
	if (!one_tab) {
		std::map<std::string, std::optional<std::string>> bytes;
		for (const auto &[file, text] : texts) {
			bytes[file] = is_script(file) ? byte_order_mark(file_bytes(to_godot(file))) + text.second : text.second;
		}
		if (const std::string failed = write_all(bytes); !failed.empty()) {
			toast(vformat("Cannot write %s, so nothing was changed.", to_godot(failed)), EditorToaster::SEVERITY_WARNING);
			return false;
		}
	}
	std::set<std::string> files;
	for (const auto &[file, text] : texts) {
		files.insert(file);
		CodeEdit *code_edit = open_code_edit(file);
		if (!code_edit) {
			continue;
		}
		const std::pair<uint64_t, uint32_t> last = { code_edit->get_instance_id(), code_edit->get_version() };
		bool undone = p_undo && refactored.count(last);
		if (undone) {
			code_edit->undo();
			undone = to_utf8(code_edit->get_text()) == without_cr(text.second);
			if (!undone) {
				code_edit->redo();
			}
		}
		if (undone) {
			refactored.erase(last);
			taken_off.insert(last);
		} else {
			std::vector<compiler::Edit> plain;
			for (const compiler::CheckedEdit &edit : p_changes.at(file)) {
				plain.push_back({ edit.begin, edit.end, edit.new_text });
			}
			apply_edits(code_edit, text.first, plain);
			refactored.insert({ last.first, code_edit->get_version() });
		}
		// Written, or back to its file, as an undo leaves it, the tab is saved.
		if (!one_tab || without_cr(SceneIndex::get().script_source(file).value_or(std::string())) == text.second) {
			code_edit->tag_saved_version();
		}
	}
	if (one_tab) {
		reload_written(files);
	} else {
		files_written(files);
	}
	return true;
}

// After the editor wrote the files: it has seen them, an LSP client that has
// one open takes its text, and they are read back (reload_written()). Then,
// as the editor saves a script, for a tab: the script editor's record of when
// the file changed follows, so that it does not take the file as changed
// elsewhere, and a running game reloads the script (editor/editor_node.cpp:1774, 8664-8665,
// editor/script/script_editor_plugin.cpp:723-742, 803-850,
// editor/script/script_editor_base.cpp:91-93, 570-573; ed1daf0).
void PetEditorPlugin::files_written(const std::set<std::string> &p_files) {
	for (const std::string &file : p_files) {
		file_seen(file);
		client_reloads(file);
	}
	reload_written(p_files);
	Node *editor_node = EditorInterface::get_singleton()->get_base_control()->get_parent();
	for (const std::string &file : p_files) {
		const String path = to_godot(file);
		const Ref<Resource> shown = open_code_edit(file) ? ResourceLoader::get_singleton()->get_cached_ref(path) : Ref<Resource>();
		if (shown.is_valid()) {
			if (compiler::is_script_path(file)) {
				write_bytecode(path);
			}
			editor_node->emit_signal("resource_saved", shown);
		} else if (compiler::is_script_path(file)) {
			script_written(path, sessions);
		}
	}
}

// After files of the project changed, written by a refactoring or by the LSP
// client, or a script's tab (architecture §2.3, §2.4). The loaded scripts
// among them take the workspace's texts, as a tab's script takes its checked
// text, and reload in one pass, so that each compiles once against the
// others' new texts. One written that the client has open takes its file's
// text: the client wrote it, or reloads it as it has no unsaved changes
// (change_files()). A tab without unsaved changes takes a script's text
// written, as the script editor reloads it
// (editor/script/script_editor_plugin.cpp:2535-2588; ed1daf0). Then
// what else is loaded is read again, as when the editor sees a file change,
// with the scripts' new members (editor/editor_node.cpp:1291-1323,
// core/io/resource.cpp:263-276; ed1daf0). A cached scene too: the editor builds
// an open scene again from the cached scenes it shows, as it shows its tab
// after one changed, and clears its history then (editor/editor_data.cpp:722-771,
// editor/editor_node.cpp:4747-4753, scene/resources/packed_scene.cpp:848;
// ed1daf0). Reading a cached scene changes the state that the open scenes
// showing it compare with, so they are reloaded; not one with unsaved changes,
// which it would lose.
void PetEditorPlugin::reload_written(const std::set<std::string> &p_files) {
	if (p_files.empty()) {
		return;
	}
	disk_changed();
	EditorInterface *editor = EditorInterface::get_singleton();
	std::set<std::string> others;
	std::vector<Ref<Script>> scripts;
	// A changed script_class takes effect (editor/file_system/editor_file_system.cpp:2400-2492; ed1daf0).
	const auto written = [&](const String &p_path) { editor->get_resource_filesystem()->update_file(p_path); };
	const auto taken = [&](const std::string &p_file) {
		const bool from_file = p_files.count(p_file) && client_scripts().count(p_file);
		return (from_file ? SceneIndex::get().script_source(p_file) : workspace().script_source(p_file)).value_or(std::string());
	};
	for (const std::string &file : p_files) {
		const String path = to_godot(file);
		if (!is_script(file)) {
			if (!file_empty(file)) {
				written(path);
			}
			others.insert(file);
			continue;
		}
		const String text = FileAccess::get_file_as_string(path).replace("\r", "");
		CodeEdit *code_edit = open_code_edit(file);
		if (code_edit && code_edit->get_version() == code_edit->get_saved_version() && code_edit->get_text() != text) {
			const int line = code_edit->get_caret_line();
			const int column = code_edit->get_caret_column();
			code_edit->set_text(text);
			code_edit->set_caret_line(line);
			code_edit->set_caret_column(column);
			code_edit->tag_saved_version();
		}
		if (without_cr(taken(file)) == to_utf8(text)) {
			written(path);
		}
	}
	// Before what else is loaded is read again, also the scripts that the LSP
	// client has unsaved texts of, which carry a rename the client has not
	// saved all of yet.
	std::set<std::string> taking = others.empty() ? std::set<std::string>() : client_scripts();
	for (const std::string &file : p_files) {
		if (is_script(file)) {
			taking.insert(file);
		}
	}
	for (const std::string &file : taking) {
		const String current = to_godot(taken(file));
		const Ref<Script> loaded = ResourceLoader::get_singleton()->get_cached_ref(to_godot(file));
		if (loaded.is_valid() && loaded->get_source_code() != current) {
			loaded->set_source_code(current);
			scripts.push_back(loaded);
		}
	}
	std::set<std::string> reloaded;
	{
		const compiler::Analysis::Pass pass(analysis());
		for (const Ref<Script> &script : scripts) {
			script->reload(true);
			reloaded.insert(to_utf8(script->get_path()));
		}
	}
	// Compiling one of them may have had the scene index read a resource with
	// the placeholder of another still to reload, which gave it the members
	// of the old text.
	if (!reloaded.empty()) {
		SceneIndex::get().files_changed(reloaded);
		disk_changed();
	}
	// One that cannot be read, as one a writer has emptied to write it again,
	// keeps what is loaded, and the open scenes showing it stay: reloading closes
	// a scene before it loads it, and leaves it closed when that fails
	// (editor/editor_node.cpp:7143-7146; ed1daf0). Once written, it is read again.
	std::set<std::string> read;
	std::set<std::string> unread;
	for (const std::string &file : others) {
		const String path = to_godot(file);
		const Ref<Resource> loaded = ResourceLoader::get_singleton()->get_cached_ref(path);
		bool readable = !file_empty(file);
		if (readable && loaded.is_valid()) {
			const Ref<Resource> again = ResourceLoader::get_singleton()->load(path, loaded->get_class(), ResourceLoader::CACHE_MODE_IGNORE);
			readable = again.is_valid();
			if (readable) {
				loaded->copy_from_resource(again);
			}
		}
		(readable ? read : unread).insert(file);
	}
	// Reloading a scene clears its history, which is no step of its own.
	const PackedStringArray unsaved = editor->get_unsaved_scenes();
	const uint64_t steps = elsewhere;
	const std::vector<std::string> kept = unread.empty() ? std::vector<std::string>() : open_scenes_showing(unread);
	for (const std::string &scene : read.empty() ? std::vector<std::string>() : open_scenes_showing(read)) {
		if (!unsaved.has(to_godot(scene)) && std::find(kept.begin(), kept.end(), scene) == kept.end()) {
			editor->reload_scene_from_path(to_godot(scene));
		}
	}
	elsewhere = steps;
	changed();
}

// Adds a step to the editor's global history, already done, that calls the
// method of this plugin that `p_do` or `p_undo` names, with the arguments
// after the name. Ctrl+Z in one of `p_files` may undo it (_input). The call
// waits until the editor's undo or redo returns: a scene reloaded clears its
// history, and the editor reads its histories then, which are half undone
// during it (core/object/undo_redo.cpp:432-434,
// editor/editor_undo_redo_manager.cpp:321-325, 402-407; ed1daf0). It leaves
// the history saved, as the files it changes say what is unsaved.
void PetEditorPlugin::record(const String &p_action, const std::set<std::string> &p_files, const Array &p_do, const Array &p_undo) {
	EditorUndoRedoManager *undo_redo = get_undo_redo();
	undo_redo->create_action(p_action, UndoRedo::MERGE_DISABLE, nullptr, false, false);
	undo_redo->add_do_method(this, "callv", "call_deferred", p_do);
	undo_redo->add_undo_method(this, "callv", "call_deferred", p_undo);
	undo_redo->commit_action(false);
	UndoRedo *global = undo_redo->get_history_undo_redo(EditorUndoRedoManager::GLOBAL_HISTORY);
	recorded[global->get_current_action()] = { p_action, p_files, elsewhere, p_do, p_undo };
	global_version = global->get_version();
}

namespace {

// The id of a declaration renamed to `p_new`: its name's last part changes,
// or a class's own in its scope.
compiler::DeclId renamed_to(compiler::DeclId p_id, const std::string &p_new) {
	if (!p_id.name.empty()) {
		const size_t dot = p_id.name.rfind('.');
		p_id.name = (dot == std::string::npos ? std::string() : p_id.name.substr(0, dot + 1)) + p_new;
	} else if (const size_t inner = p_id.scope.rfind("::"); inner != std::string::npos) {
		p_id.scope = p_id.scope.substr(0, inner + 2) + p_new;
	}
	return p_id;
}

} // namespace

// Renames the declaration `p_scope` and `p_name` identify, as undoing and
// redoing a rename do, from the text as it is now, with the candidates
// changed at the offsets given. A scene it changes is saved first, and it runs
// again deferred (scenes_saved()). One named so already is left, as a menu's
// undo that was refused leaves it once the history moves on (architecture
// §2.3). Without `p_apply`, it only finds why it would be refused, or "".
String PetEditorPlugin::rename_declaration(int p_kind, const String &p_scope, const String &p_name, const String &p_new, const Array &p_candidates, bool p_undo, bool p_apply) {
	const compiler::DeclId id{ compiler::DeclId::Kind(p_kind), to_utf8(p_scope), to_utf8(p_name) };
	const std::string file = compiler::script_file(id.scope);
	const std::optional<uint32_t> offset = declaration_offset(file, id);
	const compiler::Rename found = offset ? rename(file, *offset, to_utf8(p_new)) : compiler::Rename();
	std::set<std::string> files;
	std::vector<compiler::RenameEdit> chosen;
	int64_t candidates = 0;
	for (const compiler::RenameEdit &edit : found.edits) {
		files.insert(edit.occurrence.file);
		const bool candidate = edit.occurrence.candidate && p_candidates.has(Array::make(to_godot(edit.occurrence.file), edit.edit.begin));
		if (!edit.occurrence.candidate || candidate) {
			chosen.push_back(edit);
			candidates += candidate;
		}
	}
	std::string why;
	if (!offset) {
		if (declaration_offset(file, renamed_to(id, to_utf8(p_new)))) {
			return String();
		}
		why = "its declaration is gone.";
	} else if (!found.refusals.empty()) {
		why = found.refusals[0].message;
	} else if (candidates != p_candidates.size()) {
		why = "a name it changed by text has moved.";
	} else if (!p_apply) {
		return String();
	} else if (const Saving saved = scenes_saved(files); saved == Saving::Waiting) {
		callable_mp(this, &PetEditorPlugin::rename_declaration).call_deferred(p_kind, p_scope, p_name, p_new, p_candidates, p_undo, p_apply);
		return String();
	} else if (saved == Saving::Failed) {
		return "An open scene cannot be saved."; // As scenes_saved() told.
	} else {
		apply(file, *offset, to_utf8(p_new), chosen, false, p_undo);
		return String();
	}
	const String refusal = vformat("Cannot rename it to \"%s\": %s", p_new, to_godot(why));
	if (p_apply) {
		toast(refusal, EditorToaster::SEVERITY_WARNING);
	}
	return refusal;
}

// The tab of a script of this language gets a gutter for the bulb of quick
// fixes, right of the fold gutter (architecture §2.4). A tab opened is in
// step with its file, which the workspace notes before the tab can change: the
// signal comes as it opens (editor/script/script_editor_plugin.cpp:2305,
// 399-410; ed1daf0).
void PetEditorPlugin::script_changed() {
	workspace();
	ScriptEditor *script_editor = EditorInterface::get_singleton()->get_script_editor();
	ScriptEditorBase *current = script_editor->get_current_editor();
	CodeEdit *code_edit = current ? Object::cast_to<CodeEdit>(current->get_base_editor()) : nullptr;
	const Ref<Script> script = script_editor->get_current_script();
	fixing = Fixing();
	if (!code_edit || !Object::cast_to<PetScript>(script.ptr())) {
		return;
	}
	if (fix_gutter(code_edit) < 0) {
		const int32_t gutter = code_edit->get_gutter_count();
		code_edit->add_gutter();
		code_edit->set_gutter_name(gutter, "petrichor_fixes");
		code_edit->set_gutter_type(gutter, TextEdit::GUTTER_TYPE_CUSTOM);
		code_edit->set_gutter_custom_draw(gutter, callable_mp(this, &PetEditorPlugin::draw_bulb).bind(code_edit));
		size_gutter(code_edit->get_instance_id());
		// Once the line height follows the theme, which it does after the signal
		// (scene/gui/control.cpp:4633-4637; ed1daf0). By its id, as the tab may
		// close first.
		code_edit->connect("theme_changed", callable_mp(this, &PetEditorPlugin::size_gutter).bind(code_edit->get_instance_id()), CONNECT_DEFERRED);
		code_edit->connect("caret_changed", callable_mp(this, &PetEditorPlugin::show_bulb));
		code_edit->connect("text_changed", callable_mp(this, &PetEditorPlugin::show_bulb));
		code_edit->connect("gutter_clicked", callable_mp(this, &PetEditorPlugin::bulb_clicked));
		// After each check of its script (editor/script/script_editor_base.cpp:522-525; ed1daf0).
		current->connect("edited_script_changed", callable_mp(this, &PetEditorPlugin::validated).bind(code_edit));
	}
	fixing.code_edit = code_edit->get_instance_id();
	fixing.path = to_utf8(script->get_path());
	refresh_fixes();
}

void PetEditorPlugin::validated(Object *p_code_edit) {
	CodeEdit *code_edit = Object::cast_to<CodeEdit>(p_code_edit);
	tab_checks[code_edit->get_instance_id()] = workspace().revision();
	if (code_edit == fixing_code_edit()) {
		refresh_fixes();
	}
	changed();
}

CodeEdit *PetEditorPlugin::fixing_code_edit() const {
	return fixing.code_edit ? Object::cast_to<CodeEdit>(ObjectDB::get_instance(fixing.code_edit)) : nullptr;
}

// From the tab's text, whose check the analysis keeps from the script editor's.
// Its stamp holds the files that its fixes edit.
void PetEditorPlugin::refresh_fixes() {
	CodeEdit *code_edit = fixing_code_edit();
	if (!code_edit) {
		return;
	}
	const std::string source = workspace().script_source(fixing.path).value_or(std::string());
	fixing.errors = fixable(analysis().compile(fixing.path, source)->errors, fixing.path);
	fixing.revision = workspace().revision();
	fixing.stamp = { analysis().computed(fixing.path), { fixing.path } };
	for (const compiler::Diagnostic &error : fixing.errors) {
		for (const compiler::Fix &fix : error.fixes) {
			for (const compiler::Fix::Edit &edit : fix.edits) {
				fixing.stamp.files.insert(edit.file);
			}
		}
	}
	fixing.bulb = -1;
	show_bulb();
	code_edit->queue_redraw();
}

// On the caret's line, while one of its errors has a fix and the workspace is
// the one checked.
void PetEditorPlugin::show_bulb() {
	CodeEdit *code_edit = fixing_code_edit();
	if (!code_edit) {
		return;
	}
	const int caret = code_edit->get_caret_line();
	const bool shown = workspace().revision() == fixing.revision && std::any_of(fixing.errors.begin(), fixing.errors.end(), [&](const compiler::Diagnostic &p_error) { return p_error.line == caret + 1; });
	const int line = shown ? caret : -1;
	if (line != fixing.bulb) {
		fixing.bulb = line;
		code_edit->queue_redraw();
	}
}

// A globe over a base, in the color of warnings, drawn on the CodeEdit's own
// canvas item: the one its gutters are drawn on is not bound
// (scene/gui/text_edit.cpp:948-950, 1490-1496, scene/gui/text_edit.h:796).
// The cell is clickable, which shows a hand, as the fold gutter does
// (scene/gui/code_edit.cpp:1779-1784; ed1daf0).
void PetEditorPlugin::draw_bulb(int p_line, int p_gutter, const Rect2 &p_region, Object *p_code_edit) {
	CodeEdit *code_edit = Object::cast_to<CodeEdit>(p_code_edit);
	const bool shown = p_line == fixing.bulb && code_edit->get_instance_id() == fixing.code_edit;
	code_edit->set_line_gutter_clickable(p_line, p_gutter, shown);
	if (!shown) {
		return;
	}
	const Color color = code_edit->get_theme_color("warning_color", "Editor");
	const real_t unit = std::min(p_region.size.x, p_region.size.y) / 16;
	const Vector2 corner = p_region.get_center() - Vector2(8, 8) * unit;
	code_edit->draw_circle(corner + Vector2(8, 6.5f) * unit, 4.5f * unit, color, true, -1, true);
	code_edit->draw_rect(Rect2(corner + Vector2(6, 9) * unit, Vector2(4, 3) * unit), color);
	code_edit->draw_rect(Rect2(corner + Vector2(6.5f, 12.5f) * unit, Vector2(3, 1.5f) * unit), color);
}

void PetEditorPlugin::size_gutter(uint64_t p_code_edit) {
	if (CodeEdit *code_edit = Object::cast_to<CodeEdit>(ObjectDB::get_instance(p_code_edit))) {
		code_edit->set_gutter_width(fix_gutter(code_edit), code_edit->get_line_height());
	}
}

// A click on any gutter is reported, clickable or not
// (scene/gui/text_edit.cpp:2311-2319, 9060-9081; ed1daf0).
void PetEditorPlugin::bulb_clicked(int p_line, int p_gutter) {
	CodeEdit *code_edit = fixing_code_edit();
	if (!code_edit || p_line != fixing.bulb || p_gutter != fix_gutter(code_edit)) {
		return;
	}
	const Rect2i line = code_edit->get_rect_at_line_column(p_line, 0);
	list_fixes_at(code_edit, code_edit->get_screen_position() + Vector2(line.position.x, line.get_end().y));
}

void PetEditorPlugin::list_fixes(Object *p_code_edit) {
	if (CodeEdit *code_edit = Object::cast_to<CodeEdit>(p_code_edit)) {
		list_fixes_at(code_edit, code_edit->get_screen_position() + code_edit->get_caret_draw_pos(0));
	}
}

// The fixes of the errors on the caret's line (quick_fixes()).
void PetEditorPlugin::list_fixes_at(CodeEdit *p_code_edit, const Vector2 &p_position) {
	const std::optional<std::pair<String, uint32_t>> at = at_caret(p_code_edit);
	if (!at || p_code_edit != fixing_code_edit()) {
		return;
	}
	if (workspace().revision() != fixing.revision) {
		refresh_fixes();
	}
	const uint32_t caret = at->second;
	std::vector<const compiler::Diagnostic *> errors;
	for (const compiler::Diagnostic &error : fixing.errors) {
		if (error.line == p_code_edit->get_caret_line() + 1) {
			errors.push_back(&error);
		}
	}
	const QuickFixes fixes = quick_fixes(errors, caret, caret, fixing.errors, fixing.path);
	listed.clear();
	fix_list->clear();
	for (const auto &[error, fix] : fixes.fixes) {
		fix_list->add_item(to_godot(fix->title), int32_t(listed.size()));
		listed.emplace_back(*error, *fix);
	}
	if (!fixes.all.empty()) {
		fix_list->add_separator();
	}
	for (const compiler::FixKind kind : fixes.all) {
		fix_list->add_item(vformat("Fix all '%s' in file", compiler::FIX_KINDS[size_t(kind)].name), int32_t(listed.size() + size_t(kind)));
	}
	if (fix_list->get_item_count() == 0) {
		toast("No quick fixes on this line.");
		return;
	}
	fix_list->set_position(Vector2i(p_position));
	fix_list->reset_size();
	fix_list->popup();
	fix_list->set_focused_item(0);
}

// The fix chosen is found again (fix_again()). A change of the tab's script
// alone is one step to undo there. One of others is one step of the editor's
// history, undone by its inverse edits, checked as it is (architecture §2.4).
void PetEditorPlugin::apply_listed(int p_id) {
	CodeEdit *code_edit = fixing_code_edit();
	if (!code_edit) {
		return;
	}
	const std::string source = workspace().script_source(fixing.path).value_or(std::string());
	const bool all = p_id >= int(listed.size());
	FileEdits changes = all ? fix_again(fixing.path, fixing.stamp, nullptr, std::string(), compiler::FixKind(p_id - int(listed.size()))) : fix_again(fixing.path, fixing.stamp, &listed[p_id].first, listed[p_id].second.title, listed[p_id].second.kind);
	if (changes.empty()) {
		toast("The project changed.");
		refresh_fixes();
		return;
	}
	std::set<std::string> files;
	for (const auto &[file, edits] : changes) {
		files.insert(file);
	}
	if (files == std::set<std::string>{ fixing.path } && !tab_has(code_edit, fixing.path, source)) {
		return;
	}
	if (files == std::set<std::string>{ fixing.path }) {
		std::vector<compiler::Edit> plain;
		for (const compiler::CheckedEdit &edit : changes[fixing.path]) {
			plain.push_back({ edit.begin, edit.end, edit.new_text });
		}
		apply_edits(code_edit, source, plain);
		show_bulb();
	} else if (change_scripts(changes, false, true).is_empty()) {
		record(fix_list->get_item_text(fix_list->get_item_index(p_id)), files, Array::make("_change_scripts", to_array(changes), false), Array::make("_change_scripts", to_array(inverse(changes)), true));
	}
}

// Against the texts the workspace has, as a rename edits them (architecture
// §2.3). Then the tab checks its script at once, as when its text stops
// changing (editor/gui/code_editor.cpp:1696-1699,
// editor/script/script_editor_base.cpp:653; ed1daf0). Edits made already are
// left, as a menu's undo that was refused leaves them once the history moves
// on. Without `p_apply`, it only finds why it would be refused, or "".
String PetEditorPlugin::change_scripts(const FileEdits &p_changes, bool p_undo, bool p_apply) {
	const auto all_intact = [](const FileEdits &p_edits) {
		return std::all_of(p_edits.begin(), p_edits.end(), [](const auto &p_file) {
			const std::optional<std::string> text = workspace().script_source(p_file.first);
			return text && std::all_of(p_file.second.begin(), p_file.second.end(), [&](const compiler::CheckedEdit &p_edit) { return intact(*text, p_edit); });
		});
	};
	if (!all_intact(p_changes)) {
		if (all_intact(inverse(p_changes))) {
			return String();
		}
		if (p_apply) {
			toast("The project changed.");
			refresh_fixes();
		}
		return "The project changed.";
	}
	if (!p_apply || !change_files(p_changes, p_undo)) {
		return String();
	}
	if (CodeEdit *code_edit = fixing_code_edit()) {
		code_edit->get_parent()->emit_signal("validate_script");
	}
	return String();
}

String PetEditorPlugin::change_scripts_again(const Array &p_edits, bool p_undo, bool p_apply) {
	return change_scripts(from_array(p_edits), p_undo, p_apply);
}

void PetEditorPlugin::_exit_tree() {
	lsp.reset();
	remove_debugger_plugin(sessions);
	sessions.unref();
	memdelete(fix_list);
	fix_list = nullptr;
	memdelete(name_popup);
	name_popup = nullptr;
	remove_control_from_bottom_panel(references);
	memdelete(references);
	references = nullptr;
	EditorInterface::get_singleton()->get_script_editor()->unregister_syntax_highlighter(highlighter);
	highlighter.unref();
	remove_context_menu_plugin(code_menu);
	code_menu.unref();
	remove_export_plugin(export_plugin);
	export_plugin.unref();
}

bool PetEditorPlugin::_build() {
	const ProjectReport report = build_project(true, false);
	for (const compiler::Diagnostic &error : report.diagnostics) {
		gdextension_interface::print_script_error(error.message.c_str(), "", error.file.c_str(), error.line, true);
	}
	// Compile errors keep the project from running (spec §1.4).
	return report.failed == 0;
}

// The first scan imports the project, and lasts until its imports are done
// (editor/file_system/editor_file_system.cpp:1131-1137, 1838-1840), so a
// command on a fresh copy runs once it ends. The exit code and the output go to
// the file that `--petrichor-output=<file>` names, where pet reads them apart
// from what the engine prints (architecture §6.5), else to standard output.
// The file is written whole under another name, then renamed, so that a crash
// as the editor exits (godot#111048) leaves the whole result or none.
void PetEditorPlugin::run_headless(const String &p_command) {
	if (EditorInterface::get_singleton()->get_resource_filesystem()->is_scanning()) {
		get_tree()->connect("process_frame", callable_mp(this, &PetEditorPlugin::run_headless).bind(p_command), CONNECT_ONE_SHOT);
		return;
	}
	std::vector<std::string> args;
	String output_file;
	for (const String &arg : OS::get_singleton()->get_cmdline_user_args()) {
		args.push_back(to_utf8(arg));
		if (arg.begins_with("--petrichor-output=")) {
			output_file = arg.trim_prefix("--petrichor-output=");
		}
	}
	const std::pair<std::string, int> ran = *run_command(to_utf8(p_command), args, {});
	const Ref<FileAccess> file = output_file.is_empty() ? Ref<FileAccess>() : FileAccess::open(output_file + ".tmp", FileAccess::WRITE);
	if (file.is_valid()) {
		file->store_string(String::num_int64(ran.second) + "\n" + to_godot(ran.first));
		file->close();
		DirAccess::rename_absolute(output_file + ".tmp", output_file);
	} else if (output_file.is_empty()) {
		UtilityFunctions::print(to_godot(ran.first));
	}
	get_tree()->quit(ran.second);
}

namespace {

std::string relative(const std::string &p_path) {
	return p_path.rfind("res://", 0) == 0 ? p_path.substr(6) : p_path;
}

// As compilers print one: its file from the project's root, with its line and
// column where it has them.
std::string diagnostic_line(const compiler::Diagnostic &p_diagnostic) {
	std::string line = relative(p_diagnostic.file) + ":";
	if (p_diagnostic.line > 0) {
		line += std::to_string(p_diagnostic.line) + ":";
		if (p_diagnostic.column > 0) {
			line += std::to_string(p_diagnostic.column) + ":";
		}
	}
	const char *const SEVERITIES[] = { "error", "warning", "hint" };
	line += std::string(" ") + SEVERITIES[size_t(p_diagnostic.severity)] + ": " + p_diagnostic.message;
	return p_diagnostic.warning.empty() ? line : line + " (" + p_diagnostic.warning + ")";
}

} // namespace

// pet migrate (architecture §6.6): the files the paths in `p_args` take, the
// loop of fixes and the check after it, in memory. Unless `--dry-run` is
// among `p_args`, it then renames them with their .uid files and rewrites the
// paths naming them, all or none, as a refactoring writes (§2.3): not over
// what Godot or an LSP client holds unsaved, nor a file migrated that Godot's
// script editor has open. Godot reads the project again, and project.godot's
// settings in memory take the paths it was written with
// (core/config/project_settings.cpp:359-369; ed1daf0). It exits with 1 when
// nothing was written for that or a failure.
std::pair<std::string, int> PetEditorPlugin::migrate(const std::vector<std::string> &p_args) {
	bool dry_run = false;
	std::vector<std::string> paths;
	for (const std::string &arg : p_args) {
		if (arg == "--dry-run") {
			dry_run = true;
		} else if (arg.rfind("--petrichor-", 0) != 0) {
			paths.push_back(to_utf8(("res://" + to_godot(arg).replace("\\", "/").trim_prefix("res://")).simplify_path()));
		}
	}
	// The editor makes new scripts from the templates there (editor/file_system/editor_paths.cpp:101; ed1daf0).
	const std::string templates = to_utf8(String(ProjectSettings::get_singleton()->get_setting("editor/script/templates_search_path", "res://script_templates")));
	const compiler::MigrationSet set = compiler::migration_set(ClassDBApi::get(), SceneIndex::get(), paths, templates);
	std::string output;
	for (const std::string &path : set.unknown) {
		output += relative(path) + ": error: No GDScript file is there.\n";
	}
	if (!set.unknown.empty()) {
		return { output + "petrichor migrate: nothing was written", 1 };
	}
	std::map<std::string, std::string> renamed;
	for (const std::string &file : set.files) {
		renamed[file] = compiler::migrated_path(file);
	}
	const std::map<std::string, std::string> added(set.added.begin(), set.added.end());
	for (const auto &[file, migrated] : renamed) {
		const auto why = added.find(file);
		output += relative(file) + " → " + relative(migrated) + (why == added.end() ? "" : ", added: " + why->second) + "\n";
	}
	for (const auto &[file, why] : set.left_out) {
		output += relative(file) + ": left out: " + why + "\n";
	}
	const SceneIndex taken(set.files);
	const compiler::Migration migration = compiler::migrate(ClassDBApi::get(), taken, set.files, 10);
	if (migration.quoted > 0) {
		output += std::to_string(migration.quoted) + " × Write the string in triple quotes\n";
	}
	if (migration.typed > 0) {
		output += std::to_string(migration.typed) + " × Write the type\n";
	}
	if (migration.numbers > 0) {
		output += std::to_string(migration.numbers) + " slot(s) received both int and float, which a use tells apart.\n";
	}
	for (const compiler::Diagnostic &note : migration.notes) {
		output += diagnostic_line(note) + "\n";
	}
	int fixes = 0;
	for (const auto &[kind, count] : migration.fixes) {
		output += std::to_string(count) + " × " + compiler::FIX_KINDS[size_t(kind)].name + "\n";
		fixes += count;
	}
	if (!migration.settled) {
		output += "warning: The fixes still change the files after " + std::to_string(migration.rounds) + " rounds.\n";
	}
	for (const compiler::Diagnostic &error : migration.errors) {
		output += diagnostic_line(error) + "\n";
	}

	// What is written: each file migrated under its new name, with its .uid
	// file, and each text file naming one.
	std::map<std::string, std::optional<std::string>> bytes;
	int named = 0;
	for (const auto &[file, migrated] : renamed) {
		const std::string had = file_bytes(to_godot(file));
		compiler::renamed_in_script(had, file, renamed, named);
		bytes[file] = std::nullopt;
		bytes[migrated] = byte_order_mark(had) + migration.texts.at(migrated);
		if (FileAccess::file_exists(to_godot(file + ".uid"))) {
			bytes[file + ".uid"] = std::nullopt;
			bytes[migrated + ".uid"] = file_bytes(to_godot(file + ".uid"));
		}
	}
	PackedStringArray naming = project_files(PackedStringArray({ "pet", "gd", "tscn", "tres" }));
	naming.push_back("res://project.godot");
	for (const String &path : naming) {
		const std::string file = to_utf8(path);
		if (renamed.count(file)) {
			continue;
		}
		const std::string had = file_bytes(path);
		int count = 0;
		const std::string text = is_script(file) ? compiler::renamed_in_script(had, file, renamed, count) : compiler::renamed_in_resource(had, renamed, count);
		if (count > 0) {
			bytes[file] = text;
			named += count;
		}
	}
	for (const String &path : project_files(PackedStringArray({ "scn", "res" }))) {
		for (const std::string &dependency : SceneIndex::get().dependencies(to_utf8(path))) {
			if (renamed.count(dependency)) {
				output += relative(to_utf8(path)) + ": warning: It is binary, so it keeps naming " + dependency + " by its path, beside its uid.\n";
			}
		}
	}
	std::set<std::string> written;
	for (const auto &[file, each] : bytes) {
		if (!to_godot(file).ends_with(".uid") && file != "res://project.godot") {
			written.insert(file);
		}
	}
	// Written from its file, a tab that has not seen the file takes it as it
	// reloads.
	Held held = Held::of(written);
	held.drop(Held::Unseen);
	int refused = 0;
	for (const std::string &file : written) {
		const bool open = renamed.count(file) && open_code_edit(file);
		if (held.files.count(file) || open) {
			output += relative(file) + ": error: " + (held.files.count(file) ? to_utf8(held.about(file)) : std::string("It is open in Godot's script editor. Close it there first.")) + "\n";
			refused++;
		}
	}
	output += "petrichor migrate: " + std::to_string(renamed.size()) + " script(s), " + std::to_string(set.added.size()) + " added, " + std::to_string(set.left_out.size()) + " left out, " + std::to_string(migration.typed) + " type(s), " + std::to_string(migration.withdrawn) + " withdrawn, " + std::to_string(fixes) + " fix(es), " + std::to_string(named) + " path(s) renamed, " + std::to_string(migration.errors.size()) + " error(s)";
	if (dry_run) {
		return { output + ", dry run", 0 };
	}
	const std::string failed = refused > 0 ? std::string() : write_all(bytes);
	if (!failed.empty()) {
		output = relative(failed) + ": error: It cannot be written.\n" + output;
	}
	if (refused > 0 || !failed.empty()) {
		return { output + "; nothing was written", 1 };
	}
	ProjectSettings *settings = ProjectSettings::get_singleton();
	const TypedArray<Dictionary> properties = settings->get_property_list();
	for (int64_t i = 0; i < properties.size(); i++) {
		const String name = Dictionary(properties[i]).get("name", String());
		const Variant value = settings->get_setting(name);
		const String path = value.get_type() == Variant::STRING ? String(value).trim_prefix("*") : String();
		if (const auto found = renamed.find(to_utf8(path)); found != renamed.end()) {
			settings->set_setting(name, String(value).trim_suffix(path) + to_godot(found->second));
		}
	}
	std::set<std::string> reread;
	for (const std::string &file : written) {
		if (!renamed.count(file)) {
			reread.insert(file);
		}
	}
	if (lsp) {
		// The old files first, which drops their uids, which the new ones then
		// take (editor/file_system/editor_file_system.cpp:2415-2423, 2450, 2496-2501; ed1daf0).
		for (const auto &[file, migrated] : renamed) {
			EditorInterface::get_singleton()->get_resource_filesystem()->update_file(to_godot(file));
		}
		files_written(reread);
	}
	return { output, 0 };
}

// The output and the exit code of a command of pet (architecture §6.5), which
// the headless editor and the service give alike: the files with errors and
// what they are, then a count. Each of `p_conflicts` is warned of first. A
// command with errors exits with 1 (spec §13.1), as does a format check that
// finds a script not formatted. Formatting writes as a refactoring does
// (change_files()), but not over unsaved changes. Nullopt for no command.
std::optional<std::pair<std::string, int>> PetEditorPlugin::run_command(const std::string &p_command, const std::vector<std::string> &p_args, const std::set<std::string> &p_conflicts) {
	if (p_command != "check" && p_command != "build" && p_command != "format" && p_command != "migrate") {
		return std::nullopt;
	}
	std::string output;
	for (const std::string &file : p_conflicts) {
		output += "warning: " + relative(file) + (FileAccess::file_exists(to_godot(file)) ? " changed" : " was deleted") + " on disk while Godot has unsaved changes to it.\n";
	}
	if (p_command == "migrate") {
		const std::pair<std::string, int> migrated = migrate(p_args);
		return std::make_pair(output + migrated.first, migrated.second);
	}
	if (p_command != "format") {
		const ProjectReport report = build_project(p_command == "build", true);
		for (const compiler::Diagnostic &each : report.diagnostics) {
			output += diagnostic_line(each) + "\n";
		}
		output += to_utf8(vformat("petrichor %s: %d script(s), %d scene(s), %d resource file(s), %d with errors", to_godot(p_command), report.scripts, report.scenes, report.resources, report.failed));
		return std::make_pair(output, report.failed > 0 ? 1 : 0);
	}
	const bool check = std::find(p_args.begin(), p_args.end(), "--check") != p_args.end();
	const PackedStringArray paths = project_files(PackedStringArray({ "pet" }));
	int unformatted = 0;
	int failed = 0;
	std::set<std::string> written;
	for (const String &path : paths) {
		const std::string file = to_utf8(path);
		// As bytes, which keeps the file's line breaks.
		std::string source = file_bytes(path);
		const std::string bom = byte_order_mark(source);
		source.erase(0, bom.size());
		std::string error;
		const std::optional<std::string> result = compiler::format(source, &error);
		if (result && *result == source) {
			continue;
		}
		if (result && check) {
			unformatted++;
			output += relative(file) + " is not formatted.\n";
			continue;
		}
		if (result) {
			// It is formatted from its file, so a tab that has not seen the file
			// takes it as it reloads.
			Held held = Held::of({ file });
			held.drop(Held::Unseen);
			if (!held.empty()) {
				error = to_utf8(held.about(file));
			} else if (!write_file(path, bom + *result)) {
				error = "It cannot be written.";
			} else {
				written.insert(file);
				continue;
			}
		}
		failed++;
		output += relative(file) + ": error: " + error + "\n";
	}
	if (lsp) {
		files_written(written);
	}
	output += to_utf8(vformat("petrichor format: %d script(s), %d %s, %d with errors", int(paths.size()), check ? unformatted : int(written.size()), check ? "not formatted" : "formatted", failed));
	return std::make_pair(output, failed > 0 || unformatted > 0 ? 1 : 0);
}

} // namespace petrichor
