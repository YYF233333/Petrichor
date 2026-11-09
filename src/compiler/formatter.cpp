#include "compiler/formatter.h"

#include "compiler/lexer.h"
#include "compiler/parser.h"

#include <algorithm>
#include <climits>
#include <vector>

namespace petrichor::compiler {

namespace {

bool is_layout(TokenKind p_kind) {
	return p_kind == TokenKind::Newline || p_kind == TokenKind::Indent || p_kind == TokenKind::Dedent || p_kind == TokenKind::EndOfFile;
}

bool is_opening(TokenKind p_kind) {
	return p_kind == TokenKind::ParenOpen || p_kind == TokenKind::BracketOpen || p_kind == TokenKind::BraceOpen;
}

bool is_closing(TokenKind p_kind) {
	return p_kind == TokenKind::ParenClose || p_kind == TokenKind::BracketClose || p_kind == TokenKind::BraceClose;
}

// How the token changes the depth of brackets.
int nesting(TokenKind p_kind) {
	if (is_opening(p_kind)) {
		return 1;
	}
	return is_closing(p_kind) ? -1 : 0;
}

// Whether an operator after the token is binary, and a bracket after it calls
// or subscripts it.
bool ends_operand(TokenKind p_kind) {
	switch (p_kind) {
		case TokenKind::Identifier:
		case TokenKind::Int:
		case TokenKind::Float:
		case TokenKind::String:
		case TokenKind::StringName:
		case TokenKind::NodePath:
		case TokenKind::True:
		case TokenKind::False:
		case TokenKind::None:
		case TokenKind::Null:
		case TokenKind::Self:
		case TokenKind::Super:
		case TokenKind::ParenClose:
		case TokenKind::BracketClose:
		case TokenKind::BraceClose:
		case TokenKind::Question:
			return true;
		default:
			return false;
	}
}

bool is_unary(TokenKind p_kind) {
	return p_kind == TokenKind::Minus || p_kind == TokenKind::Plus || p_kind == TokenKind::Bang || p_kind == TokenKind::Tilde || p_kind == TokenKind::Percent;
}

// Names in a node path may be keywords, as the parser reads them.
bool is_node_name(TokenKind p_kind) {
	return p_kind == TokenKind::Identifier || (p_kind >= TokenKind::And && p_kind <= TokenKind::While);
}

std::string trimmed(std::string p_text) {
	while (!p_text.empty() && (p_text.back() == ' ' || p_text.back() == '\t')) {
		p_text.pop_back();
	}
	return p_text;
}

// A token or a piece of trivia, in the order of the text.
struct Item {
	const Token *token;
	int syntax; // Its index in SourceFile::tokens, or -1 for trivia.
};

enum class LineKind {
	Statement,
	Continuation, // Inside brackets, or after `\`.
	Comment,
};

struct Line {
	size_t first = 0; // Items.
	size_t last = 0;
	int width = 0; // The original indentation, in characters.
	int blanks = 0; // Blank lines before it, originally.
	LineKind kind = LineKind::Statement;
	int indent = 0;
	int level = 0; // The indentation of its block, which a comment line may exceed.
	bool in_brackets = false; // A comment line.
	bool annotations = false; // A statement of annotations alone.
};

// A block: the original indentation of its lines and the formatted one.
struct Level {
	int width;
	int indent;
	bool members; // A class's body, whose functions and classes are spaced.
};

// The block of a lambda inside brackets.
struct LambdaBlock {
	size_t brackets; // Open at its header.
	size_t levels; // Outside it.
	int base; // The indentation of the line its header ends on.
};

class Formatter {
	const SourceFile &file;
	const std::vector<Token> &tokens;
	std::vector<Item> items;
	std::vector<Line> lines;
	std::vector<int> token_line; // Of each syntax token other than layout.
	std::vector<bool> prefix; // A unary operator.
	std::vector<bool> joined; // Joined to the token before in a node path.
	std::vector<std::pair<size_t, int>> defs; // Lines of functions and classes, with the blank lines around them.

