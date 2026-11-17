#include "compiler/navigation.h"

#include "compiler/ast.h"
#include "compiler/builtin_api.h"
#include "compiler/checker.h"
#include "compiler/lexer.h"
#include "compiler/parser.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <map>
#include <set>
#include <tuple>

namespace petrichor::compiler {

namespace {

using Kind = SymbolKind;

// The reference whose name holds byte `p_offset` or ends at it.
const Reference *reference_at(const CompileResult &p_result, uint32_t p_offset) {
	for (const Reference &reference : p_result.references) {
		if (reference.begin <= p_offset && p_offset <= reference.end) {
			return &reference;
		}
	}
	return nullptr;
}

const Reference *declaration_of(const std::vector<Reference> &p_references, const DeclId &p_target) {
	for (const Reference &reference : p_references) {
		if (reference.declares && reference.target == p_target) {
			return &reference;
		}
	}
	return nullptr;
}

// Where the documentation files a declaration of the engine: under its class,
// or under @GlobalScope for the global enums, as "Key" and "Variant.Type".
std::optional<Lookup> engine_lookup(const EngineApi &p_engine, const DeclId &p_target) {
	const std::string &owner = p_target.scope;
	const std::string &name = p_target.name;
	Lookup found;
	found.class_name = owner;
	found.member = name;
	const builtin::Class *type = builtin::find_class(owner);
	if (builtin::find_enum(name.empty() ? owner : owner + "." + name)) {
		found.kind = Kind::Enum;
		if (name.empty() || !type) {
			found.class_name = "@GlobalScope";
			found.member = name.empty() ? owner : owner + "." + name;
		}
	} else if (name.empty()) {
		found.kind = Kind::Class;
	} else if (owner == "@GlobalScope") {
		found.kind = builtin::find_utility(name) ? Kind::Method : Kind::Constant;
	} else if (owner == "@GDScript") {
		found.kind = name == "PI" || name == "TAU" || name == "INF" || name == "NAN" ? Kind::Constant : Kind::Method;
	} else if (type && builtin::find_member(*type, name)) {
		found.kind = Kind::Property;
	} else if (type && builtin::find_method(*type, name)) {
		found.kind = Kind::Method;
	} else if (type) {
		// An enum's values are constants of the type to the documentation.
		found.kind = Kind::Constant;
	} else if (p_engine.find_signal(owner, name)) {
		found.kind = Kind::Signal;
	} else if (p_engine.find_property(owner, name)) {
		found.kind = Kind::Property;
	} else if (p_engine.find_method(owner, name)) {
		found.kind = Kind::Method;
	} else if (p_engine.find_constant(owner, name)) {
		found.kind = Kind::Constant;
	} else if (p_engine.find_enum(owner, name)) {
		found.kind = Kind::Enum;
	} else {
		return std::nullopt;
	}
	return found;
}

bool is_name_char(char p_char) {
	return std::isalnum(static_cast<unsigned char>(p_char)) || p_char == '_';
}

// A member a GDScript file declares outside its inner classes, by its text:
// a line that is not indented and, after annotations and `static`, declares
// the name, as `@export var speed := 2.0` does.
std::optional<Lookup> gdscript_declaration(const std::string &p_source, const std::string &p_name) {
	static const std::pair<const char *, Kind> KEYWORDS[] = {
		{ "func", Kind::Method },
		{ "var", Kind::Property },
		{ "const", Kind::Constant },
		{ "signal", Kind::Signal },
		{ "enum", Kind::Enum },
		{ "class", Kind::Class },
	};
	const auto word = [&](size_t &r_at) {
		const size_t first = r_at;
		while (r_at < p_source.size() && is_name_char(p_source[r_at])) {
			r_at++;
		}
		return p_source.substr(first, r_at - first);
	};
	const auto blanks = [&](size_t &r_at) {
		while (r_at < p_source.size() && (p_source[r_at] == ' ' || p_source[r_at] == '\t')) {
			r_at++;
		}
	};
	int line = 1;
	for (size_t start = 0; start < p_source.size(); line++) {
		const size_t end = std::min(p_source.find('\n', start), p_source.size());
		size_t at = start;
		while (at < end && p_source[at] == '@') {
			at++;
			word(at);
			for (int depth = 0; at < end && (depth > 0 || p_source[at] == '('); at++) {
				if (p_source[at] == '(') {
					depth++;
				} else if (p_source[at] == ')') {
					depth--;
				}
			}
			blanks(at);
		}
		std::string keyword = word(at);
		if (keyword == "static") {
			blanks(at);
			keyword = word(at);
		}
		blanks(at);
		for (const auto &[spelling, kind] : KEYWORDS) {
			if (keyword == spelling && word(at) == p_name) {
				Lookup found;
				found.kind = kind;
				found.line = line;
				return found;
			}
		}
		start = end + 1;
	}
	return std::nullopt;
}

bool ends_with(const std::string &p_text, const std::string &p_end) {
	return p_text.size() >= p_end.size() && p_text.compare(p_text.size() - p_end.size(), p_end.size(), p_end) == 0;
}

std::string strip(const std::string &p_text) {
	const auto blank = [](char p_char) { return static_cast<unsigned char>(p_char) <= ' '; };
	const auto first = std::find_if_not(p_text.begin(), p_text.end(), blank);
	const auto last = std::find_if_not(p_text.rbegin(), p_text.rend(), blank).base();
	return first < last ? std::string(first, last) : std::string();
}

enum class DocLine {
	Normal,
	Code,
	Codeblock,
	Kbd,
};

// A line of a `##` comment added to `r_text`, as GDScript adds it
// (modules/gdscript/gdscript_parser.cpp:3938-4064): lines are joined by a
// space, `[br][br]` makes a paragraph, and a code block keeps its lines,
// without the spaces that began the comment's first line.
void add_doc_line(std::string p_line, std::string &r_text, const std::string &p_indent, DocLine &r_state) {
	if (r_state == DocLine::Normal) {
		p_line.erase(0, p_line.find_first_not_of(" \t"));
	} else if (p_line.rfind(p_indent, 0) == 0) {
		p_line.erase(0, p_indent.size());
	}
	std::string join;
	if (!r_text.empty()) {
		if (r_state != DocLine::Normal || ends_with(r_text, "[/codeblock]")) {
			join = "\n";
		} else if (ends_with(r_text, "[br]")) {
			r_text.resize(r_text.size() - 4);
			p_line = "[br]" + p_line;
		} else if (r_text.back() != '\n') {
			join = " ";
		}
	}
	std::string result;
	size_t from = 0;
	size_t start = 0;
	while (true) {
		if (r_state == DocLine::Normal) {
			const size_t open = p_line.find('[', from);
			const size_t close = open == std::string::npos ? open : p_line.find(']', open + 1);
			if (close == std::string::npos) {
				break;
			}
			from = close + 1;
			const std::string tag = p_line.substr(open + 1, close - open - 1);
			if (tag == "br" && p_line.compare(from, 4, "[br]") == 0) {
				result += p_line.substr(start, open - start) + '\n';
				from += 4;
				start = from;
			} else if (tag == "code" || tag.rfind("code ", 0) == 0) {
				r_state = DocLine::Code;
			} else if (tag == "codeblock" || tag.rfind("codeblock ", 0) == 0) {
				if (open == 0) {
					join = "\n";
				} else {
					result += p_line.substr(start, open - start) + '\n';
				}
				result += "[" + tag + "]" + (from < p_line.size() ? "\n" : "");
				r_state = DocLine::Codeblock;
				start = from;
			} else if (tag == "kbd") {
				r_state = DocLine::Kbd;
			}
			continue;
		}
		static const char *const ENDS[] = { "", "[/code]", "[/codeblock]", "[/kbd]" };
		const std::string end = ENDS[int(r_state)];
		const size_t at = p_line.find(end, from);
		if (at == std::string::npos) {
			break;
		}
		from = at + end.size();
		if (r_state == DocLine::Codeblock) {
			if (at == 0) {
				join = "\n";
			} else {
				result += p_line.substr(start, at - start) + '\n';
			}
			result += end + (from < p_line.size() ? "\n" : "");
			start = from;
		}
		r_state = DocLine::Normal;
	}
	result += p_line.substr(start);
	if (r_state == DocLine::Normal) {
		result.erase(result.find_last_not_of(" \t") + 1);
	}
	r_text += join + result;
}

// `@tutorial: link` or `@tutorial(title): link`, which a class's comment
// lists; a line that begins with @tutorial but is not one is left out.
bool tutorial(const std::string &p_line, DocComment &r_comment) {
	if (p_line.rfind("@tutorial", 0) != 0) {
		return false;
	}
	size_t open = 9;
	if (open < p_line.size() && p_line[open] == ':') {
		r_comment.tutorials.emplace_back(std::string(), strip(p_line.substr(open + 1)));
		return true;
	}
	open = std::min(p_line.find_first_not_of(" \t", open), p_line.size());
	const size_t close = open < p_line.size() && p_line[open] == '(' ? p_line.find(')', open) : std::string::npos;
	const size_t colon = close == std::string::npos ? close : p_line.find_first_not_of(" \t", close + 1);
	if (colon != std::string::npos && p_line[colon] == ':') {
		r_comment.tutorials.emplace_back(strip(p_line.substr(open + 1, close - open - 1)), strip(p_line.substr(colon + 1)));
	}
	return true;
}

// `@deprecated` or `@experimental`, and the message after a colon.
bool tag(const std::string &p_line, const std::string &p_tag, std::optional<std::string> &r_message) {
	if (p_line != p_tag && p_line.rfind(p_tag + ":", 0) != 0) {
		return false;
	}
	r_message = strip(p_line.substr(std::min(p_tag.size() + 1, p_line.size())));
	return true;
}

// The comment of lines `p_lines`, each after its `##`, as GDScript reads it
// (modules/gdscript/gdscript_parser.cpp:4077-4243): a blank line ends a
// class's brief, and tags take lines of their own outside code.
DocComment parse_comment(const std::vector<std::string> &p_lines, bool p_class) {
	DocComment comment;
	if (p_lines.empty()) {
		return comment;
	}
	const std::string &first = p_lines[0];
	const std::string indent(std::min(first.find_first_not_of(' '), first.size()), ' ');
	DocLine state = DocLine::Normal;
	bool brief = p_class;
	for (const std::string &line : p_lines) {
		if (state == DocLine::Normal) {
			const std::string stripped = strip(line);
			if (brief && !comment.brief.empty() && stripped.empty()) {
				brief = false;
				continue;
			}
			if ((p_class && tutorial(stripped, comment)) || tag(stripped, "@deprecated", comment.deprecated) || tag(stripped, "@experimental", comment.experimental)) {
				continue;
			}
		}
		add_doc_line(line, brief ? comment.brief : comment.description, indent, state);
	}
	return comment;
}

// The `##` comments of a file, which document the declarations they are on
// or directly above (spec §2.2), found as GDScript finds them
// (modules/gdscript/gdscript_parser.cpp:842-862, 1068-1101, 1655-1679,
// 2195-2228).
class DocComments {
	struct Line {
		std::string text; // After the `##`.
		bool alone = false; // Without code before it.
	};

