#include "adapter/editor/pet_editor_plugin.h"

#include "adapter/editor/class_db_api.h"
#include "adapter/editor/project_files.h"
#include "adapter/editor/scene_index.h"
#include "adapter/editor/script_compiler.h"
#include "adapter/pet_language.h"
#include "adapter/pet_script.h"
#include "compiler/ast.h"
#include "compiler/builtin_api.h"
#include "compiler/compiler.h"
#include "compiler/formatter.h"
#include "compiler/lexer.h"
#include "runtime/text.h"

#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/code_edit.hpp>
#include <godot_cpp/classes/config_file.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/editor_export_platform.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/editor_toaster.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/gd_extension_manager.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/popup_menu.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/script_editor.hpp>
#include <godot_cpp/classes/script_editor_base.hpp>
#include <godot_cpp/classes/tab_bar.hpp>
#include <godot_cpp/classes/text_edit.hpp>
#include <godot_cpp/classes/tree_item.hpp>
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

namespace {

// The features that the engine's export of extensions takes as architectures
// (editor/export/gdextension_export_plugin.h:68-77; ed1daf0).
bool is_architecture(const String &p_tag) {
	for (const char *architecture : { "x86_32", "x86_64", "arm32", "arm64", "rv64", "ppc64", "wasm32", "loongarch64", "universal" }) {
		if (p_tag == architecture) {
			return true;
		}
	}
	return false;
}

// The key of [libraries] whose library the engine exports for an architecture:
// the one with the most tags, each the architecture or a feature of the export
// that is none, and the first of equals
// (core/extension/gdextension_library_loader.cpp:82-97,
// editor/export/gdextension_export_plugin.h:79-110; ed1daf0).
String library_key(const PackedStringArray &p_keys, const PackedStringArray &p_features, const String &p_architecture) {
	String best;
	int64_t most = 0;
	for (const String &key : p_keys) {
		const PackedStringArray tags = key.split(".");
		bool met = true;
		for (const String &tag : tags) {
			const String name = tag.strip_edges();
			met = met && (name == p_architecture || (p_features.has(name) && !is_architecture(name)));
		}
		if (met && tags.size() > most) {
			best = key;
			most = tags.size();
		}
	}
	return best;
}

} // namespace

String PetExportPlugin::_get_name() const {
	return "Petrichor";
}

void PetExportPlugin::_export_begin(const PackedStringArray &p_features, bool p_is_debug, const String &p_path, uint32_t p_flags) {
	const Ref<EditorExportPlatform> platform = get_export_platform();
	const auto fail = [&](const String &p_what, const String &p_remedy) {
		platform->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, "Petrichor", vformat("%s The exported game cannot load Petrichor, so its .pet scripts will not run. %s", p_what, p_remedy));
	};
	// platform/web/export/export_plugin.cpp:362-366 (ed1daf0).
	if (p_features.has("web_noextensions")) {
		fail("This Web export has \"Extensions Support\" turned off.", "Turn it on in the options of the export preset.");
	}
	// Without a library for the target, the engine exports with a warning, and
	// the game fails when it runs
	// (editor/export/gdextension_export_plugin.h:161-165; ed1daf0).
	for (const String &extension : GDExtensionManager::get_singleton()->get_loaded_extensions()) {
		// This library's own file names its entry point, wherever the project
		// keeps it (src/adapter/register_types.cpp).
		Ref<ConfigFile> config;
		config.instantiate();
		config->load(extension);
		if (String(config->get_value("configuration", "entry_symbol", String())) != "petrichor_library_init") {
			continue;
		}
		const PackedStringArray keys = config->get_section_keys("libraries");
		PackedStringArray architectures;
		for (const String &feature : p_features) {
			if (is_architecture(feature)) {
				architectures.push_back(feature);
			}
		}
		if (architectures.is_empty()) {
			architectures.push_back("unknown_arch");
		}
		for (const String &architecture : architectures) {
			// A universal library serves each architecture of the export.
			if (architecture == "universal") {
				continue;
			}
			String key = library_key(keys, p_features, architecture);
			if (key.is_empty() && architectures.has("universal")) {
				key = library_key(keys, p_features, "universal");
			}
			const String target = vformat("a %s export to %s %s", p_is_debug ? "debug" : "release", platform->get_os_name(), architecture);
			if (key.is_empty()) {
				fail(vformat("%s has no library for %s.", extension, target), "Install a release of Petrichor that supports this target.");
				continue;
			}
			// core/extension/gdextension_library_loader.cpp:100-102 (ed1daf0).
			String library = config->get_value("libraries", key);
			if (library.is_relative_path()) {
				library = extension.get_base_dir().path_join(library);
			}
			// On macOS a library may be a framework, a directory.
			if (!FileAccess::file_exists(library) && !DirAccess::dir_exists_absolute(library)) {
				fail(vformat("%s names %s as the library for %s, and that file does not exist.", extension, library, target), "Install Petrichor again, with every file of its release.");
			}
		}
	}
}

