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
#include <type_traits>

namespace petrichor::compiler {

namespace {

using Kind = SymbolKind;

const Reference *declaration_of(const std::vector<Reference> &p_references, const DeclId &p_target) {
	for (const Reference &reference : p_references) {
		if (reference.declares && reference.target == p_target) {
			return &reference;
		}
	}
	return nullptr;
}

// The keyword that declares each SymbolKind.
constexpr const char *DECLARED_BY[] = { "class", "const", "var", "func", "signal", "enum", "var", "const" };

// As hover shows a declaration: its qualifiers and keyword, its class, and
// `p_written` after it, or else its name.
void declare(Lookup &r_found, const std::string &p_qualifiers, const std::string &p_written) {
	const std::string &named = p_written.empty() ? r_found.member : p_written;
	r_found.declaration = p_qualifiers + DECLARED_BY[int(r_found.kind)] + " " + r_found.class_name + (named.empty() ? "" : "." + named);
}

// The parameters a signal passes, with their types (spec §4.7).
std::string signal_signature(const std::string &p_name, const std::vector<std::string> &p_names, const std::vector<Type> &p_types) {
	std::vector<std::string> params;
	for (size_t i = 0; i < p_names.size(); i++) {
		params.push_back(p_names[i] + ": " + p_types[i].to_string());
	}
	return signature(p_name, params, false, -1, std::string());
}

// Where the documentation files a declaration of the engine: under its class,
// or under @GlobalScope for the global enums, as "Key" and "Variant.Type".
std::optional<Lookup> engine_lookup(const EngineApi &p_engine, const DeclId &p_target) {
	const std::string &owner = p_target.scope;
	const std::string &name = p_target.name;
	Lookup found;
	found.class_name = owner;
	found.member = name;
	std::string written; // A function's or a signal's signature, a property's type.
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
		const builtin::Utility *utility = builtin::find_utility(name);
		found.kind = utility ? Kind::Method : Kind::Constant;
		written = utility ? api_signature(name, utility->arguments, utility->argument_count, utility->is_vararg, utility->return_type, -1) : std::string();
	} else if (owner == "@GDScript") {
		found.kind = name == "PI" || name == "TAU" || name == "INF" || name == "NAN" ? Kind::Constant : Kind::Method;
	} else if (const builtin::Member *member = type ? builtin::find_member(*type, name) : nullptr) {
		found.kind = Kind::Property;
		written = name + ": " + api_type(member->type, true).to_string();
	} else if (const builtin::Method *method = type ? builtin::find_method(*type, name) : nullptr) {
		found.kind = Kind::Method;
		written = api_signature(name, method->arguments, method->argument_count, method->is_vararg, method->return_type, -1);
	} else if (type) {
		// An enum's values are constants of the type to the documentation.
		found.kind = Kind::Constant;
	} else if (const std::optional<EngineMethod> signal = p_engine.find_signal(owner, name)) {
		found.kind = Kind::Signal;
		std::vector<std::string> names;
		for (const EngineParam &param : signal->params) {
			names.push_back(param.name);
		}
		written = signal_signature(name, names, signal_type(*signal).args);
	} else if (const std::optional<EngineProperty> property = p_engine.find_property(owner, name)) {
		found.kind = Kind::Property;
		Type value = property->type;
		value.nullable = value.nullable && !builtin::property_non_null(property->owner, name);
		written = name + ": " + value.to_string();
	} else if (const std::optional<EngineMethod> method = p_engine.find_method(owner, name)) {
		found.kind = Kind::Method;
		std::vector<Type> params;
		for (size_t i = 0; i < method->params.size(); i++) {
			params.push_back(engine_param(*method, i));
		}
		written = engine_signature(name, *method, params, -1, engine_result(*method).to_string());
	} else if (p_engine.find_constant(owner, name)) {
		found.kind = Kind::Constant;
	} else if (p_engine.find_enum(owner, name)) {
		found.kind = Kind::Enum;
	} else {
		return std::nullopt;
	}
	declare(found, std::string(), written);
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
		const size_t name = at;
		for (const auto &[spelling, kind] : KEYWORDS) {
			if (keyword == spelling && word(at) == p_name) {
				Lookup found;
				found.kind = kind;
				found.line = line;
				found.begin = uint32_t(name);
				found.end = uint32_t(at);
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

const Reference *reference_at(const CompileResult &p_result, uint32_t p_offset) {
	for (const Reference &reference : p_result.references) {
		if (reference.begin <= p_offset && p_offset <= reference.end) {
			return &reference;
		}
	}
	return nullptr;
}

std::string doc_class_name(const DocName &p_doc_name, const std::string &p_path) {
	std::string name = p_doc_name(script_file(p_path));
	for (size_t at = p_path.find("::"); at != std::string::npos;) {
		const size_t next = p_path.find("::", at + 2);
		name += "." + p_path.substr(at + 2, next == std::string::npos ? next : next - at - 2);
		at = next;
	}
	return name;
}

namespace {

// A class or a member of this language or another, as lookup() shows it.
// `p_result` is the script at `p_path` compiled from `p_source`.
// A member of a checked class as hover shows it after the class: a
// function's or a signal's signature as completion's call hint writes it, a
// variable's or a constant's name and type; "" for another, as a value of an
// enum. `r_qualifiers` gets a function's.
std::string written(const ClassDecl &p_class, const std::string &p_member, std::string &r_qualifiers) {
	const size_t dot = p_member.find('.');
	const std::string outer = dot == std::string::npos ? std::string() : p_member.substr(0, dot);
	const std::string name = p_member.substr(dot == std::string::npos ? 0 : dot + 1);
	const FuncDecl *called = nullptr;
	const auto find = [&](const std::vector<FuncDecl> &p_functions, const std::string &p_name) {
		for (const FuncDecl &function : p_functions) {
			called = function.name == p_name ? &function : called;
		}
	};
	const auto typed = [&](const auto &p_decls) {
		for (const auto &decl : p_decls) {
			if (decl.name == name) {
				return name + ": " + decl.type.to_string();
			}
		}
		return std::string();
	};
	const auto signal = [&](const std::vector<SignalDecl> &p_signals) {
		for (const SignalDecl &decl : p_signals) {
			if (decl.name == name) {
				std::vector<std::string> names;
				std::vector<Type> types;
				for (const ParamDecl &param : decl.params) {
					names.push_back(param.name);
					types.push_back(param.type);
				}
				return signal_signature(name, names, types);
			}
		}
		return std::string();
	};
	std::string text;
	if (outer.empty()) {
		find(p_class.functions, name);
		text = typed(p_class.members) + typed(p_class.static_vars) + typed(p_class.constants) + signal(p_class.signals);
	} else if (const StructDecl *decl = p_class.find_struct(outer)) {
		find(decl->methods, p_member);
		text = typed(decl->fields);
	}
	for (const InterfaceDecl &decl : p_class.interfaces) {
		if (decl.name == outer) {
			find(decl.methods, name);
			text = typed(decl.properties) + signal(decl.signals);
		}
	}
	if (!called) {
		return text;
	}
	r_qualifiers = std::string(called->is_static ? "static " : "") + (called->is_async ? "async " : "");
	return declared_signature(name, *called, *p_class.source, -1, called->return_type);
}

std::optional<Lookup> declared(Analysis &r_analysis, const DocName &p_doc_name, const std::string &p_path, const std::string &p_source, const std::shared_ptr<const CompileResult> &p_result, const DeclId &p_target) {
	const ProjectApi &project = *r_analysis.pass_classes().get_project();
	if (p_target.kind == DeclId::Kind::Foreign) {
		const std::optional<std::string> source = p_target.scope.empty() ? std::nullopt : project.script_source(p_target.scope);
		if (!source) {
			return std::nullopt;
		}
		std::optional<Lookup> found;
		if (p_target.name.empty()) {
			found.emplace();
			found->kind = Kind::Class;
			found->line = 1;
		} else {
			found = gdscript_declaration(*source, p_target.name);
		}
		if (found) {
			found->class_name = p_doc_name(p_target.scope);
			found->member = p_target.name;
			found->path = p_target.scope;
			declare(*found, std::string(), std::string());
		}
		return found;
	}
	const std::string file = script_file(p_target.scope);
	const std::optional<std::string> source = file == p_path ? p_source : project.script_source(file);
	if (!source) {
		return std::nullopt;
	}
	// Each file records its own declarations.
	const std::shared_ptr<const CompileResult> declared_in = file == p_path ? p_result : r_analysis.compile(file, *source);
	const Reference *declaration = declaration_of(declared_in->references, p_target);
	if (!declaration && !p_target.name.empty()) {
		return std::nullopt;
	}
	Lookup found;
	found.kind = declaration ? *declaration->declares : Kind::Class;
	found.class_name = doc_class_name(p_doc_name, p_target.scope);
	found.member = p_target.name;
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
	found.begin = declaration ? declaration->begin : 0;
	found.end = declaration ? declaration->end : 0;
	// As its help page has it, which the script editor shows itself.
	const ClassDecl *checked = r_analysis.pass_classes().declarations(file);
	for (const ClassDoc &doc : checked ? document(*checked, file, p_doc_name) : std::vector<ClassDoc>()) {
		if (doc.name == found.class_name && found.member.empty()) {
			found.comment = doc.comment;
		}
		for (const std::vector<DocMember> *members : { &doc.constants, &doc.properties, &doc.methods, &doc.signals, &doc.enums }) {
			for (const DocMember &member : *members) {
				if (doc.name == found.class_name && member.name == found.member) {
					found.comment = member.comment;
				}
			}
		}
	}
	std::string qualifiers;
	const ClassDecl *owner = r_analysis.pass_classes().declarations(p_target.scope);
	const std::string text = owner && !p_target.name.empty() ? written(*owner, p_target.name, qualifiers) : std::string();
	declare(found, qualifiers, text);
	return found;
}

} // namespace

size_t word_start(const std::string &p_text, size_t p_cursor) {
	const auto symbol = [](char p_char) {
		return p_char != '_' && ((p_char >= '!' && p_char <= '/') || (p_char >= ':' && p_char <= '@') || (p_char >= '[' && p_char <= '`') || (p_char >= '{' && p_char <= '~'));
	};
	while (p_cursor > 0 && (p_text[p_cursor - 1] == '/' || ((unsigned char)p_text[p_cursor - 1] > ' ' && !symbol(p_text[p_cursor - 1])))) {
		p_cursor--;
	}
	return p_cursor;
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
	const size_t cursor = p_source.find(CURSOR);
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
	const Analysis::Pass pass(r_analysis);
	const ProjectApi &project = *r_analysis.pass_classes().get_project();
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
			found.begin = offset;
			found.end = name != file->tokens.end() && name->offset == offset ? name->end() : offset;
			found.declaration = std::string(DECLARED_BY[int(found.kind)]) + " " + p_source.substr(found.begin, found.end - found.begin) + (found.type.empty() ? "" : ": " + found.type);
			return found;
		}
		case DeclId::Kind::Script:
		case DeclId::Kind::Foreign:
			return declared(r_analysis, p_doc_name, p_path, p_source, result, target);
		case DeclId::Kind::Engine:
			return engine_lookup(r_analysis.engine_api(), target);
		default:
			return std::nullopt;
	}
}

namespace {

// The type of what the innermost name or expression at an offset holds, in
// a checked syntax tree.
class TypeAt {
	const SourceFile &file;
	const uint32_t offset;
	const FuncDecl *function = nullptr;

