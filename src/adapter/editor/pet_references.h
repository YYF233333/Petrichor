#pragma once

#include "compiler/diagnostic.h"
#include "compiler/rename.h"

#include <godot_cpp/classes/h_box_container.hpp>
#include <godot_cpp/classes/tree.hpp>
#include <godot_cpp/classes/v_box_container.hpp>

#include <cstdint>
#include <vector>

namespace petrichor {

// The places that finding references lists, in the bottom panel by file, as
// the results of Find in Files show (editor/script/find_in_files.cpp:791-924;
// ed1daf0). Selecting one opens it. It also previews the changes of a rename,
// or shows why it cannot rename.
class PetReferences : public godot::VBoxContainer {
	GDCLASS(PetReferences, godot::VBoxContainer);

	enum class Shown {
		References,
		Changes,
		Refusals, // Their messages are the texts.
	};

	godot::Tree *tree = nullptr;
	godot::HBoxContainer *actions = nullptr;
	Shown shown = Shown::References;
	std::vector<compiler::Occurrence> found;
	uint64_t searched = 0; // The workspace's revision then.
	std::vector<bool> ticked; // Of the changes.
	godot::Callable apply;

	void fill();
	void open();
	void go(size_t p_index);
	void search_again(int64_t p_index, uint64_t p_searched);
	void tick();
	void finish(bool p_apply);
	void highlight(godot::Object *p_item, const godot::Rect2 &p_rect);

protected:
	static void _bind_methods() {}
	void _notification(int p_what);

public:
	PetReferences();
	void set_found(std::vector<compiler::Occurrence> p_found);
	// Each with a check box: candidates start unchecked, and the rest cannot
	// be unchecked. Apply calls `p_apply`.
	void set_changes(const std::vector<compiler::RenameEdit> &p_changes, const godot::Callable &p_apply);
	void set_refusals(const std::vector<compiler::Diagnostic> &p_refusals);
	const std::vector<bool> &checked() const { return ticked; }
};

} // namespace petrichor