void PetExportPlugin::_export_file(const String &p_path, const String &p_type, const PackedStringArray &p_features) {
	// An error message fails the export in the editor's window
	// (editor/export/editor_export_platform.cpp:131-148; ed1daf0).
	const compiler::Analysis::Pass from_files(analysis(), &SceneIndex::get());
	const String extension = p_path.get_extension().to_lower();
	if (p_type == "PackedScene" || extension == "tres" || extension == "res") {
		std::vector<std::string> scenes = SceneIndex::get().embedded_scenes(to_utf8(p_path));
		scenes.push_back(to_utf8(p_path));
		for (const std::string &scene : scenes) {
			for (const compiler::Diagnostic &error : check_scene(to_godot(scene), true)->errors) {
				get_export_platform()->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, "Petrichor", to_godot(error.message));
			}
		}
		return;
	}
	if (extension != "pet") {
		return;
	}
	// Without skip(), the engine would export the source as it is; exported
	// games contain only bytecode (spec §1.3).
	const String source = FileAccess::get_file_as_string(p_path);
	if (FileAccess::get_open_error() != OK) {
		skip();
		get_export_platform()->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, "Petrichor", vformat("Cannot read %s for export.", p_path));
		return;
	}
	const std::shared_ptr<const ClassImage> image = compile_script(p_path, source, nullptr, true);
	if (!image) {
		skip();
		get_export_platform()->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, "Petrichor", vformat("%s has errors and is left out of the export.", p_path));
		return;
	}
	const std::vector<uint8_t> data = bytecode::serialize(image->module);
	PackedByteArray bytes;
	bytes.resize(int64_t(data.size()));
	std::memcpy(bytes.ptrw(), data.data(), data.size());
	// With remapping, the engine leaves out the source and loads the bytecode
	// when the game asks for the `.pet` path.
	add_file(p_path.get_basename() + ".petc", bytes, true);
}

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

namespace {

// Where a scene or a resource file refers: a node path, `:` and the property
// stored there; or a connection's signal, or the node that names a track's
// member, and what it names.
String place_in_scene(const compiler::Reference &p_reference) {
	const compiler::SceneConnection &connection = p_reference.connection;
	const compiler::SceneNamedMember &named = p_reference.named;
	std::string place;
	if (!connection.signal.empty()) {
		place = connection.source + ":" + connection.signal + " → " + connection.method;
	} else if (!named.from.empty()) {
		place = named.from + (named.animation.empty() ? std::string() : ":" + named.animation) + " → " + named.node + ":" + named.name;
	} else if (p_reference.resource.find("::") != std::string::npos) {
		place = p_reference.resource.substr(p_reference.resource.find("::") + 2) + ":" + p_reference.property;
	} else if (!p_reference.resource.empty()) {
		place = p_reference.property;
	} else {
		place = p_reference.node + (p_reference.property.empty() ? std::string() : ":" + p_reference.property);
	}
	return to_godot(place);
}

} // namespace

PetReferences::PetReferences() {
	set_name("References");
	actions = memnew(HBoxContainer);
	Button *apply_button = memnew(Button);
	apply_button->set_text("Apply");
	apply_button->connect("pressed", callable_mp(this, &PetReferences::finish).bind(true));
	Button *cancel = memnew(Button);
	cancel->set_text("Cancel");
	cancel->connect("pressed", callable_mp(this, &PetReferences::finish).bind(false));
	actions->add_child(apply_button);
	actions->add_child(cancel);
	add_child(actions);
	tree = memnew(Tree);
	tree->set_v_size_flags(SIZE_EXPAND_FILL);
	tree->set_hide_root(true);
	tree->set_select_mode(Tree::SELECT_ROW);
	tree->set_allow_reselect(true);
	tree->add_theme_constant_override("inner_item_margin_left", 0);
	tree->add_theme_constant_override("inner_item_margin_right", 0);
	tree->connect("item_selected", callable_mp(this, &PetReferences::open));
	tree->connect("item_edited", callable_mp(this, &PetReferences::tick));
	add_child(tree);
}

void PetReferences::_notification(int p_what) {
	if (p_what == NOTIFICATION_THEME_CHANGED) {
		tree->add_theme_font_override("font", get_theme_font("source", "EditorFonts"));
		tree->add_theme_font_size_override("font_size", get_theme_font_size("source_size", "EditorFonts"));
		fill();
	}
}

void PetReferences::set_found(std::vector<compiler::Occurrence> p_found) {
	shown = Shown::References;
	found = std::move(p_found);
	searched = workspace().revision();
	fill();
}

void PetReferences::set_changes(const std::vector<compiler::RenameEdit> &p_changes, const Callable &p_apply) {
	shown = Shown::Changes;
	found.clear();
	ticked.clear();
	for (const compiler::RenameEdit &change : p_changes) {
		found.push_back(change.occurrence);
		ticked.push_back(!change.occurrence.candidate);
	}
	apply = p_apply;
	fill();
}

void PetReferences::set_refusals(const std::vector<compiler::Diagnostic> &p_refusals) {
	shown = Shown::Refusals;
	found.clear();
	for (const compiler::Diagnostic &refusal : p_refusals) {
		compiler::Occurrence &each = found.emplace_back();
		each.file = refusal.file;
		each.line = refusal.line;
		each.text = refusal.message;
	}
	fill();
}

void PetReferences::tick() {
	const TreeItem *item = tree->get_edited();
	ticked[int64_t(item->get_metadata(0))] = item->is_checked(0);
}

void PetReferences::finish(bool p_apply) {
	if (p_apply) {
		apply.call();
	} else {
		set_found({});
	}
}

