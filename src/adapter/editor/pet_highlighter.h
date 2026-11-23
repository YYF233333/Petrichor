#pragma once

#include <godot_cpp/classes/editor_syntax_highlighter.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace petrichor {

// Colors scripts in the script editor by the lexer's tokens, in the colors of
// the editor settings that GDScript's highlighter reads
// (modules/gdscript/editor/gdscript_highlighter.cpp:753-980; ed1daf0).
class PetHighlighter : public godot::EditorSyntaxHighlighter {
	GDCLASS(PetHighlighter, godot::EditorSyntaxHighlighter);

	godot::Color text, symbol, number, string, string_name, node_path, node_reference, annotation, comment, doc_comment, function, function_definition, global_function, member;
	// Keywords and type names, by their color where they are not members.
	std::unordered_map<std::string, godot::Color> names;

	// The text at `version` with the input method's, by line, and the colors
	// of each line from the columns they begin at.
	mutable int64_t version = -1;
	mutable godot::PackedStringArray lines;
	mutable std::vector<std::vector<std::pair<int, godot::Color>>> line_colors;

	void highlight() const;

protected:
	static void _bind_methods() {}

public:
	godot::String _get_name() const override;
	godot::PackedStringArray _get_supported_languages() const override;
	godot::Ref<godot::EditorSyntaxHighlighter> _create() const override;
	void _update_cache() override;
	godot::Dictionary _get_line_syntax_highlighting(int32_t p_line) const override;
};

} // namespace petrichor
