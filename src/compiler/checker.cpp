#include "compiler/checker.h"

#include "compiler/builtin_api.h"
#include "compiler/parser.h"
#include "compiler/script_classes.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <locale>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace petrichor::compiler {

namespace {

const char *operator_name(TokenKind p_op) {
	switch (p_op) {
		case TokenKind::Plus:
			return "+";
		case TokenKind::Minus:
			return "-";
		case TokenKind::Star:
			return "*";
		case TokenKind::StarStar:
			return "**";
		case TokenKind::Slash:
			return "/";
		case TokenKind::Percent:
			return "%";
		case TokenKind::EqualEqual:
			return "==";
		case TokenKind::BangEqual:
			return "!=";
		case TokenKind::Less:
			return "<";
		case TokenKind::LessEqual:
			return "<=";
		case TokenKind::Greater:
			return ">";
		case TokenKind::GreaterEqual:
			return ">=";
		case TokenKind::ShiftLeft:
			return "<<";
		case TokenKind::ShiftRight:
			return ">>";
		case TokenKind::Ampersand:
			return "&";
		case TokenKind::Pipe:
			return "|";
		case TokenKind::Caret:
			return "^";
		default:
			return "";
	}
}

// Utility functions that the engine binds with Variant and that are typed by
// their arguments instead (spec §10.3).
enum class ArgumentTyped {
	None,
	Same, // Every argument has the result's type, such as clamp and min.
	Lerp, // from and to have the result's type; the weight is a float.
};

ArgumentTyped argument_typed(const std::string &p_name) {
	static const std::unordered_set<std::string> same = { "floor", "ceil", "round", "abs", "sign", "snapped", "wrap", "max", "min", "clamp" };
	if (same.count(p_name)) {
		return ArgumentTyped::Same;
	}
	return p_name == "lerp" ? ArgumentTyped::Lerp : ArgumentTyped::None;
}

// The types these utilities accept in the engine (core/variant/variant_utility.cpp).
bool is_numeric_or_vector(const Type &p_type) {
	if (p_type.is_numeric()) {
		return true;
	}
	static const std::unordered_set<std::string> vectors = { "Vector2", "Vector2i", "Vector3", "Vector3i", "Vector4", "Vector4i" };
	return p_type.kind == TypeKind::Builtin && vectors.count(p_type.name);
}

bool is_interpolable(const Type &p_type) {
	static const std::unordered_set<std::string> types = { "Vector2", "Vector3", "Vector4", "Color", "Quaternion", "Basis" };
	return p_type.kind == TypeKind::Float || (p_type.kind == TypeKind::Builtin && types.count(p_type.name));
}

// Gives an expression made by the compiler the position of the code it belongs to.
void relocate(Expr &r_expr, const TokenRange &p_range) {
	r_expr.range = p_range;
	r_expr.name_token = r_expr.op_token = NO_TOKEN;
	for (Expr *child : { r_expr.left.get(), r_expr.right.get(), r_expr.base.get() }) {
		if (child) {
			relocate(*child, p_range);
		}
	}
	for (auto &arg : r_expr.args) {
		relocate(*arg, p_range);
	}
}

std::vector<Type> param_types(const std::vector<ParamDecl> &p_params) {
	std::vector<Type> types;
	for (const ParamDecl &param : p_params) {
		types.push_back(param.type);
	}
	return types;
}

// "res://a/./b/../c" as "res://a/c", as the engine's String::simplify_path()
// leaves it, so that a path matches the one the project knows the file by.
std::string simplify_path(const std::string &p_path) {
	const std::string scheme = "res://";
	const bool rooted = p_path.rfind(scheme, 0) == 0;
	const std::string rest = rooted ? p_path.substr(scheme.size()) : p_path;
	std::vector<std::string> parts;
	for (size_t start = 0; start <= rest.size();) {
		size_t end = rest.find('/', start);
		if (end == std::string::npos) {
			end = rest.size();
		}
		const std::string part = rest.substr(start, end - start);
		if (part == "..") {
			if (!parts.empty()) {
				parts.pop_back();
			}
		} else if (!part.empty() && part != ".") {
			parts.push_back(part);
		}
		start = end + 1;
	}
	std::string result = rooted ? scheme : std::string();
	for (size_t i = 0; i < parts.size(); i++) {
		result += (i > 0 ? "/" : "") + parts[i];
	}
	return result;
}

// A script of this language, by its path.
bool is_script_path(const std::string &p_path) {
	return p_path.size() > 4 && p_path.compare(p_path.size() - 4, 4, ".pet") == 0;
}

class Checker : public ClassRelation {
	ClassDecl &cls;
	const EngineApi &engine;
	const CompileOptions &options;
	std::vector<Diagnostic> &errors;
	bool check_bodies;
	ScriptClasses &classes; // The project's other scripts of this language.
	std::vector<Scene> scenes; // The scenes that use this script.
	std::vector<Reference> *references; // Recorded for this file only.

	FuncDecl *function = nullptr;
	std::vector<std::unordered_map<std::string, int>> scopes;
	// Types of locals narrowed by conditions (spec §5.4), innermost last.
	std::vector<std::unordered_map<int, Type>> narrowed;
	int loop_depth = 0;
	bool in_predelete = false;
	// The interfaces this class's own `implements` names, with where.
	std::vector<std::pair<Type, const TypeRef *>> own_interfaces;
	// The member whose initializer is being checked: it and the members after
	// it are not initialized yet (spec §6.1), nor are @onready members unless
	// it is one too (§6.3).
	int initializing = -1;
	bool initializing_onready = false;
	// Checking a constant's initializer, which may come before the members' types are known.
	bool in_constant = false;
	// The enum whose values are being computed, whose values before
	// `current_value` its own may name (spec §3.7).
	const EnumDecl *current_enum = nullptr;
	size_t current_value = 0;
	// The functions around the lambda being checked (spec §3.11), outermost
	// first, each as it was where its lambda is.
	struct Enclosing {
		FuncDecl *function;
		std::vector<std::unordered_map<std::string, int>> scopes;
		std::vector<std::unordered_map<int, Type>> narrowed;
		Expr *lambda;
		int loop_depth;
		bool in_predelete;
		int initializing;
		bool initializing_onready;
	};
	std::vector<Enclosing> enclosing;
	// The return type of a lambda without one, as its return statements give it.
	struct Returns {
		std::optional<Type> type;
		bool bare = false; // A return without a value.
	};
	Returns *returns = nullptr;

	std::unordered_map<std::string, int> member_index;
	std::unordered_map<std::string, int> constant_index; // In all_constants.
	std::unordered_map<std::string, int> function_index;
	std::unordered_map<std::string, int> signal_index;
	mutable std::unordered_map<std::string, std::optional<ForeignClass>> foreign_classes; // By path.

	// At the start of the file for what was not written, such as the base
	// class of a class without extends.
	void error(const TokenRange &p_range, const std::string &p_message) {
		const TokenRange range = p_range.first < cls.source->tokens.size() ? p_range : TokenRange{ 0, 0 };
		errors.push_back({ cls.source->begin(range), cls.source->end(range), p_message });
	}
	void error(uint32_t p_token, const std::string &p_message) {
		error(TokenRange{ p_token, p_token }, p_message);
	}

	/* References (architecture §2.1) */

	static DeclId script_id(const std::string &p_path, const std::string &p_name = std::string()) {
		return { DeclId::Kind::Script, p_path, p_name };
	}
	static DeclId foreign_id(const std::string &p_path, const std::string &p_name) {
		return { DeclId::Kind::Foreign, p_path, p_name };
	}
	static DeclId engine_id(const std::string &p_class, const std::string &p_name = std::string()) {
		return { DeclId::Kind::Engine, p_class, p_name };
	}
	static DeclId language_id(const std::string &p_type, const std::string &p_name) {
		return { DeclId::Kind::Language, p_type, p_name };
	}
	// By the name token of its declaration.
	DeclId local_id(uint32_t p_token) const {
		return { DeclId::Kind::Local, options.path, p_token < cls.source->tokens.size() ? std::to_string(cls.source->tokens[p_token].offset) : std::string() };
	}

	// A name of this file, unless the compiler made it or a syntax error left
	// it empty.
	void refer(uint32_t p_token, const DeclId &p_target, bool p_declaration = false, const std::optional<DeclId> &p_overrides = std::nullopt) {
		if (p_token < cls.source->tokens.size() && cls.source->tokens[p_token].length > 0) {
			refer_bytes(cls.source->tokens[p_token].offset, cls.source->tokens[p_token].end(), p_target, p_declaration, p_overrides);
		}
	}

	void refer_bytes(uint32_t p_begin, uint32_t p_end, const DeclId &p_target, bool p_declaration = false, const std::optional<DeclId> &p_overrides = std::nullopt) {
		if (references) {
			Reference reference;
			reference.target = p_target;
			reference.is_declaration = p_declaration;
			reference.overrides = p_overrides;
			reference.begin = p_begin;
			reference.end = p_end;
			references->push_back(std::move(reference));
		}
	}

	// What a string literal holds, when it is written without escapes.
	void refer_string(const Expr &p_literal, const DeclId &p_target) {
		if (p_literal.kind != ExprKind::StringLiteral || p_literal.range.first >= cls.source->tokens.size()) {
			return;
		}
		const Token &token = cls.source->tokens[p_literal.range.first];
		if (token.kind != TokenKind::String && token.kind != TokenKind::StringName && token.kind != TokenKind::NodePath) {
			return;
		}
		// After a prefix: &, ^ or r.
		const char first = cls.source->text[token.offset];
		const uint32_t begin = token.offset + (first == '"' || first == '\'' ? 0 : 1);
		const uint32_t length = token.end() - begin;
		const uint32_t quotes = length >= 6 && length - 6 == p_literal.text.size() ? 3 : 1;
		if (length == p_literal.text.size() + 2 * quotes && cls.source->text.compare(begin + quotes, p_literal.text.size(), p_literal.text) == 0) {
			refer_bytes(begin + quotes, token.end() - quotes, p_target);
		}
	}

	// The class of the chain from `p_class` that declares a member.
	template <typename T>
	static const ClassDecl &declaring(const ClassDecl &p_class, const T &p_decl, std::vector<T> ClassDecl::*p_own) {
		for (const ClassDecl *owner = &p_class; owner; owner = owner->base) {
			const std::vector<T> &own = owner->*p_own;
			if (std::any_of(own.begin(), own.end(), [&](const T &p_item) { return &p_item == &p_decl; })) {
				return *owner;
			}
		}
		return p_class;
	}

	// Where to report a declaration this class has or inherits: its name, or
	// `p_otherwise` for one of a base class, which is in another file.
	template <typename T>
	static TokenRange own_name(const T &p_decl, const std::vector<T> &p_own, const TokenRange &p_otherwise) {
		return is_own(p_decl, p_own) ? TokenRange{ p_decl.name_token, p_decl.name_token } : p_otherwise;
	}

	template <typename T>
	static bool is_own(const T &p_decl, const std::vector<T> &p_own) {
		return std::any_of(p_own.begin(), p_own.end(), [&](const T &p_item) { return &p_item == &p_decl; });
	}

	/* Classes of this language (spec §11.1) */

	// The declarations of a class of this language: this class itself, or
	// another script's, checked without bodies.
	const ClassDecl *script_decl(const std::string &p_path) const {
		if (p_path == options.path) {
			return &cls;
		}
		const ScriptClass *script = classes.declarations(p_path);
		return script ? &script->decl : nullptr;
	}

	// The header of a script of this language, this class's or another's as
	// read; null when it cannot be read.
	const ClassDecl *script_header(const std::string &p_path) const {
		if (p_path == options.path) {
			return &cls;
		}
		const ScriptClass *script = classes.header(p_path);
		return script ? &script->decl : nullptr;
	}

	// The type of the objects of a script of this language.
	Type script_class_type(const ClassDecl &p_header, const std::string &p_path) const {
		return Type::script_class(display_name(p_header, p_path), p_path, classes.native_base(p_header, p_path));
	}

	// The class name used in messages and types: class_name, or the file's name.
	static std::string display_name(const ClassDecl &p_decl, const std::string &p_path) {
		return p_decl.class_name.empty() ? file_name(p_path) : p_decl.class_name;
	}

	static std::string file_name(const std::string &p_path) {
		const size_t slash = p_path.find_last_of('/');
		return p_path.substr(slash == std::string::npos ? 0 : slash + 1);
	}

	/* Classes of other languages (spec §11.1) */

	// Named as one of this language is.
	static Type foreign_class_type(const std::string &p_path, const ForeignClass &p_foreign) {
		return Type::script_class(p_foreign.name.empty() ? file_name(p_path) : p_foreign.name, p_path, p_foreign.native);
	}

	// Without its members while only the declarations are checked, when the
	// script being loaded may load no other.
	const ForeignClass *foreign_class(const std::string &p_path) const {
		auto found = foreign_classes.find(p_path);
		if (found == foreign_classes.end()) {
			found = foreign_classes.emplace(p_path, options.project ? options.project->foreign_class(p_path, !options.declarations_only) : std::nullopt).first;
		}
		return found->second ? &*found->second : nullptr;
	}

	// A class reflection names: an engine class, or a script's class by its
	// global name, as GDScript names it (modules/gdscript/gdscript_parser.cpp:5457-5465).
	Type localized(Type p_type) {
		for (Type &arg : p_type.args) {
			arg = localized(arg);
		}
		if (p_type.kind != TypeKind::Object || !p_type.script.empty() || engine.class_exists(p_type.name)) {
			return p_type;
		}
		const std::string path = options.project ? options.project->global_class_path(p_type.name) : std::string();
		Type local = Type::object("Object");
		if (is_script_path(path)) {
			if (const ClassDecl *header = script_header(path)) {
				local = script_class_type(*header, path);
			}
		} else if (const ForeignClass *foreign = path.empty() ? nullptr : foreign_class(path)) {
			if (!foreign->native.empty()) {
				local = foreign_class_type(path, *foreign);
			}
		}
		local.nullable = p_type.nullable;
		return local;
	}

	// While only the declarations are checked, the members of a class of
	// another language are not known, as the scenes are not.
	Type unknown_foreign(Expr &r_expr, const Type &p_class) {
		if (options.declarations_only) {
			return set(r_expr, Type::of(TypeKind::Error));
		}
		return fail(r_expr, p_class.name + " (" + p_class.script + ") cannot be read.");
	}

	// GDScript has no pub: its members are all reached by name.
	std::optional<Type> foreign_member(Expr &r_expr, const Type &p_class) {
		const ForeignClass *foreign = foreign_class(p_class.script);
		if (!foreign || !foreign->members_known) {
			return unknown_foreign(r_expr, p_class);
		}
		for (const EngineProperty &property : foreign->properties) {
			if (property.name == r_expr.text) {
				if (!property.type.is_storable()) {
					return fail(r_expr, "The property \"" + r_expr.text + "\" has type " + property.type.to_string() + ", which is not supported yet.");
				}
				r_expr.target = Target::ObjectProperty;
				r_expr.owner = p_class.engine_class();
				refer(r_expr.name_token, foreign_id(p_class.script, r_expr.text));
				return localized(property.type);
			}
		}
		for (const EngineMethod &signal : foreign->signals) {
			if (signal.name == r_expr.text) {
				r_expr.target = Target::ObjectSignal;
				refer(r_expr.name_token, foreign_id(p_class.script, r_expr.text));
				return localized(signal_type(signal));
			}
		}
		return std::nullopt;
	}

	std::optional<Type> foreign_method(Expr &r_expr, const Type &p_class) {
		const ForeignClass *foreign = foreign_class(p_class.script);
		if (!foreign || !foreign->members_known) {
			return unknown_foreign(r_expr, p_class);
		}
		for (const EngineMethod &method : foreign->methods) {
			if (method.name == r_expr.text) {
				EngineMethod local = method;
				for (EngineParam &param : local.params) {
					param.type = localized(param.type);
				}
				local.return_type = localized(local.return_type);
				r_expr.is_foreign = true;
				refer(r_expr.name_token, foreign_id(p_class.script, r_expr.text));
				return check_engine_call(r_expr, local);
			}
		}
		return std::nullopt;
	}

	// The script itself, or one it extends.
	bool extends_script(const std::string &p_script, const std::string &p_base) const {
		std::unordered_set<std::string> seen;
		for (std::string script = p_script; !script.empty() && seen.insert(script).second;) {
			if (script == p_base) {
				return true;
			}
			if (is_script_path(script)) {
				const ClassDecl *decl = script_header(script);
				script = decl ? classes.base_path(*decl, script) : std::string();
			} else {
				const ForeignClass *foreign = foreign_class(script);
				script = foreign ? foreign->base : std::string();
			}
		}
		return false;
	}

	bool implements(const Type &p_class, const Type &p_interface) const {
		if (!is_script_path(p_class.script) || p_class.is_interface) {
			return false; // Only this language's classes implement interfaces (spec §3.9).
		}
		const ClassDecl *decl = script_decl(p_class.script);
		if (!decl) {
			return false;
		}
		for (const Type &implemented : decl->implemented) {
			if (implemented.script == p_interface.script && implemented.name == p_interface.name) {
				return true;
			}
		}
		return false;
	}

	bool is_a(const Type &p_from, const Type &p_to) const override {
		if (p_to.is_interface) {
			return p_from.is_interface ? p_from.script == p_to.script && p_from.name == p_to.name : implements(p_from, p_to);
		}
		if (p_from.is_interface) {
			return p_to.script.empty() && p_to.name == "Object";
		}
		if (!p_to.script.empty()) {
			return extends_script(p_from.script, p_to.script);
		}
		return engine.inherits(p_from.engine_class(), p_to.name);
	}

	// Whether an object can be both: `is` and `as` between them are not always false.
	bool related(const Type &p_a, const Type &p_b) const {
		return is_a(p_a, p_b) || is_a(p_b, p_a) || p_a.is_interface || p_b.is_interface;
	}

	// A class of this language named by its class_name, or an interface or a
	// struct in one, such as Combat.Damageable; nullopt if the name is not one.
	std::optional<Type> script_type(const TypeRef &p_ref) {
		const size_t dot = p_ref.name.find('.');
		const std::string head = p_ref.name.substr(0, dot);
		std::string path;
		const ConstPtr alias = class_alias(head);
		if (alias) {
			refer(p_ref.name_token, alias_id(head));
			if (alias->script_class.is_error()) {
				return alias->script_class; // Not known while only the declarations are checked.
			}
			path = alias->script;
		} else if (!cls.class_name.empty() && head == cls.class_name) {
			path = options.path;
		} else if (options.project) {
			path = options.project->global_class_path(head);
		}
		if (path.empty()) {
			// An interface or a struct of this class, by its own name.
			if (dot == std::string::npos) {
				for (const InterfaceDecl &interface : cls.interfaces) {
					if (interface.name == p_ref.name) {
						refer(p_ref.name_token, script_id(options.path, interface.name));
						return Type::interface(display_name(cls, options.path) + "." + interface.name, options.path);
					}
				}
				if (cls.find_struct(p_ref.name)) {
					refer(p_ref.name_token, script_id(options.path, p_ref.name));
					return Type::structure(display_name(cls, options.path) + "." + p_ref.name, options.path);
				}
				if (const EnumDecl *decl = p_ref.name.empty() ? nullptr : cls.find_enum(p_ref.name)) {
					refer(p_ref.name_token, script_id(decl->script, decl->name));
					return decl->type;
				}
			}
			return std::nullopt;
		}
		if (!alias) {
			refer(p_ref.name_token, is_script_path(path) ? script_id(path) : foreign_id(path, std::string()));
		}
		if (!is_script_path(path)) {
			if (dot != std::string::npos) {
				error(p_ref.range, head + " is a class of another language, which has no interfaces.");
				return Type::of(TypeKind::Error);
			}
			if (alias) {
				return alias->script_class;
			}
			const ForeignClass *foreign = foreign_class(path);
			if (!foreign || foreign->native.empty()) {
				error(p_ref.range, head + " (" + path + ") cannot be read.");
				return Type::of(TypeKind::Error);
			}
			return foreign_class_type(path, *foreign);
		}
		const ClassDecl *header = script_header(path);
		if (!header) {
			error(p_ref.range, head + " (" + path + ") cannot be read.");
			return Type::of(TypeKind::Error);
		}
		if (dot == std::string::npos) {
			return script_class_type(*header, path);
		}
		const std::string inner = p_ref.name.substr(dot + 1);
		for (const InterfaceDecl &interface : header->interfaces) {
			if (interface.name == inner) {
				if (!interface.is_pub && path != options.path) {
					error(p_ref.range, p_ref.name + " is not pub (spec §4.2).");
					return Type::of(TypeKind::Error);
				}
				refer(p_ref.name_token == NO_TOKEN ? NO_TOKEN : p_ref.name_token + 2, script_id(path, inner));
				return Type::interface(display_name(*header, path) + "." + inner, path);
			}
		}
		if (const StructDecl *found = header->find_struct(inner)) {
			if (!found->is_pub && path != options.path) {
				error(p_ref.range, p_ref.name + " is not pub (spec §4.2).");
				return Type::of(TypeKind::Error);
			}
			refer(p_ref.name_token == NO_TOKEN ? NO_TOKEN : p_ref.name_token + 2, script_id(path, inner));
			return Type::structure(display_name(*header, path) + "." + inner, path);
		}
		if (std::any_of(header->enums.begin(), header->enums.end(), [&](const EnumDecl &p_enum) { return p_enum.name == inner; })) {
			const ClassDecl *checked = path == options.path ? &cls : script_decl(path);
			const EnumDecl *found = checked ? checked->find_enum(inner) : nullptr;
			if (!found) {
				error(p_ref.range, head + " (" + path + ") has errors.");
				return Type::of(TypeKind::Error);
			}
			if (!found->is_pub && path != options.path && !extends_script(options.path, path)) {
				error(p_ref.range, p_ref.name + " is not pub (spec §4.2).");
				return Type::of(TypeKind::Error);
			}
			refer(p_ref.name_token == NO_TOKEN ? NO_TOKEN : p_ref.name_token + 2, script_id(found->script, inner));
			return found->type;
		}
		error(p_ref.range, head + " declares no interface, struct or enum \"" + inner + "\".");
		return Type::of(TypeKind::Error);
	}

	// The declaration of a struct, with its fields checked; null when its
	// script has errors.
	const StructDecl *struct_decl(const Type &p_struct) const {
		const ClassDecl *decl = script_decl(p_struct.script);
		return decl ? decl->find_struct(p_struct.interface_name()) : nullptr;
	}

	// Whether a struct can start at its default value (spec §4.3); true when
	// that is not known, which is reported elsewhere.
	bool has_default(const Type &p_struct) const {
		const StructDecl *decl = struct_decl(p_struct);
		return !decl || decl->has_default();
	}

	bool assignable(const Type &p_from, const Type &p_to) const {
		return is_assignable(p_from, p_to, *this);
	}

	bool is_type_name(const std::string &p_name) const {
		return p_name == "int" || p_name == "float" || p_name == "bool" || builtin::find_class(p_name) || engine.class_exists(p_name);
	}

	Type resolve(const TypeRef &p_ref, bool p_allow_void) {
		Type type = resolve_name(p_ref, p_allow_void);
		if (p_ref.nullable && !type.is_error()) {
			if (type.kind == TypeKind::Void) {
				error(p_ref.range, "void cannot be optional.");
				return Type::of(TypeKind::Error);
			}
			// The entry checks would not see its elements' type.
			if (type.kind == TypeKind::Builtin && !type.args.empty()) {
				error(p_ref.range, type.to_string() + "? is not supported yet; an empty array can stand for none.");
				return Type::of(TypeKind::Error);
			}
			type = optional_of(type);
		}
		return type;
	}

	Type resolve_name(const TypeRef &p_ref, bool p_allow_void) {
		if (p_ref.is_function) {
			// `func(int, Node) -> bool` (spec §2.4, §3.11).
			std::vector<Type> params;
			bool valid = true;
			for (const TypeRef &param : p_ref.args) {
				params.push_back(resolve(param, false));
				valid = valid && !params.back().is_error();
			}
			const Type result = p_ref.result.empty() ? Type::of(TypeKind::Void) : resolve(p_ref.result[0], true);
			if (!valid || result.is_error()) {
				return Type::of(TypeKind::Error);
			}
			Type type = Type::function(result, params);
			type.is_async = p_ref.is_async;
			return type;
		}
		if (!p_ref.args.empty()) {
			return resolve_generic(p_ref);
		}
		if (p_ref.name == "void") {
			if (p_allow_void) {
				return Type::of(TypeKind::Void);
			}
			error(p_ref.range, "void is only valid as a return type.");
			return Type::of(TypeKind::Error);
		}
		if (p_ref.name == "Variant" || p_ref.name == "Nil") {
			error(p_ref.range, "The type " + p_ref.name + " is not supported yet.");
			return Type::of(TypeKind::Error);
		}
		if (builtin::find_class(p_ref.name)) {
			refer(p_ref.name_token, engine_id(p_ref.name));
			return api_type(p_ref.name, false);
		}
		if (engine.class_exists(p_ref.name)) {
			refer(p_ref.name_token, engine_id(p_ref.name));
			return Type::object(p_ref.name);
		}
		if (const std::optional<Type> script = script_type(p_ref)) {
			return *script;
		}
		error(p_ref.range, "Unknown type \"" + p_ref.name + "\".");
		return Type::of(TypeKind::Error);
	}

	// `Array[T]`, the engine's typed array (spec §3.8).
	Type resolve_generic(const TypeRef &p_ref) {
		refer(p_ref.name_token, p_ref.name == "Task" ? language_id("Task", std::string()) : engine_id(p_ref.name));
		if (p_ref.name == "Task" && p_ref.args.size() == 1) {
			const Type result = resolve(p_ref.args[0], true);
			return result.is_error() ? result : Type::task(result);
		}
		if (p_ref.name == "Dictionary") {
			if (p_ref.args.size() != 2) {
				error(p_ref.range, "Dictionary takes a key type and a value type.");
				return Type::of(TypeKind::Error);
			}
			const Type key = container_element(p_ref.args[0], "Dictionaries with keys of ");
			const Type value = container_element(p_ref.args[1], "Dictionaries of ");
			return key.is_error() || value.is_error() ? Type::of(TypeKind::Error) : Type::dictionary(key, value);
		}
		if (p_ref.name == "PackedScene" && p_ref.args.size() == 1) {
			// A scene whose root has type T (spec §7.4).
			const Type root = resolve(p_ref.args[0], false);
			if (root.is_error()) {
				return root;
			}
			if (root.kind != TypeKind::Object || root.nullable || !is_a(root, Type::object("Node"))) {
				error(p_ref.args[0].range, "The root of a scene is a node, not " + root.to_string() + ".");
				return Type::of(TypeKind::Error);
			}
			return Type::scene(root);
		}
		if (p_ref.name != "Array") {
			error(p_ref.range, p_ref.name + " takes no type arguments.");
			return Type::of(TypeKind::Error);
		}
		if (p_ref.args.size() != 1) {
			error(p_ref.range, "Array takes one element type.");
			return Type::of(TypeKind::Error);
		}
		const Type element = container_element(p_ref.args[0], "Arrays of ");
		return element.is_error() ? element : Type::array(element);
	}

	// The engine types the elements of a container, and the keys of a
	// Dictionary, with one built-in type or one class, and containers one level
	// deep; this language keeps the types only it has boxed (spec §3.8).
	Type container_element(const TypeRef &p_ref, const std::string &p_what) {
		const Type element = resolve(p_ref, false);
		if (!element.is_error() && !element.args.empty() && !is_boxed(element) && !is_typed_container(element)) {
			error(p_ref.range, p_what + element.to_string() + " are not supported yet.");
			return Type::of(TypeKind::Error);
		}
		return element;
	}

	static Type function_type(const FuncDecl &p_function) {
		Type type = Type::function(p_function.return_type, param_types(p_function.params));
		type.is_async = p_function.is_async;
		return type;
	}

	// The engine passes what its signals declare (spec §4.7, §10.2).
	static Type signal_type(const EngineMethod &p_signal) {
		std::vector<Type> params;
		for (const EngineParam &param : p_signal.params) {
			Type type = param.type;
			type.nullable = false;
			params.push_back(type);
		}
		return Type::signal(params);
	}

	/* Expressions */

	Type set(Expr &r_expr, const Type &p_type) {
		r_expr.type = p_type;
		return p_type;
	}

	Type fail(Expr &r_expr, const std::string &p_message) {
		error(r_expr.range, p_message);
		return set(r_expr, Type::of(TypeKind::Error));
	}

	// A call that fails still has its arguments checked, so that their own
	// errors are reported too.
	Type fail_call(Expr &r_call, const std::string &p_message) {
		fail_call(r_call);
		return fail(r_call, p_message);
	}

	// The same, for a call whose error is reported already.
	Type fail_call(Expr &r_call) {
		for (auto &arg : r_call.args) {
			check_value(*arg);
		}
		return set(r_call, Type::of(TypeKind::Error));
	}

	// Makes a call a call of the engine's ResourceLoader, as GDScript's load()
	// and preload() are.
	void on_resource_loader(Expr &r_call) {
		auto loader = std::make_unique<Expr>();
		loader->kind = ExprKind::Name;
		loader->text = "ResourceLoader";
		loader->range = r_call.range;
		loader->name_token = NO_TOKEN;
		r_call.base = std::move(loader);
	}

	static int find_in(const std::vector<std::unordered_map<std::string, int>> &p_scopes, const std::string &p_name) {
		for (auto scope = p_scopes.rbegin(); scope != p_scopes.rend(); ++scope) {
			const auto found = scope->find(p_name);
			if (found != scope->end()) {
				return found->second;
			}
		}
		return -1;
	}

	int find_local(const std::string &p_name) const {
		return find_in(scopes, p_name);
	}

	// A local of the function being checked, or of one around the lambda
	// being checked, which a use would capture.
	const LocalInfo *visible_local(const std::string &p_name) const {
		const int local = find_local(p_name);
		if (local >= 0) {
			return &function->locals[local];
		}
		for (auto outer = enclosing.rbegin(); outer != enclosing.rend(); ++outer) {
			const int found = outer->function ? find_in(outer->scopes, p_name) : -1;
			if (found >= 0) {
				return &outer->function->locals[found];
			}
		}
		return nullptr;
	}

	// A local of a function around the lambda, which the lambda captures,
	// and so do the lambdas between (spec §3.11): the lambda's local for it,
	// or -1 when there is none.
	int capture(const std::string &p_name) {
		for (size_t level = enclosing.size(); level-- > 0;) {
			const Enclosing &outer = enclosing[level];
			int source = outer.function ? find_in(outer.scopes, p_name) : -1;
			if (source < 0) {
				continue;
			}
			// What is narrowed where the lambda is made stays narrowed in it (§5.4).
			const LocalInfo declared = outer.function->locals[source];
			const Type *narrowed_to = narrowed_in(outer.narrowed, source);
			const Type type = narrowed_to ? *narrowed_to : declared.type;
			for (size_t inner = level; inner < enclosing.size(); inner++) {
				const bool innermost = inner + 1 == enclosing.size();
				FuncDecl &into = innermost ? *function : *enclosing[inner + 1].function;
				Expr &lambda = *enclosing[inner].lambda;
				// The value as the function around the lambda reads it.
				auto read = std::make_unique<Expr>();
				read->kind = ExprKind::Name;
				read->text = p_name;
				read->range = lambda.range;
				read->name_token = NO_TOKEN;
				read->target = Target::Local;
				read->index = source;
				read->type = type;
				read->constant = declared.constant;
				lambda.args.push_back(std::move(read));
				source = int(into.locals.size());
				into.locals.push_back({ p_name, type, declared.token, declared.constant });
				into.captures.push_back(source);
				(innermost ? scopes : enclosing[inner + 1].scopes).front()[p_name] = source;
			}
			return source;
		}
		return -1;
	}

