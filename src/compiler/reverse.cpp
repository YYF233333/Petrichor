#include "compiler/reverse.h"

#include "compiler/ast.h"
#include "compiler/checker.h"
#include "compiler/migration.h"
#include "compiler/parser.h"
#include "compiler/script_classes.h"
#include "compiler/warnings.h"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <optional>
#include <type_traits>

namespace petrichor::compiler {

std::string reversed_path(const std::string &p_file) {
	return p_file.substr(0, p_file.size() - 4) + ".gd";
}

namespace {

const char *const LACKS = "GDScript has no ";
const char *const NOT_YET = "This is not converted back yet: ";

bool opens(TokenKind p_kind) {
	return p_kind == TokenKind::ParenOpen || p_kind == TokenKind::BracketOpen || p_kind == TokenKind::BraceOpen;
}

bool closes(TokenKind p_kind) {
	return p_kind == TokenKind::ParenClose || p_kind == TokenKind::BracketClose || p_kind == TokenKind::BraceClose;
}

// The bracket that closes the one at `p_open`.
uint32_t closing(const std::vector<Token> &p_tokens, uint32_t p_open) {
	int depth = 0;
	for (uint32_t i = p_open; i < p_tokens.size(); i++) {
		depth += opens(p_tokens[i].kind) ? 1 : (closes(p_tokens[i].kind) ? -1 : 0);
		if (depth == 0) {
			return i;
		}
	}
	return p_open;
}

// What GDScript lacks of a type (spec §13.4), or "".
std::string lacking(const Type &p_type) {
	if (p_type.is_two_level()) {
		return std::string(LACKS) + "T?? or Variant?, whose two Nones are one null there.";
	}
	if (p_type.is_arguments()) {
		return std::string(NOT_YET) + "what awaiting a signal with several parameters gives.";
	}
	switch (p_type.kind) {
		case TypeKind::Struct:
			return std::string(LACKS) + "struct.";
		case TypeKind::PayloadEnum:
			if (p_type.is_settled()) {
				return std::string(LACKS) + "Task value.";
			}
			return std::string(LACKS) + (p_type.is_result() ? "Result." : "enum with payloads.");
		case TypeKind::Task:
			return std::string(LACKS) + "Task value.";
		case TypeKind::Object:
			if (p_type.is_interface) {
				return std::string(LACKS) + "interface.";
			}
			break;
		default:
			break;
	}
	for (const Type &arg : p_type.args) {
		const std::string lacks = lacking(arg);
		if (!lacks.empty()) {
			return lacks;
		}
	}
	return std::string();
}

// The edits that write a .pet file's checked tree back in GDScript, by
// deleting or replacing its tokens (architecture §6.6), what they erase from
// its tree, and each place GDScript lacks.
class Converter {
	const std::string &path;
	const SourceFile &source;
	const std::vector<Token> &tokens;
	const std::string &text;
	const FuncDecl *function = nullptr; // Whose locals the statements declare.
	std::set<const Expr *> settled; // Task values awaited or detached.
	std::set<std::pair<int, std::string>> said; // Refusals by line, once.

	void refuse(uint32_t p_token, const std::string &p_message) {
		const Token &token = tokens[std::min<size_t>(p_token, tokens.size() - 1)];
		Diagnostic refusal{ token.offset, token.end(), p_message };
		const SourcePosition position = source.position(token.offset);
		refusal.line = position.line;
		refusal.column = position.column;
		refusal.file = path;
		if (said.emplace(position.line, p_message).second) {
			refused.push_back(std::move(refusal));
		}
	}

	void erase(uint32_t p_begin, uint32_t p_end, const std::string &p_text = std::string()) {
		edits.push_back({ p_begin, p_end, p_text });
	}

	uint32_t spaces_after(uint32_t p_at) const {
		while (p_at < text.size() && (text[p_at] == ' ' || text[p_at] == '\t')) {
			p_at++;
		}
		return p_at;
	}

	// The token and the spaces after it.
	void erase_word(uint32_t p_at) {
		erase(tokens[p_at].offset, spaces_after(tokens[p_at].end()));
	}

	// The tokens from `p_first` to `p_last`, with their line when nothing
	// else is on it.
	void erase_tokens(uint32_t p_first, uint32_t p_last) {
		uint32_t begin = tokens[p_first].offset;
		while (begin > 0 && (text[begin - 1] == ' ' || text[begin - 1] == '\t')) {
			begin--;
		}
		uint32_t end = spaces_after(tokens[p_last].end());
		const bool alone = (begin == 0 || text[begin - 1] == '\n') && (end == text.size() || text[end] == '\r' || text[end] == '\n');
		if (!alone) {
			erase(tokens[p_first].offset, end);
			return;
		}
		end += end < text.size() && text[end] == '\r';
		end += end < text.size() && text[end] == '\n';
		erase(begin, end);
	}