	const SourceFile &source;
	std::map<int, Line> lines;

	bool alone(int p_line) const {
		const auto at = lines.find(p_line);
		return at != lines.end() && at->second.alone;
	}

	std::vector<std::string> block(int p_last) const {
		int first = p_last + 1;
		while (alone(first - 1)) {
			first--;
		}
		std::vector<std::string> result;
		for (int line = first; line <= p_last; line++) {
			result.push_back(lines.at(line).text);
		}
		return result;
	}

public:
	explicit DocComments(const SourceFile &p_source) :
			source(p_source) {
		for (const Token &comment : p_source.trivia) {
			if (comment.kind != TokenKind::Comment || p_source.text.compare(comment.offset, 2, "##") != 0 || p_source.text.compare(comment.offset, 3, "###") == 0) {
				continue;
			}
			const int line = p_source.position(comment.offset).line;
			const uint32_t start = p_source.line_starts[line - 1];
			lines[line] = { p_source.text.substr(comment.offset + 2, comment.length - 2), p_source.text.find_first_not_of(" \t", start) == comment.offset };
		}
	}

	// Of the declaration from token `p_first`, its annotations included, whose
	// name is token `p_name`: the comment after code on the name's line, which
	// documents nothing when `p_inline` is false, else the block of lines
	// directly above.
	DocComment of(uint32_t p_first, uint32_t p_name, bool p_class, bool p_inline = true) const {
		const auto on_line = lines.find(source.line(p_name));
		if (on_line != lines.end()) {
			return p_inline ? parse_comment({ on_line->second.text }, p_class) : DocComment();
		}
		return parse_comment(block(source.line(p_first) - 1), p_class);
	}