	bool is_capture(int p_local) const {
		return function && std::find(function->captures.begin(), function->captures.end(), p_local) != function->captures.end();
	}

	// The function that code being checked is in: the one around a lambda.
	const FuncDecl *outermost() const {
		return enclosing.empty() ? function : enclosing.front().function;
	}

	// The type a name refers to when it is the base of `Name.x` or
	// `Name.f()`: not when a local, a member or a property has that name.
	bool names_type(const Expr &p_expr) const {
		return p_expr.kind == ExprKind::Name && !visible_local(p_expr.text) && !member_index.count(p_expr.text) &&
				!engine.find_property(cls.native_base, p_expr.text) && is_type_name(p_expr.text) && !engine.is_singleton(p_expr.text);
	}

	/* Constants (spec §4.5) */

	// The class's own constants and those it inherits; a name may be declared
	// once among them, the members, the functions and the signals.
	void declare_constants() {
		if (cls.base) {
			cls.all_constants = cls.base->all_constants;
			for (size_t i = 0; i < cls.all_constants.size(); i++) {
				constant_index[cls.all_constants[i]->name] = int(i);
			}
		}
		for (ConstDecl &constant : cls.constants) {
			if (constant_index.count(constant.name) || member_index.count(constant.name)) {
				error(constant.name_token, "\"" + constant.name + "\" is already declared.");
				continue;
			}
			constant_index[constant.name] = int(cls.all_constants.size());
			cls.all_constants.push_back(&constant);
			refer(constant.name_token, script_id(options.path, constant.name), true);
		}
	}

	// What a constant's initializer is checked in: the class, outside any
	// function, wherever the constant is first used.
	struct ClassScope {
		Checker &checker;
		FuncDecl *function;
		std::vector<std::unordered_map<std::string, int>> scopes;
		std::vector<std::unordered_map<int, Type>> narrowed;
		int loop_depth;
		bool in_predelete;
		int initializing;
		bool initializing_onready;

		explicit ClassScope(Checker &r_checker) :
				checker(r_checker), function(r_checker.function), scopes(std::move(r_checker.scopes)), narrowed(std::move(r_checker.narrowed)), loop_depth(r_checker.loop_depth), in_predelete(r_checker.in_predelete), initializing(r_checker.initializing), initializing_onready(r_checker.initializing_onready) {
			checker.function = nullptr;
			checker.scopes.clear();
			checker.narrowed.assign(1, {});
			checker.loop_depth = 0;
			checker.in_predelete = false;
			checker.initializing = -1;
			checker.initializing_onready = false;
		}
		~ClassScope() {
			checker.function = function;
			checker.scopes = std::move(scopes);
			checker.narrowed = std::move(narrowed);
			checker.loop_depth = loop_depth;
			checker.in_predelete = in_predelete;
			checker.initializing = initializing;
			checker.initializing_onready = initializing_onready;
		}
	};

	// A value whose type is `p_type`, from the initializer as written: the
	// type is inferred from it when not declared (spec §4.3, §4.5).
	ConstPtr constant_value(Expr &r_initializer, const std::optional<TypeRef> &p_declared, Type &r_type) {
		r_type = Type::of(TypeKind::Error);
		if (p_declared) {
			r_type = resolve(*p_declared, false);
		}
		const Type value = check_value(r_initializer, r_type);
		if (value.is_error()) {
			return nullptr;
		}
		if (p_declared) {
			if (r_type.is_error()) {
				return nullptr;
			}
			expect_assignable(r_initializer, value, r_type, "assign");
			if (!assignable(value, r_type)) {
				return nullptr;
			}
		} else if (value.kind == TypeKind::None) {
			error(r_initializer.range, "The constant needs a type: None does not say which T? it is (spec §4.5).");
			r_type = Type::of(TypeKind::Error);
			return nullptr;
		} else {
			r_type = value;
		}
		if (!r_initializer.constant) {
			require_constant(r_initializer);
			return nullptr;
		}
		return converted(r_initializer.constant, r_type);
	}

	// Checks a constant of this class when it is first needed, so that a
	// constant may use one declared after it, as in GDScript
	// (modules/gdscript/gdscript_analyzer.cpp:967-1119).
	void check_constant(ConstDecl &r_decl) {
		if (r_decl.state != ConstDecl::State::Unchecked) {
			return;
		}
		r_decl.state = ConstDecl::State::Checking;
		{
			const ClassScope scope(*this);
			const bool was_in_constant = in_constant;
			in_constant = true;
			r_decl.value = constant_value(*r_decl.initializer, r_decl.declared_type, r_decl.type);
			in_constant = was_in_constant;
		}
		r_decl.state = ConstDecl::State::Checked;
	}

	// A use of a constant of the class at `p_path`, which declares it.
	Type use_constant(Expr &r_expr, const ConstDecl &p_decl, const std::string &p_path) {
		refer(r_expr.name_token, script_id(p_path, p_decl.name));
		r_expr.target = Target::Constant;
		if (p_decl.state == ConstDecl::State::Checking) {
			return fail(r_expr, "The constant \"" + p_decl.name + "\" depends on its own value.");
		}
		if (p_decl.type.is_error()) {
			return set(r_expr, p_decl.type);
		}
		r_expr.constant = p_decl.value;
		return set(r_expr, p_decl.type);
	}

	// A constant this class declares or inherits, checked; null if none.
	const ConstDecl *own_constant(const std::string &p_name) {
		const auto found = constant_index.find(p_name);
		if (found == constant_index.end()) {
			return nullptr;
		}
		const ConstDecl *decl = cls.all_constants[found->second];
		if (is_own(*decl, cls.constants)) {
			check_constant(const_cast<ConstDecl &>(*decl));
		}
		return decl;
	}

	/* Enums (spec §3.7) */

	// The class's own enums and those it inherits, before any type names one.
	void declare_enums() {
		if (cls.base) {
			cls.all_enums = cls.base->all_enums;
		}
		for (EnumDecl &decl : cls.enums) {
			decl.script = options.path;
			decl.numbers = std::make_shared<EnumValues>();
			decl.type = decl.name.empty() ? Type::of(TypeKind::Int) : Type::enumeration(display_name(cls, options.path) + "." + decl.name, options.path, decl.numbers);
			if (!decl.name.empty()) {
				refer(decl.name_token, script_id(options.path, decl.name), true);
			}
			for (const EnumValueDecl &value : decl.values) {
				refer(value.name_token, script_id(options.path, enum_value_id(decl, value.name)), true);
			}
			cls.all_enums.push_back(&decl);
		}
	}

	// What a value is called in references: State.IDLE, or A of an unnamed enum.
	static std::string enum_value_id(const EnumDecl &p_decl, const std::string &p_value) {
		return p_decl.name.empty() ? p_value : p_decl.name + "." + p_value;
	}

	bool is_own(const EnumDecl &p_decl) const {
		return &p_decl >= cls.enums.data() && &p_decl < cls.enums.data() + cls.enums.size();
	}

	// Each name once among the class's types, constants and members, after
	// they are declared; then the values, which the declarations of other
	// scripts need too.
	void check_enums() {
		std::unordered_set<std::string> names;
		for (const InterfaceDecl &interface : cls.interfaces) {
			names.insert(interface.name);
		}
		for (const StructDecl &decl : cls.structs) {
			names.insert(decl.name);
		}
		const auto taken = [&](const std::string &p_name) {
			return constant_index.count(p_name) || member_index.count(p_name) || function_index.count(p_name) || signal_index.count(p_name);
		};
		for (const EnumDecl *decl : cls.all_enums) {
			const bool own = is_own(*decl);
			if (!decl->name.empty()) {
				const bool twice = !names.insert(decl->name).second;
				if (own && (twice || taken(decl->name) || is_type_name(decl->name) || (options.project && !options.project->global_class_path(decl->name).empty()))) {
					error(decl->name_token, "\"" + decl->name + "\" is already declared.");
				}
				continue;
			}
			for (const EnumValueDecl &value : decl->values) {
				if (!names.insert(value.name).second || taken(value.name)) {
					if (own) {
						error(value.name_token, "\"" + value.name + "\" is already declared.");
					}
				}
			}
		}
		for (EnumDecl &decl : cls.enums) {
			check_enum(decl);
		}
	}

	// The values in order: each one more than the one before, from 0, unless
	// written as a constant int (spec §3.7, §4.5).
	void check_enum(EnumDecl &r_decl) {
		if (r_decl.state != EnumDecl::State::Unchecked) {
			return;
		}
		r_decl.state = EnumDecl::State::Checking;
		{
			const ClassScope scope(*this);
			const bool was_in_constant = in_constant;
			const EnumDecl *was_enum = current_enum;
			const size_t was_value = current_value;
			in_constant = true;
			current_enum = &r_decl;
			std::unordered_set<std::string> names;
			int64_t next = 0;
			for (size_t i = 0; i < r_decl.values.size(); i++) {
				EnumValueDecl &value = r_decl.values[i];
				if (!r_decl.name.empty() && !names.insert(value.name).second) {
					error(value.name_token, "\"" + value.name + "\" is already a value of " + r_decl.name + ".");
				}
				current_value = i;
				if (value.value) {
					const Type type = check_value(*value.value, Type::of(TypeKind::Int));
					if (type.is_error()) {
					} else if (type.kind != TypeKind::Int && type.kind != TypeKind::Enum) {
						error(value.value->range, "The values of an enum are ints, not " + type.to_string() + ".");
					} else if (!value.value->constant || value.value->constant->kind != ConstValue::Kind::Int) {
						error(value.value->range, "The values of an enum must be known at compile time (spec §4.5).");
					} else {
						next = value.value->constant->i;
					}
				}
				r_decl.numbers->list.push_back({ value.name, next });
				r_decl.constants.push_back(enum_constant(next, r_decl.type));
				next = int64_t(uint64_t(next) + 1);
			}
			if (!r_decl.name.empty()) {
				r_decl.dictionary = enum_dictionary(*r_decl.numbers, r_decl.type);
			}
			in_constant = was_in_constant;
			current_enum = was_enum;
			current_value = was_value;
		}
		r_decl.state = EnumDecl::State::Checked;
	}

	// A named enum this class declares or inherits, checked; null if none.
	const EnumDecl *own_enum(const std::string &p_name) {
		for (const EnumDecl *decl : cls.all_enums) {
			if (!decl->name.empty() && decl->name == p_name) {
				if (is_own(*decl)) {
					check_enum(const_cast<EnumDecl &>(*decl));
				}
				return decl;
			}
		}
		return nullptr;
	}

	// A value written by its bare name: one of an unnamed enum of this class,
	// or one the enum being computed has before the current one.
	std::optional<std::pair<const EnumDecl *, int>> bare_enum_value(const std::string &p_name) {
		if (current_enum && !current_enum->name.empty()) {
			const int index = current_enum->find(p_name);
			if (index >= 0 && size_t(index) < current_value) {
				return std::pair(current_enum, index);
			}
		}
		for (const EnumDecl *decl : cls.all_enums) {
			if (!decl->name.empty()) {
				continue;
			}
			const int index = decl->find(p_name);
			if (index < 0) {
				continue;
			}
			if (is_own(*decl)) {
				EnumDecl &own = const_cast<EnumDecl &>(*decl);
				if (own.state == EnumDecl::State::Checking && (decl != current_enum || size_t(index) >= current_value)) {
					return std::nullopt; // Not computed yet, which the caller reports.
				}
				check_enum(own);
			}
			return std::pair(decl, index);
		}
		return std::nullopt;
	}

	// A value of an enum as a constant of it, or an int for an unnamed one.
	Type use_enum_value(Expr &r_expr, const EnumDecl &p_decl, int p_index) {
		refer(r_expr.name_token, script_id(p_decl.script, enum_value_id(p_decl, p_decl.values[p_index].name)));
		if (size_t(p_index) >= p_decl.constants.size()) {
			return fail(r_expr, "The value \"" + p_decl.values[p_index].name + "\" depends on its own value.");
		}
		r_expr.target = Target::Constant;
		r_expr.constant = p_decl.constants[p_index];
		return set(r_expr, p_decl.constants[p_index]->type);
	}

	// The name of a named enum as a value: its names and values (spec §3.7).
	Type use_enum_dictionary(Expr &r_expr, const EnumDecl &p_decl, bool p_refer = true) {
		if (p_refer) {
			refer(r_expr.name_token, script_id(p_decl.script, p_decl.name));
		}
		if (!p_decl.dictionary) {
			return fail(r_expr, "The enum " + p_decl.name + " depends on its own values.");
		}
		r_expr.target = Target::Constant;
		r_expr.constant = p_decl.dictionary;
		return set(r_expr, p_decl.dictionary->type);
	}

	// The enum that the base of `State.IDLE`, `State(x)` or `State.try_from(x)`
	// names: one of this class by its bare name, when nothing closer has it, or
	// a pub one of another class, as in `Player.State`; null for any other base.
	const EnumDecl *named_enum(Expr &r_base) {
		if (r_base.kind == ExprKind::Name) {
			if (visible_local(r_base.text) || member_index.count(r_base.text) || constant_index.count(r_base.text) || engine.find_property(cls.native_base, r_base.text)) {
				return nullptr;
			}
			const EnumDecl *decl = own_enum(r_base.text);
			if (decl) {
				refer(r_base.name_token, script_id(decl->script, decl->name));
			}
			return decl;
		}
		if (r_base.kind != ExprKind::Member || !r_base.base || !names_enum_of_class(*r_base.base, r_base.text)) {
			return nullptr;
		}
		const std::optional<Type> named = named_class(*r_base.base);
		if (!named || named->is_error() || !is_script_path(named->script)) {
			return nullptr;
		}
		const ClassDecl *decl = named->script == options.path ? &cls : script_decl(named->script);
		const EnumDecl *found = decl ? decl->find_enum(r_base.text) : nullptr;
		if (!found) {
			return nullptr;
		}
		if (named->script == options.path) {
			found = own_enum(r_base.text);
		} else if (!found->is_pub && !extends_script(options.path, named->script)) {
			error(r_base.range, "The enum " + named->name + "." + found->name + " is not pub.");
		}
		refer(r_base.name_token, script_id(found->script, found->name));
		return found;
	}

	// Whether `Class.Name` names an enum of a class of this language, found
	// as named_class() finds the class, without referring to anything yet.
	bool names_enum_of_class(const Expr &p_class, const std::string &p_name) {
		if (p_class.kind != ExprKind::Name || visible_local(p_class.text) || member_index.count(p_class.text) || engine.find_property(cls.native_base, p_class.text)) {
			return false;
		}
		std::string path;
		if (const ConstPtr alias = class_alias(p_class.text)) {
			path = alias->script;
		} else if (constant_index.count(p_class.text)) {
			return false;
		} else if (!cls.class_name.empty() && p_class.text == cls.class_name) {
			path = options.path;
		} else if (options.project) {
			path = options.project->global_class_path(p_class.text);
		}
		if (!is_script_path(path)) {
			return false;
		}
		const ClassDecl *decl = path == options.path ? &cls : script_header(path);
		return decl && (decl->find_enum(p_name) || std::any_of(decl->enums.begin(), decl->enums.end(), [&](const EnumDecl &p_enum) { return p_enum.name == p_name; }));
	}

	// How this file writes an enum: State for its own, Player.State for another class's.
	std::string enum_spelling(const Type &p_enum) const {
		return p_enum.script == options.path ? p_enum.interface_name() : p_enum.name;
	}

	// `State(x)` or `Player.State(x)`: the enum the call names, if any.
	const EnumDecl *called_enum(Expr &r_call) {
		if (!r_call.base) {
			if (function_index.count(r_call.text) || visible_local(r_call.text) || member_index.count(r_call.text)) {
				return nullptr;
			}
			const EnumDecl *decl = own_enum(r_call.text);
			if (decl) {
				refer(r_call.name_token, script_id(decl->script, decl->name));
			}
			return decl;
		}
		if (r_call.base->kind != ExprKind::Name || !names_enum_of_class(*r_call.base, r_call.text)) {
			return nullptr;
		}
		Expr type;
		type.kind = ExprKind::Member;
		type.text = r_call.text;
		type.range = r_call.range;
		type.name_token = r_call.name_token;
		type.base = std::move(r_call.base);
		const EnumDecl *decl = named_enum(type);
		r_call.base = std::move(type.base);
		return decl;
	}

	// An int as one of the enum's values, a program error when it is none
	// (spec §3.7); checked at compile time when the int is known.
	Type check_enum_conversion(Expr &r_expr, const EnumDecl &p_decl) {
		if (r_expr.args.size() != 1 || !r_expr.arg_names.empty()) {
			for (auto &arg : r_expr.args) {
				check_value(*arg);
			}
			return fail_call(r_expr, enum_spelling(p_decl.type) + "() takes one int.");
		}
		Expr &arg = *r_expr.args[0];
		const Type value = check_value(arg, Type::of(TypeKind::Int));
		if (value.is_error()) {
			return set(r_expr, value);
		}
		if (value.kind != TypeKind::Int && value.kind != TypeKind::Enum) {
			return fail(r_expr, enum_spelling(p_decl.type) + "() converts an int, not " + value.to_string() + "." + (value.kind == TypeKind::Float ? " Convert it with int(x) first." : ""));
		}
		if (arg.constant && arg.constant->kind == ConstValue::Kind::Int) {
			if (!p_decl.numbers->has(arg.constant->i)) {
				return fail(r_expr, std::to_string(arg.constant->i) + " is none of the values of " + enum_spelling(p_decl.type) + ".");
			}
			r_expr.constant = enum_constant(arg.constant->i, p_decl.type);
		}
		r_expr.target = Target::EnumFromInt;
		r_expr.param_types = { Type::of(TypeKind::Int) };
		return set(r_expr, p_decl.type);
	}

	// `State.IDLE` (spec §3.7).
	Type check_enum_value(Expr &r_expr, const EnumDecl &p_decl) {
		const int index = p_decl.find(r_expr.text);
		if (index < 0) {
			return fail(r_expr, p_decl.name + " has no value \"" + r_expr.text + "\".");
		}
		return use_enum_value(r_expr, p_decl, index);
	}

	// Whether a type has a default value (spec §4.3): an enum when 0 is one of
	// its values (§3.7).
	bool enum_has_default(const Type &p_enum) const {
		return p_enum.kind != TypeKind::Enum || !p_enum.values || p_enum.values->has(0);
	}

	// A constant initialized with a preload of a script, which names its class
	// where its bare name is used (spec §2.1): a local constant or a constant
	// of this class; null for any other name.
	ConstPtr class_alias(const std::string &p_name) {
		if (const LocalInfo *local = visible_local(p_name)) {
			return local->constant && !local->constant->script.empty() ? local->constant : nullptr;
		}
		const ConstDecl *decl = own_constant(p_name);
		return decl && decl->value && !decl->value->script.empty() ? decl->value : nullptr;
	}

	// The constant that the name of a class alias refers to.
	DeclId alias_id(const std::string &p_name) {
		if (const LocalInfo *local = visible_local(p_name)) {
			return local_id(local->token);
		}
		const ConstDecl &decl = *own_constant(p_name);
		return script_id(declaring(cls, decl, &ClassDecl::constants).path, decl.name);
	}

	// The class that the base of `Base.x` or `Base.f()` names, of this
	// language or another (spec §2.1, §11.1): through a constant of a preload
	// of its script, the preload itself, or its class_name when nothing closer
	// has the name; nullopt for any other base, and Error after reporting.
	std::optional<Type> named_class(Expr &r_base) {
		if (r_base.kind == ExprKind::Call && !r_base.base && r_base.text == "preload") {
			if (!is_script_file(preload_file(preload_path(r_base)))) {
				return std::nullopt;
			}
			check_value(r_base);
			return r_base.constant ? r_base.constant->script_class : Type::of(TypeKind::Error);
		}
		if (r_base.kind != ExprKind::Name) {
			return std::nullopt;
		}
		if (const ConstPtr alias = class_alias(r_base.text)) {
			check_value(r_base); // The constant, whose value is the script.
			return alias->script_class;
		}
		if (visible_local(r_base.text) || member_index.count(r_base.text) || constant_index.count(r_base.text) || engine.find_property(cls.native_base, r_base.text)) {
			return std::nullopt;
		}
		const bool own = !cls.class_name.empty() && r_base.text == cls.class_name;
		const std::string path = own ? options.path : (options.project ? options.project->global_class_path(r_base.text) : std::string());
		if (path.empty()) {
			return std::nullopt;
		}
		if (is_script_path(path)) {
			refer(r_base.name_token, script_id(path));
			const ClassDecl *header = script_header(path);
			if (!header) {
				error(r_base.range, r_base.text + " (" + path + ") cannot be read.");
				return Type::of(TypeKind::Error);
			}
			return script_class_type(*header, path);
		}
		refer(r_base.name_token, foreign_id(path, std::string()));
		const ForeignClass *foreign = foreign_class(path);
		if (!foreign || foreign->native.empty()) {
			return unknown_foreign(r_base, Type::script_class(r_base.text, path, std::string()));
		}
		return foreign_class_type(path, *foreign);
	}

	// A constant of the class at `p_path` (spec §4.5), as `Class.NAME` or
	// `object.NAME`; nullopt when the class has no constant of that name.
	// Other classes see only pub constants, except subclasses (§4.2).
	std::optional<Type> class_constant(Expr &r_expr, const std::string &p_path, const std::string &p_class) {
		if (p_path == options.path) {
			if (const ConstDecl *decl = own_constant(r_expr.text)) {
				return use_constant(r_expr, *decl, declaring(cls, *decl, &ClassDecl::constants).path);
			}
			if (const auto value = bare_enum_value(r_expr.text)) {
				return use_enum_value(r_expr, *value->first, value->second);
			}
			return std::nullopt;
		}
		const ClassDecl *decl = script_decl(p_path);
		if (!decl) {
			return fail(r_expr, p_class + " has errors.");
		}
		for (const ConstDecl *constant : decl->all_constants) {
			if (constant->name != r_expr.text) {
				continue;
			}
			if (!constant->is_pub && !extends_script(options.path, p_path)) {
				return fail(r_expr, "The constant \"" + constant->name + "\" of " + p_class + " is not pub.");
			}
			return use_constant(r_expr, *constant, declaring(*decl, *constant, &ClassDecl::constants).path);
		}
		// A value of an unnamed enum is a constant of the class (spec §3.7).
		for (const EnumDecl *entry : decl->all_enums) {
			const int index = entry->name.empty() ? entry->find(r_expr.text) : -1;
			if (index < 0) {
				continue;
			}
			if (!entry->is_pub && !extends_script(options.path, p_path)) {
				return fail(r_expr, "The constant \"" + r_expr.text + "\" of " + p_class + " is not pub.");
			}
			return use_enum_value(r_expr, *entry, index);
		}
		return std::nullopt;
	}

	// A name of this instance: a constant, a member, the owner, a property, a
	// function or a signal; also what `self.name` reads, past the locals.
	std::optional<Type> check_self_name(Expr &r_expr) {
		if (const auto value = bare_enum_value(r_expr.text)) {
			return use_enum_value(r_expr, *value->first, value->second);
		}
		if (const ConstDecl *constant = own_constant(r_expr.text)) {
			return use_constant(r_expr, *constant, declaring(cls, *constant, &ClassDecl::constants).path);
		}
		if (const EnumDecl *decl = own_enum(r_expr.text)) {
			return use_enum_dictionary(r_expr, *decl);
		}
		const auto member = member_index.find(r_expr.text);
		if (member != member_index.end()) {
			const VarDecl &read = *cls.all_members[member->second];
			if (in_constant) {
				refer(r_expr.name_token, script_id(declaring(cls, read, &ClassDecl::members).path, read.name));
				return fail(r_expr, "\"" + r_expr.text + "\" is a variable, not a constant (spec §4.5).");
			}
			if (initializing >= 0) {
				// @onready initializers run when the node is ready, after the
				// others (spec §6.3); each kind runs in declaration order (§6.1).
				const bool earlier = member->second < initializing;
				const bool ready = read.is_onready ? initializing_onready && earlier : initializing_onready || earlier;
				if (!ready && read.is_onready && !initializing_onready) {
					return fail(r_expr, "\"" + r_expr.text + "\" is set when the node is ready, after this initializer runs (spec §6.3).");
				}
				if (!ready) {
					return fail(r_expr, "\"" + r_expr.text + "\" is not initialized yet: initializers run in declaration order (spec §6.1).");
				}
			}
			if (read.is_onready && function && (function->name == "_init" || function->name == "_enter_tree")) {
				return fail(r_expr, "\"" + r_expr.text + "\" is set when the node is ready, after " + function->name + "() runs (spec §6.3).");
			}
			if (refuse_in_static(r_expr, r_expr.text)) {
				return set(r_expr, Type::of(TypeKind::Error));
			}
			r_expr.target = Target::Member;
			r_expr.index = member->second;
			refer(r_expr.name_token, script_id(declaring(cls, read, &ClassDecl::members).path, read.name));
			return set(r_expr, cls.all_members[member->second]->type);
		}
		if (r_expr.text == "owner" && cls.owner_ref) {
			if (refuse_in_static(r_expr, r_expr.text)) {
				return set(r_expr, Type::of(TypeKind::Error));
			}
			r_expr.target = Target::Owner;
			refer(r_expr.name_token, engine_id("Node", "owner"));
			return set(r_expr, cls.owner_type);
		}
		if (const auto property = engine.find_property(cls.native_base, r_expr.text)) {
			if (refuse_in_static(r_expr, r_expr.text)) {
				return set(r_expr, Type::of(TypeKind::Error));
			}
			r_expr.target = Target::SelfProperty;
			refer(r_expr.name_token, engine_id(property->owner, r_expr.text));
			return set(r_expr, property_type(*property, r_expr));
		}
		const auto function_found = function_index.find(r_expr.text);
		if (function_found != function_index.end()) {
			// A method value (spec §3.11), which calls the function on this instance.
			if (refuse_in_static(r_expr, r_expr.text)) {
				return set(r_expr, Type::of(TypeKind::Error));
			}
			r_expr.target = Target::MethodValue;
			r_expr.index = function_found->second;
			const FuncDecl &method = *cls.all_functions[function_found->second];
			refer(r_expr.name_token, script_id(declaring(cls, method, &ClassDecl::functions).path, method.name));
			return set(r_expr, function_type(*cls.all_functions[function_found->second]));
		}
		const auto own_signal = signal_index.find(r_expr.text);
		if (own_signal != signal_index.end()) {
			if (refuse_in_static(r_expr, r_expr.text)) {
				return set(r_expr, Type::of(TypeKind::Error));
			}
			r_expr.target = Target::SelfSignal;
			const SignalDecl &signal = *cls.all_signals[own_signal->second];
			refer(r_expr.name_token, script_id(declaring(cls, signal, &ClassDecl::signals).path, signal.name));
			return set(r_expr, Type::signal(param_types(signal.params)));
		}
		if (const auto signal = engine.find_signal(cls.native_base, r_expr.text)) {
			if (refuse_in_static(r_expr, r_expr.text)) {
				return set(r_expr, Type::of(TypeKind::Error));
			}
			r_expr.target = Target::SelfSignal;
			refer(r_expr.name_token, engine_id(signal->owner, r_expr.text));
			return set(r_expr, signal_type(*signal));
		}
		return std::nullopt;
	}

	Type check_self(Expr &r_expr) {
		if (refuse_in_static(r_expr, "self")) {
			return set(r_expr, Type::of(TypeKind::Error));
		}
		if (in_predelete) {
			return fail(r_expr, "self cannot be used as a value in the PREDELETE branch (spec §6.6).");
		}
		return set(r_expr, script_class_type(cls, options.path));
	}

	Type check_name(Expr &r_expr) {
		int local = find_local(r_expr.text);
		if (local < 0 && !enclosing.empty()) {
			local = capture(r_expr.text);
		}
		if (local >= 0) {
			r_expr.target = Target::Local;
			r_expr.index = local;
			r_expr.constant = function->locals[local].constant;
			refer(r_expr.name_token, local_id(function->locals[local].token));
			const Type *narrowed_to = narrowed_type(local);
			return set(r_expr, narrowed_to ? *narrowed_to : function->locals[local].type);
		}
		if (const std::optional<Type> found = check_self_name(r_expr)) {
			return *found;
		}
		if (engine.is_singleton(r_expr.text)) {
			r_expr.target = Target::Singleton;
			refer(r_expr.name_token, engine_id(r_expr.text));
			return set(r_expr, Type::object(r_expr.text));
		}
		const std::string autoload = options.project ? options.project->autoload_path(r_expr.text) : std::string();
		if (!autoload.empty()) {
			// Its type is its script's class (spec §7.3).
			if (!is_script_path(autoload)) {
				const ForeignClass *foreign = foreign_class(autoload);
				if (!foreign || foreign->native.empty()) {
					// Without loading it, which only the declarations do not, a script without a global name is not known.
					return options.declarations_only ? set(r_expr, Type::of(TypeKind::Error)) : fail(r_expr, "The autoload " + r_expr.text + " (" + autoload + ") cannot be read.");
				}
				r_expr.target = Target::Autoload;
				refer(r_expr.name_token, { DeclId::Kind::Autoload, std::string(), r_expr.text });
				return set(r_expr, Type::script_class(r_expr.text, autoload, foreign->native));
			}
			const ClassDecl *header = script_header(autoload);
			if (!header) {
				return fail(r_expr, "The autoload " + r_expr.text + " (" + autoload + ") cannot be read.");
			}
			r_expr.target = Target::Autoload;
			refer(r_expr.name_token, { DeclId::Kind::Autoload, std::string(), r_expr.text });
			return set(r_expr, script_class_type(*header, autoload));
		}
		if (const std::optional<int64_t> value = builtin::global_constant(r_expr.text)) {
			r_expr.target = Target::EngineConstant;
			r_expr.int_value = *value;
			refer(r_expr.name_token, engine_id("@GlobalScope", r_expr.text));
			return set(r_expr, Type::of(TypeKind::Int));
		}
		if (is_type_name(r_expr.text)) {
			return fail(r_expr, "\"" + r_expr.text + "\" is a type, not a value.");
		}
		return fail(r_expr, "Identifier \"" + r_expr.text + "\" is not declared in the current scope.");
	}

