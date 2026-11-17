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
	const String extension = p_path.get_extension().to_lower();
	if (p_type == "PackedScene" || extension == "tres" || extension == "res") {
		for (const compiler::Diagnostic &error : check_scene(p_path, true)->errors) {
			get_export_platform()->add_message(EditorExportPlatform::EXPORT_MESSAGE_ERROR, "Petrichor", to_godot(error.message));
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

// A script at the name; a scene at the node, with the property stored
// there; a resource at the property. Not on ticking a change.
void PetReferences::open() {
	if (shown == Shown::Changes && tree->get_selected_column() == 0) {
		return;
	}
	const compiler::Occurrence &each = found[int64_t(tree->get_selected()->get_metadata(0))];
	EditorInterface *editor = EditorInterface::get_singleton();
	const compiler::Reference &reference = each.reference;
	const String file = to_godot(each.file);
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
	if (!root || root->get_scene_file_path() != file) {
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
	ClassDB::bind_method(D_METHOD("_rename_declaration", "kind", "scope", "name", "new_name", "candidates"), &PetEditorPlugin::rename_declaration);
	ClassDB::bind_method(D_METHOD("_change_scripts", "edits"), &PetEditorPlugin::change_scripts_again);
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
	connect("resource_saved", callable_mp(this, &PetEditorPlugin::resource_saved));
	connect("scene_saved", callable_mp(this, &PetEditorPlugin::scene_saved));
	EditorInterface::get_singleton()->get_resource_filesystem()->connect("filesystem_changed", callable_mp(this, &PetEditorPlugin::files_or_settings_changed));
	// Such as the main scene and autoloads (core/config/project_settings.cpp:575, ed1daf0).
	ProjectSettings::get_singleton()->connect("settings_changed", callable_mp(this, &PetEditorPlugin::files_or_settings_changed));
	// An edit of an open scene, its undo and redo, as the editor learns that a
	// scene changed (editor/editor_node.cpp:8406-8409,
	// editor/editor_undo_redo_manager.cpp:298, 327, 380). Once they pause for
	// as long as the script editor waits before it checks a script
	// (editor/gui/code_editor.cpp:1014, 1166, 1696-1699; ed1daf0).
	marks = memnew(Timer);
	marks->set_one_shot(true);
	marks->set_wait_time(EditorInterface::get_singleton()->get_editor_settings()->get_setting("text_editor/completion/idle_parse_delay"));
	marks->connect("timeout", callable_mp_static(&project_changed));
	add_child(marks);
	get_undo_redo()->connect("history_changed", callable_mp(this, &PetEditorPlugin::history_changed));
	get_undo_redo()->connect("version_changed", callable_mp(this, &PetEditorPlugin::history_changed));
	connect("scene_closed", callable_mp(this, &PetEditorPlugin::history_changed).unbind(1));
	const PackedStringArray args = OS::get_singleton()->get_cmdline_user_args();
	if (args.has("--petrichor-build")) {
		callable_mp(this, &PetEditorPlugin::run_build_command).call_deferred();
	} else if (args.has("--petrichor-format")) {
		callable_mp(this, &PetEditorPlugin::run_format_command).call_deferred();
	}
}

void PetEditorPlugin::resource_saved(Object *p_resource) {
	if (const Resource *resource = Object::cast_to<Resource>(p_resource)) {
		SceneIndex::get().forget(to_utf8(resource->get_path()));
	}
	project_changed();
}

void PetEditorPlugin::scene_saved(const String &p_path) {
	SceneIndex::get().forget(to_utf8(p_path));
	project_changed();
}

void PetEditorPlugin::files_or_settings_changed() {
	project_changed();
}

void PetEditorPlugin::history_changed() {
	marks->start();
	if (!recorded.empty()) {
		const uint64_t version = get_undo_redo()->get_history_undo_redo(EditorUndoRedoManager::GLOBAL_HISTORY)->get_version();
		elsewhere += version == global_version;
		global_version = version;
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
// and the offset of the caret in its text.
std::optional<std::pair<String, uint32_t>> at_caret(const Object *p_code_edit) {
	const CodeEdit *code_edit = Object::cast_to<CodeEdit>(p_code_edit);
	const Ref<Script> script = EditorInterface::get_singleton()->get_script_editor()->get_current_script();
	if (!code_edit || !Object::cast_to<PetScript>(script.ptr())) {
		return std::nullopt;
	}
	const String text = code_edit->get_text();
	int64_t caret = 0;
	for (int line = 0; line < code_edit->get_caret_line(); line++) {
		caret = text.find("\n", caret) + 1;
	}
	return std::make_pair(script->get_path(), uint32_t(to_utf8(text.substr(0, caret + code_edit->get_caret_column())).size()));
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

bool write_file(const String &p_path, const std::string &p_bytes) {
	const Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_null()) {
		ERR_PRINT(vformat("Cannot write %s.", p_path));
		return false;
	}
	PackedByteArray data;
	data.resize(int64_t(p_bytes.size()));
	if (!p_bytes.empty()) {
		std::memcpy(data.ptrw(), p_bytes.data(), p_bytes.size());
	}
	file->store_buffer(data);
	return true;
}

// A byte order mark is no text, as the build reads a script
// (core/string/ustring.cpp:1756-1759, ed1daf0), and is written back.
std::string byte_order_mark(const std::string &p_bytes) {
	return p_bytes.rfind("\xEF\xBB\xBF", 0) == 0 ? "\xEF\xBB\xBF" : "";
}

// The open scenes among `p_files` with unsaved changes saved as Save Scene
// saves the current one; another is shown while it is saved
// (editor/editor_interface.cpp:699-704, 775-777, editor/editor_node.h:981-987,
// editor/editor_node.cpp:2466-2560, 4961-4974; ed1daf0).
void save_scenes(const std::set<std::string> &p_files) {
	EditorInterface *editor = EditorInterface::get_singleton();
	const OpenProject project;
	const Node *current = editor->get_edited_scene_root();
	const String shown = current ? current->get_scene_file_path() : String();
	for (const std::string &file : p_files) {
		if (project.is_unsaved_scene(file)) {
			editor->open_scene_from_path(to_godot(file));
			editor->save_scene_as(to_godot(file), false);
		}
	}
	if (!shown.is_empty()) {
		editor->open_scene_from_path(shown);
	}
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

// The same fix, as two errors on a line can offer.
bool same(const compiler::Fix &p_a, const compiler::Fix &p_b) {
	return p_a.title == p_b.title && std::equal(p_a.edits.begin(), p_a.edits.end(), p_b.edits.begin(), p_b.edits.end(), [](const compiler::Fix::Edit &p_x, const compiler::Fix::Edit &p_y) {
		return p_x.file == p_y.file && p_x.change.begin == p_y.change.begin && p_x.change.end == p_y.change.end && p_x.change.new_text == p_y.change.new_text;
	});
}

// The errors with fixes of the script at `p_path`, from `p_errors` of its own
// check of `p_source`, by position, each edit naming its file. The checks of
// scenes read the saved text (architecture §6.4), so their errors count while
// the text is that, and only with fixes in this script.
std::vector<compiler::Diagnostic> fixable(std::vector<compiler::Diagnostic> p_errors, const std::string &p_path, const std::string &p_source) {
	if (SceneIndex::get().script_source(p_path) == p_source) {
		const std::vector<compiler::Diagnostic> placed = placed_errors(to_godot(p_path));
		p_errors.insert(p_errors.end(), placed.begin(), placed.end());
	}
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

} // namespace

// The plugin sees a key before the CodeEdit (scene/main/viewport.cpp:3537),
// which takes Undo and Redo also with nothing to undo or redo
// (scene/gui/text_edit.cpp:2737-2746, 4766-4768; ed1daf0). Once a script of
// this language has none, they undo or redo a rename that changed it, when
// the editor's Undo or Redo would (architecture §2.3).
void PetEditorPlugin::_input(const Ref<InputEvent> &p_event) {
	const bool undo = p_event->is_action_pressed("ui_undo", true, true);
	if (recorded.empty() || !(undo || p_event->is_action_pressed("ui_redo", true, true))) {
		return;
	}
	ScriptEditor *script_editor = EditorInterface::get_singleton()->get_script_editor();
	ScriptEditorBase *current = script_editor->get_current_editor();
	const CodeEdit *code_edit = current ? Object::cast_to<CodeEdit>(current->get_base_editor()) : nullptr;
	const Ref<Script> script = script_editor->get_current_script();
	if (!code_edit || !code_edit->has_focus() || !Object::cast_to<PetScript>(script.ptr()) || (undo ? code_edit->has_undo() : code_edit->has_redo())) {
		return;
	}
	UndoRedo *global = get_undo_redo()->get_history_undo_redo(EditorUndoRedoManager::GLOBAL_HISTORY);
	const int32_t index = global->get_current_action() + (undo ? 0 : 1);
	const auto found = recorded.find(index);
	if (found == recorded.end() || index >= global->get_history_count() || global->get_action_name(index) != found->second.name || !found->second.files.count(to_utf8(script->get_path())) || found->second.elsewhere != elsewhere) {
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

void PetEditorPlugin::show_references(Object *p_code_edit) {
	const std::optional<std::pair<String, uint32_t>> at = at_caret(p_code_edit);
	if (!at) {
		return;
	}
	const OpenProject project;
	std::optional<std::vector<compiler::Occurrence>> found = find_references(at->first, to_utf8(Object::cast_to<CodeEdit>(p_code_edit)->get_text()), at->second, &project);
	if (!found) {
		toast("No declaration at the caret.");
		return;
	}
	references->set_found(std::move(*found));
	make_bottom_panel_item_visible(references);
}

void PetEditorPlugin::ask_name(Object *p_code_edit) {
	const std::optional<std::pair<String, uint32_t>> at = at_caret(p_code_edit);
	const CodeEdit *code_edit = Object::cast_to<CodeEdit>(p_code_edit);
	const String word = at ? code_edit->get_word_under_caret(0) : String();
	if (word.is_empty()) {
		if (at) {
			toast("No declaration at the caret.");
		}
		return;
	}
	renaming.path = to_utf8(at->first);
	renaming.offset = at->second;
	name_edit->set_text(word);
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
	const OpenProject project;
	renaming.changes = rename(project, renaming.path, renaming.offset, renaming.name);
	const std::vector<compiler::RenameEdit> &edits = renaming.changes.edits;
	if (!renaming.changes.refusals.empty()) {
		references->set_refusals(renaming.changes.refusals);
	} else if (std::any_of(edits.begin(), edits.end(), [](const compiler::RenameEdit &p_edit) { return p_edit.occurrence.candidate || !is_script(p_edit.occurrence.file); })) {
		references->set_changes(edits, callable_mp(this, &PetEditorPlugin::apply_previewed));
	} else {
		references->set_found({});
		apply(project, renaming.path, renaming.offset, renaming.name, edits, true);
		return;
	}
	make_bottom_panel_item_visible(references);
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
	save_scenes(files);
	const OpenProject project;
	const compiler::Rename again = rename(project, renaming.path, renaming.offset, renaming.name);
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
	apply(project, renaming.path, renaming.offset, renaming.name, chosen, true);
}

// When it changes more than one file, the open scripts are saved too, and
// the editor's history records the rename (architecture §2.3).
void PetEditorPlugin::apply(const OpenProject &p_project, const std::string &p_path, uint32_t p_offset, const std::string &p_name, const std::vector<compiler::RenameEdit> &p_edits, bool p_record) {
	std::map<std::string, std::vector<const compiler::RenameEdit *>> by_file;
	for (const compiler::RenameEdit &edit : p_edits) {
		by_file[edit.occurrence.file].push_back(&edit);
	}
	const bool one_buffer = by_file.size() == 1 && p_project.code_edit(by_file.begin()->first);
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
	change_files(p_project, changes, !one_buffer);
	// What finding references lists of the new name should be what changed.
	const OpenProject after;
	const std::optional<std::vector<compiler::Occurrence>> found = find_references(to_godot(p_path), after.script_source(p_path).value_or(std::string()), offset, &after);
	std::map<std::string, size_t> changed;
	std::map<std::string, size_t> listed;
	for (const compiler::RenameEdit &edit : p_edits) {
		if (!edit.occurrence.candidate) {
			changed[edit.occurrence.file] = is_script(edit.occurrence.file) ? changed[edit.occurrence.file] + 1 : 1;
		}
	}
	for (const compiler::Occurrence &each : found.value_or(std::vector<compiler::Occurrence>())) {
		if (!each.candidate) {
			listed[each.file] = is_script(each.file) ? listed[each.file] + 1 : 1;
		}
	}
	if (changed != listed) {
		PackedStringArray differing;
		for (const auto &[file, count] : changed) {
			if (listed[file] != count) {
				differing.push_back(to_godot(file));
			}
		}
		for (const auto &[file, count] : listed) {
			if (!changed.count(file)) {
				differing.push_back(to_godot(file));
			}
		}
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

// Each open script's buffer is edited as one step to undo. Other files are
// written as bytes, and the open scenes among them reloaded, which clears
// their undo histories (editor/editor_node.cpp:7124-7160; ed1daf0). With
// `p_save`, the open scripts are saved too, with their histories cleared.
void PetEditorPlugin::change_files(const OpenProject &p_project, const FileEdits &p_changes, bool p_save) {
	EditorInterface *editor = EditorInterface::get_singleton();
	const PackedStringArray open_scenes = editor->get_open_scenes();
	// Reloading a scene clears its history, which is no step of its own.
	const uint64_t steps = elsewhere;
	std::vector<String> written;
	std::vector<String> saved; // Open scripts.
	for (const auto &[file, edits] : p_changes) {
		const String path = to_godot(file);
		if (CodeEdit *code_edit = p_project.code_edit(file)) {
			std::vector<compiler::Edit> plain;
			for (const compiler::CheckedEdit &edit : edits) {
				plain.push_back({ edit.begin, edit.end, edit.new_text });
			}
			apply_edits(code_edit, *p_project.script_source(file), plain);
			if (!p_save) {
				continue;
			}
			code_edit->clear_undo_history();
			// Saved as its saver writes it, so that the files it reloads read it.
			if (write_file(path, to_utf8(code_edit->get_text()))) {
				written.push_back(path);
				saved.push_back(path);
			}
			continue;
		}
		std::string text = file_bytes(path);
		const std::string bom = is_script(file) ? byte_order_mark(text) : std::string();
		text.erase(0, bom.size());
		std::string result;
		uint32_t done = 0;
		bool unchanged = true;
		for (const compiler::CheckedEdit &edit : edits) {
			unchanged = unchanged && intact(text, edit);
			result += text.substr(done, edit.begin - done) + edit.new_text;
			done = edit.end;
		}
		if (!unchanged) {
			ERR_PRINT(vformat("%s changed while it was being edited, and is left as it is.", path));
			continue;
		}
		if (write_file(path, bom + result + text.substr(done))) {
			written.push_back(path);
		}
	}
	for (const String &path : written) {
		SceneIndex::get().forget(to_utf8(path));
	}
	// What is loaded is read again once every file is written, as when the
	// editor sees a file change; it leaves scenes to itself, and a script as the
	// script editor reloads one (editor/editor_node.cpp:1291-1323,
	// core/io/resource.cpp:263-276, scene/resources/packed_scene.h:256,
	// editor/script/script_editor_plugin.cpp:2564-2565; ed1daf0).
	std::vector<Ref<Script>> scripts;
	for (const String &path : written) {
		// A changed script_class takes effect (editor/file_system/editor_file_system.cpp:2400-2492; ed1daf0).
		editor->get_resource_filesystem()->update_file(path);
		if (open_scenes.has(path)) {
			editor->reload_scene_from_path(path);
			continue;
		}
		const Ref<Resource> loaded = ResourceLoader::get_singleton()->get_cached_ref(path);
		if (Script *script = Object::cast_to<Script>(loaded.ptr())) {
			script->set_source_code(FileAccess::get_file_as_string(path));
			scripts.push_back(loaded);
		} else if (loaded.is_valid() && !Object::cast_to<PackedScene>(loaded.ptr())) {
			loaded->copy_from_resource(ResourceLoader::get_singleton()->load(path, loaded->get_class(), ResourceLoader::CACHE_MODE_IGNORE));
		}
	}
	{
		// With every source set, in one pass, each script and those that use it
		// compile once.
		const compiler::Analysis::Pass pass(analysis());
		for (const Ref<Script> &script : scripts) {
			script->reload(true);
		}
	}
	// Their tabs show them saved, as when the editor saves a script
	// (editor/editor_node.cpp:1774, 8664-8665,
	// editor/script/script_editor_plugin.cpp:723-742; ed1daf0).
	for (const String &path : saved) {
		editor->get_base_control()->get_parent()->emit_signal("resource_saved", ResourceLoader::get_singleton()->get_cached_ref(path));
	}
	elsewhere = steps;
	if (!written.empty()) {
		project_changed();
	}
}

// Adds a step to the editor's global history, already done, that calls the
// method of this plugin that `p_do` or `p_undo` names, with the arguments
// after the name. Ctrl+Z in one of `p_files` may undo it (_input). The call
// waits until the editor's undo or redo returns: a scene reloaded clears its
// history, and the editor reads its histories then, which are half undone
// during it (core/object/undo_redo.cpp:432-434,
// editor/editor_undo_redo_manager.cpp:321-325, 402-407; ed1daf0).
void PetEditorPlugin::record(const String &p_action, const std::set<std::string> &p_files, const Array &p_do, const Array &p_undo) {
	EditorUndoRedoManager *undo_redo = get_undo_redo();
	undo_redo->create_action(p_action, UndoRedo::MERGE_DISABLE, nullptr, false, false);
	undo_redo->add_do_method(this, "callv", "call_deferred", p_do);
	undo_redo->add_undo_method(this, "callv", "call_deferred", p_undo);
	undo_redo->commit_action(false);
	UndoRedo *global = undo_redo->get_history_undo_redo(EditorUndoRedoManager::GLOBAL_HISTORY);
	recorded[global->get_current_action()] = { p_action, p_files, elsewhere };
	global_version = global->get_version();
}

// Renames the declaration `p_scope` and `p_name` identify, as undoing and
// redoing a rename do, from the text as it is now, with the candidates
// changed at the offsets given. A scene it changes is saved first.
void PetEditorPlugin::rename_declaration(int p_kind, const String &p_scope, const String &p_name, const String &p_new, const Array &p_candidates) {
	const compiler::DeclId id{ compiler::DeclId::Kind(p_kind), to_utf8(p_scope), to_utf8(p_name) };
	const std::string file = compiler::script_file(id.scope);
	const auto refuse = [&](const std::string &p_why) {
		toast(vformat("Cannot rename it to \"%s\": %s", p_new, to_godot(p_why)), EditorToaster::SEVERITY_WARNING);
	};
	for (bool saved = false;; saved = true) {
		const OpenProject project;
		const std::optional<std::string> source = project.script_source(file);
		const std::optional<uint32_t> offset = source ? declaration_offset(project, file, *source, id) : std::nullopt;
		if (!offset) {
			refuse("its declaration is gone.");
			return;
		}
		const compiler::Rename found = rename(project, file, *offset, to_utf8(p_new));
		if (!found.refusals.empty()) {
			refuse(found.refusals[0].message);
			return;
		}
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
		if (!saved && std::any_of(files.begin(), files.end(), [&](const std::string &p_file) { return project.is_unsaved_scene(p_file); })) {
			save_scenes(files);
			continue;
		}
		if (candidates != p_candidates.size()) {
			refuse("a name it changed by text has moved.");
			return;
		}
		apply(project, file, *offset, to_utf8(p_new), chosen, false);
		return;
	}
}

// The tab of a script of this language gets a gutter for the bulb of quick
// fixes, right of the fold gutter (architecture §2.4).
void PetEditorPlugin::script_changed() {
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
		size_gutter(code_edit);
		// Once the line height follows the theme, which it does after the signal
		// (scene/gui/control.cpp:4633-4637; ed1daf0).
		code_edit->connect("theme_changed", callable_mp(this, &PetEditorPlugin::size_gutter).bind(code_edit), CONNECT_DEFERRED);
		code_edit->connect("caret_changed", callable_mp(this, &PetEditorPlugin::show_bulb));
		code_edit->connect("gutter_clicked", callable_mp(this, &PetEditorPlugin::bulb_clicked));
		// After each check of its script (editor/script/script_editor_base.cpp:522-525; ed1daf0).
		current->connect("edited_script_changed", callable_mp(this, &PetEditorPlugin::validated).bind(code_edit));
	}
	fixing.code_edit = code_edit->get_instance_id();
	fixing.path = to_utf8(script->get_path());
	refresh_fixes();
}

void PetEditorPlugin::validated(Object *p_code_edit) {
	if (Object::cast_to<CodeEdit>(p_code_edit) == fixing_code_edit()) {
		refresh_fixes();
	}
}

CodeEdit *PetEditorPlugin::fixing_code_edit() const {
	return fixing.code_edit ? Object::cast_to<CodeEdit>(ObjectDB::get_instance(fixing.code_edit)) : nullptr;
}

// From the tab's text, whose check the analysis keeps from the script editor's.
void PetEditorPlugin::refresh_fixes() {
	CodeEdit *code_edit = fixing_code_edit();
	if (!code_edit) {
		return;
	}
	const std::string source = to_utf8(code_edit->get_text());
	fixing.errors = fixable(analysis().compile(fixing.path, source)->errors, fixing.path, source);
	fixing.version = code_edit->get_version();
	fixing.bulb = -1;
	show_bulb();
	code_edit->queue_redraw();
}

// On the caret's line, while one of its errors has a fix and the text is the
// one checked.
void PetEditorPlugin::show_bulb() {
	CodeEdit *code_edit = fixing_code_edit();
	if (!code_edit) {
		return;
	}
	const int caret = code_edit->get_caret_line();
	const bool shown = code_edit->get_version() == fixing.version && std::any_of(fixing.errors.begin(), fixing.errors.end(), [&](const compiler::Diagnostic &p_error) { return p_error.line == caret + 1; });
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
	const bool shown = p_line == fixing.bulb && code_edit->get_instance_id() == fixing.code_edit && code_edit->get_version() == fixing.version;
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

void PetEditorPlugin::size_gutter(Object *p_code_edit) {
	CodeEdit *code_edit = Object::cast_to<CodeEdit>(p_code_edit);
	code_edit->set_gutter_width(fix_gutter(code_edit), code_edit->get_line_height());
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
	if (p_code_edit->get_version() != fixing.version) {
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
			if (std::none_of(listed.begin(), listed.end(), [&](const auto &p_listed) { return same(p_listed.second, fix); })) {
				fix_list->add_item(to_godot(fix.title), int32_t(listed.size()));
				listed.emplace_back(*error, fix);
			}
		}
	}
	const auto many = [&](compiler::FixKind p_kind) {
		std::vector<const compiler::Fix *> found;
		for (const compiler::Diagnostic &error : fixing.errors) {
			for (const compiler::Fix &fix : error.fixes) {
				const bool here = error.file.empty() && std::all_of(fix.edits.begin(), fix.edits.end(), [&](const compiler::Fix::Edit &p_edit) { return p_edit.file == fixing.path; });
				if (fix.kind == p_kind && here && std::none_of(found.begin(), found.end(), [&](const compiler::Fix *p_found) { return same(*p_found, fix); })) {
					found.push_back(&fix);
				}
			}
		}
		return found.size() > 1;
	};
	std::set<compiler::FixKind> kinds;
	for (const auto &[error, fix] : listed) {
		if (fix.automatic() && kinds.insert(fix.kind).second && many(fix.kind)) {
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

// By a check of the texts the editor has now, which keeps nothing from
// earlier checks: the analysis keeps a check while the other scripts'
// declarations stay the same, though their offsets may move, and reads them
// from disk, not from their tabs. The fix chosen is found there again by its
// error and its title. A change of the tab's script alone is one step to undo
// there. One of others is one step of the editor's history, undone by its
// inverse edits, checked as it is (architecture §2.4).
void PetEditorPlugin::apply_listed(int p_id) {
	CodeEdit *code_edit = fixing_code_edit();
	if (!code_edit) {
		return;
	}
	const OpenProject project;
	const std::string source = to_utf8(code_edit->get_text());
	compiler::CompileOptions options;
	options.path = fixing.path;
	options.project = &project;
	const compiler::CompileResult result = compiler::compile(source, ClassDBApi::get(), options);
	FileEdits changes;
	if (p_id >= int(listed.size())) {
		const std::vector<compiler::CheckedEdit> edits = compiler::fix_all(result, compiler::FixKind(p_id - int(listed.size())));
		if (!edits.empty()) {
			changes[fixing.path] = edits;
		}
	} else {
		const auto &[chosen_error, chosen] = listed[p_id];
		for (const compiler::Diagnostic &error : fixable(result.errors, fixing.path, source)) {
			const auto fix = std::find_if(error.fixes.begin(), error.fixes.end(), [&](const compiler::Fix &p_fix) { return p_fix.title == chosen.title; });
			if (changes.empty() && error.begin == chosen_error.begin && error.end == chosen_error.end && error.message == chosen_error.message && error.file == chosen_error.file && fix != error.fixes.end()) {
				for (const compiler::Fix::Edit &edit : fix->edits) {
					changes[edit.file].push_back(edit.change);
				}
			}
		}
	}
	if (changes.empty()) {
		toast("The project changed.");
		refresh_fixes();
		return;
	}
	std::set<std::string> files;
	for (auto &[file, edits] : changes) {
		std::stable_sort(edits.begin(), edits.end(), [](const compiler::CheckedEdit &p_a, const compiler::CheckedEdit &p_b) { return p_a.begin < p_b.begin; });
		files.insert(file);
	}
	if (files == std::set<std::string>{ fixing.path }) {
		std::vector<compiler::Edit> plain;
		for (const compiler::CheckedEdit &edit : changes[fixing.path]) {
			plain.push_back({ edit.begin, edit.end, edit.new_text });
		}
		apply_edits(code_edit, source, plain);
		show_bulb();
	} else if (change_scripts(project, changes)) {
		record(fix_list->get_item_text(fix_list->get_item_index(p_id)), files, Array::make("_change_scripts", to_array(changes)), Array::make("_change_scripts", to_array(inverse(changes))));
	}
}

// Against the texts the editor has: an open script's, which is then saved, as
// a rename saves it (architecture §2.3), and other files' on disk. Then the
// tab checks its script at once, as when its text stops changing
// (editor/gui/code_editor.cpp:1696-1699,
// editor/script/script_editor_base.cpp:653; ed1daf0).
bool PetEditorPlugin::change_scripts(const OpenProject &p_project, const FileEdits &p_changes) {
	for (const auto &[file, edits] : p_changes) {
		const std::optional<std::string> text = p_project.script_source(file);
		if (!text || !std::all_of(edits.begin(), edits.end(), [&](const compiler::CheckedEdit &p_edit) { return intact(*text, p_edit); })) {
			toast("The project changed.");
			refresh_fixes();
			return false;
		}
	}
	change_files(p_project, p_changes, true);
	if (CodeEdit *code_edit = fixing_code_edit()) {
		code_edit->get_parent()->emit_signal("validate_script");
	}
	return true;
}

void PetEditorPlugin::change_scripts_again(const Array &p_edits) {
	const OpenProject project;
	change_scripts(project, from_array(p_edits));
}

void PetEditorPlugin::_exit_tree() {
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
