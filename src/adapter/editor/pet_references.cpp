#include "adapter/editor/pet_references.h"

#include "adapter/editor/scene_index.h"
#include "adapter/editor/script_compiler.h"
#include "compiler/ast.h"
#include "runtime/text.h"

#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_selection.hpp>
#include <godot_cpp/classes/editor_toaster.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/tree_item.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace godot;

namespace petrichor {

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
		EditorInterface::get_singleton()->get_editor_toaster()->push_toast("The project changed.");
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

} // namespace petrichor