	// A member or signal of an object of a class of this language, which other
	// code reaches by name, so only when it is pub (spec §10.5); nullopt when
	// the class has no such member, so that the engine class's are looked at.
	std::optional<Type> script_member(Expr &r_expr, const Type &p_class) {
		if (!is_script_path(p_class.script)) {
			return foreign_member(r_expr, p_class);
		}
		const ClassDecl *decl = script_decl(p_class.script);
		if (!decl) {
			return fail(r_expr, p_class.script + " has errors.");
		}
		if (p_class.is_interface) {
			// Read and written by name, whichever member or engine property provides it.
			const InterfaceProperty *property = interface_property(p_class, r_expr.text);
			if (!property) {
				return std::nullopt;
			}
			r_expr.target = Target::ObjectProperty;
			r_expr.owner = p_class.engine_class();
			refer(r_expr.name_token, script_id(p_class.script, p_class.interface_name() + "." + property->name));
			return property->type;
		}
		if (const std::optional<Type> constant = class_constant(r_expr, p_class.script, p_class.name)) {
			return constant;
		}
		for (const VarDecl *entry : decl->all_members) {
			const VarDecl &member = *entry;
			if (member.name != r_expr.text) {
				continue;
			}
			if (!member.is_pub) {
				return fail(r_expr, not_pub(p_class, "\"" + member.name + "\""));
			}
			if (member.type.is_error()) {
				// Typed from the scenes, which are not read for another script, or
				// not checked yet.
				return fail(r_expr, "The type of " + p_class.name + "." + member.name + " is not known here; declare it, as in `pub var " + member.name + ": T`.");
			}
			r_expr.target = Target::ObjectProperty;
			r_expr.owner = p_class.engine_class();
			refer(r_expr.name_token, script_id(declaring(*decl, member, &ClassDecl::members).path, member.name));
			return member.type;
		}
		for (const SignalDecl *entry : decl->all_signals) {
			const SignalDecl &signal = *entry;
			if (signal.name != r_expr.text) {
				continue;
			}
			// A signal is reached by name like a method, and the engine knows
			// every signal a class declares, so only other classes are refused.
			if (!signal.is_pub && p_class.script != options.path) {
				return fail(r_expr, not_pub(p_class, "The signal \"" + signal.name + "\""));
			}
			r_expr.target = Target::ObjectSignal;
			refer(r_expr.name_token, script_id(declaring(*decl, signal, &ClassDecl::signals).path, signal.name));
			return Type::signal(param_types(signal.params));
		}
		return std::nullopt;
	}

	// Members without pub are visible in their own file (spec §4.2), but other
	// instances are reached by name through the engine, which sees only pub
	// members (§10.5).
	std::string not_pub(const Type &p_class, const std::string &p_what) const {
		if (p_class.script == options.path) {
			return p_what + " is not pub, and members that are not pub of other instances of this class are not supported yet.";
		}
		return p_what + " of " + p_class.name + " is not pub.";
	}

	// The methods an interface declares (spec §3.9).
	const InterfaceDecl *interface_decl(const Type &p_interface) const {
		const ClassDecl *decl = script_decl(p_interface.script);
		if (!decl) {
			return nullptr;
		}
		const std::string name = p_interface.interface_name();
		for (const InterfaceDecl &interface : decl->interfaces) {
			if (interface.name == name) {
				return &interface;
			}
		}
		return nullptr;
	}

	const InterfaceProperty *interface_property(const Type &p_interface, const std::string &p_name) const {
		if (const InterfaceDecl *interface = interface_decl(p_interface)) {
			for (const InterfaceProperty &property : interface->properties) {
				if (property.name == p_name) {
					return &property;
				}
			}
		}
		return nullptr;
	}

	// A pub method of an object of a class of this language, or a method of an
	// interface, which this language calls directly (spec §8.1); an override
	// of an engine virtual method is called through the engine, which runs it.
	std::optional<Type> script_method(Expr &r_expr, const Type &p_class) {
		if (!p_class.is_interface && !is_script_path(p_class.script)) {
			return foreign_method(r_expr, p_class);
		}
		const FuncDecl *callee = nullptr;
		DeclId id;
		if (p_class.is_interface) {
			if (const InterfaceDecl *interface = interface_decl(p_class)) {
				for (const FuncDecl &method : interface->methods) {
					if (method.name == r_expr.text) {
						callee = &method;
						id = script_id(p_class.script, interface->name + "." + method.name);
					}
				}
			}
		} else if (const ClassDecl *decl = script_decl(p_class.script)) {
			for (const FuncDecl *entry : decl->all_functions) {
				const FuncDecl &function = *entry;
				if (function.name == r_expr.text) {
					callee = &function;
					id = script_id(declaring(*decl, function, &ClassDecl::functions).path, function.name);
				}
			}
		} else {
			return fail(r_expr, p_class.script + " has errors.");
		}
		if (!callee) {
			return std::nullopt;
		}
		if (!callee->is_pub && !callee->is_override) {
			return fail_call(r_expr, not_pub(p_class, "\"" + callee->name + "()\""));
		}
		check_arguments(r_expr, param_types(callee->params), required_params(*callee), false);
		refer(r_expr.name_token, id);
		if (callee->is_override && !callee->is_async) {
			r_expr.target = Target::EngineMethod;
		} else {
			r_expr.target = Target::ScriptMethod;
			r_expr.engine_params = param_types(callee->params);
			r_expr.index = int(callee->params.size()) - required_params(*callee);
		}
		r_expr.is_async = callee->is_async;
		return set(r_expr, callee->is_async ? Type::task(callee->return_type) : callee->return_type);
	}

	// An integer constant of an engine class, such as an enum value, which is
	// an int (spec §10.1).
	Type check_engine_constant(Expr &r_expr, const std::string &p_class) {
		const std::optional<int64_t> value = engine.find_constant(p_class, r_expr.text);
		if (!value) {
			return fail(r_expr, p_class + " has no constant \"" + r_expr.text + "\".");
		}
		r_expr.target = Target::EngineConstant;
		r_expr.int_value = *value;
		refer(r_expr.name_token, engine_id(p_class, r_expr.text));
		return set(r_expr, Type::of(TypeKind::Int));
	}

	Type check_member_access(Expr &r_expr) {
		Expr &base = *r_expr.base;
		if (base.kind == ExprKind::Self) {
			if (const std::optional<Type> found = check_self_name(r_expr)) {
				return *found;
			}
			return refuse_in_static(base, "self") ? set(r_expr, Type::of(TypeKind::Error)) : fail(r_expr, display_name(cls, options.path) + " has no member \"" + r_expr.text + "\".");
		}
		if (const EnumDecl *decl = named_enum(base)) {
			return check_enum_value(r_expr, *decl);
		}
		if (base.kind == ExprKind::Name && names_enum_of_class(base, r_expr.text)) {
			// `Player.State`, the enum's name as a value.
			if (const EnumDecl *decl = named_enum(r_expr)) {
				return use_enum_dictionary(r_expr, *decl, false);
			}
			return set(r_expr, Type::of(TypeKind::Error));
		}
		if (const std::optional<Type> named = named_class(base)) {
			if (named->is_error()) {
				return set(r_expr, *named);
			}
			if (!is_script_path(named->script)) {
				return check_foreign_constant(r_expr, *named);
			}
			if (const std::optional<Type> constant = class_constant(r_expr, named->script, named->name)) {
				return *constant;
			}
			return fail(r_expr, named->name + " has no constant \"" + r_expr.text + "\".");
		}
		if (names_type(base)) {
			refer(base.name_token, engine_id(base.text));
			if (const builtin::Class *type = builtin::find_class(base.text)) {
				if (const builtin::Constant *constant = builtin::find_constant(*type, r_expr.text)) {
					r_expr.target = Target::BuiltinConstant;
					r_expr.owner = base.text;
					refer(r_expr.name_token, engine_id(base.text, r_expr.text));
					return set(r_expr, api_type(constant->type, false));
				}
				return fail(r_expr, base.text + " has no constant \"" + r_expr.text + "\".");
			}
			return check_engine_constant(r_expr, base.text);
		}

		const Type base_type = check_value(base);
		if (base_type.is_error()) {
			return set(r_expr, base_type);
		}
		if (base_type.is_optional()) {
			return fail(r_expr, "The value may be None (" + base_type.to_string() + "); check it before reading \"" + r_expr.text + "\".");
		}
		if (base_type.kind == TypeKind::Struct) {
			const StructDecl *decl = struct_decl(base_type);
			if (!decl) {
				return fail(r_expr, base_type.script + " has errors.");
			}
			for (size_t i = 0; i < decl->fields.size(); i++) {
				if (decl->fields[i].name == r_expr.text) {
					r_expr.target = Target::StructField;
					r_expr.index = int(i);
					refer(r_expr.name_token, script_id(base_type.script, decl->name + "." + decl->fields[i].name));
					return set(r_expr, decl->fields[i].type);
				}
			}
			return fail(r_expr, base_type.name + " has no field \"" + r_expr.text + "\".");
		}
		const std::string builtin_name = base_type.builtin_name();
		if (!builtin_name.empty()) {
			const builtin::Class *type = builtin::find_class(builtin_name);
			const builtin::Member *member = type ? builtin::find_member(*type, r_expr.text) : nullptr;
			if (!member) {
				return fail(r_expr, base_type.to_string() + " has no member \"" + r_expr.text + "\".");
			}
			r_expr.target = Target::Field;
			r_expr.owner = builtin_name;
			refer(r_expr.name_token, engine_id(builtin_name, r_expr.text));
			return set(r_expr, api_type(member->type, false));
		}
		if (base_type.kind == TypeKind::Object) {
			if (!base_type.script.empty()) {
				const std::optional<Type> member = script_member(r_expr, base_type);
				if (member) {
					return set(r_expr, *member);
				}
			}
			const std::string &engine_class = base_type.engine_class();
			const auto property = engine.find_property(engine_class, r_expr.text);
			if (!property) {
				if (const auto signal = engine.find_signal(engine_class, r_expr.text)) {
					r_expr.target = Target::ObjectSignal;
					refer(r_expr.name_token, engine_id(signal->owner, r_expr.text));
					return set(r_expr, signal_type(*signal));
				}
				if (base.target == Target::Singleton && engine.find_constant(engine_class, r_expr.text)) {
					return check_engine_constant(r_expr, engine_class); // Such as Input.MOUSE_MODE_CAPTURED.
				}
				return fail(r_expr, base_type.name + " has no property \"" + r_expr.text + "\".");
			}
			r_expr.target = Target::ObjectProperty;
			r_expr.owner = engine_class;
			refer(r_expr.name_token, engine_id(property->owner, r_expr.text));
			return set(r_expr, property_type(*property, r_expr));
		}
		return fail(r_expr, "Values of type " + base_type.to_string() + " have no members.");
	}

	// A node path as written: $Head/Camera, %Name or $"../Level".
	static std::string shown_path(const std::string &p_path) {
		if (p_path[0] == '%') {
			return p_path;
		}
		const bool plain = p_path[0] != '/' && std::all_of(p_path.begin(), p_path.end(), [](char p_char) {
			return std::isalnum((unsigned char)p_char) || p_char == '_' || p_char == '/' || p_char == '%';
		});
		return plain ? "$" + p_path : "$\"" + p_path + "\"";
	}

	static std::vector<std::string> path_parts(const std::string &p_path) {
		std::vector<std::string> parts;
		for (size_t start = 0;;) {
			const size_t slash = p_path.find('/', start);
			parts.push_back(p_path.substr(start, slash - start));
			if (slash == std::string::npos) {
				return parts;
			}
			start = slash + 1;
		}
	}

	static std::string parent_path(const std::string &p_path) {
		const size_t slash = p_path.rfind('/');
		return slash == std::string::npos ? std::string(".") : p_path.substr(0, slash);
	}

	// Where a path leads from the node at `p_from` of a scene, part by part, as
	// the engine resolves it (scene/main/node.cpp:1924-1961; ed1daf0). `%Name`
	// is a node the current node owns, or else one its owner does: the scene's
	// root owns the scene's nodes, and an instanced scene's root those of that
	// scene (scene/resources/packed_scene.cpp:567-573).
	struct NodeWalk {
		std::vector<std::string> reached; // The node after each part.
		const SceneNode *node = nullptr; // Where it ends, unless:
		std::string missing; // the scene has no such node;
		std::string unsupported; // or the scene's own nodes do not tell.
	};

	static NodeWalk walk_node_path(const Scene &p_scene, const std::string &p_from, const std::string &p_path) {
		NodeWalk walk;
		std::string at = p_from;
		const auto into_instance = [&]() {
			for (std::string path = at;; path = parent_path(path)) {
				const SceneNode *node = p_scene.find_node(path);
				if (node && node->is_instance) {
					walk.unsupported = p_scene.path + ": paths into an instanced scene, such as " + node->path + ", are not supported yet.";
					return true;
				}
				if (path == ".") {
					return false;
				}
			}
		};
		for (const std::string &part : path_parts(p_path)) {
			if (part == "..") {
				if (at == ".") {
					walk.unsupported = p_scene.path + ": " + shown_path(p_path) + " goes above the root of the scene, which is not supported yet.";
					return walk;
				}
				at = parent_path(at);
			} else if (!part.empty() && part[0] == '%') {
				const SceneNode *current = p_scene.find_node(at);
				if (current && (current->is_instance || current->type.empty()) && into_instance()) {
					return walk;
				}
				const std::string name = part.substr(1);
				const auto unique = std::find_if(p_scene.nodes.begin(), p_scene.nodes.end(), [&](const SceneNode &p_node) {
					return p_node.unique && p_node.path.substr(p_node.path.rfind('/') + 1) == name;
				});
				if (unique == p_scene.nodes.end()) {
					if (at == ".") {
						// The root's owner is the root of a scene that instances this one.
						walk.unsupported = p_scene.path + ": the scene has no node with the unique name " + part + "; unique names of the scenes that instance it are not supported yet.";
					} else {
						walk.missing = p_scene.path + ": no node has the unique name " + part + " (spec §7.1).";
					}
					return walk;
				}
				at = unique->path;
			} else if (!part.empty() && part != ".") {
				const std::string child = at == "." ? part : at + "/" + part;
				if (!p_scene.find_node(child)) {
					if (!into_instance()) {
						walk.missing = p_scene.path + ": " + (p_from == "." ? std::string("the root") : p_from) + " has no node at \"" + p_path + "\" (spec §7.1).";
					}
					return walk;
				}
				at = child;
			}
			walk.reached.push_back(at);
		}
		walk.node = p_scene.find_node(at);
		return walk;
	}

	// `$X` (spec §7.1): the node at the path from every node that has this
	// script, in every scene; they must agree on its class. `$".."` is any
	// Node (spec §7.3).
	Type check_get_node(Expr &r_expr) {
		if (options.declarations_only) {
			return set(r_expr, Type::of(TypeKind::Error)); // Scenes are not read while loading.
		}
		const std::string &path = r_expr.text;
		if (refuse_in_static(r_expr, shown_path(path))) {
			return set(r_expr, Type::of(TypeKind::Error));
		}
		if (path == "..") {
			r_expr.owner = "Node";
			return set(r_expr, Type::object("Node"));
		}
		if (path.empty() || path[0] == '/') {
			return fail(r_expr, "Only paths from this node, such as $Head/Camera, are supported yet.");
		}
		bool attached = false;
		std::optional<Type> type;
		std::string type_scene;
		for (const Scene &scene : scenes) {
			for (const SceneNode &node : scene.nodes) {
				if (!extends_script(node.script, options.path)) {
					continue;
				}
				attached = true;
				const NodeWalk walk = walk_node_path(scene, node.path, path);
				if (!walk.unsupported.empty()) {
					return fail(r_expr, walk.unsupported);
				}
				if (!walk.node) {
					error(r_expr.range, walk.missing);
					continue;
				}
				refer_node_path(r_expr, scene, walk);
				const Type found_type = scene_node_type(r_expr, *walk.node, shown_path(path));
				if (found_type.is_error()) {
					return set(r_expr, found_type);
				}
				if (!type) {
					type = found_type;
					type_scene = scene.path;
				} else if (found_type != *type) {
					return fail(r_expr, shown_path(path) + " is a " + type->to_string() + " in " + type_scene + " but a " + found_type.to_string() + " in " + scene.path + "; assign it to a variable typed with a class both extend (spec §7.1).");
				}
			}
		}
		if (!attached) {
			return fail(r_expr, shown_path(path) + " is typed from the scenes that use this script, and no scene does (spec §7.2).");
		}
		if (!type) {
			return set(r_expr, Type::of(TypeKind::Error)); // Reported for each scene above.
		}
		r_expr.owner = type->engine_class();
		return set(r_expr, *type);
	}

	// Each name in the path of `$X` refers to the node it reaches in a scene;
	// the name of `%Name` without the "%". Not the parts of a string written
	// with escapes.
	void refer_node_path(const Expr &p_get_node, const Scene &p_scene, const NodeWalk &p_walk) {
		const std::vector<Token> &tokens = cls.source->tokens;
		using Bytes = std::optional<std::pair<uint32_t, uint32_t>>;
		std::vector<Bytes> parts;
		for (uint32_t i = p_get_node.range.first; i <= p_get_node.range.last && i < tokens.size(); i++) {
			const Token &token = tokens[i];
			if (token.kind == TokenKind::Dollar || token.kind == TokenKind::Percent || token.kind == TokenKind::Slash) {
				continue;
			}
			if (token.kind != TokenKind::String) {
				parts.push_back(Bytes({ token.offset, token.end() }));
				continue;
			}
			const bool plain = token.length == token.text.size() + 2;
			uint32_t first = 0; // Of the part in the string.
			for (uint32_t c = 0; c <= token.text.size(); c++) {
				if (c < token.text.size() && token.text[c] != '/') {
					continue;
				}
				const uint32_t unique = c > first && token.text[first] == '%';
				parts.push_back(plain ? Bytes({ token.offset + 1 + first + unique, token.offset + 1 + c }) : Bytes());
				first = c + 1;
			}
		}
		const std::vector<std::string> names = path_parts(p_get_node.text);
		if (parts.size() != names.size() || p_walk.reached.size() != names.size()) {
			return;
		}
		for (size_t i = 0; i < names.size(); i++) {
			if (parts[i] && !names[i].empty() && names[i] != "." && names[i] != "..") {
				refer_bytes(parts[i]->first, parts[i]->second, { DeclId::Kind::Node, p_scene.path, p_walk.reached[i] });
			}
		}
	}

	// The type of a node of a scene (spec §7.1, §7.4): the class of its script
	// when that is a script of this language, or else its engine class; Error
	// after reporting. `p_what` names the node in messages.
	Type scene_node_type(const Expr &p_at, const SceneNode &p_node, const std::string &p_what) {
		const std::string &script = p_node.script.empty() ? p_node.instance_script : p_node.script;
		if (is_script_path(script)) {
			const ClassDecl *header = script_header(script);
			if (!header) {
				error(p_at.range, p_what + " has the script " + script + ", which cannot be read.");
				return Type::of(TypeKind::Error);
			}
			return script_class_type(*header, script);
		}
		if (!engine.class_exists(p_node.type)) {
			error(p_at.range, p_what + " is a " + p_node.type + ", which is not supported yet.");
			return Type::of(TypeKind::Error);
		}
		return Type::object(p_node.type);
	}

	static bool has_extension(const std::string &p_path, std::initializer_list<const char *> p_extensions) {
		const size_t dot = p_path.find_last_of("./");
		const std::string extension = dot != std::string::npos && p_path[dot] == '.' ? p_path.substr(dot) : std::string();
		return std::any_of(p_extensions.begin(), p_extensions.end(), [&](const char *p_extension) { return extension == p_extension; });
	}

	// The path that a preload() of one literal String names, from this
	// script's directory when it is relative, as in GDScript; a uid:// path as
	// written. Empty for any other preload().
	std::string preload_path(const Expr &p_call) const {
		// As in GDScript, not a StringName or a NodePath
		// (modules/gdscript/gdscript_analyzer.cpp:4735-4736).
		if (p_call.args.size() != 1 || p_call.args[0]->kind != ExprKind::StringLiteral || p_call.args[0]->op != TokenKind::String) {
			return std::string();
		}
		const std::string &path = p_call.args[0]->text;
		if (path.rfind("uid://", 0) == 0) {
			return path;
		}
		return simplify_path(path.rfind("res://", 0) == 0 ? path : options.path.substr(0, options.path.find_last_of('/') + 1) + path);
	}

	// The file at a preload's path: the file a uid names; empty when there is none.
	std::string preload_file(const std::string &p_path) const {
		if (p_path.rfind("uid://", 0) == 0) {
			return options.project ? options.project->uid_path(p_path) : std::string();
		}
		return p_path;
	}

	static bool is_script_file(const std::string &p_file) {
		return has_extension(p_file, { ".pet", ".gd", ".cs" });
	}

	// preload() of a resource (spec §7.4): a scene is PackedScene[T], T its
	// root's type; a script its class (§2.1); any other resource the class its
	// file loads as, or its script's class. It loads when it runs, through the
	// engine's resource cache, as load() does, and the result is checked to be
	// of that type.
	Type check_preload(Expr &r_expr) {
		const std::string path = preload_path(r_expr);
		if (path.empty()) {
			return fail_call(r_expr, "preload() takes one literal path, a String.");
		}
		r_expr.args[0]->text = path;
		// A uid stays what is loaded, so that the file can move.
		const std::string file = preload_file(path);
		if (file.empty() && !options.declarations_only) {
			return fail(r_expr, "There is no resource with the uid " + path + ".");
		}
		Type type;
		Type script_class;
		if (is_script_file(file)) {
			// Read without loading, also while only the declarations are
			// checked, so that other classes see the types it names.
			script_class = preloaded_class(r_expr, file);
			if (script_class.is_error() && !options.declarations_only) {
				return set(r_expr, script_class);
			}
			type = script_value_type(file);
		} else if (options.declarations_only) {
			return set(r_expr, Type::of(TypeKind::Error)); // Resources are not read while loading.
		} else if (has_extension(file, { ".tscn", ".scn" })) {
			const std::optional<Scene> scene = options.project ? options.project->scene(file) : std::nullopt;
			const SceneNode *root = scene ? scene->find_node(".") : nullptr;
			if (!root) {
				return fail(r_expr, "There is no scene at " + file + ".");
			}
			const Type root_type = scene_node_type(r_expr, *root, "The root of " + file);
			if (root_type.is_error()) {
				return set(r_expr, root_type);
			}
			type = Type::scene(root_type);
		} else {
			const std::optional<ResourceInfo> resource = options.project ? options.project->resource(file) : std::nullopt;
			if (!resource) {
				return fail(r_expr, "There is no resource at " + file + ".");
			}
			type = resource_type(r_expr, file, *resource);
			if (type.is_error()) {
				return set(r_expr, type);
			}
		}
		if (!engine.is_singleton("ResourceLoader")) {
			return fail(r_expr, "preload() needs the engine's ResourceLoader.");
		}
		refer(r_expr.name_token, language_id("@GlobalScope", "preload"));
		// A call of ResourceLoader.load(), as written nowhere.
		on_resource_loader(r_expr);
		r_expr.text = "load";
		r_expr.name_token = NO_TOKEN;
		if (check_call(r_expr).is_error()) {
			return set(r_expr, Type::of(TypeKind::Error));
		}
		// A constant expression, loaded where it is used (spec §4.5, §7.4).
		auto value = std::make_shared<ConstValue>();
		value->kind = ConstValue::Kind::Preload;
		value->type = type;
		value->text = path;
		value->source = &r_expr;
		if (is_script_file(file)) {
			value->script = file;
			value->script_class = script_class;
		}
		r_expr.constant = value;
		return set(r_expr, type);
	}

	// The class of a preloaded script (spec §2.1), from its header, or from
	// reflection for another language; Error after reporting one that cannot
	// be read, and also without reporting one of another language without a
	// global name while only the declarations are checked, which load no
	// script.
	Type preloaded_class(const Expr &p_preload, const std::string &p_file) {
		if (is_script_path(p_file)) {
			const ClassDecl *header = script_header(p_file);
			if (!header) {
				error(p_preload.range, "There is no script of this language at " + p_file + ", or it has syntax errors.");
				return Type::of(TypeKind::Error);
			}
			return script_class_type(*header, p_file);
		}
		const ForeignClass *foreign = foreign_class(p_file);
		if (!foreign || foreign->native.empty()) {
			if (!options.declarations_only) {
				error(p_preload.range, "There is no script at " + p_file + ", or it cannot be loaded.");
			}
			return Type::of(TypeKind::Error);
		}
		return foreign_class_type(p_file, *foreign);
	}

	// A script used as a value (spec §2.1): of the class of its resource, such
	// as GDScript, as the project knows it.
	Type script_value_type(const std::string &p_file) const {
		const std::optional<ResourceInfo> resource = options.project ? options.project->resource(p_file) : std::nullopt;
		if (resource && engine.class_exists(resource->type) && engine.inherits(resource->type, "Script")) {
			return Type::object(resource->type);
		}
		return Type::object("Script");
	}

	// The class of a preloaded resource: its script's, or the engine class its
	// file loads as, as GDScript types it by the resource it loads
	// (modules/gdscript/gdscript_analyzer.cpp:4757-4780).
	Type resource_type(const Expr &p_preload, const std::string &p_path, const ResourceInfo &p_resource) {
		if (!p_resource.script_class.empty()) {
			TypeRef ref;
			ref.name = p_resource.script_class;
			ref.range = p_preload.range;
			ref.name_token = NO_TOKEN;
			return resolve(ref, false);
		}
		if (!engine.class_exists(p_resource.type) || !(p_resource.type == "Resource" || engine.inherits(p_resource.type, "Resource"))) {
			error(p_preload.range, "The file at " + p_path + " is not a resource the engine knows" + (p_resource.type.empty() ? std::string(".") : ": " + p_resource.type + "."));
			return Type::of(TypeKind::Error);
		}
		return Type::object(p_resource.type);
	}

	// An enum's value takes part in operations as the int it is (spec §3.7).
	static Type as_int(const Type &p_type) {
		return p_type.kind == TypeKind::Enum ? Type::of(TypeKind::Int) : p_type;
	}

	Type check_unary(Expr &r_expr) {
		const Type operand = as_int(check_value(*r_expr.left));
		if (operand.is_error()) {
			return set(r_expr, operand);
		}
		if (r_expr.op == TokenKind::Minus) {
			if (operand.is_numeric()) {
				return set(r_expr, operand);
			}
			if (operand.kind == TypeKind::Builtin) {
				const builtin::Class *type = builtin::find_class(operand.name);
				for (uint32_t i = 0; type && i < type->operator_count; i++) {
					if (std::string(type->operators[i].name) == "unary-") {
						return set(r_expr, api_type(type->operators[i].return_type, false));
					}
				}
			}
			return fail(r_expr, "Cannot negate a value of type " + operand.to_string() + ".");
		}
		if (r_expr.op == TokenKind::Tilde) {
			return operand.kind == TypeKind::Int ? set(r_expr, operand) : fail(r_expr, "\"~\" requires int, found " + operand.to_string() + ".");
		}
		if (operand.kind == TypeKind::Bool) {
			return set(r_expr, operand);
		}
		return fail(r_expr, "\"not\" requires bool, found " + operand.to_string() + ". Compare explicitly, for example x == 0.");
	}

	// The result of `p_left op p_right` (spec §5.1), or Error after reporting.
	Type binary_type(TokenKind p_op, const Type &p_left, const Type &p_right, const TokenRange &p_at) {
		if (p_left.is_error() || p_right.is_error()) {
			return Type::of(TypeKind::Error);
		}
		const bool ordering = p_op == TokenKind::Less || p_op == TokenKind::LessEqual || p_op == TokenKind::Greater || p_op == TokenKind::GreaterEqual;
		// Ordered by LESS (spec §3.6, §5.2), the types of both sides being one:
		// `<` does not wrap a T into T?.
		if (ordering && p_left == p_right && (p_left.kind == TypeKind::Struct || p_left.kind == TypeKind::Option || (is_typed_container(p_left) && p_left.name == "Array"))) {
			if (!has_less(p_left)) {
				error(p_at, "Values of type " + p_left.to_string() + " are not ordered: a struct is when all its fields are, T? and Array[T] when T is (spec §5.2).");
				return Type::of(TypeKind::Error);
			}
			return Type::of(TypeKind::Bool);
		}
		// The side with fewer levels of T? is wrapped (spec §5.2).
		if ((p_op == TokenKind::EqualEqual || p_op == TokenKind::BangEqual) && (p_left.kind == TypeKind::Option || p_right.kind == TypeKind::Option)) {
			const bool left_deeper = option_depth(p_left) >= option_depth(p_right);
			if (wraps_to(left_deeper ? p_right : p_left, left_deeper ? p_left : p_right)) {
				return Type::of(TypeKind::Bool);
			}
		}
		if (p_left.kind == TypeKind::Option || p_right.kind == TypeKind::Option) {
			const Type &optional = p_left.kind == TypeKind::Option ? p_left : p_right;
			error(p_at, "The value may be None (" + optional.to_string() + "); unwrap it first.");
			return Type::of(TypeKind::Error);
		}
		if (p_op == TokenKind::And || p_op == TokenKind::Or) {
			if (p_left.kind == TypeKind::Bool && p_right.kind == TypeKind::Bool) {
				return p_left;
			}
			error(p_at, std::string(token_name(p_op)) + " requires bool operands, found " + p_left.to_string() + " and " + p_right.to_string() + ".");
			return Type::of(TypeKind::Error);
		}
		if (p_left.kind == TypeKind::Enum || p_right.kind == TypeKind::Enum) {
			return binary_type(p_op, as_int(p_left), as_int(p_right), p_at);
		}
		if (p_left.is_primitive() && p_right.is_primitive()) {
			return primitive_binary_type(p_op, p_left, p_right, p_at);
		}
		const bool equality = p_op == TokenKind::EqualEqual || p_op == TokenKind::BangEqual;
		// Structs compare field by field (spec §3.6).
		if ((p_left.kind == TypeKind::Struct || p_right.kind == TypeKind::Struct) && equality && p_left == p_right) {
			return Type::of(TypeKind::Bool);
		}
		// Other value types follow the engine's operator table.
		const std::string left = p_left.builtin_name();
		const std::string right = p_right.builtin_name();
		// The sum of two arrays typed alike is typed so too, in the engine and in
		// GDScript's static checks (core/variant/variant_op.h:665-667,
		// modules/gdscript/gdscript_analyzer.cpp:6301-6307).
		if (p_op == TokenKind::Plus && left == "Array" && right == "Array" && !p_left.args.empty() && p_left.args == p_right.args) {
			return p_left;
		}
		if (equality && left == "Array" && right == "Array" && !p_left.args.empty() && !p_right.args.empty() && p_left.args[0] != p_right.args[0]) {
			// The engine compares elements only of the same type (spec §5.2).
			error(p_at, p_left.to_string() + " and " + p_right.to_string() + " are equal only when both are empty, so comparing them is an error.");
			return Type::of(TypeKind::Error);
		}
		const builtin::Class *type = left.empty() ? nullptr : builtin::find_class(left);
		const std::string name = operator_name(p_op);
		for (uint32_t i = 0; type && !right.empty() && i < type->operator_count; i++) {
			const builtin::Operator &op = type->operators[i];
			if (name == op.name && right == op.right_type) {
				return api_type(op.return_type, false);
			}
		}
		error(p_at, std::string("Invalid operands ") + p_left.to_string() + " and " + p_right.to_string() + " for " + token_name(p_op) + ".");
		return Type::of(TypeKind::Error);
	}