	// The walk over the tokens, which indents each line from those before it.
	size_t next = 0;
	std::vector<int> brackets; // The indentation of each open bracket's line.
	std::vector<LambdaBlock> lambdas;
	std::vector<Level> levels = { { 0, 0, true } };
	bool statement_start = true;
	int statement_indent = 0;
	int last_indent = 0; // Of the last line of code.
	bool header_class = false; // The last statement begins a class.
	size_t current = 0; // The line being laid out.
	uint32_t line_start = 0;
	int closed_base = INT_MAX; // A lambda's block ended at the start of the line.
	std::vector<size_t> comments; // Comment lines before the next line of code.
	std::vector<Level> comment_levels; // The blocks open at them.
	int comment_bracket = -1; // The indentation of the innermost bracket's line open at them.
	int unit = 4; // The original width of a level.

	// The width of the spaces and tabs before `p_offset`, a tab as wide as 4
	// spaces, so that a file without a block indents its comments by tabs.
	int width_before(uint32_t p_offset) const {
		int width = 0;
		for (uint32_t k = p_offset; k > 0 && (file.text[k - 1] == ' ' || file.text[k - 1] == '\t'); k--) {
			width += file.text[k - 1] == '\t' ? 4 : 1;
		}
		return width;
	}

	void collect() {
		size_t t = 0;
		size_t v = 0;
		for (;;) {
			while (t < tokens.size() && is_layout(tokens[t].kind)) {
				t++;
			}
			const bool token = t < tokens.size();
			const bool trivia = v < file.trivia.size();
			if (!token && !trivia) {
				break;
			}
			if (trivia && (!token || file.trivia[v].offset < tokens[t].offset)) {
				items.push_back({ &file.trivia[v++], -1 });
			} else {
				items.push_back({ &tokens[t], int(t) });
				t++;
			}
		}
		token_line.assign(tokens.size(), -1);
		for (size_t i = 0; i < items.size(); i++) {
			const uint32_t begin = items[i].token->offset;
			int breaks = 0;
			for (uint32_t k = i > 0 ? items[i - 1].token->end() : begin; k < begin; k++) {
				breaks += file.text[k] == '\n';
			}
			if (i == 0 || breaks > 0) {
				Line line;
				line.first = i;
				line.blanks = std::max(breaks - 1, 0);
				line.width = width_before(begin);
				if (items[i].syntax < 0 && items[i].token->kind == TokenKind::Comment) {
					line.kind = LineKind::Comment;
				}
				lines.push_back(line);
			}
			lines.back().last = i;
			if (items[i].syntax >= 0) {
				token_line[items[i].syntax] = int(lines.size() - 1);
			}
		}
		// From the first block, or 4 spaces, the editor's default.
		for (const Token &token : tokens) {
			if (token.kind == TokenKind::Indent) {
				unit = width_before(token.offset);
				break;
			}
		}
	}

	// A node path after `$` or `%` (parser.cpp, get_node()).
	void join_path(size_t p_start) {
		size_t i = p_start + 1;
		bool unique = tokens[p_start].kind == TokenKind::Percent;
		if (!unique && tokens[i].kind == TokenKind::Slash) {
			joined[i++] = true;
		}
		for (;;) {
			if (!unique && tokens[i].kind == TokenKind::Percent) {
				joined[i++] = true;
			}
			if (tokens[i].kind != TokenKind::String && !is_node_name(tokens[i].kind)) {
				return;
			}
			joined[i++] = true;
			unique = false;
			const TokenKind after = tokens[i + 1].kind;
			if (tokens[i].kind != TokenKind::Slash || (after != TokenKind::String && after != TokenKind::Percent && !is_node_name(after))) {
				return;
			}
			joined[i++] = true;
		}
	}

	void read_operators() {
		prefix.assign(tokens.size(), false);
		joined.assign(tokens.size(), false);
		int previous = -1;
		for (size_t t = 0; t < tokens.size(); t++) {
			const TokenKind kind = tokens[t].kind;
			if (kind == TokenKind::Newline) {
				previous = -1;
			}
			if (is_layout(kind)) {
				continue;
			}
			const bool after_operand = previous >= 0 && (ends_operand(tokens[previous].kind) || joined[previous]);
			prefix[t] = is_unary(kind) && !after_operand;
			// A keyword after "." names a member, as in `o.or(x)`.
			joined[t] = joined[t] || (previous >= 0 && tokens[previous].kind == TokenKind::Period && is_node_name(kind));
			if (!joined[t] && (kind == TokenKind::Dollar || (kind == TokenKind::Percent && prefix[t]))) {
				join_path(t);
			}
			previous = int(t);
		}
	}