	// Of the file's class, whose first member, its annotations included, is
	// token `p_member`: the first block above it, if not directly above it.
	DocComment of_file(uint32_t p_member) const {
		const int member = p_member == NO_TOKEN ? INT_MAX : source.line(p_member);
		for (const auto &[line, comment] : lines) {
			if (line >= member) {
				break;
			}
			if (comment.alone) {
				int last = line;
				while (alone(last + 1)) {
					last++;
				}
				return last + 1 == member ? DocComment() : parse_comment(block(last), true);
			}
		}
		return DocComment();
	}

	// Of the local whose name is token `p_name`, declared by a statement with
	// the annotations on the lines above it; none for a parameter.
	DocComment of_local(uint32_t p_name) const {
		const std::vector<Token> &tokens = source.tokens;
		const auto starts_line = [&](uint32_t p_token) {
			return p_token == 0 || tokens[p_token - 1].kind == TokenKind::Newline || tokens[p_token - 1].kind == TokenKind::Indent || tokens[p_token - 1].kind == TokenKind::Dedent;
		};
		uint32_t first = p_name - 1;
		if (p_name == 0 || (tokens[first].kind != TokenKind::Var && tokens[first].kind != TokenKind::Const) || !starts_line(first)) {
			return DocComment();
		}
		while (first > 0 && tokens[first - 1].kind == TokenKind::Newline) {
			uint32_t start = first - 1;
			while (!starts_line(start)) {
				start--;
			}
			if (tokens[start].kind != TokenKind::At) {
				break;
			}
			first = start;
		}
		return of(first, p_name, false);
	}
};

} // namespace

std::string doc_class_name(const DocName &p_doc_name, const std::string &p_path) {
	std::string name = p_doc_name(script_file(p_path));
	for (size_t at = p_path.find("::"); at != std::string::npos;) {
		const size_t next = p_path.find("::", at + 2);
		name += "." + p_path.substr(at + 2, next == std::string::npos ? next : next - at - 2);
		at = next;
	}
	return name;
}

Completion complete(Analysis &r_analysis, const std::string &p_path, const std::string &p_source) {
	const Analysis::Pass pass(r_analysis);
	ScriptClasses &classes = r_analysis.pass_classes();
	CompileOptions options;
	options.path = p_path;
	options.project = classes.get_project();
	options.mode = CheckMode::Cursor;
	std::vector<Diagnostic> errors;
	// An annotation's name at the cursor takes those the parser accepts where
	// it is, and at a member those of the @export family.
	const size_t cursor = p_source.find("\xEF\xBF\xBF");
	std::optional<AnnotationPlace> annotation;
	ClassDecl decl = parse(p_source, errors, false, [&](const Token &p_name, AnnotationPlace p_place) {
		if (p_name.offset <= cursor && cursor < p_name.end()) {
			annotation = p_place;
		}
	});
	Completion completion;
	if (annotation) {
		std::vector<std::pair<std::string, bool>> names = annotation_names(*annotation);
		if (*annotation == AnnotationPlace::Header || *annotation == AnnotationPlace::Member) {
			const std::vector<std::pair<std::string, bool>> exports = export_annotations();
			names.insert(names.end(), exports.begin(), exports.end());
		}
		std::map<std::string, CompletionItem> found;
		for (const auto &[name, arguments] : names) {
			CompletionItem item;
			item.kind = CompletionKind::PlainText;
			item.name = name;
			item.insert = name + (arguments ? "(" : "");
			found.emplace(name, item);
		}
		for (auto &entry : found) {
			completion.items.push_back(std::move(entry.second));
		}
		return completion;
	}
	std::vector<Reference> references;
	classes.begin_compile(p_path, p_source);
	check(decl, r_analysis.engine_api(), options, errors, classes, &references, nullptr, &completion);
	classes.end_compile();
	return completion;
}

std::optional<Lookup> lookup(Analysis &r_analysis, const DocName &p_doc_name, const std::string &p_path, const std::string &p_source, uint32_t p_offset) {
	const ProjectApi &project = r_analysis.project_api();
	const std::shared_ptr<const CompileResult> result = r_analysis.compile(p_path, p_source);
	const Reference *at = reference_at(*result, p_offset);
	if (!at) {
		return std::nullopt;
	}
	DeclId target = at->target;
	if (target.kind == DeclId::Kind::Autoload) {
		// Its script's class; the editor opens a scene itself
		// (editor/script/script_text_editor.cpp:1292-1297).
		const std::string path = project.autoload_path(target.name);
		const bool own = is_script_path(path);
		if (!own && !project.foreign_class(path, false)) {
			return std::nullopt;
		}
		target = { own ? DeclId::Kind::Script : DeclId::Kind::Foreign, path, std::string() };
	}
	switch (target.kind) {
		case DeclId::Kind::Local: {
			if (target.name.empty()) {
				return std::nullopt;
			}
			const Reference *declaration = declaration_of(result->references, target);
			const uint32_t offset = uint32_t(std::stoul(target.name));
			Lookup found;
			found.kind = declaration ? *declaration->declares : Kind::Local;
			found.type = declaration ? declaration->type : std::string();
			std::vector<Diagnostic> errors;
			const std::shared_ptr<const SourceFile> file = tokenize(p_source, errors);
			const auto name = std::lower_bound(file->tokens.begin(), file->tokens.end(), offset, [](const Token &p_token, uint32_t p_offset) {
				return p_token.offset < p_offset;
			});
			if (name != file->tokens.end() && name->offset == offset) {
				found.comment = DocComments(*file).of_local(uint32_t(name - file->tokens.begin()));
			}
			found.path = p_path;
			found.line = file->position(offset).line;
			return found;
		}
		case DeclId::Kind::Script: {
			const std::string file = script_file(target.scope);
			const std::optional<std::string> source = file == p_path ? p_source : project.script_source(file);
			if (!source) {
				return std::nullopt;
			}
			// Each file records its own declarations.
			const std::shared_ptr<const CompileResult> declared_in = file == p_path ? result : r_analysis.compile(file, *source);
			const Reference *declaration = declaration_of(declared_in->references, target);
			if (!declaration && !target.name.empty()) {
				return std::nullopt;
			}
			Lookup found;
			found.kind = declaration ? *declaration->declares : Kind::Class;
			found.class_name = doc_class_name(p_doc_name, target.scope);
			found.member = target.name;
			// A field of a struct or a member of an interface is in that one's
			// documentation, a value of an enum in the class's.
			const size_t dot = found.member.find('.');
			if (dot != std::string::npos) {
				if (found.kind != Kind::Constant) {
					found.class_name += "." + found.member.substr(0, dot);
				}
				found.member = found.member.substr(dot + 1);
			}
			if (found.kind == Kind::Class && !found.member.empty()) {
				found.class_name += "." + found.member;
				found.member.clear();
			}
			found.path = file;
			found.line = declaration ? position_in(*source, line_starts_of(*source), declaration->begin).line : 1;
			return found;
		}
		case DeclId::Kind::Foreign: {
			const std::optional<std::string> source = target.scope.empty() ? std::nullopt : project.script_source(target.scope);
			if (!source) {
				return std::nullopt;
			}
			std::optional<Lookup> found;
			if (target.name.empty()) {
				found.emplace();
				found->kind = Kind::Class;
				found->line = 1;
			} else {
				found = gdscript_declaration(*source, target.name);
			}
			if (found) {
				found->class_name = p_doc_name(target.scope);
				found->member = target.name;
				found->path = target.scope;
			}
			return found;
		}
		case DeclId::Kind::Engine:
			return engine_lookup(r_analysis.engine_api(), target);
		default:
			return std::nullopt;
	}
}

namespace {

// The whole words `p_name` in `p_text`, by offset.
std::vector<uint32_t> whole_words(const std::string &p_text, const std::string &p_name) {
	const auto in_name = [&](size_t p_at) {
		return p_at < p_text.size() && (is_name_char(p_text[p_at]) || static_cast<unsigned char>(p_text[p_at]) >= 0x80);
	};
	std::vector<uint32_t> found;
	for (size_t at = p_text.find(p_name); at != std::string::npos; at = p_text.find(p_name, at + 1)) {
		if ((at == 0 || !in_name(at - 1)) && !in_name(at + p_name.size())) {
			found.push_back(uint32_t(at));
		}
	}
	return found;
}

// The lines of a script, to place names in.
class Lines {
	const std::string &text;
	const std::vector<uint32_t> starts;

public:
	explicit Lines(const std::string &p_text) :
			text(p_text), starts(line_starts_of(p_text)) {}