	Type primitive_binary_type(TokenKind p_op, const Type &p_left, const Type &p_right, const TokenRange &p_at) {
		const bool numeric = p_left.is_numeric() && p_right.is_numeric();
		const bool both_int = p_left.kind == TypeKind::Int && p_right.kind == TypeKind::Int;
		const Type number = Type::of(both_int ? TypeKind::Int : TypeKind::Float);
		switch (p_op) {
			case TokenKind::Plus:
				if (p_left.kind == TypeKind::String && p_right.kind == TypeKind::String) {
					return p_left;
				}
				[[fallthrough]];
			case TokenKind::Minus:
			case TokenKind::Star:
			case TokenKind::StarStar:
			case TokenKind::Slash:
				if (numeric) {
					return number;
				}
				break;
			case TokenKind::Percent:
				if (both_int) {
					return number;
				}
				if (numeric) {
					error(p_at, "\"%\" requires int operands; use fmod() for float.");
					return Type::of(TypeKind::Error);
				}
				break;
			case TokenKind::ShiftLeft:
			case TokenKind::ShiftRight:
			case TokenKind::Ampersand:
			case TokenKind::Pipe:
			case TokenKind::Caret:
				if (both_int) {
					return number;
				}
				break;
			case TokenKind::Less:
			case TokenKind::LessEqual:
			case TokenKind::Greater:
			case TokenKind::GreaterEqual:
				if (numeric) {
					return Type::of(TypeKind::Bool);
				}
				break;
			case TokenKind::EqualEqual:
			case TokenKind::BangEqual:
				if (numeric || (p_left == p_right && (p_left.kind == TypeKind::Bool || p_left.kind == TypeKind::String))) {
					return Type::of(TypeKind::Bool);
				}
				error(p_at, "Cannot compare " + p_left.to_string() + " with " + p_right.to_string() + ".");
				return Type::of(TypeKind::Error);
			default:
				break;
		}
		error(p_at, std::string("Invalid operands ") + p_left.to_string() + " and " + p_right.to_string() + " for " + token_name(p_op) + ".");
		return Type::of(TypeKind::Error);
	}

	Type check_binary(Expr &r_expr) {
		const bool equality = r_expr.op == TokenKind::EqualEqual || r_expr.op == TokenKind::BangEqual;
		if (equality && (r_expr.left->kind == ExprKind::NoneLiteral || r_expr.right->kind == ExprKind::NoneLiteral)) {
			return check_none_comparison(r_expr);
		}
		if (r_expr.op == TokenKind::In) {
			return check_in(r_expr);
		}
		const Type left = check_value(*r_expr.left);
		Type right;
		if (r_expr.op == TokenKind::And || r_expr.op == TokenKind::Or) {
			// The right side runs only when the left is true (and) or false
			// (or), and narrows accordingly (spec §5.4).
			const Facts facts = facts_of(*r_expr.left);
			narrowed.push_back(r_expr.op == TokenKind::And ? facts.when_true : facts.when_false);
			right = check_value(*r_expr.right);
			narrowed.pop_back();
		} else {
			right = check_value(*r_expr.right);
		}
		return set(r_expr, binary_type(r_expr.op, left, right, r_expr.range));
	}

	// `x == None` and `x != None` (spec §5.2), which narrow x (§5.4).
	Type check_none_comparison(Expr &r_expr) {
		Expr &value = r_expr.left->kind == ExprKind::NoneLiteral ? *r_expr.right : *r_expr.left;
		const Type type = check_value(value);
		set(r_expr.left->kind == ExprKind::NoneLiteral ? *r_expr.left : *r_expr.right, type);
		if (type.is_error()) {
			return set(r_expr, type);
		}
		if (!type.is_optional() && type.kind != TypeKind::Variant) {
			const std::string hint = type.kind == TypeKind::Object ? " Check whether it is alive with is_instance_valid(x), or make its type " + type.to_string() + "?." : "";
			return fail(r_expr, "A value of type " + type.to_string() + " is never None." + hint);
		}
		r_expr.target = Target::NoneComparison;
		return set(r_expr, Type::of(TypeKind::Bool));
	}

	// `x in c` (spec §5.1): c.has(x) of an Array, a Dictionary or a packed
	// array, c.contains(x) of a String or StringName, so x takes the type
	// those take; or, on an object, whether the engine finds a member named x.
	Type check_in(Expr &r_expr) {
		const Type container = check_value(*r_expr.right);
		if (container.is_error()) {
			check_value(*r_expr.left);
			return set(r_expr, Type::of(TypeKind::Error));
		}
		if (container.kind == TypeKind::Object) {
			const Type name = check_value(*r_expr.left);
			if (container.nullable) {
				return fail(r_expr, "The value may be None (" + container.to_string() + "); unwrap it first.");
			}
			if (!name.is_error() && name.kind != TypeKind::String && name != Type::builtin("StringName")) {
				error(r_expr.left->range, "\"in\" on an object looks for a member by name, a String or StringName, not " + name.to_string() + ".");
				return set(r_expr, Type::of(TypeKind::Error));
			}
			r_expr.target = Target::HasMember;
			return set(r_expr, Type::of(TypeKind::Bool));
		}
		const std::string base = container.kind == TypeKind::String ? std::string("String") : container.builtin_name();
		const bool text = base == "String" || base == "StringName";
		const bool listed = base == "Array" || base == "Dictionary" || base.rfind("Packed", 0) == 0;
		const builtin::Class *type = text || listed ? builtin::find_class(base) : nullptr;
		const builtin::Method *method = type ? builtin::find_method(*type, text ? "contains" : "has") : nullptr;
		if (!method || method->argument_count != 1) {
			const Type value = check_value(*r_expr.left);
			if (container.kind == TypeKind::Option) {
				return fail(r_expr, "The value may be None (" + container.to_string() + "); unwrap it first.");
			}
			return value.is_error() ? set(r_expr, value) : fail(r_expr, "Invalid operands " + value.to_string() + " and " + container.to_string() + " for \"in\".");
		}
		r_expr.engine_params = { api_type(method->arguments[0].type, false) };
		Type element = r_expr.engine_params[0];
		if ((base == "Array" || base == "Dictionary") && !container.args.empty()) {
			element = container.args[0]; // The element or key type (spec §3.8).
		}
		const Type value = check_value(*r_expr.left, element);
		if (!value.is_error() && !assignable(value, element)) {
			std::string message = "\"in\" looks for a value of type " + element.to_string() + " in " + container.to_string() + ", not " + value.to_string() + ".";
			if (value.kind == TypeKind::Float && element.kind == TypeKind::Int) {
				message += " Convert explicitly with int(x).";
			}
			error(r_expr.left->range, message);
			return set(r_expr, Type::of(TypeKind::Error));
		}
		r_expr.target = Target::Contains;
		r_expr.owner = base;
		r_expr.text = method->name;
		r_expr.param_types = { element };
		return set(r_expr, Type::of(TypeKind::Bool));
	}

	// `x is T` (spec §5.4).
	Type check_type_test(Expr &r_expr) {
		const Type value = check_value(*r_expr.left);
		const Type tested = resolve(r_expr.cast_type, false);
		if (value.is_error() || tested.is_error()) {
			return set(r_expr, Type::of(TypeKind::Error));
		}
		if (tested.kind != TypeKind::Object || tested.nullable) {
			return fail(r_expr, "is tests classes, not " + tested.to_string() + ".");
		}
		if (value.kind != TypeKind::Object) {
			return fail(r_expr, "is tests objects, not " + value.to_string() + ".");
		}
		if (!related(value, tested)) {
			return fail(r_expr, value.name + " and " + tested.name + " are unrelated classes; is would always be false.");
		}
		r_expr.tested = tested;
		return set(r_expr, Type::of(TypeKind::Bool));
	}

	/* Narrowing (spec §5.4) */

	// What a condition says about locals and parameters when it is true, and
	// when it is false.
	struct Facts {
		std::unordered_map<int, Type> when_true;
		std::unordered_map<int, Type> when_false;
	};

	// The local a condition tests, or -1.
	static int tested_local(const Expr &p_expr) {
		return p_expr.kind == ExprKind::Name && p_expr.target == Target::Local ? p_expr.index : -1;
	}

	Facts facts_of(const Expr &p_condition) const {
		Facts facts;
		if (p_condition.type.is_error()) {
			return facts;
		}
		switch (p_condition.kind) {
			case ExprKind::Binary:
				if (p_condition.target == Target::NoneComparison) {
					const Expr &value = p_condition.left->kind == ExprKind::NoneLiteral ? *p_condition.right : *p_condition.left;
					const int local = tested_local(value);
					if (local >= 0) {
						(p_condition.op == TokenKind::BangEqual ? facts.when_true : facts.when_false)[local] = value.type.unwrapped();
					}
				} else if (p_condition.op == TokenKind::And) {
					facts.when_true = facts_of(*p_condition.left).when_true;
					for (const auto &fact : facts_of(*p_condition.right).when_true) {
						facts.when_true[fact.first] = fact.second;
					}
				} else if (p_condition.op == TokenKind::Or) {
					facts.when_false = facts_of(*p_condition.left).when_false;
					for (const auto &fact : facts_of(*p_condition.right).when_false) {
						facts.when_false[fact.first] = fact.second;
					}
				}
				break;
			case ExprKind::Unary:
				if (p_condition.op == TokenKind::Not) {
					const Facts inner = facts_of(*p_condition.left);
					facts.when_true = inner.when_false;
					facts.when_false = inner.when_true;
				}
				break;
			case ExprKind::TypeTest: {
				const int local = tested_local(*p_condition.left);
				if (local >= 0) {
					(p_condition.bool_value ? facts.when_false : facts.when_true)[local] = p_condition.tested;
				}
				break;
			}
			default:
				break;
		}
		return facts;
	}

	// The narrowed type of a local, if any; the innermost fact wins.
	static const Type *narrowed_in(const std::vector<std::unordered_map<int, Type>> &p_narrowed, int p_local) {
		for (auto map = p_narrowed.rbegin(); map != p_narrowed.rend(); ++map) {
			const auto found = map->find(p_local);
			if (found != map->end()) {
				return &found->second;
			}
		}
		return nullptr;
	}

	const Type *narrowed_type(int p_local) const {
		return narrowed_in(narrowed, p_local);
	}

	// An assignment ends what was known about the local, also in the
	// enclosing blocks.
	void forget(int p_local) {
		for (auto &map : narrowed) {
			map.erase(p_local);
		}
	}

	void add_facts(const std::unordered_map<int, Type> &p_facts) {
		for (const auto &fact : p_facts) {
			narrowed.back()[fact.first] = fact.second;
		}
	}

	// Before a loop: what its body assigns is not known on the next iteration.
	void forget_assigned(const Block &p_block) {
		for (const auto &stmt : p_block) {
			if (stmt->kind == StmtKind::Assign && stmt->target->kind == ExprKind::Name) {
				const int local = find_local(stmt->target->text);
				if (local >= 0) {
					forget(local);
				}
			}
			forget_assigned(stmt->body);
			forget_assigned(stmt->else_body);
			for (const MatchBranch &branch : stmt->branches) {
				forget_assigned(branch.body);
			}
		}
	}

	// Ends on every path with return, or, when `p_or_jump`, also with break or
	// continue, so that it leaves the block.
	static bool always_ends(const Block &p_block, bool p_or_jump) {
		if (p_block.empty()) {
			return false;
		}
		const Stmt &last = *p_block.back();
		if (last.kind == StmtKind::Return || (p_or_jump && (last.kind == StmtKind::Break || last.kind == StmtKind::Continue))) {
			return true;
		}
		if (last.kind == StmtKind::Match) {
			// Some branch takes every value, and every branch leaves.
			bool total = false;
			for (const MatchBranch &branch : last.branches) {
				for (const Pattern &pattern : branch.patterns) {
					total = total || (!branch.guard && pattern.kind != PatternKind::Value);
				}
				if (!always_ends(branch.body, p_or_jump)) {
					return false;
				}
			}
			return total;
		}
		return last.kind == StmtKind::If && always_ends(last.body, p_or_jump) && always_ends(last.else_body, p_or_jump);
	}

	// Checks the arguments of a call against parameter types.
	void check_arguments(Expr &r_call, const std::vector<Type> &p_params, int p_required, bool p_vararg) {
		const int count = int(r_call.args.size());
		const int max = int(p_params.size());
		if (count < p_required || (!p_vararg && count > max)) {
			std::string expected = std::to_string(p_required);
			if (p_vararg) {
				expected = "at least " + expected;
			} else if (p_required != max) {
				expected += " to " + std::to_string(max);
			}
			error(r_call.range, "Function \"" + r_call.text + "()\" takes " + expected + " argument(s), " + std::to_string(count) + " given.");
		}
		for (int i = 0; i < count; i++) {
			Expr &arg = *r_call.args[i];
			const Type param = i < max ? p_params[i] : Type::of(TypeKind::Variant);
			const Type arg_type = check_value(arg, param);
			r_call.param_types.push_back(param);
			expect_assignable(arg, arg_type, param, "pass");
		}
	}

	std::vector<Type> engine_params(const EngineMethod &p_method, Expr &r_call, bool &r_ok) {
		std::vector<Type> params;
		r_ok = true;
		for (const EngineParam &param : p_method.params) {
			if (!param.type.is_storable()) {
				fail(r_call, "\"" + r_call.text + "()\" takes a parameter of type " + param.type.to_string() + ", which is not supported yet.");
				r_ok = false;
				return params;
			}
			Type type = param.type;
			// A parameter the engine requires to be an object (spec §10.2).
			if (type.kind == TypeKind::Object && builtin::param_non_null(p_method.owner, p_method.name, uint32_t(params.size()))) {
				type.nullable = false;
			}
			params.push_back(type);
		}
		return params;
	}

	// What the engine returns: T? unless the binding metadata says it is
	// null only when misused (spec §10.2), which a check at run time reports.
	static Type engine_result(const EngineMethod &p_method) {
		Type type = p_method.return_type;
		if (type.kind == TypeKind::Object && builtin::returns_non_null(p_method.owner, p_method.name)) {
			type.nullable = false;
		}
		return type;
	}

	// The property's type, and the range its setter takes (spec §10.6); Error
	// after reporting a type this language cannot hold yet.
	Type property_type(const EngineProperty &p_property, Expr &r_expr) {
		if (!p_property.type.is_storable()) {
			return fail(r_expr, "The property \"" + r_expr.text + "\" has type " + p_property.type.to_string() + ", which is not supported yet.");
		}
		Type type = p_property.type;
		if (type.kind == TypeKind::Object && builtin::property_non_null(p_property.owner, p_property.name)) {
			type.nullable = false;
		}
		if (type.kind == TypeKind::Int) {
			r_expr.int_range = builtin::property_int_range(p_property.owner, p_property.name);
		}
		return type;
	}

	// A method of an engine class, or with `p_static_class` a static one called
	// through that class's name, as FileAccess.open(). A static one called on
	// an object or on self is its class's (spec §4.8), but that of a GDScript
	// class is found by name on the object, as GDScript's instance finds it
	// (modules/gdscript/gdscript.cpp:1954-1958).
	Type check_engine_call(Expr &r_expr, const EngineMethod &p_method, std::string p_static_class = std::string()) {
		if (p_method.is_static && p_static_class.empty() && !r_expr.is_foreign) {
			p_static_class = p_method.owner;
			r_expr.static_on_value = r_expr.base != nullptr;
		}
		if (!p_method.is_static && !p_static_class.empty()) {
			return fail_call(r_expr, "\"" + r_expr.text + "()\" of " + p_method.owner + " is not static; call it on an object.");
		}
		if (p_method.is_virtual) {
			return fail_call(r_expr, "\"" + r_expr.text + "()\" is a virtual engine method.");
		}
		if (p_method.return_type.kind == TypeKind::Unsupported) {
			return fail_call(r_expr, "\"" + r_expr.text + "()\" returns " + p_method.return_type.to_string() + ", which is not supported yet.");
		}
		bool ok = false;
		const std::vector<Type> params = engine_params(p_method, r_expr, ok);
		if (!ok) {
			return fail_call(r_expr);
		}
		check_arguments(r_expr, params, int(params.size()) - p_method.default_count, p_method.is_vararg);
		for (size_t i = 0; i < params.size(); i++) {
			r_expr.arg_ranges.push_back(params[i].kind == TypeKind::Int ? builtin::param_int_range(p_method.owner, p_method.name, uint32_t(i)) : std::nullopt);
		}
		// A method of this class that the engine is to call by a literal name,
		// as in call_deferred("reset"), is one it must find (spec §10.5).
		for (size_t i = 0; !r_expr.base && i < p_method.params.size() && i < r_expr.args.size(); i++) {
			const Expr &arg = *r_expr.args[i];
			const auto named = arg.kind == ExprKind::StringLiteral ? function_index.find(arg.text) : function_index.end();
			if (p_method.params[i].name == "method" && named != function_index.end()) {
				cls.exposed_methods.insert(arg.text);
				const FuncDecl &method = *cls.all_functions[named->second];
				refer_string(arg, script_id(declaring(cls, method, &ClassDecl::functions).path, method.name));
			}
		}
		if (!r_expr.is_foreign) {
			refer(r_expr.name_token, engine_id(p_method.owner, r_expr.text));
		}
		r_expr.target = Target::EngineMethod;
		if (!p_static_class.empty()) {
			r_expr.is_static = true;
			r_expr.owner = p_static_class;
		}
		r_expr.engine_return = engine_result(p_method);
		return set(r_expr, filtered_result(r_expr, p_method));
	}

	// An array that the engine fills with objects of a class named by an
	// argument holds that class, when the name is a literal (spec §10.3).
	Type filtered_result(const Expr &p_call, const EngineMethod &p_method) {
		const Type &result = p_call.engine_return;
		const int index = builtin::class_filter_param(p_method.owner, p_method.name);
		if (index < 0 || index >= int(p_call.args.size()) || p_call.args[index]->kind != ExprKind::StringLiteral || result.args.empty()) {
			return result;
		}
		const Expr &arg = *p_call.args[index];
		const std::string &name = arg.text;
		if (name.empty() || name.rfind("res://", 0) == 0) {
			return result; // No filter, or a script by its path.
		}
		Type element;
		const std::string path = options.project ? options.project->global_class_path(name) : std::string();
		if (engine.class_exists(name)) {
			element = Type::object(name);
			refer_string(arg, engine_id(name));
		} else if (is_script_path(path)) {
			refer_string(arg, script_id(path));
			TypeRef ref;
			ref.name = name;
			ref.range = arg.range;
			ref.name_token = NO_TOKEN;
			const std::optional<Type> script = script_type(ref);
			if (!script || script->is_error()) {
				return Type::of(TypeKind::Error);
			}
			element = *script;
		} else if (!path.empty()) {
			return result; // A class of another language.
		} else {
			error(arg.range, p_method.name + "() filters by the class \"" + name + "\", which does not exist.");
			return Type::of(TypeKind::Error);
		}
		if (!is_a(element, result.args[0])) {
			error(arg.range, p_method.name + "() returns objects of " + result.args[0].to_string() + ", and " + element.to_string() + " is not one.");
			return Type::of(TypeKind::Error);
		}
		return Type::array(element);
	}

	// The parameters of a function of the engine's API, and in `r_required`
	// how many a call must pass; nullopt after reporting one this language
	// cannot pass yet.
	std::optional<std::vector<Type>> api_params(Expr &r_call, const builtin::Argument *p_args, uint32_t p_count, int &r_required) {
		std::vector<Type> params;
		r_required = 0;
		for (uint32_t i = 0; i < p_count; i++) {
			const Type param = api_type(p_args[i].type, true);
			if (param.kind == TypeKind::Object || !param.is_storable()) {
				fail_call(r_call, "\"" + r_call.text + "()\" takes " + param.to_string() + ", which is not supported yet.");
				return std::nullopt;
			}
			params.push_back(param);
			r_required += p_args[i].has_default ? 0 : 1;
		}
		return params;
	}

	// Whether this language can hold what a function of the engine's API
	// returns; false after reporting it.
	bool api_result_supported(Expr &r_call, const Type &p_result) {
		if (p_result.kind == TypeKind::Object || p_result.kind == TypeKind::Unsupported) {
			fail(r_call, "\"" + r_call.text + "()\" returns " + p_result.to_string() + ", which is not supported yet.");
			return false;
		}
		return true;
	}

	Type check_utility(Expr &r_expr, const builtin::Utility &p_utility) {
		r_expr.target = Target::Utility;
		refer(r_expr.name_token, engine_id("@GlobalScope", r_expr.text));
		r_expr.hash = p_utility.hash;
		r_expr.is_vararg = p_utility.is_vararg;
		r_expr.engine_return = api_type(p_utility.return_type, true);
		const ArgumentTyped rule = argument_typed(r_expr.text);
		if (rule == ArgumentTyped::None) {
			int required = 0;
			const std::optional<std::vector<Type>> params = api_params(r_expr, p_utility.arguments, p_utility.argument_count, required);
			if (!params) {
				return r_expr.type;
			}
			if (!api_result_supported(r_expr, r_expr.engine_return)) {
				return r_expr.type;
			}
			check_arguments(r_expr, *params, required, p_utility.is_vararg);
			return set(r_expr, r_expr.engine_return);
		}

		// Typed by the arguments (spec §10.3); the engine takes them as Variant.
		const size_t expected = rule == ArgumentTyped::Lerp ? 3 : p_utility.argument_count;
		if (r_expr.args.size() < expected || (!p_utility.is_vararg && r_expr.args.size() > expected)) {
			return fail_call(r_expr, "Function \"" + r_expr.text + "()\" takes " + (p_utility.is_vararg ? "at least " : "") + std::to_string(expected) + " argument(s).");
		}
		std::vector<Type> types;
		for (auto &arg : r_expr.args) {
			types.push_back(check_value(*arg));
			r_expr.param_types.push_back(Type::of(TypeKind::Variant));
		}
		for (const Type &type : types) {
			if (type.is_error()) {
				return set(r_expr, type);
			}
		}
		const Type result = types[0];
		const size_t same_count = rule == ArgumentTyped::Lerp ? 2 : types.size();
		for (size_t i = 1; i < same_count; i++) {
			if (types[i] != result) {
				std::string message = "The arguments of " + r_expr.text + "() must have the same type, found " + result.to_string() + " and " + types[i].to_string() + ".";
				if (types[i].is_numeric() && result.is_numeric()) {
					message += " Write the int argument as a float, for example 1.0.";
				}
				return fail(*r_expr.args[i], message);
			}
		}
		if (rule == ArgumentTyped::Lerp) {
			if (!is_interpolable(result)) {
				return fail(r_expr, "lerp() interpolates float, vectors, Color, Quaternion and Basis, not " + result.to_string() + ".");
			}
			if (!types[2].is_numeric()) {
				return fail(*r_expr.args[2], "The weight of lerp() must be a number.");
			}
			// An int weight converts to float (spec §3.2) before it goes to the engine.
			r_expr.param_types[2] = Type::of(TypeKind::Float);
		} else if (!is_numeric_or_vector(result)) {
			return fail(r_expr, r_expr.text + "() takes numbers or vectors, not " + result.to_string() + ".");
		}
		return set(r_expr, result);
	}

	Type check_constructor(Expr &r_expr, const builtin::Class &p_type) {
		refer(r_expr.name_token, engine_id(p_type.name));
		std::vector<Type> args;
		for (auto &arg : r_expr.args) {
			args.push_back(check_value(*arg));
		}
		for (const Type &type : args) {
			if (type.is_error()) {
				return set(r_expr, type);
			}
		}
		const std::string name = p_type.name;
		if (args.size() == 1 && args[0].name == name && is_typed_container(args[0]) && is_boxed(args[0])) {
			// An untyped copy with the elements encoded (spec §3.8, §10.1).
			r_expr.param_types.push_back(args[0]);
			r_expr.target = Target::Encode;
			return set(r_expr, Type::builtin(name));
		}
		// The first constructor that takes these types exactly; otherwise the
		// first that takes them with conversions.
		int chosen = -1;
		for (uint32_t i = 0; i < p_type.constructor_count; i++) {
			const builtin::Constructor &constructor = p_type.constructors[i];
			if (constructor.argument_count != args.size()) {
				continue;
			}
			bool viable = true;
			bool exact = true;
			for (uint32_t a = 0; a < constructor.argument_count; a++) {
				const Type param = api_type(constructor.arguments[a].type, false);
				viable = viable && param.kind != TypeKind::Object && assignable(args[a], param);
				exact = exact && param == args[a];
			}
			if (viable && exact) {
				chosen = int(i);
				break;
			}
			if (viable && chosen < 0) {
				chosen = int(i);
			}
		}
		if (chosen < 0) {
			std::string types;
			for (const Type &type : args) {
				types += (types.empty() ? "" : ", ") + type.to_string();
			}
			return fail(r_expr, "No constructor of " + std::string(p_type.name) + " takes (" + types + ").");
		}
		const builtin::Constructor &constructor = p_type.constructors[chosen];
		for (uint32_t a = 0; a < constructor.argument_count; a++) {
			const Type param = api_type(constructor.arguments[a].type, false);
			r_expr.param_types.push_back(param);
			// Chosen by its own type, a string literal is built as the parameter's.
			Expr &arg = *r_expr.args[a];
			if (arg.kind == ExprKind::StringLiteral && literal_type(arg, param) != arg.type) {
				arg.constant = nullptr;
				check_expr(arg, param);
			}
		}
		r_expr.target = Target::BuiltinConstructor;
		r_expr.index = chosen;
		r_expr.owner = p_type.name;
		return set(r_expr, api_type(p_type.name, false));
	}

	// The engine binds Array's methods with Variant; for Array[T], the values
	// are elements of type T (spec §3.8). pop_front() and pop_back() give T?,
	// since the engine returns null on an empty array
	// (core/variant/array.cpp:781-800); pop_at() gives T, an index out of
	// bounds being a program error as for a[i].
	static void element_typed(const std::string &p_method, const Type &p_array, std::vector<Type> &r_params, Type &r_result) {
		const Type &element = p_array.args[0];
		if (p_method == "reduce") {
			return; // The accumulator is not an element.
		}
		for (Type &param : r_params) {
			if (param.kind == TypeKind::Variant) {
				param = element;
			} else if (param == Type::builtin("Array") && (p_method == "append_array" || p_method == "assign")) {
				param = p_array;
			}
		}
		if (r_result.kind == TypeKind::Variant) {
			const bool pops = p_method == "pop_front" || p_method == "pop_back";
			r_result = pops ? optional_of(element) : element;
		} else if (r_result == Type::builtin("Array") && (p_method == "duplicate" || p_method == "slice" || p_method == "filter")) {
			r_result = p_array;
		}
	}

	// Dictionary's methods take and give keys and values as Variant; for
	// Dictionary[K, V] they are K and V (spec §3.8). find_key() gives K?, the
	// engine returning null when no key has the value
	// (core/variant/dictionary.cpp:237-246).
	static void entry_typed(const builtin::Method &p_method, const Type &p_dictionary, std::vector<Type> &r_params, Type &r_result) {
		const Type &key = p_dictionary.args[0];
		const Type &value = p_dictionary.args[1];
		for (size_t i = 0; i < r_params.size(); i++) {
			const std::string name = p_method.arguments[i].name;
			if (name == "key") {
				r_params[i] = key;
			} else if (name == "value" || name == "default") {
				r_params[i] = value;
			} else if (name == "keys") {
				r_params[i] = Type::array(key);
			} else if (name == "dictionary") {
				r_params[i] = p_dictionary;
			}
		}
		const std::string method = p_method.name;
		if (r_result.kind == TypeKind::Variant) {
			r_result = method == "find_key" ? optional_of(key) : value;
		} else if (r_result == Type::builtin("Array") && (method == "keys" || method == "values")) {
			// The engine gives typed arrays (core/variant/dictionary.cpp:386-425).
			r_result = Type::array(method == "keys" ? key : value);
		} else if (r_result == Type::builtin("Dictionary")) {
			r_result = p_dictionary;
		}
	}

	// Whether values of the type have `<` (spec §5.2): those the engine's
	// operator table orders (core/variant/variant_op.cpp:731-791), and Variant,
	// ordered when the program runs; a struct when all its fields have it, T?
	// and Array[T] when T has it.
	bool has_less(const Type &p_type) const {
		static const std::unordered_set<std::string> ordered = { "StringName", "Vector2", "Vector2i", "Vector3", "Vector3i", "Vector4", "Vector4i", "RID" };
		switch (p_type.kind) {
			case TypeKind::Int:
			case TypeKind::Float:
			case TypeKind::Bool:
			case TypeKind::String:
			case TypeKind::Variant:
				return true;
			case TypeKind::Option:
				return has_less(p_type.args[0]);
			case TypeKind::Struct: {
				const StructDecl *decl = struct_decl(p_type);
				return !decl || std::all_of(decl->fields.begin(), decl->fields.end(), [&](const VarDecl &p_field) { return has_less(p_field.type); });
			}
			case TypeKind::Builtin:
				if (p_type.name == "Array") {
					return p_type.args.empty() || has_less(p_type.args[0]);
				}
				return ordered.count(p_type.name) > 0;
			default:
				return false;
		}
	}

	static int option_depth(const Type &p_type) {
		return p_type.kind == TypeKind::Option ? 1 + option_depth(p_type.args[0]) : 0;
	}

	// Whether a value of `p_from` becomes a `p_to` when wrapped in Some as
	// many times as needed (spec §5.2).
	bool wraps_to(const Type &p_from, const Type &p_to) const {
		if (p_from == p_to || (p_from.kind != TypeKind::Option && p_to.kind != TypeKind::Option && assignable(p_from, p_to))) {
			return true;
		}
		return p_to.kind == TypeKind::Option && wraps_to(p_from, p_to.args[0]);
	}

	// Methods that order a container's elements, or a Dictionary's keys, need
	// `<` on them, and resize() a default value for the elements (spec §3.8).
	bool check_container_method(const Expr &p_call, const Type &p_base) {
		const std::string &method = p_call.text;
		const bool array = p_base.name == "Array" && !p_base.args.empty();
		const bool orders = array ? (method == "sort" || method == "bsearch" || method == "min" || method == "max") : (p_base.name == "Dictionary" && method == "sort");
		if (orders) {
			const Type &ordered = p_base.args.empty() ? Type::of(TypeKind::Variant) : p_base.args[0];
			if (!has_less(ordered)) {
				error(p_call.range, "\"" + method + "()\" orders the " + (array ? "elements" : "keys") + " by <, which " + ordered.to_string() + " does not have (spec §3.8); use " + (array ? "sort_custom() or bsearch_custom() with a comparison" : "a sorted Array of its keys") + ".");
				return false;
			}
		}
		const Type &element = array ? p_base.args[0] : Type::of(TypeKind::Error);
		if (method == "resize" && ((element.kind == TypeKind::Object && !element.nullable) || (element.kind == TypeKind::Struct && !has_default(element)) || !enum_has_default(element))) {
			const std::string fix = element.kind == TypeKind::Struct ? "give every field of " + element.to_string() + " a default value, or " : "";
			error(p_call.range, "\"resize()\" fills new elements with a default value, which " + element.to_string() + " does not have (spec §3.8); " + fix + "declare the array as Array[" + element.to_string() + "?].");
			return false;
		}
		return true;
	}

