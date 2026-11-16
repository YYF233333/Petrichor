#include "adapter/editor/pet_editor_plugin.h"

#include "adapter/editor/class_db_api.h"
#include "adapter/editor/project_files.h"
#include "adapter/editor/scene_index.h"
#include "adapter/editor/script_compiler.h"
#include "adapter/pet_language.h"
#include "adapter/pet_script.h"
#include "compiler/ast.h"
#include "compiler/builtin_api.h"
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
}

void PetEditorPlugin::_enter_tree() {
	export_plugin.instantiate();
	add_export_plugin(export_plugin);
	const auto shortcut = [](const char *p_setting, const char *p_name, Key p_key, bool p_shift_alt) {
		Ref<InputEventKey> key;
		key.instantiate();
		key->set_keycode(p_key);
		key->set_shift_pressed(p_shift_alt);
		key->set_alt_pressed(p_shift_alt);
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
	code_menu->find_references = shortcut("petrichor/find_references", "Find References", KEY_F12, true);
	code_menu->add_menu_shortcut(code_menu->find_references, callable_mp(this, &PetEditorPlugin::show_references));
	// F2, as Rename in the scene tree and the FileSystem docks, which take it
	// only where they have focus (editor/docks/scene_tree_dock.cpp:181-184, 5013;
	// editor/docks/filesystem_dock.cpp:2780, 3885, 4449; ed1daf0). The script
	// editor binds no F2.
	code_menu->rename = shortcut("petrichor/rename", "Rename", KEY_F2, false);
	code_menu->add_menu_shortcut(code_menu->rename, callable_mp(this, &PetEditorPlugin::ask_name));
	add_context_menu_plugin(EditorContextMenuPlugin::CONTEXT_SLOT_SCRIPT_EDITOR_CODE, code_menu);
	references = memnew(PetReferences);
	add_control_to_bottom_panel(references, "References");
	name_popup = memnew(PopupPanel);
	name_edit = memnew(LineEdit);
	name_edit->set_custom_minimum_size(Vector2(240, 0) * EditorInterface::get_singleton()->get_editor_scale());
	name_edit->connect("text_submitted", callable_mp(this, &PetEditorPlugin::rename_to));
	name_popup->add_child(name_edit);
	add_child(name_popup);
	highlighter.instantiate();
	EditorInterface::get_singleton()->get_script_editor()->register_syntax_highlighter(highlighter);
	// editor/plugins/editor_plugin.cpp:284-289 (ed1daf0).
	connect("resource_saved", callable_mp(this, &PetEditorPlugin::resource_saved));
	connect("scene_saved", callable_mp(this, &PetEditorPlugin::scene_saved));
	EditorInterface::get_singleton()->get_resource_filesystem()->connect("filesystem_changed", callable_mp(this, &PetEditorPlugin::files_or_settings_changed));
	// Such as the main scene and autoloads (core/config/project_settings.cpp:575, ed1daf0).
	ProjectSettings::get_singleton()->connect("settings_changed", callable_mp(this, &PetEditorPlugin::files_or_settings_changed));
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

} // namespace

void PetEditorPlugin::show_references(Object *p_code_edit) {
	const std::optional<std::pair<String, uint32_t>> at = at_caret(p_code_edit);
	if (!at) {
		return;
	}
	std::optional<std::vector<compiler::Occurrence>> found = find_references(at->first, to_utf8(Object::cast_to<CodeEdit>(p_code_edit)->get_text()), at->second);
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

// Each open script's buffer is edited as one step to undo. Other files are
// written as bytes, and the open scenes among them reloaded, which clears
// their undo histories (editor/editor_node.cpp:7124-7160; ed1daf0). When it
// changes more than one buffer, the histories of the open scripts are cleared
// too, and the editor's history records the rename (architecture §2.3).
void PetEditorPlugin::apply(const OpenProject &p_project, const std::string &p_path, uint32_t p_offset, const std::string &p_name, const std::vector<compiler::RenameEdit> &p_edits, bool p_record) {
	std::map<std::string, std::vector<const compiler::RenameEdit *>> by_file;
	for (const compiler::RenameEdit &edit : p_edits) {
		by_file[edit.occurrence.file].push_back(&edit);
	}
	const bool one_buffer = by_file.size() == 1 && p_project.code_edit(by_file.begin()->first);
	EditorInterface *editor = EditorInterface::get_singleton();
	const PackedStringArray open_scenes = editor->get_open_scenes();
	// Where the name renamed is after the edits, and where the candidates
	// changed are before and after them.
	uint32_t offset = p_offset;
	std::string old_name;
	Array candidates_before;
	Array candidates_after;
	for (auto &[file, edits] : by_file) {
		std::sort(edits.begin(), edits.end(), [](const compiler::RenameEdit *p_a, const compiler::RenameEdit *p_b) { return p_a->edit.begin < p_b->edit.begin; });
		int64_t delta = 0;
		std::vector<compiler::Edit> plain;
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
			plain.push_back({ edit.begin, edit.end, edit.new_text });
		}
		const String path = to_godot(file);
		if (CodeEdit *code_edit = p_project.code_edit(file)) {
			apply_edits(code_edit, *p_project.script_source(file), plain);
			if (!one_buffer) {
				code_edit->clear_undo_history();
			}
			continue;
		}
		std::string text = file_bytes(path);
		const std::string bom = is_script(file) ? byte_order_mark(text) : std::string();
		text.erase(0, bom.size());
		std::string result;
		uint32_t done = 0;
		bool intact = true;
		for (const compiler::RenameEdit *each : edits) {
			const compiler::CheckedEdit &edit = each->edit;
			intact = intact && text.compare(edit.begin, edit.end - edit.begin, edit.old_text) == 0;
			result += text.substr(done, edit.begin - done) + edit.new_text;
			done = edit.end;
		}
		if (!intact) {
			ERR_PRINT(vformat("%s changed while renaming, and is left as it is.", path));
			continue;
		}
		if (!write_file(path, bom + result + text.substr(done))) {
			continue;
		}
		SceneIndex::get().forget(file);
		// A changed script_class takes effect (editor/file_system/editor_file_system.cpp:2400-2492; ed1daf0).
		editor->get_resource_filesystem()->update_file(path);
		if (open_scenes.has(path)) {
			editor->reload_scene_from_path(path);
			continue;
		}
		// What is loaded is read again, as when the editor sees a file change; it
		// leaves scenes to itself, and a script as the script editor reloads one
		// (editor/editor_node.cpp:1291-1323, core/io/resource.cpp:263-276,
		// scene/resources/packed_scene.h:256, editor/script/script_editor_plugin.cpp:2564-2565; ed1daf0).
		const Ref<Resource> loaded = ResourceLoader::get_singleton()->get_cached_ref(path);
		if (Script *script = Object::cast_to<Script>(loaded.ptr())) {
			script->set_source_code(FileAccess::get_file_as_string(path));
			script->reload(true);
		} else if (loaded.is_valid() && !Object::cast_to<PackedScene>(loaded.ptr())) {
			loaded->copy_from_resource(ResourceLoader::get_singleton()->load(path, loaded->get_class(), ResourceLoader::CACHE_MODE_IGNORE));
		}
	}
	project_changed();
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
	if (!p_record || one_buffer || !found || declared == p_edits.end()) {
		return;
	}
	const compiler::DeclId &before = declared->occurrence.reference.target;
	const compiler::DeclId &renamed = found->front().reference.target;
	EditorUndoRedoManager *undo_redo = get_undo_redo();
	undo_redo->create_action(vformat("Rename \"%s\" to \"%s\"", to_godot(old_name), to_godot(p_name)), UndoRedo::MERGE_DISABLE, nullptr, false, false);
	undo_redo->add_do_method(this, "_rename_declaration", int(before.kind), to_godot(before.scope), to_godot(before.name), to_godot(p_name), candidates_before);
	undo_redo->add_undo_method(this, "_rename_declaration", int(renamed.kind), to_godot(renamed.scope), to_godot(renamed.name), to_godot(old_name), candidates_after);
	undo_redo->commit_action(false);
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

void PetEditorPlugin::_exit_tree() {
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