// By file, with the candidates in files of their own after the rest, dimmed.
// Changes have their check boxes before them.
void PetReferences::fill() {
	tree->clear();
	actions->set_visible(shown == Shown::Changes);
	tree->set_columns(shown == Shown::Changes ? 2 : 1);
	tree->set_column_expand(0, shown != Shown::Changes);
	const int column = tree->get_columns() - 1;
	TreeItem *root = tree->create_item();
	const Color color = tree->get_theme_color("font_color");
	TreeItem *file = nullptr;
	for (size_t i = 0; i < found.size(); i++) {
		const compiler::Occurrence &each = found[i];
		if (shown == Shown::Refusals) {
			TreeItem *item = tree->create_item(root);
			item->set_text(0, to_godot(each.file) + (each.line > 0 ? vformat(":%d", each.line) : String()) + ": " + to_godot(each.text));
			item->set_metadata(0, int64_t(i));
			continue;
		}
		if (i == 0 || each.file != found[i - 1].file || each.candidate != found[i - 1].candidate) {
			file = tree->create_item(root);
			file->set_text(column, to_godot(each.file));
			file->set_custom_color(column, color * Color(1, 1, 1, 0.67));
			file->set_selectable(0, false);
			file->set_selectable(column, false);
		}
		TreeItem *item = tree->create_item(file);
		if (shown == Shown::Changes) {
			item->set_cell_mode(0, TreeItem::CELL_MODE_CHECK);
			item->set_checked(0, ticked[i]);
			item->set_editable(0, each.candidate);
		}
		if (each.line > 0) {
			// First, as it clears the cell.
			item->set_cell_mode(column, TreeItem::CELL_MODE_CUSTOM);
			item->set_text(column, vformat("%3s: ", each.line) + to_godot(each.text).strip_edges(true, false));
			item->set_custom_draw_callback(column, callable_mp(this, &PetReferences::highlight));
		} else {
			item->set_text(column, place_in_scene(each.reference));
		}
		item->set_metadata(0, int64_t(i));
		if (each.candidate) {
			item->set_custom_color(column, color * Color(1, 1, 1, 0.5));
		}
	}
}

// Behind the name in its line, as Find in Files marks a match.
void PetReferences::highlight(Object *p_item, const Rect2 &p_rect) {
	const TreeItem *item = Object::cast_to<TreeItem>(p_item);
	const compiler::Occurrence &each = found[int64_t(item->get_metadata(0))];
	const String line = to_godot(each.text);
	const String shown = item->get_text(tree->get_columns() - 1);
	const Ref<Font> font = tree->get_theme_font("font");
	const int32_t size = tree->get_theme_font_size("font_size");
	const float scale = EditorInterface::get_singleton()->get_editor_scale();
	// The line is shown after its number, without its indentation.
	const int64_t before = shown.length() - line.length() + each.column - 1;
	Rect2 rect = p_rect;
	rect.position.x += font->get_string_size(shown.left(before), HORIZONTAL_ALIGNMENT_LEFT, -1, size).x - 1;
	rect.position.y += scale;
	rect.size.x = font->get_string_size(line.substr(each.column - 1, each.end_column - each.column), HORIZONTAL_ALIGNMENT_LEFT, -1, size).x + 1;
	rect.size.y -= 2 * scale;
	const Color accent = get_theme_color("accent_color", "Editor");
	const RID canvas = tree->get_custom_drawing_canvas_item();
	RenderingServer::get_singleton()->canvas_item_add_rect(canvas, rect, Color(accent, 0.17));
	const PackedVector2Array outline({ rect.position, rect.position + Vector2(rect.size.x, 0), rect.get_end(), rect.position + Vector2(0, rect.size.y), rect.position });
	RenderingServer::get_singleton()->canvas_item_add_polyline(canvas, outline, PackedColorArray({ Color(accent, 0.33) }), 2.0);
}

void PetReferences::open() {
	if (shown != Shown::Changes || tree->get_selected_column() != 0) {
		go(size_t(int64_t(tree->get_selected()->get_metadata(0))));
	}
}

// A script at the name; a scene at the node, with the property stored
// there; a resource at the property; a scene that a file embeds at that file.
// A reference in a file changed since is found again first, after the Tree's
// selection: the Tree emits item_selected while it walks its items, which
// filling it again frees (scene/gui/tree.cpp:3006-3074; ed1daf0).
void PetReferences::go(size_t p_index) {
	const compiler::Occurrence &each = found[p_index];
	if (shown == Shown::References && workspace().version(each.file) > searched) {
		callable_mp(this, &PetReferences::search_again).call_deferred(int64_t(p_index), searched);
		return;
	}
	EditorInterface *editor = EditorInterface::get_singleton();
	const compiler::Reference &reference = each.reference;
	const String file = to_godot(each.file).get_slice("::", 0);
	const String property = to_godot(reference.property);
	const String extension = file.get_extension().to_lower();
	if (extension == "pet" || extension == "gd" || extension == "cs") {
		editor->edit_script(ResourceLoader::get_singleton()->load(file), each.line, each.column);
		return;
	}
	if (extension != "tscn" && extension != "scn") {
		editor->inspect_object(ResourceLoader::get_singleton()->load(file).ptr(), property);
		return;
	}
	editor->open_scene_from_path(file);
	Node *root = editor->get_edited_scene_root();
	if (!root || root->get_scene_file_path() != file || file != to_godot(each.file)) {
		return;
	}
	if (!reference.resource.empty()) {
		// The scene's own resources are cached with it.
		const Ref<Resource> resource = ResourceLoader::get_singleton()->get_cached_ref(to_godot(reference.resource));
		if (resource.is_valid()) {
			editor->inspect_object(resource.ptr(), property);
		}
		return;
	}
	// The node that sends a connection's signal, or whose track names it.
	std::string path = reference.node;
	if (!reference.connection.source.empty()) {
		path = reference.connection.source;
	} else if (!reference.named.from.empty()) {
		path = reference.named.from;
	}
	Node *node = root->get_node_or_null(NodePath(to_godot(path)));
	if (!node) {
		return;
	}
	editor->get_selection()->clear();
	editor->get_selection()->add_node(node);
	editor->inspect_object(node, property);
}