	// A method of a value of a built-in type, or with `p_static` a static one
	// called through the type's name, as Color.from_hsv().
	Type check_builtin_method(Expr &r_expr, const Type &p_base, bool p_static = false) {
		const std::string base_name = p_base.builtin_name();
		const builtin::Class *type = builtin::find_class(base_name);
		const builtin::Method *method = type ? builtin::find_method(*type, r_expr.text) : nullptr;
		if (!method) {
			return fail_call(r_expr, p_base.to_string() + " has no method \"" + r_expr.text + "()\".");
		}
		// On a value, the type's own (spec §4.8).
		r_expr.static_on_value = method->is_static && !p_static;
		if (!method->is_static && p_static) {
			return fail_call(r_expr, "\"" + r_expr.text + "()\" of " + base_name + " is not static; call it on a value.");
		}
		const bool typed_dictionary = base_name == "Dictionary" && !p_base.args.empty();
		// get(k) of Dictionary[K, V] gives V?, None for a missing key (spec §3.8).
		const bool get_or_none = typed_dictionary && r_expr.text == "get" && r_expr.args.size() == 1;
		int required = 0;
		std::optional<std::vector<Type>> api = api_params(r_expr, method->arguments, method->argument_count, required);
		if (!api) {
			return r_expr.type;
		}
		std::vector<Type> params = std::move(*api);
		if (get_or_none) {
			params.resize(1);
		}
		// The engine's default values are source, such as `true` or
		// `Vector3(0, 1, 0)`, checked and passed as if written at the call.
		for (size_t i = r_expr.args.size(); int(r_expr.args.size()) >= required && i < params.size(); i++) {
			std::vector<Diagnostic> ignored;
			std::unique_ptr<Expr> value = parse_expression(method->arguments[i].default_value, ignored);
			if (!value) {
				return fail(r_expr, "The default value of \"" + std::string(method->arguments[i].name) + "\" of \"" + r_expr.text + "()\" is not supported yet; pass it.");
			}
			relocate(*value, r_expr.range);
			r_expr.args.push_back(std::move(value));
		}
		Type result = api_type(method->return_type, true);
		if (!api_result_supported(r_expr, result)) {
			return r_expr.type;
		}
		r_expr.engine_params = params;
		r_expr.engine_return = result;
		if (base_name == "Array" && !p_base.args.empty()) {
			// With T?, pop_front() and pop_back() give T??, which tells an empty
			// array from a None element (spec §3.8).
			const bool pops = r_expr.text == "pop_front" || r_expr.text == "pop_back";
			if (pops && p_base.args[0].is_optional()) {
				return fail(r_expr, "\"" + r_expr.text + "()\" of " + p_base.to_string() + " gives " + p_base.args[0].to_string() + "?, which is not supported yet.");
			}
			element_typed(r_expr.text, p_base, params, result);
		}
		if (typed_dictionary) {
			entry_typed(*method, p_base, params, result);
			if (get_or_none) {
				result = optional_of(p_base.args[1]);
			}
		}
		if (!check_container_method(r_expr, p_base)) {
			return set(r_expr, Type::of(TypeKind::Error));
		}
		// A method that changes a value changes it where it came from, which
		// must be a place that can be written; Array and Dictionary are shared
		// and change in place.
		if (!method->is_const && !method->is_static && !is_shared(p_base)) {
			const Expr &base = *r_expr.base;
			const bool place = base.target == Target::Local || base.target == Target::Member || base.target == Target::SelfProperty ||
					base.target == Target::ObjectProperty || base.target == Target::Field || base.target == Target::StructField;
			if (!place) {
				return fail(r_expr, "\"" + r_expr.text + "()\" changes the " + base_name + " it is called on, which is a temporary value here.");
			}
			if (!check_place(base)) {
				return set(r_expr, Type::of(TypeKind::Error));
			}
		}
		check_arguments(r_expr, params, int(params.size()), method->is_vararg);
		r_expr.is_vararg = method->is_vararg;
		r_expr.target = Target::BuiltinMethod;
		r_expr.owner = base_name;
		refer(r_expr.name_token, engine_id(base_name, r_expr.text));
		r_expr.hash = method->hash;
		r_expr.is_const = method->is_const;
		r_expr.is_static = method->is_static;
		return set(r_expr, result);
	}

	// `f.bind(a, b)` drops the last two parameters of f, which the engine
	// passes after the caller's arguments (spec §3.11,
	// core/variant/callable_bind.cpp:141-150).
	Type check_bind(Expr &r_expr, const Type &p_function) {
		const size_t param_count = p_function.args.size() - 1;
		const size_t bound = r_expr.args.size();
		if (bound > param_count) {
			return fail_call(r_expr, "bind() binds " + std::to_string(bound) + " argument(s), but " + p_function.to_string() + " takes " + std::to_string(param_count) + ".");
		}
		const std::vector<Type> rest(p_function.args.begin() + 1, p_function.args.end() - bound);
		const std::vector<Type> bound_params(p_function.args.end() - bound, p_function.args.end());
		check_arguments(r_expr, bound_params, int(bound), false);
		r_expr.target = Target::BindArguments;
		refer(r_expr.name_token, engine_id("Callable", "bind"));
		Type type = Type::function(p_function.args[0], rest);
		type.is_async = p_function.is_async;
		return set(r_expr, type);
	}

	// `f.call(x)`, checked against f's type, and `f.callv(args)`, whose
	// arguments the function checks as it is called (spec §3.11). A program
	// error in it aborts the caller too (§8.1).
	Type check_function_call(Expr &r_expr, const Type &p_function) {
		if (p_function.is_async) {
			return fail_call(r_expr, "Calling an async function value is not supported yet.");
		}
		const bool spread = r_expr.text == "callv";
		const std::vector<Type> params(p_function.args.begin() + 1, p_function.args.end());
		check_arguments(r_expr, spread ? std::vector<Type>{ Type::builtin("Array") } : params, spread ? 1 : int(params.size()), false);
		r_expr.target = Target::CallValue;
		refer(r_expr.name_token, engine_id("Callable", r_expr.text));
		return set(r_expr, p_function.args[0]);
	}

	// `func(x): body` (spec §3.11): code of the function around it, checked
	// as a function of its own. Its parameters take the types of the function
	// type it goes to, unless written, and its return type is what it returns
	// (§4.6). Checked once, also when a call checks its arguments again.
	Type check_lambda(Expr &r_expr, const Type &p_expected) {
		FuncDecl &lambda = *r_expr.function;
		if (lambda.checked) {
			return r_expr.type;
		}
		lambda.checked = true;
		if (lambda.is_async) {
			return fail(r_expr, "Async lambdas are not supported yet.");
		}
		if (in_constant) {
			return fail(r_expr, "A lambda is not a constant expression (spec §4.5).");
		}
		lambda.is_static = function && function->is_static;
		const bool typed_by_expected = p_expected.kind == TypeKind::Function && p_expected.args.size() == lambda.params.size() + 1;
		for (size_t i = 0; i < lambda.params.size(); i++) {
			ParamDecl &param = lambda.params[i];
			if (param.default_value) {
				error(param.default_value->range, "Lambda parameters have no default values (spec §3.11).");
			}
			if (param.declared_type) {
				param.type = resolve(*param.declared_type, false);
			} else if (typed_by_expected) {
				param.type = p_expected.args[i + 1];
			} else {
				error(param.name_token, "Parameter \"" + param.name + "\" needs a type: nothing the lambda goes to gives one (spec §4.6).");
				param.type = Type::of(TypeKind::Error);
			}
		}
		Returns inferred;
		Returns *const outer_returns = returns;
		if (lambda.return_type_ref) {
			lambda.return_type = resolve(*lambda.return_type_ref, true);
			returns = nullptr;
		} else {
			returns = &inferred;
		}

		enclosing.push_back({ function, std::move(scopes), std::move(narrowed), &r_expr, loop_depth, in_predelete, initializing, initializing_onready });
		function = &lambda;
		scopes = { {} };
		narrowed = { {} };
		loop_depth = 0;
		in_predelete = false;
		initializing = -1;
		declare_params(lambda);
		check_block(lambda.body);
		if (!lambda.return_type_ref) {
			lambda.return_type = inferred.type.value_or(Type::of(TypeKind::Void));
			if (inferred.type && inferred.bare) {
				error(r_expr.range, function_label() + " returns a value on some paths and none on others.");
			}
		}
		if (lambda.return_type.kind != TypeKind::Void && !lambda.return_type.is_error() && !always_ends(lambda.body, false)) {
			error(r_expr.range, "Not all paths of " + (lambda.name.empty() ? std::string("the lambda") : "the lambda \"" + lambda.name + "\"") + " return a value.");
		}
		Enclosing &outer = enclosing.back();
		function = outer.function;
		scopes = std::move(outer.scopes);
		narrowed = std::move(outer.narrowed);
		loop_depth = outer.loop_depth;
		in_predelete = outer.in_predelete;
		initializing = outer.initializing;
		initializing_onready = outer.initializing_onready;
		enclosing.pop_back();
		returns = outer_returns;

		Type type = Type::function(lambda.return_type, param_types(lambda.params));
		return set(r_expr, type);
	}

	// A function connected to a signal takes what the signal passes (spec §4.7).
	void check_handler(const Expr &p_handler, const Type &p_signal) {
		const Type &handler = p_handler.type;
		if (handler.is_error()) {
			return;
		}
		if (handler.kind != TypeKind::Function) {
			error(p_handler.range, "A signal is connected to a function value, such as a method name, whose parameters it can check; found " + handler.to_string() + ".");
			return;
		}
		if (handler.args.size() - 1 != p_signal.args.size()) {
			error(p_handler.range, p_signal.to_string() + " passes " + std::to_string(p_signal.args.size()) + " argument(s), but " + handler.to_string() + " takes " + std::to_string(handler.args.size() - 1) + ".");
			return;
		}
		for (size_t i = 0; i < p_signal.args.size(); i++) {
			if (!assignable(p_signal.args[i], handler.args[i + 1])) {
				error(p_handler.range, p_signal.to_string() + " passes " + p_signal.args[i].to_string() + " as argument " + std::to_string(i + 1) + ", which " + handler.to_string() + " does not take.");
				return;
			}
		}
	}

	// The methods of T? (spec §3.4).
	Type check_option_method(Expr &r_expr, const Type &p_base) {
		const std::string &name = r_expr.text;
		if (name == "unwrap" || name == "expect") {
			std::vector<Type> params;
			if (name == "expect") {
				params.push_back(Type::of(TypeKind::String));
			}
			check_arguments(r_expr, params, int(params.size()), false);
			r_expr.target = Target::Unwrap;
			refer(r_expr.name_token, language_id("Option", name));
			return set(r_expr, p_base.unwrapped());
		}
		static const std::unordered_set<std::string> later = { "unwrap_or", "unwrap_or_else", "map", "and_then", "or_else", "filter", "ok_or", "ok_or_else", "flatten" };
		if (later.count(name)) {
			return fail_call(r_expr, name + "() of " + p_base.to_string() + " is not supported yet.");
		}
		return fail_call(r_expr, "The value may be None (" + p_base.to_string() + "); check it before calling \"" + name + "()\".");
	}

	// The type both branches fit: one of them, or for objects the nearest
	// class both extend; Error if there is none.
	Type common_type(const Type &p_a, const Type &p_b) const {
		if (assignable(p_a, p_b)) {
			return p_b;
		}
		if (assignable(p_b, p_a)) {
			return p_a;
		}
		if (p_a.kind == TypeKind::Object && p_b.kind == TypeKind::Object && p_a.args.empty() && p_b.args.empty() && !p_a.is_interface && !p_b.is_interface) {
			for (std::string path = p_a.script; is_script_path(path);) {
				const ClassDecl *header = script_header(path);
				if (!header) {
					break;
				}
				if (extends_script(p_b.script, path)) {
					Type result = script_class_type(*header, path);
					result.nullable = p_a.nullable || p_b.nullable;
					return result;
				}
				path = classes.base_path(*header, path);
			}
			for (std::string cls = p_a.engine_class(); !cls.empty(); cls = engine.parent_class(cls)) {
				if (engine.inherits(p_b.engine_class(), cls)) {
					return Type::object(cls, p_a.nullable || p_b.nullable);
				}
			}
		}
		return Type::of(TypeKind::Error);
	}

	// `a if c else b` (spec §5.5).
	Type check_conditional(Expr &r_expr, const Type &p_expected) {
		expect_condition(*r_expr.base);
		const Facts facts = facts_of(*r_expr.base);
		narrowed.push_back(facts.when_true);
		const Type a = check_value(*r_expr.left, p_expected);
		narrowed.back() = facts.when_false;
		const Type b = check_value(*r_expr.right, p_expected);
		narrowed.pop_back();
		if (a.is_error() || b.is_error()) {
			return set(r_expr, Type::of(TypeKind::Error));
		}
		const Type result = common_type(a, b);
		if (result.is_error()) {
			return fail(r_expr, "The branches have types " + a.to_string() + " and " + b.to_string() + ", which have no common type.");
		}
		return set(r_expr, result);
	}

	// `a[i]`, or `d[k]` (spec §3.8); `p_written` when it is only assigned to.
	Type check_subscript(Expr &r_expr, bool p_written = false) {
		const Type base = check_value(*r_expr.base);
		const std::optional<std::pair<Type, Type>> entries = base.is_error() ? std::nullopt : entries_of(base);
		const Type index = check_value(*r_expr.right, entries ? entries->first : Type::of(TypeKind::Error));
		if (base.is_error() || index.is_error()) {
			return set(r_expr, Type::of(TypeKind::Error));
		}
		if (base.is_optional()) {
			return fail(r_expr, "The value may be None (" + base.to_string() + "); check it before indexing it.");
		}
		if (entries) {
			if (!assignable(index, entries->first)) {
				return fail(*r_expr.right, "The keys of " + base.to_string() + " are " + entries->first.to_string() + ", not " + index.to_string() + ".");
			}
			r_expr.param_types = { entries->first };
			if (!p_written && refuse_variant_elements(entries->second, r_expr)) {
				return set(r_expr, Type::of(TypeKind::Error));
			}
			return set(r_expr, entries->second);
		}
		const std::optional<Type> element = element_of(base);
		if (!element) {
			return fail(r_expr, "Values of type " + base.to_string() + " cannot be indexed" + (base.kind == TypeKind::String ? " yet." : "."));
		}
		if (index.kind != TypeKind::Int) {
			return fail(*r_expr.right, "An index must be int, found " + index.to_string() + ".");
		}
		if (!p_written && refuse_variant_elements(*element, r_expr)) {
			return set(r_expr, Type::of(TypeKind::Error));
		}
		return set(r_expr, *element);
	}

	// Elements read from an untyped Array or Dictionary, which are Variant,
	// are refused for now; true after reporting it at `p_at`.
	bool refuse_variant_elements(const Type &p_element, const Expr &p_at) {
		if (p_element.kind != TypeKind::Variant) {
			return false;
		}
		error(p_at.range, "The elements of an untyped Array or Dictionary are Variant, which is not supported yet; declare it as Array[T] or Dictionary[K, V].");
		return true;
	}

	// `[a, b]` takes its type from where it goes (spec §4.3).
	Type check_array_literal(Expr &r_expr, const Type &p_expected) {
		const Type expected = p_expected.kind == TypeKind::Option ? p_expected.args[0] : p_expected;
		if (expected.kind != TypeKind::Builtin || expected.name != "Array") {
			if (r_expr.args.empty()) {
				return fail(r_expr, "An empty array needs a declared type, for example var a: Array[int] = [].");
			}
			// Otherwise its elements' common type gives it (spec §4.3).
			const std::optional<Type> element = common_element(r_expr.args, 0, 1, "elements");
			if (!element) {
				return set(r_expr, Type::of(TypeKind::Error));
			}
			r_expr.param_types.assign(r_expr.args.size(), *element);
			return set(r_expr, Type::array(*element));
		}
		const Type element = expected.args.empty() ? Type::of(TypeKind::Variant) : expected.args[0];
		for (auto &arg : r_expr.args) {
			const Type type = check_value(*arg, element);
			r_expr.param_types.push_back(element);
			if (!assignable(type, element)) {
				error(arg->range, "Cannot put a value of type " + type.to_string() + " in " + expected.to_string() + ".");
			}
		}
		return set(r_expr, expected);
	}

	// The common type of the values at `p_first`, `p_first + p_step`… of a
	// literal, which have no type to go to; nullopt after reporting.
	std::optional<Type> common_element(std::vector<std::unique_ptr<Expr>> &r_values, size_t p_first, size_t p_step, const std::string &p_what) {
		std::optional<Type> common;
		bool ok = true;
		for (size_t i = p_first; i < r_values.size(); i += p_step) {
			const Type type = check_value(*r_values[i]);
			if (type.is_error() || !ok) {
				ok = false;
				continue;
			}
			if (type.kind == TypeKind::None) {
				error(r_values[i]->range, "None does not say which T? the " + p_what + " are; declare the type.");
				ok = false;
				continue;
			}
			const Type next = common ? common_type(*common, type) : type;
			if (next.is_error()) {
				error(r_values[i]->range, "The " + p_what + " have types " + common->to_string() + " and " + type.to_string() + ", which have no common type; declare the type.");
				ok = false;
				continue;
			}
			common = next;
		}
		return ok ? common : std::nullopt;
	}

	// `{k: v}` takes its type from where it goes, as `[a, b]` does, or from
	// its keys and values. A key written as a name is a StringName, as in
	// GDScript (modules/gdscript/gdscript_parser.cpp:3330-3336).
	Type check_dictionary_literal(Expr &r_expr, const Type &p_expected) {
		const Type expected = p_expected.kind == TypeKind::Option ? p_expected.args[0] : p_expected;
		const std::optional<std::pair<Type, Type>> entries = entries_of(expected);
		if (!entries && r_expr.args.empty()) {
			return fail_call(r_expr, "An empty dictionary needs a declared type, for example var d: Dictionary[String, int] = {}.");
		}
		if (!entries) {
			const std::optional<Type> key = r_expr.bool_value ? std::optional<Type>(Type::builtin("StringName")) : common_element(r_expr.args, 0, 2, "keys");
			if (r_expr.bool_value) {
				for (size_t i = 0; i < r_expr.args.size(); i += 2) {
					check_value(*r_expr.args[i], *key);
				}
			}
			const std::optional<Type> value = common_element(r_expr.args, 1, 2, "values");
			if (!key || !value) {
				return set(r_expr, Type::of(TypeKind::Error));
			}
			for (size_t i = 0; i + 1 < r_expr.args.size(); i += 2) {
				r_expr.param_types.push_back(*key);
				r_expr.param_types.push_back(*value);
			}
			return set(r_expr, Type::dictionary(*key, *value));
		}
		const Type name_key = entries->first.kind == TypeKind::Variant ? Type::builtin("StringName") : entries->first;
		for (size_t i = 0; i + 1 < r_expr.args.size(); i += 2) {
			const Type &want_key = r_expr.bool_value ? name_key : entries->first;
			const Type key = check_value(*r_expr.args[i], want_key);
			const Type value = check_value(*r_expr.args[i + 1], entries->second);
			r_expr.param_types.push_back(want_key);
			r_expr.param_types.push_back(entries->second);
			if (!assignable(key, want_key)) {
				error(r_expr.args[i]->range, "The keys of " + expected.to_string() + " are " + entries->first.to_string() + ", not " + key.to_string() + ".");
			}
			if (!assignable(value, entries->second)) {
				error(r_expr.args[i + 1]->range, "Cannot put a value of type " + value.to_string() + " in " + expected.to_string() + ".");
			}
		}
		return set(r_expr, expected);
	}

	// `x as T` (spec §5.3).
	Type check_cast(Expr &r_expr) {
		const Type from = check_value(*r_expr.left);
		const Type to = resolve(r_expr.cast_type, false);
		if (from.is_error() || to.is_error()) {
			return set(r_expr, Type::of(TypeKind::Error));
		}
		if (to.kind != TypeKind::Object) {
			const std::string name = to.unwrapped().to_string();
			return fail(r_expr, "as only converts objects; convert to " + name + " with " + name + "(x) or " + name + ".try_from(x).");
		}
		if (from.kind != TypeKind::Object) {
			return fail(r_expr, "as only converts objects, not " + from.to_string() + ".");
		}
		if (!related(from, to)) {
			return fail(r_expr, from.name + " and " + to.name + " are unrelated classes; as would always give None.");
		}
		r_expr.tested = to;
		// Already a T: nothing to convert.
		Type result = to;
		result.nullable = from.nullable || !is_a(from, to);
		return set(r_expr, result);
	}

	Type check_super(Expr &r_expr) {
		if (!function) {
			return fail_call(r_expr, "super is only valid inside a function.");
		}
		if (!r_expr.base) {
			// `super()` names no function.
			r_expr.text = function->name;
			r_expr.name_token = NO_TOKEN;
		}
		const FuncDecl *callee = base_function(r_expr.text);
		if (callee && callee->is_abstract) {
			return fail_call(r_expr, "\"" + callee->name + "()\" is abstract in the base class, which has no body to call (spec §4.1).");
		}
		if (callee) {
			refer(r_expr.name_token, script_id(declaring(*cls.base, *callee, &ClassDecl::functions).path, callee->name));
			if (!callee->is_static && refuse_in_static(r_expr, "super." + r_expr.text)) {
				return fail_call(r_expr);
			}
			check_arguments(r_expr, param_types(callee->params), required_params(*callee), false);
			r_expr.target = Target::SuperMethod;
			r_expr.engine_params = param_types(callee->params);
			r_expr.index = int(callee->params.size()) - required_params(*callee);
			r_expr.is_async = callee->is_async;
			return set(r_expr, callee->is_async ? Type::task(callee->return_type) : callee->return_type);
		}
		if (r_expr.text == "_init") {
			if (refuse_in_static(r_expr, "super._init")) {
				return fail_call(r_expr);
			}
			check_arguments(r_expr, {}, 0, false);
			r_expr.target = Target::Noop;
			refer(r_expr.name_token, engine_id("Object", "_init"));
			return set(r_expr, Type::of(TypeKind::Void));
		}
		const auto method = engine.find_method(cls.native_base, r_expr.text);
		if (!method || method->is_virtual) {
			return fail_call(r_expr, "No base implementation of \"" + r_expr.text + "()\".");
		}
		if (!method->is_static && refuse_in_static(r_expr, r_expr.text)) {
			return fail_call(r_expr);
		}
		r_expr.base.reset();
		return check_engine_call(r_expr, *method);
	}

	// The struct that a call constructs (spec §2.4): `Hit(...)` in the file
	// that declares it, `Combat.Hit(...)` anywhere; Error after reporting one
	// that may not be used here.
	std::optional<Type> constructed_struct(const Expr &p_call) {
		TypeRef ref;
		ref.range = p_call.range;
		ref.name_token = p_call.name_token;
		if (!p_call.base) {
			if (!cls.find_struct(p_call.text) || function_index.count(p_call.text)) {
				return std::nullopt;
			}
			ref.name = p_call.text;
		} else {
			const Expr &base = *p_call.base;
			if (base.kind != ExprKind::Name || member_index.count(base.text)) {
				return std::nullopt;
			}
			const ConstPtr alias = class_alias(base.text);
			if (!alias && (visible_local(base.text) || constant_index.count(base.text))) {
				return std::nullopt;
			}
			ref.name_token = base.name_token; // The struct's name two tokens on.
			std::string path = alias ? alias->script : std::string();
			if (!alias) {
				const bool own = !cls.class_name.empty() && base.text == cls.class_name;
				path = own ? options.path : (options.project ? options.project->global_class_path(base.text) : std::string());
			}
			const ClassDecl *header = is_script_path(path) ? script_header(path) : nullptr;
			if (!header || !header->find_struct(p_call.text)) {
				return std::nullopt;
			}
			ref.name = base.text + "." + p_call.text;
		}
		return script_type(ref);
	}

	// Fields by name, each once; those with a default may be left out (spec §3.6).
	Type check_struct_construction(Expr &r_expr, const Type &p_struct) {
		if (p_struct.is_error()) {
			return fail_call(r_expr);
		}
		const StructDecl *decl = struct_decl(p_struct);
		if (!decl) {
			return fail_call(r_expr, p_struct.script + " has errors.");
		}
		std::vector<bool> given(decl->fields.size(), false);
		for (size_t i = 0; i < r_expr.args.size(); i++) {
			Expr &arg = *r_expr.args[i];
			const std::string name = i < r_expr.arg_names.size() ? r_expr.arg_names[i] : std::string();
			size_t field = 0;
			while (field < decl->fields.size() && decl->fields[field].name != name) {
				field++;
			}
			if (field == decl->fields.size()) {
				check_value(arg);
				r_expr.param_types.push_back(Type::of(TypeKind::Error));
				error(arg.range, name.empty() ? "A struct takes its fields by name, as in " + p_struct.name + "(" + (decl->fields.empty() ? std::string("") : decl->fields[0].name + " = ...") + ") (spec §3.6)." : p_struct.name + " has no field \"" + name + "\".");
				continue;
			}
			if (given[field]) {
				error(arg.range, "\"" + name + "\" is given twice.");
			}
			given[field] = true;
			refer(r_expr.arg_name_tokens[i], script_id(p_struct.script, decl->name + "." + name));
			const Type &type = decl->fields[field].type;
			r_expr.param_types.push_back(type);
			expect_assignable(arg, check_value(arg, type), type, "pass");
		}
		for (size_t field = 0; field < decl->fields.size(); field++) {
			if (!given[field] && !decl->fields[field].initializer) {
				error(r_expr.range, p_struct.name + "() needs \"" + decl->fields[field].name + "\", which has no default value.");
			}
		}
		r_expr.target = Target::StructConstructor;
		return set(r_expr, p_struct);
	}

	// A static method called through the name of a built-in type or an engine
	// class, as Color.from_hsv() or FileAccess.open().
	Type check_static_call(Expr &r_expr) {
		const Expr &base = *r_expr.base;
		refer(base.name_token, engine_id(base.text));
		if (builtin::find_class(base.text)) {
			return check_builtin_method(r_expr, api_type(base.text, false), true);
		}
		if (const auto method = engine.find_method(base.text, r_expr.text)) {
			return check_engine_call(r_expr, *method, base.text);
		}
		return fail_call(r_expr, base.text + " has no method \"" + r_expr.text + "()\".");
	}

	// `Class.new()` or a static method of a class of this language, through its
	// class_name or a preload (spec §2.1, §4.6, §4.8).
	Type check_class_call(Expr &r_expr, const std::string &p_path) {
		const ClassDecl *header = script_header(p_path);
		if (!header) {
			return fail_call(r_expr, p_path + " cannot be read.");
		}
		const std::string class_name = display_name(*header, p_path);
		if (r_expr.text == "new") {
			const Type type = script_class_type(*header, p_path);
			if (header->is_abstract) {
				return fail_call(r_expr, class_name + " is abstract and cannot be instantiated (spec §4.1).");
			}
			if (!engine.can_instantiate(type.engine_class())) {
				return fail_call(r_expr, class_name + " cannot be instantiated: the engine cannot instantiate " + type.engine_class() + ", which it extends.");
			}
			if (!r_expr.args.empty()) {
				return fail_call(r_expr, "new() of " + class_name + " takes no arguments: _init() with parameters is not supported yet.");
			}
			refer(r_expr.name_token, script_id(p_path, "new"));
			r_expr.target = Target::NewObject;
			return set(r_expr, type);
		}
		const ClassDecl *decl = script_decl(p_path);
		if (!decl) {
			return fail_call(r_expr, class_name + " has errors.");
		}
		const auto found = std::find_if(decl->all_functions.begin(), decl->all_functions.end(), [&](const FuncDecl *p_function) { return p_function->name == r_expr.text; });
		if (found == decl->all_functions.end()) {
			return fail_call(r_expr, class_name + " has no method \"" + r_expr.text + "()\".");
		}
		const FuncDecl &callee = **found;
		if (!callee.is_static) {
			return fail_call(r_expr, "\"" + callee.name + "()\" of " + class_name + " is not static; call it on an object.");
		}
		if (!callee.is_pub && !extends_script(options.path, p_path)) {
			return fail_call(r_expr, "\"" + callee.name + "()\" of " + class_name + " is not pub.");
		}
		check_arguments(r_expr, param_types(callee.params), required_params(callee), false);
		refer(r_expr.name_token, script_id(declaring(*decl, callee, &ClassDecl::functions).path, callee.name));
		if (p_path == options.path) {
			r_expr.target = Target::ScriptFunction;
			r_expr.index = int(found - decl->all_functions.begin());
		} else {
			// Called on the class's current image (spec §12.4).
			r_expr.target = Target::ScriptMethod;
			r_expr.is_static = true;
			r_expr.owner = p_path;
			r_expr.engine_params = param_types(callee.params);
			r_expr.index = int(callee.params.size()) - required_params(callee);
		}
		r_expr.is_async = callee.is_async;
		return set(r_expr, callee.is_async ? Type::task(callee.return_type) : callee.return_type);
	}

	// The script of a class of another language as the base of `r_access`,
	// which its static methods are called and its constants read on (spec
	// §11.1): a constant or a preload of it is the script already; a
	// class_name loads it as preload() does.
	Type script_base(Expr &r_access, const std::string &p_path) {
		if (!r_access.base->constant) {
			auto path = std::make_unique<Expr>();
			path->kind = ExprKind::StringLiteral;
			path->op = TokenKind::String;
			path->text = p_path;
			path->range = r_access.base->range;
			auto load = std::make_unique<Expr>();
			load->kind = ExprKind::Call;
			load->text = "preload";
			load->range = r_access.base->range;
			load->name_token = NO_TOKEN;
			load->args.push_back(std::move(path));
			r_access.base = std::move(load);
			return check_value(*r_access.base);
		}
		return r_access.base->type;
	}

	// `Class.new(...)` or a static method of a class of another language,
	// called on its script; new() takes what its _init does (spec §11.1).
	Type check_foreign_class_call(Expr &r_expr, const Type &p_class) {
		const ForeignClass *foreign = foreign_class(p_class.script);
		if (!foreign || !foreign->members_known) {
			fail_call(r_expr);
			return unknown_foreign(r_expr, p_class);
		}
		const auto find = [&](const std::string &p_name) {
			return std::find_if(foreign->methods.begin(), foreign->methods.end(), [&](const EngineMethod &p_method) { return p_method.name == p_name; });
		};
		EngineMethod method;
		if (r_expr.text == "new") {
			if (!engine.can_instantiate(p_class.engine_class())) {
				return fail_call(r_expr, p_class.name + " cannot be instantiated: the engine cannot instantiate " + p_class.engine_class() + ", which it extends.");
			}
			const auto init = find("_init");
			if (init != foreign->methods.end()) {
				method = *init;
			}
			method.name = "new";
			method.return_type = p_class;
		} else {
			const auto found = find(r_expr.text);
			if (found == foreign->methods.end()) {
				return fail_call(r_expr, p_class.name + " has no method \"" + r_expr.text + "()\".");
			}
			if (!found->is_static) {
				return fail_call(r_expr, "\"" + r_expr.text + "()\" of " + p_class.name + " is not static; call it on an object.");
			}
			method = *found;
			method.return_type = localized(method.return_type);
		}
		for (EngineParam &param : method.params) {
			param.type = localized(param.type);
		}
		// A method of the script object, which GDScript's answers by calling the
		// static function (modules/gdscript/gdscript.cpp:928-940).
		method.is_static = false;
		method.is_virtual = false;
		if (script_base(r_expr, p_class.script).is_error()) {
			return fail_call(r_expr);
		}
		r_expr.is_foreign = true;
		refer(r_expr.name_token, foreign_id(p_class.script, r_expr.text));
		return check_engine_call(r_expr, method);
	}