	bool on(uint32_t p_token) const {
		return p_token < file.tokens.size() && file.tokens[p_token].offset <= offset && offset <= file.tokens[p_token].end();
	}

public:
	std::optional<Type> found;

	TypeAt(const SourceFile &p_file, uint32_t p_offset) :
			file(p_file), offset(p_offset) {}

	template <typename Node>
	void visit(const Node &p_node) {
		if constexpr (std::is_same_v<Node, Expr>) {
			const bool named = p_node.kind == ExprKind::Name || p_node.kind == ExprKind::Member || p_node.kind == ExprKind::Call;
			const bool within = file.begin(p_node.range) <= offset && offset <= file.end(p_node.range);
			if (named ? on(p_node.name_token) : within) {
				found = p_node.type;
			}
		} else if constexpr (std::is_same_v<Node, VarDecl> || std::is_same_v<Node, ParamDecl> || std::is_same_v<Node, ConstDecl> || std::is_same_v<Node, InterfaceProperty>) {
			if (on(p_node.name_token)) {
				found = p_node.type;
			}
		} else if constexpr (std::is_same_v<Node, Stmt> || std::is_same_v<Node, Pattern>) {
			bool declares = false;
			if constexpr (std::is_same_v<Node, Stmt>) {
				declares = p_node.kind == StmtKind::Var || p_node.kind == StmtKind::Const || p_node.kind == StmtKind::For;
			} else {
				declares = p_node.kind == PatternKind::Bind;
			}
			if (declares && function && p_node.local >= 0 && size_t(p_node.local) < function->locals.size() && on(p_node.name_token)) {
				found = function->locals[p_node.local].type;
			}
		}
		const FuncDecl *outer = function;
		if constexpr (std::is_same_v<Node, FuncDecl>) {
			function = &p_node;
		}
		for_each_child(p_node, [&](const auto &p_child) { visit(p_child); });
		function = outer;
	}
};

// The declarations of the classes, structs, interfaces and enums of scripts
// in a type.
void declarations_in(const Type &p_type, std::vector<DeclId> &r_found) {
	if (!p_type.script.empty()) {
		const bool member = p_type.kind == TypeKind::Struct || p_type.kind == TypeKind::Enum || p_type.kind == TypeKind::PayloadEnum || p_type.is_interface;
		const DeclId id{ is_script_path(p_type.script) ? DeclId::Kind::Script : DeclId::Kind::Foreign, p_type.script, member ? p_type.interface_name() : std::string() };
		if (std::find(r_found.begin(), r_found.end(), id) == r_found.end()) {
			r_found.push_back(id);
		}
	}
	for (const Type &arg : p_type.args) {
		declarations_in(arg, r_found);
	}
}

} // namespace

std::vector<Lookup> type_definition(Analysis &r_analysis, const DocName &p_doc_name, const std::string &p_path, const std::string &p_source, uint32_t p_offset) {
	const Analysis::Pass pass(r_analysis);
	ScriptClasses &classes = r_analysis.pass_classes();
	CompileOptions options;
	options.path = p_path;
	options.project = classes.get_project();
	std::vector<Diagnostic> errors;
	ClassDecl decl = parse(p_source, errors);
	TypeAt at(*decl.source, p_offset);
	classes.begin_compile(p_path, p_source);
	check(decl, r_analysis.engine_api(), options, errors, classes);
	at.visit(decl);
	classes.end_compile();
	std::vector<DeclId> ids;
	if (at.found) {
		declarations_in(*at.found, ids);
	}
	std::vector<Lookup> found;
	const std::shared_ptr<const CompileResult> result = r_analysis.compile(p_path, p_source);
	for (const DeclId &id : ids) {
		if (std::optional<Lookup> each = declared(r_analysis, p_doc_name, p_path, p_source, result, id)) {
			found.push_back(std::move(*each));
		}
	}
	return found;
}

namespace {

class Outline {
	const SourceFile &file;