String PetHighlighter::_get_name() const {
	return PetLanguage::get_singleton()->_get_name();
}

// The script editor gives a script the highlighter of its language
// (editor/script/script_editor_plugin.cpp:2319-2345; ed1daf0).
PackedStringArray PetHighlighter::_get_supported_languages() const {
	return PackedStringArray({ _get_name() });
}

Ref<EditorSyntaxHighlighter> PetHighlighter::_create() const {
	Ref<PetHighlighter> created;
	created.instantiate();
	return created;
}

void PetHighlighter::_update_cache() {
	const Ref<EditorSettings> settings = EditorInterface::get_singleton()->get_editor_settings();
	const auto setting = [&](const char *p_name) -> Color {
		return settings->get_setting(String("text_editor/theme/highlighting/") + p_name);
	};
	text = get_text_edit()->get_theme_color("font_color");
	symbol = setting("symbol_color");
	number = setting("number_color");
	string = setting("string_color");
	comment = setting("comment_color");
	doc_comment = setting("doc_comment_color");
	function = setting("function_color");
	member = setting("member_variable_color");
	function_definition = setting("gdscript/function_definition_color");
	global_function = setting("gdscript/global_function_color");
	node_path = setting("gdscript/node_path_color");
	node_reference = setting("gdscript/node_reference_color");
	annotation = setting("gdscript/annotation_color");
	string_name = setting("gdscript/string_name_color");

	// As in GDScript's, a type's color wins over a keyword's, and a global
	// function's over both.
	names.clear();
	const PetLanguage *language = PetLanguage::get_singleton();
	const Color keyword = setting("keyword_color");
	const Color control_flow = setting("control_flow_keyword_color");
	for (const String &word : language->_get_reserved_words()) {
		names[to_utf8(word)] = language->_is_control_flow_keyword(word) ? control_flow : keyword;
	}
	const Color engine_type = setting("engine_type_color");
	for (const std::string &name : ClassDBApi::get().class_list()) {
		names[name] = engine_type;
	}
	const Color user_type = setting("user_type_color");
	for (const auto &entry : SceneIndex::get().global_class_paths()) {
		names[entry.first] = user_type;
	}
	for (const std::string &name : SceneIndex::get().autoload_names()) {
		names[name] = user_type;
	}
	// Built-in types and global enums.
	const Color base_type = setting("base_type_color");
	std::vector<std::string> types;
	std::vector<std::string> values;
	compiler::builtin::global_names(types, values);
	for (const std::string &name : types) {
		names[name] = compiler::builtin::find_class(name) ? base_type : engine_type;
	}
	names["Variant"] = base_type;
	names["void"] = base_type;
	names["assert"] = global_function;
	names["preload"] = global_function;
	version = -1;
}

void PetHighlighter::highlight() const {
	using compiler::TokenKind;
	TextEdit *edit = get_text_edit();
	version = edit->get_version();
	lines.clear();
	for (int32_t i = 0; i < edit->get_line_count(); i++) {
		lines.push_back(edit->get_line_with_ime(i));
	}
	std::vector<compiler::Diagnostic> errors;
	const std::shared_ptr<const compiler::SourceFile> file = compiler::tokenize(to_utf8(String("\n").join(lines)), errors);
	struct Span {
		uint32_t begin;
		uint32_t end;
		Color color;
	};
	std::vector<Span> spans;
	const std::vector<compiler::Token> &tokens = file->tokens;
	// A node reference: `$` or a `%` after no value, and the names, strings
	// and "/" right after it.
	bool node_reference_goes_on = false;
	for (size_t i = 0; i < tokens.size(); i++) {
		const compiler::Token &token = tokens[i];
		if (token.length == 0) {
			continue;
		}
		const TokenKind kind = token.kind;
		const TokenKind previous = i > 0 ? tokens[i - 1].kind : TokenKind::Newline;
		const bool adjacent = i > 0 && tokens[i - 1].end() == token.offset;
		const bool word = (kind == TokenKind::Identifier || (kind >= TokenKind::Await && kind <= TokenKind::While)) && !token.text.empty();
		const bool call = tokens[i + 1].kind == TokenKind::ParenOpen;
		const bool unique_name = kind == TokenKind::Percent && !compiler::ends_operand(previous);
		node_reference_goes_on = node_reference_goes_on && adjacent && (word || kind == TokenKind::String || kind == TokenKind::Slash);
		Color color = symbol;
		if (node_reference_goes_on || kind == TokenKind::Dollar || unique_name) {
			node_reference_goes_on = true;
			color = node_reference;
		} else if (kind == TokenKind::Int || kind == TokenKind::Float) {
			color = number;
		} else if (kind == TokenKind::String) {
			color = string;
		} else if (kind == TokenKind::StringName) {
			color = string_name;
		} else if (kind == TokenKind::NodePath) {
			color = node_path;
		} else if (kind == TokenKind::At || (word && previous == TokenKind::At && adjacent)) {
			color = annotation;
		} else if (word && previous == TokenKind::Period) {
			color = call ? function : member;
		} else if (word && previous == TokenKind::Func) {
			color = function_definition;
		} else if (word && previous == TokenKind::Signal) {
			color = member;
		} else if (word && call && compiler::builtin::find_utility(token.text)) {
			color = global_function;
		} else if (word) {
			const auto found = names.find(token.text);
			color = found != names.end() ? found->second : call ? function
																: text;
		}
		spans.push_back({ token.offset, token.end(), color });
	}
	for (const compiler::Token &trivia : file->trivia) {
		const bool is_comment = trivia.kind == TokenKind::Comment;
		const bool doc = is_comment && file->text.compare(trivia.offset, 2, "##") == 0;
		spans.push_back({ trivia.offset, trivia.end(), doc ? doc_comment : is_comment ? comment
																					  : symbol });
	}
	std::sort(spans.begin(), spans.end(), [](const Span &p_a, const Span &p_b) { return p_a.begin < p_b.begin; });
	line_colors.assign(size_t(lines.size()), {});
	for (const Span &span : spans) {
		const compiler::SourcePosition begin = file->position(span.begin);
		const compiler::SourcePosition end = file->position(span.end);
		line_colors[begin.line - 1].push_back({ begin.column - 1, span.color });
		// The lines a string goes on to.
		for (int line = begin.line; line < end.line; line++) {
			line_colors[line].push_back({ 0, span.color });
		}
	}
}