	// A constant of a class of another language, read on its script (spec
	// §11.1), which cannot be assigned.
	Type check_foreign_constant(Expr &r_expr, const Type &p_class) {
		const ForeignClass *foreign = foreign_class(p_class.script);
		if (!foreign || !foreign->members_known) {
			return unknown_foreign(r_expr, p_class);
		}
		for (const EngineProperty &constant : foreign->constants) {
			if (constant.name != r_expr.text) {
				continue;
			}
			if (!constant.type.is_storable()) {
				return fail(r_expr, "The constant \"" + r_expr.text + "\" has type " + constant.type.to_string() + ", which is not supported yet.");
			}
			const Type script = script_base(r_expr, p_class.script);
			if (script.is_error()) {
				return set(r_expr, script);
			}
			r_expr.target = Target::ObjectProperty;
			r_expr.owner = script.engine_class();
			r_expr.is_static = true;
			refer(r_expr.name_token, foreign_id(p_class.script, r_expr.text));
			return set(r_expr, localized(constant.type));
		}
		return fail(r_expr, p_class.name + " has no constant \"" + r_expr.text + "\".");
	}

	Type check_call(Expr &r_expr) {
		if (const std::optional<Type> constructed = constructed_struct(r_expr)) {
			return check_struct_construction(r_expr, *constructed);
		}
		if (const EnumDecl *decl = called_enum(r_expr)) {
			return check_enum_conversion(r_expr, *decl);
		}
		if (r_expr.base && r_expr.text == "try_from" && named_enum(*r_expr.base)) {
			for (auto &arg : r_expr.args) {
				check_value(*arg);
			}
			return fail_call(r_expr, "try_from() is not supported yet; convert with " + r_expr.base->text + "(x).");
		}
		if (!r_expr.arg_names.empty()) {
			return fail_call(r_expr, "Only structs take arguments by name yet; \"" + r_expr.text + "()\" takes them by position (spec §4.6).");
		}
		if ((!r_expr.base && r_expr.text == "super") || (r_expr.base && r_expr.base->kind == ExprKind::Name && r_expr.base->text == "super")) {
			return check_super(r_expr);
		}
		const std::string &name = r_expr.text;

		if (r_expr.base && r_expr.base->kind != ExprKind::Self) {
			Expr &base = *r_expr.base;
			if (names_type(base)) {
				if (name == "new" && engine.class_exists(base.text) && !builtin::find_class(base.text)) {
					if (!engine.can_instantiate(base.text)) {
						return fail_call(r_expr, base.text + " cannot be instantiated.");
					}
					if (!r_expr.args.empty()) {
						return fail_call(r_expr, "new() of an engine class takes no arguments.");
					}
					r_expr.target = Target::NewObject;
					r_expr.owner = base.text;
					refer(base.name_token, engine_id(base.text));
					refer(r_expr.name_token, engine_id(base.text, name));
					return set(r_expr, Type::object(base.text));
				}
				return check_static_call(r_expr);
			}
			if (const std::optional<Type> named = named_class(base)) {
				if (named->is_error()) {
					return fail_call(r_expr);
				}
				return is_script_path(named->script) ? check_class_call(r_expr, named->script) : check_foreign_class_call(r_expr, *named);
			}
			const Type base_type = check_value(base);
			if (base_type.is_error()) {
				return fail_call(r_expr);
			}
			if (base_type.kind == TypeKind::Task && !base_type.nullable && (name == "cancel" || name == "detach")) {
				check_arguments(r_expr, {}, 0, false);
				r_expr.target = name == "cancel" ? Target::TaskCancel : Target::TaskDetach;
				refer(r_expr.name_token, language_id("Task", name));
				return set(r_expr, Type::of(TypeKind::Void));
			}
			if (base_type.is_optional()) {
				return check_option_method(r_expr, base_type);
			}
			if (base_type.kind == TypeKind::Function && (name == "bind" || name == "call" || name == "callv")) {
				return name == "bind" ? check_bind(r_expr, base_type) : check_function_call(r_expr, base_type);
			}
			if (base_type.kind == TypeKind::Signal && name == "emit") {
				// What the connected functions take (spec §4.7).
				check_arguments(r_expr, base_type.args, int(base_type.args.size()), false);
				r_expr.target = Target::EmitSignal;
				refer(r_expr.name_token, engine_id("Signal", "emit"));
				return set(r_expr, Type::of(TypeKind::Void));
			}
			if (base_type.kind == TypeKind::Signal && (name == "connect" || name == "disconnect" || name == "is_connected")) {
				if (!r_expr.args.empty() && r_expr.args[0]->kind == ExprKind::Lambda) {
					// It takes what the signal passes (spec §4.7).
					check_lambda(*r_expr.args[0], Type::function(Type::of(TypeKind::Void), base_type.args));
				}
				const Type result = check_builtin_method(r_expr, base_type);
				if (!result.is_error() && !r_expr.args.empty()) {
					check_handler(*r_expr.args[0], base_type);
				}
				return result;
			}
			if (!base_type.builtin_name().empty()) {
				const Type result = check_builtin_method(r_expr, base_type);
				const std::string constant = result.is_error() || r_expr.is_const ? std::string() : constant_root(base);
				if (!constant.empty()) {
					return fail(r_expr, "\"" + constant + "\" is a constant, whose containers are read-only (spec §4.5).");
				}
				return result;
			}
			if (base_type.kind == TypeKind::Object) {
				if (!base_type.script.empty()) {
					const std::optional<Type> result = script_method(r_expr, base_type);
					if (result) {
						return *result;
					}
				}
				if (const auto method = engine.find_method(base_type.engine_class(), name)) {
					const Type result = check_engine_call(r_expr, *method);
					if (name == "instantiate" && base_type.name == "PackedScene" && !base_type.args.empty() && !result.is_error()) {
						// The scene's root, checked as it is made (spec §7.4).
						return set(r_expr, base_type.args[0]);
					}
					return result;
				}
				return fail(r_expr, base_type.name + " has no method \"" + name + "()\".");
			}
			return fail(r_expr, "Values of type " + base_type.to_string() + " have no methods.");
		}

		if (!r_expr.base) {
			// As in GDScript (modules/gdscript/gdscript_analyzer.cpp:3741-3745).
			const LocalInfo *local = visible_local(name);
			const auto member = member_index.find(name);
			const Type *value = local ? &local->type : (member != member_index.end() ? &cls.all_members[member->second]->type : nullptr);
			if (value && value->unwrapped().kind == TypeKind::Function) {
				return fail_call(r_expr, "\"" + name + "\" is a function value; call it as " + name + ".call(...) (spec §3.11).");
			}
		}
		const auto script = function_index.find(name);
		if (script != function_index.end()) {
			const FuncDecl &callee = *cls.all_functions[script->second];
			if (!callee.is_static && refuse_in_static(r_expr, name + "()")) {
				return fail_call(r_expr);
			}
			check_arguments(r_expr, param_types(callee.params), required_params(callee), false);
			r_expr.target = Target::ScriptFunction;
			r_expr.index = script->second;
			refer(r_expr.name_token, script_id(declaring(cls, callee, &ClassDecl::functions).path, callee.name));
			r_expr.is_async = callee.is_async;
			return set(r_expr, callee.is_async ? Type::task(callee.return_type) : callee.return_type);
		}

		if (const auto method = engine.find_method(cls.native_base, name)) {
			if (method->is_virtual) {
				return fail_call(r_expr, "\"" + name + "()\" is a virtual engine method that this class does not define.");
			}
			// A static method needs no instance, but `self` does.
			if ((!method->is_static || r_expr.base) && refuse_in_static(r_expr, method->is_static ? std::string("self") : name + "()")) {
				return set(r_expr, Type::of(TypeKind::Error));
			}
			r_expr.base = nullptr; // `self.f()` is `f()`.
			return check_engine_call(r_expr, *method);
		}

		if (!r_expr.base) {
			if (name == "int" || name == "float") {
				const TypeKind result = name == "int" ? TypeKind::Int : TypeKind::Float;
				r_expr.target = name == "int" ? Target::ConvertToInt : Target::ConvertToFloat;
				refer(r_expr.name_token, engine_id(name));
				if (r_expr.args.size() != 1) {
					return fail_call(r_expr, name + "() takes one argument.");
				}
				const Type arg = check_value(*r_expr.args[0]);
				if (!arg.is_error() && !arg.is_numeric()) {
					return fail(*r_expr.args[0], name + "() of " + arg.to_string() + " is not supported yet.");
				}
				r_expr.param_types.push_back(arg);
				return set(r_expr, Type::of(result));
			}
			if (name == "print_debug" && outermost()) {
				// GDScript's own: print() of the arguments and where it is called
				// (modules/gdscript/gdscript_utility_functions.cpp:349-362), which is
				// known here.
				auto where = std::make_unique<Expr>();
				where->kind = ExprKind::StringLiteral;
				where->range = r_expr.range;
				where->text = "\n   At: " + options.path + ":" + std::to_string(cls.source->line(r_expr.range.first)) + ":" + outermost()->name + "()";
				r_expr.args.push_back(std::move(where));
				refer(r_expr.name_token, engine_id("@GlobalScope", "print_debug"));
				// Calls print(), as written nowhere.
				r_expr.text = "print";
				r_expr.name_token = NO_TOKEN;
			}
			if (const builtin::Utility *utility = builtin::find_utility(r_expr.text)) {
				return check_utility(r_expr, *utility);
			}
			if (name != "bool" && name != "String") {
				if (const builtin::Class *type = builtin::find_class(name)) {
					return check_constructor(r_expr, *type);
				}
			}
			if (name == "preload") {
				return check_preload(r_expr);
			}
			if (name == "load" && engine.is_singleton("ResourceLoader")) {
				// GDScript's load() is ResourceLoader.load(), whose result may be
				// None (spec §7.4, §10.2).
				on_resource_loader(r_expr);
				return check_call(r_expr);
			}
		}

		return fail_call(r_expr, "Function \"" + name + "()\" not found in the class or its base " + cls.extends + ".");
	}

	Type check_await(Expr &r_expr) {
		if (in_predelete) {
			return fail(r_expr, "await is not allowed in the PREDELETE branch.");
		}
		if (!function || !function->is_async || initializing >= 0) {
			return fail(r_expr, "await requires an async function and cannot appear in an initializer.");
		}
		Expr &operand = *r_expr.left;
		operand.awaited = true;
		const Type type = check_expr(operand);
		if (type.is_error()) {
			return set(r_expr, type);
		}
		if (type.kind == TypeKind::Task) {
			if (type.nullable) {
				return fail(r_expr, "Unwrap the optional Task before awaiting it.");
			}
			return set(r_expr, type.args[0]);
		}
		if (type.kind == TypeKind::Signal) {
			return set(r_expr, type.args.size() == 1 ? type.args[0] : Type::of(TypeKind::Void));
		}
		if (type.builtin_name() == "Signal") {
			return set(r_expr, Type::of(TypeKind::Variant));
		}
		if (operand.kind == ExprKind::Call && (operand.is_foreign || (operand.base && operand.base->type == Type::builtin("Callable") && (operand.text == "call" || operand.text == "callv")))) {
			return set(r_expr, type);
		}
		return fail(r_expr, "await expects a Task, a signal or a call to GDScript.");
	}

	// Types an expression and computes its value when it is a constant
	// expression (spec §4.5).
	Type check_expr(Expr &r_expr, const Type &p_expected = Type::of(TypeKind::Error)) {
		const Type type = type_expr(r_expr, p_expected);
		if (type.is_error()) {
			return type;
		}
		if (!r_expr.constant) {
			r_expr.constant = fold(r_expr);
		}
		return type;
	}

	void report_constant_error(const Expr &p_expr) {
		if (p_expr.constant_error_at) {
			error(p_expr.constant_error_at->range, p_expr.constant_error);
		} else if (p_expr.kind == ExprKind::Binary && p_expr.op_token != NO_TOKEN) {
			error(p_expr.op_token, p_expr.constant_error);
		} else {
			error(p_expr.range, p_expr.constant_error);
		}
	}

	// The part of an expression that keeps it from being a constant expression.
	static const Expr &not_constant_part(const Expr &p_expr) {
		for (const Expr *child : { p_expr.base.get(), p_expr.left.get(), p_expr.right.get() }) {
			if (child && !child->constant && !child->type.is_error()) {
				return not_constant_part(*child);
			}
		}
		for (const auto &arg : p_expr.args) {
			if (!arg->constant && !arg->type.is_error()) {
				return not_constant_part(*arg);
			}
		}
		return p_expr;
	}

	// Reports why an expression whose value must be a constant is not one.
	void require_constant(const Expr &p_expr) {
		const Expr &part = not_constant_part(p_expr);
		if (!part.constant_error.empty()) {
			report_constant_error(part);
			return;
		}
		std::string what = "This is not a constant expression";
		switch (part.kind) {
			case ExprKind::Call:
				what = "\"" + part.text + "()\" is not a constant expression: of calls, only constructors of built-in types and math functions are";
				break;
			case ExprKind::Name:
			case ExprKind::Member:
				if (part.target == Target::Local || part.target == Target::Member || part.target == Target::SelfProperty || part.target == Target::ObjectProperty) {
					what = "\"" + part.text + "\" is a variable, not a constant";
				}
				break;
			case ExprKind::Self:
				what = "self is not a constant expression";
				break;
			case ExprKind::Cast:
			case ExprKind::TypeTest:
				what = "as and is are not supported in constant expressions";
				break;
			default:
				break;
		}
		error(part.range, what + " (spec §4.5).");
	}

	// The string type a string literal is built as: the one it goes to, when
	// it converts to that (spec §10.7), otherwise its own.
	Type literal_type(const Expr &p_literal, const Type &p_to) const {
		const Type own = p_literal.op == TokenKind::StringName ? Type::builtin("StringName") : (p_literal.op == TokenKind::NodePath ? Type::builtin("NodePath") : Type::of(TypeKind::String));
		const Type to = p_to.kind == TypeKind::Option ? p_to.args[0] : p_to;
		const bool text = to.kind == TypeKind::String || to.builtin_name() == "StringName" || to.builtin_name() == "NodePath";
		return text && assignable(own, to) ? to : own;
	}

	// `p_expected` is the type the value goes to, if known; literals of arrays,
	// dictionaries, strings and None take their type from it.
	Type type_expr(Expr &r_expr, const Type &p_expected) {
		switch (r_expr.kind) {
			case ExprKind::IntLiteral:
				if (r_expr.int_overflows) {
					return fail(r_expr, "Integer literal is too large for int.");
				}
				return set(r_expr, Type::of(TypeKind::Int));
			case ExprKind::FloatLiteral:
				return set(r_expr, Type::of(TypeKind::Float));
			case ExprKind::StringLiteral:
				return set(r_expr, literal_type(r_expr, p_expected));
			case ExprKind::BoolLiteral:
				return set(r_expr, Type::of(TypeKind::Bool));
			case ExprKind::Name:
				return check_name(r_expr);
			case ExprKind::Await:
				return check_await(r_expr);
			case ExprKind::Self:
				return check_self(r_expr);
			case ExprKind::Unary:
				return check_unary(r_expr);
			case ExprKind::Binary:
				return check_binary(r_expr);
			case ExprKind::Call:
				return check_call(r_expr);
			case ExprKind::Member:
				return check_member_access(r_expr);
			case ExprKind::GetNode:
				return check_get_node(r_expr);
			case ExprKind::Cast:
				return check_cast(r_expr);
			case ExprKind::ArrayLiteral:
				return check_array_literal(r_expr, p_expected);
			case ExprKind::DictionaryLiteral:
				return check_dictionary_literal(r_expr, p_expected);
			case ExprKind::Subscript:
				return check_subscript(r_expr);
			case ExprKind::Conditional:
				return check_conditional(r_expr, p_expected);
			case ExprKind::NoneLiteral:
				// The T? it goes to, where that is known.
				if (p_expected.is_optional() || p_expected.kind == TypeKind::Variant) {
					return set(r_expr, p_expected);
				}
				return set(r_expr, Type::of(TypeKind::None));
			case ExprKind::TypeTest:
				return check_type_test(r_expr);
			case ExprKind::Lambda:
				return check_lambda(r_expr, p_expected);
			case ExprKind::Error:
				break;
		}
		return set(r_expr, Type::of(TypeKind::Error));
	}

	// Checks an expression whose value is stored, passed or returned.
	Type check_value(Expr &r_expr, const Type &p_expected = Type::of(TypeKind::Error)) {
		const Type type = check_expr(r_expr, p_expected);
		if (type.kind == TypeKind::Void) {
			error(r_expr.range, "This call returns void and has no value.");
			return Type::of(TypeKind::Error);
		}
		if (type.kind == TypeKind::Unsupported || type.kind == TypeKind::Variant) {
			error(r_expr.range, "Values of type " + type.to_string() + " are not supported yet.");
			return Type::of(TypeKind::Error);
		}
		return type;
	}

	// Whether the expression is a place that can be written: a local, a
	// member, a property, or a member of a built-in value in such a place,
	// which is then written back (architecture §2.1).
	bool check_place(const Expr &p_target) {
		const std::string constant = constant_root(p_target);
		if (!constant.empty()) {
			const bool whole = p_target.kind != ExprKind::Subscript && p_target.target != Target::Field && p_target.target != Target::StructField;
			error(p_target.range, whole ? "Cannot assign to the constant \"" + constant + "\"." : "\"" + constant + "\" is a constant, whose containers are read-only (spec §4.5).");
			return false;
		}
		if (p_target.kind == ExprKind::Subscript) {
			// An Array or a Dictionary changes in place; a packed array is a value,
			// written back along the chain.
			return is_shared(p_target.base->type) || check_written_back(p_target);
		}
		switch (p_target.target) {
			case Target::Local:
				if (is_capture(p_target.index)) {
					// It would change neither the variable outside nor the next call (spec §3.11).
					error(p_target.range, "Cannot assign to \"" + p_target.text + "\", which the lambda captures as a copy; keep what it shares in a member or a container (spec §3.11).");
					return false;
				}
				return true;
			case Target::Member:
			case Target::Owner:
				return true;
			case Target::SelfProperty:
			case Target::ObjectProperty: {
				if (p_target.is_static) {
					error(p_target.range, "Cannot assign to the constant \"" + p_target.text + "\".");
					return false;
				}
				const std::string &owner = p_target.target == Target::SelfProperty ? cls.native_base : p_target.owner;
				const auto property = engine.find_property(owner, p_target.text);
				if (property && !property->writable) {
					error(p_target.range, "The property \"" + p_target.text + "\" is read-only.");
					return false;
				}
				const InterfaceProperty *required = p_target.base && p_target.base->type.is_interface ? interface_property(p_target.base->type, p_target.text) : nullptr;
				if (required && required->is_readonly) {
					error(p_target.range, "The property \"" + p_target.text + "\" of " + p_target.base->type.to_string() + " is @readonly.");
					return false;
				}
				return true;
			}
			case Target::Field:
			case Target::StructField:
				return check_written_back(p_target);
			default:
				if (!p_target.type.is_error()) {
					error(p_target.range, "Cannot assign to this expression.");
				}
				return false;
		}
	}

	// The constant an expression is or is a part of, as `C`, `C[i]` and `C.x`
	// are; empty for anything else.
	std::string constant_root(const Expr &p_expr) const {
		const bool name = p_expr.kind == ExprKind::Name || p_expr.kind == ExprKind::Member;
		if (name && (p_expr.target == Target::Constant || (p_expr.target == Target::Local && function && function->locals[p_expr.index].constant))) {
			return p_expr.text;
		}
		const bool part = p_expr.kind == ExprKind::Subscript || (p_expr.kind == ExprKind::Member && (p_expr.target == Target::Field || p_expr.target == Target::StructField));
		return part && p_expr.base ? constant_root(*p_expr.base) : std::string();
	}

	// A part of a value, changed in a copy written back along the chain (spec §3.6).
	bool check_written_back(const Expr &p_target) {
		const ExprKind base = p_target.base->kind;
		if (base == ExprKind::Name || base == ExprKind::Member || base == ExprKind::Subscript) {
			return check_place(*p_target.base);
		}
		error(p_target.range, p_target.kind == ExprKind::Subscript ? "Cannot assign to an element of a temporary value." : "Cannot assign to a member of a temporary value.");
		return false;
	}

	void expect_assignable(const Expr &p_value, const Type &p_from, const Type &p_to, const char *p_action) {
		if (assignable(p_from, p_to)) {
			return;
		}
		std::string message = std::string("Cannot ") + p_action + " a value of type " + p_from.to_string() + " as " + p_to.to_string() + ".";
		if (p_from.kind == TypeKind::Float && p_to.kind == TypeKind::Int) {
			message += " Convert explicitly with int(x).";
		}
		if (p_to.unwrapped().kind == TypeKind::Enum && (p_from.kind == TypeKind::Int || p_from.kind == TypeKind::Enum)) {
			const std::string name = enum_spelling(p_to.unwrapped());
			message += " Convert explicitly with " + name + "(x), which checks that it is one of the values (spec §3.7).";
		}
		if (p_to.kind == TypeKind::Variant && is_typed_container(p_from) && is_boxed(p_from)) {
			message += " Its elements exist only in this language; convert it explicitly with " + p_from.name + "(x), a copy with the elements encoded (spec §3.8).";
		}
		error(p_value.range, message);
	}

	void expect_condition(Expr &r_condition) {
		const Type type = check_value(r_condition);
		if (type.is_error() || type.kind == TypeKind::Bool) {
			return;
		}
		std::string hint;
		if (type.is_numeric()) {
			hint = " Compare explicitly, for example x != 0.";
		} else if (type.kind == TypeKind::String) {
			hint = " Compare explicitly, for example x != \"\".";
		}
		error(r_condition.range, "A condition must be bool, found " + type.to_string() + "." + hint);
	}

	/* Statements */

	// Returns what is known at the end of the block, starting from `p_facts`.
	std::unordered_map<int, Type> check_block(Block &r_block, const std::unordered_map<int, Type> &p_facts = {}) {
		scopes.emplace_back();
		narrowed.push_back(p_facts);
		for (auto &stmt : r_block) {
			check_stmt(*stmt);
		}
		std::unordered_map<int, Type> facts = std::move(narrowed.back());
		narrowed.pop_back();
		scopes.pop_back();
		return facts;
	}

	int declare_local(const std::string &p_name, uint32_t p_at, const Type &p_type) {
		const int known = find_local(p_name);
		if (scopes.back().count(p_name)) {
			error(p_at, "\"" + p_name + "\" is already declared in this block.");
		} else if (known >= 0 ? is_capture(known) : visible_local(p_name) != nullptr) {
			error(p_at, "\"" + p_name + "\" is already declared in the function around this lambda (spec §4.6).");
		} else if (scopes.front().count(p_name)) {
			error(p_at, "\"" + p_name + "\" is already a parameter of this function (spec §4.6).");
		} else if (known >= 0) {
			error(p_at, "\"" + p_name + "\" is already declared in an enclosing block (spec §4.6).");
		}
		const int local = int(function->locals.size());
		function->locals.push_back({ p_name, p_type, p_at });
		refer(p_at, local_id(p_at), true);
		scopes.back()[p_name] = local;
		return local;
	}

	void declare_local(Stmt &r_stmt, const Type &p_type) {
		r_stmt.local = declare_local(r_stmt.name, r_stmt.name_token, p_type);
	}

	// `match` (spec §5.6). The matched value goes in a local without a name,
	// which the debugger shows as a temporary, and a value pattern becomes the
	// comparison of that local with the pattern, checked as == is.
	void check_match(Stmt &r_stmt) {
		const Type matched = check_value(*r_stmt.value);
		if (matched.is_optional()) {
			error(r_stmt.value->range, "A match over " + matched.to_string() + " is not supported yet; compare it with None, or unwrap it first.");
		}
		scopes.emplace_back();
		r_stmt.local = int(function->locals.size());
		function->locals.push_back({ std::string(), matched });
		scopes.back()[std::string()] = r_stmt.local;
		const bool was_predelete = in_predelete;
		for (MatchBranch &branch : r_stmt.branches) {
			scopes.emplace_back();
			for (Pattern &pattern : branch.patterns) {
				if (pattern.kind == PatternKind::Bind && branch.patterns.size() > 1) {
					error(pattern.range, "Patterns separated by commas cannot bind (spec §5.6).");
				}
				check_pattern(pattern, matched);
			}
			in_predelete = was_predelete || predelete_branch(*r_stmt.value, branch);
			if (branch.guard) {
				expect_condition(*branch.guard);
			}
			check_block(branch.body, {});
			in_predelete = was_predelete;
			scopes.pop_back();
		}
		scopes.pop_back();
		if (matched.kind == TypeKind::Enum && matched.values) {
			check_exhaustive(r_stmt, matched);
		}
	}

	// A match over an enum handles every value or has `_`; branches with
	// `when` do not count (spec §3.7, §5.6).
	void check_exhaustive(const Stmt &p_match, const Type &p_enum) {
		std::set<int64_t> handled;
		for (const MatchBranch &branch : p_match.branches) {
			for (const Pattern &pattern : branch.patterns) {
				if (branch.guard) {
					continue;
				}
				if (pattern.kind != PatternKind::Value) {
					return;
				}
				const Expr *value = pattern.value && pattern.value->right ? pattern.value->right.get() : nullptr;
				if (value && value->constant && value->constant->kind == ConstValue::Kind::Int) {
					handled.insert(value->constant->i);
				}
			}
		}
		std::vector<std::string> missing;
		for (const auto &[name, value] : p_enum.values->list) {
			if (!handled.count(value)) {
				missing.push_back(name);
			}
		}
		if (missing.empty()) {
			return;
		}
		std::string names;
		for (size_t i = 0; i < missing.size() && i < 8; i++) {
			names += (i == 0 ? "" : (i + 1 == missing.size() ? " and " : ", ")) + missing[i];
		}
		if (missing.size() > 8) {
			names += " and " + std::to_string(missing.size() - 8) + " more";
		}
		error(p_match.value->range, "This match over " + enum_spelling(p_enum) + " does not handle " + names + "; handle them or add a `_` branch (spec §5.6).");
	}

	void check_pattern(Pattern &r_pattern, const Type &p_matched) {
		switch (r_pattern.kind) {
			case PatternKind::Wildcard:
				return;
			case PatternKind::Bind:
				r_pattern.local = declare_local(r_pattern.name, r_pattern.name_token, p_matched);
				return;
			case PatternKind::Value: {
				auto compare = std::make_unique<Expr>();
				compare->kind = ExprKind::Binary;
				compare->op = TokenKind::EqualEqual;
				compare->range = r_pattern.range;
				compare->left = std::make_unique<Expr>();
				compare->left->kind = ExprKind::Name;
				compare->left->range = r_pattern.range;
				compare->left->name_token = NO_TOKEN;
				compare->right = std::move(r_pattern.value);
				r_pattern.value = std::move(compare);
				check_expr(*r_pattern.value);
				const Expr &value = *r_pattern.value->right;
				if (!value.type.is_error() && !value.constant) {
					error(value.range, "A pattern is a literal or a constant expression (spec §5.6).");
				}
				return;
			}
		}
	}

	// `for i in n`, over the elements of an array and over the keys of a
	// Dictionary (spec §5.7).
	void check_for(Stmt &r_stmt) {
		const Type iterated = check_value(*r_stmt.value);
		Type element = Type::of(TypeKind::Error);
		const std::string &name = iterated.name;
		if (iterated.kind == TypeKind::Int) {
			element = iterated;
		} else if (iterated.kind == TypeKind::Float || (iterated.kind == TypeKind::Builtin && (name == "Vector2" || name == "Vector2i" || name == "Vector3" || name == "Vector3i"))) {
			error(r_stmt.value->range, "Cannot iterate over " + iterated.to_string() + "; iterate over range(...) instead.");
		} else if (const std::optional<std::pair<Type, Type>> entries = entries_of(iterated)) {
			if (!refuse_variant_elements(entries->first, *r_stmt.value)) {
				element = entries->first;
			}
		} else if (const std::optional<Type> elements = element_of(iterated)) {
			if (!refuse_variant_elements(*elements, *r_stmt.value)) {
				element = *elements;
			}
		} else if (!iterated.is_error()) {
			error(r_stmt.value->range, "Iterating over " + iterated.to_string() + " is not supported yet.");
		}
		r_stmt.element = element;
		Type type = element;
		if (r_stmt.declared_type) {
			const Type declared = resolve(*r_stmt.declared_type, false);
			// The elements' type or a class they extend, without conversions.
			const bool fits = declared == element || (declared.kind == TypeKind::Object && element.kind == TypeKind::Object && assignable(element, declared));
			if (!declared.is_error() && !element.is_error() && !fits) {
				error(r_stmt.declared_type->range, "The elements are " + element.to_string() + ", which the loop variable of type " + declared.to_string() + " cannot hold.");
			}
			type = declared;
		}
		scopes.emplace_back();
		declare_local(r_stmt, type);
		loop_depth++;
		check_block(r_stmt.body);
		loop_depth--;
		scopes.pop_back();
	}

	bool predelete_branch(const Expr &p_condition, bool p_true) const {
		if (p_condition.kind != ExprKind::Binary || p_condition.op != (p_true ? TokenKind::EqualEqual : TokenKind::BangEqual)) {
			return false;
		}
		const Expr *value = p_condition.left.get();
		const Expr *constant = p_condition.right.get();
		if (value->target == Target::EngineConstant) {
			std::swap(value, constant);
		}
		return is_notification(*value) && is_predelete(*constant);
	}

	// `NOTIFICATION_PREDELETE:` in `match what:`.
	bool predelete_branch(const Expr &p_matched, const MatchBranch &p_branch) const {
		for (const Pattern &pattern : p_branch.patterns) {
			if (pattern.kind == PatternKind::Value && is_notification(p_matched) && is_predelete(*pattern.value->right)) {
				return true;
			}
		}
		return false;
	}

	// The parameter of _notification(what).
	bool is_notification(const Expr &p_expr) const {
		return function && function->name == "_notification" && p_expr.target == Target::Local && p_expr.index == 0;
	}

	static bool is_predelete(const Expr &p_expr) {
		return p_expr.target == Target::EngineConstant && p_expr.text == "NOTIFICATION_PREDELETE";
	}

	// A string on a line of its own, which GDScript takes for a comment
	// (modules/gdscript/gdscript_parser.cpp:2177-2182).
	static bool is_comment(const Expr &p_expr) {
		return p_expr.kind == ExprKind::StringLiteral && p_expr.op == TokenKind::String;
	}