	// A type written where GDScript writes it as well, its `?` and the root of
	// a PackedScene deleted (spec §1.2, §7.4).
	void written(const TypeRef &p_ref, const Type &p_type) {
		if (p_ref.is_function) {
			refuse(p_ref.name_token, std::string(NOT_YET) + "a function type, which GDScript writes Callable.");
			return;
		}
		if (p_ref.optional > 0) {
			const bool object = (p_type.kind == TypeKind::Object && p_type.nullable) && p_ref.optional == 1 && tokens[p_ref.range.last].kind == TokenKind::Question;
			if (!object) {
				refuse(p_ref.name_token, std::string(NOT_YET) + "a value type's T? inside a type.");
				return;
			}
			erase(tokens[p_ref.range.last].offset, tokens[p_ref.range.last].end());
		}
		const uint32_t open = p_ref.name_token + 2 * uint32_t(std::count(p_ref.name.begin(), p_ref.name.end(), '.')) + 1;
		if (p_ref.name == "PackedScene" && !p_ref.args.empty()) {
			erase(tokens[open].offset, tokens[closing(tokens, open)].end());
			return;
		}
		for (size_t i = 0; i < p_ref.args.size() && i < p_type.args.size(); i++) {
			if (is_typed_container(p_type.args[i])) {
				refuse(p_ref.args[i].name_token, std::string(NOT_YET) + "a typed container inside a typed container.");
			} else {
				written(p_ref.args[i], p_type.args[i]);
			}
		}
	}

	// The type written for a declaration, from the byte `p_from` on: a value
	// type's T? is no type in GDScript (spec §10.1), so it goes.
	void declared(const TypeRef &p_ref, const Type &p_type, uint32_t p_from, bool p_exported) {
		const std::string lacks = lacking(p_type);
		if (!lacks.empty()) {
			refuse(p_ref.name_token, lacks);
			return;
		}
		if (p_type.kind != TypeKind::Option) {
			written(p_ref, p_type);
		} else if (p_exported) {
			refuse(p_ref.name_token, std::string(NOT_YET) + "an export of a value type's T?, which GDScript exports with a type.");
		} else {
			erase(p_from, tokens[p_ref.range.last].end());
			erased.types.insert(p_ref.name_token);
		}
	}

	// `name := value` of a value type's T?, which GDScript cannot infer: `=`.
	void inferred(uint32_t p_name, const Type &p_type) {
		const std::string lacks = lacking(p_type);
		if (!lacks.empty()) {
			refuse(p_name, lacks);
		} else if (p_type.kind == TypeKind::Option && tokens[p_name + 1].kind == TokenKind::Colon && tokens[p_name + 2].kind == TokenKind::Equal) {
			erase(tokens[p_name + 1].offset, tokens[p_name + 1].end());
		}
	}

	// A call that reads as its receiver (-1), or as its argument (0) in
	// parentheses where it is an operand and not one token or call already.
	void collapse(const Expr &p_call, int p_inner, bool p_operand) {
		const uint32_t open = p_call.name_token + 1;
		const uint32_t close = closing(tokens, open);
		erased.calls[p_call.name_token] = p_inner;
		if (p_inner < 0) {
			erase(tokens[p_call.name_token - 1].offset, tokens[close].end());
			return;
		}
		const Expr &arg = *p_call.args[0];
		const bool parenthesized = tokens[arg.range.first].kind == TokenKind::ParenOpen && closing(tokens, arg.range.first) == arg.range.last;
		const bool atomic = parenthesized || (arg.kind != ExprKind::Unary && arg.kind != ExprKind::Binary && arg.kind != ExprKind::Cast && arg.kind != ExprKind::TypeTest && arg.kind != ExprKind::Conditional && arg.kind != ExprKind::Await && arg.kind != ExprKind::Lambda);
		const bool kept = p_operand && !atomic;
		erase(tokens[p_call.name_token].offset, kept ? tokens[open].offset : tokens[open].end());
		if (!kept) {
			erase(tokens[close].offset, tokens[close].end());
		}
	}

	void expression(const Expr &p_expr, bool p_operand) {
		const std::string lacks = lacking(p_expr.type);
		if (!lacks.empty() && (p_expr.type.kind != TypeKind::Task || !settled.count(&p_expr))) {
			refuse(p_expr.range.first, lacks);
		}
		switch (p_expr.kind) {
			case ExprKind::Await:
				settled.insert(p_expr.left.get());
				break;
			case ExprKind::Try:
				refuse(p_expr.range.last, std::string(LACKS) + "? operator.");
				break;
			case ExprKind::Cast:
			case ExprKind::TypeTest:
				if (p_expr.tested.is_interface) {
					refuse(p_expr.cast_type.name_token, std::string(LACKS) + "interface.");
				} else {
					written(p_expr.cast_type, p_expr.tested);
				}
				break;
			case ExprKind::NoneLiteral:
				if (p_expr.op != TokenKind::Null) {
					for (uint32_t i = p_expr.range.first; i <= p_expr.range.last; i++) {
						if (is_word(tokens[i], "None")) {
							erase(tokens[i].offset, tokens[i].end(), "null");
						}
					}
				}
				break;
			case ExprKind::Lambda:
				visit(*p_expr.function);
				return;
			case ExprKind::Call:
				call(p_expr, p_operand);
				break;
			default:
				break;
		}
		for_each_child(p_expr, [&](const auto &p_child) {
			if constexpr (std::is_same_v<std::decay_t<decltype(p_child)>, Expr>) {
				const bool operand = &p_child == p_expr.base.get() || p_expr.kind == ExprKind::Unary || p_expr.kind == ExprKind::Binary || p_expr.kind == ExprKind::Cast || p_expr.kind == ExprKind::TypeTest || p_expr.kind == ExprKind::Await || p_expr.kind == ExprKind::Conditional || p_expr.kind == ExprKind::Try;
				expression(p_child, operand);
			} else {
				visit(p_child);
			}
		});
	}