	// A name does not go on to another line.
	Occurrence at(const std::string &p_file, const Reference &p_reference) const {
		Occurrence found;
		found.file = p_file;
		found.reference = p_reference;
		const SourcePosition begin = position_in(text, starts, p_reference.begin);
		found.line = begin.line;
		found.column = begin.column;
		found.end_column = position_in(text, starts, p_reference.end).column;
		const uint32_t start = starts[found.line - 1];
		const size_t end = size_t(found.line) < starts.size() ? starts[found.line] - 1 : text.size();
		found.text = text.substr(start, end - start);
		if (!found.text.empty() && found.text.back() == '\r') {
			found.text.pop_back();
		}
		return found;
	}

	// The whole words `p_name`, which nothing resolves.
	std::vector<Occurrence> words(const std::string &p_file, const std::string &p_name) const {
		std::vector<Occurrence> found;
		for (const uint32_t offset : whole_words(text, p_name)) {
			Reference word;
			word.target = { DeclId::Kind::Unresolved, std::string(), p_name };
			word.begin = offset;
			word.end = offset + uint32_t(p_name.size());
			found.push_back(at(p_file, word));
		}
		return found;
	}
};

struct Script {
	std::string path;
	std::string source;
	std::shared_ptr<const CompileResult> result;
};

// Whether the engine finds a declaration of this language by name, as other
// languages and dynamic access do (spec §10.5): a pub member, a file's class
// by its global name, a pub inner class, and a field, a value or a member of
// a pub struct, enum or interface.
bool reflected(ScriptClasses &r_classes, const DeclId &p_id) {
	const ClassDecl *cls = r_classes.header(p_id.scope);
	if (!cls || p_id.name.empty()) {
		return cls && (cls->name.empty() || cls->is_pub);
	}
	const std::string head = p_id.name.substr(0, p_id.name.find('.'));
	const auto pub = [&](const auto &p_decls) {
		return std::any_of(p_decls.begin(), p_decls.end(), [&](const auto &p_decl) { return p_decl.name == head && p_decl.is_pub; });
	};
	const bool unnamed_value = std::any_of(cls->enums.begin(), cls->enums.end(), [&](const EnumDecl &p_enum) {
		return p_enum.name.empty() && p_enum.is_pub && std::any_of(p_enum.values.begin(), p_enum.values.end(), [&](const EnumValueDecl &p_value) { return p_value.name == head; });
	});
	return unnamed_value || pub(cls->members) || pub(cls->static_vars) || pub(cls->constants) || pub(cls->functions) || pub(cls->signals) || pub(cls->structs) || pub(cls->enums) || pub(cls->interfaces);
}

} // namespace

std::optional<std::vector<Occurrence>> find_references(Analysis &r_analysis, const std::string &p_path, const std::string &p_source, uint32_t p_offset, const std::vector<std::string> &p_other_scripts) {
	using Kind = DeclId::Kind;
	const Analysis::Pass pass(r_analysis);
	const ProjectApi &project = *r_analysis.pass_classes().get_project();
	std::vector<Script> scripts{ { p_path, p_source, r_analysis.compile(p_path, p_source) } };
	const std::vector<Reference> &own = scripts[0].result->references;
	const Reference *at = reference_at(*scripts[0].result, p_offset);
	if (!at || at->target.kind == Kind::Unresolved) {
		return std::nullopt;
	}
	const DeclId target = at->target;
	// A node path names the node in each scene the script is in.
	std::set<DeclId> family;
	for (const Reference &reference : own) {
		if (reference.begin == at->begin && reference.end == at->end) {
			family.insert(reference.target);
		}
	}
	if (target.kind != Kind::Local) {
		for (const std::string &path : project.script_paths()) {
			const std::optional<std::string> source = is_script_path(path) && path != p_path ? project.script_source(path) : std::nullopt;
			if (source) {
				scripts.push_back({ path, *source, r_analysis.compile(path, *source) });
			}
		}
	}
	// Declarations that override or implement each other are renamed together,
	// but not through those of the engine or of another language.
	std::vector<std::pair<DeclId, DeclId>> overrides;
	for (const Script &script : scripts) {
		for (const Reference &reference : script.result->references) {
			if (reference.declares && reference.overrides && reference.target.kind == Kind::Script && reference.overrides->kind == Kind::Script) {
				overrides.emplace_back(reference.target, *reference.overrides);
			}
		}
	}
	for (bool grown = true; grown;) {
		grown = false;
		for (const auto &[overriding, overridden] : overrides) {
			if (family.count(overriding) != family.count(overridden)) {
				family.insert(overriding);
				family.insert(overridden);
				grown = true;
			}
		}
	}

	std::vector<Occurrence> found;
	std::string name; // As its declaration spells it, for a declaration of this language.
	for (const Script &script : scripts) {
		const Lines lines(script.source);
		for (const Reference &reference : script.result->references) {
			if (family.count(reference.target)) {
				found.push_back(lines.at(script.path, reference));
				found.back().declaration = reference.declares.has_value();
			}
			if (reference.declares && reference.target == target && target.kind == Kind::Script) {
				name = script.source.substr(reference.begin, reference.end - reference.begin);
			}
		}
	}
	for (const DeclId &id : family) {
		if (id.kind == Kind::Node) {
			Occurrence node;
			node.file = id.scope;
			node.reference.target = id;
			node.reference.scene = id.scope;
			node.reference.node = id.name;
			node.declaration = true;
			found.push_back(std::move(node));
		}
		const std::optional<std::string> source = id.kind == Kind::Foreign && !id.name.empty() ? project.script_source(id.scope) : std::nullopt;
		const std::optional<Lookup> declared = source ? gdscript_declaration(*source, id.name) : std::nullopt;
		for (Occurrence &word : declared ? Lines(*source).words(id.scope, id.name) : std::vector<Occurrence>()) {
			if (word.line == declared->line) {
				word.reference.target = id;
				word.declaration = true;
				found.push_back(std::move(word));
				break;
			}
		}
	}
	if (std::any_of(family.begin(), family.end(), [](const DeclId &p_id) { return p_id.kind == Kind::Script; })) {
		for (const std::string &scene : project.scene_paths()) {
			for (const Reference &reference : r_analysis.check_scene(scene)->references) {
				if (family.count(reference.target)) {
					found.push_back({ reference.scene, reference });
				}
			}
		}
	}

	const bool by_name = std::any_of(family.begin(), family.end(), [&](const DeclId &p_id) { return p_id.kind == Kind::Script && reflected(r_analysis.pass_classes(), p_id); });
	if (!name.empty() && by_name) {
		for (const Script &script : scripts) {
			const Lines lines(script.source);
			for (const Reference &reference : script.result->references) {
				if (reference.target == DeclId{ Kind::Unresolved, std::string(), name }) {
					found.push_back(lines.at(script.path, reference));
					found.back().candidate = true;
				}
			}
		}
		for (const std::string &path : p_other_scripts) {
			const std::string source = project.script_source(path).value_or(std::string());
			for (Occurrence &word : Lines(source).words(path, name)) {
				word.candidate = true;
				found.push_back(std::move(word));
			}
		}
	}

	// The files of declarations first, the name's own first of all.
	const auto order = [&](const Occurrence &p_found) {
		return p_found.declaration ? int(p_found.reference.target != target) : 2;
	};
	std::map<std::string, int> file_order;
	for (const Occurrence &each : found) {
		const auto entry = file_order.emplace(each.file, order(each)).first;
		entry->second = std::min(entry->second, order(each));
	}
	const auto key = [&](const Occurrence &p_found) {
		return std::make_tuple(p_found.candidate, p_found.candidate ? 2 : file_order.at(p_found.file), std::cref(p_found.file), order(p_found), p_found.reference.begin);
	};
	std::stable_sort(found.begin(), found.end(), [&](const Occurrence &p_a, const Occurrence &p_b) { return key(p_a) < key(p_b); });
	// A name in a script once, which an implementation of an interface's
	// member and a node path in several scenes have several references for.
	const auto same_place = [](const Occurrence &p_a, const Occurrence &p_b) {
		return p_a.line > 0 && p_a.file == p_b.file && p_a.reference.begin == p_b.reference.begin;
	};
	found.erase(std::unique(found.begin(), found.end(), same_place), found.end());
	return found;
}

namespace {

// The first token of a class's first member, its annotations included, or
// NO_TOKEN without members.
uint32_t first_member(const ClassDecl &p_class) {
	uint32_t first = NO_TOKEN;
	const auto earliest = [&](const auto &p_decls) {
		for (const auto &decl : p_decls) {
			first = std::min(first, decl.range.first);
		}
	};
	earliest(p_class.constants);
	earliest(p_class.members);
	earliest(p_class.static_vars);
	earliest(p_class.functions);
	earliest(p_class.signals);
	earliest(p_class.enums);
	earliest(p_class.structs);
	earliest(p_class.interfaces);
	for (const std::unique_ptr<ClassDecl> &inner : p_class.classes) {
		first = std::min(first, inner->range.first);
	}
	for (const ExportGroupDecl &group : p_class.groups) {
		first = std::min(first, group.annotation.range.first);
	}
	return first;
}

struct Documenter {
	const SourceFile &source;
	const DocName &doc_name;
	DocComments comments;
	std::vector<ClassDoc> docs;