	void check_stmt(Stmt &r_stmt) {
		switch (r_stmt.kind) {
			case StmtKind::Var: {
				Type type = Type::of(TypeKind::Error);
				if (r_stmt.declared_type) {
					type = resolve(*r_stmt.declared_type, false);
				}
				if (r_stmt.value) {
					const Type value = check_value(*r_stmt.value, type);
					if (r_stmt.declared_type) {
						expect_assignable(*r_stmt.value, value, type, "assign");
					} else if (value.kind == TypeKind::None) {
						error(r_stmt.name_token, "\"" + r_stmt.name + "\" needs a type: None does not say which T? it is (spec §4.3).");
						type = Type::of(TypeKind::Error);
					} else {
						type = value;
					}
				} else if (!r_stmt.declared_type) {
					error(r_stmt.name_token, "\"" + r_stmt.name + "\" needs a type or an initial value.");
				} else if (((type.kind == TypeKind::Object || type.kind == TypeKind::Task) && !type.nullable) || (type.kind == TypeKind::Struct && !has_default(type))) {
					error(r_stmt.name_token, "\"" + r_stmt.name + "\" of type " + type.to_string() + " needs an initial value.");
				} else if (!enum_has_default(type)) {
					error(r_stmt.name_token, "\"" + r_stmt.name + "\" needs an initial value: " + enum_spelling(type) + " has no value 0 to start at (spec §3.7).");
				}
				declare_local(r_stmt, type);
				return;
			}
			case StmtKind::Const: {
				Type type;
				const ConstPtr value = constant_value(*r_stmt.value, r_stmt.declared_type, type);
				declare_local(r_stmt, type);
				function->locals[r_stmt.local].constant = value;
				return;
			}
			case StmtKind::Assign: {
				Expr &target = *r_stmt.target;
				const bool compound = r_stmt.assign_op != TokenKind::Equal;
				Type target_type = Type::of(TypeKind::Error);
				if (target.kind == ExprKind::Name || target.kind == ExprKind::Member) {
					target_type = check_expr(target);
				} else if (target.kind == ExprKind::Subscript) {
					// An element of an untyped container is written as a Variant.
					target_type = check_subscript(target, !compound);
				} else {
					check_expr(target);
					error(target.range, "Cannot assign to this expression.");
					check_value(*r_stmt.value);
					return;
				}
				// A compound assignment reads the local as it is narrowed.
				const Type read_type = target_type;
				const int local = target.target == Target::Local ? target.index : -1;
				if (local >= 0) {
					// A narrowed local takes what its declared type takes.
					target_type = function->locals[local].type;
					if (!compound) {
						target.type = target_type;
					}
				}
				Type value = check_value(*r_stmt.value, compound ? Type::of(TypeKind::Error) : target_type);
				const bool place = !target_type.is_error() && check_place(target);
				if (compound && place) {
					value = binary_type(compound_operator(r_stmt.assign_op), read_type, value, r_stmt.range);
					r_stmt.compound_result = value;
				}
				if (local >= 0) {
					forget(local);
					// Narrowing follows the new value (spec §5.4).
					if (target_type.is_optional() && !value.is_error() && !value.is_optional() && value.kind != TypeKind::None) {
						narrowed.back()[local] = target_type.unwrapped();
					}
				}
				if (!place) {
					return;
				}
				expect_assignable(*r_stmt.value, value, target_type, "assign");
				return;
			}
			case StmtKind::If: {
				expect_condition(*r_stmt.condition);
				const Facts facts = facts_of(*r_stmt.condition);
				const bool was_predelete = in_predelete;
				in_predelete = was_predelete || predelete_branch(*r_stmt.condition, true);
				const std::unordered_map<int, Type> then_end = check_block(r_stmt.body, facts.when_true);
				in_predelete = was_predelete || predelete_branch(*r_stmt.condition, false);
				const std::unordered_map<int, Type> else_end = check_block(r_stmt.else_body, facts.when_false);
				in_predelete = was_predelete;
				// Where one branch always leaves, what holds at the end of the
				// other, which may have assigned what the condition tested.
				if (always_ends(r_stmt.body, true)) {
					add_facts(else_end);
				}
				if (always_ends(r_stmt.else_body, true)) {
					add_facts(then_end);
				}
				return;
			}
			case StmtKind::While: {
				forget_assigned(r_stmt.body);
				expect_condition(*r_stmt.condition);
				loop_depth++;
				check_block(r_stmt.body, facts_of(*r_stmt.condition).when_true);
				loop_depth--;
				return;
			}
			case StmtKind::For:
				forget_assigned(r_stmt.body);
				check_for(r_stmt);
				return;
			case StmtKind::Match:
				check_match(r_stmt);
				return;
			case StmtKind::Return: {
				if (returns) {
					// The lambda's return type is what it returns (spec §4.6).
					if (!r_stmt.value) {
						returns->bare = true;
						return;
					}
					Type value = check_value(*r_stmt.value);
					if (value.kind == TypeKind::None) {
						error(r_stmt.value->range, function_label() + " needs a return type: None does not say which T? it returns.");
						value = Type::of(TypeKind::Error);
					}
					if (!returns->type || value.is_error() || returns->type->is_error()) {
						returns->type = returns->type ? Type::of(TypeKind::Error) : value;
						return;
					}
					const Type common = common_type(*returns->type, value);
					if (common.is_error()) {
						error(r_stmt.value->range, function_label() + " returns " + returns->type->to_string() + " and " + value.to_string() + "; declare its return type.");
					}
					returns->type = common;
					return;
				}
				const Type expected = function->return_type;
				if (!r_stmt.value) {
					if (expected.kind != TypeKind::Void && !expected.is_error()) {
						error(r_stmt.range, function_label() + " must return a value of type " + expected.to_string() + ".");
					}
					return;
				}
				const Type value = check_value(*r_stmt.value, expected);
				if (expected.kind == TypeKind::Void) {
					error(r_stmt.value->range, function_label() + " returns void. Declare its return type, for example -> " + (value.is_error() ? std::string("int") : value.to_string()) + ".");
					return;
				}
				expect_assignable(*r_stmt.value, value, expected, "return");
				return;
			}
			case StmtKind::Pass:
				return;
			case StmtKind::Error:
				check_block(r_stmt.body);
				return;
			case StmtKind::Break:
			case StmtKind::Continue:
				if (loop_depth == 0) {
					error(r_stmt.range, std::string(r_stmt.kind == StmtKind::Break ? "break" : "continue") + " is only valid in a loop.");
				}
				return;
			case StmtKind::Expression:
				if (is_comment(*r_stmt.value)) {
					return;
				}
				if (r_stmt.value->kind != ExprKind::Call && r_stmt.value->kind != ExprKind::Await) {
					check_expr(*r_stmt.value);
					error(r_stmt.value->range, "This expression has no effect.");
					return;
				}
				r_stmt.value->discarded = true;
				if (check_expr(*r_stmt.value).kind == TypeKind::Task) {
					error(r_stmt.range, "An async call must be awaited, stored or explicitly detached with detach().");
				}
				return;
		}
	}

	/* Declarations */

	void declare_members() {
		for (size_t i = 0; i < cls.members.size(); i++) {
			VarDecl &member = cls.members[i];
			if (member_index.count(member.name) || function_index.count(member.name) || signal_index.count(member.name)) {
				error(member.name_token, "The member \"" + member.name + "\" is already declared.");
				continue;
			}
			member_index[member.name] = int(cls.all_members.size());
			cls.all_members.push_back(&member);
			refer(member.name_token, script_id(options.path, member.name), true);
		}
	}

	/* Annotations (spec §2.3) */

	// A number of @export_range as GDScript writes it into the hint string,
	// having converted it to float (modules/gdscript/gdscript_analyzer.cpp:1700-1722,
	// core/variant/variant.cpp:1605-1606): 0.0, -5.0, 0.1.
	static std::string number_text(const ConstValue &p_value) {
		std::ostringstream stream;
		stream.imbue(std::locale::classic());
		stream << std::setprecision(15) << (p_value.kind == ConstValue::Kind::Int ? double(p_value.i) : p_value.f);
		std::string text = stream.str();
		if (text.find_first_of(".einf") == std::string::npos) {
			text += ".0";
		}
		return text;
	}

	// An argument of an annotation, which the compiler must know (spec §2.3,
	// §4.5); null after reporting.
	const ConstValue *annotation_argument(Expr &r_arg) {
		const bool was_in_constant = in_constant;
		in_constant = true;
		const Type type = check_value(r_arg);
		in_constant = was_in_constant;
		if (type.is_error()) {
			return nullptr;
		}
		if (!r_arg.constant) {
			require_constant(r_arg);
			return nullptr;
		}
		if (!r_arg.constant->is_known()) {
			error(r_arg.range, "An annotation argument must be known at compile time, and this one is computed when the class loads (spec §4.5).");
			return nullptr;
		}
		return r_arg.constant.get();
	}

	// What an annotation of the @export family that gives a hint applies to.
	enum class HintTarget {
		Number,
		Int, // GDScript also lets a float take it (modules/gdscript/gdscript_parser.cpp:4970-4971).
		String,
		IntOrString,
		StringOrDictionary,
		Color,
		NodePath,
	};

	struct HintAnnotation {
		const char *name;
		uint32_t hint; // PropertyHint as the engine numbers it (core/object/property_info.h:39-85).
		HintTarget target;
		uint32_t least; // Arguments.
		uint32_t most;
	};

	// As GDScript registers them (modules/gdscript/gdscript_parser.cpp:153-173).
	static const HintAnnotation *hint_annotation(const std::string &p_name) {
		constexpr uint32_t ANY = UINT32_MAX;
		static const HintAnnotation annotations[] = {
			{ "export_enum", 2, HintTarget::IntOrString, 1, ANY },
			{ "export_file", 13, HintTarget::String, 0, ANY },
			{ "export_file_path", 44, HintTarget::String, 0, ANY },
			{ "export_dir", 14, HintTarget::String, 0, 0 },
			{ "export_global_file", 15, HintTarget::String, 0, ANY },
			{ "export_global_dir", 16, HintTarget::String, 0, 0 },
			{ "export_multiline", 18, HintTarget::StringOrDictionary, 0, ANY },
			{ "export_placeholder", 20, HintTarget::String, 1, 1 },
			{ "export_range", 1, HintTarget::Number, 2, ANY },
			{ "export_exp_easing", 4, HintTarget::Number, 0, ANY },
			{ "export_color_no_alpha", 21, HintTarget::Color, 0, 0 },
			{ "export_node_path", 26, HintTarget::NodePath, 0, ANY },
			{ "export_flags", 6, HintTarget::Int, 1, ANY },
			{ "export_flags_2d_render", 7, HintTarget::Int, 0, 0 },
			{ "export_flags_2d_physics", 8, HintTarget::Int, 0, 0 },
			{ "export_flags_2d_navigation", 9, HintTarget::Int, 0, 0 },
			{ "export_flags_3d_render", 10, HintTarget::Int, 0, 0 },
			{ "export_flags_3d_physics", 11, HintTarget::Int, 0, 0 },
			{ "export_flags_3d_navigation", 12, HintTarget::Int, 0, 0 },
			{ "export_flags_avoidance", 37, HintTarget::Int, 0, 0 },
		};
		for (const HintAnnotation &annotation : annotations) {
			if (p_name == annotation.name) {
				return &annotation;
			}
		}
		return nullptr;
	}

	static bool fits(HintTarget p_target, const Type &p_type) {
		switch (p_target) {
			case HintTarget::Number:
			case HintTarget::Int:
				return p_type.is_numeric();
			case HintTarget::String:
				return p_type.kind == TypeKind::String;
			case HintTarget::IntOrString:
				return p_type.kind == TypeKind::Int || p_type.kind == TypeKind::String;
			case HintTarget::StringOrDictionary:
				return p_type.kind == TypeKind::String || p_type == Type::builtin("Dictionary");
			case HintTarget::Color:
				return p_type.builtin_name() == "Color";
			case HintTarget::NodePath:
				return p_type.builtin_name() == "NodePath";
		}
		return false;
	}

	static const char *target_text(HintTarget p_target) {
		switch (p_target) {
			case HintTarget::Number:
			case HintTarget::Int:
				return "int and float";
			case HintTarget::String:
				return "String";
			case HintTarget::IntOrString:
				return "int and String";
			case HintTarget::StringOrDictionary:
				return "String and Dictionary";
			case HintTarget::Color:
				return "Color";
			case HintTarget::NodePath:
				return "NodePath";
		}
		return "";
	}

	// An annotation that gives the inspector a hint, its arguments joined by
	// commas into the hint string, each a String, or for @export_range numbers
	// first. On an Array[T] or a packed array it applies to the elements, and
	// the hint string says so (modules/gdscript/gdscript_parser.cpp:4679-4739,
	// 4752-4761, 4979-4989).
	void check_hint(VarDecl &r_member, const Annotation &p_annotation, const HintAnnotation &p_hint) {
		const std::string name = "@" + p_annotation.name;
		const std::optional<Type> element = element_of(r_member.type);
		const bool of_elements = element && element->kind != TypeKind::Variant;
		const Type &target = of_elements ? *element : r_member.type;
		if (!fits(p_hint.target, target)) {
			error(p_annotation.range, name + " applies to " + target_text(p_hint.target) + ", not " + r_member.type.to_string() + ".");
			return;
		}
		const size_t count = p_annotation.args.size();
		if (count < p_hint.least || count > p_hint.most) {
			if (p_hint.target == HintTarget::Number && p_hint.hint == 1) {
				error(p_annotation.range, "@export_range takes a minimum, a maximum and an optional step.");
			} else if (p_hint.least == 1 && p_hint.most == UINT32_MAX) {
				error(p_annotation.range, name + (p_hint.hint == 2 ? " takes the names of the values." : " takes at least one argument."));
			} else {
				error(p_annotation.range, name + (p_hint.most == 0 ? " takes no arguments." : " takes one argument."));
			}
			return;
		}
		std::string hint;
		size_t numbers = 0;
		for (size_t i = 0; i < count; i++) {
			const Expr &arg = *p_annotation.args[i];
			const ConstValue *value = annotation_argument(*p_annotation.args[i]);
			if (!value) {
				return;
			}
			const bool number = value->kind == ConstValue::Kind::Int || value->kind == ConstValue::Kind::Float;
			if (p_hint.hint == 1 && (number ? numbers < i : value->kind != ConstValue::Kind::String)) {
				error(arg.range, "@export_range takes numbers, then options such as \"or_greater\".");
				return;
			}
			if (p_hint.hint != 1 && value->kind != ConstValue::Kind::String) {
				error(arg.range, name + " takes strings.");
				return;
			}
			numbers += number ? 1 : 0;
			const std::string text = number ? number_text(*value) : value->text;
			if (p_hint.hint != 20 && (text.empty() || text.find(',') != std::string::npos)) {
				error(arg.range, text.empty() ? name + " takes no empty strings." : "An argument of " + name + " contains a comma; give the parts as separate arguments.");
				return;
			}
			if ((p_hint.hint == 6 && !check_flag(arg, text, i)) || (p_hint.hint == 26 && !check_node_class(arg, text))) {
				return;
			}
			hint += (i ? "," : "") + text;
		}
		if (p_hint.hint == 1 && (numbers < 2 || numbers > 3)) {
			error(p_annotation.range, "@export_range takes a minimum, a maximum and an optional step.");
			return;
		}
		r_member.hint = p_hint.hint;
		r_member.hint_string = hint;
		if (of_elements) {
			// PROPERTY_HINT_TYPE_STRING: "type/hint:hint string" of the elements.
			r_member.hint = 23;
			r_member.hint_string = std::to_string(uint32_t(bytecode::value_type_from_name(element->to_string()))) + "/" + std::to_string(p_hint.hint) + ":" + hint;
		}
	}

	// `"Name"` or `"Name:value"` of @export_flags, whose value is a bit set
	// (modules/gdscript/gdscript_parser.cpp:4695-4719).
	bool check_flag(const Expr &p_arg, const std::string &p_text, size_t p_index) {
		const size_t colon = p_text.find(':');
		if (colon == 0) {
			error(p_arg.range, "A flag of @export_flags needs a name.");
			return false;
		}
		if (colon == std::string::npos) {
			if (p_index >= 32) {
				error(p_arg.range, "From the 33rd flag on, @export_flags needs the value of each, as in \"Name:value\".");
				return false;
			}
			return true;
		}
		const std::string digits = p_text.substr(colon + 1);
		const bool number = !digits.empty() && digits.size() <= 10 && std::all_of(digits.begin(), digits.end(), [](char p_char) { return p_char >= '0' && p_char <= '9'; });
		if (!number || std::stoll(digits) < 1 || std::stoll(digits) >= (int64_t(1) << 32)) {
			error(p_arg.range, "The value of a flag of @export_flags is an integer from 1 to 2 ** 32 - 1.");
			return false;
		}
		return true;
	}

	// A class of @export_node_path, which must be a node
	// (modules/gdscript/gdscript_parser.cpp:4720-4731).
	bool check_node_class(const Expr &p_arg, const std::string &p_class) {
		const std::string path = options.project ? options.project->global_class_path(p_class) : std::string();
		std::string native = p_class;
		if (is_script_path(path)) {
			const ClassDecl *header = script_header(path);
			native = header ? script_class_type(*header, path).engine_class() : std::string();
		} else if (const ForeignClass *foreign = path.empty() ? nullptr : foreign_class(path)) {
			native = foreign->native;
		}
		if (!engine.class_exists(native) || !(native == "Node" || engine.inherits(native, "Node"))) {
			error(p_arg.range, "@export_node_path takes classes of nodes, and " + p_class + " is not one.");
			return false;
		}
		return true;
	}

	// @export_custom(hint, hint_string, usage), which GDScript passes to the
	// engine as written (modules/gdscript/gdscript_parser.cpp:5019-5045).
	void check_export_custom(VarDecl &r_member, const Annotation &p_annotation) {
		if (p_annotation.args.size() < 2 || p_annotation.args.size() > 3) {
			error(p_annotation.range, "@export_custom takes a hint, a hint string and optional usage flags.");
			return;
		}
		std::vector<const ConstValue *> values;
		for (const auto &arg : p_annotation.args) {
			values.push_back(annotation_argument(*arg));
			if (!values.back()) {
				return;
			}
		}
		const bool has_usage = values.size() == 3;
		if (values[0]->kind != ConstValue::Kind::Int || values[1]->kind != ConstValue::Kind::String || (has_usage && values[2]->kind != ConstValue::Kind::Int)) {
			error(p_annotation.range, "@export_custom takes a PropertyHint, a String and PropertyUsageFlags.");
			return;
		}
		r_member.hint = uint32_t(values[0]->i);
		r_member.hint_string = values[1]->text;
		if (has_usage) {
			r_member.usage = uint32_t(values[2]->i);
		}
	}

	// The inspector's view of an export (spec §2.3): @export alone shows it as
	// its type, an annotation of the family adds a hint, @export_storage stores
	// it without showing it. PropertyUsageFlags as the engine numbers them
	// (core/object/property_info.h:90-91, 120).
	void check_annotations(VarDecl &r_member) {
		const Annotation *exported = nullptr;
		if (r_member.is_export) {
			r_member.usage = 6; // PROPERTY_USAGE_DEFAULT
		}
		for (const Annotation &annotation : r_member.annotations) {
			if (annotation.name.rfind("export", 0) != 0) {
				if (!annotation.args.empty()) {
					error(annotation.range, "@" + annotation.name + " takes no arguments.");
				}
				continue;
			}
			if (exported) {
				error(annotation.range, "@" + annotation.name + " cannot be used with another @export annotation.");
				continue;
			}
			exported = &annotation;
			if (const HintAnnotation *hint = hint_annotation(annotation.name)) {
				check_hint(r_member, annotation, *hint);
			} else if (annotation.name == "export_custom") {
				check_export_custom(r_member, annotation);
			} else if (annotation.name == "export" || annotation.name == "export_storage") {
				if (!annotation.args.empty()) {
					error(annotation.range, "@" + annotation.name + " takes no arguments.");
				}
				if (annotation.name == "export_storage") {
					r_member.usage = 2; // PROPERTY_USAGE_STORAGE
				}
			} else if (annotation.name == "export_tool_button") {
				error(annotation.range, "@export_tool_button needs @tool, which is not in the first version (spec §2.3).");
			} else {
				error(annotation.range, "The annotation @" + annotation.name + " is not supported yet.");
			}
		}
	}

	// @rpc(mode, sync, transfer_mode, channel): the strings in any order, the
	// channel fourth, as in GDScript (modules/gdscript/gdscript_parser.cpp:187,
	// 5238-5298). The peers call the method by name (spec §10.5).
	void check_rpc(FuncDecl &r_function, const Annotation &p_annotation) {
		if (r_function.rpc.mode != 0) {
			error(p_annotation.range, "A function takes @rpc once.");
			return;
		}
		if (p_annotation.args.size() > 4) {
			error(p_annotation.range, "@rpc takes at most four arguments.");
			return;
		}
		bytecode::Rpc rpc;
		rpc.mode = 2; // RPC_MODE_AUTHORITY
		int locality = 0;
		int permission = 0;
		int transfer = 0;
		for (size_t i = 0; i < p_annotation.args.size(); i++) {
			const Expr &arg = *p_annotation.args[i];
			const ConstValue *value = annotation_argument(*p_annotation.args[i]);
			if (!value) {
				return;
			}
			if (i == 3) {
				if (value->kind != ConstValue::Kind::Int || value->i < 0 || value->i > INT32_MAX) {
					error(arg.range, "The fourth argument of @rpc is the channel, an int that is not negative.");
					return;
				}
				rpc.channel = int32_t(value->i);
				continue;
			}
			const std::string text = value->kind == ConstValue::Kind::String ? value->text : std::string();
			if (text == "call_local" || text == "call_remote") {
				locality++;
				rpc.call_local = text == "call_local";
			} else if (text == "any_peer" || text == "authority") {
				permission++;
				rpc.mode = text == "any_peer" ? 1 : 2;
			} else if (text == "unreliable" || text == "unreliable_ordered" || text == "reliable") {
				transfer++;
				rpc.transfer_mode = 2; // TRANSFER_MODE_RELIABLE
				if (text != "reliable") {
					rpc.transfer_mode = text == "unreliable" ? 0 : 1;
				}
			} else {
				error(arg.range, "@rpc takes \"call_local\" or \"call_remote\", \"any_peer\" or \"authority\", and \"reliable\", \"unreliable\" or \"unreliable_ordered\", then the channel.");
				return;
			}
		}
		if (locality > 1) {
			error(p_annotation.range, "@rpc takes one of \"call_local\" and \"call_remote\".");
			return;
		}
		if (permission > 1) {
			error(p_annotation.range, "@rpc takes one of \"any_peer\" and \"authority\".");
			return;
		}
		if (transfer > 1) {
			error(p_annotation.range, "@rpc takes one transfer mode.");
			return;
		}
		r_function.rpc = rpc;
		cls.exposed_methods.insert(r_function.name);
	}

	// Their arguments name warnings, as strings; which names suppress anything
	// is for the lints to say (spec §13.2).
	void check_warning_ignores() {
		for (const Annotation &annotation : cls.warning_ignores) {
			if (annotation.args.empty()) {
				error(annotation.range, "@" + annotation.name + " takes the names of warnings.");
			}
			for (const auto &arg : annotation.args) {
				const ConstValue *value = annotation_argument(*arg);
				if (value && value->kind != ConstValue::Kind::String) {
					error(arg->range, "@" + annotation.name + " takes the names of warnings, as strings.");
				}
			}
		}
	}

	// `@export_group(name, prefix)`, `@export_subgroup(name, prefix)`, `@export_category(name)`.
	void check_groups() {
		for (ExportGroupDecl &group : cls.groups) {
			const Annotation &annotation = group.annotation;
			const size_t most = annotation.name == "export_category" ? 1 : 2;
			bool ok = !annotation.args.empty() && annotation.args.size() <= most;
			std::vector<std::string> texts;
			for (const auto &arg : annotation.args) {
				const ConstValue *value = annotation_argument(*arg);
				if (!value) {
					ok = false;
					break;
				}
				ok = ok && value->kind == ConstValue::Kind::String;
				texts.push_back(value->text);
			}
			if (!ok) {
				if (texts.size() == annotation.args.size()) {
					error(annotation.range, "@" + annotation.name + " takes a name" + (most == 2 ? " and an optional prefix" : "") + ", as strings.");
				}
				continue;
			}
			group.name = texts[0];
			group.prefix = texts.size() > 1 ? texts[1] : std::string();
		}
	}

	// After the signatures, since an initializer can call this class's functions.
	// The other members first, then the @onready ones, whose initializers may
	// read any of the others (spec §6.3), so that a member's type is known
	// before an initializer reads it.
	void check_members() {
		cls.is_node = engine.inherits(cls.native_base, "Node");
		for (const bool onready : { false, true }) {
			for (size_t i = 0; i < cls.members.size(); i++) {
				if (cls.members[i].is_onready == onready) {
					check_member_declaration(i);
				}
			}
		}
	}

	void check_member_declaration(size_t p_index) {
		// A name declared twice is reported where it is registered.
		VarDecl &member = cls.members[p_index];
		const auto registered = member_index.find(member.name);
		if (registered == member_index.end() || cls.all_members[registered->second] != &member) {
			return;
		}
		Type type = Type::of(TypeKind::Error);
		if (member.declared_type) {
			type = resolve(*member.declared_type, false);
		}
		if (member.is_onready) {
			if (!cls.is_node) {
				error(member.name_token, "@onready is for Node scripts; a " + cls.extends + " is never ready (spec §6.3).");
			}
			if (member.is_export) {
				error(member.name_token, "@onready and @export do not go together: the node's ready would overwrite what the scene assigned (spec §6.3).");
			}
			if (!member.initializer) {
				error(member.name_token, "An @onready member needs an initializer.");
			}
		}
		if (member.initializer) {
			initializing = registered->second;
			initializing_onready = member.is_onready;
			const Type value = check_value(*member.initializer, type);
			initializing = -1;
			initializing_onready = false;
			if (member.declared_type) {
				expect_assignable(*member.initializer, value, type, "assign");
			} else {
				type = value;
			}
			// An @onready member gets even a constant when the node is ready.
			const ConstPtr initial = converted(member.initializer->constant, type);
			member.constant_initializer = !member.is_onready && initial && !loads(*initial);
		} else if (!member.declared_type) {
			error(member.name_token, "\"" + member.name + "\" needs a type or an initial value.");
		} else if ((type.kind == TypeKind::Object || type.kind == TypeKind::Task) && !type.nullable) {
			// An export that the scene or resource must assign (spec §7.5); a
			// member that the creator or an ancestor must assign (§6.2) is not
			// supported yet.
			member.must_assign = member.is_export && type.kind == TypeKind::Object;
			if (!member.is_export) {
				error(member.name_token, "Members that must be assigned, such as \"" + member.name + "\" of type " + type.to_string() + (type.kind == TypeKind::Task ? ", need an initial value." : ", are not supported yet unless exported; give it an initial value."));
			}
		} else if (type.kind == TypeKind::Struct && !has_default(type)) {
			// Starts at its default only when every field has one (spec §4.3).
			error(member.name_token, "Members that must be assigned, such as \"" + member.name + "\" of type " + type.to_string() + ", are not supported yet; give it an initial value.");
		} else if (!enum_has_default(type)) {
			error(member.name_token, "\"" + member.name + "\" needs an initial value: " + enum_spelling(type) + " has no value 0 to start at (spec §3.7).");
		}
		if (member.is_export && type.unwrapped().kind == TypeKind::Struct) {
			error(member.name_token, "Structs cannot be exported (spec §3.6).");
		} else if (member.is_export && is_typed_container(type) && is_boxed(type)) {
			error(member.name_token, "Containers whose elements exist only in this language, such as " + type.to_string() + ", cannot be exported (spec §3.6, §3.8).");
		}
		if (member.is_export && type.kind == TypeKind::Object && type.name == "PackedScene" && type.args.empty()) {
			error(member.name_token, "An exported scene is written PackedScene[T], with the type of its root (spec §7.4).");
		}
		if (member.is_export && type.kind == TypeKind::Task) {
			error(member.name_token, "Task handles cannot be exported (spec §4.4, §9.4).");
		}
		member.type = type;
		check_annotations(member);
		check_accessors(member);
	}

	// A property's setter and getter (spec §4.4): its own inline ones, which
	// take and give its type, or functions of the class that do.
	void check_accessors(VarDecl &r_member) {
		const auto accessor = [&](const std::string &p_name, bool p_setter) {
			if (p_name.empty()) {
				return -1;
			}
			const std::string kind = p_setter ? "setter" : "getter";
			FuncDecl *function = nullptr;
			for (FuncDecl &own : cls.functions) {
				if (own.name == p_name) {
					function = &own;
				}
			}
			if (!function) {
				error(r_member.accessor_token, "The " + kind + " " + p_name + "() is not a function of this class.");
				return -1;
			}
			if (p_name[0] == '@') {
				if (p_setter) {
					function->params[0].type = r_member.type;
				} else {
					function->return_type = r_member.type;
				}
			} else {
				if (!function->accessor_of.empty()) {
					error(r_member.accessor_token, p_name + "() is already an accessor of \"" + function->accessor_of + "\".");
				}
				function->accessor_of = r_member.name;
				refer(p_setter ? r_member.setter_token : r_member.getter_token, script_id(options.path, p_name));
				const bool fits = p_setter ? function->params.size() == 1 && function->params[0].type == r_member.type && function->return_type.kind == TypeKind::Void : function->params.empty() && function->return_type == r_member.type;
				if (!fits && !r_member.type.is_error()) {
					error(function->name_token, "As the " + kind + " of \"" + r_member.name + "\", " + p_name + "() " + (p_setter ? "takes one argument of type " + r_member.type.to_string() + " and returns nothing" : "takes no argument and returns " + r_member.type.to_string()) + " (spec §4.4).");
				}
			}
			if (function->is_async || function->is_static) {
				error(function->name_token, "A " + kind + " cannot be async or static (spec §4.4).");
			}
			const auto found = function_index.find(p_name);
			return found == function_index.end() ? -1 : found->second;
		};
		r_member.setter_index = accessor(r_member.setter, true);
		r_member.getter_index = accessor(r_member.getter, false);
	}

	// Default parameters use the same constants as member initializers (spec §4.6).
	void check_defaults(FuncDecl &r_function) {
		bool any = false;
		for (ParamDecl &param : r_function.params) {
			if (!param.default_value) {
				if (any) {
					error(param.name_token, "Parameter \"" + param.name + "\" follows one with a default value and needs one too.");
				}
				continue;
			}
			any = true;
			const size_t before = errors.size();
			const Type value = check_value(*param.default_value, param.type);
			expect_assignable(*param.default_value, value, param.type, "use");
			const ConstPtr constant = converted(param.default_value->constant, param.type);
			if (errors.size() == before && (!constant || loads(*constant))) {
				error(param.default_value->range, "Non-constant default values are not supported yet; pass the argument explicitly.");
			}
		}
	}

	static int required_params(const FuncDecl &p_function) {
		int count = 0;
		while (count < int(p_function.params.size()) && !p_function.params[count].default_value) {
			count++;
		}
		return count;
	}