	void call(const Expr &p_call, bool p_operand) {
		if (!p_call.member_names.empty()) {
			refuse(p_call.member_name_tokens[0], std::string(NOT_YET) + "members passed by name to new() or instantiate().");
		}
		switch (p_call.target) {
			case Target::LanguageMethod:
				if (p_call.language->of == LanguageFunction::Of::Option && p_call.text == "unwrap") {
					collapse(p_call, -1, p_operand);
				} else if (p_call.language->of == LanguageFunction::Of::Option) {
					refuse(p_call.name_token, std::string(NOT_YET) + p_call.text + "() of T?.");
				}
				break;
			case Target::TaskDetach:
				settled.insert(p_call.base.get());
				collapse(p_call, -1, p_operand);
				break;
			case Target::Some:
				collapse(p_call, 0, p_operand);
				break;
			case Target::TryFrom:
				refuse(p_call.name_token, std::string(LACKS) + "try_from().");
				break;
			case Target::TaskCancel:
			case Target::TaskSettled:
			case Target::TaskAny:
			case Target::TaskAll:
			case Target::TaskOf:
			case Target::TaskDelay:
				refuse(p_call.name_token, std::string(LACKS) + "Task value.");
				break;
			case Target::EnumFromInt:
				refuse(p_call.name_token, std::string(NOT_YET) + "the conversion of an int to an enum.");
				break;
			case Target::Encode:
				refuse(p_call.name_token, std::string(NOT_YET) + p_call.text + "() of a container.");
				break;
			default:
				if (p_call.type.is_result() && p_call.text.rfind("try_", 0) == 0) {
					refuse(p_call.name_token, std::string(LACKS) + "try_ version of an engine method.");
				}
				break;
		}
	}

public:
	std::vector<Edit> edits;
	Erased erased;
	std::vector<Diagnostic> refused;

	Converter(const std::string &p_path, const SourceFile &p_source) :
			path(p_path), source(p_source), tokens(p_source.tokens), text(p_source.text) {}

	// `pub` and `async` where they begin a declaration's syntax (spec §2.2).
	void keywords() {
		for (uint32_t i = 0; i + 1 < tokens.size(); i++) {
			const Token &next = tokens[i + 1];
			const TokenKind after = next.kind;
			const bool line_start = i == 0 || tokens[i - 1].kind == TokenKind::Newline || tokens[i - 1].kind == TokenKind::Indent || tokens[i - 1].kind == TokenKind::Dedent;
			if (is_word(tokens[i], "pub") && (after == TokenKind::Static || after == TokenKind::Var || after == TokenKind::Const || after == TokenKind::Func || after == TokenKind::Signal || after == TokenKind::Enum || after == TokenKind::Class || is_word(next, "async"))) {
				erase_word(i);
			} else if (is_word(tokens[i], "async") && (after == TokenKind::Func || (line_start && (is_word(next, "set") || is_word(next, "get"))))) {
				erase_word(i);
			}
		}
	}