	std::string text(const std::unique_ptr<Expr> &p_expr) const {
		if (!p_expr) {
			return std::string();
		}
		const uint32_t begin = source.begin(p_expr->range);
		return source.text.substr(begin, source.end(p_expr->range) - begin);
	}

	// As this language writes the type, but a typed array as `T[]` and an enum
	// as int with the enum's name, which the documentation links
	// (editor/doc/editor_help.cpp:410-470).
	DocMember typed(const std::string &p_name, const Type &p_type) const {
		DocMember member;
		member.name = p_name;
		if (p_type.kind == TypeKind::Enum) {
			member.type = "int";
			member.enumeration = p_type.script.empty() ? p_type.name : doc_class_name(doc_name, p_type.script) + "." + p_type.interface_name();
		} else if (p_type.kind == TypeKind::Builtin && p_type.name == "Array" && p_type.args.size() == 1) {
			member.type = p_type.args[0].to_string() + "[]";
		} else {
			member.type = p_type.to_string();
		}
		return member;
	}

	template <typename T>
	DocMember member(const T &p_decl, const Type &p_type) const {
		DocMember member = typed(p_decl.name, p_type);
		member.comment = comments.of(p_decl.range.first, p_decl.name_token, false);
		return member;
	}

	DocMember function(const FuncDecl &p_function) const {
		DocMember method = member(p_function, p_function.return_type);
		for (const char *qualifier : { p_function.is_static ? "static" : "", p_function.is_async ? "async" : "", p_function.is_abstract ? "abstract" : "" }) {
			if (*qualifier) {
				method.qualifiers += (method.qualifiers.empty() ? "" : " ") + std::string(qualifier);
			}
		}
		for (const ParamDecl &param : p_function.params) {
			method.arguments.push_back(typed(param.name, param.type));
			method.arguments.back().value = text(param.default_value);
		}
		return method;
	}