	// A static function has no instance (spec §4.8).
	bool refuse_in_static(const Expr &p_expr, const std::string &p_name) {
		if (!function || !function->is_static) {
			// A lambda that uses self holds it, and so do those it is in (spec §3.11).
			for (size_t i = 0; function && function->is_lambda && i <= enclosing.size(); i++) {
				FuncDecl *lambda = i < enclosing.size() ? enclosing[i].function : function;
				if (lambda && lambda->is_lambda) {
					lambda->uses_self = true;
				}
			}
			return false;
		}
		error(p_expr.range, "\"" + p_name + "\" belongs to an instance, which the static function \"" + outermost()->name + "()\" does not have.");
		return true;
	}

	// The function being checked, as messages name it.
	std::string function_label() const {
		if (!function->is_lambda) {
			return "\"" + function->name + "()\"";
		}
		return function->name.empty() ? std::string("The lambda") : "The lambda \"" + function->name + "\"";
	}

	const FuncDecl *base_function(const std::string &p_name) const {
		if (cls.base) {
			for (const FuncDecl *fn : cls.base->all_functions) {
				if (fn->name == p_name) {
					return fn;
				}
			}
		}
		return nullptr;
	}

	bool override_assignable(const Type &p_from, const Type &p_to) const {
		if (p_from == p_to || p_from.is_error() || p_to.is_error()) {
			return true;
		}
		if (p_to.kind == TypeKind::Option) {
			return p_from.kind == TypeKind::Option ? p_from.args == p_to.args : override_assignable(p_from, p_to.args[0]);
		}
		if (p_from.kind == TypeKind::Object && p_to.kind == TypeKind::Object) {
			return assignable(p_from, p_to);
		}
		if (p_from.kind == TypeKind::Function && p_to.kind == TypeKind::Function) {
			return assignable(p_from, p_to);
		}
		if (p_from.kind == TypeKind::Task && p_to.kind == TypeKind::Task) {
			return assignable(p_from, p_to);
		}
		return p_from.kind == TypeKind::Builtin && p_to.kind == TypeKind::Builtin && p_from.name == "Array" && p_to.name == "Array" && p_to.args.empty();
	}

	void check_script_override(FuncDecl &r_fn, const FuncDecl &p_base) {
		r_fn.is_override = p_base.is_override;
		if (r_fn.is_pub != p_base.is_pub) {
			error(r_fn.name_token, "An override must preserve its base method's visibility (spec §4.2).");
		}
		if (r_fn.is_static != p_base.is_static || r_fn.is_async != p_base.is_async) {
			error(r_fn.name_token, "An override must preserve static and async (spec §4.6).");
		}
		for (size_t i = 0; i < r_fn.params.size(); ++i) {
			ParamDecl &param = r_fn.params[i];
			if (param.declared_type) {
				param.type = resolve(*param.declared_type, false);
			} else if (i < p_base.params.size()) {
				param.type = p_base.params[i].type;
			} else {
				error(param.name_token, "Parameter \"" + param.name + "\" needs a type.");
			}
			if (i < p_base.params.size() && !override_assignable(p_base.params[i].type, param.type)) {
				error(param.name_token, "The override narrows its base parameter type (spec §4.6).");
			}
		}
		check_defaults(r_fn);
		if (r_fn.params.size() < p_base.params.size() || required_params(r_fn) > required_params(p_base)) {
			error(r_fn.name_token, "The override must accept every argument count of its base method (spec §4.6).");
		}
		r_fn.return_type = r_fn.return_type_ref ? resolve(*r_fn.return_type_ref, true) : p_base.return_type;
		if (!override_assignable(r_fn.return_type, p_base.return_type)) {
			error(r_fn.name_token, "The override widens its base return type (spec §4.6).");
		}
		if (r_fn.name == "_init" && (!r_fn.params.empty() || r_fn.is_async || r_fn.return_type.kind != TypeKind::Void)) {
			error(r_fn.name_token, "_init() must be synchronous, return void and take no parameters for now.");
		}
	}

	// Its declaration, with what it overrides; an inline accessor has no name
	// of its own.
	void declare_function(const FuncDecl &p_function, const std::optional<DeclId> &p_overrides) {
		if (p_function.name.rfind('@', 0) != 0) {
			refer(p_function.name_token, script_id(options.path, p_function.name), true, p_overrides);
		}
	}

	// @abstract as GDScript checks it (modules/gdscript/gdscript_analyzer.cpp:1532-1556,
	// 2008-2022; modules/gdscript/gdscript_parser.cpp:4513-4516).
	void check_abstract(const FuncDecl &p_function) {
		if (p_function.is_abstract && p_function.is_static) {
			error(p_function.name_token, "A static function cannot be @abstract.");
		} else if (p_function.is_abstract && p_function.has_body) {
			error(p_function.name_token, "An abstract function has no body; a class that extends this one gives it one.");
		} else if (p_function.is_abstract && !cls.is_abstract) {
			error(p_function.name_token, display_name(cls, options.path) + " is not abstract but declares the abstract \"" + p_function.name + "()\"; mark the class @abstract.");
		}
	}

	// A class that is not abstract implements every abstract method it inherits.
	void check_implemented() {
		if (cls.is_abstract || !cls.base) {
			return;
		}
		for (const FuncDecl *function : cls.all_functions) {
			if (function->is_abstract && std::none_of(cls.functions.begin(), cls.functions.end(), [&](const FuncDecl &p_own) { return &p_own == function; })) {
				const ClassDecl &owner = declaring(*cls.base, *function, &ClassDecl::functions);
				error(cls.extends_token, display_name(cls, options.path) + " must implement \"" + display_name(owner, owner.path) + "." + function->name + "()\", and the other abstract methods it inherits, or be marked @abstract.");
				return;
			}
		}
	}

	void check_signature(FuncDecl &r_function) {
		check_abstract(r_function);
		if (const FuncDecl *parent = base_function(r_function.name)) {
			declare_function(r_function, script_id(declaring(*cls.base, *parent, &ClassDecl::functions).path, parent->name));
			check_script_override(r_function, *parent);
			return;
		}
		const auto engine_method = engine.find_method(cls.native_base, r_function.name);
		declare_function(r_function, engine_method && engine_method->is_virtual ? std::optional<DeclId>(engine_id(engine_method->owner, r_function.name)) : std::nullopt);
		if (engine_method && !engine_method->is_virtual) {
			error(r_function.name_token, "\"" + r_function.name + "()\" is a method of " + cls.extends + " that cannot be overridden.");
		}
		r_function.is_override = engine_method && engine_method->is_virtual;
		if (r_function.is_async) {
			if (r_function.name == "_init") {
				error(r_function.name_token, "_init() cannot be async (spec §9.1).");
			} else if (r_function.is_override && !engine_method->return_type.is_error() && engine_method->return_type.kind != TypeKind::Void) {
				error(r_function.name_token, "An engine virtual method returning " + engine_method->return_type.to_string() + " cannot be async (spec §9.1).");
			}
		}

		if (r_function.is_override) {
			if (r_function.params.size() != engine_method->params.size()) {
				error(r_function.name_token, "\"" + r_function.name + "()\" overrides an engine method with " + std::to_string(engine_method->params.size()) + " parameter(s).");
			}
		}
		for (size_t i = 0; i < r_function.params.size(); i++) {
			ParamDecl &param = r_function.params[i];
			Type engine_type = Type::of(TypeKind::Error);
			if (r_function.is_override && i < engine_method->params.size()) {
				engine_type = engine_method->params[i].type;
				// The engine passes its own objects to virtual methods.
				engine_type.nullable = false;
				if (!engine_type.is_storable() || engine_type.kind == TypeKind::Variant) {
					error(param.name_token, "Parameters of type " + engine_type.to_string() + " are not supported yet.");
					engine_type = Type::of(TypeKind::Error);
				}
			}
			if (param.declared_type) {
				param.type = resolve(*param.declared_type, false);
				// A parameter may accept more than the overridden one does, such as
				// T? for T (spec §4.6).
				if (r_function.is_override && !engine_type.is_error() && !param.type.is_error() && !assignable(engine_type, param.type)) {
					error(param.declared_type->range, "The overridden method declares this parameter as " + engine_type.to_string() + ".");
				}
			} else if (r_function.is_override) {
				// The type comes from the overridden declaration (spec §4.6).
				param.type = engine_type;
			} else if (!r_function.accessor_of.empty()) {
				// An inline setter takes the member's type (check_accessors()).
			} else {
				error(param.name_token, "Parameter \"" + param.name + "\" needs a type.");
				param.type = Type::of(TypeKind::Error);
			}
		}
		check_defaults(r_function);
		if (r_function.is_static && r_function.is_override) {
			error(r_function.name_token, "\"" + r_function.name + "()\" overrides an engine method and cannot be static.");
		}

		// The engine constructs Node and Resource scripts with no arguments
		// (spec §6.2); RefCounted constructors with arguments come with new(...).
		if (r_function.name == "_init" && !r_function.params.empty()) {
			const bool engine_made = engine.inherits(cls.native_base, "Node") || engine.inherits(cls.native_base, "Resource");
			error(r_function.name_token, engine_made ? "_init() of a " + cls.extends + " script cannot take parameters: the engine constructs it with none. Use members that must be assigned instead (spec §6.2)." : "_init() with parameters is not supported yet.");
		}
		if (r_function.name == "_init" && r_function.return_type_ref && r_function.return_type_ref->name != "void") {
			error(r_function.return_type_ref->range, "_init() returns nothing.");
		}

		if (r_function.return_type_ref) {
			r_function.return_type = resolve(*r_function.return_type_ref, true);
			if (r_function.is_override && !engine_method->return_type.is_error() && r_function.return_type != engine_method->return_type) {
				error(r_function.return_type_ref->range, "The overridden method returns " + engine_method->return_type.to_string() + ".");
			}
		} else if (r_function.is_override) {
			r_function.return_type = engine_method->return_type;
			if (r_function.return_type.kind != TypeKind::Void && (!r_function.return_type.is_storable() || r_function.return_type.kind == TypeKind::Variant)) {
				error(r_function.name_token, "Return values of type " + r_function.return_type.to_string() + " are not supported yet.");
				r_function.return_type = Type::of(TypeKind::Error);
			}
		} else {
			r_function.return_type = Type::of(TypeKind::Void);
		}
	}

	void declare_params(FuncDecl &r_function) {
		for (const ParamDecl &param : r_function.params) {
			if (scopes.back().count(param.name)) {
				error(param.name_token, "Parameter \"" + param.name + "\" is declared twice.");
			}
			scopes.back()[param.name] = int(r_function.locals.size());
			r_function.locals.push_back({ param.name, param.type, param.name_token });
			refer(param.name_token, local_id(param.name_token), true);
		}
	}

	// Connections saved in scenes to methods of this class (spec §7.7): the
	// method must exist and take what the signal passes; the engine finds it
	// by name (§10.5).
	void check_connections() {
		const TokenRange header{ cls.extends_token, cls.extends_token };
		for (const Scene &scene : scenes) {
			for (const SceneConnection &connection : scene.connections) {
				const SceneNode *target = scene.find_node(connection.target);
				if (!target || target->script != options.path) {
					continue;
				}
				const std::string where = scene.path + ": the signal \"" + connection.signal + "\" of " + connection.source + " is connected to " + connection.method + "()";
				const auto found = function_index.find(connection.method);
				if (found == function_index.end()) {
					error(cls.extends_token, where + ", which this class does not declare.");
					continue;
				}
				const FuncDecl &method = *cls.all_functions[found->second];
				const bool own = is_own(method, cls.functions);
				cls.exposed_methods.insert(method.name);
				if (references) {
					Reference reference;
					reference.target = script_id(declaring(cls, method, &ClassDecl::functions).path, method.name);
					reference.scene = scene.path;
					reference.connection = connection;
					references->push_back(std::move(reference));
				}
				const SceneNode *source = scene.find_node(connection.source);
				bool missing = false;
				const std::optional<std::vector<Type>> arguments = source ? signal_arguments(*source, connection.signal, missing) : std::nullopt;
				if (missing) {
					error(cls.extends_token, where + ", but " + connection.source + " has no such signal.");
				}
				if (!arguments) {
					continue;
				}
				const int passed = int(arguments->size()) - connection.unbinds + connection.binds;
				if (int(method.params.size()) != passed) {
					error(own_name(method, cls.functions, header), where + ", which passes " + std::to_string(passed) + " argument(s); " + method.name + "() takes " + std::to_string(method.params.size()) + ".");
					continue;
				}
				for (int i = 0; i < int(arguments->size()) - connection.unbinds; i++) {
					if (!assignable((*arguments)[i], method.params[i].type)) {
						error(own ? TokenRange{ method.params[i].name_token, method.params[i].name_token } : header, where + ", which passes " + (*arguments)[i].to_string() + " here.");
					}
				}
			}
		}
	}

	// What a signal of a node in a scene passes: one its script of this
	// language declares (spec §4.7), or one of its engine class. nullopt when
	// that is not known: a script of another language, or one with errors;
	// `r_missing` when the node certainly has no such signal.
	std::optional<std::vector<Type>> signal_arguments(const SceneNode &p_node, const std::string &p_signal, bool &r_missing) {
		const std::string &script = p_node.script.empty() ? p_node.instance_script : p_node.script;
		if (!script.empty() && !is_script_path(script)) {
			return std::nullopt;
		}
		if (!script.empty()) {
			const ClassDecl *decl = script_decl(script);
			if (!decl) {
				return std::nullopt;
			}
			for (const SignalDecl *entry : decl->all_signals) {
				const SignalDecl &signal = *entry;
				if (signal.name == p_signal) {
					return param_types(signal.params);
				}
			}
		}
		const auto signal = engine.find_signal(p_node.type, p_signal);
		if (!signal) {
			r_missing = engine.class_exists(p_node.type);
			return std::nullopt;
		}
		return signal_type(*signal).args;
	}

	// The signatures of the interfaces the class declares (spec §3.9).
	void declare_interfaces() {
		std::unordered_set<std::string> names;
		for (InterfaceDecl &interface : cls.interfaces) {
			if (!names.insert(interface.name).second) {
				error(interface.name_token, "The interface \"" + interface.name + "\" is already declared.");
			}
			refer(interface.name_token, script_id(options.path, interface.name), true);
			for (FuncDecl &method : interface.methods) {
				refer(method.name_token, script_id(options.path, interface.name + "." + method.name), true);
				for (ParamDecl &param : method.params) {
					if (!param.declared_type) {
						error(param.name_token, "Parameter \"" + param.name + "\" needs a type.");
						param.type = Type::of(TypeKind::Error);
						continue;
					}
					param.type = resolve(*param.declared_type, false);
				}
				method.return_type = method.return_type_ref ? resolve(*method.return_type_ref, true) : Type::of(TypeKind::Void);
			}
			std::unordered_set<std::string> members;
			for (const FuncDecl &method : interface.methods) {
				members.insert(method.name);
			}
			for (InterfaceProperty &property : interface.properties) {
				if (!members.insert(property.name).second) {
					error(property.name_token, "\"" + property.name + "\" is already declared in " + interface.name + ".");
				}
				refer(property.name_token, script_id(options.path, interface.name + "." + property.name), true);
				property.type = resolve(property.type_ref, false);
			}
		}
	}

	// The fields of the structs the class declares (spec §3.6), whose defaults
	// are constants for now, as default parameters are.
	void declare_structs() {
		std::unordered_set<std::string> names;
		for (const InterfaceDecl &interface : cls.interfaces) {
			names.insert(interface.name);
		}
		for (StructDecl &decl : cls.structs) {
			if (!names.insert(decl.name).second || is_type_name(decl.name) || (options.project && !options.project->global_class_path(decl.name).empty())) {
				error(decl.name_token, "\"" + decl.name + "\" is already declared.");
			}
			refer(decl.name_token, script_id(options.path, decl.name), true);
			std::unordered_set<std::string> fields;
			for (VarDecl &field : decl.fields) {
				refer(field.name_token, script_id(options.path, decl.name + "." + field.name), true);
				if (!fields.insert(field.name).second) {
					error(field.name_token, "The field \"" + field.name + "\" is already declared in " + decl.name + ".");
				}
				Type type = field.declared_type ? resolve(*field.declared_type, false) : Type::of(TypeKind::Error);
				if (field.initializer) {
					const size_t before = errors.size();
					const Type value = check_value(*field.initializer, type);
					if (field.declared_type) {
						expect_assignable(*field.initializer, value, type, "assign");
					} else if (value.kind == TypeKind::None) {
						error(field.name_token, "\"" + field.name + "\" needs a type: None does not say which T? it is (spec §4.3).");
					} else {
						type = value;
					}
					const ConstPtr constant = converted(field.initializer->constant, type);
					if (errors.size() == before && (!constant || loads(*constant))) {
						error(field.initializer->range, "Non-constant default values are not supported yet.");
					}
				} else if (!field.declared_type) {
					error(field.name_token, "\"" + field.name + "\" needs a type or a default value.");
				}
				field.type = type;
			}
		}
		for (const StructDecl &decl : cls.structs) {
			std::vector<std::string> cycle;
			std::unordered_set<std::string> seen;
			if (holds(cls, options.path, decl, options.path + "::" + decl.name, cycle, seen)) {
				std::string fields;
				for (const std::string &field : cycle) {
					fields += (fields.empty() ? "" : ", ") + field;
				}
				error(decl.name_token, decl.name + " contains itself through " + fields + ", so it has no finite size; hold it in a container or a RefCounted class (spec §3.6).");
			}
		}
	}

	// Whether the struct holds the struct `p_target` ("script::Name") in its
	// slot, directly or through other structs, whose fields are read as
	// declared, so that this does not wait for their scripts to be checked;
	// `r_cycle` lists the fields on the way.
	bool holds(const ClassDecl &p_header, const std::string &p_path, const StructDecl &p_struct, const std::string &p_target, std::vector<std::string> &r_cycle, std::unordered_set<std::string> &r_seen) {
		if (!r_seen.insert(p_path + "::" + p_struct.name).second) {
			return false;
		}
		for (const VarDecl &field : p_struct.fields) {
			// T and T? hold a T; a container holds a reference.
			if (!field.declared_type || !field.declared_type->args.empty()) {
				continue;
			}
			const std::string &name = field.declared_type->name;
			const size_t dot = name.find('.');
			std::string path = p_path;
			const ClassDecl *header = &p_header;
			if (dot != std::string::npos) {
				const std::string head = name.substr(0, dot);
				path = head == p_header.class_name ? p_path : (options.project ? options.project->global_class_path(head) : std::string());
				header = is_script_path(path) ? script_header(path) : nullptr;
			}
			const StructDecl *inner = header ? header->find_struct(name.substr(dot == std::string::npos ? 0 : dot + 1)) : nullptr;
			if (!inner) {
				continue;
			}
			r_cycle.push_back(p_struct.name + "." + field.name);
			if (path + "::" + inner->name == p_target || holds(*header, path, *inner, p_target, r_cycle, r_seen)) {
				return true;
			}
			r_cycle.pop_back();
		}
		return false;
	}

	// `implements I`: the class has every method of I, pub and with the same
	// signature (spec §3.9).
	void check_implements() {
		for (const TypeRef &ref : cls.implements) {
			const Type type = resolve(ref, false);
			if (type.is_error()) {
				continue;
			}
			if (!type.is_interface) {
				error(ref.range, type.to_string() + " is not an interface.");
				continue;
			}
			cls.implemented.push_back(type);
			own_interfaces.push_back({ type, &ref });
			const InterfaceDecl *interface = interface_decl(type);
			if (!interface) {
				error(ref.range, type.script + " has errors.");
				continue;
			}
			for (const FuncDecl &method : interface->methods) {
				const auto found = function_index.find(method.name);
				if (found == function_index.end()) {
					error(ref.range, "The class does not implement " + method.name + "() of " + type.name + ".");
					continue;
				}
				const FuncDecl &function = *cls.all_functions[found->second];
				if (is_own(function, cls.functions)) {
					refer(function.name_token, script_id(options.path, function.name), true, script_id(type.script, interface->name + "." + method.name));
				}
				if (!function.is_pub) {
					error(own_name(function, cls.functions, ref.range), "\"" + function.name + "()\" implements " + type.name + " and must be pub (spec §3.9).");
				}
				bool same = function.params.size() == method.params.size() && function.return_type == method.return_type && function.is_async == method.is_async;
				for (size_t i = 0; same && i < method.params.size(); i++) {
					same = function.params[i].type == method.params[i].type;
				}
				if (!same) {
					error(own_name(function, cls.functions, ref.range), "\"" + function.name + "()\" does not have the signature that " + type.name + " declares, " + function_type(method).to_string() + ".");
				}
			}
		}
	}

	// @owner(T): a class of this language, an interface or an engine class, on
	// a class that extends Node (spec §7.3).
	void declare_owner() {
		if (!cls.owner_ref) {
			return;
		}
		const TypeRef &ref = *cls.owner_ref;
		cls.owner_type = resolve(ref, false);
		if (cls.owner_type.is_error()) {
			return;
		}
		if (cls.owner_type.kind != TypeKind::Object || cls.owner_type.nullable) {
			error(ref.range, "@owner takes a class or an interface, not " + cls.owner_type.to_string() + ".");
			cls.owner_type = Type::of(TypeKind::Error);
		} else if (!engine.inherits(cls.native_base, "Node")) {
			error(ref.range, "@owner is for classes that extend Node.");
			cls.owner_type = Type::of(TypeKind::Error);
		}
	}

	// Where scenes place the script, its owner satisfies @owner(T) (spec §7.3):
	// that of a node is the root of its scene, and that of a scene's root the
	// root of each scene that instances it.
	void check_owner_scenes() {
		if (!cls.owner_ref || cls.owner_type.is_error()) {
			return;
		}
		Expr at;
		at.range = cls.owner_ref->range;
		const auto check_owner = [&](const Scene &p_scene, const SceneNode &p_node) {
			const SceneNode *root = p_scene.find_node(".");
			if (p_node.path == "." || !root) {
				return;
			}
			const std::string &script = root->script.empty() ? root->instance_script : root->script;
			if (!script.empty() && !is_script_path(script) && !cls.owner_type.is_interface && !cls.owner_type.script.empty()) {
				return; // A class of another language, which the reads check.
			}
			const Type owner = scene_node_type(at, *root, p_scene.path + ": the root");
			if (!owner.is_error() && !assignable(owner, cls.owner_type)) {
				error(at.range, p_scene.path + ": the owner of " + p_node.path + " is " + owner.to_string() + ", not " + cls.owner_type.to_string() + " (@owner).");
			}
		};
		for (const Scene &scene : scenes) {
			for (const SceneNode &node : scene.nodes) {
				if (extends_script(node.script, options.path)) {
					check_owner(scene, node);
				}
			}
		}
		if (!options.project || options.declarations_only || options.path.empty()) {
			return;
		}
		for (const Scene &scene : options.project->scenes_instancing_script(options.path)) {
			for (const SceneNode &node : scene.nodes) {
				if (node.is_instance && node.script.empty() && extends_script(node.instance_script, options.path)) {
					check_owner(scene, node);
				}
			}
		}
	}

	// The properties the class's interfaces require, after the members' types
	// are known: a pub member of the same type, or an inherited engine property,
	// which must have a setter unless the requirement is @readonly (spec §3.9).
	void check_interface_properties() {
		for (const auto &[type, ref] : own_interfaces) {
			const InterfaceDecl *interface = interface_decl(type);
			if (!interface) {
				continue;
			}
			for (const InterfaceProperty &required : interface->properties) {
				if (required.type.is_error()) {
					continue;
				}
				const auto member = member_index.find(required.name);
				if (member != member_index.end()) {
					const VarDecl &provided = *cls.all_members[member->second];
					if (is_own(provided, cls.members)) {
						refer(provided.name_token, script_id(options.path, provided.name), true, script_id(type.script, interface->name + "." + required.name));
					}
					if (!provided.is_pub) {
						error(own_name(provided, cls.members, ref->range), "\"" + provided.name + "\" implements " + type.name + " and must be pub (spec §3.9).");
					} else if (!provided.type.is_error() && provided.type != required.type) {
						error(own_name(provided, cls.members, ref->range), "\"" + provided.name + "\" has type " + provided.type.to_string() + ", but " + type.name + " requires " + required.type.to_string() + ".");
					}
					continue;
				}
				const auto property = engine.find_property(cls.native_base, required.name);
				if (!property) {
					error(ref->range, "The class does not implement the property \"" + required.name + "\" of " + type.name + ".");
				} else if (property->type != required.type) {
					error(ref->range, "The engine property \"" + required.name + "\" has type " + property->type.to_string() + ", but " + type.name + " requires " + required.type.to_string() + ".");
				} else if (!property->writable && !required.is_readonly) {
					error(ref->range, "The engine property \"" + required.name + "\" is read-only, so it satisfies only a @readonly property of " + type.name + ".");
				}
			}
		}
	}

	// The class's signals, whose arguments are typed (spec §4.7).
	void declare_signals() {
		for (size_t i = 0; i < cls.signals.size(); i++) {
			SignalDecl &signal = cls.signals[i];
			if (signal_index.count(signal.name) || member_index.count(signal.name) || constant_index.count(signal.name) || function_index.count(signal.name) || engine.find_signal(cls.native_base, signal.name)) {
				error(signal.name_token, "\"" + signal.name + "\" is already declared.");
				continue;
			}
			signal_index[signal.name] = int(cls.all_signals.size());
			cls.all_signals.push_back(&signal);
			refer(signal.name_token, script_id(options.path, signal.name), true);
			for (ParamDecl &param : signal.params) {
				if (!param.declared_type) {
					error(param.name_token, "The argument \"" + param.name + "\" of a signal needs a type (spec §4.7).");
					param.type = Type::of(TypeKind::Error);
					continue;
				}
				if (param.default_value) {
					error(param.default_value->range, "The arguments of a signal have no default values.");
				}
				param.type = resolve(*param.declared_type, false);
			}
		}
	}

	void check_body(FuncDecl &r_function) {
		function = &r_function;
		scopes.clear();
		scopes.emplace_back();
		narrowed.clear();
		narrowed.emplace_back();
		declare_params(r_function);
		if (options.declarations_only || !check_bodies) {
			function = nullptr;
			scopes.clear();
			return;
		}
		check_block(r_function.body);
		if (r_function.has_body && r_function.return_type.kind != TypeKind::Void && !r_function.return_type.is_error() && !always_ends(r_function.body, false)) {
			error(r_function.name_token, "Not all paths of \"" + r_function.name + "()\" return a value.");
		}
		function = nullptr;
		scopes.clear();
	}

public:
	Checker(ClassDecl &r_class, const EngineApi &p_engine, const CompileOptions &p_options, std::vector<Diagnostic> &r_errors, ScriptClasses &r_classes, bool p_check_bodies, std::vector<Reference> *r_references) :
			cls(r_class), engine(p_engine), options(p_options), errors(r_errors), check_bodies(p_check_bodies), classes(r_classes), references(r_references) {}

	void run() {
		cls.path = options.path;
		cls.native_base = classes.native_base(cls, options.path);
		cls.base_script = classes.base_path(cls, options.path);
		refer(cls.class_name_token, script_id(options.path), true);
		if (!cls.extends_path) {
			refer(cls.extends_token, cls.base_script.empty() ? engine_id(cls.extends) : (is_script_path(cls.base_script) ? script_id(cls.base_script) : foreign_id(cls.base_script, std::string())));
		} else if (cls.extends_token < cls.source->tokens.size()) {
			const Token &token = cls.source->tokens[cls.extends_token];
			if (token.length == cls.extends.size() + 2) {
				refer_bytes(token.offset + 1, token.end() - 1, script_id(cls.base_script));
			}
		}
		if (!cls.base_script.empty()) {
			if (!is_script_path(cls.base_script)) {
				error(cls.extends_token, "Inheritance from a script of another language is not supported (spec §4.1).");
				return;
			}
			std::unordered_set<std::string> seen{ options.path };
			for (std::string path = cls.base_script; !path.empty();) {
				if (!seen.insert(path).second) {
					error(cls.extends_token, "Inheritance cycle involving " + path + ".");
					return;
				}
				const ScriptClass *header = classes.header(path);
				if (!header) {
					break;
				}
				path = classes.base_path(header->decl, path);
			}
			const ScriptClass *parent = classes.declarations(cls.base_script, !options.declarations_only);
			if (!parent) {
				error(cls.extends_token, "Base script " + cls.base_script + " has errors or cannot be read.");
				return;
			}
			cls.base = &parent->decl;
			cls.all_members = cls.base->all_members;
			cls.all_functions = cls.base->all_functions;
			cls.all_signals = cls.base->all_signals;
			cls.implemented = cls.base->implemented;
			cls.exposed_methods = cls.base->exposed_methods;
			for (size_t i = 0; i < cls.all_members.size(); ++i) {
				member_index[cls.all_members[i]->name] = int(i);
			}
			for (size_t i = 0; i < cls.all_functions.size(); ++i) {
				function_index[cls.all_functions[i]->name] = int(i);
			}
			for (size_t i = 0; i < cls.all_signals.size(); ++i) {
				signal_index[cls.all_signals[i]->name] = int(i);
			}
		}
		if (cls.native_base.empty()) {
			error(cls.extends_token, "Unknown base class \"" + cls.extends + "\".");
			return;
		}
		if (options.project && !options.declarations_only && !options.path.empty()) {
			scenes = options.project->scenes_with_script(options.path);
		}
		declare_enums();
		declare_members();
		// Before the types that may name a class through a constant (spec §2.1).
		declare_constants();
		declare_interfaces();
		declare_structs();
		declare_owner();
		std::unordered_set<std::string> own_functions;
		for (FuncDecl &fn : cls.functions) {
			if (!own_functions.insert(fn.name).second || member_index.count(fn.name) || constant_index.count(fn.name) || signal_index.count(fn.name)) {
				error(fn.name_token, "\"" + fn.name + "\" is already declared.");
				continue;
			}
			const auto found = function_index.find(fn.name);
			if (found == function_index.end()) {
				function_index[fn.name] = int(cls.all_functions.size());
				cls.all_functions.push_back(&fn);
			} else {
				cls.all_functions[found->second] = &fn;
			}
			check_signature(fn);
		}
		check_implemented();
		check_implements();
		declare_signals();
		check_enums();
		for (ConstDecl &constant : cls.constants) {
			const auto registered = constant_index.find(constant.name);
			if (registered != constant_index.end() && cls.all_constants[registered->second] == &constant) {
				check_constant(constant);
			}
		}
		check_members();
		check_interface_properties();
		check_groups();
		for (FuncDecl &fn : cls.functions) {
			for (const Annotation &annotation : fn.annotations) {
				check_rpc(fn, annotation);
			}
		}
		check_warning_ignores();
		check_connections();
		check_owner_scenes();
		for (FuncDecl &fn : cls.functions) {
			const auto found = function_index.find(fn.name);
			if (found != function_index.end() && cls.all_functions[found->second] == &fn) {
				check_body(fn);
			}
		}
	}
};

} // namespace

void check(ClassDecl &r_class, const EngineApi &p_engine, const CompileOptions &p_options, std::vector<Diagnostic> &r_errors, ScriptClasses &r_classes, bool p_check_bodies, std::vector<Reference> *r_references) {
	Checker(r_class, p_engine, p_options, r_errors, r_classes, p_check_bodies, r_references).run();
}

} // namespace petrichor::compiler