	void take(size_t p_token) {
		const Token &token = tokens[p_token];
		switch (token.kind) {
			// Only the line being laid out begins with one.
			case TokenKind::Indent:
				if (in_brackets()) {
					lambdas.push_back({ brackets.size(), levels.size(), last_indent });
					levels.push_back({ lines[current].width, last_indent + 1, false });
				} else {
					levels.push_back({ lines[current].width, levels.back().indent + 1, header_class });
				}
				break;
			case TokenKind::Dedent:
				levels.pop_back();
				if (!lambdas.empty() && levels.size() == lambdas.back().levels) {
					if (token.offset == line_start) {
						// The line must stay no deeper than the header's, or the
						// block would take it.
						closed_base = std::min(closed_base, lambdas.back().base);
					}
					lambdas.pop_back();
				}
				break;
			case TokenKind::Newline:
				statement_start = true;
				break;
			case TokenKind::EndOfFile:
				break;
			default:
				statement_start = false;
				if (is_opening(token.kind)) {
					brackets.push_back(lines[token_line[p_token]].indent);
				} else if (is_closing(token.kind) && !brackets.empty()) {
					brackets.pop_back();
				}
				break;
		}
	}

	// Takes the tokens before `p_offset`, and the layout tokens at it when
	// `p_layout`: those of the line that begins there.
	void advance(uint32_t p_offset, bool p_layout) {
		while (next < tokens.size()) {
			const Token &token = tokens[next];
			if (token.offset > p_offset || (token.offset == p_offset && !(p_layout && is_layout(token.kind)))) {
				break;
			}
			take(next++);
		}
	}

	bool in_brackets() const {
		return brackets.size() > (lambdas.empty() ? 0 : lambdas.back().brackets);
	}

	// What a statement begins: functions and classes are spaced, a class's
	// body has members, annotations alone attach to what follows.
	void read_statement(size_t p_line) {
		Line &line = lines[p_line];
		size_t t = size_t(items[line.first].syntax);
		const size_t first = t;
		while (tokens[t].kind == TokenKind::At) {
			t += 2;
			if (tokens[t].kind == TokenKind::ParenOpen) {
				for (int depth = 0;; t++) {
					depth += nesting(tokens[t].kind);
					if (depth == 0) {
						t++;
						break;
					}
				}
			}
		}
		line.annotations = t > first && (tokens[t].kind == TokenKind::Newline || tokens[t].kind == TokenKind::EndOfFile);
		while (tokens[t].kind == TokenKind::Pub || tokens[t].kind == TokenKind::Static || tokens[t].kind == TokenKind::Async) {
			t++;
		}
		const TokenKind kind = tokens[t].kind;
		header_class = kind == TokenKind::Class || kind == TokenKind::Struct || kind == TokenKind::Interface;
		bool spaced = header_class;
		if (kind == TokenKind::Func) {
			// A method without a body has no colon outside its brackets.
			int depth = 0;
			for (size_t u = t + 1; tokens[u].kind != TokenKind::Newline && tokens[u].kind != TokenKind::EndOfFile; u++) {
				depth += nesting(tokens[u].kind);
				if (depth == 0 && tokens[u].kind == TokenKind::Colon) {
					spaced = true;
					break;
				}
			}
		}
		if (spaced && levels.size() == 1) {
			defs.push_back({ p_line, 2 });
		} else if (spaced && levels.back().members) {
			defs.push_back({ p_line, 1 });
		}
	}