Dictionary PetHighlighter::_get_line_syntax_highlighting(int32_t p_line) const {
	const TextEdit *edit = get_text_edit();
	ERR_FAIL_INDEX_V(p_line, edit->get_line_count(), Dictionary());
	// The engine asks again for the lines from an edited one on
	// (scene/resources/syntax_highlighter.cpp:37-66; ed1daf0). An edit changes
	// the version; the input method's text changes only its line
	// (scene/gui/text_edit.cpp:3472-3492).
	if (int64_t(edit->get_version()) != version || p_line >= lines.size() || edit->get_line_with_ime(p_line) != lines[p_line]) {
		highlight();
	}
	Dictionary colors;
	for (const auto &[column, color] : line_colors[p_line]) {
		Dictionary entry;
		entry["color"] = color;
		colors[column] = entry;
	}
	return colors;
}

void PetEditorPlugin::_bind_methods() {
	// For the editor's undo history, which calls methods by name.
	ClassDB::bind_method(D_METHOD("_rename_declaration", "kind", "scope", "name", "new_name", "candidates", "apply"), &PetEditorPlugin::rename_declaration, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("_change_scripts", "edits", "apply"), &PetEditorPlugin::change_scripts_again, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("_move_nodes", "scene", "moves"), &PetEditorPlugin::move_nodes);
}

static void resource_saved(const Ref<Resource> &p_resource) {
	file_seen(to_utf8(p_resource->get_path()));
}