	Symbol make(Symbol::Kind p_kind, const std::string &p_name, const TokenRange &p_range, uint32_t p_name_token) const {
		Symbol made;
		made.kind = p_kind;
		made.name = p_name;
		made.begin = file.begin(p_range);
		made.end = file.end(p_range);
		const bool named = p_name_token < file.tokens.size();
		made.name_begin = named ? file.tokens[p_name_token].offset : made.begin;
		made.name_end = named ? file.tokens[p_name_token].end() : made.begin;
		return made;
	}

	template <typename Decl>
	void add(std::vector<Symbol> &r_to, Symbol::Kind p_kind, const std::vector<Decl> &p_decls) const {
		for (const Decl &decl : p_decls) {
			r_to.push_back(make(p_kind, decl.name, decl.range, decl.name_token));
		}
	}

	void add_functions(std::vector<Symbol> &r_to, const std::vector<FuncDecl> &p_functions, size_t p_prefix) const {
		for (const FuncDecl &function : p_functions) {
			// Not the parser's inline accessors, named "@set_x", nor what it
			// kept of a broken line.
			if (!function.name.empty() && function.name[0] != '@') {
				r_to.push_back(make(Symbol::Kind::Method, function.name.substr(p_prefix), function.range, function.name_token));
			}
		}
	}

public:
	explicit Outline(const SourceFile &p_file) :
			file(p_file) {}