	// Comment lines are indented as their block, keeping the levels they have
	// beyond it, as code the editor comments out does. Before the first line
	// of a block they are in it; where several blocks end, each belongs to the
	// one its original column falls in, but to none shallower than the next
	// comment's (architecture §2.1).
	void place_comments(const Line *p_next) {
		if (comments.empty()) {
			return;
		}
		const auto place = [&](size_t p_line, const Level &p_level) {
			Line &line = lines[p_line];
			line.level = p_level.indent;
			line.indent = p_level.indent + (line.width > p_level.width ? (line.width - p_level.width) / unit : 0);
		};
		const Level own = p_next ? Level{ p_next->width, p_next->indent, false } : Level{ 0, 0, false };
		if (p_next && levels.size() > comment_levels.size()) {
			for (size_t line : comments) {
				place(line, own);
			}
		} else if (comment_bracket >= 0) {
			// Continuation lines keep no original indentation to go by.
			for (size_t line : comments) {
				lines[line].indent = lines[line].level = comment_bracket + 1;
			}
		} else {
			std::vector<Level> candidates = { own };
			candidates.insert(candidates.end(), comment_levels.begin() + std::ptrdiff_t(levels.size()), comment_levels.end());
			std::vector<size_t> chosen(comments.size(), 0);
			for (size_t i = comments.size(); i-- > 0;) {
				for (size_t c = 1; c < candidates.size(); c++) {
					if (candidates[c].width <= lines[comments[i]].width) {
						chosen[i] = c;
					}
				}
				if (i + 1 < comments.size()) {
					chosen[i] = std::max(chosen[i], chosen[i + 1]);
				}
				place(comments[i], candidates[chosen[i]]);
			}
		}
		comments.clear();
	}

	void indent_lines() {
		for (size_t l = 0; l < lines.size(); l++) {
			Line &line = lines[l];
			current = l;
			line_start = items[line.first].token->offset;
			if (line.kind == LineKind::Comment) {
				// No token comes between comment lines.
				if (comments.empty()) {
					advance(line_start, false);
					comment_levels = levels;
					comment_bracket = in_brackets() ? brackets.back() : -1;
				}
				line.in_brackets = comment_bracket >= 0;
				comments.push_back(l);
				continue;
			}
			closed_base = INT_MAX;
			advance(line_start, true);
			if (in_brackets()) {
				line.kind = LineKind::Continuation;
				const int indent = brackets.back() + (is_closing(items[line.first].token->kind) ? 0 : 1);
				line.indent = std::min(indent, closed_base);
			} else if (statement_start) {
				line.indent = levels.back().indent;
				statement_indent = line.indent;
				if (items[line.first].syntax >= 0) { // Not a line of only `\`, before the statement's first token.
					read_statement(l);
				}
			} else {
				line.kind = LineKind::Continuation;
				line.indent = statement_indent + 1;
			}
			line.level = line.indent;
			last_indent = line.indent;
			place_comments(&line);
		}
		advance(UINT32_MAX, true);
		place_comments(nullptr);
	}

	static bool attached(const Line &p_line, int p_level) {
		return p_line.level == p_level && ((p_line.kind == LineKind::Comment && !p_line.in_brackets) || (p_line.kind == LineKind::Statement && p_line.annotations));
	}

	static bool ends_block(const Line &p_line, int p_level) {
		return p_line.level <= p_level && (p_line.kind == LineKind::Statement || (p_line.kind == LineKind::Comment && !p_line.in_brackets));
	}

	// The blank lines before each line.
	std::vector<int> space_lines() const {
		std::vector<int> required(lines.size(), -1);
		for (const auto &[at, blanks] : defs) {
			const int indent = lines[at].indent;
			size_t start = at;
			while (start > 0 && lines[start].blanks == 0 && attached(lines[start - 1], indent)) {
				start--;
			}
			// Not after the header of its block.
			if (start > 0 && lines[start - 1].level >= indent) {
				required[start] = std::max(required[start], blanks);
			}
			size_t end = at + 1;
			while (end < lines.size() && !ends_block(lines[end], indent)) {
				end++;
			}
			if (end < lines.size() && lines[end].level == indent) {
				required[end] = std::max(required[end], blanks);
			}
		}
		std::vector<int> result(lines.size(), 0);
		for (size_t l = 1; l < lines.size(); l++) {
			result[l] = required[l] >= 0 ? required[l] : std::min(lines[l].blanks, 1);
		}
		return result;
	}