	template <typename Node>
	void visit(const Node &p_node) {
		using T = std::decay_t<Node>;
		if constexpr (std::is_same_v<T, Expr>) {
			expression(p_node, false);
			return;
		} else if constexpr (std::is_same_v<T, TypeRef>) {
			return; // Where it is declared.
		} else if constexpr (std::is_same_v<T, Annotation>) {
			if (p_node.name == "late") {
				erase_tokens(p_node.range.first, p_node.range.last);
			} else if (p_node.name.rfind("warning_ignore", 0) == 0) {
				warning_ignore(p_node);
			}
		} else if constexpr (std::is_same_v<T, VarDecl>) {
			for (const Annotation &annotation : p_node.annotations) {
				visit(annotation);
			}
			if (p_node.declared_type) {
				declared(*p_node.declared_type, p_node.type, tokens[p_node.name_token].end(), p_node.is_export);
			} else {
				inferred(p_node.name_token, p_node.type);
			}
			if (p_node.initializer) {
				visit(*p_node.initializer);
			}
			return;
		} else if constexpr (std::is_same_v<T, ParamDecl> || std::is_same_v<T, ConstDecl>) {
			if (p_node.declared_type) {
				declared(*p_node.declared_type, p_node.type, tokens[p_node.name_token].end(), false);
			} else if constexpr (std::is_same_v<T, ParamDecl>) {
				inferred(p_node.name_token, p_node.type);
			}
		} else if constexpr (std::is_same_v<T, FuncDecl>) {
			const FuncDecl *outer = function;
			function = &p_node;
			for (const Annotation &annotation : p_node.annotations) {
				visit(annotation);
			}
			for (const ParamDecl &param : p_node.params) {
				visit(param);
			}
			if (const std::optional<TypeRef> &ref = p_node.return_type_ref; ref && ref->range.first >= 2) {
				declared(*ref, p_node.return_type, tokens[ref->range.first - 2].end(), false);
			}
			for (const std::unique_ptr<Stmt> &stmt : p_node.body) {
				visit(*stmt);
			}
			function = outer;
			return;
		} else if constexpr (std::is_same_v<T, Stmt>) {
			const bool declares = p_node.kind == StmtKind::Var || p_node.kind == StmtKind::Const || p_node.kind == StmtKind::For;
			if (declares && function && p_node.local >= 0 && size_t(p_node.local) < function->locals.size()) {
				const Type &type = function->locals[size_t(p_node.local)].type;
				if (p_node.declared_type) {
					declared(*p_node.declared_type, type, tokens[p_node.name_token].end(), false);
				} else if (p_node.kind == StmtKind::Var) {
					inferred(p_node.name_token, type);
				}
			}
		} else if constexpr (std::is_same_v<T, Pattern>) {
			if (p_node.kind == PatternKind::Constructor) {
				refuse(p_node.range.first, std::string(NOT_YET) + "a pattern of None, Some(...), a struct or a variant.");
			}
		} else if constexpr (std::is_same_v<T, StructDecl>) {
			refuse(p_node.name_token, std::string(LACKS) + "struct.");
			return;
		} else if constexpr (std::is_same_v<T, InterfaceDecl>) {
			refuse(p_node.name_token, std::string(LACKS) + "interface.");
			return;
		} else if constexpr (std::is_same_v<T, EnumDecl>) {
			if (p_node.has_payloads) {
				refuse(p_node.name_token, std::string(LACKS) + "enum with payloads.");
				return;
			}
		} else if constexpr (std::is_same_v<T, ClassDecl>) {
			if (!p_node.implements.empty()) {
				refuse(p_node.implements[0].name_token, std::string(LACKS) + "interface.");
			}
			// @owner(T) and @parent(T) go, and `owner` is the Node GDScript has
			// (spec §7.3).
			for (const std::optional<TypeRef> *required : { &p_node.owner_ref, &p_node.parent_ref }) {
				if (*required && (*required)->range.first >= 3 && tokens[(*required)->range.first - 3].kind == TokenKind::At) {
					erase_tokens((*required)->range.first - 3, closing(tokens, (*required)->range.first - 1));
				}
			}
		}
		for_each_child(p_node, [&](const auto &p_child) { visit(p_child); });
	}

	// This language's names leave @warning_ignore, as GDScript rejects names
	// it does not know (modules/gdscript/gdscript_parser.cpp:5122-5129;
	// ed1daf0); with them all, the annotation goes.
	void warning_ignore(const Annotation &p_annotation) {
		std::vector<std::string> kept;
		for (const std::unique_ptr<Expr> &arg : p_annotation.args) {
			if (arg->kind != ExprKind::StringLiteral || !is_warning_name(arg->text)) {
				kept.push_back(source.text_of(arg->range));
			}
		}
		if (kept.size() == p_annotation.args.size()) {
			return;
		}
		if (kept.empty()) {
			erase_tokens(p_annotation.range.first, p_annotation.range.last);
			return;
		}
		std::string joined;
		for (const std::string &each : kept) {
			joined += (joined.empty() ? "" : ", ") + each;
		}
		erase(tokens[p_annotation.name_token + 1].end(), tokens[p_annotation.range.last].offset, joined);
	}

	// The text with the edits applied; nullopt where two overlap.
	std::optional<std::string> applied() {
		std::sort(edits.begin(), edits.end(), [](const Edit &p_a, const Edit &p_b) { return p_a.begin < p_b.begin; });
		std::string result;
		uint32_t done = 0;
		for (const Edit &edit : edits) {
			if (edit.begin < done) {
				return std::nullopt;
			}
			result += text.substr(done, edit.begin - done) + edit.text;
			done = edit.end;
		}
		return result + text.substr(done);
	}
};

// A tree as the re-parse check compares it: a line for each node, then a
// tab and the line it begins on, which the comparison skips. It leaves out
// what converting a file back always deletes — pub, async, the ? of types,
// how None is written, @late, @owner(T) and @parent(T), the root type of
// PackedScene, `:=` of members and this language's names in @warning_ignore —
// and what `erased` says, and names the paths of `renamed` by their new ones.
class Printer {
	const SourceFile &source;
	const Erased &erased;
	const std::map<std::string, std::string> &renamed;
	const std::string &path;
	int depth = 0;