static void scene_saved(const String &p_path) {
	file_seen(to_utf8(p_path));
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
	connect("scene_saved", callable_mp_static(&scene_saved));
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
	connect("scene_closed", callable_mp(this, &PetEditorPlugin::history_changed).unbind(1));
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
	if (args.has("--petrichor-build")) {
		callable_mp(this, &PetEditorPlugin::run_build_command).call_deferred();
	} else if (args.has("--petrichor-format")) {
		callable_mp(this, &PetEditorPlugin::run_format_command).call_deferred();
	} else if (args.has("--petrichor-lsp") || DisplayServer::get_singleton()->get_name() != "headless") {
		lsp = std::make_unique<LspServer>(sessions, [this] { changed(); }, [this](const std::set<std::string> &p_files) { reload_written(p_files); });
		get_tree()->connect("process_frame", callable_mp(this, &PetEditorPlugin::poll_lsp));
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
	undo_redo->add_do_method(this, "callv", "call_deferred", Array::make("_move_nodes", scene, moves));
	undo_redo->add_undo_method(this, "callv", "call_deferred", Array::make("_move_nodes", scene, back));
	undo_redo->commit_action();
	merging = false;
}

// From the texts as they are, as an undo of a rename recomputes it
// (rename_declaration()). Each change and each path left is a line of the
// Output panel.
void PetEditorPlugin::move_nodes(const String &p_scene, const Dictionary &p_moves) {
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
	if (!changes.empty() && !change_files(changes)) {
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
// the refactoring's edit last, and Redo once it has nothing to redo. One that
// would be refused is not, and the histories do not move (architecture §2.3).
void PetEditorPlugin::_input(const Ref<InputEvent> &p_event) {
	const bool undo = p_event->is_action_pressed("ui_undo", true, true);
	if (recorded.empty() || !(undo || p_event->is_action_pressed("ui_redo", true, true))) {
		return;
	}
	ScriptEditor *script_editor = EditorInterface::get_singleton()->get_script_editor();
	ScriptEditorBase *current = script_editor->get_current_editor();
	const CodeEdit *code_edit = current ? Object::cast_to<CodeEdit>(current->get_base_editor()) : nullptr;
	const Ref<Script> script = script_editor->get_current_script();
	if (!code_edit || !code_edit->has_focus() || !Object::cast_to<PetScript>(script.ptr())) {
		return;
	}
	const auto last = refactored.find(code_edit->get_instance_id());
	if (undo ? code_edit->has_undo() && (last == refactored.end() || last->second != code_edit->get_version()) : code_edit->has_redo()) {
		return;
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
void PetReferences::search_again(int64_t p_index, uint64_t p_searched) {
	if (p_searched != searched || size_t(p_index) >= found.size()) {
		return;
	}
	const std::string file = found[p_index].file;
	const size_t nth = size_t(std::count_if(found.begin(), found.begin() + int64_t(p_index), [&](const compiler::Occurrence &p_each) { return p_each.file == file; }));
	const compiler::Occurrence &first = found.front();
	const std::string declared = compiler::script_file(first.reference.target.scope);
	const std::optional<uint32_t> offset = first.declaration && compiler::is_script_path(declared) ? declaration_offset(declared, first.reference.target) : std::nullopt;
	std::optional<std::vector<compiler::Occurrence>> again = offset ? find_references(declared, *offset) : std::nullopt;
	if (!again) {
		set_found({});
		toast("The project changed.");
		return;
	}
	set_found(std::move(*again));
	for (size_t i = 0, seen = 0; i < found.size(); i++) {
		if (found[i].file == file && seen++ == nth) {
			go(i);
			return;
		}
	}
}

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
		apply(renaming.path, renaming.offset, renaming.name, edits, true);
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
PetEditorPlugin::Saving PetEditorPlugin::scenes_saved(const std::set<std::string> &p_files) {
	EditorInterface *editor = EditorInterface::get_singleton();
	TabBar *tabs = scene_tabs();
	std::string failed;
	for (const std::string &scene : open_scenes_showing(p_files)) {
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
	const int32_t shown = shown_before;
	shown_before = -1;
	if (tabs && shown >= 0 && tabs->get_current_tab() != shown) {
		tabs->set_current_tab(shown);
		if (failed.empty()) {
			return Saving::Waiting;
		}
	}
	if (!failed.empty()) {
		toast(vformat("%s cannot be saved, so nothing was renamed.", to_godot(failed)), EditorToaster::SEVERITY_WARNING);
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
	apply(renaming.path, renaming.offset, renaming.name, chosen, true);
}

// A rename in one open script is a step of its undo; one that changes more
// files, a step of the editor's history (architecture §2.3).
void PetEditorPlugin::apply(const std::string &p_path, uint32_t p_offset, const std::string &p_name, const std::vector<compiler::RenameEdit> &p_edits, bool p_record) {
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
	} else if (!change_files(changes)) {
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
	record(vformat("Rename \"%s\" to \"%s\"", to_godot(old_name), to_godot(p_name)), files, Array::make("_rename_declaration", int(before.kind), to_godot(before.scope), to_godot(before.name), to_godot(p_name), candidates_before), Array::make("_rename_declaration", int(renamed.kind), to_godot(renamed.scope), to_godot(renamed.name), to_godot(old_name), candidates_after));
}

// Changes all files or none, and fails when a text is not what an edit
// replaces or a file cannot be written (architecture §2.3). A script's text
// changes: an open one's in its tab, as one step to undo there, and a closed
// one's held unsaved. Other files are written as bytes, the first ones
// written back if one cannot be, and the open scenes that show their nodes
// reloaded, which clears their undo histories (editor/editor_node.cpp:7124-7160;
// ed1daf0).
bool PetEditorPlugin::change_files(const FileEdits &p_changes) {
	std::map<std::string, std::pair<std::string, std::string>> texts; // Before and after, by file.
	std::set<std::string> others; // The files that are not scripts.
	for (const auto &[file, edits] : p_changes) {
		if (!is_script(file)) {
			others.insert(file);
		}
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
	std::set<std::string> written;
	for (const auto &[file, text] : texts) {
		if (is_script(file)) {
			continue;
		}
		if (!write_file(to_godot(file), text.second)) {
			for (const std::string &each : written) {
				write_file(to_godot(each), texts[each].first);
			}
			toast(vformat("Cannot write %s, so nothing was changed.", to_godot(file)), EditorToaster::SEVERITY_WARNING);
			return false;
		}
		written.insert(file);
	}
	for (const auto &[file, text] : texts) {
		if (!is_script(file)) {
			continue;
		}
		CodeEdit *code_edit = open_code_edit(file);
		if (!code_edit) {
			hold_script(file, text.second);
			continue;
		}
		std::vector<compiler::Edit> plain;
		for (const compiler::CheckedEdit &edit : p_changes.at(file)) {
			plain.push_back({ edit.begin, edit.end, edit.new_text });
		}
		apply_edits(code_edit, text.first, plain);
		refactored[code_edit->get_instance_id()] = code_edit->get_version();
		// Back to its file, as an undo leaves it, the tab is saved.
		if (without_cr(SceneIndex::get().script_source(file).value_or(std::string())) == text.second) {
			code_edit->tag_saved_version();
		}
	}
	std::set<std::string> files;
	for (const auto &[file, edits] : p_changes) {
		files.insert(file);
	}
	reload_written(files);
	return true;
}

// After a refactoring changed files of the project, the editor's or the LSP
// client's (architecture §2.3, §2.4): scripts in their texts, held or in tabs,
// or written, and the other files written. The loaded scripts among them take
// the workspace's texts, as a tab's script takes its checked text, and reload
// in one pass, so that each compiles once against the others' new texts. A tab
// without unsaved changes takes a script's text written, as the script editor
// reloads it (editor/script/script_editor_plugin.cpp:2535-2588; ed1daf0). Then
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
	for (const std::string &file : p_files) {
		file_seen(file);
		const String path = to_godot(file);
		if (!is_script(file)) {
			written(path);
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
		if (without_cr(workspace().script_source(file).value_or(std::string())) == to_utf8(text)) {
			written(path);
		}
	}
	// Before what else is loaded is read again, also the scripts that the LSP
	// client or a refactoring has unsaved texts of, which carry a rename the
	// client has not saved all of yet.
	std::set<std::string> taking = others.empty() ? std::set<std::string>() : overlaid_scripts();
	for (const std::string &file : p_files) {
		if (is_script(file)) {
			taking.insert(file);
		}
	}
	for (const std::string &file : taking) {
		const String current = to_godot(workspace().script_source(file).value_or(std::string()));
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
		SceneIndex::get().scripts_reloaded(reloaded);
		disk_changed();
	}
	for (const std::string &file : others) {
		const String path = to_godot(file);
		const Ref<Resource> loaded = ResourceLoader::get_singleton()->get_cached_ref(path);
		if (loaded.is_valid()) {
			loaded->copy_from_resource(ResourceLoader::get_singleton()->load(path, loaded->get_class(), ResourceLoader::CACHE_MODE_IGNORE));
		}
	}
	// Reloading a scene clears its history, which is no step of its own.
	const PackedStringArray unsaved = editor->get_unsaved_scenes();
	const uint64_t steps = elsewhere;
	for (const std::string &scene : others.empty() ? std::vector<std::string>() : open_scenes_showing(others)) {
		if (!unsaved.has(to_godot(scene))) {
			editor->reload_scene_from_path(to_godot(scene));
		}
	}
	elsewhere = steps;
	changed();
}

// The held scripts are written as a script's tab is saved, and those loaded
// read again against their new texts, as the script editor reloads a script
// (editor/script/script_editor_plugin.cpp:2564-2565; ed1daf0). One that cannot
// be written stays held; one whose file is gone is dropped, and so is one whose
// file changed since, which is told (keep_held()).
void PetEditorPlugin::save_held() {
	std::set<std::string> written;
	const std::map<std::string, std::string> files = held_scripts();
	for (const auto &[file, text] : files) {
		const String path = to_godot(file);
		if (!FileAccess::file_exists(path)) {
			drop_held(file);
			continue;
		}
		String why;
		if (!keep_held(file, SceneIndex::get().script_source(file).value_or(std::string()), why)) {
			toast(why, EditorToaster::SEVERITY_WARNING);
			continue;
		}
		if (write_file(path, byte_order_mark(file_bytes(path)) + text)) {
			drop_held(file);
			written.insert(file);
		}
	}
	reload_written(written);
	for (const std::string &file : written) {
		script_written(to_godot(file), sessions);
	}
	changed();
}

// The scripts held unsaved, in the words of the script editor's own
// (editor/script/script_editor_plugin.cpp:4332-4372; ed1daf0). None is of a
// scene, whose tab the editor would otherwise mark unsaved
// (editor/editor_node.cpp:2657-2677).
String PetEditorPlugin::_get_unsaved_status(const String &p_for_scene) const {
	if (!p_for_scene.is_empty() || held_scripts().empty()) {
		return String();
	}
	PackedStringArray message({ "Save changes to the following script(s) before quitting?" });
	for (const auto &held : held_scripts()) {
		message.push_back(to_godot(held.first));
	}
	return String("\n").join(message);
}

// When a scene is saved, or Save finds no scene's root to save, before the game
// runs, and on quitting when the user chooses to (editor/editor_node.cpp:2415-2420,
// 2526, 2603, 3488-3495, 3730-3733, 3793-3806; ed1daf0). Not as the editor
// leaves the tree, which asks every plugin whatever the user chose; the script
// editor keeps its tabs then too (editor/editor_node.cpp:969-991,
// editor/script/script_editor_plugin.cpp:4375-4378). This plugin, a child of
// the editor, leaves first (editor/editor_node.cpp:4365-4370, 9421;
// scene/main/node.cpp:421-427; ed1daf0).
void PetEditorPlugin::_save_external_data() {
	if (!exiting) {
		save_held();
	}
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
String PetEditorPlugin::rename_declaration(int p_kind, const String &p_scope, const String &p_name, const String &p_new, const Array &p_candidates, bool p_apply) {
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
		callable_mp(this, &PetEditorPlugin::rename_declaration).call_deferred(p_kind, p_scope, p_name, p_new, p_candidates, p_apply);
		return String();
	} else if (saved == Saving::Failed) {
		return "An open scene cannot be saved."; // As scenes_saved() told.
	} else {
		apply(file, *offset, to_utf8(p_new), chosen, false);
		return String();
	}
	const String refusal = vformat("Cannot rename it to \"%s\": %s", p_new, to_godot(why));
	if (p_apply) {
		toast(refusal, EditorToaster::SEVERITY_WARNING);
	}
	return refusal;
}

// The tab of a script of this language gets a gutter for the bulb of quick
// fixes, right of the fold gutter (architecture §2.4). A tab opened takes the
// text held for its script before anything else reads it: the signal comes
// before the tab's first check (editor/script/script_editor_plugin.cpp:2305,
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

// The fixes of the errors on the caret's line: first of those that hold the
// caret, then of the others by position. Apart, fixing all of each automatic
// kind among them, where the script's own check has more than one fix of it
// that changes only this script, as fixing all takes them (architecture §2.1).
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
	std::stable_partition(errors.begin(), errors.end(), [&](const compiler::Diagnostic *p_error) { return p_error->begin <= caret && caret <= p_error->end; });
	listed.clear();
	fix_list->clear();
	for (const compiler::Diagnostic *error : errors) {
		for (const compiler::Fix &fix : error->fixes) {
			if (std::none_of(listed.begin(), listed.end(), [&](const auto &p_listed) { return same_fix(p_listed.second, fix); })) {
				fix_list->add_item(to_godot(fix.title), int32_t(listed.size()));
				listed.emplace_back(*error, fix);
			}
		}
	}
	std::set<compiler::FixKind> kinds;
	for (const auto &[error, fix] : listed) {
		if (fix.automatic() && kinds.insert(fix.kind).second && fixes_all(fixing.errors, fixing.path, fix.kind)) {
			if (fix_list->get_item_count() == int32_t(listed.size())) {
				fix_list->add_separator();
			}
			fix_list->add_item(vformat("Fix all '%s' in file", compiler::FIX_KINDS[size_t(fix.kind)].name), int32_t(listed.size() + size_t(fix.kind)));
		}
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
	} else if (change_scripts(changes, true).is_empty()) {
		record(fix_list->get_item_text(fix_list->get_item_index(p_id)), files, Array::make("_change_scripts", to_array(changes)), Array::make("_change_scripts", to_array(inverse(changes))));
	}
}

// Against the texts the workspace has, as a rename edits them (architecture
// §2.3). Then the tab checks its script at once, as when its text stops
// changing (editor/gui/code_editor.cpp:1696-1699,
// editor/script/script_editor_base.cpp:653; ed1daf0). Edits made already are
// left, as a menu's undo that was refused leaves them once the history moves
// on. Without `p_apply`, it only finds why it would be refused, or "".
String PetEditorPlugin::change_scripts(const FileEdits &p_changes, bool p_apply) {
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
	if (!p_apply || !change_files(p_changes)) {
		return String();
	}
	if (CodeEdit *code_edit = fixing_code_edit()) {
		code_edit->get_parent()->emit_signal("validate_script");
	}
	return String();
}

String PetEditorPlugin::change_scripts_again(const Array &p_edits, bool p_apply) {
	return change_scripts(from_array(p_edits), p_apply);
}

void PetEditorPlugin::_exit_tree() {
	exiting = true;
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
	// Compile errors keep the project from running (spec §1.4).
	return build_project() == 0;
}

// The first scan imports the project, and lasts until its imports are done
// (editor/file_system/editor_file_system.cpp:1131-1137, 1838-1840), so a
// command on a fresh copy runs once it ends.
bool PetEditorPlugin::waits_for_scan(void (PetEditorPlugin::*p_command)()) {
	if (!EditorInterface::get_singleton()->get_resource_filesystem()->is_scanning()) {
		return false;
	}
	get_tree()->connect("process_frame", callable_mp(this, p_command), CONNECT_ONE_SHOT);
	return true;
}

void PetEditorPlugin::run_build_command() {
	if (waits_for_scan(&PetEditorPlugin::run_build_command)) {
		return;
	}
	int scripts = 0;
	int scenes = 0;
	int resources = 0;
	const int failed = build_project(&scripts, &scenes, &resources, true);
	UtilityFunctions::print(vformat("petrichor build: %d script(s), %d scene(s), %d resource file(s), %d with errors", scripts, scenes, resources, failed));
	// A non-zero exit code on compile errors (spec §13.1).
	get_tree()->quit(failed > 0 ? 1 : 0);
}

void PetEditorPlugin::run_format_command() {
	if (waits_for_scan(&PetEditorPlugin::run_format_command)) {
		return;
	}
	const bool check = OS::get_singleton()->get_cmdline_user_args().has("--check");
	const PackedStringArray paths = project_files(PackedStringArray({ "pet" }));
	int changed = 0;
	int failed = 0;
	for (const String &path : paths) {
		// As bytes, which keeps the file's line breaks.
		std::string source = file_bytes(path);
		const std::string bom = byte_order_mark(source);
		source.erase(0, bom.size());
		std::string error;
		const std::optional<std::string> result = compiler::format(source, &error);
		if (!result) {
			failed++;
			const CharString file = path.utf8();
			gdextension_interface::print_script_error(error.c_str(), "", file.get_data(), 0, false);
			continue;
		}
		if (*result == source) {
			continue;
		}
		changed++;
		if (check) {
			UtilityFunctions::print(vformat("%s is not formatted.", path));
			continue;
		}
		failed += !write_file(path, bom + *result);
	}
	UtilityFunctions::print(vformat("petrichor format: %d script(s), %d %s, %d with errors", int(paths.size()), changed, check ? "not formatted" : "formatted", failed));
	get_tree()->quit(failed > 0 || (check && changed > 0) ? 1 : 0);
}

} // namespace petrichor