	int space(size_t p_item) const {
		const Item &a = items[p_item - 1];
		const Item &b = items[p_item];
		if (b.syntax < 0) {
			return b.token->kind == TokenKind::Comment ? 2 : 1;
		}
		if (a.syntax < 0) {
			return 1;
		}
		const size_t ta = size_t(a.syntax);
		const size_t tb = size_t(b.syntax);
		const TokenKind left = tokens[ta].kind;
		const TokenKind right = tokens[tb].kind;
		if (is_opening(left) || is_closing(right) || right == TokenKind::Comma || right == TokenKind::Semicolon || left == TokenKind::Period || right == TokenKind::Period || left == TokenKind::Ellipsis || right == TokenKind::Question || left == TokenKind::At || left == TokenKind::Dollar || joined[tb] || prefix[ta]) {
			return 0;
		}
		if (right == TokenKind::Colon) {
			// `x := 1` infers a type.
			return tokens[tb + 1].kind == TokenKind::Equal ? 1 : 0;
		}
		if (left == TokenKind::Colon && right == TokenKind::Equal) {
			return 0;
		}
		if ((right == TokenKind::ParenOpen || right == TokenKind::BracketOpen) && (ends_operand(left) || joined[ta] || left == TokenKind::Func || left == TokenKind::Assert)) {
			return 0;
		}
		return 1;
	}

	std::string text_of(const Item &p_item) const {
		const std::string text = file.text.substr(p_item.token->offset, p_item.token->length);
		return p_item.token->kind == TokenKind::Comment ? trimmed(text) : text;
	}

public:
	explicit Formatter(const SourceFile &p_file) :
			file(p_file), tokens(p_file.tokens) {}

	std::string run() {
		collect();
		read_operators();
		indent_lines();
		const std::vector<int> blanks = space_lines();
		const size_t first_break = file.text.find('\n');
		const std::string newline = first_break != std::string::npos && first_break > 0 && file.text[first_break - 1] == '\r' ? "\r\n" : "\n";
		std::string result;
		for (size_t l = 0; l < lines.size(); l++) {
			const Line &line = lines[l];
			for (int k = 0; l > 0 && k <= blanks[l]; k++) {
				result += newline;
			}
			result.append(size_t(line.indent), '\t');
			for (size_t i = line.first; i <= line.last; i++) {
				if (i > line.first) {
					result.append(size_t(space(i)), ' ');
				}
				result += text_of(items[i]);
			}
		}
		if (!lines.empty()) {
			result += newline;
		}
		return result;
	}

	// Whether the text lexes to the same tokens, with the same comments.
	bool same_code(const std::string &p_text, std::string *r_error) const {
		std::vector<Diagnostic> errors;
		const std::shared_ptr<const SourceFile> other = tokenize(p_text, errors);
		uint32_t differs = UINT32_MAX;
		const auto same = [&](const Token &p_a, const Token &p_b, bool p_trim) {
			if (p_a.kind != p_b.kind) {
				return false;
			}
			const std::string a = file.text.substr(p_a.offset, p_a.length);
			const std::string b = p_text.substr(p_b.offset, p_b.length);
			return p_trim ? trimmed(a) == trimmed(b) : a == b;
		};
		for (size_t i = 0; i < tokens.size() && differs == UINT32_MAX; i++) {
			if (i >= other->tokens.size() || !same(tokens[i], other->tokens[i], false)) {
				differs = tokens[i].offset;
			}
		}
		for (size_t i = 0; i < file.trivia.size() && differs == UINT32_MAX; i++) {
			if (i >= other->trivia.size() || !same(file.trivia[i], other->trivia[i], true)) {
				differs = file.trivia[i].offset;
			}
		}
		if (differs == UINT32_MAX && errors.empty() && other->tokens.size() == tokens.size() && other->trivia.size() == file.trivia.size()) {
			return true;
		}
		if (r_error) {
			const uint32_t at = differs == UINT32_MAX ? uint32_t(file.text.size()) : differs;
			*r_error = "Formatting would change the code at line " + std::to_string(file.position(at).line) + ", so it is left as it is.";
		}
		return false;
	}
};

} // namespace

std::optional<std::string> format(const std::string &p_source, std::string *r_error) {
	std::vector<Diagnostic> errors;
	const ClassDecl parsed = parse(p_source, errors);
	if (!errors.empty()) {
		if (r_error) {
			*r_error = "The file has syntax errors.";
		}
		return std::nullopt;
	}
	Formatter formatter(*parsed.source);
	std::string text = formatter.run();
	if (!formatter.same_code(text, r_error)) {
		return std::nullopt;
	}
	return text;
}

} // namespace petrichor::compiler