	void line(const std::string &p_label, uint32_t p_token) {
		const int at = p_token < source.tokens.size() ? source.line(p_token) : 0;
		text += std::string(size_t(depth), ' ') + p_label + "\t" + std::to_string(at) + "\n";
	}

	template <typename Node>
	void children(const Node &p_node) {
		depth++;
		for_each_child(p_node, [&](const auto &p_child) { visit(p_child); });
		depth--;
	}

	std::string literal(const Expr &p_expr) const {
		const auto found = p_expr.kind == ExprKind::StringLiteral ? renamed.find(resolved_path(p_expr.text, path)) : renamed.end();
		if (found == renamed.end()) {
			return p_expr.text;
		}
		return p_expr.text.substr(0, p_expr.text.rfind('.')) + found->second.substr(found->second.rfind('.'));
	}

public:
	std::string text;

	Printer(const SourceFile &p_source, const Erased &p_erased, const std::map<std::string, std::string> &p_renamed, const std::string &p_path) :
			source(p_source), erased(p_erased), renamed(p_renamed), path(p_path) {}

	template <typename Node>
	void visit(const Node &p_node) {
		using T = std::decay_t<Node>;
		if constexpr (std::is_same_v<T, TypeRef>) {
			if (erased.types.count(p_node.name_token)) {
				return;
			}
			line("type " + p_node.name + (p_node.is_function ? " func" : ""), p_node.name_token);
			if (p_node.name != "PackedScene") {
				children(p_node);
			}
		} else if constexpr (std::is_same_v<T, Expr>) {
			const auto collapsed = p_node.kind == ExprKind::Call ? erased.calls.find(p_node.name_token) : erased.calls.end();
			const Expr *inner = collapsed == erased.calls.end() ? nullptr : (collapsed->second < 0 ? p_node.base.get() : (p_node.args.empty() ? nullptr : p_node.args[0].get()));
			if (inner) {
				visit(*inner);
				return;
			}
			char number[64];
			std::snprintf(number, sizeof(number), "%lld %.17g", static_cast<long long>(p_node.int_value), p_node.float_value);
			std::string label = "expr " + std::to_string(int(p_node.kind)) + " " + (p_node.kind == ExprKind::NoneLiteral ? std::string() : std::to_string(int(p_node.op))) + " " + literal(p_node) + " " + number + (p_node.bool_value ? " true" : "");
			for (const std::vector<std::string> *names : { &p_node.arg_names, &p_node.member_names }) {
				for (const std::string &name : *names) {
					label += " " + name + "=";
				}
			}
			line(label, p_node.range.first);
			children(p_node);
		} else if constexpr (std::is_same_v<T, Stmt>) {
			line("stmt " + std::to_string(int(p_node.kind)) + " " + p_node.name + " " + std::to_string(int(p_node.assign_op)), p_node.range.first);
			children(p_node);
		} else if constexpr (std::is_same_v<T, Pattern>) {
			line("pattern " + std::to_string(int(p_node.kind)) + " " + p_node.name + " " + p_node.field + (p_node.rest ? " .." : ""), p_node.range.first);
			children(p_node);
		} else if constexpr (std::is_same_v<T, MatchBranch>) {
			line("branch", p_node.range.first);
			children(p_node);
		} else if constexpr (std::is_same_v<T, Annotation>) {
			if (p_node.name == "late") {
				return;
			}
			const bool ignore = p_node.name.rfind("warning_ignore", 0) == 0;
			std::vector<const Expr *> args;
			for (const std::unique_ptr<Expr> &arg : p_node.args) {
				if (!ignore || arg->kind != ExprKind::StringLiteral || !is_warning_name(arg->text)) {
					args.push_back(arg.get());
				}
			}
			if (ignore && args.empty() && !p_node.args.empty()) {
				return;
			}
			line("@" + p_node.name, p_node.name_token);
			depth++;
			for (const Expr *arg : args) {
				visit(*arg);
			}
			depth--;
		} else if constexpr (std::is_same_v<T, VarDecl>) {
			line(std::string("var ") + p_node.name + (p_node.is_static ? " static" : "") + (p_node.is_onready ? " onready" : "") + " set=" + p_node.setter + " get=" + p_node.getter, p_node.name_token);
			children(p_node);
		} else if constexpr (std::is_same_v<T, ConstDecl> || std::is_same_v<T, SignalDecl> || std::is_same_v<T, EnumDecl> || std::is_same_v<T, EnumValueDecl> || std::is_same_v<T, StructDecl> || std::is_same_v<T, InterfaceDecl> || std::is_same_v<T, InterfaceProperty>) {
			line("decl " + p_node.name, p_node.name_token);
			children(p_node);
		} else if constexpr (std::is_same_v<T, ParamDecl>) {
			line("param " + p_node.name + (p_node.is_rest ? " ..." : ""), p_node.name_token);
			children(p_node);
		} else if constexpr (std::is_same_v<T, FuncDecl>) {
			line("func " + p_node.name + (p_node.is_static ? " static" : "") + (p_node.has_body ? "" : " bodiless") + " of=" + p_node.accessor_of, p_node.name_token);
			children(p_node);
		} else if constexpr (std::is_same_v<T, ClassDecl>) {
			line("class " + p_node.name + " " + p_node.class_name + " extends " + p_node.extends + (p_node.is_abstract ? " abstract" : "") + (p_node.static_unload ? " static_unload" : "") + " " + p_node.icon, p_node.name_token);
			depth++;
			for_each_child(p_node, [&](const auto &p_child) {
				if constexpr (std::is_same_v<std::decay_t<decltype(p_child)>, TypeRef>) {
					if ((p_node.owner_ref && &p_child == &*p_node.owner_ref) || (p_node.parent_ref && &p_child == &*p_node.parent_ref)) {
						return;
					}
				}
				visit(p_child);
			});
			depth--;
		}
	}
};

// A class-level access from one .pet file to the class of another: a call of
// its static method, a use of its enum or inner class, or of its constant in
// a constant, by where it is written.
struct Access {
	std::string target;
	std::string written;
	bool is_static = false;
	bool in_constant = false;
	uint32_t begin = 0;
	uint32_t end = 0;
};

// The class-level accesses of the file to the .pet files that `p_names`
// binds to a global class or an autoload (the latter with a `*` before its
// path), and that its constants bind by preload() or load() of one path.
std::vector<Access> class_accesses(const std::string &p_file, const ClassDecl &p_header, const std::map<std::string, std::string> &p_names, ScriptClasses &r_classes) {
	const std::vector<Token> &tokens = p_header.source->tokens;
	std::map<std::string, std::string> names = p_names;
	for (const ConstDecl &constant : p_header.constants) {
		const Expr *value = constant.initializer.get();
		if (value && value->kind == ExprKind::Call && (value->text == "preload" || value->text == "load") && value->args.size() == 1 && value->args[0]->kind == ExprKind::StringLiteral) {
			names[constant.name] = resolved_path(value->args[0]->text, p_file);
		}
	}
	std::vector<Access> found;
	bool in_constant = false;
	for (size_t i = 0; i + 2 < tokens.size(); i++) {
		const TokenKind kind = tokens[i].kind;
		in_constant = kind == TokenKind::Const || (in_constant && kind != TokenKind::Newline && kind != TokenKind::Semicolon);
		const auto named = kind == TokenKind::Identifier && (i == 0 || tokens[i - 1].kind != TokenKind::Period) && tokens[i + 1].kind == TokenKind::Period ? names.find(tokens[i].text) : names.end();
		const bool autoload = named != names.end() && named->second.front() == '*';
		const std::string target = named == names.end() ? std::string() : named->second.substr(autoload ? 1 : 0);
		const ClassDecl *decl = target.empty() || target == p_file || !is_script_path(target) ? nullptr : r_classes.header(target);
		if (!decl) {
			continue;
		}
		const std::string &member = tokens[i + 2].text;
		const bool is_static = !autoload && std::any_of(decl->functions.begin(), decl->functions.end(), [&](const FuncDecl &p_function) { return p_function.name == member && p_function.is_static; });
		const bool type = decl->find_class(member) || std::any_of(decl->enums.begin(), decl->enums.end(), [&](const EnumDecl &p_enum) { return p_enum.name == member; });
		const bool constant = in_constant && std::any_of(decl->constants.begin(), decl->constants.end(), [&](const ConstDecl &p_constant) { return p_constant.name == member; });
		if (is_static || type || constant) {
			found.push_back({ target, tokens[i].text + "." + member + (is_static ? "()" : ""), is_static, constant && !type, tokens[i].offset, tokens[i + 2].end() });
		}
	}
	return found;
}

} // namespace

std::string reparse_difference(const std::string &p_path, const std::string &p_source, const Erased &p_erased, const std::string &p_gdscript, const std::map<std::string, std::string> &p_renamed) {
	std::vector<Diagnostic> errors;
	const ClassDecl pet = parse(p_source, errors);
	errors.clear();
	const ClassDecl gdscript = parse(p_gdscript, errors);
	if (!errors.empty()) {
		return "the GDScript does not parse at line " + std::to_string(gdscript.source->position(errors[0].begin).line) + ": " + errors[0].message;
	}
	Printer before(*pet.source, p_erased, p_renamed, p_path);
	before.visit(pet);
	const Erased none;
	Printer after(*gdscript.source, none, p_renamed, reversed_path(p_path));
	after.visit(gdscript);
	size_t a = 0;
	size_t b = 0;
	while (a < before.text.size() || b < after.text.size()) {
		const size_t a_end = std::min(before.text.find('\n', a), before.text.size());
		const size_t b_end = std::min(after.text.find('\n', b), after.text.size());
		const std::string left = before.text.substr(a, a_end - a);
		const std::string right = after.text.substr(b, b_end - b);
		const size_t left_tab = left.rfind('\t');
		const size_t right_tab = right.rfind('\t');
		if (left.substr(0, left_tab) != right.substr(0, right_tab)) {
			const auto shown = [](const std::string &p_line, size_t p_tab) {
				return p_line.empty() ? std::string("nothing") : "\"" + p_line.substr(p_line.find_first_not_of(' '), p_tab - p_line.find_first_not_of(' ')) + "\" at line " + p_line.substr(p_tab + 1);
			};
			return "its tree has " + shown(left, left_tab) + ", the GDScript's " + shown(right, right_tab);
		}
		a = a_end + 1;
		b = b_end + 1;
	}
	return std::string();
}

Reversal reverse(const EngineApi &p_engine, const ProjectApi &p_project, const std::vector<std::string> &p_paths) {
	Reversal result;
	ScriptClasses classes(p_engine, &p_project);
	std::vector<std::string> files;
	for (const std::string &path : p_project.script_paths()) {
		if (is_script_path(path)) {
			files.push_back(path);
		}
	}
	std::sort(files.begin(), files.end());
	std::set<std::string> asked;
	for (const std::string &path : p_paths) {
		const std::string directory = path.back() == '/' ? path : path + "/";
		const auto under = [&](const std::string &p_file) { return p_file == path || p_file.rfind(directory, 0) == 0; };
		const size_t before = asked.size();
		std::copy_if(files.begin(), files.end(), std::inserter(asked, asked.end()), under);
		if (asked.size() == before && std::none_of(files.begin(), files.end(), under)) {
			result.unknown.push_back(path);
		}
	}
	if (p_paths.empty()) {
		asked.insert(files.begin(), files.end());
	}

	// The inheritance trees among the .pet files.
	std::map<std::string, std::string> up;
	const auto tree = [&](std::string p_file) {
		while (up.count(p_file) && up[p_file] != p_file) {
			p_file = up[p_file];
		}
		return p_file;
	};
	for (const std::string &file : files) {
		const ClassDecl *decl = classes.header(file);
		const auto extend = [&](const ClassDecl &p_class, const std::string &p_path, const auto &p_self) -> void {
			const std::string base = script_file(classes.base_path(p_class, p_path));
			if (std::binary_search(files.begin(), files.end(), base) && tree(base) != tree(file)) {
				up[tree(base)] = tree(file);
			}
			for (const std::unique_ptr<ClassDecl> &inner : p_class.classes) {
				p_self(*inner, p_path + "::" + inner->name, p_self);
			}
		};
		if (decl) {
			extend(*decl, file, extend);
		}
	}
	std::map<std::string, std::vector<std::string>> trees;
	for (const std::string &file : files) {
		trees[tree(file)].push_back(file);
	}

	// Each file converted once, when its tree is first asked about.
	struct Converted {
		std::string text;
		Erased erased;
		std::vector<Diagnostic> refused;
	};
	std::map<std::string, Converted> converted;
	const auto convert = [&](const std::string &p_file) -> const Converted & {
		const auto found = converted.find(p_file);
		if (found != converted.end()) {
			return found->second;
		}
		const std::string source = p_project.script_source(p_file).value_or(std::string());
		std::vector<Diagnostic> errors;
		ClassDecl decl = parse(source, errors);
		Converter converter(p_file, *decl.source);
		if (p_project.script_source(reversed_path(p_file))) {
			converter.refused.push_back({ 0, 0, reversed_path(p_file) + " is there already." });
		} else if (errors.empty()) {
			CompileOptions options;
			options.path = p_file;
			options.project = &p_project;
			classes.begin_compile(p_file, source);
			check(decl, p_engine, options, errors, classes);
			if (errors.empty()) {
				converter.keywords();
				converter.visit(decl);
			}
			classes.end_compile();
		}
		if (!errors.empty()) {
			converter.refused.push_back({ errors[0].begin, errors[0].end, "It has compile errors, the first: " + errors[0].message });
		}
		std::stable_sort(converter.refused.begin(), converter.refused.end(), [](const Diagnostic &p_a, const Diagnostic &p_b) { return p_a.begin < p_b.begin; });
		for (Diagnostic &refusal : converter.refused) {
			const SourcePosition position = decl.source->position(refusal.begin);
			refusal.line = refusal.line ? refusal.line : position.line;
			refusal.column = refusal.column ? refusal.column : position.column;
			refusal.file = p_file;
		}
		const std::optional<std::string> text = converter.applied();
		if (!text && converter.refused.empty()) {
			result.defects.push_back({ 0, 0, "Two of its erasures overlap." });
			result.defects.back().file = p_file;
		}
		return converted[p_file] = { text.value_or(std::string()), converter.erased, converter.refused };
	};
	// The file of a tree that cannot convert, or "".
	const auto refusing = [&](const std::string &p_root) {
		for (const std::string &file : trees[p_root]) {
			if (!convert(file).refused.empty()) {
				return file;
			}
		}
		return std::string();
	};

	// The trees asked for, and those that the files converted access at class
	// level, which GDScript cannot do to a class of this language (spec
	// §11.3): such a tree converts too, or else keeps the file's.
	std::map<std::string, std::string> names; // Global classes and autoloads, the latter after a `*`.
	for (const auto &[name, path] : p_project.global_class_paths()) {
		names[name] = path;
	}
	for (const std::string &name : p_project.autoload_names()) {
		names[name] = "*" + p_project.autoload_path(name);
	}
	std::map<std::string, std::string> taken; // By their roots, with why; "" when asked for.
	std::map<std::string, std::pair<std::string, std::string>> held; // By their roots: the file that keeps each, and why.
	for (const std::string &file : asked) {
		taken.emplace(tree(file), std::string());
	}
	for (bool more = true; more;) {
		more = false;
		for (const auto &[root, pulled] : std::map<std::string, std::string>(taken)) {
			std::string why;
			std::string keeping = refusing(root);
			if (!keeping.empty()) {
				why = "it cannot convert, as listed";
			}
			for (const std::string &file : keeping.empty() ? trees[root] : std::vector<std::string>()) {
				for (const Access &access : class_accesses(file, *classes.header(file), names, classes)) {
					const std::string target = tree(access.target);
					if (taken.count(target)) {
						continue;
					}
					const std::string said = file + " uses " + access.written + " of it";
					if (!held.count(target) && refusing(target).empty()) {
						taken.emplace(target, said);
					} else {
						keeping = file;
						why = "it uses " + access.written + " of " + access.target + ", which stays";
					}
					more = true;
					break;
				}
				if (more) {
					break;
				}
			}
			if (!keeping.empty()) {
				held[root] = { keeping, why };
				taken.erase(root);
				more = true;
			}
			if (more) {
				break;
			}
		}
	}
	const auto mark = [&](const std::string &p_root, const std::string &p_keeping, const std::string &p_why) {
		for (const std::string &file : trees[p_root]) {
			const Converted &each = convert(file);
			if (file == p_keeping) {
				result.left_out.emplace_back(file, p_why);
				result.refused.insert(result.refused.end(), each.refused.begin(), each.refused.end());
			} else {
				result.left_out.emplace_back(file, "in the inheritance tree of " + p_keeping + ", which stays");
			}
		}
	};
	for (const auto &[root, keeping] : held) {
		mark(root, keeping.first, keeping.second);
	}
	std::sort(result.left_out.begin(), result.left_out.end());
	for (const auto &[root, why] : taken) {
		const std::vector<std::string> &members = trees[root];
		const auto mine = std::find_if(members.begin(), members.end(), [&](const std::string &p_file) { return asked.count(p_file) > 0; });
		for (const std::string &file : members) {
			result.texts[file] = convert(file).text;
			result.erased[file] = convert(file).erased;
			if (!asked.count(file)) {
				result.added.emplace_back(file, mine == members.end() ? why : "in the inheritance tree of " + *mine);
			}
		}
	}

	std::map<std::string, std::string> renamed;
	for (const auto &[file, text] : result.texts) {
		renamed[file] = reversed_path(file);
	}
	for (auto &[file, text] : result.texts) {
		int count = 0;
		text = renamed_in_script(text, reversed_path(file), renamed, count);
		const std::string difference = reparse_difference(file, p_project.script_source(file).value_or(std::string()), result.erased[file], text, renamed);
		if (!difference.empty()) {
			result.defects.push_back({ 0, 0, "Converting it back would change its meaning: " + difference + "." });
			result.defects.back().file = file;
		}
	}

	// What the .pet files that stay lose of the classes that convert, of
	// which this language reaches only static methods and constants' values
	// (spec §11.1).
	for (const std::string &file : files) {
		const ClassDecl *header = renamed.count(file) ? nullptr : classes.header(file);
		for (const Access &access : header ? class_accesses(file, *header, names, classes) : std::vector<Access>()) {
			if (!renamed.count(access.target) || access.is_static) {
				continue;
			}
			Diagnostic lost{ access.begin, access.end, "It uses " + access.written + " of " + access.target + (access.in_constant ? " in a constant" : "") + ", which converts to GDScript, and this language cannot use a class of GDScript so." };
			const SourcePosition position = header->source->position(lost.begin);
			lost.line = position.line;
			lost.column = position.column;
			lost.severity = Severity::Warning;
			lost.file = file;
			result.lost.push_back(std::move(lost));
		}
	}
	return result;
}

} // namespace petrichor::compiler