	void fill(Symbol &r_class, const ClassDecl &p_class) const {
		std::vector<Symbol> &into = r_class.children;
		add(into, Symbol::Kind::Variable, p_class.members);
		add(into, Symbol::Kind::Variable, p_class.static_vars);
		add(into, Symbol::Kind::Constant, p_class.constants);
		add(into, Symbol::Kind::Signal, p_class.signals);
		add_functions(into, p_class.functions, 0);
		for (const EnumDecl &decl : p_class.enums) {
			// An unnamed enum's values are constants of the class.
			Symbol named = make(Symbol::Kind::Enum, decl.name, decl.range, decl.name_token);
			std::vector<Symbol> &values = decl.name.empty() ? into : named.children;
			for (const EnumValueDecl &value : decl.values) {
				values.push_back(make(Symbol::Kind::Value, value.name, value.range, value.name_token));
				add(values.back().children, Symbol::Kind::Variable, value.fields);
			}
			if (!decl.name.empty()) {
				into.push_back(std::move(named));
			}
		}
		for (const StructDecl &decl : p_class.structs) {
			into.push_back(make(Symbol::Kind::Struct, decl.name, decl.range, decl.name_token));
			add(into.back().children, Symbol::Kind::Variable, decl.fields);
			add_functions(into.back().children, decl.methods, decl.name.size() + 1);
		}
		for (const InterfaceDecl &decl : p_class.interfaces) {
			into.push_back(make(Symbol::Kind::Interface, decl.name, decl.range, decl.name_token));
			add(into.back().children, Symbol::Kind::Variable, decl.properties);
			add(into.back().children, Symbol::Kind::Signal, decl.signals);
			add_functions(into.back().children, decl.methods, 0);
		}
		for (const std::unique_ptr<ClassDecl> &inner : p_class.classes) {
			into.push_back(make(Symbol::Kind::Class, inner->name, inner->range, inner->name_token));
			fill(into.back(), *inner);
		}
		for (Symbol &each : into) {
			std::stable_sort(each.children.begin(), each.children.end(), [](const Symbol &p_a, const Symbol &p_b) { return p_a.begin < p_b.begin; });
		}
		std::stable_sort(into.begin(), into.end(), [](const Symbol &p_a, const Symbol &p_b) { return p_a.begin < p_b.begin; });
	}
};

} // namespace

Symbol outline(const std::string &p_source, const std::string &p_name) {
	std::vector<Diagnostic> errors;
	const ClassDecl decl = parse(p_source, errors);
	Symbol file;
	file.name = decl.class_name.empty() ? p_name : decl.class_name;
	file.end = uint32_t(p_source.size());
	if (decl.class_name_token < decl.source->tokens.size()) {
		file.name_begin = decl.source->tokens[decl.class_name_token].offset;
		file.name_end = decl.source->tokens[decl.class_name_token].end();
	}
	Outline(*decl.source).fill(file, decl);
	return file;
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

std::optional<std::vector<Occurrence>> find_references(Analysis &r_analysis, const std::string &p_path, uint32_t p_offset, const std::vector<std::string> &p_other_scripts) {
	using Kind = DeclId::Kind;
	const Analysis::Pass pass(r_analysis);
	const ProjectApi &project = *r_analysis.pass_classes().get_project();
	const std::string source = project.script_source(p_path).value_or(std::string());
	std::vector<Script> scripts{ { p_path, source, r_analysis.compile(p_path, source) } };
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
