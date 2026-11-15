#include "adapter/editor/pet_editor_plugin.h"

#include "adapter/editor/class_db_api.h"
#include "adapter/editor/project_files.h"
#include "adapter/editor/scene_index.h"
#include "adapter/editor/script_compiler.h"
#include "adapter/pet_language.h"
#include "adapter/pet_script.h"
#include "compiler/builtin_api.h"
#include "compiler/formatter.h"
#include "compiler/lexer.h"
#include "runtime/text.h"

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
	tree = memnew(Tree);
	tree->set_v_size_flags(SIZE_EXPAND_FILL);
	tree->set_hide_root(true);
	tree->set_select_mode(Tree::SELECT_ROW);
	tree->set_allow_reselect(true);
	tree->add_theme_constant_override("inner_item_margin_left", 0);
	tree->add_theme_constant_override("inner_item_margin_right", 0);
	tree->connect("item_selected", callable_mp(this, &PetReferences::open));
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
	found = std::move(p_found);
	fill();
}

// By file, with the candidates in files of their own after the rest, dimmed.
void PetReferences::fill() {
	tree->clear();
	TreeItem *root = tree->create_item();
	const Color color = tree->get_theme_color("font_color");
	TreeItem *file = nullptr;
	for (size_t i = 0; i < found.size(); i++) {
		const compiler::Occurrence &each = found[i];
		if (i == 0 || each.file != found[i - 1].file || each.candidate != found[i - 1].candidate) {
			file = tree->create_item(root);
			file->set_text(0, to_godot(each.file));
			file->set_custom_color(0, color * Color(1, 1, 1, 0.67));
			file->set_selectable(0, false);
		}
		TreeItem *item = tree->create_item(file);
		if (each.line > 0) {
			// First, as it clears the cell.
			item->set_cell_mode(0, TreeItem::CELL_MODE_CUSTOM);
			item->set_text(0, vformat("%3s: ", each.line) + to_godot(each.text).strip_edges(true, false));
			item->set_custom_draw_callback(0, callable_mp(this, &PetReferences::highlight));
		} else {
			item->set_text(0, place_in_scene(each.reference));
		}
		item->set_metadata(0, int64_t(i));
		if (each.candidate) {
			item->set_custom_color(0, color * Color(1, 1, 1, 0.5));
		}
	}
}

// Behind the name in its line, as Find in Files marks a match.
void PetReferences::highlight(Object *p_item, const Rect2 &p_rect) {
	const TreeItem *item = Object::cast_to<TreeItem>(p_item);
	const compiler::Occurrence &each = found[int64_t(item->get_metadata(0))];
	const String line = to_godot(each.text);
	const String shown = item->get_text(0);
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
// there; a resource at the property.
void PetReferences::open() {
	const compiler::Occurrence &each = found[int64_t(tree->get_selected()->get_metadata(0))];
	EditorInterface *editor = EditorInterface::get_singleton();
	const compiler::Reference &reference = each.reference;
	const String file = to_godot(each.file);
	const String property = to_godot(reference.property);
	if (each.line > 0) {
		editor->edit_script(ResourceLoader::get_singleton()->load(file), each.line, each.column);
		return;
	}
	const String extension = file.get_extension().to_lower();
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

void PetEditorPlugin::_enter_tree() {
	export_plugin.instantiate();
	add_export_plugin(export_plugin);
	Ref<InputEventKey> key;
	key.instantiate();
	key->set_keycode(KEY_F12);
	key->set_shift_pressed(true);
	key->set_alt_pressed(true);
	Ref<Shortcut> shortcut;
	shortcut.instantiate();
	shortcut->set_name("Find References");
	shortcut->set_events(Array::make(key));
	// So that users can change it; Godot 4.4 does not bind add_shortcut.
	if (ClassDBSingleton::get_singleton()->class_has_method("EditorSettings", "add_shortcut")) {
		EditorInterface::get_singleton()->get_editor_settings()->call("add_shortcut", "petrichor/find_references", shortcut);
	}
	code_menu.instantiate();
	code_menu->find_references = shortcut;
	code_menu->add_menu_shortcut(shortcut, callable_mp(this, &PetEditorPlugin::show_references));
	add_context_menu_plugin(EditorContextMenuPlugin::CONTEXT_SLOT_SCRIPT_EDITOR_CODE, code_menu);
	references = memnew(PetReferences);
	add_control_to_bottom_panel(references, "References");
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
	if (p_event->is_pressed() && !p_event->is_echo() && code_menu->find_references->matches_event(p_event) && script_editor->is_visible_in_tree() && current) {
		show_references(current->get_base_editor());
		get_viewport()->set_input_as_handled();
	}
}

void PetEditorPlugin::show_references(Object *p_code_edit) {
	const CodeEdit *code_edit = Object::cast_to<CodeEdit>(p_code_edit);
	const Ref<Script> script = EditorInterface::get_singleton()->get_script_editor()->get_current_script();
	if (!code_edit || !Object::cast_to<PetScript>(script.ptr())) {
		return;
	}
	const String text = code_edit->get_text();
	int64_t caret = 0;
	for (int line = 0; line < code_edit->get_caret_line(); line++) {
		caret = text.find("\n", caret) + 1;
	}
	const std::string before = to_utf8(text.substr(0, caret + code_edit->get_caret_column()));
	std::optional<std::vector<compiler::Occurrence>> found = find_references(script->get_path(), to_utf8(text), uint32_t(before.size()));
	if (!found) {
		EditorInterface::get_singleton()->get_editor_toaster()->push_toast("No declaration at the caret.", EditorToaster::SEVERITY_INFO);
		return;
	}
	references->set_found(std::move(*found));
	make_bottom_panel_item_visible(references);
}

void PetEditorPlugin::_exit_tree() {
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
		// As bytes, which keeps the file's line breaks. A byte order mark is no
		// text, as the build reads it (core/string/ustring.cpp:1756-1759, ed1daf0),
		// and is written back.
		const PackedByteArray bytes = FileAccess::get_file_as_bytes(path);
		std::string source = bytes.is_empty() ? std::string() : std::string(reinterpret_cast<const char *>(bytes.ptr()), size_t(bytes.size()));
		const std::string bom = source.rfind("\xEF\xBB\xBF", 0) == 0 ? "\xEF\xBB\xBF" : "";
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
		const Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
		if (file.is_null()) {
			failed++;
			ERR_PRINT(vformat("Cannot write %s.", path));
			continue;
		}
		const std::string text = bom + *result;
		PackedByteArray data;
		data.resize(int64_t(text.size()));
		if (!text.empty()) {
			std::memcpy(data.ptrw(), text.data(), text.size());
		}
		file->store_buffer(data);
	}
	UtilityFunctions::print(vformat("petrichor format: %d script(s), %d %s, %d with errors", int(paths.size()), changed, check ? "not formatted" : "formatted", failed));
	get_tree()->quit(failed > 0 || (check && changed > 0) ? 1 : 0);
}

} // namespace petrichor