	DocMember signal(const SignalDecl &p_signal) const {
		DocMember documented;
		documented.name = p_signal.name;
		documented.comment = comments.of(p_signal.range.first, p_signal.name_token, false);
		for (const ParamDecl &param : p_signal.params) {
			documented.arguments.push_back(typed(param.name, param.type));
		}
		return documented;
	}

	void document(const ClassDecl &p_class, const std::string &p_path) {
		for (const std::unique_ptr<ClassDecl> &inner : p_class.classes) {
			document(*inner, p_path + "::" + inner->name);
		}
		const std::string name = doc_class_name(doc_name, p_path);
		for (const StructDecl &decl : p_class.structs) {
			ClassDoc doc;
			doc.name = name + "." + decl.name;
			doc.comment = comments.of(decl.range.first, decl.name_token, true);
			for (const VarDecl &field : decl.fields) {
				doc.properties.push_back(member(field, field.type));
				doc.properties.back().value = text(field.initializer);
			}
			for (const FuncDecl &method : decl.methods) {
				doc.methods.push_back(function(method));
				doc.methods.back().name = method.name.substr(decl.name.size() + 1);
			}
			docs.push_back(std::move(doc));
		}
		for (const InterfaceDecl &decl : p_class.interfaces) {
			ClassDoc doc;
			doc.name = name + "." + decl.name;
			// The engine's documentation follows one parent (core/doc_data.h:700,
			// editor/doc/editor_help.cpp:1032-1044; ed1daf0), so an interface
			// with several names them in its first line.
			std::vector<std::string> parents;
			for (const Type &parent : decl.parents) {
				parents.push_back(doc_class_name(doc_name, parent.script) + "." + parent.interface_name());
			}
			doc.comment = comments.of(decl.range.first, decl.name_token, true);
			if (parents.size() == 1) {
				doc.inherits = parents[0];
			} else if (!parents.empty()) {
				std::string named = parents[0];
				for (size_t i = 1; i < parents.size(); i++) {
					named += ", " + parents[i];
				}
				doc.comment.brief = "Extends " + named + "." + (doc.comment.brief.empty() ? std::string() : "\n" + doc.comment.brief);
			}
			for (const FuncDecl &method : decl.methods) {
				doc.methods.push_back(function(method));
			}
			for (const InterfaceProperty &property : decl.properties) {
				doc.properties.push_back(member(property, property.type));
			}
			for (const SignalDecl &declared : decl.signals) {
				doc.signals.push_back(signal(declared));
			}
			docs.push_back(std::move(doc));
		}
		ClassDoc doc;
		doc.name = name;
		doc.inherits = p_class.base_script.empty() ? p_class.extends : doc_class_name(doc_name, p_class.base_script);
		doc.comment = p_class.name_token == NO_TOKEN ? comments.of_file(first_member(p_class)) : comments.of(p_class.range.first, p_class.name_token, true);
		for (const ConstDecl &constant : p_class.constants) {
			doc.constants.push_back(member(constant, constant.type));
			doc.constants.back().value = text(constant.initializer);
		}
		for (const EnumDecl &decl : p_class.enums) {
			if (!decl.name.empty()) {
				DocMember documented;
				documented.name = decl.name;
				documented.comment = comments.of(decl.range.first, decl.name_token, false);
				doc.enums.push_back(std::move(documented));
			}
			for (size_t i = 0; i < decl.values.size(); i++) {
				const EnumValueDecl &value = decl.values[i];
				DocMember constant = typed(value.name, decl.type);
				// A comment after a line's values is the last one's.
				const bool last = i + 1 == decl.values.size() || source.line(decl.values[i + 1].name_token) > source.line(value.name_token);
				constant.comment = comments.of(value.name_token, value.name_token, false, last);
				if (decl.numbers) {
					for (const auto &[number_name, number] : decl.numbers->list) {
						if (number_name == value.name) {
							constant.value = std::to_string(number);
						}
					}
				}
				doc.constants.push_back(std::move(constant));
			}
		}
		for (const std::vector<VarDecl> *variables : { &p_class.members, &p_class.static_vars }) {
			for (const VarDecl &variable : *variables) {
				doc.properties.push_back(member(variable, variable.type));
				doc.properties.back().value = text(variable.initializer);
			}
		}
		for (const FuncDecl &method : p_class.functions) {
			// Not the setters and getters written inside a property, named @set_x
			// and @get_x, nor what a syntax error left of a function.
			if (!method.name.empty() && method.name[0] != '@') {
				doc.methods.push_back(function(method));
			}
		}
		for (const SignalDecl &declared : p_class.signals) {
			doc.signals.push_back(signal(declared));
		}
		docs.push_back(std::move(doc));
	}
};

} // namespace

std::vector<ClassDoc> document(const ClassDecl &p_class, const std::string &p_path, const DocName &p_doc_name) {
	Documenter documenter{ *p_class.source, p_doc_name, DocComments(*p_class.source), {} };
	documenter.document(p_class, p_path);
	return documenter.docs;
}

} // namespace petrichor::compiler
