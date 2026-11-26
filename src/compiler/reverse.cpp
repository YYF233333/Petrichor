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

// Whether an expression stays one operand wherever it is written: in
// parentheses, or of a kind without an operator outside them.
bool atomic(const std::vector<Token> &p_tokens, const Expr &p_expr) {
	if (p_tokens[p_expr.range.first].kind == TokenKind::ParenOpen && closing(p_tokens, p_expr.range.first) == p_expr.range.last) {
		return true;
	}
	switch (p_expr.kind) {
		case ExprKind::Unary:
		case ExprKind::Binary:
		case ExprKind::Cast:
		case ExprKind::TypeTest:
		case ExprKind::Conditional:
		case ExprKind::Await:
		case ExprKind::Lambda:
			return false;
		default:
			return true;
	}
}

// A typed container of typed containers, whose elements GDScript reads as
// untyped containers, since it writes only the outer type (spec §10.1).
bool nested(const Type &p_type) {
	return is_typed_container(p_type) && !p_type.args.empty() && is_typed_container(p_type.args.back());
}

// What GDScript lacks of a type (spec §13.4), or "".
std::string lacking(const Type &p_type) {
	if (p_type.is_two_level()) {
		return std::string(LACKS) + "T?? or Variant?, whose two Nones are one null there.";
	}
	switch (p_type.kind) {
		case TypeKind::Struct:
			// What awaiting a signal with several parameters gives is an Array
			// there (spec §9.6).
			if (p_type.is_arguments()) {
				break;
			}
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
	const EngineApi &engine;
	const std::set<std::string> &globals; // The global classes' names.
	const FuncDecl *function = nullptr; // Whose locals the statements declare.
	const ClassDecl *klass = nullptr; // Whose members the names read.
	bool constant = false; // In a constant's value.
	std::set<const Expr *> settled; // Task values awaited or detached.
	// Calls that a statement after theirs takes apart: expect(), whose message
	// goes to an assert, and new() or instantiate() with members by name.
	std::set<const Expr *> split;
	std::set<uint32_t> null_kept; // Some(p) patterns whose p takes no null: a branch of None comes before, or a guard tests it.
	std::set<std::pair<const FuncDecl *, int>> wide; // Locals that GDScript holds untyped or as untyped containers.
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
	// a PackedScene deleted (spec §1.2, §7.4). A function type is Callable
	// there (spec §3.11); a nested container has only its outer type, and a
	// container of a value type's T? none (spec §10.1).
	void written(const TypeRef &p_ref, const Type &p_type) {
		if (p_ref.is_function) {
			erase(tokens[p_ref.range.first].offset, tokens[p_ref.range.last].end(), "Callable");
			erased.types[p_ref.name_token] = "Callable";
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
		if (p_ref.args.empty()) {
			return;
		}
		const bool untyped = std::any_of(p_type.args.begin(), p_type.args.end(), [](const Type &p_arg) { return p_arg.kind == TypeKind::Option; });
		if (p_ref.name == "PackedScene" || untyped) {
			const uint32_t open = opening(p_ref);
			erase(tokens[open].offset, tokens[closing(tokens, open)].end());
			for (const TypeRef &arg : untyped ? p_ref.args : std::vector<TypeRef>()) {
				erased.types[arg.name_token] = "";
			}
			return;
		}
		for (size_t i = 0; i < p_ref.args.size() && i < p_type.args.size(); i++) {
			const TypeRef &arg = p_ref.args[i];
			if (!is_typed_container(p_type.args[i]) || arg.args.empty()) {
				written(arg, p_type.args[i]);
				continue;
			}
			const uint32_t open = opening(arg);
			erase(tokens[open].offset, tokens[closing(tokens, open)].end());
			for (const TypeRef &each : arg.args) {
				erased.types[each.name_token] = "";
			}
		}
	}

	// The bracket after a type's name, as in `Combat.Damageable[`.
	uint32_t opening(const TypeRef &p_ref) const {
		return p_ref.name_token + 2 * uint32_t(std::count(p_ref.name.begin(), p_ref.name.end(), '.')) + 1;
	}

	// The type written for a declaration, from the byte `p_from` on: a value
	// type's T? is no type in GDScript (spec §10.1), so it goes; exported, it
	// is Variant, which GDScript exports as this language exports it
	// (src/adapter/pet_script.cpp:85; modules/gdscript/gdscript_parser.cpp:4850-4854,
	// ed1daf0).
	void declared(const TypeRef &p_ref, const Type &p_type, uint32_t p_from, bool p_exported) {
		const std::string lacks = lacking(p_type);
		if (!lacks.empty()) {
			refuse(p_ref.name_token, lacks);
			return;
		}
		if (p_type.kind != TypeKind::Option) {
			written(p_ref, p_type);
		} else if (p_exported) {
			erase(tokens[p_ref.range.first].offset, tokens[p_ref.range.last].end(), "Variant");
			erased.types[p_ref.name_token] = "Variant";
		} else {
			erase(p_from, tokens[p_ref.range.last].end());
			erased.types[p_ref.name_token] = "";
		}
	}

	// `name := value`: `=` of a value type's T? and of what awaiting a signal
	// with several parameters gives, which GDScript cannot infer. A value
	// GDScript reads untyped where this language has its type, it cannot
	// infer either (modules/gdscript/gdscript_analyzer.cpp:2140-2142; ed1daf0).
	void inferred(uint32_t p_name, const Type &p_type, const Expr *p_value, bool p_exported) {
		const std::string lacks = lacking(p_type);
		if (!lacks.empty()) {
			refuse(p_name, lacks);
			return;
		}
		if (tokens[p_name + 1].kind != TokenKind::Colon || tokens[p_name + 2].kind != TokenKind::Equal) {
			return;
		}
		if (p_type.kind == TypeKind::Option || p_type.is_arguments()) {
			if (p_exported) {
				refuse(p_name, std::string(NOT_YET) + "an export of a value type's T? written with :=, which GDScript exports with the value's type.");
			} else {
				erase(tokens[p_name + 1].offset, tokens[p_name + 1].end());
			}
		} else if (p_value && untyped(*p_value)) {
			refuse(p_name, std::string(NOT_YET) + "`:=` of a value that GDScript reads untyped, here " + p_type.to_string() + ".");
		}
	}

	// Whether GDScript reads an expression untyped, or as an untyped container
	// where this language has a typed one: a value type's T? (spec §10.1), an
	// element of a nested container, and what operators make of them, but for
	// comparisons with null and formatting (modules/gdscript/gdscript_analyzer.cpp:3166-3180;
	// ed1daf0).
	bool untyped(const Expr &p_expr) const {
		if (p_expr.type.kind == TypeKind::Option) {
			return true;
		}
		switch (p_expr.kind) {
			case ExprKind::Subscript:
				return p_expr.type.kind != TypeKind::Variant && (nested(p_expr.base->type) || untyped(*p_expr.base));
			case ExprKind::Name:
				if (p_expr.target == Target::Member) {
					// Declared `var m = v`, which GDScript leaves untyped.
					const VarDecl *member = this->member(p_expr.text);
					return member && !member->declared_type && !member->infers_type;
				}
				return p_expr.target == Target::Local && wide.count({ function, p_expr.index });
			case ExprKind::Unary:
				return untyped(*p_expr.left);
			case ExprKind::Binary:
				if (p_expr.left->kind == ExprKind::NoneLiteral || p_expr.right->kind == ExprKind::NoneLiteral || (p_expr.op == TokenKind::Percent && p_expr.left->type.kind == TypeKind::String)) {
					return false;
				}
				return untyped(*p_expr.left) || untyped(*p_expr.right);
			case ExprKind::Call:
				// A function type is Callable there, whose call() gives Variant.
				if (p_expr.target == Target::CallValue) {
					return p_expr.type.kind != TypeKind::Variant;
				}
				// The conditional expressions that option_method() writes give
				// the type of both values, or else Variant (:5189-5202).
				if (p_expr.target != Target::LanguageMethod || p_expr.language->of != LanguageFunction::Of::Option) {
					return false;
				}
				if (p_expr.text == "unwrap" || p_expr.text == "expect" || p_expr.text == "unwrap_or" || p_expr.text == "or" || p_expr.text == "filter") {
					return untyped(*p_expr.base);
				}
				return true;
			default:
				return false;
		}
	}

	const VarDecl *member(const std::string &p_name) const {
		const std::vector<const VarDecl *> none;
		const std::vector<const VarDecl *> &members = klass ? klass->all_members : none;
		const auto found = std::find_if(members.begin(), members.end(), [&](const VarDecl *p_member) { return p_member->name == p_name; });
		return found == members.end() ? nullptr : *found;
	}

	// Whether a name reads a value without calling anything: a local, or a
	// member of this class without a getter.
	bool plain(const Expr &p_expr) const {
		if (p_expr.kind != ExprKind::Name || p_expr.range.first != p_expr.range.last) {
			return false;
		}
		const VarDecl *read = p_expr.target == Target::Member ? member(p_expr.text) : nullptr;
		return p_expr.target == Target::Local || (read && read->getter.empty() && read->getter_index < 0);
	}

	// Whether evaluating an expression cannot be observed, so that GDScript
	// may evaluate it only where it uses it: a literal, self, a lambda, Some
	// of such a value, a name that reads without calling anything, a constant,
	// a method or a class as a value, and operators over those but / and %,
	// which fail on a zero.
	bool unobserved(const Expr &p_expr) const {
		switch (p_expr.kind) {
			case ExprKind::IntLiteral:
			case ExprKind::FloatLiteral:
			case ExprKind::StringLiteral:
			case ExprKind::BoolLiteral:
			case ExprKind::NoneLiteral:
			case ExprKind::Self:
			case ExprKind::Lambda:
				return true;
			case ExprKind::Name:
			case ExprKind::Member:
				if (plain(p_expr)) {
					return true;
				}
				switch (p_expr.target) {
					case Target::Constant:
					case Target::EngineConstant:
					case Target::BuiltinConstant:
					case Target::MethodValue:
					case Target::EngineMethodValue:
					case Target::StaticValue:
					case Target::EngineStaticValue:
					case Target::NewValue:
					case Target::ClassScript:
						// The base of a member is a type, or a value read.
						return !p_expr.base || (p_expr.base->kind == ExprKind::Name && (p_expr.base->target == Target::None || p_expr.base->target == Target::ClassScript)) || unobserved(*p_expr.base);
					default:
						return false;
				}
			case ExprKind::Unary:
			case ExprKind::Cast:
			case ExprKind::TypeTest:
				return unobserved(*p_expr.left);
			case ExprKind::Binary:
				return p_expr.op != TokenKind::Slash && p_expr.op != TokenKind::Percent && unobserved(*p_expr.left) && unobserved(*p_expr.right);
			case ExprKind::Conditional:
				return unobserved(*p_expr.left) && unobserved(*p_expr.base) && unobserved(*p_expr.right);
			case ExprKind::Call:
				return p_expr.target == Target::Some && unobserved(*p_expr.args[0]);
			default:
				return false;
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
		const bool kept = p_operand && !atomic(tokens, *p_call.args[0]);
		erase(tokens[p_call.name_token].offset, kept ? tokens[open].offset : tokens[open].end());
		if (!kept) {
			erase(tokens[close].offset, tokens[close].end());
		}
	}

	// A call written as GDScript `p_before`, its one argument, then `p_after`:
	// the tokens before the argument and after it replaced, the argument in
	// parentheses unless it stays one operand, and the whole where it is an
	// operand.
	void around(const Expr &p_call, const std::string &p_before, const std::string &p_after, bool p_operand) {
		const Expr &arg = *p_call.args[0];
		const bool own = !atomic(tokens, arg);
		const uint32_t open = p_call.name_token + 1;
		const uint32_t close = closing(tokens, open);
		erase(tokens[p_call.range.first].offset, tokens[open].end(), std::string(p_operand ? "(" : "") + p_before + (own ? "(" : ""));
		erase(tokens[close].offset, tokens[close].end(), std::string(own ? ")" : "") + p_after + (p_operand ? ")" : ""));
		erased.rewrites[p_call.name_token] = { p_before + "(__0)" + p_after, { arg.range } };
	}

	// The methods of T? but unwrap() and expect() on a name, as GDScript's
	// conditional expression (architecture §6.6); other receivers stay, and
	// so do arguments that GDScript would not always evaluate where it could
	// be seen, as this language evaluates the arguments of a call (spec §3.4).
	void option_method(const Expr &p_call, bool p_operand) {
		const Expr *receiver = p_call.base.get();
		if (!receiver || !plain(*receiver) || p_call.args.size() != 1) {
			refuse(p_call.name_token, std::string(NOT_YET) + p_call.text + "() of T? on more than a name.");
			return;
		}
		if (!unobserved(*p_call.args[0])) {
			refuse(p_call.name_token, std::string(NOT_YET) + p_call.text + "() of T? whose argument may have effects, which GDScript would evaluate only where it uses it.");
			return;
		}
		const std::string x = receiver->text;
		const std::string &method = p_call.text;
		if (method == "unwrap_or" || method == "or") {
			around(p_call, x + " if " + x + " != null else ", "", p_operand);
		} else if (method == "unwrap_or_else" || method == "or_else") {
			around(p_call, x + " if " + x + " != null else ", ".call()", p_operand);
		} else if (method == "map" || method == "and_then") {
			around(p_call, "", ".call(" + x + ") if " + x + " != null else null", p_operand);
		} else if (method == "filter") {
			around(p_call, x + " if " + x + " != null and ", ".call(" + x + ") else null", p_operand);
		} else {
			refuse(p_call.name_token, std::string(NOT_YET) + method + "() of T?.");
		}
	}

	// How GDScript names a type, or "": a class of this language needs its
	// global name.
	std::string named(const Type &p_type) const {
		switch (p_type.kind) {
			case TypeKind::Object:
				return p_type.args.empty() && (engine.class_exists(p_type.name) || globals.count(p_type.name)) ? p_type.name : std::string();
			case TypeKind::Builtin: {
				if (p_type.args.empty()) {
					return p_type.name;
				}
				std::string text = p_type.name + "[";
				for (size_t i = 0; i < p_type.args.size(); i++) {
					const std::string arg = is_typed_container(p_type.args[i]) ? p_type.args[i].name : named(p_type.args[i]);
					if (arg.empty()) {
						return std::string();
					}
					text += (i > 0 ? ", " : "") + arg;
				}
				return text + "]";
			}
			case TypeKind::Bool:
			case TypeKind::Int:
			case TypeKind::Float:
			case TypeKind::String:
			case TypeKind::Variant:
				return p_type.to_string();
			default:
				return std::string();
		}
	}

	// A literal of a typed container that GDScript makes untyped where it
	// knows no type for it, as an element of a nested container or a value
	// for an untyped object: cast to its type, which types it there
	// (modules/gdscript/gdscript_analyzer.cpp:3808-3815; ed1daf0).
	void typed_literal(const Expr &p_value) {
		const bool dictionary = p_value.kind == ExprKind::DictionaryLiteral && !p_value.bool_value;
		if ((p_value.kind != ExprKind::ArrayLiteral && !dictionary) || !is_typed_container(p_value.type)) {
			return;
		}
		const std::string type = named(p_value.type);
		if (type.empty()) {
			refuse(p_value.range.first, std::string(NOT_YET) + "a literal of " + p_value.type.to_string() + " where GDScript knows no type for it.");
			return;
		}
		std::string gdscript;
		std::vector<TokenRange> operands;
		for (size_t i = 0; i < p_value.args.size(); i++) {
			gdscript += std::string(i == 0 ? "" : (dictionary && i % 2 ? ": " : ", ")) + "__" + std::to_string(i);
			operands.push_back(p_value.args[i]->range);
		}
		erase(tokens[p_value.range.last].end(), tokens[p_value.range.last].end(), " as " + type);
		erased.rewrites[p_value.range.first] = { (dictionary ? "{" : "[") + gdscript + (dictionary ? "}" : "]") + " as " + type, operands };
	}

	void expression(const Expr &p_expr, bool p_operand) {
		const std::string lacks = lacking(p_expr.type);
		if (!lacks.empty() && (p_expr.type.kind != TypeKind::Task || !settled.count(&p_expr))) {
			refuse(p_expr.range.first, lacks);
		}
		const size_t before = edits.size();
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
			case ExprKind::Member:
				// A field of what awaiting a signal with several parameters gives,
				// which is an Array there (spec §9.6).
				if (p_expr.target == Target::StructField && p_expr.base->type.is_arguments()) {
					const std::string index = "[" + std::to_string(p_expr.index) + "]";
					erase(tokens[p_expr.name_token - 1].offset, tokens[p_expr.name_token].end(), index);
					erased.rewrites[p_expr.name_token] = { "__0" + index, { p_expr.base->range } };
				}
				break;
			case ExprKind::ArrayLiteral:
				for (size_t i = 0; nested(p_expr.type) && i < p_expr.args.size(); i++) {
					typed_literal(*p_expr.args[i]);
				}
				break;
			case ExprKind::Lambda:
				visit(*p_expr.function);
				return;
			case ExprKind::Name:
				// GDScript finds an outer class's static members only through its
				// name (modules/gdscript/gdscript_analyzer.cpp:4228-4256, 4270-4275;
				// ed1daf0).
				if (p_expr.target == Target::StaticVar && klass && std::none_of(klass->all_static_vars.begin(), klass->all_static_vars.end(), [&](const VarDecl *p_var) { return p_var->name == p_expr.text; })) {
					refuse(p_expr.range.first, std::string(NOT_YET) + "a static member of an outer class by its name alone, which GDScript does not find.");
				}
				break;
			case ExprKind::Call:
				if (!p_expr.base && (p_expr.target == Target::ScriptFunction || p_expr.target == Target::ScriptMethod) && klass && std::none_of(klass->all_functions.begin(), klass->all_functions.end(), [&](const FuncDecl *p_function) { return p_function->name == p_expr.text; })) {
					refuse(p_expr.name_token, std::string(NOT_YET) + "a static member of an outer class by its name alone, which GDScript does not find.");
				}
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
		// What a statement after this one takes is moved as it is written.
		if (split.count(&p_expr)) {
			for (size_t i = before; i < edits.size(); i++) {
				const auto moved = [&](const std::unique_ptr<Expr> &p_value) { return edits[i].begin >= tokens[p_value->range.first].offset && edits[i].end <= tokens[p_value->range.last].end(); };
				if (std::any_of(p_expr.args.begin(), p_expr.args.end(), moved) || std::any_of(p_expr.member_values.begin(), p_expr.member_values.end(), moved)) {
					refuse(p_expr.name_token, std::string(NOT_YET) + "a value moved to a statement after this one that converting changes.");
					break;
				}
			}
		}
	}

	void call(const Expr &p_call, bool p_operand) {
		if (!p_call.member_names.empty() && !split.count(&p_call)) {
			refuse(p_call.member_name_tokens[0], std::string(NOT_YET) + "members passed by name to new() or instantiate() but where they declare a variable.");
		}
		if (p_call.base && (nested(p_call.base->type) || untyped(*p_call.base))) {
			for (const std::unique_ptr<Expr> &arg : p_call.args) {
				typed_literal(*arg);
			}
		}
		switch (p_call.target) {
			case Target::LanguageMethod:
				if (p_call.language->of != LanguageFunction::Of::Option) {
					break;
				}
				if (p_call.text == "unwrap" || (p_call.text == "expect" && split.count(&p_call))) {
					collapse(p_call, -1, p_operand);
				} else if (p_call.text == "expect") {
					refuse(p_call.name_token, std::string(NOT_YET) + "expect() but as the whole value of a variable declared or a name assigned.");
				} else {
					option_method(p_call, p_operand);
				}
				break;
			case Target::TaskDetach:
				settled.insert(p_call.base.get());
				collapse(p_call, -1, p_operand);
				break;
			case Target::Some:
				collapse(p_call, 0, p_operand);
				break;
			case Target::ConstructFromVariant:
				// Array(x) of a signal's arguments, which are an Array there.
				if (p_call.args.size() == 1 && p_call.args[0]->type.is_arguments()) {
					collapse(p_call, 0, p_operand);
				}
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
				// GDScript casts an int to an enum unchecked, which keeps a value
				// of the enum (modules/gdscript/gdscript_analyzer.cpp:3826-3828;
				// ed1daf0), but in a constant.
				if (constant) {
					refuse(p_call.name_token, std::string(NOT_YET) + "the conversion of an int to an enum in a constant.");
				} else {
					around(p_call, "", " as " + text.substr(tokens[p_call.range.first].offset, tokens[p_call.name_token].end() - tokens[p_call.range.first].offset), p_operand);
				}
				break;
			case Target::Encode: {
				// An untyped copy, as Array(base, type, class, script) of no type
				// makes (core/variant/array.cpp:874-879, core/variant/dictionary.cpp:753-758; ed1daf0).
				const std::string untyped = std::string(", TYPE_NIL, &\"\", null") + (p_call.text == "Dictionary" ? ", TYPE_NIL, &\"\", null" : "");
				const uint32_t close = closing(tokens, p_call.name_token + 1);
				erase(tokens[close].offset, tokens[close].offset, untyped);
				erased.rewrites[p_call.name_token] = { p_call.text + "(__0" + untyped + ")", { p_call.args[0]->range } };
				break;
			}
			default:
				if (p_call.type.is_result() && p_call.text.rfind("try_", 0) == 0) {
					refuse(p_call.name_token, std::string(LACKS) + "try_ version of an engine method.");
				}
				break;
		}
	}

	// A statement written after `p_stmt`, at its indent on a line of its own
	// (architecture §6.6).
	void append(const Stmt &p_stmt, const std::string &p_gdscript, const Rewrite &p_rewrite) {
		const uint32_t first = tokens[p_stmt.range.first].offset;
		uint32_t line = first;
		while (line > 0 && (text[line - 1] == ' ' || text[line - 1] == '\t')) {
			line--;
		}
		if (line > 0 && text[line - 1] != '\n') {
			refuse(p_stmt.range.first, std::string(NOT_YET) + "a statement that a statement written after it would follow on a line of its own, here after another on its line.");
			return;
		}
		uint32_t end = tokens[p_stmt.range.last].end();
		while (end < text.size() && text[end] != '\r' && text[end] != '\n') {
			end++;
		}
		const std::string ends = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
		erase(end, end, ends + text.substr(line, first - line) + p_gdscript);
		erased.after[p_stmt.range.first].push_back(p_rewrite);
	}

	// `var x := y.expect(message)` and `x = y.expect(message)` as the
	// statement without expect() and an assert after it; `var x := T.new(m =
	// v)` as the statement without the member and its assignment after it, as
	// the call assigns it (src/compiler/lower.cpp:1594-1600).
	void taken_apart(const Stmt &p_stmt) {
		const bool declares = p_stmt.kind == StmtKind::Var;
		const Expr *value = p_stmt.value.get();
		if (!value || value->kind != ExprKind::Call || !(declares || (p_stmt.kind == StmtKind::Assign && p_stmt.assign_op == TokenKind::Equal))) {
			return;
		}
		const std::string name = declares ? p_stmt.name : (plain(*p_stmt.target) ? p_stmt.target->text : std::string());
		const bool expects = value->target == Target::LanguageMethod && value->language->of == LanguageFunction::Of::Option && value->text == "expect" && value->args.size() == 1;
		if (expects && !name.empty()) {
			split.insert(value);
			const std::string message = source.text_of(value->args[0]->range);
			append(p_stmt, "assert(" + name + " != null, " + message + ")", { "assert(" + name + " != null, __0)", { value->args[0]->range } });
			return;
		}
		// GDScript types instantiate() Node, which lacks the members.
		const bool typed = p_stmt.declared_type || value->text != "instantiate";
		if (value->member_names.empty() || !declares || !typed) {
			return;
		}
		split.insert(value);
		const uint32_t from = value->args.empty() ? tokens[value->member_name_tokens[0]].offset : tokens[value->args.back()->range.last].end();
		erase(from, tokens[value->member_values.back()->range.last].end());
		erased.moved.insert(value->name_token);
		for (size_t i = 0; i < value->member_names.size(); i++) {
			const std::string assigned = name + "." + value->member_names[i] + " = ";
			append(p_stmt, assigned + source.text_of(value->member_values[i]->range), { assigned + "__0", { value->member_values[i]->range } });
		}
	}

	// `Some(p)` as `p`, which GDScript matches against null too unless it is
	// a value, an array or a dictionary, a branch of None comes before it, or
	// a guard tests it (spec §3.4, §5.6).
	void some(const Pattern &p_pattern) {
		const Pattern &inner = p_pattern.items[0];
		const bool takes_null = inner.kind == PatternKind::Bind || inner.kind == PatternKind::Wildcard || (inner.kind == PatternKind::Value && inner.value->kind == ExprKind::NoneLiteral);
		if (takes_null && !null_kept.count(p_pattern.range.first)) {
			refuse(p_pattern.range.first, std::string(NOT_YET) + "Some(_) or Some(None), which would take null too, before a branch of None.");
			return;
		}
		const uint32_t open = p_pattern.value->name_token + 1;
		erase(tokens[p_pattern.range.first].offset, tokens[open].end());
		erase(tokens[p_pattern.range.last].offset, tokens[p_pattern.range.last].end());
		erased.calls[p_pattern.value->name_token] = 0;
	}

	// `Some(var x)` that no branch of None comes before as `var x when x !=
	// null`, which takes no null: GDScript tries each branch in order, its
	// guard after its pattern, and goes on to the next when either fails
	// (modules/gdscript/gdscript_compiler.cpp:1938-1983; ed1daf0). A guard
	// written already follows the test.
	void guarded(const MatchBranch &p_branch) {
		const Pattern &pattern = p_branch.patterns[0];
		const std::string test = pattern.items[0].name + " != null";
		null_kept.insert(pattern.range.first);
		if (!p_branch.guard) {
			erase(tokens[pattern.range.last].end(), tokens[pattern.range.last].end(), " when " + test);
			erased.rewrites[p_branch.range.first] = { test, {} };
			return;
		}
		const Expr &guard = *p_branch.guard;
		const bool own = !atomic(tokens, guard);
		erase(tokens[guard.range.first].offset, tokens[guard.range.first].offset, test + " and " + (own ? "(" : ""));
		if (own) {
			erase(tokens[guard.range.last].end(), tokens[guard.range.last].end(), ")");
		}
		erased.rewrites[p_branch.range.first] = { test + " and (__0)", { guard.range } };
	}

	static bool is_some(const Pattern &p_pattern) {
		return p_pattern.kind == PatternKind::Constructor && p_pattern.value && p_pattern.value->kind == ExprKind::Name && p_pattern.value->text == "Some" && p_pattern.items.size() == 1 && p_pattern.items[0].field.empty() && !p_pattern.rest;
	}

	static bool is_none(const Pattern &p_pattern) {
		return (p_pattern.kind == PatternKind::Value || p_pattern.kind == PatternKind::Constructor) && p_pattern.value && p_pattern.value->kind == ExprKind::NoneLiteral;
	}

public:
	std::vector<Edit> edits;
	Erased erased;
	std::vector<Diagnostic> refused;

	Converter(const std::string &p_path, const SourceFile &p_source, const EngineApi &p_engine, const std::set<std::string> &p_globals) :
			path(p_path), source(p_source), tokens(p_source.tokens), text(p_source.text), engine(p_engine), globals(p_globals) {}

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
				inferred(p_node.name_token, p_node.type, p_node.initializer.get(), p_node.is_export);
			}
			if (p_node.initializer) {
				visit(*p_node.initializer);
			}
			return;
		} else if constexpr (std::is_same_v<T, ParamDecl>) {
			if (p_node.declared_type) {
				declared(*p_node.declared_type, p_node.type, tokens[p_node.name_token].end(), false);
			} else {
				inferred(p_node.name_token, p_node.type, p_node.default_value.get(), false);
			}
		} else if constexpr (std::is_same_v<T, ConstDecl>) {
			if (p_node.declared_type) {
				declared(*p_node.declared_type, p_node.type, tokens[p_node.name_token].end(), false);
			}
			constant = true;
			for_each_child(p_node, [&](const auto &p_child) { visit(p_child); });
			constant = false;
			return;
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
			statement(p_node);
			return;
		} else if constexpr (std::is_same_v<T, Pattern>) {
			if (is_some(p_node)) {
				some(p_node);
			} else if (p_node.kind == PatternKind::Constructor && !is_none(p_node)) {
				refuse(p_node.range.first, std::string(NOT_YET) + "a pattern of a struct, a variant or a constructor.");
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
			const ClassDecl *outer = klass;
			klass = &p_node;
			for_each_child(p_node, [&](const auto &p_child) { visit(p_child); });
			klass = outer;
			return;
		}
		for_each_child(p_node, [&](const auto &p_child) { visit(p_child); });
	}

	void statement(const Stmt &p_stmt) {
		const bool declares = p_stmt.kind == StmtKind::Var || p_stmt.kind == StmtKind::Const || p_stmt.kind == StmtKind::For;
		const LocalInfo *local = declares && function && p_stmt.local >= 0 && size_t(p_stmt.local) < function->locals.size() ? &function->locals[size_t(p_stmt.local)] : nullptr;
		if (local && p_stmt.declared_type) {
			declared(*p_stmt.declared_type, local->type, tokens[p_stmt.name_token].end(), false);
		} else if (local && p_stmt.kind == StmtKind::Var) {
			inferred(p_stmt.name_token, local->type, p_stmt.value.get(), false);
			// `var x = v`, which GDScript leaves untyped.
			if (tokens[p_stmt.name_token + 1].kind != TokenKind::Colon) {
				wide.insert({ function, p_stmt.local });
			}
		} else if (local && p_stmt.kind == StmtKind::For && p_stmt.value && (nested(p_stmt.value->type) || untyped(*p_stmt.value))) {
			wide.insert({ function, p_stmt.local });
		}
		taken_apart(p_stmt);
		const Expr *base = p_stmt.kind == StmtKind::Assign && p_stmt.target && (p_stmt.target->kind == ExprKind::Subscript || p_stmt.target->kind == ExprKind::Member) ? p_stmt.target->base.get() : nullptr;
		if (base && p_stmt.value && ((p_stmt.target->kind == ExprKind::Subscript && nested(base->type)) || untyped(*base))) {
			typed_literal(*p_stmt.value);
		}
		if (p_stmt.kind == StmtKind::Match) {
			// GDScript counts a match as returning only with a branch that takes
			// any value (modules/gdscript/gdscript_parser.cpp:2467-2475, 2543;
			// ed1daf0), this language also when it is exhaustive (spec §5.6).
			const auto any = [](const MatchBranch &p_branch) { return !p_branch.guard && std::any_of(p_branch.patterns.begin(), p_branch.patterns.end(), [](const Pattern &p_pattern) { return p_pattern.kind == PatternKind::Wildcard || p_pattern.kind == PatternKind::Bind; }); };
			const auto returns = [](const MatchBranch &p_branch) { return !p_branch.body.empty() && p_branch.body.back()->kind == StmtKind::Return; };
			const bool typed = function && function->return_type_ref && function->return_type.kind != TypeKind::Void && function->return_type.kind != TypeKind::Option;
			if (typed && std::none_of(p_stmt.branches.begin(), p_stmt.branches.end(), any) && std::all_of(p_stmt.branches.begin(), p_stmt.branches.end(), returns)) {
				refuse(p_stmt.range.first, std::string(NOT_YET) + "a match that returns on every path without _, which GDScript does not count as returning.");
			}
			bool none = false;
			for (const MatchBranch &branch : p_stmt.branches) {
				for (const Pattern &pattern : branch.patterns) {
					if (none && is_some(pattern)) {
						null_kept.insert(pattern.range.first);
					} else if (is_some(pattern) && pattern.items[0].kind == PatternKind::Bind) {
						guarded(branch);
					}
				}
				none = none || (!branch.guard && std::any_of(branch.patterns.begin(), branch.patterns.end(), is_none));
			}
		}
		constant = p_stmt.kind == StmtKind::Const;
		for_each_child(p_stmt, [&](const auto &p_child) { visit(p_child); });
		constant = false;
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

	// The text with the edits applied, what is written at a byte before what
	// replaces it; nullopt where two overlap.
	std::optional<std::string> applied() {
		std::stable_sort(edits.begin(), edits.end(), [](const Edit &p_a, const Edit &p_b) { return p_a.begin < p_b.begin || (p_a.begin == p_b.begin && p_a.end < p_b.end); });
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

	// A path written in the file, as `extends` or a string names one.
	std::string literal(const std::string &p_text) const {
		const auto found = renamed.find(resolved_path(p_text, path));
		if (found == renamed.end()) {
			return p_text;
		}
		return p_text.substr(0, p_text.rfind('.')) + found->second.substr(found->second.rfind('.'));
	}

	// While a rewrite's GDScript prints, what its `__0`... stand for, and that
	// its nodes are no .pet file's, which `erased` names by their tokens.
	std::vector<const Expr *> operands;
	bool raw = false;

	template <typename Node>
	static const Expr *find(const Node &p_node, const TokenRange &p_range) {
		const Expr *found = nullptr;
		for_each_child(p_node, [&](const auto &p_child) {
			if constexpr (std::is_same_v<std::decay_t<decltype(p_child)>, Expr>) {
				if (!found && p_child.range.first == p_range.first && p_child.range.last == p_range.last) {
					found = &p_child;
				}
			}
			if (!found) {
				found = find(p_child, p_range);
			}
		});
		return found;
	}

	// A rewrite as GDScript parses it, an expression or a statement, with the
	// .pet file's operands within `p_node` in it.
	template <typename Node>
	void rewritten(const Rewrite &p_rewrite, const Node &p_node, bool p_statement) {
		std::vector<Diagnostic> errors;
		const ClassDecl parsed = parse("func f():\n\t" + std::string(p_statement ? "" : "return ") + p_rewrite.gdscript + "\n", errors);
		const Stmt *stmt = errors.empty() && !parsed.functions.empty() && !parsed.functions[0].body.empty() ? parsed.functions[0].body[0].get() : nullptr;
		std::vector<const Expr *> found;
		for (const TokenRange &range : p_rewrite.operands) {
			found.push_back(find(p_node, range));
		}
		if (!stmt || (!p_statement && !stmt->value) || std::count(found.begin(), found.end(), nullptr)) {
			line("unparsed " + p_rewrite.gdscript, 0);
			return;
		}
		const std::vector<const Expr *> outer = operands;
		const bool was_raw = raw;
		operands = found;
		raw = true;
		if (p_statement) {
			visit(*stmt);
		} else {
			visit(*stmt->value);
		}
		operands = outer;
		raw = was_raw;
	}

public:
	std::string text;

	Printer(const SourceFile &p_source, const Erased &p_erased, const std::map<std::string, std::string> &p_renamed, const std::string &p_path) :
			source(p_source), erased(p_erased), renamed(p_renamed), path(p_path) {}

	template <typename Node>
	void visit(const Node &p_node) {
		using T = std::decay_t<Node>;
		if constexpr (std::is_same_v<T, TypeRef>) {
			if (const auto found = raw ? erased.types.end() : erased.types.find(p_node.name_token); found != erased.types.end()) {
				if (!found->second.empty()) {
					line("type " + found->second, p_node.name_token);
				}
				return;
			}
			line("type " + p_node.name + (p_node.is_function ? " func" : ""), p_node.name_token);
			if (p_node.name != "PackedScene") {
				children(p_node);
			}
		} else if constexpr (std::is_same_v<T, Expr>) {
			if (raw && p_node.kind == ExprKind::Name && p_node.text.rfind("__", 0) == 0) {
				const std::vector<const Expr *> outer = operands;
				operands.clear();
				raw = false;
				visit(*outer[std::stoul(p_node.text.substr(2))]);
				operands = outer;
				raw = true;
				return;
			}
			const bool named = !raw && (p_node.kind == ExprKind::Call || p_node.kind == ExprKind::Member);
			const bool listing = !raw && (p_node.kind == ExprKind::ArrayLiteral || p_node.kind == ExprKind::DictionaryLiteral);
			if (const auto found = named || listing ? erased.rewrites.find(named ? p_node.name_token : p_node.range.first) : erased.rewrites.end(); found != erased.rewrites.end()) {
				rewritten(found->second, p_node, false);
				return;
			}
			const auto collapsed = named && p_node.kind == ExprKind::Call ? erased.calls.find(p_node.name_token) : erased.calls.end();
			const Expr *inner = collapsed == erased.calls.end() ? nullptr : (collapsed->second < 0 ? p_node.base.get() : (p_node.args.empty() ? nullptr : p_node.args[0].get()));
			if (inner) {
				visit(*inner);
				return;
			}
			const bool moved = named && erased.moved.count(p_node.name_token);
			char number[64];
			std::snprintf(number, sizeof(number), "%lld %.17g", static_cast<long long>(p_node.int_value), p_node.float_value);
			std::string label = "expr " + std::to_string(int(p_node.kind)) + " " + (p_node.kind == ExprKind::NoneLiteral ? std::string() : std::to_string(int(p_node.op))) + " " + (p_node.kind == ExprKind::StringLiteral ? literal(p_node.text) : p_node.text) + " " + number + (p_node.bool_value ? " true" : "");
			// Unchecked, the members passed by name are arguments by name.
			for (const std::vector<std::string> *names : { &p_node.arg_names, &p_node.member_names }) {
				for (const std::string &name : moved ? std::vector<std::string>() : *names) {
					label += " " + name + "=";
				}
			}
			line(label, p_node.range.first);
			depth++;
			for_each_child(p_node, [&](const auto &p_child) {
				if constexpr (std::is_same_v<std::decay_t<decltype(p_child)>, Expr>) {
					for (size_t i = 0; moved && i < p_node.args.size() && i < p_node.arg_names.size(); i++) {
						if (p_node.args[i].get() == &p_child && !p_node.arg_names[i].empty()) {
							return;
						}
					}
				}
				visit(p_child);
			});
			depth--;
		} else if constexpr (std::is_same_v<T, Stmt>) {
			line("stmt " + std::to_string(int(p_node.kind)) + " " + p_node.name + " " + std::to_string(int(p_node.assign_op)), p_node.range.first);
			children(p_node);
			if (const auto found = raw ? erased.after.end() : erased.after.find(p_node.range.first); found != erased.after.end()) {
				for (const Rewrite &each : found->second) {
					rewritten(each, p_node, true);
				}
			}
		} else if constexpr (std::is_same_v<T, Pattern>) {
			if (!raw && p_node.kind == PatternKind::Constructor && p_node.value && p_node.items.size() == 1 && erased.calls.count(p_node.value->name_token)) {
				visit(p_node.items[0]);
				return;
			}
			line("pattern " + std::to_string(int(p_node.kind)) + " " + p_node.name + " " + p_node.field + (p_node.rest ? " .." : ""), p_node.range.first);
			children(p_node);
		} else if constexpr (std::is_same_v<T, MatchBranch>) {
			line("branch", p_node.range.first);
			// The guard written for Some(var x), the one written after it.
			const auto guard = raw ? erased.rewrites.end() : erased.rewrites.find(p_node.range.first);
			if (guard == erased.rewrites.end()) {
				children(p_node);
				return;
			}
			depth++;
			for (const Pattern &pattern : p_node.patterns) {
				visit(pattern);
			}
			rewritten(guard->second, p_node, false);
			for (const std::unique_ptr<Stmt> &stmt : p_node.body) {
				visit(*stmt);
			}
			depth--;
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
			line("class " + p_node.name + " " + p_node.class_name + " extends " + literal(p_node.extends) + (p_node.is_abstract ? " abstract" : "") + (p_node.static_unload ? " static_unload" : "") + " " + p_node.icon, p_node.name_token);
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
// a constant, as it is written.
struct Access {
	std::string target;
	std::string written;
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
			found.push_back({ target, tokens[i].text + "." + member + (is_static ? "()" : "") });
		}
	}
	return found;
}

// A type as reflection shows GDScript's, which this language's view of a
// GDScript class reads (src/adapter/editor/class_db_api.cpp:23-62, spec
// §11.1): an object as T?, of its global class or an engine class, an enum of
// a script as int, and the rest as the GDScript written declares them.
Type reflected(const Type &p_type, const EngineApi &p_engine, const std::set<std::string> &p_globals) {
	switch (p_type.kind) {
		case TypeKind::Object:
			return Type::object(p_engine.class_exists(p_type.name) || p_globals.count(p_type.name) ? p_type.name : "Object", true);
		case TypeKind::Option:
			return Type::of(TypeKind::Variant);
		case TypeKind::Enum:
			return p_type.script.empty() ? p_type : Type::of(TypeKind::Int);
		case TypeKind::Function:
			return Type::builtin("Callable");
		case TypeKind::Builtin: {
			if (!is_typed_container(p_type)) {
				return p_type;
			}
			std::string hint;
			for (size_t i = 0; i < p_type.args.size(); i++) {
				if (p_type.args[i].kind == TypeKind::Option) {
					return Type::builtin(p_type.name);
				}
				const Type each = is_typed_container(p_type.args[i]) ? Type::builtin(p_type.args[i].name) : reflected(p_type.args[i], p_engine, p_globals);
				hint += (i > 0 ? ";" : "") + (each.kind == TypeKind::Object ? each.name : each.to_string());
			}
			return api_type((p_type.name == "Array" ? "typedarray::" : "typeddictionary::") + hint, true);
		}
		default:
			return p_type;
	}
}

// What reflection would show of a class converted back, at its new path
// (src/adapter/editor/scene_index.cpp:879-939): its members and those it
// inherits, untyped ones as Variant, its enums as Dictionary constants.
ForeignClass gdscript_view(const ClassDecl &p_class, const std::string &p_path, const EngineApi &p_engine, const std::set<std::string> &p_globals) {
	ForeignClass view;
	view.name = p_class.class_name;
	view.native = p_class.native_base;
	view.base = p_class.base_script;
	view.members_known = true;
	const auto typed = [&](bool p_written, const Type &p_type) { return p_written ? reflected(p_type, p_engine, p_globals) : Type::of(TypeKind::Variant); };
	for (const VarDecl *member : p_class.all_members) {
		view.properties.push_back({ member->name, typed(member->declared_type || member->infers_type, member->type), true, p_path });
	}
	for (const FuncDecl *function : p_class.all_functions) {
		if (function->is_lambda || !function->accessor_of.empty()) {
			continue;
		}
		EngineMethod method;
		method.name = function->name;
		method.owner = p_path;
		method.is_static = function->is_static;
		for (const ParamDecl &param : function->params) {
			method.params.push_back({ param.name, typed(param.declared_type || param.infers_type, param.type), param.default_value ? param.default_value->text : std::string() });
			method.default_count += param.default_value != nullptr;
		}
		method.return_type = function->return_type_ref ? reflected(function->return_type, p_engine, p_globals) : Type::of(TypeKind::Variant);
		view.methods.push_back(std::move(method));
	}
	for (const SignalDecl *signal : p_class.all_signals) {
		EngineMethod method;
		method.name = signal->name;
		method.owner = p_path;
		for (const ParamDecl &param : signal->params) {
			method.params.push_back({ param.name, typed(param.declared_type.has_value(), param.type), std::string() });
		}
		view.signals.push_back(std::move(method));
	}
	for (const ConstDecl *constant : p_class.all_constants) {
		view.constants.push_back({ constant->name, reflected(constant->type, p_engine, p_globals), false, p_path });
	}
	for (const EnumDecl *each : p_class.all_enums) {
		if (!each->name.empty()) {
			view.constants.push_back({ each->name, Type::builtin("Dictionary"), false, p_path });
			continue;
		}
		for (const EnumValueDecl &value : each->values) {
			view.constants.push_back({ value.name, Type::of(TypeKind::Int), false, p_path });
		}
	}
	return view;
}

// The errors a check of the .pet file at `p_file` finds in `p_project`, with
// their lines and the file.
std::vector<Diagnostic> errors_of(const EngineApi &p_engine, const ProjectApi &p_project, ScriptClasses &r_classes, const std::string &p_file) {
	const std::string source = p_project.script_source(p_file).value_or(std::string());
	std::vector<Diagnostic> found;
	ClassDecl decl = parse(source, found);
	if (found.empty()) {
		CompileOptions options;
		options.path = p_file;
		options.project = &p_project;
		r_classes.begin_compile(p_file, source);
		check(decl, p_engine, options, found, r_classes);
		r_classes.end_compile();
	}
	std::vector<Diagnostic> errors;
	for (Diagnostic &each : found) {
		if (each.severity != Severity::Error) {
			continue;
		}
		const SourcePosition position = decl.source->position(each.begin);
		each.line = position.line;
		each.column = position.column;
		each.file = p_file;
		errors.push_back(std::move(each));
	}
	return errors;
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
		ForeignClass view; // As reflection would show its GDScript, its base by its old path.
	};
	std::map<std::string, Converted> converted;
	std::set<std::string> globals;
	for (const auto &[name, path] : p_project.global_class_paths()) {
		globals.insert(name);
	}
	const auto convert = [&](const std::string &p_file) -> const Converted & {
		const auto found = converted.find(p_file);
		if (found != converted.end()) {
			return found->second;
		}
		const std::string source = p_project.script_source(p_file).value_or(std::string());
		std::vector<Diagnostic> errors;
		ClassDecl decl = parse(source, errors);
		Converter converter(p_file, *decl.source, p_engine, globals);
		ForeignClass view;
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
				view = gdscript_view(decl, reversed_path(p_file), p_engine, globals);
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
		return converted[p_file] = { text.value_or(std::string()), converter.erased, converter.refused, view };
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

	// The errors that converting causes in the .pet files that stay, which
	// see the converted classes as GDScript (spec §11.1): those a check of
	// them in memory finds that it did not before.
	std::vector<std::string> staying;
	std::copy_if(files.begin(), files.end(), std::back_inserter(staying), [&](const std::string &p_file) { return !renamed.count(p_file); });
	if (renamed.empty() || staying.empty()) {
		return result;
	}
	std::map<std::string, std::string> texts;
	std::map<std::string, ForeignClass> views;
	for (const auto &[file, now] : renamed) {
		texts[now] = result.texts[file];
		ForeignClass view = converted.at(file).view;
		view.base = renamed.count(view.base) ? renamed.at(view.base) : view.base;
		views[now] = std::move(view);
	}
	const Migrated after(p_project, renamed, texts, views);
	ScriptClasses after_classes(p_engine, &after);
	for (const std::string &file : staying) {
		std::set<std::pair<int, std::string>> had;
		for (const Diagnostic &error : errors_of(p_engine, p_project, classes, file)) {
			std::string message = error.message;
			for (const auto &[old, now] : renamed) {
				for (size_t at = message.find(old); at != std::string::npos; at = message.find(old, at + now.size())) {
					message.replace(at, old.size(), now);
				}
			}
			had.emplace(error.line, message);
		}
		for (Diagnostic &error : errors_of(p_engine, after, after_classes, file)) {
			if (!had.count({ error.line, error.message })) {
				result.caused.push_back(std::move(error));
			}
		}
	}
	return result;
}

} // namespace petrichor::compiler
