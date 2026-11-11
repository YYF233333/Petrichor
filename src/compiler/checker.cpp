#include "compiler/checker.h"

#include "compiler/builtin_api.h"
#include "compiler/navigation.h"
#include "compiler/parser.h"
#include "compiler/script_classes.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <limits>
#include <locale>
#include <map>
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

// The methods of T? (spec §3.4), as Rust's; flatten() only of T??.
const std::unordered_set<std::string> &option_methods() {
	static const std::unordered_set<std::string> methods = { "unwrap", "expect", "unwrap_or", "unwrap_or_else", "map", "and_then", "or", "or_else", "filter", "ok_or", "ok_or_else", "flatten" };
	return methods;
}

// The methods of Result (spec §3.5), as Rust's.
const std::unordered_set<std::string> &result_methods() {
	static const std::unordered_set<std::string> methods = { "unwrap", "expect", "unwrap_or", "unwrap_or_else", "map", "map_err", "and_then", "or_else", "ok", "err" };
	return methods;
}

struct HintAnnotation {
	const char *name;
	uint32_t hint; // PropertyHint as the engine numbers it (core/object/property_info.h:39-85).
	// The types it applies to, joined by " and " as errors name them; null
	// for @export and @export_storage. GDScript also lets a float take a
	// flags hint (modules/gdscript/gdscript_parser.cpp:4970-4971).
	const char *target;
	uint32_t least; // Arguments.
	uint32_t most;
};

// As GDScript registers them (modules/gdscript/gdscript_parser.cpp:153-173).
const HintAnnotation HINT_ANNOTATIONS[] = {
	{ "export", 0, nullptr, 0, 0 },
	{ "export_storage", 0, nullptr, 0, 0 },
	{ "export_enum", 2, "int and String", 1, UINT32_MAX },
	{ "export_file", 13, "String", 0, UINT32_MAX },
	{ "export_file_path", 44, "String", 0, UINT32_MAX },
	{ "export_dir", 14, "String", 0, 0 },
	{ "export_global_file", 15, "String", 0, UINT32_MAX },
	{ "export_global_dir", 16, "String", 0, 0 },
	{ "export_multiline", 18, "String and Dictionary", 0, UINT32_MAX },
	{ "export_placeholder", 20, "String", 1, 1 },
	{ "export_range", 1, "int and float", 2, UINT32_MAX },
	{ "export_exp_easing", 4, "int and float", 0, UINT32_MAX },
	{ "export_color_no_alpha", 21, "Color", 0, 0 },
	{ "export_node_path", 26, "NodePath", 0, UINT32_MAX },
	{ "export_flags", 6, "int and float", 1, UINT32_MAX },
	{ "export_flags_2d_render", 7, "int and float", 0, 0 },
	{ "export_flags_2d_physics", 8, "int and float", 0, 0 },
	{ "export_flags_2d_navigation", 9, "int and float", 0, 0 },
	{ "export_flags_3d_render", 10, "int and float", 0, 0 },
	{ "export_flags_3d_physics", 11, "int and float", 0, 0 },
	{ "export_flags_3d_navigation", 12, "int and float", 0, 0 },
	{ "export_flags_avoidance", 37, "int and float", 0, 0 },
};

// What the script editor puts at the cursor for completion, U+FFFF
// (scene/gui/code_edit.cpp:2390-2400), which reads as part of a name.
constexpr const char *CURSOR = "\xEF\xBF\xBF";

bool at_cursor(const std::string &p_name) {
	return p_name.find(CURSOR) != std::string::npos;
}

// Where the script editor's word at the cursor begins, which an option
// replaces: after a symbol other than "/" (scene/gui/code_edit.cpp:3869-3875,
// core/string/char_utils.h:127-129; ed1daf0).
size_t word_start(const std::string &p_text, size_t p_cursor) {
	const auto symbol = [](char p_char) {
		return p_char != '_' && ((p_char >= '!' && p_char <= '/') || (p_char >= ':' && p_char <= '@') || (p_char >= '[' && p_char <= '`') || (p_char >= '{' && p_char <= '~'));
	};
	while (p_cursor > 0 && (p_text[p_cursor - 1] == '/' || ((unsigned char)p_text[p_cursor - 1] > ' ' && !symbol(p_text[p_cursor - 1])))) {
		p_cursor--;
	}
	return p_cursor;
}

class Checker;

// The classes of one file, each checked by a checker of its own (spec §2.1,
// §4.1): all are declared before any function body is checked, and they reach
// each other's declarations here, also while they are being declared.
struct FileCheck {
	std::vector<std::unique_ptr<Checker>> checkers; // Each class before its inner classes.

	Checker *find(const std::string &p_path) const;
};

class Checker : public ClassRelation {
	ClassDecl &cls;
	const EngineApi &engine;
	const CompileOptions options;
	std::vector<Diagnostic> &errors;
	bool check_bodies;
	ScriptClasses &classes; // The project's other scripts of this language.
	std::vector<Scene> scenes; // The scenes that use this script, or the one whose check this is.
	// A node of a scene as the engine builds the scene (spec §7.1), with those
	// of the scenes it instances.
	struct ViewNode {
		std::string path; // From the root of the scene built.
		SceneNode node; // As the scene that stores it has it, with the changes of the scenes that instance that one.
		std::string scene; // The scene that stores it.
		std::string owner; // The path of its owner; "" for the root.
	};
	std::map<std::string, std::vector<ViewNode>> views; // view_of(), by the scene's path.
	std::map<std::string, std::vector<std::string>> loaded_files; // loaded_with(), as found.
	std::vector<Reference> *references; // Recorded for this file only.
	// What can be written at the cursor (spec §13.2), whose function alone has
	// its body checked; null when not completing.
	Completion *completion;
	// Trying a name where completion asks: calls are checked without their
	// arguments, and give how many the callee takes.
	bool probing = false;
	int probed_params = -1;
	// Where the name of the call whose signature completion shows ends.
	uint32_t hint_at = 0;
	// The expression of the expression statement being checked, and the
	// statement before it in its block, for the keywords there.
	const Expr *statement_value = nullptr;
	const Stmt *previous_statement = nullptr;
	FileCheck *file = nullptr;
	Checker *outer_checker = nullptr; // The checker of the class around this one, if any (spec §4.1).
	enum class State {
		Unchecked,
		Declaring,
		Declared,
	};
	State state = State::Unchecked;
	bool declared = false; // Declared without an error that stops it, so its bodies are checked.

	FuncDecl *function = nullptr;
	std::vector<std::unordered_map<std::string, int>> scopes;
	// Types of locals narrowed by conditions (spec §5.4), innermost last.
	std::vector<std::unordered_map<int, Type>> narrowed;
	int loop_depth = 0;
	bool in_predelete = false;
	bool in_assert = false;
	// The interfaces this class's own `implements` names, with where.
	std::vector<std::pair<Type, const TypeRef *>> own_interfaces;
	// The members that must be assigned which _init assigns on every path
	// that ends, and the class's _ready, walked once every body is checked
	// (spec §6.2).
	std::set<int> init_assigned;
	FuncDecl *ready = nullptr;
	// The member whose initializer is being checked: it and the members after
	// it are not initialized yet (spec §6.1), nor are @onready members unless
	// it is one too (§6.3).
	int initializing = -1;
	bool initializing_onready = false;
	// The static variable whose initializer is being checked, by its index in
	// the class's own: it and those after it are not initialized yet, and
	// there is no instance (spec §4.8).
	int static_initializing = -1;
	// Checking a constant's initializer, which may come before the members' types are known.
	bool in_constant = false;
	// The enum whose values are being computed, whose values before
	// `current_value` its own may name (spec §3.7).
	const EnumDecl *current_enum = nullptr;
	size_t current_value = 0;
	// The struct whose method or field's default value is being checked (spec
	// §3.6), and its type.
	const StructDecl *structure = nullptr;
	Type structure_type;
	const VarDecl *field_default = nullptr;
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
		int static_initializing;
	};
	std::vector<Enclosing> enclosing;
	// The return type of a lambda without one, as its return statements give it.
	struct Returns {
		std::optional<Type> type;
		bool bare = false; // A return without a value.
		// What the function type the lambda goes to returns, if known, which the
		// values it returns take their type from, as Ok(v) and None do.
		Type expected;
		// Returned values that leave part of their type to the others, typed
		// as the lambda's return type once that is known.
		std::vector<Expr *> partial;
	};
	Returns *returns = nullptr;

	std::unordered_map<std::string, int> member_index;
	std::unordered_map<std::string, int> static_index; // In all_static_vars.
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
	void error_in_scene(const TokenRange &p_range, const Scene &p_scene, const std::string &p_node, const std::string &p_message) {
		error(p_range, p_message);
		errors.back().scene = p_scene.path;
		errors.back().node = p_node;
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
	static DeclId autoload_id(const std::string &p_name) {
		return { DeclId::Kind::Autoload, std::string(), p_name };
	}
	// By the name token of its declaration.
	DeclId local_id(uint32_t p_token) const {
		return { DeclId::Kind::Local, options.path, p_token < cls.source->tokens.size() ? std::to_string(cls.source->tokens[p_token].offset) : std::string() };
	}

	// A name of this file, unless the compiler made it or a syntax error left
	// it empty; the entry, if one is recorded.
	Reference *refer(uint32_t p_token, const DeclId &p_target, std::optional<SymbolKind> p_declares = std::nullopt, const std::optional<DeclId> &p_overrides = std::nullopt) {
		if (p_token >= cls.source->tokens.size() || cls.source->tokens[p_token].length == 0) {
			return nullptr;
		}
		Reference *reference = refer_bytes(cls.source->tokens[p_token].offset, cls.source->tokens[p_token].end(), p_target);
		if (reference) {
			reference->declares = p_declares;
			reference->overrides = p_overrides;
		}
		return reference;
	}

	Reference *refer_bytes(uint32_t p_begin, uint32_t p_end, const DeclId &p_target) {
		if (!references) {
			return nullptr;
		}
		Reference reference;
		reference.target = p_target;
		reference.begin = p_begin;
		reference.end = p_end;
		references->push_back(std::move(reference));
		return &references->back();
	}

	void refer_local(uint32_t p_token, const Type &p_type, bool p_constant = false) {
		if (Reference *declared = refer(p_token, local_id(p_token), p_constant ? SymbolKind::LocalConstant : SymbolKind::Local)) {
			declared->type = p_type.is_error() ? std::string() : p_type.to_string();
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
	// One of this file, while it is being declared too.
	const ClassDecl *script_decl(const std::string &p_path) const {
		if (p_path == options.path) {
			return &cls;
		}
		if (Checker *local = file ? file->find(p_path) : nullptr) {
			local->declare();
			return local->state == State::Declared && !local->declared ? nullptr : &local->cls;
		}
		return classes.declarations(p_path);
	}

	// The header of a script of this language, this class's or another's as
	// read; null when it cannot be read.
	const ClassDecl *script_header(const std::string &p_path) const {
		if (p_path == options.path) {
			return &cls;
		}
		if (const Checker *local = file ? file->find(p_path) : nullptr) {
			return &local->cls;
		}
		return classes.header(p_path);
	}

	// The error of using a declaration of another file that has errors, as
	// `p_what`, which other scripts may use only where it has none; nullopt
	// for a declaration without.
	std::optional<std::string> broken(const void *p_decl, const std::string &p_what) const {
		const auto found = classes.errors_in(p_decl);
		if (!found) {
			return std::nullopt;
		}
		return p_what + " has errors (" + found->first + ", line " + std::to_string(found->second) + ").";
	}

	// Why the declaration `p_name` of the class `p_class` cannot be used here:
	// not pub where it is not seen (spec §4.2), named as `p_kind` and its
	// name, or with errors.
	std::optional<std::string> refused(const void *p_decl, bool p_visible, const std::string &p_kind, const std::string &p_class, const std::string &p_name) const {
		if (!p_visible) {
			return p_kind + "\"" + p_name + "\" of " + p_class + " is not pub.";
		}
		return broken(p_decl, p_class + "." + p_name);
	}

	// The message for a name that the class at `p_script` lacks, which a
	// syntax error there or in a class it extends may have lost.
	std::string missing(std::string p_message, const std::string &p_script) const {
		if (!is_script_path(p_script)) {
			return p_message;
		}
		if (const auto broken = classes.syntax_error(p_script)) {
			p_message.pop_back(); // The full stop.
			p_message += "; " + broken->first + " has syntax errors (line " + std::to_string(broken->second) + ").";
		}
		return p_message;
	}

	// The type of the objects of a script of this language.
	Type script_class_type(const ClassDecl &p_header, const std::string &p_path) const {
		return Type::script_class(display_name(p_header, p_path), p_path, classes.native_base(p_header, p_path));
	}

	// The class name used in messages and types: class_name, or the file's
	// name, followed by the names of the inner classes down to it, as
	// Player.Shot.
	std::string display_name(const ClassDecl &p_decl, const std::string &p_path) const {
		const size_t inner = p_path.find("::");
		if (inner == std::string::npos) {
			return p_decl.class_name.empty() ? file_name(p_path) : p_decl.class_name;
		}
		const std::string file_path = p_path.substr(0, inner);
		const ClassDecl *file_decl = script_header(file_path);
		std::string name = file_decl && !file_decl->class_name.empty() ? file_decl->class_name : file_name(file_path);
		for (size_t at = inner; at != std::string::npos;) {
			const size_t next = p_path.find("::", at + 2);
			name += "." + p_path.substr(at + 2, next == std::string::npos ? next : next - at - 2);
			at = next;
		}
		return name;
	}

	// Whether the class at `p_path` is in this file, which sees what it
	// declares without pub (spec §4.2).
	bool in_this_file(const std::string &p_path) const {
		return script_file(p_path) == script_file(options.path);
	}

	// Whether code here sees what the class at `p_path` declares without pub:
	// in its file or a subclass (spec §4.2).
	bool sees(const std::string &p_path) const {
		return in_this_file(p_path) || extends_script(options.path, p_path);
	}

	/* Inner classes (spec §4.1) */

	// The inner class `p_name` that the class at `p_path` declares or inherits;
	// empty when there is none.
	std::string inner_class_path(const std::string &p_path, const std::string &p_name) const {
		std::unordered_set<std::string> seen;
		for (std::string path = p_path; is_script_path(path) && seen.insert(path).second;) {
			const ClassDecl *decl = script_header(path);
			if (!decl) {
				break;
			}
			if (decl->find_class(p_name)) {
				return path + "::" + p_name;
			}
			path = classes.base_path(*decl, path);
		}
		return std::string();
	}

	// Whether this class, a local or the engine class it extends has the name,
	// which the classes around it then do not give (spec §4.1).
	bool declares(const std::string &p_name) const {
		return declares_in_script(p_name) || engine.find_property(cls.native_base, p_name) || engine.find_method(cls.native_base, p_name) || engine.find_signal(cls.native_base, p_name) ||
				engine.find_constant(cls.native_base, p_name) || engine.find_enum(cls.native_base, p_name);
	}

	// Whether this class, a base of this language or a local has the name.
	bool declares_in_script(const std::string &p_name) const {
		if (visible_local(p_name) || member_index.count(p_name) || static_index.count(p_name) || constant_index.count(p_name) || function_index.count(p_name) || signal_index.count(p_name) || cls.find_struct(p_name) ||
				!inner_class_path(options.path, p_name).empty()) {
			return true;
		}
		for (const InterfaceDecl &interface : cls.interfaces) {
			if (interface.name == p_name) {
				return true;
			}
		}
		for (const EnumDecl *decl : cls.all_enums) {
			if (decl->name == p_name || (decl->name.empty() && decl->find(p_name) >= 0)) {
				return true;
			}
		}
		return false;
	}

	// Whether a bare name is declared by this class or one around it, which
	// then comes before the engine's global names (spec §4.1).
	bool declared_in_scope(const std::string &p_name) const {
		return p_name.find('.') == std::string::npos && (declares_in_script(p_name) || class_around(p_name));
	}

	// A name of the engine class this one extends, used bare in an inner class
	// whose classes around it declare it too: GDScript would take theirs, this
	// language the inherited one, so it is an error (spec §4.1).
	bool refuse_ambiguous_engine_name(const std::string &p_name, const TokenRange &p_at) {
		for (Checker *around = outer_checker; around; around = around->outer_checker) {
			if (around->declares_in_script(p_name)) {
				error(p_at, "\"" + p_name + "\" is both a name of " + cls.native_base + ", which " + display_name(cls, options.path) + " extends, and one of " + display_name(around->cls, around->path()) + "; write " + cls.native_base + "." + p_name + " or " + display_name(around->cls, around->path()) + "." + p_name + ".");
				return true;
			}
		}
		return false;
	}

	// The checker of the nearest class around this one that has a name this
	// class does not; null when none has it.
	Checker *class_around(const std::string &p_name) const {
		if (declares(p_name)) {
			return nullptr;
		}
		for (Checker *around = outer_checker; around; around = around->outer_checker) {
			if (around->declares(p_name)) {
				return around;
			}
		}
		return nullptr;
	}

	// An inner class by its bare name: one this class has, or one a class
	// around it has; empty for any other name.
	std::string scope_class(const std::string &p_name) const {
		const std::string own = inner_class_path(options.path, p_name);
		if (!own.empty()) {
			return own;
		}
		const Checker *around = class_around(p_name);
		return around ? inner_class_path(around->path(), p_name) : std::string();
	}

	// The class of this language that a name or a chain of names such as
	// `Outer.Inner` names, found as named_class() finds it but referring to
	// nothing; empty for anything else. The first name of a chain may be an
	// autoload, an object the class is reached through.
	std::string class_path_of(const Expr &p_expr, bool p_object = false) {
		if (p_expr.kind == ExprKind::Member && p_expr.base) {
			const std::string owner = class_path_of(*p_expr.base, true);
			return owner.empty() ? owner : inner_class_path(owner, p_expr.text);
		}
		if (p_expr.kind == ExprKind::Call && !p_expr.base && p_expr.text == "preload") {
			const std::string file = preload_file(preload_path(p_expr));
			return is_script_path(file) ? file : std::string();
		}
		if (p_expr.kind != ExprKind::Name || visible_local(p_expr.text) || member_index.count(p_expr.text) || static_index.count(p_expr.text) || engine.find_property(cls.native_base, p_expr.text)) {
			return std::string();
		}
		if (const ConstPtr alias = class_alias(p_expr.text)) {
			return is_script_path(alias->script) ? alias->script : std::string();
		}
		if (constant_index.count(p_expr.text)) {
			return std::string();
		}
		std::string path = scope_class(p_expr.text);
		if (path.empty() && !cls.class_name.empty() && p_expr.text == cls.class_name) {
			path = options.path;
		}
		if (path.empty() && options.project) {
			path = options.project->global_class_path(p_expr.text);
			if (path.empty() && p_object) {
				path = autoload_script(p_expr.text);
			}
		}
		return is_script_path(path) ? path : std::string();
	}

	// `Outer.Inner` and the like as written, from its first name.
	static std::string dotted_name(const Expr &p_expr) {
		return p_expr.kind == ExprKind::Member && p_expr.base ? dotted_name(*p_expr.base) + "." + p_expr.text : p_expr.text;
	}

	static uint32_t first_name_token(const Expr &p_expr) {
		return p_expr.kind == ExprKind::Member && p_expr.base ? first_name_token(*p_expr.base) : p_expr.name_token;
	}

	// A class of this language as a value: its script, which for an inner
	// class has no path of its own to preload (spec §2.1, §4.1).
	Type class_value(Expr &r_expr, const std::string &p_path) {
		const ClassDecl *header = script_header(p_path);
		if (!header) {
			return fail(r_expr, p_path + " cannot be read.");
		}
		refer(r_expr.name_token, script_id(p_path));
		const size_t inner = p_path.rfind("::");
		if (inner != std::string::npos && !header->is_pub && !sees(p_path.substr(0, inner))) {
			return fail(r_expr, display_name(*header, p_path) + " is not pub.");
		}
		r_expr.target = Target::ClassScript;
		r_expr.owner = p_path;
		return set(r_expr, script_value_type(script_file(p_path)));
	}

	// A name of the class around this one, `r_around`, as a value: what needs
	// no instance (spec §4.1).
	Type value_around(Expr &r_expr, Checker &r_around) {
		const std::string &name = r_expr.text;
		if (const auto value = r_around.bare_enum_value(name)) {
			return use_enum_value(r_expr, *value->first, value->second);
		}
		if (const ConstDecl *constant = r_around.own_constant(name)) {
			return use_constant(r_expr, *constant, declaring(r_around.cls, *constant, &ClassDecl::constants).path);
		}
		if (const EnumDecl *decl = r_around.own_enum(name)) {
			return use_enum_dictionary(r_expr, *decl);
		}
		const std::string inner = inner_class_path(r_around.path(), name);
		if (!inner.empty()) {
			return class_value(r_expr, inner);
		}
		const auto variable = r_around.static_index.find(name);
		if (variable != r_around.static_index.end()) {
			const VarDecl &found = *r_around.cls.all_static_vars[variable->second];
			return use_static(r_expr, found, declaring(r_around.cls, found, &ClassDecl::static_vars));
		}
		const auto function = r_around.function_index.find(name);
		if (function != r_around.function_index.end() && r_around.cls.all_functions[function->second]->is_static) {
			const FuncDecl &found = *r_around.cls.all_functions[function->second];
			const std::string &owner = declaring(r_around.cls, found, &ClassDecl::functions).path;
			refer(r_expr.name_token, script_id(owner, name));
			return static_value(r_expr, found, owner);
		}
		if (r_around.member_index.count(name) || r_around.signal_index.count(name) || function != r_around.function_index.end() || engine.find_property(r_around.cls.native_base, name) ||
				engine.find_method(r_around.cls.native_base, name) || engine.find_signal(r_around.cls.native_base, name)) {
			return fail(r_expr, no_instance(name, r_around));
		}
		return fail(r_expr, "\"" + name + "\" is a type, not a value.");
	}

	std::string no_instance(const std::string &p_name, const Checker &p_around) const {
		return "\"" + p_name + "\" belongs to an object of " + display_name(p_around.cls, p_around.path()) + ", which " + display_name(cls, options.path) + " does not have: an inner class has no object of the class around it.";
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

	// The class an autoload's name stands for, as a value and in a type (spec
	// §11.1): its script's, or for a scene its root's; nullopt for a name that
	// is no autoload, Error after reporting one that cannot be read.
	std::optional<Type> autoload_class(const std::string &p_name, const TokenRange &p_at) {
		const std::string path = options.project ? options.project->autoload_path(p_name) : std::string();
		if (path.empty()) {
			return std::nullopt;
		}
		if (is_script_path(path)) {
			if (const ClassDecl *header = script_header(path)) {
				return script_class_type(*header, path);
			}
		} else if (is_scene_file(path)) {
			if (options.declarations_only) {
				return Type::of(TypeKind::Error); // Scenes are not read while loading.
			}
			const std::optional<Scene> scene = options.project->scene(path);
			if (const SceneNode *root = scene ? scene->find_node(".") : nullptr) {
				return scene_node_type(p_at, *root, "The root of the autoload " + p_name);
			}
		} else if (const ForeignClass *foreign = foreign_class(path); foreign && !foreign->native.empty()) {
			return Type::script_class(p_name, path, foreign->native);
		} else if (options.declarations_only) {
			// Without loading it, which only the declarations do not, a script without a global name is not known.
			return Type::of(TypeKind::Error);
		}
		error(p_at, "The autoload " + p_name + " (" + path + ") cannot be read.");
		return Type::of(TypeKind::Error);
	}

	// The script of this language whose class an autoload's name stands for,
	// found without reporting; empty for any other name.
	std::string autoload_script(const std::string &p_name) {
		const size_t reported = errors.size();
		const std::optional<Type> found = autoload_class(p_name, TokenRange{ 0, 0 });
		errors.resize(reported);
		return found && is_script_path(found->script) ? found->script : std::string();
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
		if (const std::optional<EngineMethod> method = foreign_method_named(r_expr, p_class, *foreign)) {
			return engine_method_value(r_expr, *method);
		}
		return std::nullopt;
	}

	std::optional<Type> foreign_method(Expr &r_expr, const Type &p_class) {
		const ForeignClass *foreign = foreign_class(p_class.script);
		if (!foreign || !foreign->members_known) {
			return unknown_foreign(r_expr, p_class);
		}
		if (const std::optional<EngineMethod> method = foreign_method_named(r_expr, p_class, *foreign)) {
			return check_engine_call(r_expr, *method);
		}
		return std::nullopt;
	}

	// The method `r_expr` names, with types of this language; the engine runs
	// it through the object.
	std::optional<EngineMethod> foreign_method_named(Expr &r_expr, const Type &p_class, const ForeignClass &p_foreign) {
		for (const EngineMethod &method : p_foreign.methods) {
			if (method.name == r_expr.text) {
				EngineMethod local = method;
				for (EngineParam &param : local.params) {
					param.type = localized(param.type);
				}
				local.return_type = localized(local.return_type);
				r_expr.is_foreign = true;
				refer(r_expr.name_token, foreign_id(p_class.script, r_expr.text));
				return local;
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
			if (!p_from.is_interface) {
				return implements(p_from, p_to);
			}
			// The interface or one it extends (spec §3.13).
			const std::vector<Type> up = interface_order({ p_from });
			return std::any_of(up.begin(), up.end(), [&](const Type &p_interface) { return p_interface.script == p_to.script && p_interface.name == p_to.name; });
		}
		if (p_from.is_interface) {
			return p_to.script.empty() && p_to.name == "Object";
		}
		if (!p_to.script.empty()) {
			return extends_script(p_from.script, p_to.script);
		}
		return engine.inherits(p_from.engine_class(), p_to.name);
	}

	std::vector<Type> fields(const Type &p_struct) const override {
		std::vector<Type> types;
		if (const StructDecl *decl = struct_decl(p_struct)) {
			for (const VarDecl &field : decl->fields) {
				types.push_back(field.type);
			}
		}
		return types;
	}

	// Whether an object can be both: `is` and `as` between them are not always false.
	bool related(const Type &p_a, const Type &p_b) const {
		return is_a(p_a, p_b) || is_a(p_b, p_a) || p_a.is_interface || p_b.is_interface;
	}

	// A class of this language named by its class_name, or an interface or a
	// struct in one, such as Combat.Damageable; nullopt if the name is not one.
	std::optional<Type> script_type(const TypeRef &p_ref) {
		std::vector<std::string> names;
		for (size_t at = 0; at <= p_ref.name.size();) {
			const size_t dot = p_ref.name.find('.', at);
			names.push_back(p_ref.name.substr(at, dot == std::string::npos ? dot : dot - at));
			at = dot == std::string::npos ? p_ref.name.size() + 1 : dot + 1;
		}
		const std::string &head = names[0];
		// The token of each name of `Outer.Inner.Hit`.
		const auto token = [&](size_t p_index) { return p_ref.name_token == NO_TOKEN ? NO_TOKEN : p_ref.name_token + uint32_t(2 * p_index); };
		std::string path;
		const ConstPtr alias = class_alias(head);
		if (alias) {
			refer(p_ref.name_token, alias_id(head));
			if (alias->script_class.is_error()) {
				return alias->script_class; // Not known while only the declarations are checked.
			}
			path = alias->script;
		} else if (!(path = scope_class(head)).empty()) {
		} else if (!cls.class_name.empty() && head == cls.class_name) {
			path = options.path;
		} else if (options.project) {
			path = options.project->global_class_path(head);
		}
		if (path.empty()) {
			// A type of this class or of one around it, by its own name, and an
			// enum of the engine class this one extends, come before an
			// autoload's name (spec §4.1, §11.1).
			std::optional<Type> own = own_type(head, p_ref.name_token);
			if (Checker *around = own ? nullptr : class_around(head)) {
				own = around->own_type(head, p_ref.name_token);
			}
			const bool nearer = own || (names.size() == 1 && engine.find_enum(cls.native_base, head));
			const std::optional<Type> autoload = nearer ? std::nullopt : autoload_class(head, p_ref.range);
			if (!autoload) {
				return names.size() == 1 ? own : std::nullopt;
			}
			refer(p_ref.name_token, autoload_id(head));
			if (names.size() == 1 || autoload->is_error()) {
				return autoload;
			}
			if (autoload->script.empty()) {
				// The engine class of a scene's root: its enums.
				const std::optional<Type> found = names.size() == 2 ? engine.find_enum(autoload->name, names[1]) : std::nullopt;
				if (!found) {
					error(p_ref.range, "Unknown type \"" + p_ref.name + "\".");
					return Type::of(TypeKind::Error);
				}
				refer(token(1), engine_id(autoload->name, names[1]));
				return found;
			}
			path = autoload->script;
		} else if (!alias) {
			refer(p_ref.name_token, is_script_path(path) ? script_id(path) : foreign_id(path, std::string()));
		}
		const size_t dot = p_ref.name.find('.');
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
		// The inner classes down to the last name (spec §4.1).
		for (size_t i = 1; i < names.size(); i++) {
			const std::string inner = inner_class_path(path, names[i]);
			if (inner.empty()) {
				if (i + 1 < names.size()) {
					error(p_ref.range, missing(display_name(*header, path) + " declares no class \"" + names[i] + "\".", path));
					return Type::of(TypeKind::Error);
				}
				break;
			}
			path = inner;
			header = script_header(path);
			refer(token(i), script_id(path));
			if (!header->is_pub && !sees(path.substr(0, path.rfind("::")))) {
				error(p_ref.range, p_ref.name + " is not pub.");
				return Type::of(TypeKind::Error);
			}
			if (i + 1 == names.size()) {
				return script_class_type(*header, path);
			}
		}
		if (names.size() == 1) {
			return script_class_type(*header, path);
		}
		const std::string &inner = names.back();
		const uint32_t at = token(names.size() - 1);
		for (const InterfaceDecl &interface : header->interfaces) {
			if (interface.name == inner) {
				if (!interface.is_pub && !in_this_file(path)) {
					error(p_ref.range, p_ref.name + " is not pub.");
					return Type::of(TypeKind::Error);
				}
				if (const auto message = broken(&interface, display_name(*header, path) + "." + inner)) {
					error(p_ref.range, *message);
					return Type::of(TypeKind::Error);
				}
				refer(at, script_id(path, inner));
				return Type::interface(display_name(*header, path) + "." + inner, path);
			}
		}
		if (const StructDecl *found = header->find_struct(inner)) {
			if (!found->is_pub && !in_this_file(path)) {
				error(p_ref.range, p_ref.name + " is not pub.");
				return Type::of(TypeKind::Error);
			}
			// As checked, unless that check is under way, as when two files'
			// structs name each other.
			const ClassDecl *checked = script_decl(path);
			const StructDecl *checked_struct = checked ? checked->find_struct(inner) : nullptr;
			if (const auto message = broken(checked_struct ? checked_struct : found, display_name(*header, path) + "." + inner)) {
				error(p_ref.range, *message);
				return Type::of(TypeKind::Error);
			}
			refer(at, script_id(path, inner));
			return Type::structure(display_name(*header, path) + "." + inner, path);
		}
		if (std::any_of(header->enums.begin(), header->enums.end(), [&](const EnumDecl &p_enum) { return p_enum.name == inner; })) {
			const ClassDecl *checked = path == options.path ? &cls : script_decl(path);
			const EnumDecl *found = checked ? checked->find_enum(inner) : nullptr;
			if (!found) {
				error(p_ref.range, display_name(*header, path) + " (" + path + ") has errors.");
				return Type::of(TypeKind::Error);
			}
			if (!found->is_pub && !sees(path)) {
				error(p_ref.range, p_ref.name + " is not pub.");
				return Type::of(TypeKind::Error);
			}
			if (const auto message = broken(found, display_name(*header, path) + "." + inner)) {
				error(p_ref.range, *message);
				return Type::of(TypeKind::Error);
			}
			refer(at, script_id(found->script, inner));
			return found->type;
		}
		error(p_ref.range, missing(display_name(*header, path) + " declares no class, interface, struct or enum \"" + inner + "\".", path));
		return Type::of(TypeKind::Error);
	}

	// A type this class declares, by its bare name: an interface, a struct or
	// an enum, or an enum that a constant names.
	std::optional<Type> own_type(const std::string &p_name, uint32_t p_token) {
		if (const std::optional<Type> aliased = constant_index.count(p_name) ? aliased_enum_type(p_name) : std::nullopt) {
			const EnumDecl *decl = aliased_enum(p_name);
			refer(p_token, alias_id(p_name));
			return decl ? decl->type : *aliased;
		}
		for (const InterfaceDecl &interface : cls.interfaces) {
			if (interface.name == p_name) {
				refer(p_token, script_id(options.path, interface.name));
				return Type::interface(display_name(cls, options.path) + "." + interface.name, options.path);
			}
		}
		if (cls.find_struct(p_name)) {
			refer(p_token, script_id(options.path, p_name));
			return Type::structure(display_name(cls, options.path) + "." + p_name, options.path);
		}
		if (const EnumDecl *decl = p_name.empty() ? nullptr : cls.find_enum(p_name)) {
			refer(p_token, script_id(decl->script, decl->name));
			return decl->type;
		}
		return std::nullopt;
	}

	// The declaration of a struct, with its fields checked; null when its
	// script has errors.
	// Null also when it has errors, which its type's name reports.
	const StructDecl *struct_decl(const Type &p_struct) const {
		if (p_struct == Type::conversion_error()) {
			return &conversion_error_decl();
		}
		const ClassDecl *decl = script_decl(p_struct.script);
		const StructDecl *found = decl ? decl->find_struct(p_struct.interface_name()) : nullptr;
		return found && !classes.errors_in(found) ? found : nullptr;
	}

	// Whether a struct can start at its default value (spec §4.3); true when
	// that is not known, which is reported elsewhere. An enum with payloads
	// never can (§3.7); other types are left to the caller.
	bool has_default(const Type &p_type) const {
		if (p_type.kind == TypeKind::PayloadEnum) {
			return false;
		}
		const StructDecl *decl = p_type.kind == TypeKind::Struct ? struct_decl(p_type) : nullptr;
		return !decl || decl->has_default();
	}

	// Whether a variable of the type starts at a value without an initializer
	// (spec §4.3): not a non-null object, a function, an enum without 0 or
	// with payloads, nor a struct with a field that has no default.
	bool has_value_without_initializer(const Type &p_type) const {
		const bool reference = p_type.kind == TypeKind::Object || p_type.kind == TypeKind::Task;
		return !(reference && !p_type.nullable) && p_type.kind != TypeKind::Function && has_default(p_type) && enum_has_default(p_type);
	}

	bool assignable(const Type &p_from, const Type &p_to) const {
		return is_assignable(p_from, p_to, *this);
	}

	bool is_type_name(const std::string &p_name) const {
		return p_name == "int" || p_name == "float" || p_name == "bool" || builtin::find_class(p_name) || engine.class_exists(p_name);
	}

	Type resolve(const TypeRef &p_ref, bool p_allow_void) {
		Type type = resolve_name(p_ref, p_allow_void);
		if (p_ref.optional > 0 && type.kind == TypeKind::Void) {
			error(p_ref.range, "void cannot be optional.");
			return Type::of(TypeKind::Error);
		}
		for (uint32_t i = 0; i < p_ref.optional && !type.is_error(); i++) {
			type = optional_of(type);
		}
		return type;
	}

	Type resolve_name(const TypeRef &p_ref, bool p_allow_void) {
		if (completion && at_cursor(p_ref.name)) {
			complete_type(p_ref, p_allow_void);
			return Type::of(TypeKind::Error);
		}
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
		if (p_ref.name == "Variant") {
			refer(p_ref.name_token, engine_id(p_ref.name));
			return Type::of(TypeKind::Variant);
		}
		// The engine's name for the type of null, which is no type, as in
		// GDScript (modules/gdscript/gdscript_parser.cpp:48-65; ed1daf0).
		if (p_ref.name == "Nil") {
			error(p_ref.range, "Unknown type \"Nil\".");
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
		// Such as Node.ProcessMode or Key, but a type this class or one around
		// it declares comes first, as an inner class named Key (spec §4.1).
		const std::optional<Type> engine_enum = named_engine_enum(p_ref.name);
		const bool engine_enum_later = engine_enum && declared_in_scope(p_ref.name);
		if (engine_enum && !engine_enum_later) {
			refer_engine_enum(p_ref.name, p_ref.name_token, p_ref.name_token == NO_TOKEN ? NO_TOKEN : p_ref.name_token + 2);
			return *engine_enum;
		}
		// An enum of the engine class this one extends, such as ProcessMode
		// (spec §4.1), after what the class declares and the global classes.
		const std::optional<Type> base_enum = p_ref.name.find('.') == std::string::npos && !declares_in_script(p_ref.name) ? engine.find_enum(cls.native_base, p_ref.name) : std::nullopt;
		if (base_enum && refuse_ambiguous_engine_name(p_ref.name, p_ref.range)) {
			return Type::of(TypeKind::Error);
		}
		if (const std::optional<Type> script = script_type(p_ref)) {
			return *script;
		}
		if (base_enum) {
			refer(p_ref.name_token, engine_id(base_enum->name.substr(0, base_enum->name.find('.')), p_ref.name));
			return *base_enum;
		}
		if (engine_enum_later) {
			refer(p_ref.name_token, engine_id(p_ref.name));
			return *engine_enum;
		}
		if (p_ref.name == "ConversionError") {
			refer(p_ref.name_token, language_id(p_ref.name, std::string()));
			return Type::conversion_error();
		}
		if (p_ref.name == "CancelReason") {
			refer(p_ref.name_token, language_id(p_ref.name, std::string()));
			return Type::cancel_reason();
		}
		error(p_ref.range, "Unknown type \"" + p_ref.name + "\".");
		return Type::of(TypeKind::Error);
	}

	// `Array[T]`, the engine's typed array (spec §3.8).
	Type resolve_generic(const TypeRef &p_ref) {
		refer(p_ref.name_token, p_ref.name == "Task" || p_ref.name == "Result" || p_ref.name == "Settled" ? language_id(p_ref.name, std::string()) : engine_id(p_ref.name));
		if ((p_ref.name == "Task" || p_ref.name == "Settled") && p_ref.args.size() == 1) {
			const Type result = resolve(p_ref.args[0], true);
			return result.is_error() ? result : (p_ref.name == "Task" ? Type::task(result) : Type::settled(result));
		}
		if (p_ref.name == "Result") {
			if (p_ref.args.size() != 2) {
				error(p_ref.range, "Result takes a value type and an error type, as in Result[int, String].");
				return Type::of(TypeKind::Error);
			}
			const Type value = resolve(p_ref.args[0], false);
			const Type failure = resolve(p_ref.args[1], false);
			return value.is_error() || failure.is_error() ? Type::of(TypeKind::Error) : Type::result(value, failure);
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

	// Of a function with a rest parameter, a Callable of unknown signature
	// (spec §4.6).
	static Type function_type(const FuncDecl &p_function) {
		if (p_function.has_rest()) {
			return Type::builtin("Callable");
		}
		Type type = Type::function(p_function.return_type, param_types(p_function.params));
		type.is_async = p_function.is_async;
		return type;
	}

	// The signature as a function type writes it, a rest parameter as
	// `...Array[T]`.
	static std::string signature_text(const FuncDecl &p_function) {
		std::vector<Type> params = param_types(p_function.params);
		if (p_function.has_rest()) {
			params.back() = Type::builtin("..." + params.back().to_string()); // Only written out.
		}
		Type type = Type::function(p_function.return_type, params);
		type.is_async = p_function.is_async;
		return type.to_string();
	}

	// The engine passes what its signals declare, an object only where the
	// audit says it may pass null being T? (spec §4.7, §10.2).
	static Type signal_type(const EngineMethod &p_signal) {
		std::vector<Type> params;
		for (const EngineParam &param : p_signal.params) {
			Type type = param.type;
			type.nullable = type.kind == TypeKind::Object && builtin::signal_param_nullable(p_signal.owner, p_signal.name, uint32_t(params.size()));
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

	// Whether the code is in an instance method of a struct, whose self is the
	// struct (spec §3.6).
	bool has_struct_self() const {
		return structure && !field_default && !outermost()->is_static;
	}

	// The code of a struct without self, as messages name it after "the".
	std::string selfless() const {
		return field_default ? "default value of \"" + field_default->name + "\"" : "static function \"" + outermost()->name + "()\"";
	}

	// The type a name refers to when it is the base of `Name.x` or
	// `Name.f()`: not when a local, a member or a property has that name.
	bool names_type(const Expr &p_expr) const {
		return p_expr.kind == ExprKind::Name && !names_variable(p_expr.text) && is_type_name(p_expr.text) && !engine.is_singleton(p_expr.text);
	}

	bool names_variable(const std::string &p_name) const {
		return visible_local(p_name) || member_index.count(p_name) || static_index.count(p_name) || engine.find_property(cls.native_base, p_name);
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
			if (constant_index.count(constant.name) || member_index.count(constant.name) || static_index.count(constant.name)) {
				error(constant.name_token, "\"" + constant.name + "\" is already declared.");
				continue;
			}
			constant_index[constant.name] = int(cls.all_constants.size());
			cls.all_constants.push_back(&constant);
			refer(constant.name_token, script_id(options.path, constant.name), SymbolKind::Constant);
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
		int static_initializing;

		explicit ClassScope(Checker &r_checker) :
				checker(r_checker), function(r_checker.function), scopes(std::move(r_checker.scopes)), narrowed(std::move(r_checker.narrowed)), loop_depth(r_checker.loop_depth), in_predelete(r_checker.in_predelete), initializing(r_checker.initializing), initializing_onready(r_checker.initializing_onready), static_initializing(r_checker.static_initializing) {
			checker.function = nullptr;
			checker.scopes.clear();
			checker.narrowed.assign(1, {});
			checker.loop_depth = 0;
			checker.in_predelete = false;
			checker.initializing = -1;
			checker.initializing_onready = false;
			checker.static_initializing = -1;
		}
		~ClassScope() {
			checker.function = function;
			checker.scopes = std::move(scopes);
			checker.narrowed = std::move(narrowed);
			checker.loop_depth = loop_depth;
			checker.in_predelete = in_predelete;
			checker.initializing = initializing;
			checker.initializing_onready = initializing_onready;
			checker.static_initializing = static_initializing;
		}
	};

	// The value of an expression whose type is unknown while loading because
	// of the preloads in it: a preload, or the literal of an untyped container
	// of such values and of known ones.
	static ConstPtr loading_value(const Expr &p_expr) {
		const bool array = p_expr.kind == ExprKind::ArrayLiteral;
		if (p_expr.constant || (!array && p_expr.kind != ExprKind::DictionaryLiteral)) {
			return p_expr.constant;
		}
		auto value = std::make_shared<ConstValue>();
		value->kind = array ? ConstValue::Kind::Array : ConstValue::Kind::Dictionary;
		value->type = Type::builtin(array ? "Array" : "Dictionary");
		for (size_t i = 0; i < p_expr.args.size(); i++) {
			ConstPtr item = loading_value(*p_expr.args[i]);
			if (!item || (!array && i % 2 == 0 && !item->is_known())) {
				return nullptr;
			}
			value->items.push_back(std::move(item));
		}
		return value;
	}

	// A value whose type is `p_type`, from the initializer as written: the
	// type is inferred from it when not declared (spec §4.3, §4.5).
	ConstPtr constant_value(Expr &r_initializer, const std::optional<TypeRef> &p_declared, Type &r_type) {
		r_type = Type::of(TypeKind::Error);
		if (p_declared) {
			r_type = resolve(*p_declared, false);
		}
		const Type value = check_value(r_initializer, r_type);
		if (value.is_error()) {
			// While loading, a preload's type is unknown, and so is that of a
			// container of them; the constant still holds its value, for
			// GDScript to read (spec §11.3).
			return options.declarations_only ? loading_value(r_initializer) : nullptr;
		}
		if (p_declared) {
			if (r_type.is_error()) {
				return nullptr;
			}
			// A constant keeps a scene statically known (spec §7.4).
			if (!assignable(value, r_type)) {
				expect_assignable(r_initializer, value, r_type, "assign");
				return nullptr;
			}
			expect_implemented(r_initializer, value, r_type);
		} else if (value.kind == TypeKind::None) {
			error(r_initializer.range, "The constant needs a type: None does not say which T? it is.");
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
			// One initialized with the name of an enum with payloads names the
			// enum, but has no value, as the name has none (spec §3.7).
			if (const EnumDecl *payload = r_decl.declared_type ? nullptr : initializer_payload_enum(*r_decl.initializer)) {
				auto alias = std::make_shared<ConstValue>();
				alias->type = payload->type;
				alias->names_enum = true;
				r_decl.type = payload->type;
				r_decl.value = alias;
			} else {
				r_decl.value = constant_value(*r_decl.initializer, r_decl.declared_type, r_decl.type);
			}
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
		if (names_payload_enum(p_decl)) {
			return fail(r_expr, p_decl.name + " names an enum with payloads, whose name is no value; its variants are.");
		}
		r_expr.constant = p_decl.value;
		return set(r_expr, p_decl.type);
	}

	// The enum with payloads that a constant's initializer names, also through
	// another constant, which is checked as any use of it checks it.
	const EnumDecl *initializer_payload_enum(Expr &r_initializer) {
		if (r_initializer.kind == ExprKind::Name && !visible_local(r_initializer.text) && constant_index.count(r_initializer.text)) {
			const EnumDecl *decl = aliased_enum(r_initializer.text);
			if (!decl || !decl->has_payloads) {
				return nullptr;
			}
			refer(r_initializer.name_token, alias_id(r_initializer.text));
			return decl;
		}
		return payload_enum_named(r_initializer);
	}

	// A constant initialized with the name of an enum with payloads.
	static bool names_payload_enum(const ConstDecl &p_decl) {
		return p_decl.value && p_decl.value->names_enum && p_decl.type.kind == TypeKind::PayloadEnum;
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
			decl.type = decl.name.empty() ? Type::of(TypeKind::Int) : Type::enumeration(display_name(cls, options.path) + "." + decl.name, options.path, decl.numbers, decl.has_payloads ? TypeKind::PayloadEnum : TypeKind::Enum);
			if (!decl.name.empty()) {
				refer(decl.name_token, script_id(options.path, decl.name), SymbolKind::Enum);
			}
			for (const EnumValueDecl &value : decl.values) {
				refer(value.name_token, script_id(options.path, enum_value_id(decl, value.name)), SymbolKind::Constant);
				for (const VarDecl &field : value.fields) {
					refer(field.name_token, script_id(options.path, enum_value_id(decl, value.name) + "." + field.name), SymbolKind::Property);
				}
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

	// The names of the types the class declares and of its unnamed enums'
	// values (spec §4.1), after everything else is declared: each once among
	// the class's declarations, and no type a built-in, engine or global class.
	void check_type_names() {
		std::unordered_set<std::string> names;
		const auto declare = [&](const std::string &p_name, uint32_t p_at, bool p_type) {
			const bool taken = !names.insert(p_name).second || constant_index.count(p_name) || member_index.count(p_name) || static_index.count(p_name) || function_index.count(p_name) || signal_index.count(p_name);
			if (taken || (p_type && (is_type_name(p_name) || (options.project && !options.project->global_class_path(p_name).empty())))) {
				error(p_at, "\"" + p_name + "\" is already declared.");
			}
		};
		for (const EnumDecl *decl : cls.all_enums) {
			if (is_own(*decl)) {
				continue;
			}
			if (!decl->name.empty()) {
				names.insert(decl->name);
				continue;
			}
			for (const EnumValueDecl &value : decl->values) {
				names.insert(value.name);
			}
		}
		for (const InterfaceDecl &interface : cls.interfaces) {
			declare(interface.name, interface.name_token, true);
		}
		for (const StructDecl &decl : cls.structs) {
			declare(decl.name, decl.name_token, true);
		}
		for (const EnumDecl &decl : cls.enums) {
			if (!decl.name.empty()) {
				declare(decl.name, decl.name_token, true);
				continue;
			}
			for (const EnumValueDecl &value : decl.values) {
				declare(value.name, value.name_token, false);
			}
		}
		for (const std::unique_ptr<ClassDecl> &inner : cls.classes) {
			declare(inner->name, inner->name_token, true);
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
			if (r_decl.has_payloads && r_decl.name.empty()) {
				error(r_decl.range, "An enum with payloads needs a name: its variants are not int constants.");
			}
			for (size_t i = 0; i < r_decl.values.size(); i++) {
				EnumValueDecl &value = r_decl.values[i];
				if (!r_decl.name.empty() && !names.insert(value.name).second) {
					error(value.name_token, "\"" + value.name + "\" is already a value of " + r_decl.name + ".");
				}
				current_value = i;
				if (r_decl.has_payloads) {
					if (value.value) {
						error(value.value->range, "The variants of an enum with payloads have no int values.");
					}
					check_variant_fields(r_decl, value);
					r_decl.numbers->list.push_back({ value.name, int64_t(i) });
					r_decl.constants.push_back(enum_constant(int64_t(i), r_decl.type));
					continue;
				}
				if (value.value) {
					const Type type = check_value(*value.value, Type::of(TypeKind::Int));
					if (type.is_error()) {
					} else if (type.kind != TypeKind::Int && type.kind != TypeKind::Enum) {
						error(value.value->range, "The values of an enum are ints, not " + type.to_string() + ".");
					} else if (!value.value->constant || value.value->constant->kind != ConstValue::Kind::Int) {
						error(value.value->range, "The values of an enum must be known at compile time.");
					} else {
						next = value.value->constant->i;
					}
				}
				r_decl.numbers->list.push_back({ value.name, next });
				r_decl.constants.push_back(enum_constant(next, r_decl.type));
				next = int64_t(uint64_t(next) + 1);
			}
			if (!r_decl.name.empty() && !r_decl.has_payloads) {
				r_decl.dictionary = enum_dictionary(*r_decl.numbers, r_decl.type);
			}
			in_constant = was_in_constant;
			current_enum = was_enum;
			current_value = was_value;
		}
		r_decl.state = EnumDecl::State::Checked;
	}

	// The fields of a variant: named and typed, each once (spec §3.7).
	void check_variant_fields(EnumDecl &r_decl, EnumValueDecl &r_value) {
		std::unordered_set<std::string> names;
		std::vector<std::pair<std::string, Type>> &fields = r_decl.numbers->fields.emplace_back();
		for (VarDecl &field : r_value.fields) {
			if (!names.insert(field.name).second) {
				error(field.name_token, "The field \"" + field.name + "\" is already declared in " + r_value.name + ".");
			}
			field.type = field.declared_type ? resolve(*field.declared_type, false) : Type::of(TypeKind::Error);
			fields.push_back({ field.name, field.type });
		}
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
		const EnumValueDecl &value = p_decl.values[p_index];
		refer(r_expr.name_token, enum_id(p_decl, value.name));
		if (!value.fields.empty()) {
			return fail(r_expr, p_decl.name + "." + value.name + " has a payload; make it with " + p_decl.name + "." + value.name + "(" + value.fields[0].name + " = ...).");
		}
		if (size_t(p_index) >= p_decl.constants.size()) {
			return fail(r_expr, "The value \"" + value.name + "\" depends on its own value.");
		}
		r_expr.target = Target::Constant;
		r_expr.constant = p_decl.constants[p_index];
		return set(r_expr, p_decl.constants[p_index]->type);
	}

	// The name of a named enum as a value: its names and values (spec §3.7).
	Type use_enum_dictionary(Expr &r_expr, const EnumDecl &p_decl, bool p_refer = true) {
		if (p_refer) {
			refer(r_expr.name_token, enum_id(p_decl));
		}
		if (p_decl.has_payloads) {
			return fail(r_expr, p_decl.name + " is an enum with payloads, whose name is no value; its variants are, as " + p_decl.name + "." + p_decl.values[0].name + ".");
		}
		if (!p_decl.dictionary) {
			return fail(r_expr, "The enum " + p_decl.name + " depends on its own values.");
		}
		r_expr.target = Target::Constant;
		r_expr.constant = p_decl.dictionary;
		return set(r_expr, p_decl.dictionary->type);
	}

	// The name of an enum of the engine as a value, as for one of this
	// language: its names and values (spec §3.7).
	Type use_engine_enum_dictionary(Expr &r_expr, const Type &p_enum) {
		r_expr.target = Target::Constant;
		r_expr.constant = enum_dictionary(*p_enum.values, p_enum);
		return set(r_expr, r_expr.constant->type);
	}

	// The enum that the base of `State.IDLE`, `State(x)` or `State.try_from(x)`
	// names: one of this class by its bare name, when nothing closer has it, or
	// a pub one of another class, as in `Player.State`; null for any other base.
	const EnumDecl *named_enum(Expr &r_base) {
		if (r_base.kind == ExprKind::Name) {
			if (visible_local(r_base.text) || member_index.count(r_base.text) || engine.find_property(cls.native_base, r_base.text)) {
				return nullptr;
			}
			if (constant_index.count(r_base.text)) {
				const EnumDecl *aliased = aliased_enum(r_base.text);
				if (aliased) {
					refer(r_base.name_token, alias_id(r_base.text));
				}
				return aliased;
			}
			const EnumDecl *decl = scope_enum(r_base.text);
			if (decl) {
				refer(r_base.name_token, enum_id(*decl));
			}
			return decl;
		}
		if (r_base.kind != ExprKind::Member || !r_base.base || !names_enum_of_class(*r_base.base, r_base.text)) {
			return nullptr;
		}
		std::optional<Type> named = named_class(*r_base.base);
		if (!named && r_base.base->kind == ExprKind::Name) {
			named = check_value(*r_base.base); // An autoload, which leads to its class (spec §11.1).
		}
		if (!named || named->is_error() || !is_script_path(named->script)) {
			return nullptr;
		}
		const ClassDecl *decl = named->script == options.path ? &cls : script_decl(named->script);
		const EnumDecl *found = decl ? decl->find_enum(r_base.text) : nullptr;
		if (!found) {
			return nullptr;
		}
		Checker *local = file->find(named->script);
		// Checked when first used, also while that class is declared.
		if (const EnumDecl *checked = local ? local->own_enum(r_base.text) : nullptr) {
			found = checked;
		}
		if (!found->is_pub && !sees(found->script)) {
			error(r_base.range, "The enum " + named->name + "." + found->name + " is not pub.");
		} else if (const auto message = local ? std::nullopt : broken(found, named->name + "." + found->name)) {
			error(r_base.range, *message);
		}
		refer(r_base.name_token, script_id(found->script, found->name));
		return found;
	}

	// The enum that a constant of this class names, being initialized with
	// the enum's name, as in `const STATE = Inspector.STATE` (spec §3.7).
	const EnumDecl *aliased_enum(const std::string &p_name) {
		const std::optional<Type> type = aliased_enum_type(p_name);
		if (!type || type->values->is_engine) {
			return nullptr;
		}
		const ClassDecl *decl = type->script == options.path ? &cls : script_decl(type->script);
		return decl ? decl->find_enum(type->interface_name()) : nullptr;
	}

	// The enum, of this language or of the engine, that a constant of this
	// class names, as `const EASE = Tween.EaseType`: its Dictionary's values,
	// or for an enum with payloads, which has none, its own type.
	std::optional<Type> aliased_enum_type(const std::string &p_name) {
		const ConstDecl *constant = own_constant(p_name);
		if (!constant || !constant->value || !constant->value->names_enum) {
			return std::nullopt;
		}
		const Type &type = names_payload_enum(*constant) ? constant->type : constant->type.args.size() == 2 ? constant->type.args[1]
																											: constant->type;
		if (type.kind != TypeKind::Enum && type.kind != TypeKind::PayloadEnum) {
			return std::nullopt;
		}
		return type.values ? std::optional<Type>(type) : std::nullopt;
	}

	// The engine's enum that the base of `Tween.EaseType.EASE_IN` or
	// `EASE.EASE_IN` names: by the engine's name, unless a closer one hides
	// it, or by a constant that names it (spec §3.7).
	std::optional<Type> named_engine_enum_base(const Expr &p_base) {
		if (p_base.kind == ExprKind::Name) {
			if (visible_local(p_base.text) || member_index.count(p_base.text) || engine.find_property(cls.native_base, p_base.text)) {
				return std::nullopt;
			}
			if (constant_index.count(p_base.text)) {
				const std::optional<Type> aliased = aliased_enum_type(p_base.text);
				if (aliased && aliased->values->is_engine) {
					refer(p_base.name_token, alias_id(p_base.text));
					return aliased;
				}
				return std::nullopt;
			}
			if (declared_in_scope(p_base.text)) {
				return std::nullopt;
			}
			if (const std::optional<Type> found = named_engine_enum(p_base.text)) {
				refer_engine_enum(p_base.text, p_base.name_token, p_base.name_token);
				return found;
			}
			// One of the engine class this one extends, such as ProcessMode (spec §4.1).
			const std::optional<Type> found = engine.find_enum(cls.native_base, p_base.text);
			if (found) {
				refer(p_base.name_token, engine_id(found->name.substr(0, found->name.find('.')), p_base.text));
			}
			return found;
		}
		if (p_base.kind != ExprKind::Member || !p_base.base || !names_type(*p_base.base)) {
			return std::nullopt;
		}
		const std::string name = p_base.base->text + "." + p_base.text;
		const std::optional<Type> found = named_engine_enum(name);
		if (found) {
			refer_engine_enum(name, p_base.base->name_token, p_base.name_token);
		}
		return found;
	}

	// A value of the engine's enum by its name, as `Tween.EaseType.EASE_IN`.
	Type check_engine_enum_value(Expr &r_expr, const Type &p_enum) {
		for (const auto &[name, number] : p_enum.values->list) {
			if (name == r_expr.text) {
				r_expr.target = Target::Constant;
				r_expr.constant = enum_constant(number, p_enum);
				const size_t dot = p_enum.name.find('.');
				refer(r_expr.name_token, engine_id(dot == std::string::npos ? "@GlobalScope" : p_enum.name.substr(0, dot), name));
				return set(r_expr, p_enum);
			}
		}
		return fail(r_expr, p_enum.name + " has no value \"" + r_expr.text + "\".");
	}

	// Whether `Class.Name` names an enum of a class of this language, found
	// as named_class() finds the class, without referring to anything yet.
	bool names_enum_of_class(const Expr &p_class, const std::string &p_name) {
		if (p_class.kind != ExprKind::Name && p_class.kind != ExprKind::Member) {
			return false;
		}
		// Declared or inherited, as the headers say.
		std::unordered_set<std::string> seen;
		for (std::string path = class_path_of(p_class, true); is_script_path(path) && seen.insert(path).second;) {
			const ClassDecl *decl = script_header(path);
			if (!decl) {
				return false;
			}
			if (std::any_of(decl->enums.begin(), decl->enums.end(), [&](const EnumDecl &p_enum) { return p_enum.name == p_name; })) {
				return true;
			}
			path = classes.base_path(*decl, path);
		}
		return false;
	}

	// A named enum by its bare name: one this class has, checked, or one a
	// class around it has (spec §4.1).
	const EnumDecl *scope_enum(const std::string &p_name) {
		if (const EnumDecl *own = own_enum(p_name)) {
			return own;
		}
		Checker *around = class_around(p_name);
		if (!around && p_name == "CancelReason" && !declares_in_script(p_name)) {
			return &cancel_reason_decl();
		}
		return around ? around->own_enum(p_name) : nullptr;
	}

	// CancelReason (spec §9.2), the enum that the language declares.
	static const EnumDecl &cancel_reason_decl() {
		static const EnumDecl decl = [] {
			EnumDecl result;
			result.name = "CancelReason";
			result.is_pub = true;
			result.state = EnumDecl::State::Checked;
			result.type = Type::cancel_reason();
			result.numbers = std::make_shared<EnumValues>(*result.type.values);
			for (const auto &[name, number] : result.numbers->list) {
				result.values.emplace_back().name = name;
				result.constants.push_back(enum_constant(number, result.type));
			}
			result.dictionary = enum_dictionary(*result.numbers, result.type);
			return result;
		}();
		return decl;
	}

	// What a use of a named enum, or of one of its values, refers to.
	static DeclId enum_id(const EnumDecl &p_decl, const std::string &p_value = std::string()) {
		if (&p_decl == &cancel_reason_decl()) {
			return language_id(p_decl.name, p_value);
		}
		return script_id(p_decl.script, p_value.empty() ? p_decl.name : enum_value_id(p_decl, p_value));
	}

	// An enum of the engine by the name code writes: a global one, such as
	// MouseButton or Variant.Type, one of a built-in type, such as
	// Vector3.Axis, or one of a class, such as Node.ProcessMode (spec §3.7).
	std::optional<Type> named_engine_enum(const std::string &p_name) const {
		const size_t dot = p_name.find('.');
		if (dot != std::string::npos && p_name.find('.', dot + 1) == std::string::npos && engine.class_exists(p_name.substr(0, dot))) {
			return engine.find_enum(p_name.substr(0, dot), p_name.substr(dot + 1));
		}
		return builtin_enum(p_name);
	}

	// How this file writes an enum: State for its own, Player.State for another class's.
	std::string enum_spelling(const Type &p_enum) const {
		return p_enum.values && !p_enum.values->is_engine && !p_enum.is_result() && p_enum.script == options.path ? p_enum.interface_name() : p_enum.to_string();
	}

	// `State(x)` or `Player.State(x)`: the enum the call names, if any.
	const EnumDecl *called_enum(Expr &r_call) {
		if (!r_call.base) {
			if (function_index.count(r_call.text) || visible_local(r_call.text) || member_index.count(r_call.text)) {
				return nullptr;
			}
			if (constant_index.count(r_call.text)) {
				const EnumDecl *aliased = aliased_enum(r_call.text);
				if (aliased) {
					refer(r_call.name_token, alias_id(r_call.text));
				}
				return aliased;
			}
			const EnumDecl *decl = scope_enum(r_call.text);
			if (decl) {
				refer(r_call.name_token, enum_id(*decl));
			}
			return decl;
		}
		if (!names_enum_of_class(*r_call.base, r_call.text)) {
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

	// `MouseButton(x)`, `Node.ProcessMode(x)` or `Vector3.Axis(x)`: the
	// engine's enum the call names, if any.
	std::optional<Type> called_engine_enum(Expr &r_call) {
		std::string name = r_call.text;
		if (r_call.base) {
			if (r_call.base->kind != ExprKind::Name || visible_local(r_call.base->text) || member_index.count(r_call.base->text) || constant_index.count(r_call.base->text)) {
				return std::nullopt;
			}
			name = r_call.base->text + "." + r_call.text;
		} else if (constant_index.count(name) && !visible_local(name)) {
			const std::optional<Type> aliased = aliased_enum_type(name);
			if (!aliased || !aliased->values->is_engine) {
				return std::nullopt;
			}
			refer(r_call.name_token, alias_id(name));
			return aliased;
		} else if (declared_in_scope(name)) {
			return std::nullopt;
		}
		const std::optional<Type> found = named_engine_enum(name);
		if (found) {
			refer_engine_enum(name, r_call.base ? r_call.base->name_token : r_call.name_token, r_call.name_token);
		} else if (!r_call.base) {
			// One of the engine class this one extends (spec §4.1).
			const std::optional<Type> base_enum = engine.find_enum(cls.native_base, name);
			if (base_enum) {
				refer(r_call.name_token, engine_id(base_enum->name.substr(0, base_enum->name.find('.')), name));
			}
			return base_enum;
		}
		return found;
	}

	// MouseButton, or Node and ProcessMode of Node.ProcessMode, each by its token.
	void refer_engine_enum(const std::string &p_name, uint32_t p_first, uint32_t p_last) {
		const size_t dot = p_name.find('.');
		if (dot == std::string::npos) {
			refer(p_first, engine_id(p_name));
			return;
		}
		refer(p_first, engine_id(p_name.substr(0, dot)));
		refer(p_last, engine_id(p_name.substr(0, dot), p_name.substr(dot + 1)));
	}

	// An int as one of the enum's values, a program error when it is none
	// (spec §3.7); checked at compile time when the int is known. An enum of
	// the engine is open, and the int is only marked as one of it.
	Type check_enum_conversion(Expr &r_expr, const Type &p_enum) {
		if (r_expr.args.size() != 1 || !r_expr.arg_names.empty()) {
			return fail_call(r_expr, enum_spelling(p_enum) + "() takes one int.");
		}
		Expr &arg = *r_expr.args[0];
		const Type value = check_value(arg, Type::of(TypeKind::Int));
		if (value.is_error()) {
			return set(r_expr, value);
		}
		if (value.kind != TypeKind::Int && value.kind != TypeKind::Enum) {
			return fail(r_expr, enum_spelling(p_enum) + "() converts an int, not " + value.to_string() + "." + (value.kind == TypeKind::Float ? " Convert it with int(x) first." : ""));
		}
		if (arg.constant && arg.constant->kind == ConstValue::Kind::Int) {
			if (!p_enum.values->is_engine && !p_enum.values->has(arg.constant->i)) {
				return fail(r_expr, std::to_string(arg.constant->i) + " is none of the values of " + enum_spelling(p_enum) + ".");
			}
			r_expr.constant = enum_constant(arg.constant->i, p_enum);
		}
		r_expr.target = Target::EnumFromInt;
		r_expr.param_types = { Type::of(TypeKind::Int) };
		return set(r_expr, p_enum);
	}

	// `State.IDLE` (spec §3.7).
	Type check_enum_value(Expr &r_expr, const EnumDecl &p_decl) {
		const int index = p_decl.find(r_expr.text);
		if (index < 0) {
			return fail(r_expr, missing(p_decl.name + " has no value \"" + r_expr.text + "\".", p_decl.script));
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
		const ConstDecl *decl = alias_decl(p_name).first;
		return decl && decl->value && !decl->value->script.empty() ? decl->value : nullptr;
	}

	// A constant of this class or of one around it (spec §4.1), and the class
	// it is reached in.
	std::pair<const ConstDecl *, const ClassDecl *> alias_decl(const std::string &p_name) {
		if (const ConstDecl *decl = own_constant(p_name)) {
			return { decl, &cls };
		}
		Checker *around = constant_index.count(p_name) ? nullptr : class_around(p_name);
		const ConstDecl *decl = around ? around->own_constant(p_name) : nullptr;
		return { decl, decl ? &around->cls : nullptr };
	}

	// The constant that the name of a class alias refers to.
	DeclId alias_id(const std::string &p_name) {
		if (const LocalInfo *local = visible_local(p_name)) {
			return local_id(local->token);
		}
		const auto [decl, owner] = alias_decl(p_name);
		return script_id(declaring(*owner, *decl, &ClassDecl::constants).path, decl->name);
	}

	// The class that the base of `Base.x` or `Base.f()` names, of this
	// language or another (spec §2.1, §11.1): through a constant of a preload
	// of its script, the preload itself, or its class_name when nothing closer
	// has the name; nullopt for any other base, and Error after reporting.
	// The file whose class_name the name is: this one, or another of the
	// project; empty if none.
	std::string class_name_path(const std::string &p_name) const {
		if (!cls.class_name.empty() && p_name == cls.class_name) {
			return options.path;
		}
		return options.project ? options.project->global_class_path(p_name) : std::string();
	}

	std::optional<Type> named_class(Expr &r_base) {
		if (r_base.kind == ExprKind::Call && !r_base.base && r_base.text == "preload") {
			if (!is_script_file(preload_file(preload_path(r_base)))) {
				return std::nullopt;
			}
			check_value(r_base);
			return r_base.constant ? r_base.constant->script_class : Type::of(TypeKind::Error);
		}
		if (r_base.kind == ExprKind::Member && r_base.base) {
			// `Outer.Inner`, through the class or an object of it, which is not
			// evaluated (spec §4.1).
			const std::string path = class_path_of(r_base);
			if (path.empty()) {
				return std::nullopt;
			}
			if (!named_class(*r_base.base)) {
				check_value(*r_base.base);
			}
			const ClassDecl *header = script_header(path);
			refer(r_base.name_token, script_id(path));
			if (!header->is_pub && !sees(path.substr(0, path.rfind("::")))) {
				error(r_base.range, display_name(*header, path) + " is not pub.");
				return Type::of(TypeKind::Error);
			}
			return script_class_type(*header, path);
		}
		if (r_base.kind != ExprKind::Name) {
			return std::nullopt;
		}
		if (const ConstPtr alias = class_alias(r_base.text)) {
			check_value(r_base); // The constant, whose value is the script.
			return alias->script_class;
		}
		if (visible_local(r_base.text) || member_index.count(r_base.text) || static_index.count(r_base.text) || constant_index.count(r_base.text) || engine.find_property(cls.native_base, r_base.text)) {
			return std::nullopt;
		}
		// An inner class of this class or of one around it (spec §4.1).
		const std::string inner = scope_class(r_base.text);
		if (!inner.empty()) {
			refer(r_base.name_token, script_id(inner));
			return script_class_type(*script_header(inner), inner);
		}
		const std::string path = class_name_path(r_base.text);
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
			const std::string &owner = declaring(*decl, *constant, &ClassDecl::constants).path;
			if (const auto message = refused(constant, constant->is_pub || sees(owner), "The constant ", p_class, constant->name)) {
				return fail(r_expr, *message);
			}
			return use_constant(r_expr, *constant, owner);
		}
		// A value of an unnamed enum is a constant of the class (spec §3.7).
		for (const EnumDecl *entry : decl->all_enums) {
			const int index = entry->name.empty() ? entry->find(r_expr.text) : -1;
			if (index < 0) {
				continue;
			}
			if (const auto message = refused(entry, entry->is_pub || sees(entry->script), "The constant ", p_class, r_expr.text)) {
				return fail(r_expr, *message);
			}
			return use_enum_value(r_expr, *entry, index);
		}
		return std::nullopt;
	}

	// A static variable that the class `p_owner` declares (spec §4.8), which
	// an initializer of that class may read once it is initialized.
	Type use_static(Expr &r_expr, const VarDecl &p_variable, const ClassDecl &p_owner) {
		refer(r_expr.name_token, script_id(p_owner.path, p_variable.name));
		if (in_constant) {
			return fail(r_expr, "\"" + p_variable.name + "\" is a variable, not a constant.");
		}
		const int index = int(&p_variable - p_owner.static_vars.data());
		if (static_initializing >= 0 && p_owner.path == options.path && index >= static_initializing) {
			return fail(r_expr, "\"" + p_variable.name + "\" is not initialized yet: static variables are initialized in declaration order.");
		}
		if (p_variable.type.is_error()) {
			return fail(r_expr, "The type of " + display_name(p_owner, p_owner.path) + "." + p_variable.name + " is not known here" + (p_variable.declared_type ? std::string(".") : "; declare it, as in `static var " + p_variable.name + ": T`."));
		}
		r_expr.target = Target::StaticVar;
		r_expr.owner = p_owner.path;
		r_expr.index = index;
		r_expr.variable = &p_variable;
		return set(r_expr, p_variable.type);
	}

	// A static variable of the class at `p_path`, as `Class.name` or
	// `object.name` (spec §4.8); nullopt when the class has none of that
	// name. Other classes see only pub ones, except subclasses (§4.2).
	std::optional<Type> class_static(Expr &r_expr, const std::string &p_path, const std::string &p_class) {
		const ClassDecl *decl = script_decl(p_path);
		if (!decl) {
			return fail(r_expr, p_class + " has errors.");
		}
		for (const VarDecl *variable : decl->all_static_vars) {
			if (variable->name != r_expr.text) {
				continue;
			}
			const ClassDecl &owner = declaring(*decl, *variable, &ClassDecl::static_vars);
			if (!variable->is_pub && !sees(owner.path)) {
				refer(r_expr.name_token, script_id(owner.path, variable->name));
				return fail(r_expr, "The static variable \"" + variable->name + "\" of " + p_class + " is not pub.");
			}
			if (const auto message = broken(variable, p_class + "." + variable->name)) {
				return fail(r_expr, *message);
			}
			return use_static(r_expr, *variable, owner);
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
				return fail(r_expr, "\"" + r_expr.text + "\" is a variable, not a constant.");
			}
			if (initializing >= 0) {
				// @onready initializers run when the node is ready, after the
				// others (spec §6.3); each kind runs in declaration order (§6.1).
				const bool earlier = member->second < initializing;
				const bool ready = read.is_onready ? initializing_onready && earlier : initializing_onready || earlier;
				if (!ready && read.is_onready && !initializing_onready) {
					return fail(r_expr, "\"" + r_expr.text + "\" is set when the node is ready, after this initializer runs.");
				}
				if (!ready) {
					return fail(r_expr, "\"" + r_expr.text + "\" is not initialized yet: initializers run in declaration order.");
				}
			}
			if (read.is_onready && function && (function->name == "_init" || function->name == "_enter_tree")) {
				return fail(r_expr, "\"" + r_expr.text + "\" is set when the node is ready, after " + function->name + "() runs.");
			}
			if (refuse_in_static(r_expr, r_expr.text)) {
				return set(r_expr, Type::of(TypeKind::Error));
			}
			r_expr.target = Target::Member;
			r_expr.index = member->second;
			refer(r_expr.name_token, script_id(declaring(cls, read, &ClassDecl::members).path, read.name));
			return set(r_expr, cls.all_members[member->second]->type);
		}
		const auto found_static = static_index.find(r_expr.text);
		if (found_static != static_index.end()) {
			const VarDecl &variable = *cls.all_static_vars[found_static->second];
			return use_static(r_expr, variable, declaring(cls, variable, &ClassDecl::static_vars));
		}
		if (r_expr.text == "owner" && cls.owner_ref) {
			if (refuse_in_static(r_expr, r_expr.text)) {
				return set(r_expr, Type::of(TypeKind::Error));
			}
			r_expr.target = Target::Owner;
			refer(r_expr.name_token, engine_id("Node", "owner"));
			if (const auto property = engine.find_property("Node", "owner")) {
				r_expr.setter = property->setter; // Spec §10.5.
			}
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
			const FuncDecl &method = *cls.all_functions[function_found->second];
			const std::string &owner = declaring(cls, method, &ClassDecl::functions).path;
			refer(r_expr.name_token, script_id(owner, method.name));
			if (method.is_static) {
				return static_value(r_expr, method, owner);
			}
			// A method value (spec §3.11), which calls the function on this instance.
			if (refuse_in_static(r_expr, r_expr.text)) {
				return set(r_expr, Type::of(TypeKind::Error));
			}
			r_expr.target = Target::MethodValue;
			r_expr.callee = &method;
			return set(r_expr, function_type(method));
		}
		const auto own_signal = signal_index.find(r_expr.text);
		if (own_signal != signal_index.end()) {
			if (refuse_in_static(r_expr, r_expr.text)) {
				return set(r_expr, Type::of(TypeKind::Error));
			}
			r_expr.target = Target::SelfSignal;
			const SignalDecl &signal = *cls.all_signals[own_signal->second];
			r_expr.signal = &signal;
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
		if (const auto method = engine.find_method(cls.native_base, r_expr.text)) {
			if (!method->is_static && refuse_in_static(r_expr, r_expr.text)) {
				return set(r_expr, Type::of(TypeKind::Error));
			}
			return engine_method_value(r_expr, *method);
		}
		// Such as NOTIFICATION_PREDELETE (spec §4.1).
		if (engine.find_constant(cls.native_base, r_expr.text)) {
			if (refuse_ambiguous_engine_name(r_expr.text, r_expr.range)) {
				return set(r_expr, Type::of(TypeKind::Error));
			}
			return check_engine_constant(r_expr, cls.native_base);
		}
		// Such as ProcessMode, an enum's name as a value (spec §3.7, §4.1).
		if (const std::optional<Type> found = engine.find_enum(cls.native_base, r_expr.text)) {
			if (refuse_ambiguous_engine_name(r_expr.text, r_expr.range)) {
				return set(r_expr, Type::of(TypeKind::Error));
			}
			refer(r_expr.name_token, engine_id(found->name.substr(0, found->name.find('.')), r_expr.text));
			return use_engine_enum_dictionary(r_expr, *found);
		}
		return std::nullopt;
	}

	Type check_self(Expr &r_expr) {
		if (has_struct_self()) {
			// A local, which lambdas capture as a copy (spec §3.11).
			int local = find_local("self");
			if (local < 0) {
				local = capture("self");
			}
			r_expr.target = Target::Local;
			r_expr.index = local;
			return set(r_expr, structure_type);
		}
		if (structure) {
			return fail(r_expr, "The " + selfless() + " has no self.");
		}
		if (refuse_in_static(r_expr, "self")) {
			return set(r_expr, Type::of(TypeKind::Error));
		}
		if (in_predelete) {
			return fail(r_expr, "self cannot be used as a value in the PREDELETE branch: the object is being freed, so nothing may keep it.");
		}
		return set(r_expr, script_class_type(cls, options.path));
	}

	// Self, unchecked, as the base that a field or a method of the struct
	// named bare in its methods is on.
	static std::unique_ptr<Expr> struct_self(const Expr &p_at) {
		auto self = std::make_unique<Expr>();
		self->kind = ExprKind::Self;
		self->range = p_at.range;
		self->name_token = NO_TOKEN;
		return self;
	}

	// A field or a method of the struct whose method is being checked, by its
	// bare name (spec §3.6): a field of self.
	std::optional<Type> struct_name(Expr &r_expr) {
		const std::string &name = r_expr.text;
		if (std::any_of(structure->fields.begin(), structure->fields.end(), [&](const VarDecl &p_field) { return p_field.name == name; })) {
			if (!has_struct_self()) {
				return fail(r_expr, "\"" + name + "\" is a field of self, which the " + selfless() + " does not have.");
			}
			r_expr.base = struct_self(r_expr);
			r_expr.kind = ExprKind::Member;
			return check_member_access(r_expr);
		}
		if (structure->find_method(name)) {
			return struct_method_value(r_expr, structure_type, false);
		}
		return std::nullopt;
	}

	// A method of a struct as a value (spec §3.11), found as check_struct_call()
	// finds a call's; an instance method keeps a copy of the struct.
	Type struct_method_value(Expr &r_expr, const Type &p_struct, bool p_on_value) {
		const StructDecl *decl = struct_decl(p_struct);
		if (!decl) {
			return fail(r_expr, p_struct.script + " has errors.");
		}
		const FuncDecl *method = decl->find_method(r_expr.text);
		if (!method) {
			return fail(r_expr, missing(p_struct.name + " has no method \"" + r_expr.text + "()\".", p_struct.script));
		}
		refer(r_expr.name_token, script_id(p_struct.script, method->name));
		if (method->is_static) {
			r_expr.static_on_value = p_on_value;
			return static_value(r_expr, *method, p_struct.script);
		}
		if (!p_on_value) {
			if (r_expr.base) {
				return fail(r_expr, "\"" + r_expr.text + "()\" of " + p_struct.name + " is not static; take it from a value.");
			}
			if (!has_struct_self()) {
				return fail(r_expr, "\"" + r_expr.text + "()\" needs self, which the " + selfless() + " does not have.");
			}
			r_expr.base = struct_self(r_expr);
			check_value(*r_expr.base);
		}
		r_expr.target = Target::StructMethodValue;
		r_expr.callee = method;
		r_expr.owner = p_struct.script;
		return set(r_expr, function_type(*method));
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
		if (const std::optional<Type> found = structure ? struct_name(r_expr) : std::nullopt) {
			return *found;
		}
		if (const std::optional<Type> found = check_self_name(r_expr)) {
			return *found;
		}
		const std::string inner = inner_class_path(options.path, r_expr.text);
		if (!inner.empty()) {
			return class_value(r_expr, inner);
		}
		if (Checker *around = class_around(r_expr.text)) {
			return value_around(r_expr, *around);
		}
		if (engine.is_singleton(r_expr.text)) {
			r_expr.target = Target::Singleton;
			refer(r_expr.name_token, engine_id(r_expr.text));
			return set(r_expr, Type::object(r_expr.text));
		}
		if (const std::optional<Type> autoload = autoload_class(r_expr.text, r_expr.range)) {
			r_expr.target = Target::Autoload;
			refer(r_expr.name_token, autoload_id(r_expr.text));
			return set(r_expr, *autoload);
		}
		const std::string global = class_name_path(r_expr.text);
		if (is_script_path(global)) {
			return class_value(r_expr, global);
		}
		// GDScript's own constants, which declarations can hide, as in GDScript
		// (modules/gdscript/gdscript.cpp:2154-2157, gdscript_tokenizer.cpp:202-207; ed1daf0).
		static const std::pair<const char *, double> gdscript_constants[] = {
			{ "PI", 3.1415926535897932384626433833 },
			{ "TAU", 6.2831853071795864769252867666 },
			{ "INF", std::numeric_limits<double>::infinity() },
			{ "NAN", std::numeric_limits<double>::quiet_NaN() },
		};
		for (const auto &[name, value] : gdscript_constants) {
			if (r_expr.text == name) {
				r_expr.target = Target::EngineConstant;
				r_expr.float_value = value;
				refer(r_expr.name_token, engine_id("@GDScript", name));
				return set(r_expr, Type::of(TypeKind::Float));
			}
		}
		if (const std::optional<int64_t> value = builtin::global_constant(r_expr.text)) {
			r_expr.target = Target::EngineConstant;
			r_expr.int_value = *value;
			refer(r_expr.name_token, engine_id("@GlobalScope", r_expr.text));
			// Typed by its enum, such as Key of KEY_ESCAPE (spec §3.7).
			const builtin::Enum *of = builtin::global_constant_enum(r_expr.text);
			const std::optional<Type> type = of ? builtin_enum(of->name) : std::nullopt;
			return set(r_expr, type ? *type : Type::of(TypeKind::Int));
		}
		// Such as MouseButton, a global enum's name as a value (spec §3.7).
		if (const std::optional<Type> found = builtin_enum(r_expr.text)) {
			refer(r_expr.name_token, engine_id(r_expr.text));
			return use_engine_enum_dictionary(r_expr, *found);
		}
		if (is_type_name(r_expr.text)) {
			return fail(r_expr, "\"" + r_expr.text + "\" is a type, not a value.");
		}
		return fail(r_expr, "Identifier \"" + r_expr.text + "\" is not declared in the current scope.");
	}

	// A member or signal of an object of a class of this language, seen where
	// its class's declarations are (spec §4.2); nullopt when the class has no
	// such member, so that the engine class's are looked at.
	std::optional<Type> script_member(Expr &r_expr, const Type &p_class) {
		if (!is_script_path(p_class.script)) {
			return foreign_member(r_expr, p_class);
		}
		const ClassDecl *decl = script_decl(p_class.script);
		if (!decl) {
			return fail(r_expr, p_class.script + " has errors.");
		}
		if (p_class.is_interface) {
			if (const InterfaceDecl *interface = interface_decl(p_class)) {
				if (const auto message = broken(interface, p_class.name)) {
					return fail(r_expr, *message);
				}
			}
			const std::optional<InterfaceMember> member = interface_member(p_class, r_expr.text);
			if (!member) {
				return std::nullopt;
			}
			refer(r_expr.name_token, interface_member_id(*member));
			if (member->method) {
				return method_value(r_expr, *member->method);
			}
			if (member->signal) {
				// Of the class, which must declare it pub (spec §3.9, §10.5).
				r_expr.target = Target::ObjectSignal;
				r_expr.signal = member->signal;
				return Type::signal(param_types(member->signal->params));
			}
			// Read and written by name, whichever member or engine property provides it.
			r_expr.target = Target::ObjectProperty;
			r_expr.owner = p_class.engine_class();
			return member->property->type;
		}
		if (const std::optional<Type> constant = class_constant(r_expr, p_class.script, p_class.name)) {
			return constant;
		}
		// An inner class, reached through an object as constants are (spec §4.1).
		const std::string inner = inner_class_path(p_class.script, r_expr.text);
		if (!inner.empty()) {
			return class_value(r_expr, inner);
		}
		// A static variable, through an object as in GDScript, which is
		// evaluated first (spec §4.8).
		if (const std::optional<Type> variable = class_static(r_expr, p_class.script, p_class.name)) {
			r_expr.static_on_value = r_expr.target == Target::StaticVar;
			return variable;
		}
		for (const VarDecl *entry : decl->all_members) {
			const VarDecl &member = *entry;
			if (member.name != r_expr.text) {
				continue;
			}
			const std::string &owner = declaring(*decl, member, &ClassDecl::members).path;
			if (const auto message = refused(&member, member.is_pub || sees(owner), "", p_class.name, member.name)) {
				return fail(r_expr, *message);
			}
			if (member.type.is_error()) {
				// Typed from the scenes, which are not read for another script, or
				// not checked yet.
				return fail(r_expr, "The type of " + p_class.name + "." + member.name + " is not known here; declare it, as in `pub var " + member.name + ": T`.");
			}
			r_expr.target = Target::ObjectProperty;
			r_expr.owner = p_class.engine_class();
			r_expr.variable = &member;
			refer(r_expr.name_token, script_id(owner, member.name));
			return member.type;
		}
		for (const SignalDecl *entry : decl->all_signals) {
			const SignalDecl &signal = *entry;
			if (signal.name != r_expr.text) {
				continue;
			}
			// The engine knows every signal a class declares.
			if (const auto message = refused(&signal, signal.is_pub || sees(declaring(*decl, signal, &ClassDecl::signals).path), "The signal ", p_class.name, signal.name)) {
				return fail(r_expr, *message);
			}
			r_expr.target = Target::ObjectSignal;
			r_expr.signal = &signal;
			refer(r_expr.name_token, script_id(declaring(*decl, signal, &ClassDecl::signals).path, signal.name));
			return Type::signal(param_types(signal.params));
		}
		for (const FuncDecl *entry : decl->all_functions) {
			const FuncDecl &function = *entry;
			if (function.name != r_expr.text) {
				continue;
			}
			const std::string &owner = declaring(*decl, function, &ClassDecl::functions).path;
			if (const auto message = refused(&function, function.is_pub || function.is_override || sees(owner), "", p_class.name, function.name + "()")) {
				return fail(r_expr, *message);
			}
			refer(r_expr.name_token, script_id(owner, function.name));
			if (function.is_static) {
				// Through an object, which is evaluated first, as for a call (spec §4.8).
				r_expr.static_on_value = true;
				return static_value(r_expr, function, owner);
			}
			return method_value(r_expr, function);
		}
		return std::nullopt;
	}

	// `Class.name` of the class of this language at `p_path`: a constant, a
	// static variable, an inner class, or a static function as a value
	// (spec §3.11, §4.1, §4.8).
	Type class_member_value(Expr &r_expr, const std::string &p_path, const std::string &p_class) {
		if (const std::optional<Type> constant = class_constant(r_expr, p_path, p_class)) {
			return *constant;
		}
		if (const std::optional<Type> variable = class_static(r_expr, p_path, p_class)) {
			return *variable;
		}
		const std::string inner = inner_class_path(p_path, r_expr.text);
		if (!inner.empty()) {
			return class_value(r_expr, inner);
		}
		if (r_expr.text == "new") {
			return new_value(r_expr, p_path);
		}
		const ClassDecl *decl = script_decl(p_path); // class_static() reports one with errors.
		for (const FuncDecl *entry : decl->all_functions) {
			const FuncDecl &function = *entry;
			if (function.name != r_expr.text) {
				continue;
			}
			const std::string &owner = declaring(*decl, function, &ClassDecl::functions).path;
			if (!function.is_static) {
				return fail(r_expr, "\"" + function.name + "()\" of " + p_class + " is not static; take it from an object.");
			}
			if (const auto message = refused(&function, function.is_pub || sees(owner), "", p_class, function.name + "()")) {
				return fail(r_expr, *message);
			}
			refer(r_expr.name_token, script_id(owner, function.name));
			return static_value(r_expr, function, owner);
		}
		return fail(r_expr, missing(p_class + " has no constant, static variable or static function \"" + r_expr.text + "\".", p_path));
	}

	// `Class.new` as a value (spec §3.11), which constructs as new(...) does,
	// with the arguments of the nearest _init() and its visibility (§4.2).
	Type new_value(Expr &r_expr, const std::string &p_path) {
		const ClassDecl &header = *script_header(p_path); // class_static() found its declarations.
		const FuncDecl *init = nullptr;
		if (!find_init(r_expr, header, p_path, init)) {
			return r_expr.type;
		}
		refer(r_expr.name_token, script_id(p_path, "new"));
		r_expr.target = Target::NewValue;
		r_expr.callee = init;
		r_expr.owner = p_path;
		Type type = init ? function_type(*init) : Type::function(Type::of(TypeKind::Void), {});
		if (type.kind == TypeKind::Function) {
			type.args[0] = script_class_type(header, p_path);
		}
		return set(r_expr, type);
	}

	// A static function of this language as a value (spec §3.11), called on the
	// current image of the class at `p_owner` (§12.4).
	Type static_value(Expr &r_expr, const FuncDecl &p_function, const std::string &p_owner) {
		r_expr.target = Target::StaticValue;
		r_expr.callee = &p_function;
		r_expr.owner = p_owner;
		return set(r_expr, function_type(p_function));
	}

	// A method of this language as a value (spec §3.11), found by name on the
	// object's script when it is called.
	Type method_value(Expr &r_expr, const FuncDecl &p_method) {
		if (!receiver_kept(r_expr)) {
			return Type::of(TypeKind::Error);
		}
		r_expr.target = Target::MethodValue;
		r_expr.callee = &p_method;
		return function_type(p_method);
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

	// A method, property or signal of an interface, its own or one it
	// inherits, and the interface that declares it (spec §3.9).
	struct InterfaceMember {
		const FuncDecl *method = nullptr;
		const InterfaceProperty *property = nullptr;
		const SignalDecl *signal = nullptr;
		Type owner;

		const std::string &name() const { return method ? method->name : property ? property->name
																				  : signal->name; }
	};

	// The interfaces from `p_roots` up through those they extend, each once
	// and before those it extends.
	std::vector<Type> interface_order(const std::vector<Type> &p_roots) const {
		std::vector<Type> order;
		std::unordered_set<std::string> seen;
		const std::function<void(const Type &)> visit = [&](const Type &p_interface) {
			if (!seen.insert(p_interface.script + "\n" + p_interface.name).second) {
				return;
			}
			if (const InterfaceDecl *decl = interface_decl(p_interface)) {
				for (const Type &parent : decl->parents) {
					visit(parent);
				}
			}
			order.push_back(p_interface);
		};
		for (const Type &root : p_roots) {
			visit(root);
		}
		std::reverse(order.begin(), order.end());
		return order;
	}

	// The members of the interfaces, each name once: as the interface nearest
	// the roots declares it, so that a redeclaration with a narrower return
	// type comes first; of two properties, the one that must be writable.
	std::vector<InterfaceMember> interface_members(const std::vector<Type> &p_roots) const {
		std::vector<InterfaceMember> members;
		for (const Type &owner : interface_order(p_roots)) {
			const InterfaceDecl *decl = interface_decl(owner);
			if (!decl) {
				continue;
			}
			const auto add = [&](InterfaceMember p_member) {
				p_member.owner = owner;
				for (InterfaceMember &known : members) {
					if (known.name() == p_member.name()) {
						if (known.property && p_member.property && known.property->is_readonly && !p_member.property->is_readonly) {
							known = p_member;
						}
						return;
					}
				}
				members.push_back(p_member);
			};
			for (const FuncDecl &method : decl->methods) {
				add({ &method });
			}
			for (const InterfaceProperty &property : decl->properties) {
				add({ nullptr, &property });
			}
			for (const SignalDecl &signal : decl->signals) {
				add({ nullptr, nullptr, &signal });
			}
		}
		return members;
	}

	std::optional<InterfaceMember> interface_member(const Type &p_interface, const std::string &p_name) const {
		for (const InterfaceMember &member : interface_members({ p_interface })) {
			if (member.name() == p_name) {
				return member;
			}
		}
		return std::nullopt;
	}

	// The id of an interface's member, at the interface that declares it.
	static DeclId interface_member_id(const InterfaceMember &p_member) {
		return script_id(p_member.owner.script, p_member.owner.interface_name() + "." + p_member.name());
	}

	const InterfaceProperty *interface_property(const Type &p_interface, const std::string &p_name) const {
		const std::optional<InterfaceMember> member = interface_member(p_interface, p_name);
		return member ? member->property : nullptr;
	}

	// A pub method of an object of a class of this language, or a method of an
	// interface, which this language calls directly (spec §8.1), also when it
	// overrides an engine virtual method.
	std::optional<Type> script_method(Expr &r_expr, const Type &p_class) {
		if (!p_class.is_interface && !is_script_path(p_class.script)) {
			return foreign_method(r_expr, p_class);
		}
		const FuncDecl *callee = nullptr;
		const SourceFile *source = nullptr; // Of the callee.
		DeclId id;
		std::string owner = p_class.script; // The class that declares it.
		if (p_class.is_interface) {
			if (const InterfaceDecl *interface = interface_decl(p_class)) {
				if (const auto message = broken(interface, p_class.name)) {
					return fail_call(r_expr, *message);
				}
				const std::optional<InterfaceMember> member = interface_member(p_class, r_expr.text);
				if (member && member->method) {
					callee = member->method;
					source = script_decl(member->owner.script)->source.get();
					id = interface_member_id(*member);
				}
			}
		} else if (const ClassDecl *decl = script_decl(p_class.script)) {
			for (const FuncDecl *entry : decl->all_functions) {
				const FuncDecl &function = *entry;
				if (function.name == r_expr.text) {
					const ClassDecl &declarer = declaring(*decl, function, &ClassDecl::functions);
					callee = &function;
					source = declarer.source.get();
					owner = declarer.path;
					id = script_id(owner, function.name);
				}
			}
		} else {
			return fail(r_expr, p_class.script + " has errors.");
		}
		if (!callee) {
			return std::nullopt;
		}
		if (const auto message = refused(callee, callee->is_pub || callee->is_override || sees(owner), "", p_class.name, callee->name + "()")) {
			return fail_call(r_expr, *message);
		}
		const Type result = call_declared(r_expr, *callee, *source, callee->return_type);
		refer(r_expr.name_token, id);
		r_expr.target = Target::ScriptMethod;
		return result;
	}

	// A call to a function of this language once it is found (spec §4.6): its
	// hint, showing `p_result`, its arguments, and the defaults it is left to
	// fill when called by name.
	Type call_declared(Expr &r_expr, const FuncDecl &p_callee, const SourceFile &p_source, const Type &p_result) {
		hint(r_expr, [&](int p_current) { return declared_signature(r_expr.text, p_callee, p_source, p_current, p_result); });
		const std::vector<Type> params = param_types(p_callee.params);
		const bool rest = p_callee.has_rest();
		const std::vector<Type> fixed(params.begin(), params.end() - (rest ? 1 : 0));
		check_arguments(r_expr, fixed, required_params(p_callee), rest, rest ? rest_element(params.back()) : Type::of(TypeKind::Variant));
		r_expr.engine_params = params;
		r_expr.index = int(fixed.size()) - required_params(p_callee);
		r_expr.is_vararg = rest;
		r_expr.is_async = p_callee.is_async;
		return set(r_expr, p_callee.is_async ? Type::task(p_result) : p_result);
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
		// The class that declares it, also when named through a subclass.
		std::string owner = p_class;
		for (std::string up = engine.parent_class(owner); !up.empty() && engine.find_constant(up, r_expr.text); up = engine.parent_class(up)) {
			owner = up;
		}
		refer(r_expr.name_token, engine_id(owner, r_expr.text));
		return set(r_expr, engine.constant_type(p_class, r_expr.text));
	}

	Type check_member_access(Expr &r_expr) {
		Expr &base = *r_expr.base;
		if (base.kind == ExprKind::Self && !structure) {
			if (const std::optional<Type> found = check_self_name(r_expr)) {
				return *found;
			}
			return refuse_in_static(base, "self") ? set(r_expr, Type::of(TypeKind::Error)) : fail(r_expr, display_name(cls, options.path) + " has no member \"" + r_expr.text + "\".");
		}
		if (const EnumDecl *decl = named_enum(base)) {
			return check_enum_value(r_expr, *decl);
		}
		if (const std::optional<Type> engine_enum = named_engine_enum_base(base)) {
			return check_engine_enum_value(r_expr, *engine_enum);
		}
		if (names_enum_of_class(base, r_expr.text)) {
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
				return r_expr.text == "new" ? check_foreign_class_call(r_expr, *named, true) : check_foreign_constant(r_expr, *named);
			}
			return class_member_value(r_expr, named->script, named->name);
		}
		if (names_type(base)) {
			refer(base.name_token, engine_id(base.text));
			// Such as Tween.TransitionType or Vector3.Axis, an enum's name as a value (spec §3.7).
			if (const std::optional<Type> found = named_engine_enum(base.text + "." + r_expr.text)) {
				refer(r_expr.name_token, engine_id(base.text, r_expr.text));
				return use_engine_enum_dictionary(r_expr, *found);
			}
			if (const builtin::Class *type = builtin::find_class(base.text)) {
				if (const builtin::Constant *constant = builtin::find_constant(*type, r_expr.text)) {
					r_expr.target = Target::BuiltinConstant;
					r_expr.owner = base.text;
					refer(r_expr.name_token, engine_id(base.text, r_expr.text));
					return set(r_expr, api_type(constant->type, false));
				}
				// A value of one of its enums, such as Vector3.AXIS_X (spec §3.7).
				if (const builtin::Enum *of = builtin::find_class_enum(base.text, r_expr.text, r_expr.int_value)) {
					r_expr.target = Target::EngineConstant;
					refer(r_expr.name_token, engine_id(base.text, r_expr.text));
					const std::optional<Type> type = builtin_enum(of->name);
					return set(r_expr, type ? *type : Type::of(TypeKind::Int));
				}
				return fail(r_expr, base.text + " has no constant \"" + r_expr.text + "\".");
			}
			if (r_expr.text == "new") {
				return fail(r_expr, base.text + ".new is not a value until engine classes are values; write a lambda, as func(): return " + base.text + ".new().");
			}
			if (const char *getter = builtin::error_getter(base.text, r_expr.text)) {
				const auto method = engine.find_method(base.text, std::string(builtin::tried_method(r_expr.text)));
				const auto error_getter = engine.find_method(base.text, getter);
				if (method && error_getter) {
					return tried_value(r_expr, *method, *error_getter);
				}
			}
			if (const auto method = engine.find_method(base.text, r_expr.text)) {
				if (!method->is_static) {
					return fail(r_expr, "\"" + r_expr.text + "()\" of " + base.text + " is not static; take it from an object.");
				}
				return engine_method_value(r_expr, *method);
			}
			return check_engine_constant(r_expr, base.text);
		}
		if ((base.kind == ExprKind::Member || base.kind == ExprKind::Name) && !visible_local(base.text)) {
			if (const std::optional<Type> named = constructed_struct(base)) {
				return named->is_error() ? set(r_expr, *named) : struct_method_value(r_expr, *named, false);
			}
		}

		const Type base_type = check_value(base);
		if (base_type.is_error()) {
			return set(r_expr, base_type);
		}
		if (base.target == Target::ClassScript) {
			// An inner class reached through an object (spec §4.1).
			return class_member_value(r_expr, base.owner, display_name(*script_header(base.owner), base.owner));
		}
		if (base_type.is_optional()) {
			return fail(r_expr, "The value may be None (" + base_type.to_string() + "); check it before reading \"" + r_expr.text + "\".");
		}
		if (base_type.kind == TypeKind::Variant) {
			return fail(r_expr, "Cannot read \"" + r_expr.text + "\" of a Variant." + variant_hint());
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
			if (decl->find_method(r_expr.text)) {
				return struct_method_value(r_expr, base_type, true);
			}
			return fail(r_expr, missing(base_type.name + " has no field \"" + r_expr.text + "\".", base_type.script));
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
				if (const auto method = engine.find_method(engine_class, r_expr.text)) {
					return engine_method_value(r_expr, *method);
				}
				return fail(r_expr, missing(base_type.name + " has no property \"" + r_expr.text + "\".", base_type.script));
			}
			r_expr.target = Target::ObjectProperty;
			r_expr.owner = engine_class;
			refer(r_expr.name_token, engine_id(property->owner, r_expr.text));
			return set(r_expr, property_type(*property, r_expr));
		}
		if (base_type.kind == TypeKind::PayloadEnum && !base_type.is_result()) {
			// As the variant that has such a field writes it.
			std::string example;
			for (size_t v = 0; v < base_type.values->fields.size() && example.empty(); v++) {
				for (const auto &[name, type] : base_type.values->fields[v]) {
					if (name == r_expr.text) {
						example = ", as in " + enum_spelling(base_type) + "." + base_type.values->list[v].first + "(var " + name + ", ..)";
					}
				}
			}
			return fail(r_expr, "The fields of a variant are read by match" + example + ".");
		}
		return fail(r_expr, "Values of type " + base_type.to_string() + " have no members.");
	}

	// A node path as written: $Head/Camera, %Name or $"../Level".
	static std::string shown_path(const std::string &p_path) {
		if (p_path[0] == '%') {
			return p_path;
		}
		return plain_path(p_path) ? "$" + p_path : "$\"" + p_path + "\"";
	}

	// Whether `$` takes a path without quotes: names joined by "/", each
	// possibly after "%".
	static bool plain_path(const std::string &p_path) {
		for (const std::string &part : path_parts(p_path)) {
			const size_t name = !part.empty() && part[0] == '%' ? 1 : 0;
			if (part.size() == name || std::isdigit((unsigned char)part[name]) || !std::all_of(part.begin() + name, part.end(), [](char p_char) { return std::isalnum((unsigned char)p_char) || p_char == '_'; })) {
				return false;
			}
		}
		return true;
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

	// The nodes of a scene as the engine builds it (spec §7.1): its own, with
	// the nodes of each scene it instances under the instance, whose root owns
	// them, and the changes it stores for those
	// (scene/resources/packed_scene.cpp:282-300, 567-573; ed1daf0).
	const std::vector<ViewNode> &view_of(const Scene &p_scene, int p_depth = 0) {
		const auto known = views.find(p_scene.path);
		if (known != views.end()) {
			return known->second;
		}
		std::vector<ViewNode> nodes;
		for (const SceneNode &node : p_scene.nodes) {
			if (node.type.empty() && !node.is_instance) {
				continue; // A change of an instanced scene's node, applied below.
			}
			nodes.push_back({ node.path, node, p_scene.path, node.path == "." ? std::string() : std::string(".") });
			// A scene cannot instance itself; the depth stops a project that says so.
			const std::optional<Scene> inner = node.instance.empty() || p_depth > 32 ? std::nullopt : classes.scene_structure(node.instance);
			for (const ViewNode &each : inner ? view_of(*inner, p_depth + 1) : std::vector<ViewNode>()) {
				if (each.path != ".") {
					nodes.push_back({ node.path + "/" + each.path, each.node, each.scene, each.owner == "." ? node.path : node.path + "/" + each.owner });
				}
			}
		}
		for (const SceneNode &change : p_scene.nodes) {
			const auto changed = std::find_if(nodes.begin(), nodes.end(), [&](const ViewNode &p_node) { return p_node.path == change.path; });
			if (change.type.empty() && !change.is_instance && !change.script.empty() && changed != nodes.end()) {
				changed->node.script = change.script;
				changed->node.instance_script.clear();
			}
		}
		return views.emplace(p_scene.path, std::move(nodes)).first->second;
	}

	// Where a path leads from the node at `p_from` of a scene, part by part, as
	// the engine resolves it (scene/main/node.cpp:1924-1961; ed1daf0). `%Name`
	// is a node the current node owns, or else one its owner does.
	struct NodeWalk {
		std::string scene;
		std::vector<const ViewNode *> reached; // The node after each part.
		const ViewNode *node = nullptr; // Where it ends, unless:
		std::string missing; // the scene has no such node;
		// or it leads past the root, out to the scenes that instance this one:
		// above it, or to a unique name that the root's owner gives.
		bool leaves = false;
	};

	const ViewNode *view_node(const Scene &p_scene, const std::function<bool(const ViewNode &)> &p_is) {
		const std::vector<ViewNode> &view = view_of(p_scene);
		const auto found = std::find_if(view.begin(), view.end(), p_is);
		return found == view.end() ? nullptr : &*found;
	}

	const ViewNode *view_node(const Scene &p_scene, const std::string &p_path) {
		return view_node(p_scene, [&](const ViewNode &p_node) { return p_node.path == p_path; });
	}

	NodeWalk walk_node_path(const Scene &p_scene, const std::string &p_from, const std::string &p_path) {
		const auto find = [&](const std::function<bool(const ViewNode &)> &p_is) { return view_node(p_scene, p_is); };
		const auto at_path = [&](const std::string &p_at) { return view_node(p_scene, p_at); };
		NodeWalk walk;
		walk.scene = p_scene.path;
		walk.missing = p_scene.path + ": " + (p_from == "." ? std::string("the root") : p_from) + " has no node at \"" + p_path + "\".";
		const ViewNode *at = at_path(p_from);
		for (const std::string &part : path_parts(p_path)) {
			if (!at) {
				return walk;
			}
			if (part == "..") {
				if (at->path == ".") {
					walk.leaves = true;
					return walk;
				}
				at = at_path(parent_path(at->path));
			} else if (!part.empty() && part[0] == '%') {
				const std::string name = part.substr(1);
				const auto owned_by = [&](const std::string &p_owner) {
					return find([&](const ViewNode &p_node) { return p_node.node.unique && p_node.owner == p_owner && p_node.path.substr(p_node.path.rfind('/') + 1) == name; });
				};
				const ViewNode *unique = owned_by(at->path);
				if (!unique && at->owner.empty()) {
					walk.leaves = true;
					return walk;
				}
				at = unique ? unique : owned_by(at->owner);
				if (!at) {
					walk.missing = p_scene.path + ": no node has the unique name " + part + ".";
				}
			} else if (!part.empty() && part != ".") {
				at = at_path(at->path == "." ? part : at->path + "/" + part);
			}
			walk.reached.push_back(at);
		}
		walk.node = at;
		return walk;
	}

	// Each node a path leads to from a node of a scene that has the script
	// (spec §7.1): in that scene, or, past its root, in each scene that
	// instances it, as it is there. A scene that lacks the node has the error
	// marked on it, at `p_marked`, unless the path is optional or the scene
	// is used only as the base of inherited scenes (spec §7.2).
	void walk_from(const Expr &p_at, const Scene &p_scene, const std::string &p_from, const std::string &p_marked, const std::string &p_path, const std::string &p_shown, bool p_optional, std::vector<NodeWalk> &r_found, int p_depth = 0) {
		NodeWalk walk = walk_node_path(p_scene, p_from, p_path);
		if (walk.node) {
			r_found.push_back(std::move(walk));
			return;
		}
		std::string missing = walk.missing;
		const std::vector<Scene> outers = p_depth > 32 ? std::vector<Scene>() : classes.scenes_instancing(p_scene.path);
		bool instanced = false;
		for (const Scene &outer : walk.leaves ? outers : std::vector<Scene>()) {
			for (const SceneNode &node : outer.nodes) {
				if (node.instance == p_scene.path) {
					instanced = true;
					walk_from(p_at, outer, p_from == "." ? node.path : node.path + "/" + p_from, node.path, p_path, p_shown, p_optional, r_found, p_depth + 1);
				}
			}
		}
		if (walk.leaves) {
			missing = p_scene.path + ": " + p_shown + " leads out of the scene, and no scene instances it.";
		}
		// Such as $"../Music" for an autoload beside the main scene (spec §7.3).
		std::string named = p_path;
		while (named.rfind("../", 0) == 0 || named.rfind("./", 0) == 0) {
			named = named.substr(named.find('/') + 1);
		}
		const std::string way = autoload_way(named);
		if (!way.empty()) {
			missing += " " + way.substr(0, way.find('.')) + " is an autoload; write " + way + ".";
		}
		const bool only_inherited = !outers.empty() && p_scene.path != options.project->main_scene() && std::all_of(outers.begin(), outers.end(), [&](const Scene &p_outer) {
			return p_outer.base == p_scene.path && std::none_of(p_outer.nodes.begin(), p_outer.nodes.end(), [&](const SceneNode &p_node) { return p_node.instance == p_scene.path; });
		});
		if (!instanced && !p_optional && !only_inherited) {
			error_in_scene(p_at.range, p_scene, p_marked, missing);
		}
	}

	// Why a literal path from the root into an autoload is a compile error:
	// the autoload is reached by its name (spec §7.3). Empty for another path.
	std::string through_autoload(const std::string &p_path, const std::string &p_written) const {
		const std::string root = "/root/";
		const std::string way = p_path.rfind(root, 0) == 0 ? autoload_way(p_path.substr(root.size())) : std::string();
		if (way.empty()) {
			return std::string();
		}
		const std::string name = way.substr(0, way.find('.'));
		return p_written + (way == name ? " is the autoload " : " is in the autoload ") + name + "; write " + way + ".";
	}

	// How to reach what a path names when its first name is an autoload's:
	// the autoload, or get_node() of the rest on it. Empty for another path.
	std::string autoload_way(const std::string &p_path) const {
		const size_t end = p_path.find('/');
		const std::string name = p_path.substr(0, end);
		if (name.empty() || !options.project || options.project->autoload_path(name).empty()) {
			return std::string();
		}
		return end == std::string::npos ? name : name + ".get_node(\"" + p_path.substr(end + 1) + "\")";
	}

	// `$X` (spec §7.1): the node at the path from every node that has this
	// script, in every scene; they must agree on its class. `$".."` is any
	// Node, or what @parent(T) requires (spec §7.3).
	Type check_get_node(Expr &r_expr) {
		if (options.declarations_only) {
			return set(r_expr, Type::of(TypeKind::Error)); // Scenes are not read while loading.
		}
		const std::string &path = r_expr.text;
		if (refuse_in_static(r_expr, shown_path(path))) {
			return set(r_expr, Type::of(TypeKind::Error));
		}
		// Through the MethodBind, as GDScript calls it (modules/gdscript/gdscript_compiler.cpp:742-753).
		if (const std::optional<EngineMethod> get_node = engine.find_method("Node", "get_node")) {
			r_expr.hash = get_node->hash;
		}
		if (path == "..") {
			const Type parent = cls.parent_ref ? cls.parent_type : Type::object("Node");
			r_expr.owner = parent.engine_class();
			return set(r_expr, parent);
		}
		const std::string autoload = through_autoload(path, shown_path(path));
		if (!autoload.empty()) {
			return fail(r_expr, autoload);
		}
		if (path.empty()) {
			return fail(r_expr, "Only paths from this node, such as $Head/Camera, are supported yet.");
		}
		// A node that code put under the root, as get_node() types it (spec §7.3).
		if (path[0] == '/') {
			r_expr.owner = "Node";
			return set(r_expr, Type::object("Node"));
		}
		const Type type = scene_node_at(r_expr, shown_path(path), false);
		r_expr.owner = type.engine_class();
		return set(r_expr, type);
	}

	// The node at a literal path from this node, `p_path_expr` holding the
	// path, as `$X` and get_node() name it (spec §7.1, §7.2): its type in every
	// scene that gives this script a node, which must agree. With `p_optional`,
	// for get_node_or_null(), a scene may lack it and the type is T?, or Node?
	// when no scene has it. Error after reporting.
	Type scene_node_at(const Expr &p_path_expr, const std::string &p_shown, bool p_optional) {
		const std::string &path = p_path_expr.text;
		bool attached = false;
		std::vector<NodeWalk> found;
		for (const Scene &scene : scenes) {
			for (const SceneNode &node : scene.nodes) {
				if (extends_script(node.script, options.path)) {
					attached = true;
					walk_from(p_path_expr, scene, node.path, node.path, path, p_shown, p_optional, found);
				}
			}
		}
		std::optional<Type> type;
		std::string type_scene;
		for (const NodeWalk &walk : found) {
			refer_node_path(p_path_expr, walk);
			const Type found_type = scene_node_type(p_path_expr.range, walk.node->node, p_shown);
			if (found_type.is_error()) {
				return found_type;
			}
			if (!type) {
				type = found_type;
				type_scene = walk.scene;
			} else if (found_type != *type) {
				error(p_path_expr.range, p_shown + " is a " + type->to_string() + " in " + type_scene + " but a " + found_type.to_string() + " in " + walk.scene + "; assign it to a variable typed with a class both extend.");
				return Type::of(TypeKind::Error);
			}
		}
		if (!type && p_optional) {
			return Type::object("Node", true);
		}
		if (!attached) {
			error(p_path_expr.range, p_shown + " is typed from the scenes that use this script, and no scene does.");
		}
		if (!type) {
			return Type::of(TypeKind::Error); // Reported for each scene above.
		}
		Type result = *type;
		result.nullable = p_optional;
		return result;
	}

	// get_node() and get_node_or_null() of a literal path from this node,
	// typed as `$X` (spec §7.1, §7.2); get_node("..") as `$".."`, and
	// get_node_or_null("..") as the engine types it (§7.3). nullopt for any
	// other call, and for a path from the root.
	std::optional<Type> literal_node_type(const Expr &p_call, const EngineMethod &p_method) {
		const bool optional = p_method.name == "get_node_or_null";
		if (p_method.owner != "Node" || (p_method.name != "get_node" && !optional) || p_call.args.size() != 1 || p_call.args[0]->kind != ExprKind::StringLiteral || (p_call.base && p_call.base->kind != ExprKind::Self)) {
			return std::nullopt;
		}
		const std::string &path = p_call.args[0]->text;
		if (path.empty() || path[0] == '/' || (optional && path == "..")) {
			return std::nullopt;
		}
		if (options.declarations_only) {
			return Type::of(TypeKind::Error); // Scenes are not read while loading.
		}
		if (path == "..") {
			return cls.parent_ref ? cls.parent_type : Type::object("Node");
		}
		return scene_node_at(*p_call.args[0], p_call.text + "(\"" + path + "\")", optional);
	}

	// Each name in the path of `$X` refers to the node it reaches in a scene;
	// the name of `%Name` without the "%". Not the parts of a string written
	// with escapes.
	void refer_node_path(const Expr &p_get_node, const NodeWalk &p_walk) {
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
				refer_bytes(parts[i]->first, parts[i]->second, { DeclId::Kind::Node, p_walk.reached[i]->scene, p_walk.reached[i]->node.path });
			}
		}
	}

	// The type of a node of a scene (spec §7.1, §7.4): the class of its script
	// when that is a script of this language, or else its engine class; Error
	// after reporting. `p_what` names the node in messages.
	Type scene_node_type(const TokenRange &p_at, const SceneNode &p_node, const std::string &p_what) {
		const std::string &script = p_node.script.empty() ? p_node.instance_script : p_node.script;
		if (is_script_path(script)) {
			const ClassDecl *header = script_header(script);
			if (!header) {
				error(p_at, p_what + " has the script " + script + ", which cannot be read.");
				return Type::of(TypeKind::Error);
			}
			return script_class_type(*header, script);
		}
		// A script of another language, by its class as reflection shows it
		// (spec §11.1); by the engine class when it cannot be read.
		const ForeignClass *foreign = script.empty() ? nullptr : foreign_class(script);
		if (foreign && foreign->members_known && !foreign->native.empty()) {
			return foreign_class_type(script, *foreign);
		}
		if (!engine.class_exists(p_node.type)) {
			error(p_at, p_what + " is a " + p_node.type + ", which is not supported yet.");
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
		return resolved_path(p_call.args[0]->text, options.path);
	}

	// A preload whose file loads this script again is a compile error: the
	// class loads its preloads as it loads, which that file would wait for
	// (spec §7.6).
	void check_load_cycle(const Expr &p_preload, const std::string &p_file) {
		std::set<std::string> seen;
		std::vector<std::string> chain;
		if (!loads_this_script(p_file, seen, chain)) {
			return;
		}
		std::string cycle;
		for (const std::string &file : chain) {
			cycle += file + " -> ";
		}
		error(p_preload.range, "This preload loads this script again (" + cycle + script_file(options.path) + "); load one of them with load() where it is used.");
	}

	bool loads_this_script(const std::string &p_file, std::set<std::string> &r_seen, std::vector<std::string> &r_chain) {
		if (p_file == script_file(options.path)) {
			return true;
		}
		if (p_file.empty() || !r_seen.insert(p_file).second) {
			return false;
		}
		r_chain.push_back(p_file);
		for (const std::string &next : loaded_with(p_file)) {
			if (loads_this_script(next, r_seen, r_chain)) {
				return true;
			}
		}
		r_chain.pop_back();
		return false;
	}

	// What loading a file loads with it: a scene's or a resource's
	// ext_resources; a script's base and the files it preloads, which GDScript
	// loads as it compiles the script and this language as the class loads.
	const std::vector<std::string> &loaded_with(const std::string &p_file) {
		const auto known = loaded_files.find(p_file);
		if (known != loaded_files.end()) {
			return known->second;
		}
		std::vector<std::string> files;
		if (!is_script_file(p_file)) {
			files = options.project->dependencies(p_file);
		} else {
			if (has_extension(p_file, { ".pet" })) {
				if (const ClassDecl *header = classes.header(p_file)) {
					files.push_back(script_file(classes.base_path(*header, p_file)));
				}
			} else if (const std::optional<ForeignClass> foreign = options.project->foreign_class(p_file, false)) {
				files.push_back(foreign->base);
			}
			for (const std::string &path : classes.preloads(p_file)) {
				files.push_back(preload_file(resolved_path(path, p_file)));
			}
		}
		return loaded_files.emplace(p_file, std::move(files)).first->second;
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

	static bool is_scene_file(const std::string &p_file) {
		return has_extension(p_file, { ".tscn", ".scn" });
	}

	// The type of the resource in a file other than a script, by what it holds
	// (spec §7.4): a scene is PackedScene[T], T its root's type; any other
	// resource the class its file loads as, or its script's class. nullopt
	// when the project has no such file.
	std::optional<Type> content_type(const Expr &p_at, const std::string &p_file) {
		if (is_scene_file(p_file)) {
			const std::optional<Scene> scene = options.project ? options.project->scene(p_file) : std::nullopt;
			const SceneNode *root = scene ? scene->find_node(".") : nullptr;
			if (!root) {
				return std::nullopt;
			}
			const Type root_type = scene_node_type(p_at.range, *root, "The root of " + p_file);
			return root_type.is_error() ? root_type : Type::scene(root_type);
		}
		const std::optional<ResourceInfo> resource = options.project ? options.project->resource(p_file) : std::nullopt;
		if (!resource) {
			return std::nullopt;
		}
		return resource_type(p_at, p_file, *resource);
	}

	// The file that load() of one literal path names; "" for another path.
	std::string literal_load_file(const Expr &p_call) const {
		if (p_call.args.size() != 1 || p_call.args[0]->kind != ExprKind::StringLiteral || p_call.args[0]->op != TokenKind::String) {
			return std::string();
		}
		const std::string &written = p_call.args[0]->text;
		if (written.find("://") != std::string::npos && written.rfind("res://", 0) != 0 && written.rfind("uid://", 0) != 0) {
			return std::string(); // Such as user://.
		}
		return preload_file(resolved_path(written, "res://"));
	}

	// The scene that a PackedScene[T] is statically known to be (spec §7.4):
	// that of preload(), of load() of a literal path, or of a constant one of
	// them initializes; nullopt for another.
	std::optional<Scene> known_scene(const Expr &p_value) {
		std::string file;
		if (p_value.constant && p_value.constant->kind == ConstValue::Kind::Preload) {
			file = preload_file(p_value.constant->text);
		} else if (p_value.kind == ExprKind::Call && p_value.text == "load" && p_value.base && p_value.base->text == "ResourceLoader") {
			file = literal_load_file(p_value); // load(), checked as ResourceLoader.load().
		}
		return is_scene_file(file) && options.project && !options.declarations_only ? options.project->scene(file) : std::nullopt;
	}

	// load() of one literal path in the project, typed as preload() types it
	// (spec §7.4), but a script as the resource it is, not its class (§2.1).
	// A relative path is from res://, as the engine resolves it
	// (core/io/resource_loader.cpp:693-694, ed1daf0). nullopt for any other
	// path, and for a file the project does not have, which the engine's
	// Resource? types.
	std::optional<Type> literal_load(const Expr &p_call) {
		const std::string file = literal_load_file(p_call);
		if (file.empty()) {
			return std::nullopt;
		}
		if (is_script_file(file)) {
			return options.project && options.project->resource(file) ? std::optional<Type>(script_value_type(file)) : std::nullopt;
		}
		if (options.declarations_only) {
			return Type::of(TypeKind::Error); // Resources are not read while loading.
		}
		return content_type(p_call, file);
	}

	// preload() of a resource (spec §7.4): a scene is PackedScene[T], T its
	// root's type; a script its class (§2.1); any other resource the class its
	// file loads as, or its script's class. It is a constant, which the class
	// loads as it loads, as GDScript loads it as it compiles the script
	// (modules/gdscript/gdscript_analyzer.cpp:4723-4780).
	Type check_preload(Expr &r_expr) {
		if (completion && r_expr.args.size() == 1 && r_expr.args[0]->kind == ExprKind::StringLiteral && at_cursor(r_expr.args[0]->text)) {
			complete_string(nullptr, 0, false);
			return set(r_expr, Type::of(TypeKind::Error));
		}
		const std::string path = preload_path(r_expr);
		if (path.empty()) {
			return fail_call(r_expr, "preload() takes one literal path, a String.");
		}
		r_expr.args[0]->text = path;
		// A constant expression (spec §4.5, §7.4).
		const auto loaded = [&](const Type &p_type) {
			auto value = std::make_shared<ConstValue>();
			value->kind = ConstValue::Kind::Preload;
			value->type = p_type;
			value->text = path;
			return value;
		};
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
			// Resources are not read while loading, so its type is unknown here;
			// a constant still holds it, for GDScript to read (spec §11.3).
			r_expr.constant = loaded(Type::object("Resource"));
			return set(r_expr, Type::of(TypeKind::Error));
		} else {
			const std::optional<Type> content = content_type(r_expr, file);
			if (!content) {
				return fail(r_expr, std::string(is_scene_file(file) ? "There is no scene at " : "There is no resource at ") + file + ".");
			}
			if (content->is_error()) {
				return set(r_expr, *content);
			}
			type = *content;
		}
		if (options.project && !options.declarations_only) {
			check_load_cycle(r_expr, file);
		}
		refer(r_expr.name_token, language_id("@GlobalScope", "preload"));
		check_value(*r_expr.args[0]);
		const auto value = loaded(type);
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
				error(p_preload.range, "There is no script of this language at " + p_file + ".");
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
		if (r_expr.op != TokenKind::Minus && r_expr.op != TokenKind::Tilde) {
			if (operand.kind == TypeKind::Bool) {
				return set(r_expr, operand);
			}
			return fail(r_expr, "\"not\" requires bool, found " + operand.to_string() + "." + compare_hint(operand, true));
		}
		if (operand.kind == TypeKind::Variant) {
			return fail(r_expr, std::string("Cannot use ") + token_name(r_expr.op) + " on a Variant." + variant_hint());
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
		return operand.kind == TypeKind::Int ? set(r_expr, operand) : fail(r_expr, "\"~\" requires int, found " + operand.to_string() + ".");
	}

	// How to write what GDScript's truth test of a value means: `x != 0`, or
	// for `not x` `x == 0` (spec §5.4); empty for a type it has none for.
	static std::string compare_hint(const Type &p_type, bool p_negated) {
		const std::string op = p_negated ? " == " : " != ";
		if (p_type.is_numeric()) {
			return " Compare explicitly, for example x" + op + "0.";
		}
		if (p_type.kind == TypeKind::String) {
			return " Compare explicitly, for example x" + op + "\"\".";
		}
		if (p_type.is_optional() || p_type.kind == TypeKind::Variant) {
			return " Compare explicitly, for example x" + op + "None.";
		}
		if (p_type.kind == TypeKind::Builtin && (p_type.name == "Array" || p_type.name == "Dictionary" || p_type.name.rfind("Packed", 0) == 0)) {
			return std::string(" Test it explicitly, for example ") + (p_negated ? "x.is_empty()." : "not x.is_empty().");
		}
		return std::string();
	}

	// The result of `p_left op p_right` (spec §5.1), or Error after reporting;
	// with `r_compared`, the type both sides of == are compared as, where they
	// convert to it: Variant, or the deeper T? (§5.2).
	Type binary_type(TokenKind p_op, const Type &p_left, const Type &p_right, const TokenRange &p_at, Type *r_compared = nullptr) {
		if (p_left.is_error() || p_right.is_error()) {
			return Type::of(TypeKind::Error);
		}
		// Formats a value of any type, which enters the engine as anywhere else
		// (spec §5.1, §10.1).
		if (p_op == TokenKind::Percent && is_format_string(p_left)) {
			if (!assignable(p_right, Type::of(TypeKind::Variant))) {
				error(p_at, "Cannot format a value of type " + p_right.to_string() + "." + variant_refusal(p_right));
				return Type::of(TypeKind::Error);
			}
			return Type::of(TypeKind::String);
		}
		const bool ordering = p_op == TokenKind::Less || p_op == TokenKind::LessEqual || p_op == TokenKind::Greater || p_op == TokenKind::GreaterEqual;
		// Ordered by LESS (spec §3.6, §5.2), the types of both sides being one:
		// `<` does not wrap a T into T?.
		if (ordering && p_left == p_right && (p_left.kind == TypeKind::Struct || p_left.kind == TypeKind::PayloadEnum || p_left.kind == TypeKind::Option || (is_typed_container(p_left) && p_left.name == "Array"))) {
			if (!has_less(p_left)) {
				error(p_at, "Values of type " + p_left.to_string() + " are not ordered: a struct or an enum with payloads is when all its fields are, T? and Array[T] when T is.");
				return Type::of(TypeKind::Error);
			}
			return Type::of(TypeKind::Bool);
		}
		// The side with fewer levels of T? is wrapped (spec §5.2).
		if ((p_op == TokenKind::EqualEqual || p_op == TokenKind::BangEqual) && (p_left.kind == TypeKind::Option || p_right.kind == TypeKind::Option)) {
			const bool left_deeper = option_depth(p_left) >= option_depth(p_right);
			if (wraps_to(left_deeper ? p_right : p_left, left_deeper ? p_left : p_right)) {
				if (r_compared) {
					*r_compared = left_deeper ? p_left : p_right;
				}
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
		// Only == and != take a Variant, by the engine's operator table when
		// the program runs (spec §3.13, §5.2).
		if (p_left.kind == TypeKind::Variant || p_right.kind == TypeKind::Variant) {
			const Type &other = p_left.kind == TypeKind::Variant ? p_right : p_left;
			const bool compares = p_op == TokenKind::EqualEqual || p_op == TokenKind::BangEqual;
			if (compares && assignable(other, Type::of(TypeKind::Variant))) {
				if (r_compared) {
					*r_compared = Type::of(TypeKind::Variant);
				}
				return Type::of(TypeKind::Bool);
			}
			error(p_at, compares ? "Cannot compare a Variant with " + other.to_string() + "." : std::string("Cannot use ") + token_name(p_op) + " on a Variant." + variant_hint());
			return Type::of(TypeKind::Error);
		}
		// Objects by identity (spec §5.2), which two classes that do not extend
		// one another never share.
		if ((p_op == TokenKind::EqualEqual || p_op == TokenKind::BangEqual) && p_left.kind == TypeKind::Object && p_right.kind == TypeKind::Object) {
			Type left = p_left;
			Type right = p_right;
			left.nullable = right.nullable = false;
			if (!left.is_interface && !right.is_interface && !assignable(left, right) && !assignable(right, left)) {
				error(p_at, left.to_string() + " and " + right.to_string() + " are never the same object, since neither class extends the other.");
				return Type::of(TypeKind::Error);
			}
			return Type::of(TypeKind::Bool);
		}
		// Tasks by identity too (spec §5.2), when one could be the other.
		if ((p_op == TokenKind::EqualEqual || p_op == TokenKind::BangEqual) && p_left.kind == TypeKind::Task && p_right.kind == TypeKind::Task) {
			Type left = p_left;
			Type right = p_right;
			left.nullable = right.nullable = false;
			if (assignable(left, right) || assignable(right, left)) {
				return Type::of(TypeKind::Bool);
			}
			error(p_at, left.to_string() + " and " + right.to_string() + " are never the same task, since neither result type is the other.");
			return Type::of(TypeKind::Error);
		}
		if (p_left.kind == TypeKind::Enum || p_right.kind == TypeKind::Enum) {
			return binary_type(p_op, as_int(p_left), as_int(p_right), p_at);
		}
		if (p_left.is_primitive() && p_right.is_primitive()) {
			return primitive_binary_type(p_op, p_left, p_right, p_at);
		}
		const bool equality = p_op == TokenKind::EqualEqual || p_op == TokenKind::BangEqual;
		// Structs compare field by field (spec §3.6), enums with payloads by
		// variant and then so (§3.7).
		if ((p_left.kind == TypeKind::Struct || p_left.kind == TypeKind::PayloadEnum) && equality && p_left == p_right) {
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
		} else if (r_expr.op == TokenKind::Percent && is_format_string(left)) {
			// The values to format, of any types (spec §5.1).
			right = check_value(*r_expr.right, Type::builtin("Array"));
		} else {
			// Ok(v) and Err(e) take their type from the Result they are compared
			// with (spec §3.5).
			right = check_value(*r_expr.right, left.is_result() ? left : Type::of(TypeKind::Error));
		}
		Type compared = Type::of(TypeKind::Error);
		const Type type = binary_type(r_expr.op, left, right, r_expr.range, &compared);
		const bool variant = compared.kind == TypeKind::Variant;
		r_expr.target = variant ? Target::VariantEqual : Target::None;
		r_expr.param_types = variant || compared.is_error() ? std::vector<Type>() : std::vector<Type>{ compared, compared };
		return set(r_expr, type);
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
		if (container.kind == TypeKind::Variant) {
			check_value(*r_expr.left);
			return fail(r_expr, "Cannot look for a value in a Variant." + variant_hint());
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
		// Through an untyped view, checked as the program runs (spec §3.8).
		r_expr.checks_elements = (base == "Array" || base == "Dictionary") && container.args.empty();
		return set(r_expr, Type::of(TypeKind::Bool));
	}

	// `x is T` (spec §5.4).
	Type check_type_test(Expr &r_expr) {
		const Type value = check_value(*r_expr.left);
		const Type tested = resolve(r_expr.cast_type, false);
		if (value.is_error() || tested.is_error()) {
			return set(r_expr, Type::of(TypeKind::Error));
		}
		// What a Variant holds, or of an untyped container, whether it is typed
		// so (spec §3.8, §3.13).
		const bool untyped = value.kind == TypeKind::Builtin && value.args.empty() && is_typed_container(tested) && tested.name == value.name;
		if (value.kind == TypeKind::Variant || untyped) {
			const bool object = tested.kind == TypeKind::Object && !tested.nullable;
			if (!object && !testable(tested)) {
				return fail(r_expr, "is tests classes and built-in types, not " + tested.to_string() + ".");
			}
			r_expr.tested = tested;
			return set(r_expr, Type::of(TypeKind::Bool));
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

	// A built-in type whose values a Variant tells by their engine type, a
	// container by the types the engine keeps for it (spec §3.8).
	static bool testable(const Type &p_type) {
		if (p_type.is_primitive()) {
			return true;
		}
		if (p_type.kind != TypeKind::Builtin || is_boxed(p_type)) {
			return false;
		}
		return std::none_of(p_type.args.begin(), p_type.args.end(), [](const Type &p_arg) { return p_arg.is_interface || is_typed_container(p_arg); });
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

	// A Callable that a function value goes into is of that function's type.
	void narrow_by_value(int p_local, const Type &p_value) {
		if (p_value.kind == TypeKind::Function && function->locals[p_local].type == Type::builtin("Callable")) {
			narrowed.back()[p_local] = p_value;
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
			for (const MatchBranch &branch : last.branches) {
				if (!always_ends(branch.body, p_or_jump)) {
					return false;
				}
			}
			return last.exhaustive;
		}
		return last.kind == StmtKind::If && always_ends(last.body, p_or_jump) && always_ends(last.else_body, p_or_jump);
	}

	// "2", "1 to 3" or "at least 1".
	static std::string argument_count(int p_required, int p_max, bool p_vararg) {
		if (p_vararg) {
			return "at least " + std::to_string(p_required);
		}
		return (p_required == p_max ? std::string() : std::to_string(p_required) + " to ") + std::to_string(p_max);
	}

	static std::string argument_count(const FuncDecl &p_function) {
		return argument_count(required_params(p_function), int(p_function.params.size()), p_function.has_rest());
	}

	// Checks the arguments of a call against parameter types.
	// With `p_vararg`, those after `p_params` are of type `p_rest`.
	void check_arguments(Expr &r_call, const std::vector<Type> &p_params, int p_required, bool p_vararg, const Type &p_rest = Type::of(TypeKind::Variant)) {
		const int count = int(r_call.args.size());
		const int max = int(p_params.size());
		if (probing) {
			probed_params = max + (p_vararg ? 1 : 0);
		} else if (count < p_required || (!p_vararg && count > max)) {
			error(r_call.range, "Function \"" + r_call.text + "()\" takes " + argument_count(p_required, max, p_vararg) + " argument(s), " + std::to_string(count) + " given.");
		}
		for (int i = 0; i < count; i++) {
			Expr &arg = *r_call.args[i];
			const Type param = i < max ? p_params[i] : p_rest;
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
		r_expr.getter = p_property.getter;
		r_expr.setter = p_property.setter;
		r_expr.property_index = p_property.index;
		r_expr.returns_reference = p_property.getter && type.kind == TypeKind::Object && engine.inherits(type.engine_class(), "RefCounted");
		return type;
	}

	// An engine method as a value (spec §3.11): the engine's Callable, which
	// finds the method by name on the object, or for a static one through its
	// class, after the object if there is one (§4.8).
	Type engine_method_value(Expr &r_expr, const EngineMethod &p_method) {
		if (p_method.is_virtual) {
			return fail(r_expr, "\"" + r_expr.text + "()\" is a virtual engine method.");
		}
		if (p_method.return_type.kind == TypeKind::Unsupported) {
			return fail(r_expr, "\"" + r_expr.text + "()\" returns " + p_method.return_type.to_string() + ", which is not supported yet.");
		}
		const bool through_class = p_method.is_static && !r_expr.is_foreign;
		if (!through_class && !receiver_kept(r_expr)) {
			return set(r_expr, Type::of(TypeKind::Error));
		}
		bool ok = false;
		const std::vector<Type> params = engine_params(p_method, r_expr, ok);
		if (!ok) {
			return set(r_expr, Type::of(TypeKind::Error));
		}
		if (!r_expr.is_foreign) {
			refer(r_expr.name_token, engine_id(p_method.owner, r_expr.text));
		}
		r_expr.target = through_class ? Target::EngineStaticValue : Target::EngineMethodValue;
		if (through_class) {
			r_expr.owner = p_method.owner;
			r_expr.static_on_value = r_expr.base && !names_type(*r_expr.base);
		}
		// Of a method that takes any number of arguments, a Callable of unknown signature.
		return set(r_expr, p_method.is_vararg ? Type::builtin("Callable") : Type::function(engine_result(p_method), params));
	}

	// A try_ version as a value (spec §3.11, §10.3), which is lowered at each
	// call: the lambda that calls it, as func(path, flags): return
	// FileAccess.try_open(path, flags), checked in place of the name.
	Type tried_value(Expr &r_expr, const EngineMethod &p_method, const EngineMethod &p_getter) {
		bool ok = false;
		const std::vector<Type> params = engine_params(p_method, r_expr, ok);
		if (!ok) {
			return set(r_expr, Type::of(TypeKind::Error));
		}
		const Type type = Type::function(tried_result(p_method, p_getter), params);
		if (in_constant) {
			return set(r_expr, type); // No constant, as no other function value is (spec §4.5).
		}
		const auto made = [&](ExprKind p_kind, const std::string &p_text) {
			auto expr = std::make_unique<Expr>();
			expr->kind = p_kind;
			expr->text = p_text;
			expr->range = r_expr.range;
			expr->name_token = NO_TOKEN;
			return expr;
		};
		std::unique_ptr<Expr> call = made(ExprKind::Call, r_expr.text);
		call->name_token = r_expr.name_token;
		call->base = made(ExprKind::Name, r_expr.base->text);
		auto lambda = std::make_unique<FuncDecl>();
		lambda->is_lambda = true;
		lambda->name = r_expr.base->text + "." + r_expr.text;
		lambda->range = r_expr.range;
		lambda->name_token = r_expr.name_token;
		for (const EngineParam &engine_param : p_method.params) {
			ParamDecl &param = lambda->params.emplace_back();
			param.name = engine_param.name;
			param.range = r_expr.range;
			param.name_token = NO_TOKEN;
			call->args.push_back(made(ExprKind::Name, param.name));
		}
		auto body = std::make_unique<Stmt>();
		body->kind = StmtKind::Return;
		body->range = r_expr.range;
		body->value = std::move(call);
		lambda->body.push_back(std::move(body));
		r_expr.kind = ExprKind::Lambda;
		r_expr.base.reset();
		r_expr.function = std::move(lambda);
		return check_lambda(r_expr, type);
	}

	// A method value keeps no reference to its object (spec §3.11), so one of
	// a new RefCounted, which nothing else holds, is refused.
	bool receiver_kept(Expr &r_expr) {
		return receiver_kept(r_expr.base.get(), r_expr.range);
	}

	bool receiver_kept(const Expr *p_base, const TokenRange &p_range) {
		if (p_base && p_base->kind == ExprKind::Call && p_base->target == Target::NewObject && engine.inherits(p_base->type.engine_class(), "RefCounted")) {
			error(p_range, "A method value does not keep its object, and nothing else holds this new " + p_base->type.to_string() + ".");
			return false;
		}
		return true;
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
		hint(r_expr, [&](int p_current) {
			std::vector<std::string> texts;
			for (size_t i = 0; i < params.size(); i++) {
				const std::string &value = p_method.params[i].default_value;
				texts.push_back(p_method.params[i].name + ": " + params[i].to_string() + (value.empty() ? "" : " = " + (value == "null" ? "None" : value)));
			}
			return signature(r_expr.text, texts, p_method.is_vararg, p_current, (r_expr.getter ? tried_result(p_method, *r_expr.getter) : engine_result(p_method)).to_string());
		});
		check_arguments(r_expr, params, int(params.size()) - p_method.default_count, p_method.is_vararg);
		if (p_method.owner == "Node" && (p_method.name == "get_node" || p_method.name == "get_node_or_null") && !r_expr.args.empty() && r_expr.args[0]->kind == ExprKind::StringLiteral) {
			const std::string autoload = through_autoload(r_expr.args[0]->text, r_expr.text + "(\"" + r_expr.args[0]->text + "\")");
			if (!autoload.empty()) {
				return fail_call(r_expr, autoload);
			}
		}
		const bool on_self = !r_expr.base || extends_script(r_expr.base->type.script, options.path);
		for (size_t i = 0; i < params.size(); i++) {
			r_expr.arg_ranges.push_back(params[i].kind == TypeKind::Int ? builtin::param_int_range(p_method.owner, p_method.name, uint32_t(i)) : std::nullopt);
			if (completion && i < r_expr.args.size() && r_expr.args[i]->kind == ExprKind::StringLiteral && at_cursor(r_expr.args[i]->text)) {
				complete_string(&p_method, i, on_self);
			}
		}
		if (const std::optional<builtin::MemberName> named = builtin::member_name_param(p_method.owner, p_method.name)) {
			check_member_name(r_expr, p_method, *named);
		}
		if (!r_expr.is_foreign) {
			refer(r_expr.name_token, engine_id(p_method.owner, r_expr.text));
		}
		r_expr.target = Target::EngineMethod;
		if (!p_static_class.empty()) {
			r_expr.is_static = true;
			r_expr.owner = p_static_class;
		}
		if (p_method.hash && !r_expr.is_foreign && !p_method.is_vararg) {
			// Called through its MethodBind (architecture §2.2).
			r_expr.hash = p_method.hash;
			if (p_static_class.empty()) {
				r_expr.owner = p_method.owner;
			}
			r_expr.engine_params = params;
			for (size_t i = params.size() - p_method.default_count; i < params.size(); i++) {
				r_expr.engine_defaults.push_back(p_method.params[i].default_value);
			}
			r_expr.returns_reference = p_method.return_type.kind == TypeKind::Object && engine.inherits(p_method.return_type.engine_class(), "RefCounted");
		}
		r_expr.engine_return = engine_result(p_method);
		if (p_method.owner == "Resource" && (p_method.name == "duplicate" || p_method.name == "duplicate_deep")) {
			// Of the receiver's type (spec §7.6), which the result is checked for.
			Type copy = r_expr.base ? r_expr.base->type : script_class_type(cls, options.path);
			copy.nullable = false;
			return set(r_expr, copy);
		}
		if (const std::optional<Type> node = literal_node_type(r_expr, p_method)) {
			return set(r_expr, *node);
		}
		if (p_method.owner == "Node" && p_method.name == "get_parent" && cls.parent_ref && (!r_expr.base || r_expr.base->kind == ExprKind::Self)) {
			// What @parent(T) requires, which a node without a parent fails (spec §7.3).
			return set(r_expr, cls.parent_type);
		}
		return set(r_expr, r_expr.getter ? tried_result(p_method, *r_expr.getter) : filtered_result(r_expr, p_method));
	}

	// The member of a class a literal name refers to (spec §10.4).
	struct NamedMember {
		bool found = false;
		bool unknown = false; // Of another language's class whose members are not known here.
		bool hidden = false; // Of this language and not pub.
		DeclId id;
		Type type = Type::of(TypeKind::Error); // A property's, or Error.
		std::vector<Type> params; // A method's or a signal's.
		int required = 0; // A method's.
		std::optional<Type> rest; // What a method takes after `params`, when it takes more.
	};

	// The member of the kind named `p_name` that `p_class` has, as `object.name`
	// finds it: a member of a class of this language, of an interface, of
	// another language's class, or of the engine class.
	NamedMember named_member(const Type &p_class, builtin::MemberName::Kind p_kind, const std::string &p_name) {
		using Kind = builtin::MemberName::Kind;
		NamedMember out;
		const auto from_function = [&](const FuncDecl &p_function, const std::string &p_owner) {
			out.found = true;
			out.hidden = !p_function.is_pub && !p_function.is_override;
			out.id = script_id(p_owner, p_function.name);
			out.params = param_types(p_function.params);
			out.required = required_params(p_function);
			if (p_function.has_rest()) {
				out.rest = rest_element(out.params.back());
				out.params.pop_back();
			}
		};
		const auto from_engine = [&](const EngineMethod &p_method, const DeclId &p_id) {
			out.found = true;
			out.id = p_id;
			for (const EngineParam &param : p_method.params) {
				out.params.push_back(localized(param.type));
			}
			out.required = int(p_method.params.size()) - p_method.default_count;
			if (p_method.is_vararg) {
				out.rest = Type::of(TypeKind::Variant);
			}
		};
		if (p_class.is_interface) {
			const std::optional<InterfaceMember> member = interface_member(p_class, p_name);
			if (member && member->property && p_kind == Kind::Property) {
				out.found = true;
				out.id = interface_member_id(*member);
				out.type = member->property->type;
				return out;
			}
			if (member && member->method && p_kind == Kind::Method) {
				from_function(*member->method, member->owner.script);
				out.id = interface_member_id(*member);
				out.hidden = false;
				return out;
			}
			if (member && member->signal && p_kind == Kind::Signal) {
				out.found = true;
				out.id = interface_member_id(*member);
				out.params = param_types(member->signal->params);
				return out;
			}
		} else if (is_script_path(p_class.script)) {
			if (const ClassDecl *decl = script_decl(p_class.script)) {
				if (p_kind == Kind::Property) {
					for (const VarDecl *member : decl->all_members) {
						if (member->name == p_name) {
							out.found = true;
							out.hidden = !member->is_pub;
							out.id = script_id(declaring(*decl, *member, &ClassDecl::members).path, member->name);
							out.type = member->type;
							return out;
						}
					}
				} else if (p_kind == Kind::Method) {
					for (const FuncDecl *function : decl->all_functions) {
						if (function->name == p_name) {
							from_function(*function, declaring(*decl, *function, &ClassDecl::functions).path);
							return out;
						}
					}
				} else {
					for (const SignalDecl *signal : decl->all_signals) {
						if (signal->name == p_name) {
							out.found = true;
							out.hidden = !signal->is_pub;
							out.id = script_id(declaring(*decl, *signal, &ClassDecl::signals).path, signal->name);
							out.params = param_types(signal->params);
							return out;
						}
					}
				}
			}
		} else if (!p_class.script.empty()) {
			const ForeignClass *foreign = foreign_class(p_class.script);
			if (!foreign || !foreign->members_known) {
				out.found = out.unknown = true;
				return out;
			}
			if (p_kind == Kind::Property) {
				for (const EngineProperty &property : foreign->properties) {
					if (property.name == p_name) {
						out.found = true;
						out.id = foreign_id(p_class.script, p_name);
						out.type = property.type.is_storable() ? localized(property.type) : Type::of(TypeKind::Error);
						return out;
					}
				}
			}
			for (const EngineMethod &method : p_kind == Kind::Method ? foreign->methods : p_kind == Kind::Signal ? foreign->signals
																												 : std::vector<EngineMethod>()) {
				if (method.name == p_name) {
					from_engine(method, foreign_id(p_class.script, p_name));
					return out;
				}
			}
		}
		const std::string &engine_class = p_class.engine_class();
		if (p_kind == Kind::Property) {
			if (const auto property = engine.find_property(engine_class, p_name)) {
				out.found = true;
				out.id = engine_id(property->owner, p_name);
				out.type = property->type.is_storable() ? property->type : Type::of(TypeKind::Error);
			}
		} else if (const auto member = p_kind == Kind::Method ? engine.find_method(engine_class, p_name) : engine.find_signal(engine_class, p_name)) {
			from_engine(*member, engine_id(member->owner, p_name));
		}
		return out;
	}

	// The member a literal name at a position the binding metadata lists
	// refers to (spec §10.4): a static reference, which the object's static
	// type must have, as for `object.name`, unless the type is Object or
	// Variant, the method only asks, or the name is of a property the engine
	// lists per instance. A non-pub member is one the engine cannot find
	// (§10.5). nullopt when the name is not a static reference or is wrong;
	// `r_path` has the name and, of a property path as "position:x", the
	// subnames that index the property's value.
	std::optional<NamedMember> literal_member(Expr &r_call, const builtin::MemberName &p_named, std::vector<std::string> &r_path) {
		using Kind = builtin::MemberName::Kind;
		if (p_named.index >= r_call.args.size() || r_call.args[p_named.index]->kind != ExprKind::StringLiteral) {
			return std::nullopt;
		}
		Expr &literal = *r_call.args[p_named.index];
		const std::string &text = literal.text;
		if (!text.empty() && text[0] == '@') {
			error(literal.range, "\"" + text + "\" is how the engine knows a signal that is not pub; only its class reaches it, by the signal itself.");
			return std::nullopt;
		}
		Type object;
		if (p_named.target < 0) {
			object = r_call.base ? r_call.base->type : script_class_type(cls, options.path);
		} else if (size_t(p_named.target) < r_call.args.size()) {
			object = r_call.args[p_named.target]->type;
		}
		object.nullable = false;
		if (object.kind != TypeKind::Object || (object.name == "Object" && object.script.empty()) || text.find('/') != std::string::npos) {
			return std::nullopt;
		}
		size_t start = p_named.path && !text.empty() && text[0] == ':' ? 1 : 0;
		while (true) {
			const size_t colon = p_named.path ? text.find(':', start) : std::string::npos;
			r_path.push_back(text.substr(start, colon == std::string::npos ? std::string::npos : colon - start));
			if (colon == std::string::npos) {
				break;
			}
			start = colon + 1;
		}
		const std::string &name = r_path[0];
		const NamedMember member = named_member(object, p_named.kind, name);
		const char *kind = p_named.kind == Kind::Method ? "method" : p_named.kind == Kind::Property ? "property"
																									: "signal";
		if (!member.found) {
			if (!p_named.query) {
				error(literal.range, object.to_string() + " has no " + kind + " \"" + name + "\".");
			}
			return std::nullopt;
		}
		if (member.unknown) {
			return std::nullopt;
		}
		if (member.hidden) {
			const std::string fix = p_named.kind == Kind::Method ? "pass the method itself, as " + name : p_named.kind == Kind::Signal ? "use the signal itself, as " + name
																																	   : "make it pub";
			error(literal.range, "\"" + name + (p_named.kind == Kind::Method ? "()" : "") + "\" is not pub, so the engine cannot find it by name; " + fix + ".");
			return std::nullopt;
		}
		if (r_path.size() == 1) {
			refer_string(literal, member.id);
		}
		return member;
	}

	// A literal member name passed to the engine, and what the call gives the
	// member: the value of a property, the arguments of a method or a signal,
	// or the function connected to a signal (spec §10.4).
	void check_member_name(Expr &r_call, const EngineMethod &p_method, const builtin::MemberName &p_named) {
		using Kind = builtin::MemberName::Kind;
		std::vector<std::string> path;
		const std::optional<NamedMember> found = literal_member(r_call, p_named, path);
		if (!found || p_named.query) {
			return;
		}
		const NamedMember &member = *found;
		const Expr &literal = *r_call.args[p_named.index];
		const std::string &text = literal.text;
		const std::string &name = path[0];
		const size_t first = p_named.index + 1;
		if (p_named.kind == Kind::Property) {
			std::optional<Type> type = member.type;
			for (size_t i = 1; i < path.size() && type && !type->is_error(); i++) {
				type = indexed_type(*type, path[i]);
				if (type && type->is_error()) {
					error(literal.range, "\"" + path[i - 1] + "\" has no member \"" + path[i] + "\".");
				}
			}
			if (p_named.value >= 0 && size_t(p_named.value) < r_call.args.size() && type && !type->is_error()) {
				const Expr &value = *r_call.args[p_named.value];
				if (!value.type.is_error() && !assignable(value.type, *type)) {
					error(value.range, "\"" + text + "\" is " + type->to_string() + ", which takes no " + value.type.to_string() + ".");
				}
			}
		} else if (p_method.is_vararg) {
			// The arguments that follow are passed on: to the method, or with the
			// signal as it is emitted.
			const size_t given = r_call.args.size() - first;
			const size_t required = p_named.kind == Kind::Method ? size_t(member.required) : member.params.size();
			if (given < required || (!member.rest && given > member.params.size())) {
				error(literal.range, "\"" + name + "\" takes " + argument_count(int(required), int(member.params.size()), member.rest.has_value()) + " argument(s); " + std::to_string(given) + " follow.");
				return;
			}
			for (size_t i = 0; i < given; i++) {
				const Expr &arg = *r_call.args[first + i];
				const Type &param = i < member.params.size() ? member.params[i] : *member.rest;
				if (!arg.type.is_error() && !assignable(arg.type, param)) {
					error(arg.range, "\"" + name + "\" takes " + param.to_string() + " as argument " + std::to_string(i + 1) + "; found " + arg.type.to_string() + ".");
				}
			}
		} else if (p_named.kind == Kind::Signal && first < r_call.args.size() && (r_call.args[first]->type.kind == TypeKind::Function || rest_function(*r_call.args[first]))) {
			check_handler(*r_call.args[first], Type::signal(member.params));
		}
	}

	// The type of `p_name` in a value of `p_type`, as a property path indexes
	// it: a member of a built-in type, such as Vector2's x, or a property of an
	// object; Error when it has none of that name, nullopt for a key or a type
	// not known here.
	std::optional<Type> indexed_type(const Type &p_type, const std::string &p_name) {
		if (p_type.kind == TypeKind::Object) {
			const NamedMember member = named_member(p_type, builtin::MemberName::Kind::Property, p_name);
			if (member.unknown) {
				return std::nullopt;
			}
			return member.found && !member.hidden ? member.type : Type::of(TypeKind::Error);
		}
		const builtin::Class *type = p_type.builtin_name().empty() ? nullptr : builtin::find_class(p_type.builtin_name());
		if (!type || type->member_count == 0) {
			return std::nullopt;
		}
		const builtin::Member *member = builtin::find_member(*type, p_name);
		return member ? api_type(member->type, false) : Type::of(TypeKind::Error);
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
	// returns; false after reporting it. An object is T? (spec §10.2), as
	// instance_from_id() and get_object() give it.
	bool api_result_supported(Expr &r_call, const Type &p_result) {
		if (p_result.kind == TypeKind::Unsupported) {
			fail(r_call, "\"" + r_call.text + "()\" returns " + p_result.to_string() + ", which is not supported yet.");
			return false;
		}
		return true;
	}

	Type check_utility(Expr &r_expr, const builtin::Utility &p_utility) {
		hint(r_expr, [&](int p_current) { return api_signature(r_expr.text, p_utility.arguments, p_utility.argument_count, p_utility.is_vararg, p_utility.return_type, p_current); });
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
		if (probing) {
			probed_params = int(expected);
			return set(r_expr, Type::of(TypeKind::Variant));
		}
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
			// An int weight converts to float (spec §3.14) before it goes to the engine.
			r_expr.param_types[2] = Type::of(TypeKind::Float);
		} else if (!is_numeric_or_vector(result)) {
			return fail(r_expr, r_expr.text + "() takes numbers or vectors, not " + result.to_string() + ".");
		}
		return set(r_expr, result);
	}

	// `T(v)`: the engine constructs a T from what the Variant holds, as
	// GDScript's conversion does (spec §3.13).
	Type construct_from_variant(Expr &r_expr, const Type &p_type) {
		r_expr.target = Target::ConstructFromVariant;
		r_expr.param_types = { Type::of(TypeKind::Variant) };
		return set(r_expr, p_type);
	}

	Type check_constructor(Expr &r_expr, const builtin::Class &p_type) {
		// Each constructor that takes the argument at the cursor, as GDScript
		// shows them (modules/gdscript/gdscript_editor.cpp:3400-3415).
		hint(r_expr, [&](int p_current) {
			std::string hints;
			for (uint32_t i = 0; i < p_type.constructor_count; i++) {
				if (p_current < int(p_type.constructors[i].argument_count)) {
					hints += (hints.empty() ? "" : "\n") + api_signature(p_type.name, p_type.constructors[i].arguments, p_type.constructors[i].argument_count, false, p_type.name, p_current);
				}
			}
			return hints;
		});
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
		// Of a number, int() and float() check NaN and the range as the program
		// runs (spec §3.14), which the engine's constructors do not.
		if ((name == "int" || name == "float") && args.size() == 1 && args[0].is_numeric()) {
			r_expr.target = name == "int" ? Target::ConvertToInt : Target::ConvertToFloat;
			r_expr.param_types.push_back(args[0]);
			return set(r_expr, Type::of(name == "int" ? TypeKind::Int : TypeKind::Float));
		}
		if ((name == "Callable" || name == "Signal") && args.size() == 2 && args[0].kind == TypeKind::Object) {
			if (args[0].is_optional()) {
				return fail(r_expr, "The value may be None (" + args[0].to_string() + "); check it before taking its " + (name == "Signal" ? "signal." : "method."));
			}
			if (name == "Callable" && !receiver_kept(r_expr.args[0].get(), r_expr.range)) {
				return set(r_expr, Type::of(TypeKind::Error));
			}
			// A name known only at run time is explicit dynamic access, which
			// the engine's constructor makes (spec §10.4).
			if (r_expr.args[1]->kind == ExprKind::StringLiteral) {
				return check_named_value(r_expr, name == "Signal");
			}
		}
		if (args.size() == 1 && args[0].kind == TypeKind::Variant) {
			return construct_from_variant(r_expr, api_type(name, false));
		}
		if (args.size() == 1 && args[0].name == name && is_typed_container(args[0]) && is_boxed(args[0])) {
			// An untyped copy with the elements encoded (spec §3.8, §10.1).
			const std::string loss = encoding_loss(args[0], "the engine", false, true);
			if (!loss.empty()) {
				return fail(r_expr, name + "(x) encodes " + args[0].to_string() + ", but " + loss);
			}
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
				viable = viable && assignable(args[a], param);
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

	// Callable(object, "m") and Signal(object, "s"): with a literal name, the
	// method value or the signal of the object, as object.m and object.s are
	// (spec §10.4).
	Type check_named_value(Expr &r_expr, bool p_signal) {
		const builtin::MemberName named{ 1, 0, p_signal ? builtin::MemberName::Kind::Signal : builtin::MemberName::Kind::Method, false, -1, false };
		std::vector<std::string> path;
		const size_t before = errors.size();
		const std::optional<NamedMember> member = literal_member(r_expr, named, path);
		if (errors.size() != before) {
			return set(r_expr, Type::of(TypeKind::Error));
		}
		r_expr.target = p_signal ? Target::NamedSignal : Target::NamedCallable;
		// A method of this language as its method value; any other as the
		// engine's Callable, which finds it by name.
		r_expr.index = member && member->id.kind == DeclId::Kind::Script ? 1 : 0;
		return set(r_expr, p_signal && member ? Type::signal(member->params) : Type::builtin(p_signal ? "Signal" : "Callable"));
	}

	static Type role_type(bytecode::ElementRole p_role, const Type &p_container) {
		using R = bytecode::ElementRole;
		switch (p_role) {
			case R::Element:
			case R::Key:
				return p_container.args[0];
			case R::OptionalElement:
			case R::OptionalKey:
				return optional_of(p_container.args[0]);
			case R::Elements:
				return Type::array(p_container.args.back());
			case R::Keys:
				return Type::array(p_container.args[0]);
			case R::Value:
				return p_container.args[1];
			case R::Entries:
				return p_container;
			case R::None:
				break;
		}
		return Type::of(TypeKind::Void);
	}

	// The engine binds Array's and Dictionary's methods with Variant; for
	// Array[T] and Dictionary[K, V], the arguments and return values take
	// their roles in the container (spec §3.8).
	static void container_typed(const bytecode::ContainerMethod &p_method, const Type &p_container, std::vector<Type> &r_params, Type &r_result) {
		for (size_t i = 0; i < r_params.size() && i < 2; i++) {
			if (p_method.args[i] != bytecode::ElementRole::None) {
				r_params[i] = role_type(p_method.args[i], p_container);
			}
		}
		if (p_method.result != bytecode::ElementRole::None) {
			r_result = role_type(p_method.result, p_container);
		}
	}

	// Whether values of the type have `<` (spec §5.2): those the engine's
	// operator table orders (core/variant/variant_op.cpp:731-791), and Variant,
	// ordered when the program runs; a struct when all its fields have it, T?
	// and Array[T] when T has it. A type met again inside itself, through a
	// container (spec §3.6), has it when the rest has.
	bool has_less(const Type &p_type) const {
		std::set<std::string> visiting;
		return has_less(p_type, visiting);
	}

	bool has_less(const Type &p_type, std::set<std::string> &r_visiting) const {
		static const std::unordered_set<std::string> ordered = { "StringName", "Vector2", "Vector2i", "Vector3", "Vector3i", "Vector4", "Vector4i", "RID" };
		const auto has = [&](const Type &p_part) { return has_less(p_part, r_visiting); };
		if ((p_type.kind == TypeKind::Struct || p_type.kind == TypeKind::PayloadEnum) && !r_visiting.insert(p_type.script + "::" + p_type.to_string()).second) {
			return true;
		}
		switch (p_type.kind) {
			case TypeKind::Int:
			case TypeKind::Enum:
			case TypeKind::Float:
			case TypeKind::Bool:
			case TypeKind::String:
			case TypeKind::Variant:
				return true;
			case TypeKind::Option:
				return has(p_type.args[0]);
			case TypeKind::Struct: {
				const StructDecl *decl = struct_decl(p_type);
				return !decl || std::all_of(decl->fields.begin(), decl->fields.end(), [&](const VarDecl &p_field) { return has(p_field.type); });
			}
			case TypeKind::PayloadEnum:
				for (const auto &fields : p_type.values->fields) {
					for (const auto &[name, type] : fields) {
						if (!has(type)) {
							return false;
						}
					}
				}
				return true;
			case TypeKind::Builtin:
				if (p_type.name == "Array") {
					return p_type.args.empty() || has(p_type.args[0]);
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
	bool check_container_method(const Expr &p_call, const Type &p_base, const bytecode::ContainerMethod *p_container) {
		const std::string &method = p_call.text;
		const bool array = p_base.name == "Array" && !p_base.args.empty();
		if (p_container && p_container->orders && (array || p_base.name == "Dictionary")) {
			const Type &ordered = p_base.args.empty() ? Type::of(TypeKind::Variant) : p_base.args[0];
			if (!has_less(ordered)) {
				error(p_call.range, "\"" + method + "()\" orders the " + (array ? "elements" : "keys") + " by <, which " + ordered.to_string() + " does not have; use " + (array ? "sort_custom() or bsearch_custom() with a comparison" : "a sorted Array of its keys") + ".");
				return false;
			}
		}
		const Type &element = array ? p_base.args[0] : Type::of(TypeKind::Error);
		if (method == "resize" && !has_value_without_initializer(element)) {
			const std::string fix = element.kind == TypeKind::Struct ? "give every field of " + element.to_string() + " a default value, or " : "";
			error(p_call.range, "\"resize()\" fills new elements with a default value, which " + element.to_string() + " does not have; " + fix + "declare the array as Array[" + optional_of(element).to_string() + "].");
			return false;
		}
		return true;
	}

	// The function an Array method of the engine calls on the elements: a
	// comparison for sort_custom() and bsearch_custom(), whose value is an
	// element too, and a test for the others, but map(), whose function
	// returns any type (core/variant/array.cpp:392-470, 604-767; ed1daf0).
	static Type element_function(const std::string &p_method, const Type &p_array) {
		const Type element = p_array.args.empty() ? Type::of(TypeKind::Variant) : p_array.args[0];
		if (p_method == "sort_custom" || p_method == "bsearch_custom") {
			return Type::function(Type::of(TypeKind::Bool), { element, element });
		}
		return Type::function(p_method == "map" ? Type::of(TypeKind::Error) : Type::of(TypeKind::Bool), { element });
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
		hint(r_expr, [&](int p_current) { return api_signature(r_expr.text, method->arguments, method->argument_count, method->is_vararg, method->return_type, p_current); });
		// On a value, the type's own (spec §4.8).
		r_expr.static_on_value = method->is_static && !p_static;
		if (!method->is_static && p_static) {
			return fail_call(r_expr, "\"" + r_expr.text + "()\" of " + base_name + " is not static; call it on a value.");
		}
		const bytecode::ContainerMethod *container = (base_name == "Array" || base_name == "Dictionary") ? bytecode::find_container_method(bytecode::value_type_from_name(base_name), r_expr.text) : nullptr;
		const bool typed_dictionary = base_name == "Dictionary" && !p_base.args.empty();
		// get(k) of Dictionary[K, V] gives V?, None for a missing key, and of an
		// untyped one Variant? (spec §3.8).
		const bool get_or_none = base_name == "Dictionary" && r_expr.text == "get" && r_expr.args.size() == 1;
		bool pop_or_none = false;
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
		if (container && !p_base.args.empty()) {
			// The engine's null of a missing key is also that of a None key.
			if (container->result == bytecode::ElementRole::OptionalKey && p_base.args[0].kind == TypeKind::Object && p_base.args[0].nullable) {
				return fail(r_expr, "\"" + r_expr.text + "()\" of " + p_base.to_string() + " gives " + p_base.args[0].to_string() + "?, which is not supported yet.");
			}
			container_typed(*container, p_base, params, result);
			// With T?, pop_front() and pop_back() give T??, which tells an empty
			// array from a None element (spec §3.8), as the engine's null does not.
			pop_or_none = container->result == bytecode::ElementRole::OptionalElement && p_base.args[0].is_optional();
		} else if (container && container->result == bytecode::ElementRole::OptionalElement) {
			// Of an untyped Array, Variant?, which tells an empty array from a
			// null element (spec §3.4, §3.8).
			result = optional_of(result);
			pop_or_none = true;
		}
		if (get_or_none) {
			result = optional_of(typed_dictionary ? p_base.args[1] : Type::of(TypeKind::Variant));
		}
		// Filled from an untyped container, whose elements are checked as the
		// program runs (spec §3.8); a container that keeps boxes only from one of
		// its type, and a literal is of the container's type.
		const bool literal = !r_expr.args.empty() && (r_expr.args[0]->kind == ExprKind::ArrayLiteral || r_expr.args[0]->kind == ExprKind::DictionaryLiteral);
		const bool filled = is_typed_container(p_base) && !is_boxed(p_base) && container && container->fills && !params.empty() && !literal;
		if (filled) {
			params[0] = Type::builtin(base_name);
		}
		if (!check_container_method(r_expr, p_base, container)) {
			return set(r_expr, Type::of(TypeKind::Error));
		}
		// A method that changes a value changes it where it came from, which
		// must be a place that can be written; Array and Dictionary are shared
		// and change in place.
		if (!method->is_const && !method->is_static && !is_shared(p_base)) {
			const Expr &base = *r_expr.base;
			const bool place = base.kind == ExprKind::Subscript || base.target == Target::Local || base.target == Target::Member || base.target == Target::StaticVar ||
					base.target == Target::Owner || base.target == Target::SelfProperty || base.target == Target::ObjectProperty || base.target == Target::Field ||
					base.target == Target::StructField;
			if (!place) {
				return fail(r_expr, "\"" + r_expr.text + "()\" changes the " + base_name + " it is called on, which is a temporary value here.");
			}
			if (!check_place(base)) {
				return set(r_expr, Type::of(TypeKind::Error));
			}
		}
		// A lambda passed to filter(), sort_custom() and the like takes the
		// elements (spec §3.8, §4.6). That of reduce() also takes what it
		// accumulates, which has no type here.
		for (size_t i = 0; base_name == "Array" && r_expr.text != "reduce" && i < r_expr.args.size() && i < params.size(); i++) {
			if (params[i] == Type::builtin("Callable") && r_expr.args[i]->kind == ExprKind::Lambda) {
				check_lambda(*r_expr.args[i], element_function(r_expr.text, p_base));
			}
		}
		check_arguments(r_expr, params, int(params.size()), method->is_vararg);
		r_expr.checks_elements = (base_name == "Array" || base_name == "Dictionary") && p_base.args.empty();
		if (filled && !r_expr.args.empty()) {
			const Type &given = r_expr.args[0]->type;
			if (is_typed_container(given) && given != p_base) {
				error(r_expr.args[0]->range, "Cannot pass a value of type " + given.to_string() + " as " + p_base.to_string() + ".");
			}
			r_expr.checks_elements = !is_typed_container(given);
		}
		r_expr.is_vararg = method->is_vararg;
		// resize() sets the elements that the engine fills with null or an empty
		// untyped container to their defaults (spec §3.8).
		const bool resize = r_expr.text == "resize" && base_name == "Array" && !p_base.args.empty() && (is_boxed(p_base) || is_typed_container(p_base.args[0]));
		r_expr.target = get_or_none ? Target::GetOrNone : (pop_or_none ? Target::PopOrNone : (resize ? Target::ResizeWithDefaults : Target::BuiltinMethod));
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
		// The engine's Callable keeps them as Variants.
		for (size_t i = 0; i < bound; i++) {
			const std::string loss = encoding_loss(bound_params[i], "the engine", false);
			if (!loss.empty()) {
				error(r_expr.args[i]->range, "bind() keeps a value of type " + bound_params[i].to_string() + ", but " + loss);
			}
		}
		r_expr.target = Target::BindArguments;
		refer(r_expr.name_token, engine_id("Callable", "bind"));
		Type type = Type::function(p_function.args[0], rest);
		type.is_async = p_function.is_async;
		return set(r_expr, type);
	}

	// `f.call(x)`, checked against f's type, and `f.callv(args)`, whose
	// arguments the function checks as it is called (spec §3.11). A program
	// error in it aborts the caller too (§8.1). An async one gives its Task
	// (§9.1).
	Type check_function_call(Expr &r_expr, const Type &p_function) {
		const bool spread = r_expr.text == "callv";
		const std::vector<Type> params(p_function.args.begin() + 1, p_function.args.end());
		hint(r_expr, [&](int p_current) { return signature(r_expr.text, spread ? std::vector<std::string>{ "arguments: Array" } : type_names(params), false, p_current, p_function.args[0].to_string()); });
		check_arguments(r_expr, spread ? std::vector<Type>{ Type::builtin("Array") } : params, spread ? 1 : int(params.size()), false);
		r_expr.target = Target::CallValue;
		refer(r_expr.name_token, engine_id("Callable", r_expr.text));
		r_expr.is_async = p_function.is_async;
		return set(r_expr, p_function.is_async ? Type::task(p_function.args[0]) : p_function.args[0]);
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
		if (in_constant) {
			return fail(r_expr, "A lambda is not a constant expression.");
		}
		lambda.is_static = function ? function->is_static : static_initializing >= 0;
		// A function type has no rest parameter (spec §4.6).
		const bool typed_by_expected = p_expected.kind == TypeKind::Function && p_expected.args.size() == lambda.params.size() + 1 && !lambda.has_rest();
		for (size_t i = 0; i < lambda.params.size(); i++) {
			ParamDecl &param = lambda.params[i];
			if (param.default_value) {
				error(param.default_value->range, "Lambda parameters have no default values.");
			}
			if (param.declared_type) {
				param.type = resolve(*param.declared_type, false);
			} else if (typed_by_expected) {
				param.type = p_expected.args[i + 1];
			} else {
				untyped(param, ": nothing the lambda goes to gives one");
			}
		}
		check_rest(lambda);
		Returns inferred;
		if (typed_by_expected) {
			inferred.expected = p_expected.args[0];
		}
		Returns *const outer_returns = returns;
		if (lambda.return_type_ref) {
			lambda.return_type = resolve(*lambda.return_type_ref, true);
			returns = nullptr;
		} else {
			returns = &inferred;
		}

		enclosing.push_back({ function, std::move(scopes), std::move(narrowed), &r_expr, loop_depth, in_predelete, initializing, initializing_onready, static_initializing });
		function = &lambda;
		scopes = { {} };
		narrowed = { {} };
		loop_depth = 0;
		in_predelete = false;
		const bool was_assert = in_assert;
		in_assert = false;
		initializing = -1;
		static_initializing = -1;
		declare_params(lambda);
		check_block(lambda.body);
		in_assert = was_assert;
		if (!lambda.return_type_ref) {
			lambda.return_type = inferred.type.value_or(Type::of(TypeKind::Void));
			if (inferred.type && inferred.bare) {
				error(r_expr.range, function_label() + " returns a value on some paths and none on others.");
			}
			if (partial(lambda.return_type)) {
				const Type &left = lambda.return_type;
				const std::string missing = !left.is_result() ? "which T? None is" : (left.args[0].is_error() ? "what Ok(...) holds" : "what Err(...) holds");
				error(r_expr.range, function_label() + " needs a return type: what it returns does not say " + missing + ".");
				lambda.return_type = Type::of(TypeKind::Error);
			}
			for (Expr *value : inferred.partial) {
				set(*value, lambda.return_type);
			}
		}
		if (lambda.return_type.kind != TypeKind::Void && !lambda.return_type.is_error() && !always_ends(lambda.body, false)) {
			error(r_expr.range, "Not all paths of " + (lambda.name.empty() ? std::string("the lambda") : "the lambda \"" + lambda.name + "\"") + " return a value.");
		}
		// Its task keeps the result as the engine does (§9.8).
		const std::string loss = lambda.is_async ? encoding_loss(lambda.return_type, "the engine", false) : std::string();
		if (!loss.empty()) {
			error(r_expr.range, function_label() + " returns " + lambda.return_type.to_string() + ", but " + loss);
		}
		Enclosing &outer = enclosing.back();
		function = outer.function;
		scopes = std::move(outer.scopes);
		narrowed = std::move(outer.narrowed);
		loop_depth = outer.loop_depth;
		in_predelete = outer.in_predelete;
		initializing = outer.initializing;
		initializing_onready = outer.initializing_onready;
		static_initializing = outer.static_initializing;
		enclosing.pop_back();
		returns = outer_returns;

		return set(r_expr, function_type(lambda));
	}

	// The lambda or method value with a rest parameter that the value is, a
	// Callable (spec §4.6).
	static const FuncDecl *rest_function(const Expr &p_value) {
		const bool method = p_value.target == Target::MethodValue || p_value.target == Target::StaticValue || p_value.target == Target::StructMethodValue || p_value.target == Target::NewValue;
		const FuncDecl *declared = p_value.kind == ExprKind::Lambda ? p_value.function.get() : (method ? p_value.callee : nullptr);
		return declared && declared->has_rest() ? declared : nullptr;
	}

	// A function connected to a signal takes what the signal passes (spec §4.7).
	void check_handler(const Expr &p_handler, const Type &p_signal) {
		const Type &handler = p_handler.type;
		if (handler.is_error()) {
			return;
		}
		// A Callable, checked by its declaration.
		if (const FuncDecl *declared = rest_function(p_handler)) {
			if (!takes(*declared, p_signal.args.size())) {
				error(p_handler.range, p_signal.to_string() + " passes " + std::to_string(p_signal.args.size()) + " argument(s), but " + signature_text(*declared) + " takes " + argument_count(*declared) + ".");
				return;
			}
			for (size_t i = 0; i < p_signal.args.size(); i++) {
				if (!assignable(p_signal.args[i], *argument_type(*declared, i))) {
					error(p_handler.range, p_signal.to_string() + " passes " + p_signal.args[i].to_string() + " as argument " + std::to_string(i + 1) + ", which " + signature_text(*declared) + " does not take.");
					return;
				}
			}
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

	// The methods of T? (spec §3.4), as Rust's. Those that take a function
	// call it only when there is a value, or only when there is none. ok_or()
	// takes the error type from the Result expected, if any.
	Type check_option_method(Expr &r_expr, const Type &p_base, const Type &p_expected) {
		const std::string &name = r_expr.text;
		const Type value = p_base.unwrapped();
		const std::string t = value.to_string();
		if (!option_methods().count(name)) {
			return fail_call(r_expr, "The value may be None (" + p_base.to_string() + "); check it before calling \"" + name + "()\".");
		}
		refer(r_expr.name_token, language_id("Option", name));
		r_expr.target = name == "unwrap" || name == "expect" ? Target::Unwrap : Target::OptionMethod;
		if (name == "flatten") {
			hint(r_expr, [&](int p_current) { return signature(name, {}, false, p_current, t); });
			check_arguments(r_expr, {}, 0, false);
			if (!value.is_optional()) {
				return fail_call(r_expr, p_base.to_string() + " has no method \"flatten()\", which makes a T?? into a T?.");
			}
			return set(r_expr, value);
		}
		if (name == "unwrap" || name == "expect" || name == "unwrap_or" || name == "or") {
			const std::vector<Type> params = name == "unwrap" ? std::vector<Type>() : std::vector<Type>{ name == "expect" ? Type::of(TypeKind::String) : (name == "or" ? p_base : value) };
			const std::string shown = name == "expect" ? "message: String" : (name == "or" ? "other: " + p_base.to_string() : "default: " + t);
			hint(r_expr, [&](int p_current) { return signature(name, params.empty() ? std::vector<std::string>() : std::vector<std::string>{ shown }, false, p_current, name == "or" ? p_base.to_string() : t); });
			check_arguments(r_expr, params, int(params.size()), false);
			return set(r_expr, name == "or" ? p_base : value);
		}
		if (name == "ok_or") {
			const Type wanted = p_expected.is_result() ? p_expected.args[1] : Type::of(TypeKind::Error);
			hint(r_expr, [&](int p_current) { return signature(name, { "error: E" }, false, p_current, "Result[" + t + ", E]"); });
			if (r_expr.args.size() != 1) {
				return fail_call(r_expr, "Function \"ok_or()\" takes 1 argument(s), " + std::to_string(r_expr.args.size()) + " given.");
			}
			Expr &arg = *r_expr.args[0];
			Type failure = check_value(arg, wanted);
			if (!wanted.is_error()) {
				expect_assignable(arg, failure, wanted, "pass");
				failure = wanted;
			}
			if (failure.kind == TypeKind::None) {
				return fail(r_expr, "ok_or(None) needs the Result type it makes, from where it goes.");
			}
			r_expr.param_types.push_back(failure);
			return set(r_expr, failure.is_error() ? failure : Type::result(value, failure));
		}
		// The others take a function: of the value, or of nothing.
		const bool of_value = name == "map" || name == "and_then" || name == "filter";
		const std::map<std::string, std::string> returns = { { "unwrap_or_else", t }, { "map", "U" }, { "and_then", "U?" }, { "or_else", p_base.to_string() }, { "filter", "bool" }, { "ok_or_else", "E" } };
		const std::map<std::string, std::string> results = { { "unwrap_or_else", t }, { "map", "U?" }, { "and_then", "U?" }, { "or_else", p_base.to_string() }, { "filter", p_base.to_string() }, { "ok_or_else", "Result[" + t + ", E]" } };
		hint(r_expr, [&](int p_current) { return signature(name, { "f: func(" + (of_value ? t : std::string()) + ") -> " + returns.at(name) }, false, p_current, results.at(name)); });
		const std::map<std::string, Type> fitting = { { "unwrap_or_else", value }, { "or_else", p_base }, { "filter", Type::of(TypeKind::Bool) } };
		const auto fits_type = fitting.find(name);
		// and_then()'s function returns some U?, which its returns say.
		const Type wanted = fits_type != fitting.end() ? fits_type->second : (name == "and_then" ? Type::option(Type::of(TypeKind::Error)) : Type::of(TypeKind::Error));
		const std::optional<Type> returned = check_function_argument(r_expr, of_value ? std::vector<Type>{ value } : std::vector<Type>(), wanted);
		if (!returned || returned->is_error()) {
			return set(r_expr, Type::of(TypeKind::Error));
		}
		const bool fits = fits_type != fitting.end() ? assignable(*returned, fits_type->second) : (name == "and_then" ? returned->is_optional() : returned->kind != TypeKind::Void);
		if (!fits) {
			error(r_expr.args[0]->range, "The function passed to " + name + "() returns " + returned->to_string() + ", not " + returns.at(name) + ".");
			return set(r_expr, Type::of(TypeKind::Error));
		}
		if (name == "map") {
			return set(r_expr, optional_of(*returned));
		}
		if (name == "and_then") {
			return set(r_expr, *returned);
		}
		return set(r_expr, name == "ok_or_else" ? Type::result(value, *returned) : (name == "unwrap_or_else" ? value : p_base));
	}

	// `Some(v)` (spec §3.4): of the T? it goes to, which says what v is, or of
	// v's type.
	Type check_some(Expr &r_expr, const Type &p_expected) {
		const Type inner = p_expected.is_optional() ? p_expected.unwrapped() : Type::of(TypeKind::Error);
		hint(r_expr, [&](int p_current) { return signature("Some", { "value: " + (inner.is_error() ? std::string("T") : inner.to_string()) }, false, p_current, inner.is_error() ? std::string("T?") : p_expected.to_string()); });
		if (!r_expr.arg_names.empty() || r_expr.args.size() != 1) {
			for (auto &arg : r_expr.args) {
				check_value(*arg);
			}
			return fail_call(r_expr, "Some(...) takes one value by position, as in Some(x).");
		}
		refer(r_expr.name_token, language_id("Option", "Some"));
		Expr &arg = *r_expr.args[0];
		const Type given = check_value(arg, inner);
		if (given.is_error()) {
			return set(r_expr, given);
		}
		if (!inner.is_error()) {
			expect_assignable(arg, given, inner, "pass");
		} else if (given.kind == TypeKind::None || given.kind == TypeKind::Void) {
			return fail_call(r_expr, std::string("Some(") + (given.kind == TypeKind::None ? "None" : "...") + ") does not say which type it holds; declare the type it goes to.");
		}
		const Type value = inner.is_error() ? given : inner;
		r_expr.target = Target::Some;
		r_expr.param_types = { value };
		return set(r_expr, optional_of(value));
	}

	// `Ok(v)` and `Err(e)` (spec §3.5): of the Result they go to, which says
	// what v or e is.
	Type check_result_construction(Expr &r_expr, const Type &p_expected) {
		const Type result = p_expected.kind == TypeKind::Option ? p_expected.args[0] : p_expected;
		if (!result.is_result() || !r_expr.arg_names.empty()) {
			if (!r_expr.arg_names.empty()) {
				return fail_call(r_expr, r_expr.text + "(...) takes its value by position, as in " + r_expr.text + "(x).");
			}
			if (!result.is_error()) {
				return fail_call(r_expr, r_expr.text + "(...) makes a Result, but a value of type " + result.to_string() + " goes here.");
			}
			return fail_call(r_expr, r_expr.text + "(...) takes its type from where it goes, as in `var r: Result[int, String] = " + r_expr.text + "(...)`; nothing here gives one.");
		}
		const int variant = r_expr.text == "Ok" ? 0 : 1;
		refer(r_expr.name_token, language_id("Result", r_expr.text));
		r_expr.target = Target::VariantConstructor;
		r_expr.index = variant;
		// Returned by a lambda going to and_then() or or_else(), whose Result
		// leaves this side to what the lambda returns (spec §4.6).
		if (result.args[variant].is_error() && r_expr.args.size() == 1) {
			const Type given = check_value(*r_expr.args[0]);
			if (given.kind == TypeKind::None || given.kind == TypeKind::Void) {
				return fail_call(r_expr, r_expr.text + "(" + (given.kind == TypeKind::None ? "None" : "...") + ") does not say which type it holds; declare the lambda's return type.");
			}
			r_expr.param_types.push_back(given);
			Type made = result;
			made.args[variant] = given;
			return given.is_error() ? set(r_expr, given) : set(r_expr, Type::result(made.args[0], made.args[1]));
		}
		hint(r_expr, [&](int p_current) { return signature(r_expr.text, { (variant == 0 ? "value: " : "error: ") + result.args[variant].to_string() }, false, p_current, result.to_string()); });
		check_arguments(r_expr, { result.args[variant] }, 1, false);
		return set(r_expr, result);
	}

	// The methods of Result[T, E] (spec §3.5), as Rust's. Those that take a
	// function call it only on the variant they are for.
	Type check_result_method(Expr &r_expr, const Type &p_base) {
		const std::string &name = r_expr.text;
		const Type &value = p_base.args[0];
		const Type &failure = p_base.args[1];
		const std::string t = value.to_string();
		const std::string e = failure.to_string();
		if (!result_methods().count(name)) {
			return fail_call(r_expr, p_base.to_string() + " has no method \"" + name + "()\".");
		}
		refer(r_expr.name_token, language_id("Result", name));
		r_expr.target = Target::ResultMethod;
		if (name == "unwrap" || name == "expect" || name == "unwrap_or") {
			const std::vector<Type> params = name == "unwrap" ? std::vector<Type>() : std::vector<Type>{ name == "expect" ? Type::of(TypeKind::String) : value };
			hint(r_expr, [&](int p_current) { return signature(name, params.empty() ? std::vector<std::string>() : std::vector<std::string>{ name == "expect" ? "message: String" : "default: " + t }, false, p_current, t); });
			check_arguments(r_expr, params, int(params.size()), false);
			return set(r_expr, value);
		}
		if (name == "ok" || name == "err") {
			const Type &inner = name == "ok" ? value : failure;
			hint(r_expr, [&](int p_current) { return signature(name, {}, false, p_current, optional_of(inner).to_string()); });
			check_arguments(r_expr, {}, 0, false);
			return set(r_expr, optional_of(inner));
		}
		// The function's parameter, and what it returns as the hint shows it.
		const bool on_error = name == "unwrap_or_else" || name == "map_err" || name == "or_else";
		const std::map<std::string, std::string> returns = { { "unwrap_or_else", t }, { "map", "U" }, { "map_err", "F" }, { "and_then", "Result[U, " + e + "]" }, { "or_else", "Result[" + t + ", F]" } };
		const std::map<std::string, std::string> results = { { "unwrap_or_else", t }, { "map", "Result[U, " + e + "]" }, { "map_err", "Result[" + t + ", F]" }, { "and_then", "Result[U, " + e + "]" }, { "or_else", "Result[" + t + ", F]" } };
		hint(r_expr, [&](int p_current) { return signature(name, { "f: func(" + (on_error ? e : t) + ") -> " + returns.at(name) }, false, p_current, results.at(name)); });
		// and_then()'s function returns some Result[U, E], or_else()'s some
		// Result[T, F], which its returns say.
		const std::map<std::string, Type> wanted = { { "unwrap_or_else", value }, { "and_then", Type::result(Type::of(TypeKind::Error), failure) }, { "or_else", Type::result(value, Type::of(TypeKind::Error)) } };
		const auto known = wanted.find(name);
		const std::optional<Type> returned = check_function_argument(r_expr, { on_error ? failure : value }, known != wanted.end() ? known->second : Type::of(TypeKind::Error));
		if (!returned || returned->is_error()) {
			return set(r_expr, Type::of(TypeKind::Error));
		}
		// and_then keeps the error type, or_else the value type.
		const size_t kept = name == "and_then" ? 1 : 0;
		const bool fits = name == "unwrap_or_else" ? assignable(*returned, value) : (name == "map" || name == "map_err" ? returned->kind != TypeKind::Void : returned->is_result() && returned->args[kept] == p_base.args[kept]);
		if (!fits) {
			error(r_expr.args[0]->range, "The function passed to " + name + "() returns " + returned->to_string() + ", not " + returns.at(name) + ".");
			return set(r_expr, Type::of(TypeKind::Error));
		}
		if (name == "unwrap_or_else") {
			return set(r_expr, value);
		}
		if (name == "map" || name == "map_err") {
			return set(r_expr, name == "map" ? Type::result(*returned, failure) : Type::result(value, *returned));
		}
		return set(r_expr, *returned);
	}

	// The one argument of map() and the like: a function whose parameters take
	// `p_params`, none or one, which a lambda's parameters then have unless
	// written, as its returned values take `p_returns` when known; what it
	// returns, or nullopt after reporting.
	std::optional<Type> check_function_argument(Expr &r_call, const std::vector<Type> &p_params, const Type &p_returns = Type::of(TypeKind::Error)) {
		if (r_call.args.size() != 1) {
			for (auto &arg : r_call.args) {
				check_value(*arg);
			}
			error(r_call.range, "Function \"" + r_call.text + "()\" takes 1 argument(s), " + std::to_string(r_call.args.size()) + " given.");
			return std::nullopt;
		}
		Expr &arg = *r_call.args[0];
		const Type function = check_value(arg, Type::function(p_returns, p_params));
		if (function.is_error()) {
			return std::nullopt;
		}
		if (function.kind != TypeKind::Function || function.is_async || function.args.size() != p_params.size() + 1 || (!p_params.empty() && !assignable(p_params[0], function.args[1]))) {
			const std::string wanted = p_params.empty() ? "without parameters" : "of one parameter, which takes " + p_params[0].to_string();
			error(arg.range, "\"" + r_call.text + "()\" takes a function " + wanted + "; found " + function.to_string() + ".");
			return std::nullopt;
		}
		r_call.param_types.push_back(function);
		return function.args[0];
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
		Type a = check_value(*r_expr.left, p_expected);
		narrowed.back() = facts.when_false;
		Type b = check_value(*r_expr.right, p_expected);
		narrowed.pop_back();
		if (a.is_error() || b.is_error()) {
			return set(r_expr, Type::of(TypeKind::Error));
		}
		// As Ok(x) if c else Err(e), returned by a lambda going to and_then().
		if (a.is_result() && partial(a) && !partial(b)) {
			a = set(*r_expr.left, completed(a, b));
		} else if (b.is_result() && partial(b) && !partial(a)) {
			b = set(*r_expr.right, completed(b, a));
		}
		if (!p_expected.is_error() && !partial(p_expected) && assignable(a, p_expected) && assignable(b, p_expected)) {
			expect_implemented(*r_expr.left, a, p_expected);
			expect_implemented(*r_expr.right, b, p_expected);
			return set(r_expr, p_expected);
		}
		Type result = common_type(a, b);
		if (result.is_error() && (a.kind == TypeKind::None) != (b.kind == TypeKind::None)) {
			result = optional_of(a.kind == TypeKind::None ? b : a);
		}
		if (result.is_error()) {
			return fail(r_expr, "The branches have types " + a.to_string() + " and " + b.to_string() + ", which have no common type; declare the type.");
		}
		return set(r_expr, result);
	}

	// `a[i]`, or `d[k]` (spec §3.8); `p_written` when it is only assigned to.
	Type check_subscript(Expr &r_expr) {
		const Type base = check_value(*r_expr.base);
		const std::optional<std::pair<Type, Type>> entries = base.is_error() ? std::nullopt : entries_of(base);
		const Type index = check_value(*r_expr.right, entries ? entries->first : Type::of(TypeKind::Error));
		if (base.is_error() || index.is_error()) {
			return set(r_expr, Type::of(TypeKind::Error));
		}
		if (base.is_optional()) {
			return fail(r_expr, "The value may be None (" + base.to_string() + "); check it before indexing it.");
		}
		if (base.kind == TypeKind::Variant) {
			return fail(r_expr, "Cannot index a Variant." + variant_hint());
		}
		if (entries) {
			if (!assignable(index, entries->first)) {
				return fail(*r_expr.right, "The keys of " + base.to_string() + " are " + entries->first.to_string() + ", not " + index.to_string() + ".");
			}
			r_expr.param_types = { entries->first };
			return set(r_expr, entries->second);
		}
		// A String's elements are its characters, each a String.
		const std::optional<Type> element = base.kind == TypeKind::String ? Type::of(TypeKind::String) : element_of(base);
		if (!element) {
			return fail(r_expr, "Values of type " + base.to_string() + " cannot be indexed.");
		}
		if (index.kind != TypeKind::Int) {
			return fail(*r_expr.right, "An index must be int, found " + index.to_string() + ".");
		}
		return set(r_expr, *element);
	}

	// `[a, b]` takes its type from where it goes (spec §4.3).
	Type check_array_literal(Expr &r_expr, const Type &p_expected) {
		// Into a Variant, as in an untyped Array, it is an untyped Array.
		const Type wanted = p_expected.kind == TypeKind::Option ? p_expected.args[0] : p_expected;
		const Type expected = wanted.kind == TypeKind::Variant ? Type::builtin("Array") : wanted;
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
			} else {
				expect_implemented(*arg, type, element);
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
		const Type wanted = p_expected.kind == TypeKind::Option ? p_expected.args[0] : p_expected;
		const Type expected = wanted.kind == TypeKind::Variant ? Type::builtin("Dictionary") : wanted;
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
			} else {
				expect_implemented(*r_expr.args[i], key, want_key);
			}
			if (!assignable(value, entries->second)) {
				error(r_expr.args[i + 1]->range, "Cannot put a value of type " + value.to_string() + " in " + expected.to_string() + ".");
			} else {
				expect_implemented(*r_expr.args[i + 1], value, entries->second);
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
		if (from.kind == TypeKind::Variant) {
			// None also when it holds no object (spec §5.3).
			r_expr.tested = to;
			Type result = to;
			result.nullable = true;
			return set(r_expr, result);
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
			return fail_call(r_expr, "\"" + callee->name + "()\" is abstract in the base class, which has no body to call.");
		}
		if (callee) {
			const ClassDecl &owner = declaring(*cls.base, *callee, &ClassDecl::functions);
			refer(r_expr.name_token, script_id(owner.path, callee->name));
			if (!callee->is_static && refuse_in_static(r_expr, "super." + r_expr.text)) {
				return fail_call(r_expr);
			}
			const Type result = call_declared(r_expr, *callee, *owner.source, callee->return_type);
			r_expr.target = Target::SuperMethod;
			return result;
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
			if (function_index.count(p_call.text)) {
				return std::nullopt;
			}
			if (!cls.find_struct(p_call.text)) {
				// One of a class around this one (spec §4.1).
				const Checker *around = class_around(p_call.text);
				if (!around || !around->cls.find_struct(p_call.text)) {
					if (p_call.text == "ConversionError" && !declares_in_script(p_call.text)) {
						refer(p_call.name_token, language_id(p_call.text, std::string()));
						return Type::conversion_error();
					}
					return std::nullopt;
				}
			}
			ref.name = p_call.text;
		} else {
			const Expr &base = *p_call.base;
			const std::string path = class_path_of(base, true);
			const ClassDecl *header = path.empty() ? nullptr : script_header(path);
			if (!header || !header->find_struct(p_call.text)) {
				return std::nullopt;
			}
			ref.name_token = first_name_token(base); // Each name two tokens after the one before.
			ref.name = dotted_name(base) + "." + p_call.text;
		}
		return script_type(ref);
	}

	// `Command.MOVE(...)`: the enum with payloads whose variant the call
	// makes, if any.
	const EnumDecl *called_variant(Expr &r_call) {
		return r_call.base ? payload_enum_named(*r_call.base) : nullptr;
	}

	// The type that the base of `T.try_from(v)` names: `int`, `Combat.Hit`,
	// `Array[Hit]` or `Dictionary[String, int]`; nullopt for a value, with
	// nothing referred to or reported.
	std::optional<Type> named_type(const Expr &p_base) {
		TypeRef ref;
		if (!type_ref_of(p_base, ref) || (p_base.kind == ExprKind::Name && (visible_local(p_base.text) || member_index.count(p_base.text) || static_index.count(p_base.text) || constant_index.count(p_base.text)))) {
			return std::nullopt;
		}
		const size_t referred = references ? references->size() : 0;
		const size_t reported = errors.size();
		const Type type = resolve(ref, false);
		if (!type.is_error()) {
			return type;
		}
		if (references) {
			references->resize(referred);
		}
		errors.resize(reported);
		return std::nullopt;
	}

	// An expression written as a type is: names joined by dots, `?` after one,
	// and `Array[...]` or `Dictionary[..., ...]`.
	static bool type_ref_of(const Expr &p_expr, TypeRef &r_ref) {
		r_ref.range = p_expr.range;
		switch (p_expr.kind) {
			case ExprKind::Name:
				r_ref.name = p_expr.text;
				r_ref.name_token = p_expr.name_token;
				return true;
			case ExprKind::Member:
				if (!type_ref_of(*p_expr.base, r_ref) || !r_ref.args.empty() || r_ref.optional > 0) {
					return false;
				}
				r_ref.name += "." + p_expr.text;
				r_ref.range = p_expr.range;
				return true;
			case ExprKind::Try:
				if (!type_ref_of(*p_expr.left, r_ref)) {
					return false;
				}
				r_ref.optional++;
				r_ref.range = p_expr.range;
				return true;
			case ExprKind::Subscript: {
				if (p_expr.base->kind != ExprKind::Name || (p_expr.base->text != "Array" && p_expr.base->text != "Dictionary")) {
					return false;
				}
				r_ref.name = p_expr.base->text;
				r_ref.name_token = p_expr.base->name_token;
				std::vector<const Expr *> args = { p_expr.right.get() };
				for (const auto &arg : p_expr.args) {
					args.push_back(arg.get());
				}
				for (const Expr *arg : args) {
					r_ref.args.emplace_back();
					if (!type_ref_of(*arg, r_ref.args.back())) {
						return false;
					}
				}
				return true;
			}
			default:
				return false;
		}
	}

	// `T.try_from(v)` (spec §5.3): v as a T, decoded as a struct's field is
	// (§3.6), or Err with why not. Any value goes in as a Variant.
	Type check_try_from(Expr &r_expr, const Type &p_type) {
		const Type result = Type::result(p_type, Type::conversion_error());
		hint(r_expr, [&](int p_current) { return signature("try_from", { "value: Variant" }, false, p_current, result.to_string()); });
		refer(r_expr.name_token, language_id(p_type.to_string(), r_expr.text));
		if (p_type.kind == TypeKind::Function || p_type.kind == TypeKind::Signal || p_type.kind == TypeKind::Task || p_type.kind == TypeKind::Unsupported) {
			return fail_call(r_expr, "try_from() converts to a value or an object type, not " + p_type.to_string() + ".");
		}
		check_arguments(r_expr, { Type::of(TypeKind::Variant) }, 1, false);
		r_expr.target = Target::TryFrom;
		return set(r_expr, result);
	}

	// The enum with payloads that the base of `Command.MOVE` names, if any;
	// for any other base, nothing is referred to or reported, as it is then
	// checked as what it is.
	const EnumDecl *payload_enum_named(Expr &r_base) {
		const size_t referred = references ? references->size() : 0;
		const size_t reported = errors.size();
		const EnumDecl *decl = named_enum(r_base);
		if (decl && decl->has_payloads) {
			return decl;
		}
		if (references) {
			references->resize(referred);
		}
		errors.resize(reported);
		return nullptr;
	}

	// The fields of a variant, by name (spec §3.7), in a call that the
	// arguments' names map to them.
	Type check_variant_construction(Expr &r_expr, const EnumDecl &p_decl) {
		const Type &type = p_decl.type;
		const int index = p_decl.find(r_expr.text);
		if (index < 0) {
			return fail_call(r_expr, missing(p_decl.name + " has no variant \"" + r_expr.text + "\".", p_decl.script));
		}
		const EnumValueDecl &variant = p_decl.values[index];
		refer(r_expr.name_token, script_id(p_decl.script, enum_value_id(p_decl, variant.name)));
		const std::string written = enum_spelling(type) + "." + variant.name;
		hint(r_expr, [&](int) {
			std::vector<std::string> fields;
			for (const VarDecl &field : variant.fields) {
				fields.push_back(field.name + ": " + field.type.to_string());
			}
			return signature(variant.name, fields, false, -1, type.to_string());
		});
		if (variant.fields.empty()) {
			return fail_call(r_expr, written + " has no payload; write it without parentheses.");
		}
		check_named_fields(r_expr, variant.fields, "variant", written, p_decl.script, enum_value_id(p_decl, variant.name));
		r_expr.target = Target::VariantConstructor;
		r_expr.index = index;
		return set(r_expr, type);
	}

	// The arguments of a struct's or a variant's construction (`p_kind`), each
	// given by the name of a field, once; a field without a default must be
	// (spec §3.6, §3.7). `p_id` is what the fields' references are under.
	void check_named_fields(Expr &r_expr, const std::vector<VarDecl> &p_fields, const std::string &p_kind, const std::string &p_written, const std::string &p_script, const std::string &p_id) {
		std::vector<bool> given(p_fields.size(), false);
		for (size_t i = 0; i < r_expr.args.size(); i++) {
			Expr &arg = *r_expr.args[i];
			const std::string name = i < r_expr.arg_names.size() ? r_expr.arg_names[i] : std::string();
			const auto field = std::find_if(p_fields.begin(), p_fields.end(), [&](const VarDecl &p_field) { return p_field.name == name; });
			if (field == p_fields.end()) {
				check_value(arg);
				r_expr.param_types.push_back(Type::of(TypeKind::Error));
				error(arg.range, name.empty() ? "A " + p_kind + " takes its fields by name, as in " + p_written + "(" + (p_fields.empty() ? std::string() : p_fields[0].name + " = ...") + ")." : missing(p_written + " has no field \"" + name + "\".", p_script));
				continue;
			}
			if (given[field - p_fields.begin()]) {
				error(arg.range, "\"" + name + "\" is given twice.");
			}
			given[field - p_fields.begin()] = true;
			refer(r_expr.arg_name_tokens[i], script_id(p_script, p_id + "." + name));
			r_expr.param_types.push_back(field->type);
			expect_assignable(arg, check_value(arg, field->type), field->type, "pass");
		}
		for (size_t field = 0; field < p_fields.size(); field++) {
			if (!given[field] && !p_fields[field].initializer) {
				// A variant's fields have no defaults.
				error(r_expr.range, p_written + "() needs \"" + p_fields[field].name + "\"" + (p_kind == "struct" ? ", which has no default value." : "."));
			}
		}
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
		// Its fields, which are given by name, so none is marked.
		hint(r_expr, [&](int) {
			const SourceFile &source = *script_header(p_struct.script)->source;
			std::vector<std::string> fields;
			for (const VarDecl &field : decl->fields) {
				fields.push_back(field.name + ": " + field.type.to_string());
				if (field.initializer) {
					const uint32_t begin = source.begin(field.initializer->range);
					fields.back() += " = " + source.text.substr(begin, source.end(field.initializer->range) - begin);
				}
			}
			return signature(p_struct.interface_name(), fields, false, -1, p_struct.to_string());
		});
		check_named_fields(r_expr, decl->fields, "struct", p_struct.name, p_struct.script, decl->name);
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
		// A try_ version (spec §10.3), which no engine member of its name hides.
		if (const char *getter = builtin::error_getter(base.text, r_expr.text)) {
			const auto method = engine.find_method(base.text, std::string(builtin::tried_method(r_expr.text)));
			r_expr.getter = engine.find_method(base.text, getter);
			if (method && r_expr.getter) {
				return check_engine_call(r_expr, *method, base.text);
			}
			r_expr.getter.reset();
		}
		if (const auto method = engine.find_method(base.text, r_expr.text)) {
			return check_engine_call(r_expr, *method, base.text);
		}
		return fail_call(r_expr, base.text + " has no method \"" + r_expr.text + "()\".");
	}

	// What a try_ version gives (spec §10.3): what the engine method returns
	// when it succeeds, or the Error its getter reads.
	static Type tried_result(const EngineMethod &p_method, const EngineMethod &p_getter) {
		Type value = engine_result(p_method);
		value.nullable = false;
		return Type::result(value, p_getter.return_type);
	}

	// What new(...) of the class at `p_path` calls: the nearest _init(), whose
	// arguments it takes and whose visibility it has, or null for none (spec
	// §4.2, §6.1). False after reporting why the class cannot be made here.
	bool find_init(Expr &r_expr, const ClassDecl &p_header, const std::string &p_path, const FuncDecl *&r_init) {
		const std::string class_name = display_name(p_header, p_path);
		const std::string native = script_class_type(p_header, p_path).engine_class();
		std::string refused;
		const ClassDecl *decl = script_decl(p_path);
		if (p_header.is_abstract) {
			refused = class_name + " is abstract and cannot be instantiated.";
		} else if (!engine.can_instantiate(native)) {
			refused = class_name + " cannot be instantiated: the engine cannot instantiate " + native + ", which it extends.";
		} else if (!decl) {
			refused = class_name + " has errors.";
		} else {
			const auto init = std::find_if(decl->all_functions.begin(), decl->all_functions.end(), [](const FuncDecl *p_function) { return p_function->name == "_init"; });
			r_init = init == decl->all_functions.end() ? nullptr : *init;
			if (r_init && !r_init->is_pub && !sees(declaring(*decl, *r_init, &ClassDecl::functions).path)) {
				refused = "new() of " + class_name + " cannot be called here: its _init() is not pub.";
			} else if (const auto message = r_init ? broken(r_init, class_name + "._init()") : std::nullopt) {
				refused = *message;
			}
		}
		if (!refused.empty()) {
			fail_call(r_expr, refused);
		}
		return refused.empty();
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
			const FuncDecl *init = nullptr;
			if (!find_init(r_expr, *header, p_path, init)) {
				return r_expr.type;
			}
			if (init) {
				call_declared(r_expr, *init, *declaring(*script_decl(p_path), *init, &ClassDecl::functions).source, type);
			} else {
				hint(r_expr, [&](int p_current) { return signature("new", {}, false, p_current, type.to_string()); });
				check_arguments(r_expr, {}, 0, false);
			}
			refer(r_expr.name_token, script_id(p_path, "new"));
			r_expr.target = Target::NewObject;
			check_members(r_expr, type, false);
			return set(r_expr, type);
		}
		const ClassDecl *decl = script_decl(p_path);
		if (!decl) {
			return fail_call(r_expr, class_name + " has errors.");
		}
		const auto found = std::find_if(decl->all_functions.begin(), decl->all_functions.end(), [&](const FuncDecl *p_function) { return p_function->name == r_expr.text; });
		if (found == decl->all_functions.end()) {
			return fail_call(r_expr, missing(class_name + " has no method \"" + r_expr.text + "()\".", p_path));
		}
		const FuncDecl &callee = **found;
		if (!callee.is_static) {
			return fail_call(r_expr, "\"" + callee.name + "()\" of " + class_name + " is not static; call it on an object.");
		}
		const ClassDecl &owner = declaring(*decl, callee, &ClassDecl::functions);
		// A bare name is one the class around this one sees (spec §4.1).
		if (const auto message = refused(&callee, callee.is_pub || !r_expr.base || sees(owner.path), "", class_name, callee.name + "()")) {
			return fail_call(r_expr, *message);
		}
		const Type result = call_declared(r_expr, callee, *owner.source, callee.return_type);
		refer(r_expr.name_token, script_id(owner.path, callee.name));
		if (p_path == options.path) {
			r_expr.target = Target::ScriptFunction;
			r_expr.index = int(found - decl->all_functions.begin());
		} else {
			// Called on the class's current image (spec §12.4).
			r_expr.target = Target::ScriptMethod;
			r_expr.is_static = true;
			r_expr.owner = p_path;
		}
		return result;
	}

	// A method of a struct (spec §3.6): on a value; static through the
	// struct's name; or either bare in the struct's methods, an instance
	// method then on self.
	Type check_struct_call(Expr &r_expr, const Type &p_struct, bool p_on_value) {
		const StructDecl *decl = struct_decl(p_struct);
		if (!decl) {
			return fail_call(r_expr, p_struct.script + " has errors.");
		}
		const FuncDecl *callee = decl->find_method(r_expr.text);
		if (!callee) {
			return fail_call(r_expr, missing(p_struct.name + " has no method \"" + r_expr.text + "()\".", p_struct.script));
		}
		refer(r_expr.name_token, script_id(p_struct.script, callee->name));
		if (!callee->is_static && !p_on_value) {
			if (r_expr.base) {
				return fail_call(r_expr, "\"" + r_expr.text + "()\" of " + p_struct.name + " is not static; call it on a value.");
			}
			if (!has_struct_self()) {
				return fail_call(r_expr, "\"" + r_expr.text + "()\" needs self, which the " + selfless() + " does not have.");
			}
			r_expr.base = struct_self(r_expr);
			check_value(*r_expr.base);
		}
		r_expr.is_static = callee->is_static;
		r_expr.static_on_value = callee->is_static && p_on_value;
		const ClassDecl *declaring = p_struct.script == options.path ? &cls : script_decl(p_struct.script);
		const Type result = call_declared(r_expr, *callee, *declaring->source, callee->return_type);
		r_expr.target = Target::StructMethod;
		r_expr.callee = callee;
		r_expr.owner = p_struct.script;
		return result;
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
	// called on its script; new() takes what its _init does (spec §11.1). With
	// `p_value`, the method as a value (§3.11), as GDScript's `Class.new` is the
	// script's Callable (core/object/class_db.cpp:1655-1657).
	Type check_foreign_class_call(Expr &r_expr, const Type &p_class, bool p_value = false) {
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
		return p_value ? engine_method_value(r_expr, method) : check_engine_call(r_expr, method);
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

	Type check_call(Expr &r_expr, const Type &p_expected = Type::of(TypeKind::Error)) {
		const Type type = type_call(r_expr, p_expected);
		if (r_expr.member_types.size() < r_expr.member_values.size()) {
			for (size_t i = r_expr.member_types.size(); i < r_expr.member_values.size(); i++) {
				check_value(*r_expr.member_values[i]);
			}
			if (!type.is_error()) {
				return fail_call(r_expr, "Only new(...) of a class of this language and instantiate(...) of a PackedScene[T] take members by name.");
			}
		}
		return type;
	}

	// The members that new(...) or instantiate(...) assigns by name (spec
	// §4.6, §6.2), which come after the arguments by position: set apart from
	// them, to be checked once the class made is known.
	bool split_members(Expr &r_call) {
		size_t positional = 0;
		while (positional < r_call.args.size() && r_call.arg_names[positional].empty()) {
			positional++;
		}
		for (size_t i = positional; i < r_call.args.size(); i++) {
			if (r_call.arg_names[i].empty()) {
				error(r_call.args[i]->range, "Arguments by position come before those by name.");
				return false;
			}
		}
		for (size_t i = positional; i < r_call.args.size(); i++) {
			r_call.member_names.push_back(r_call.arg_names[i]);
			r_call.member_name_tokens.push_back(r_call.arg_name_tokens[i]);
			r_call.member_values.push_back(std::move(r_call.args[i]));
		}
		r_call.args.resize(positional);
		r_call.arg_names.clear();
		r_call.arg_name_tokens.clear();
		return true;
	}

	// The members by name of a call that makes an object of `p_made`: each
	// one that must be assigned, given once (spec §4.6, §6.2). instantiate(...)
	// must give those the scene does not export, and of a scene statically
	// known, `p_scene`, the exports it does not assign (§7.4).
	void check_members(Expr &r_call, const Type &p_made, bool p_instantiate, const Scene *p_scene = nullptr) {
		r_call.member_types.clear();
		const ClassDecl *decl = p_made.kind == TypeKind::Object && !p_made.script.empty() ? script_decl(p_made.script) : nullptr;
		std::unordered_set<std::string> given;
		for (size_t i = 0; i < r_call.member_values.size(); i++) {
			Expr &value = *r_call.member_values[i];
			const std::string &name = r_call.member_names[i];
			const VarDecl *found = nullptr;
			for (const VarDecl *candidate : decl ? decl->all_members : std::vector<const VarDecl *>()) {
				found = candidate->name == name ? candidate : found;
			}
			if (!found) {
				r_call.member_types.push_back(check_value(value));
				error(value.range, missing(p_made.to_string() + " has no member \"" + name + "\".", p_made.script));
				continue;
			}
			const VarDecl &member = *found;
			const std::string declared_in = declaring(*decl, member, &ClassDecl::members).path;
			refer(r_call.member_name_tokens[i], script_id(declared_in, name));
			r_call.member_types.push_back(member.type);
			const Type type = check_value(value, member.type);
			if (!member.is_pub && !sees(declared_in)) {
				error(value.range, "\"" + name + "\" of " + p_made.to_string() + " is not pub.");
			} else if (!member.must_assign) {
				error(value.range, "\"" + name + "\" has a value of its own; by name, " + r_call.text + "(...) takes only members that must be assigned.");
			} else if (!given.insert(name).second) {
				error(value.range, "\"" + name + "\" is given twice.");
			} else {
				expect_assignable(value, type, member.type, "assign");
			}
		}
		for (const VarDecl *member : decl &&p_instantiate ? decl->all_members : std::vector<const VarDecl *>()) {
			if (!member->must_assign || given.count(member->name) || (member->is_export && (!p_scene || stored_value(*p_scene, ".", member->name).value_or(false)))) {
				continue;
			}
			if (member->is_export) {
				error(r_call.range, "instantiate() needs " + member->name + " = ..., an export of " + p_made.to_string() + " that must be assigned and " + p_scene->path + " does not assign.");
			} else if (!member->is_pub && !sees(declaring(*decl, *member, &ClassDecl::members).path)) {
				error(r_call.range, "instantiate() cannot give \"" + member->name + "\", a member of " + p_made.to_string() + " that must be assigned and is not pub. Export it, make it pub or give it an initial value.");
			} else {
				error(r_call.range, "instantiate() needs " + member->name + " = ..., a member of " + p_made.to_string() + " that must be assigned and the scene cannot.");
			}
		}
	}

	Type type_call(Expr &r_expr, const Type &p_expected) {
		if (!r_expr.base && (r_expr.text == "Ok" || r_expr.text == "Err") && !declares_in_script(r_expr.text)) {
			return check_result_construction(r_expr, p_expected);
		}
		if (!r_expr.base && r_expr.text == "Some" && !declares_in_script(r_expr.text)) {
			return check_some(r_expr, p_expected);
		}
		if (r_expr.base && r_expr.text == "try_from") {
			if (const std::optional<Type> named = named_type(*r_expr.base)) {
				return check_try_from(r_expr, *named);
			}
		}
		if (r_expr.base && r_expr.base->kind == ExprKind::Name && r_expr.base->text == "Task" && !declares_in_script("Task") && !visible_local("Task")) {
			return check_task_function(r_expr);
		}
		if (const std::optional<Type> constructed = constructed_struct(r_expr)) {
			return check_struct_construction(r_expr, *constructed);
		}
		if (const EnumDecl *decl = called_enum(r_expr)) {
			if (decl->has_payloads) {
				return fail_call(r_expr, "No int converts to " + enum_spelling(decl->type) + ", an enum with payloads.");
			}
			return check_enum_conversion(r_expr, decl->type);
		}
		if (const std::optional<Type> found = called_engine_enum(r_expr)) {
			return check_enum_conversion(r_expr, *found);
		}
		if (const EnumDecl *decl = called_variant(r_expr)) {
			return check_variant_construction(r_expr, *decl);
		}
		if (!r_expr.arg_names.empty() && r_expr.base && (r_expr.text == "new" || r_expr.text == "instantiate") && !split_members(r_expr)) {
			return fail_call(r_expr);
		}
		if (!r_expr.arg_names.empty()) {
			return fail_call(r_expr, "Only structs, variants and the members that new(...) and instantiate(...) assign take arguments by name; \"" + r_expr.text + "()\" takes them by position.");
		}
		if ((!r_expr.base && r_expr.text == "super") || (r_expr.base && r_expr.base->kind == ExprKind::Name && r_expr.base->text == "super")) {
			return check_super(r_expr);
		}
		const std::string &name = r_expr.text;

		if (r_expr.base && (r_expr.base->kind != ExprKind::Self || structure)) {
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
			if ((base.kind == ExprKind::Member || base.kind == ExprKind::Name) && !visible_local(base.text)) {
				if (const std::optional<Type> named = constructed_struct(base)) {
					return named->is_error() ? fail_call(r_expr) : check_struct_call(r_expr, *named, false);
				}
			}
			const Type base_type = check_value(base);
			if (base_type.is_error()) {
				return fail_call(r_expr);
			}
			if (base.target == Target::ClassScript) {
				return check_class_call(r_expr, base.owner); // An inner class reached through an object (spec §4.1).
			}
			if (base_type.kind == TypeKind::Task && !base_type.nullable && (name == "cancel" || name == "detach")) {
				check_arguments(r_expr, {}, 0, false);
				r_expr.target = name == "cancel" ? Target::TaskCancel : Target::TaskDetach;
				refer(r_expr.name_token, language_id("Task", name));
				return set(r_expr, Type::of(TypeKind::Void));
			}
			if (base_type.kind == TypeKind::Task && !base_type.nullable && name == "settled") {
				// A task of its own, which owns a task started for it, as an await does (spec §9.2, §9.3).
				check_arguments(r_expr, {}, 0, false);
				r_expr.target = Target::TaskSettled;
				r_expr.is_async = true;
				refer(r_expr.name_token, language_id("Task", name));
				return set(r_expr, Type::task(Type::settled(base_type.args[0])));
			}
			if (base_type.is_optional()) {
				return check_option_method(r_expr, base_type, p_expected);
			}
			if (base_type.is_result()) {
				return check_result_method(r_expr, base_type);
			}
			if (base_type.kind == TypeKind::Variant) {
				return fail_call(r_expr, "Cannot call \"" + name + "()\" on a Variant." + variant_hint() + " To call a method by name, convert it to Object and use call().");
			}
			if (base_type.kind == TypeKind::Function && (name == "bind" || name == "call" || name == "callv")) {
				return name == "bind" ? check_bind(r_expr, base_type) : check_function_call(r_expr, base_type);
			}
			if (base_type.kind == TypeKind::Signal && name == "emit") {
				// What the connected functions take (spec §4.7).
				hint(r_expr, [&](int p_current) { return signature(name, type_names(base_type.args), false, p_current, "void"); });
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
					return fail(r_expr, "\"" + constant + "\" is a constant, whose containers are read-only.");
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
						// The scene's root, checked as it is made, with the members it
						// must be assigned (spec §7.4).
						const std::optional<Scene> scene = known_scene(*r_expr.base);
						check_members(r_expr, base_type.args[0], true, scene ? &*scene : nullptr);
						return set(r_expr, base_type.args[0]);
					}
					return result;
				}
				return fail(r_expr, missing(base_type.name + " has no method \"" + name + "()\".", base_type.script));
			}
			if (base_type.kind == TypeKind::Struct) {
				return check_struct_call(r_expr, base_type, true);
			}
			return fail(r_expr, "Values of type " + base_type.to_string() + " have no methods.");
		}

		if (!r_expr.base) {
			// As in GDScript (modules/gdscript/gdscript_analyzer.cpp:3741-3745).
			const LocalInfo *local = visible_local(name);
			const auto member = member_index.find(name);
			const Type *value = local ? &local->type : (member != member_index.end() ? &cls.all_members[member->second]->type : nullptr);
			if (value && value->unwrapped().kind == TypeKind::Function) {
				return fail_call(r_expr, "\"" + name + "\" is a function value; call it as " + name + ".call(...).");
			}
			if (structure && structure->find_method(name)) {
				return check_struct_call(r_expr, structure_type, false);
			}
		}
		const auto script = function_index.find(name);
		if (script != function_index.end()) {
			const FuncDecl &callee = *cls.all_functions[script->second];
			if (!callee.is_static && refuse_in_static(r_expr, name + "()")) {
				return fail_call(r_expr);
			}
			const ClassDecl &owner = declaring(cls, callee, &ClassDecl::functions);
			const Type result = call_declared(r_expr, callee, *owner.source, callee.return_type);
			r_expr.target = Target::ScriptFunction;
			r_expr.index = script->second;
			refer(r_expr.name_token, script_id(owner.path, callee.name));
			return result;
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

		// A static function of a class around this one (spec §4.1).
		if (Checker *around = r_expr.base ? nullptr : class_around(name)) {
			const auto found = around->function_index.find(name);
			if (found != around->function_index.end() && around->cls.all_functions[found->second]->is_static) {
				return check_class_call(r_expr, around->path());
			}
			if (found != around->function_index.end() || around->member_index.count(name) || engine.find_method(around->cls.native_base, name)) {
				return fail_call(r_expr, no_instance(name + "()", *around));
			}
		}

		if (!r_expr.base) {
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
			if (const builtin::Class *type = builtin::find_class(name)) {
				return check_constructor(r_expr, *type);
			}
			if (name == "preload") {
				return check_preload(r_expr);
			}
			if (name == "range") {
				return check_range(r_expr);
			}
			if (name == "len" || name == "char" || name == "ord" || name == "print_stack" || name == "get_stack") {
				return check_gdscript_utility(r_expr);
			}
			if (name == "is_instance_of") {
				return check_is_instance_of(r_expr);
			}
			// What Godot's documentation says to use instead
			// (modules/gdscript/doc_classes/@GDScript.xml:14, 61, 77, 106, 269; ed1daf0).
			static const std::pair<const char *, const char *> deprecated[] = {
				{ "Color8", "Color.from_rgba8()" },
				{ "convert", "type_convert()" },
				{ "dict_to_inst", "JSON.to_native() or Object.get_property_list()" },
				{ "inst_to_dict", "JSON.from_native() or Object.get_property_list()" },
				{ "type_exists", "ClassDB.class_exists()" },
			};
			for (const auto &[old, instead] : deprecated) {
				if (name == old) {
					return fail_call(r_expr, name + "() is deprecated in GDScript and left out of this language; use " + instead + " instead.");
				}
			}
			if (name == "load" && engine.is_singleton("ResourceLoader")) {
				// GDScript's load() is ResourceLoader.load(), whose result may be
				// None (spec §7.4, §10.2), but of a literal path as preload().
				on_resource_loader(r_expr);
				const Type result = check_call(r_expr);
				const std::optional<Type> loaded = result.is_error() ? std::nullopt : literal_load(r_expr);
				return loaded ? set(r_expr, *loaded) : result;
			}
		}

		return fail_call(r_expr, "Function \"" + name + "()\" not found in the class or its base " + cls.extends + ".");
	}

	// GDScript's range(to), range(from, to) and range(from, to, step) of ints,
	// without converting floats as GDScript does (spec §3.14).
	Type check_range(Expr &r_expr) {
		refer(r_expr.name_token, engine_id("@GDScript", "range"));
		check_arguments(r_expr, std::vector<Type>(3, Type::of(TypeKind::Int)), 1, false);
		const ConstPtr &step = r_expr.args.size() == 3 ? r_expr.args[2]->constant : nullptr;
		if (step && step->kind == ConstValue::Kind::Int && step->i == 0) {
			error(r_expr.args[2]->range, "The step of range() is 0.");
		}
		r_expr.target = Target::Range;
		return set(r_expr, Type::array(Type::of(TypeKind::Int)));
	}

	// GDScript's own len(), char(), ord(), print_stack() and get_stack()
	// (modules/gdscript/gdscript_utility_functions.cpp:120-134, 364-452; ed1daf0).
	Type check_gdscript_utility(Expr &r_expr) {
		const std::string &name = r_expr.text;
		refer(r_expr.name_token, engine_id("@GDScript", name));
		if (name == "char") {
			// String.chr() of a code that fits a char32, as GDScript checks it.
			auto type = std::make_unique<Expr>();
			type->kind = ExprKind::Name;
			type->text = "String";
			type->range = r_expr.range;
			type->name_token = NO_TOKEN;
			r_expr.base = std::move(type);
			r_expr.text = "chr";
			r_expr.name_token = NO_TOKEN;
			const Type result = check_call(r_expr);
			r_expr.arg_ranges.assign(1, bytecode::IntRange::CHAR32);
			return result;
		}
		r_expr.target = Target::GDScriptUtility;
		if (name == "len") {
			if (r_expr.args.size() != 1) {
				return fail_call(r_expr, "Function \"len()\" takes 1 argument(s), " + std::to_string(r_expr.args.size()) + " given.");
			}
			const Type value = check_value(*r_expr.args[0]);
			const bool text = value.kind == TypeKind::String || value == Type::builtin("StringName");
			if (!value.is_error() && !text && !element_of(value) && !entries_of(value)) {
				return fail(r_expr, "len() takes a String, a StringName, an Array, a Dictionary or a packed array, not " + value.to_string() + ".");
			}
			return set(r_expr, Type::of(TypeKind::Int));
		}
		if (name == "ord") {
			check_arguments(r_expr, { Type::of(TypeKind::String) }, 1, false);
			return set(r_expr, Type::of(TypeKind::Int));
		}
		check_arguments(r_expr, {}, 0, false);
		return set(r_expr, name == "get_stack" ? Type::array(Type::builtin("Dictionary")) : Type::of(TypeKind::Void));
	}

	// is_instance_of(v, T) of a class name or a TYPE_* constant, as `v is T`;
	// of a Script value, by the rules of `is` without narrowing (spec §2.6).
	Type check_is_instance_of(Expr &r_expr) {
		refer(r_expr.name_token, engine_id("@GDScript", "is_instance_of"));
		if (r_expr.args.size() != 2) {
			return fail_call(r_expr, "Function \"is_instance_of()\" takes 2 argument(s), " + std::to_string(r_expr.args.size()) + " given.");
		}
		const Expr &type = *r_expr.args[1];
		if (!written_type(type)) {
			return check_script_test(r_expr);
		}
		TypeRef tested;
		tested.range = type.range;
		tested.name_token = first_name_token(type);
		tested.name = dotted_name(type);
		const builtin::Enum *of = type.kind == ExprKind::Name ? builtin::global_constant_enum(type.text) : nullptr;
		const int64_t variant_type = of && std::string(of->name) == "Variant.Type" ? *builtin::global_constant(type.text) : 0;
		// TYPE_NIL and TYPE_MAX name no type.
		if (variant_type > 0 && variant_type < int64_t(bytecode::ValueType::Variant)) {
			tested.name = bytecode::value_type_name(bytecode::ValueType(variant_type));
			tested.name_token = NO_TOKEN;
			refer(type.name_token, engine_id("@GlobalScope", type.text));
		}
		r_expr.kind = ExprKind::TypeTest;
		r_expr.left = std::move(r_expr.args[0]);
		r_expr.cast_type = tested;
		r_expr.args.clear();
		return check_type_test(r_expr);
	}

	// Whether T of is_instance_of() is written as a type: names, as A.B, the
	// first of which no variable or autoload hides.
	bool written_type(const Expr &p_expr) const {
		if (p_expr.kind == ExprKind::Member) {
			return p_expr.base && written_type(*p_expr.base);
		}
		return p_expr.kind == ExprKind::Name && !names_variable(p_expr.text) && !(options.project && !options.project->autoload_path(p_expr.text).empty());
	}

	Type check_script_test(Expr &r_expr) {
		const Type value = check_value(*r_expr.args[0]);
		const Type script = check_value(*r_expr.args[1]);
		if (value.is_error() || script.is_error()) {
			return set(r_expr, Type::of(TypeKind::Error));
		}
		const Type wanted = Type::object("Script");
		const bool of_script = script.kind == TypeKind::Object && is_a(script, wanted);
		if (of_script && script.nullable) {
			error(r_expr.args[1]->range, "The value may be None (" + script.to_string() + "); unwrap it first.");
		} else if (!of_script) {
			const bool may_be = script.kind == TypeKind::Variant || (script.kind == TypeKind::Object && related(script, wanted));
			error(r_expr.args[1]->range, "is_instance_of() takes a class name, a TYPE_* constant or a Script, not " + script.to_string() + (may_be ? "; test it with is Script first." : "."));
		} else if (value.kind != TypeKind::Object && value.kind != TypeKind::Variant) {
			error(r_expr.args[0]->range, "is_instance_of() tests objects against a Script, not " + value.to_string() + ".");
		} else {
			r_expr.target = Target::GDScriptUtility;
			return set(r_expr, Type::of(TypeKind::Bool));
		}
		return set(r_expr, Type::of(TypeKind::Error));
	}

	// Why await cannot be written here; empty where it can.
	std::string no_await() const {
		if (in_assert) {
			return "await cannot appear in an assert: it would vanish with the assertion in release builds.";
		}
		if (in_predelete) {
			return "await is not allowed in the PREDELETE branch.";
		}
		if (!function || !function->is_async || initializing >= 0) {
			return "await requires an async function and cannot appear in an initializer.";
		}
		return std::string();
	}

	// `Task.of(signal)`, `Task.delay(seconds)`, `Task.any([...])` and
	// `Task.all([...])` (spec §9.5): tasks the runtime makes, which an await
	// owns when it starts them, as a call. A signal gives what an await of it
	// gives as a value (§9.6).
	Type check_task_function(Expr &r_expr) {
		refer(r_expr.base->name_token, language_id("Task", std::string()));
		const std::string &name = r_expr.text;
		if (name != "of" && name != "delay" && name != "any" && name != "all") {
			return fail_call(r_expr, "Task has no function \"" + name + "()\".");
		}
		refer(r_expr.name_token, language_id("Task", name));
		r_expr.is_async = true;
		if (name == "any" || name == "all") {
			return check_task_join(r_expr);
		}
		if (name == "delay") {
			check_arguments(r_expr, { Type::of(TypeKind::Float) }, 1, false);
			r_expr.target = Target::TaskDelay;
			return set(r_expr, Type::task(Type::of(TypeKind::Void)));
		}
		if (r_expr.args.size() != 1) {
			return fail_call(r_expr, "Task.of() takes one signal.");
		}
		const Type signal = check_value(*r_expr.args[0]);
		std::optional<Type> result;
		if (signal.kind == TypeKind::Signal) {
			result = signal.args.size() == 1 ? signal.args[0] : Type::of(TypeKind::Void);
		} else if (signal.builtin_name() == "Signal") {
			result = Type::of(TypeKind::Variant);
		} else if (!signal.is_error()) {
			error(r_expr.args[0]->range, "Task.of() takes a signal, not " + signal.to_string() + ".");
		}
		if (!result) {
			return set(r_expr, Type::of(TypeKind::Error));
		}
		r_expr.param_types.push_back(signal);
		r_expr.target = Target::TaskOf;
		return set(r_expr, Type::task(*result));
	}

	// The tasks of Task.any() or Task.all(), listed in brackets, where a task
	// started is the join's own, or in an Array[Task[T]] (spec §9.5). any gives
	// the index of the first to finish; all gives nothing, as tuples could give
	// the results (BACKLOG).
	Type check_task_join(Expr &r_expr) {
		const bool any = r_expr.text == "any";
		if (r_expr.args.size() != 1) {
			return fail_call(r_expr, "Task." + r_expr.text + "() takes one list of tasks, as Task." + r_expr.text + "([a, f()]).");
		}
		Expr &list = *r_expr.args[0];
		if (list.kind == ExprKind::ArrayLiteral) {
			if (any && list.args.empty()) {
				error(list.range, "Task.any() needs a task to wait for.");
			}
			for (auto &member : list.args) {
				const Type type = check_value(*member);
				if (!type.is_error() && (type.kind != TypeKind::Task || type.nullable)) {
					error(member->range, "Task." + r_expr.text + "() takes tasks, not " + type.to_string() + ".");
				}
			}
			set(list, Type::builtin("Array"));
		} else {
			const Type tasks = check_value(list);
			if (!tasks.is_error() && (!is_typed_container(tasks) || tasks.name != "Array" || tasks.args[0].kind != TypeKind::Task || tasks.args[0].nullable)) {
				error(list.range, "Task." + r_expr.text + "() takes tasks in brackets or an Array[Task[T]], not " + tasks.to_string() + ".");
			}
		}
		r_expr.target = any ? Target::TaskAny : Target::TaskAll;
		return set(r_expr, Type::task(Type::of(any ? TypeKind::Int : TypeKind::Void)));
	}

	Type check_await(Expr &r_expr) {
		const std::string refused = no_await();
		if (!refused.empty()) {
			return fail(r_expr, refused);
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

	// Whether a type a lambda returns leaves out what its T? or Result holds,
	// as None does, or Ok(v) going to and_then() (spec §4.6).
	static bool partial(const Type &p_type) {
		return p_type.kind == TypeKind::None || (p_type.kind == TypeKind::Option && p_type.args[0].is_error()) || (p_type.is_result() && (p_type.args[0].is_error() || p_type.args[1].is_error()));
	}

	// A partial type that a lambda returns, completed by another it returns.
	static Type completed(const Type &p_partial, const Type &p_other) {
		if (p_partial.is_result()) {
			return p_other.is_result() ? Type::result(p_partial.args[0].is_error() ? p_other.args[0] : p_partial.args[0], p_partial.args[1].is_error() ? p_other.args[1] : p_partial.args[1]) : p_partial;
		}
		if (p_other.kind == TypeKind::None || (p_other.kind == TypeKind::Option && p_other.args[0].is_error())) {
			return p_partial;
		}
		return p_other.is_optional() ? p_other : optional_of(p_other);
	}

	// A type that a lambda without a return type returns, whose return type is
	// the type all of them fit (spec §4.6).
	void add_returned(Type p_value, const TokenRange &p_at) {
		if (!returns->type || p_value.is_error() || returns->type->is_error()) {
			returns->type = returns->type ? Type::of(TypeKind::Error) : p_value;
			return;
		}
		Type known = *returns->type;
		if (partial(known)) {
			known = completed(known, p_value);
		}
		if (partial(p_value)) {
			p_value = completed(p_value, known);
		}
		const Type common = common_type(known, p_value);
		if (common.is_error()) {
			error(p_at, function_label() + " returns " + returned_text(*returns->type) + " and " + returned_text(p_value) + "; declare its return type.");
		}
		returns->type = common;
	}

	// A type a lambda returns, as a diagnostic shows it, even if partial.
	static std::string returned_text(const Type &p_type) {
		if (p_type.kind == TypeKind::None || p_type.kind == TypeKind::Option) {
			return p_type.kind == TypeKind::None || p_type.args[0].is_error() ? std::string("None") : p_type.to_string();
		}
		return p_type.is_result() && partial(p_type) ? (p_type.args[0].is_error() ? "Err(" + p_type.args[1].to_string() + ")" : "Ok(" + p_type.args[0].to_string() + ")") : p_type.to_string();
	}

	// `x?` (spec §8.2): the value inside x, or the function returns the Err or
	// the None x holds. So it returns a Result with the same error type, or a
	// T?; between the two, ok() and ok_or() convert.
	Type check_try(Expr &r_expr) {
		const Type type = check_value(*r_expr.left);
		if (type.is_error()) {
			return set(r_expr, type);
		}
		const bool result = type.is_result();
		if (!result && !type.is_optional()) {
			return fail(r_expr, "? takes apart a Result or a T?, not " + type.to_string() + ".");
		}
		const std::string found = result ? "the Err(...) of " + type.to_string() : "the None of " + type.to_string();
		if (in_assert) {
			return fail(r_expr, "? cannot appear in an assert: it would vanish with the assertion in release builds.");
		}
		if (!function || initializing >= 0 || static_initializing >= 0) {
			return fail(r_expr, "? returns " + found + " from the function, and cannot appear in an initializer.");
		}
		if (returns && returns->expected.is_error()) {
			return fail(r_expr, "? returns " + found + " from the lambda, whose return type is then needed; write it, as in `func(x) -> " + (result ? "Result[int, " + type.args[1].to_string() + "]" : std::string("int?")) + ":`.");
		}
		// A lambda returns what the function type it goes to does.
		const Type to = returns ? returns->expected : function->return_type;
		if (to.is_error()) {
			return set(r_expr, Type::of(TypeKind::Error));
		}
		if (result && to.is_result() && to.args[1] != type.args[1]) {
			return fail(r_expr, "? returns " + found + ", but " + function_label() + " returns " + to.to_string() + "; convert the error with map_err().");
		}
		if (result ? !to.is_result() : !to.is_optional()) {
			const std::string convert = to.is_optional() ? "; convert it with ok()" : (to.is_result() ? "; convert it with ok_or()" : "");
			return fail(r_expr, "? returns " + found + ", but " + function_label() + " returns " + to.to_string() + convert + ".");
		}
		if (returns) {
			add_returned(to, r_expr.range);
		}
		return set(r_expr, result ? type.args[0] : type.unwrapped());
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
				if (part.target == Target::Local || part.target == Target::Member || part.target == Target::StaticVar || part.target == Target::SelfProperty || part.target == Target::ObjectProperty) {
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
		error(part.range, what + ".");
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
		if (completion && (r_expr.kind == ExprKind::Name || r_expr.kind == ExprKind::Member || r_expr.kind == ExprKind::Call || r_expr.kind == ExprKind::GetNode) && at_cursor(r_expr.text)) {
			r_expr.kind == ExprKind::GetNode ? complete_node_path(r_expr) : complete(r_expr, p_expected);
			return set(r_expr, Type::of(TypeKind::Error));
		}
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
				return check_call(r_expr, p_expected);
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
				if ((p_expected.is_optional() && !partial(p_expected)) || p_expected.kind == TypeKind::Variant) {
					return set(r_expr, p_expected);
				}
				return set(r_expr, Type::of(TypeKind::None));
			case ExprKind::TypeTest:
				return check_type_test(r_expr);
			case ExprKind::Lambda:
				return check_lambda(r_expr, p_expected);
			case ExprKind::Try:
				return check_try(r_expr);
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
		if (type.kind == TypeKind::Unsupported) {
			error(r_expr.range, "Values of type " + type.to_string() + " are not supported yet.");
			return Type::of(TypeKind::Error);
		}
		return type;
	}

	// What a Variant needs before it is used as a value of a type (spec §3.14).
	static std::string variant_hint(const Type &p_to = Type::of(TypeKind::Error)) {
		if (p_to.kind == TypeKind::Object) {
			return " Convert it first with v as " + p_to.name + ", or narrow it with `if v is " + p_to.name + ":`.";
		}
		if (is_typed_container(p_to)) {
			return " Narrow it first with `if v is " + p_to.to_string() + ":`.";
		}
		const std::string name = p_to.kind == TypeKind::Builtin || p_to.is_primitive() ? p_to.to_string() : "T";
		return " Convert it first with " + name + "(v), or narrow it with `if v is " + name + ":`.";
	}

	// Whether the expression is a place that can be written: a local, a
	// member, a property, or a member of a built-in value in such a place,
	// which is then written back (architecture §2.1).
	bool check_place(const Expr &p_target) {
		const std::string constant = constant_root(p_target);
		if (!constant.empty()) {
			const bool whole = p_target.kind != ExprKind::Subscript && p_target.target != Target::Field && p_target.target != Target::StructField;
			error(p_target.range, whole ? "Cannot assign to the constant \"" + constant + "\"." : "\"" + constant + "\" is a constant, whose containers are read-only.");
			return false;
		}
		if (p_target.kind == ExprKind::Subscript) {
			if (p_target.base->type.kind == TypeKind::String) {
				error(p_target.range, "A String's characters cannot be assigned. Remove one with s = s.erase(i), or replace it with s = s.erase(i).insert(i, c).");
				return false;
			}
			// An Array or a Dictionary changes in place; a packed array is a value,
			// written back along the chain.
			return is_shared(p_target.base->type) || check_written_back(p_target);
		}
		switch (p_target.target) {
			case Target::Local:
				if (p_target.kind == ExprKind::Self) {
					error(p_target.range, "self is read-only in a struct's methods; return a changed copy instead.");
					return false;
				}
				if (is_capture(p_target.index)) {
					// It would change neither the variable outside nor the next call (spec §3.11).
					error(p_target.range, "Cannot assign to \"" + p_target.text + "\", which the lambda captures as a copy; keep what it shares in a member or a container.");
					return false;
				}
				return true;
			case Target::Member:
			case Target::StaticVar:
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
		if (base == ExprKind::Name || base == ExprKind::Member || base == ExprKind::Subscript || base == ExprKind::Self) {
			return check_place(*p_target.base);
		}
		error(p_target.range, p_target.kind == ExprKind::Subscript ? "Cannot assign to an element of a temporary value." : "Cannot assign to a member of a temporary value.");
		return false;
	}

	// A scene statically known that a variable or a parameter of a
	// PackedScene[T] takes (spec §7.4), whose instantiate(...) then asks only
	// for the members of T that must be assigned and are not exported. The
	// scene may leave no other member to it.
	void expect_scene_fits(const Expr &p_value, const Type &p_to) {
		const std::optional<Scene> scene = is_typed_scene(p_to) ? known_scene(p_value) : std::nullopt;
		const std::string unfit = scene ? unfit_scene(*scene, p_to) : std::string();
		if (!unfit.empty()) {
			error(p_value.range, scene->path + " " + unfit);
		}
	}

	static bool is_typed_scene(const Type &p_type) {
		return p_type.kind == TypeKind::Object && p_type.name == "PackedScene" && !p_type.args.empty();
	}

	// What a scene leaves to instantiate(...) that a PackedScene[T] does not
	// ask for, as the error says it after the scene; "" for nothing.
	std::string unfit_scene(const Scene &p_scene, const Type &p_to) {
		const SceneNode *root = p_scene.find_node(".");
		const std::string script = !root ? std::string() : root->script.empty() ? root->instance_script
																				: root->script;
		const ClassDecl *made = is_script_path(script) ? script_decl(script) : nullptr;
		const Type &taken = p_to.args[0];
		const ClassDecl *asks = taken.kind == TypeKind::Object && !taken.is_interface && !taken.script.empty() ? script_decl(taken.script) : nullptr;
		for (const VarDecl *member : made ? made->all_members : std::vector<const VarDecl *>()) {
			const bool asked = !member->is_export && asks && std::any_of(asks->all_members.begin(), asks->all_members.end(), [&](const VarDecl *p_member) { return p_member->name == member->name; });
			if (member->must_assign && !asked && (!member->is_export || !stored_value(p_scene, ".", member->name).value_or(false))) {
				return "leaves \"" + member->name + "\" to instantiate(), but a " + p_to.to_string() + " does not ask for it.";
			}
		}
		return std::string();
	}

	// Why the engine would lose a None of a value of the type (spec §10.1), as
	// a sentence whose subject is `p_who`; empty when it would not. With
	// `p_fix`, a T?? or Variant? itself is given the call that makes it fit.
	// `p_what` names the value, as "its result", when it is not the subject's.
	std::string encoding_loss(const Type &p_type, const std::string &p_who, bool p_fix, bool p_copied = false, const std::string &p_what = std::string()) const {
		const std::optional<Type> lost = lost_in_encoding(p_type, *this, p_copied);
		if (!lost) {
			return std::string();
		}
		const bool variant = lost->args[0].kind == TypeKind::Variant;
		const bool whole = *lost == p_type;
		const std::string none = whole ? (p_what.empty() ? std::string("its None") : "the None of " + p_what) : "the None of the " + lost->to_string() + " inside" + (p_what.empty() ? std::string() : " " + p_what);
		std::string text = p_who + " cannot tell " + none + " from " + (variant ? "null" : "Some(None)");
		if (whole && p_fix) {
			text += variant ? "; pass x.unwrap_or(null)" : "; pass x.flatten()";
		}
		return text + ".";
	}

	// Why a value of the type does not go to a Variant or an untyped container
	// as it is (spec §3.14), after the error that says so.
	std::string variant_refusal(const Type &p_type) const {
		const std::string loss = encoding_loss(p_type, "A Variant", true);
		if (!loss.empty()) {
			return " " + loss;
		}
		if (is_typed_container(p_type) && is_boxed(p_type)) {
			return " Its elements exist only in this language; convert it explicitly with " + p_type.name + "(x), a copy with the elements encoded.";
		}
		return std::string();
	}

	// Why a function value does not go to a Callable or a Variant, whose
	// calls encode as those of a pub function do (spec §10.1).
	std::string callable_refusal(const Type &p_function) const {
		for (size_t i = 1; i <= p_function.args.size(); i++) {
			const size_t part = i % p_function.args.size(); // The result last.
			const std::string loss = encoding_loss(p_function.args[part], "The engine", false, false, part == 0 ? std::string("its result") : "its parameter " + std::to_string(part));
			if (!loss.empty()) {
				return " " + loss;
			}
		}
		return std::string();
	}

	void expect_assignable(const Expr &p_value, const Type &p_from, const Type &p_to, const char *p_action) {
		if (assignable(p_from, p_to)) {
			expect_implemented(p_value, p_from, p_to);
			if (std::strcmp(p_action, "assign") == 0 || std::strcmp(p_action, "pass") == 0) {
				expect_scene_fits(p_value, p_to);
			}
			return;
		}
		std::string message = std::string("Cannot ") + p_action + " a value of type " + p_from.to_string() + " as " + p_to.to_string() + ".";
		if (p_from.kind == TypeKind::Float && p_to.kind == TypeKind::Int) {
			message += " Convert explicitly with int(x).";
		}
		if (p_to.unwrapped().kind == TypeKind::Enum && (p_from.kind == TypeKind::Int || p_from.kind == TypeKind::Enum)) {
			const std::string name = enum_spelling(p_to.unwrapped());
			message += " Convert explicitly with " + name + "(x), which checks that it is one of the values.";
		}
		const bool untyped_view = p_to.kind == TypeKind::Builtin && p_to.args.empty() && p_to.name == p_from.name;
		if (p_to.kind == TypeKind::Variant || untyped_view) {
			message += variant_refusal(p_from);
		}
		if (p_from.kind == TypeKind::Function && (p_to.kind == TypeKind::Variant || p_to == Type::builtin("Callable"))) {
			message += callable_refusal(p_from);
		}
		if (p_from.kind == TypeKind::Variant) {
			message += variant_hint(p_to.unwrapped());
		}
		if (is_typed_container(p_to) && p_from.kind == TypeKind::Builtin && p_from.args.empty() && p_from.name == p_to.name) {
			message += " Its elements may be of any type; narrow it with `if x is " + p_to.to_string() + ":`, or copy them in with assign().";
		}
		error(p_value.range, message);
	}

	void expect_condition(Expr &r_condition) {
		const Type type = check_value(r_condition);
		if (type.is_error() || type.kind == TypeKind::Bool) {
			return;
		}
		error(r_condition.range, "A condition must be bool, found " + type.to_string() + "." + compare_hint(type, false));
	}

	/* Statements */

	// Returns what is known at the end of the block, starting from `p_facts`.
	std::unordered_map<int, Type> check_block(Block &r_block, const std::unordered_map<int, Type> &p_facts = {}) {
		scopes.emplace_back();
		narrowed.push_back(p_facts);
		const Stmt *previous = nullptr;
		for (auto &stmt : r_block) {
			previous_statement = previous;
			check_stmt(*stmt);
			previous = stmt.get();
		}
		std::unordered_map<int, Type> facts = std::move(narrowed.back());
		narrowed.pop_back();
		scopes.pop_back();
		return facts;
	}

	int declare_local(const std::string &p_name, uint32_t p_at, const Type &p_type, bool p_constant = false) {
		const int known = find_local(p_name);
		if (scopes.back().count(p_name)) {
			error(p_at, "\"" + p_name + "\" is already declared in this block.");
		} else if (known >= 0 ? is_capture(known) : visible_local(p_name) != nullptr) {
			error(p_at, "\"" + p_name + "\" is already declared in the function around this lambda.");
		} else if (scopes.front().count(p_name)) {
			error(p_at, "\"" + p_name + "\" is already a parameter of this function.");
		} else if (known >= 0) {
			error(p_at, "\"" + p_name + "\" is already declared in an enclosing block.");
		}
		const int local = int(function->locals.size());
		function->locals.push_back({ p_name, p_type, p_at });
		refer_local(p_at, p_type, p_constant);
		scopes.back()[p_name] = local;
		return local;
	}

	void declare_local(Stmt &r_stmt, const Type &p_type) {
		r_stmt.local = declare_local(r_stmt.name, r_stmt.name_token, p_type, r_stmt.kind == StmtKind::Const);
	}

	// `match` (spec §5.6). The matched value goes in a local without a name,
	// which the debugger shows as a temporary.
	void check_match(Stmt &r_stmt) {
		const Type matched = check_value(*r_stmt.value);
		scopes.emplace_back();
		r_stmt.local = int(function->locals.size());
		function->locals.push_back({ std::string(), matched });
		scopes.back()[std::string()] = r_stmt.local;
		const bool was_predelete = in_predelete;
		// After a branch for None, `var x` of a T? binds the T inside.
		bool none_handled = false;
		for (MatchBranch &branch : r_stmt.branches) {
			scopes.emplace_back();
			for (Pattern &pattern : branch.patterns) {
				if (branch.patterns.size() > 1 && binds(pattern)) {
					error(pattern.range, "Patterns separated by commas cannot bind.");
				}
				check_pattern(pattern, pattern.kind == PatternKind::Bind && none_handled ? matched.unwrapped() : matched, r_stmt.local);
				none_handled = none_handled || (!branch.guard && pattern.kind == PatternKind::Constructor && matched.is_optional() && pattern.variant == 0);
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
		check_exhaustive(r_stmt, matched);
	}

	static bool binds(const Pattern &p_pattern) {
		return p_pattern.kind == PatternKind::Bind || std::any_of(p_pattern.items.begin(), p_pattern.items.end(), [](const Pattern &p_item) { return binds(p_item); });
	}

	// A pattern of the value in the local `p_local`, which a value pattern
	// becomes the comparison of with the pattern, checked as == is.
	void check_pattern(Pattern &r_pattern, const Type &p_matched, int p_local) {
		switch (r_pattern.kind) {
			case PatternKind::Wildcard:
				return;
			case PatternKind::Bind:
				r_pattern.local = declare_local(r_pattern.name, r_pattern.name_token, p_matched);
				return;
			case PatternKind::Constructor:
				check_constructor_pattern(r_pattern, p_matched, p_local);
				return;
			case PatternKind::Array:
			case PatternKind::Dictionary:
				check_container_pattern(r_pattern, p_matched);
				return;
			case PatternKind::Value: {
				if (r_pattern.value->kind == ExprKind::NoneLiteral && p_matched.is_optional()) {
					r_pattern.kind = PatternKind::Constructor;
					r_pattern.type = p_matched;
					return;
				}
				// Finished of a Task[void], which has no value (spec §9.2).
				if (r_pattern.value->kind == ExprKind::Name && r_pattern.value->text == "Finished" && p_matched.is_settled() && p_matched.values->fields[0].empty() && !declares_in_script("Finished")) {
					refer(r_pattern.value->name_token, language_id("Settled", "Finished"));
					r_pattern.kind = PatternKind::Constructor;
					r_pattern.type = p_matched;
					return;
				}
				r_pattern.local = p_local;
				auto compare = std::make_unique<Expr>();
				compare->kind = ExprKind::Binary;
				compare->op = TokenKind::EqualEqual;
				compare->range = r_pattern.value->range;
				compare->left = std::make_unique<Expr>();
				compare->left->kind = ExprKind::Name;
				compare->left->range = r_pattern.value->range;
				compare->left->name_token = NO_TOKEN;
				compare->right = std::move(r_pattern.value);
				r_pattern.value = std::move(compare);
				// The nameless local is the one that holds what the pattern matches.
				const auto outer = scopes.back().find(std::string());
				const int was = outer == scopes.back().end() ? -1 : outer->second;
				scopes.back()[std::string()] = p_local;
				check_expr(*r_pattern.value);
				if (r_pattern.value->target == Target::VariantEqual && r_pattern.value->left->type.kind == TypeKind::Variant) {
					r_pattern.value->target = Target::VariantMatch;
				}
				if (was < 0) {
					scopes.back().erase(std::string());
				} else {
					scopes.back()[std::string()] = was;
				}
				const Expr &value = *r_pattern.value->right;
				const Expr *named = &value;
				while (named->kind == ExprKind::Member) {
					named = named->base.get();
				}
				if (!value.type.is_error() && !value.constant && named->kind != ExprKind::Name) {
					error(value.range, "A pattern is a literal, a constant expression, an identifier or an attribute access.");
				} else if (!r_pattern.value->type.is_error() && !pattern_fits(value.type, p_matched)) {
					error(value.range, "The pattern is " + value.type.to_string() + ", but the value matched is " + p_matched.to_string() + "; a value pattern must have its type.");
				}
				return;
			}
		}
	}

	// `Command.MOVE(target = var t, ..)`, `Hit(var damage)` or `Some(var x)`
	// (spec §5.6); otherwise a call, as `Vector2(1, 2)`, which is a value.
	void check_constructor_pattern(Pattern &r_pattern, const Type &p_matched, int p_local) {
		Expr &callee = *r_pattern.value;
		const auto fits = [&](const Type &p_type) {
			if (!p_matched.is_error() && p_type != p_matched) {
				error(r_pattern.range, "The pattern is " + p_type.to_string() + ", but the value matched is " + p_matched.to_string() + ".");
				return false;
			}
			return true;
		};
		if (callee.kind == ExprKind::Name && (callee.text == "Finished" || callee.text == "Cancelled") && !declares_in_script(callee.text)) {
			const int variant = callee.text == "Finished" ? 0 : 1;
			if (!p_matched.is_error() && !p_matched.is_settled()) {
				error(r_pattern.range, callee.text + "(...) matches a value of a type Settled[T], not " + p_matched.to_string() + ".");
			} else if (!p_matched.is_error() && p_matched.values->fields[variant].empty()) {
				error(r_pattern.range, "Finished of " + p_matched.to_string() + " has no value; match it without parentheses.");
			} else if (!p_matched.is_error()) {
				refer(callee.name_token, language_id("Settled", callee.text));
				r_pattern.type = p_matched;
				r_pattern.variant = variant;
				check_items(r_pattern, {}, { p_matched.values->fields[variant][0].second }, std::string(), std::string());
				return;
			}
			check_items(r_pattern, {}, {}, std::string(), std::string());
			return;
		}
		if (callee.kind == ExprKind::Name && (callee.text == "Some" || callee.text == "Ok" || callee.text == "Err") && !declares_in_script(callee.text)) {
			const bool some = callee.text == "Some";
			if (!p_matched.is_error() && (some ? !p_matched.is_optional() : !p_matched.is_result())) {
				error(r_pattern.range, callee.text + "(...) matches a value of a type " + (some ? "T?" : "Result[T, E]") + ", not " + p_matched.to_string() + ".");
			} else if (!p_matched.is_error()) {
				r_pattern.type = p_matched;
				r_pattern.variant = callee.text == "Ok" ? 0 : 1;
				if (!some) {
					refer(callee.name_token, language_id("Result", callee.text));
				}
				check_items(r_pattern, {}, { some ? p_matched.unwrapped() : p_matched.args[r_pattern.variant] }, std::string(), std::string());
				return;
			}
			check_items(r_pattern, {}, {}, std::string(), std::string());
			return;
		}
		if (callee.kind == ExprKind::Member) {
			if (const EnumDecl *decl = payload_enum_named(*callee.base)) {
				const int index = decl->find(callee.text);
				const std::string written = enum_spelling(decl->type) + "." + callee.text;
				if (index < 0) {
					error(callee.range, missing(decl->name + " has no variant \"" + callee.text + "\".", decl->script));
				} else if (decl->values[index].fields.empty()) {
					error(r_pattern.range, written + " has no payload; match it without parentheses.");
				} else if (fits(decl->type)) {
					const EnumValueDecl &variant = decl->values[index];
					refer(callee.name_token, script_id(decl->script, enum_value_id(*decl, variant.name)));
					r_pattern.type = decl->type;
					r_pattern.variant = index;
					std::vector<std::string> names;
					std::vector<Type> types;
					for (const VarDecl &field : variant.fields) {
						names.push_back(field.name);
						types.push_back(field.type);
					}
					check_items(r_pattern, names, types, written, enum_value_id(*decl, variant.name) + ".");
					return;
				}
				check_items(r_pattern, {}, {}, std::string(), std::string());
				return;
			}
		}
		Expr probe;
		probe.text = callee.text;
		probe.name_token = callee.name_token;
		probe.range = callee.range;
		probe.base = std::move(callee.base);
		const std::optional<Type> constructed = constructed_struct(probe);
		callee.base = std::move(probe.base);
		if (constructed) {
			const StructDecl *decl = constructed->is_error() ? nullptr : struct_decl(*constructed);
			if (decl && fits(*constructed)) {
				r_pattern.type = *constructed;
				std::vector<std::string> names;
				std::vector<Type> types;
				for (const VarDecl &field : decl->fields) {
					names.push_back(field.name);
					types.push_back(field.type);
				}
				check_items(r_pattern, names, types, constructed->interface_name(), decl->name + ".");
				return;
			}
			check_items(r_pattern, {}, {}, std::string(), std::string());
			return;
		}
		if (std::unique_ptr<Expr> call = pattern_call(r_pattern)) {
			r_pattern.kind = PatternKind::Value;
			r_pattern.value = std::move(call);
			check_pattern(r_pattern, p_matched, p_local);
			return;
		}
		error(callee.range, "\"" + callee.text + "\" names no variant, struct or Some(...) whose fields a pattern could match.");
		check_items(r_pattern, {}, {}, std::string(), std::string());
	}

	// `[1, var x, ..]` of an Array[T], or `{"name": var n, ..}` of a
	// Dictionary[K, V], whose items match values of T or V, and whose keys are
	// constants that `in` would take; of a Variant, one that holds a container
	// of the kind, of any type, its items matching Variants (spec §5.6).
	void check_container_pattern(Pattern &r_pattern, const Type &p_matched) {
		const bool array = r_pattern.kind == PatternKind::Array;
		const std::string kind = array ? "Array" : "Dictionary";
		std::optional<std::pair<Type, Type>> entries;
		r_pattern.type = p_matched;
		if (p_matched.kind == TypeKind::Variant) {
			r_pattern.type = Type::builtin(kind);
			entries = std::make_pair(p_matched, p_matched);
		} else if (p_matched.kind == TypeKind::Builtin && p_matched.name == kind) {
			entries = array ? std::make_pair(Type::of(TypeKind::Int), *element_of(p_matched)) : *entries_of(p_matched);
		} else if (!p_matched.is_error()) {
			error(r_pattern.range, std::string(array ? "An array pattern matches an Array" : "A dictionary pattern matches a Dictionary") + " or a Variant, not " + p_matched.to_string() + ".");
		}
		const Type key = entries ? entries->first : Type::of(TypeKind::Error);
		const Type element = entries ? entries->second : Type::of(TypeKind::Error);
		std::vector<ConstPtr> keys;
		for (Pattern &item : r_pattern.items) {
			if (item.key) {
				const Type type = check_value(*item.key, key);
				if (type.is_error() || key.is_error()) {
				} else if (!item.key->constant) {
					error(item.key->range, "A key of a dictionary pattern is a constant expression.");
				} else if (!assignable(type, key)) {
					error(item.key->range, "The keys of " + p_matched.to_string() + " are " + key.to_string() + ", not " + type.to_string() + ".");
				} else if (const ConstPtr value = converted(item.key->constant, key); value && value->is_known()) {
					const auto same = [&](const ConstPtr &p_other) {
						return p_other->kind == value->kind && p_other->type == value->type && p_other->i == value->i && p_other->f == value->f && p_other->b == value->b && p_other->text == value->text;
					};
					if (std::any_of(keys.begin(), keys.end(), same)) {
						error(item.key->range, "This key is matched twice.");
					}
					keys.push_back(value);
				}
			}
			int local = -1;
			if (item.kind == PatternKind::Value || item.kind == PatternKind::Constructor) {
				local = int(function->locals.size());
				function->locals.push_back({ std::string(), element });
			}
			check_pattern(item, element, local);
		}
	}

	// The patterns of a constructor's fields: of a variant or a struct, which
	// `p_written` names, by name; of Some(...) by position. They are put in
	// their fields' order, `_` for one left out, which needs `..` (spec §5.6).
	// Without fields or a name, the constructor is unknown, and only the
	// patterns' binds are declared.
	void check_items(Pattern &r_pattern, const std::vector<std::string> &p_names, const std::vector<Type> &p_types, const std::string &p_written, const std::string &p_field_id) {
		const bool named = !p_written.empty();
		const bool known = named || !p_types.empty();
		std::vector<Pattern> ordered(p_types.size());
		std::vector<bool> given(p_types.size(), false);
		size_t position = 0;
		for (Pattern &item : r_pattern.items) {
			std::string field = item.field;
			uint32_t field_token = item.field_token;
			if (named && field.empty() && item.kind == PatternKind::Bind) {
				field = item.name; // `var target` is `target = var target`.
				field_token = item.name_token;
			}
			size_t index = p_types.size();
			if (!known) {
			} else if (named) {
				const size_t found = std::find(p_names.begin(), p_names.end(), field) - p_names.begin();
				if (field.empty()) {
					error(item.range, "The fields of " + p_written + " are matched by name, as in " + (p_names.empty() ? std::string("field") : p_names[0]) + " = pattern.");
				} else if (found == p_names.size()) {
					error(item.range, p_written + " has no field \"" + field + "\".");
				} else if (given[found]) {
					error(item.range, "\"" + field + "\" is matched twice.");
				} else {
					index = found;
					refer(field_token, script_id(r_pattern.type.script, p_field_id + field));
				}
			} else if (!field.empty()) {
				error(item.range, "Some(...) takes the pattern of the value inside, without a name.");
			} else if (position >= p_types.size()) {
				error(item.range, "Some(...) takes one pattern.");
			} else {
				index = position++;
			}
			const Type type = index < p_types.size() ? p_types[index] : Type::of(TypeKind::Error);
			int local = -1;
			if (item.kind == PatternKind::Value || item.kind == PatternKind::Constructor) {
				local = int(function->locals.size());
				function->locals.push_back({ std::string(), type });
			}
			check_pattern(item, type, local);
			if (index < p_types.size()) {
				given[index] = true;
				ordered[index] = std::move(item);
			}
		}
		if (known && !named && (r_pattern.rest || position < p_types.size())) {
			error(r_pattern.range, "Some(...) takes one pattern, of the value inside.");
		}
		for (size_t i = 0; known && named && !r_pattern.rest && i < p_types.size(); i++) {
			if (!given[i]) {
				error(r_pattern.range, p_written + "(...) leaves out \"" + p_names[i] + "\"; match it, or end with `..`.");
			}
		}
		r_pattern.items = std::move(ordered);
	}

	/* Exhaustiveness (spec §5.6) */

	// A pattern as the exhaustiveness check sees it: one that matches every
	// value, those of one constructor, whose fields' patterns are `args`, or
	// some other values.
	struct Shape {
		enum class Kind {
			Any,
			Constructor,
			Other,
		};
		Kind kind = Kind::Any;
		int64_t constructor = 0;
		std::vector<Shape> args;
	};

	// The constructors of a type whose values they all make: its variants,
	// None and Some, the values of an enum of this language, false and true,
	// a struct's one; nullopt when there are too many to list.
	std::optional<std::vector<int64_t>> constructors(const Type &p_type) const {
		if (p_type.kind == TypeKind::Bool || p_type.is_optional()) {
			return std::vector<int64_t>{ 0, 1 };
		}
		if (p_type.kind == TypeKind::Struct) {
			return std::vector<int64_t>{ 0 };
		}
		if (p_type.kind == TypeKind::PayloadEnum || (p_type.kind == TypeKind::Enum && p_type.values && !p_type.values->is_engine)) {
			std::vector<int64_t> list;
			for (const auto &[name, value] : p_type.values->list) {
				if (std::find(list.begin(), list.end(), value) == list.end()) {
					list.push_back(value);
				}
			}
			return list;
		}
		return std::nullopt;
	}

	// The types of a constructor's fields.
	std::vector<Type> constructor_fields(const Type &p_type, int64_t p_constructor) const {
		std::vector<Type> fields;
		if (p_type.kind == TypeKind::PayloadEnum) {
			for (const auto &[name, type] : p_type.values->fields[p_constructor]) {
				fields.push_back(type);
			}
		} else if (p_type.is_optional() && p_constructor == 1) {
			fields.push_back(p_type.unwrapped());
		} else if (p_type.kind == TypeKind::Struct) {
			if (const StructDecl *decl = struct_decl(p_type)) {
				for (const VarDecl &field : decl->fields) {
					fields.push_back(field.type);
				}
			}
		}
		return fields;
	}

	Shape shape_of(const Pattern &p_pattern, const Type &p_type) const {
		Shape shape;
		if (p_pattern.kind == PatternKind::Wildcard || p_pattern.kind == PatternKind::Bind) {
			return shape;
		}
		shape.kind = Shape::Kind::Other;
		if (p_pattern.kind == PatternKind::Constructor) {
			const std::vector<Type> fields = constructor_fields(p_type, p_pattern.variant);
			if (p_pattern.type.is_error() || p_pattern.type != p_type || fields.size() != p_pattern.items.size()) {
				return shape;
			}
			shape.kind = Shape::Kind::Constructor;
			shape.constructor = p_pattern.variant;
			for (size_t i = 0; i < fields.size(); i++) {
				shape.args.push_back(shape_of(p_pattern.items[i], fields[i]));
			}
			return shape;
		}
		// A constant of a bool, of an enum or of a variant without fields.
		const Expr *value = p_pattern.value && p_pattern.value->right ? p_pattern.value->right.get() : nullptr;
		const bool listed = p_type.kind == TypeKind::Bool || p_type.kind == TypeKind::Enum || p_type.kind == TypeKind::PayloadEnum;
		if (listed && value && value->constant && value->type == p_type && constructors(p_type)) {
			if (value->constant->kind == ConstValue::Kind::Int || value->constant->kind == ConstValue::Kind::Bool) {
				shape.kind = Shape::Kind::Constructor;
				shape.constructor = value->constant->kind == ConstValue::Kind::Bool ? int64_t(value->constant->b) : value->constant->i;
			}
		}
		return shape;
	}

	// The rows that match the constructor, each with its fields' patterns in
	// place of its first: `_` for each field after one that matches anything.
	static std::vector<std::vector<Shape>> specialized(const std::vector<std::vector<Shape>> &p_rows, int64_t p_constructor, size_t p_arity) {
		std::vector<std::vector<Shape>> rows;
		for (const std::vector<Shape> &row : p_rows) {
			const Shape &head = row[0];
			if (head.kind == Shape::Kind::Any) {
				rows.emplace_back(p_arity);
			} else if (head.kind == Shape::Kind::Constructor && head.constructor == p_constructor) {
				rows.push_back(head.args);
			} else {
				continue;
			}
			rows.back().insert(rows.back().end(), row.begin() + 1, row.end());
		}
		return rows;
	}

	// Whether some values that `p_query` matches match none of the rows, the
	// columns being of `p_types` (Maranget, "Warnings for pattern matching",
	// 2007); the query is made only of `_` and constructors.
	bool useful(const std::vector<std::vector<Shape>> &p_rows, const std::vector<Shape> &p_query, const std::vector<Type> &p_types) const {
		if (p_query.empty()) {
			return p_rows.empty();
		}
		const std::vector<Type> rest_types(p_types.begin() + 1, p_types.end());
		const std::vector<Shape> rest(p_query.begin() + 1, p_query.end());
		const auto specialize = [&](int64_t p_constructor, const std::vector<Shape> &p_args) {
			std::vector<Type> types = constructor_fields(p_types[0], p_constructor);
			std::vector<Shape> query = p_args;
			query.resize(types.size());
			types.insert(types.end(), rest_types.begin(), rest_types.end());
			query.insert(query.end(), rest.begin(), rest.end());
			return useful(specialized(p_rows, p_constructor, types.size() - rest_types.size()), query, types);
		};
		if (p_query[0].kind == Shape::Kind::Constructor) {
			return specialize(p_query[0].constructor, p_query[0].args);
		}
		const std::optional<std::vector<int64_t>> all = constructors(p_types[0]);
		const bool complete = all && std::all_of(all->begin(), all->end(), [&](int64_t p_constructor) {
			return std::any_of(p_rows.begin(), p_rows.end(), [&](const std::vector<Shape> &p_row) { return p_row[0].kind == Shape::Kind::Constructor && p_row[0].constructor == p_constructor; });
		});
		if (complete) {
			return std::any_of(all->begin(), all->end(), [&](int64_t p_constructor) { return specialize(p_constructor, {}); });
		}
		std::vector<std::vector<Shape>> defaults;
		for (const std::vector<Shape> &row : p_rows) {
			if (row[0].kind == Shape::Kind::Any) {
				defaults.emplace_back(row.begin() + 1, row.end());
			}
		}
		return useful(defaults, rest, rest_types);
	}

	// A match over an enum of this language, an enum with payloads or T?
	// handles every value or has `_`, as one over an enum of the engine must;
	// branches with `when` do not count (spec §3.7, §5.6). One that handles
	// every value ends a function when each branch does.
	void check_exhaustive(Stmt &r_match, const Type &p_matched) {
		std::vector<std::vector<Shape>> rows;
		for (const MatchBranch &branch : r_match.branches) {
			for (const Pattern &pattern : branch.patterns) {
				if (!branch.guard) {
					rows.push_back({ shape_of(pattern, p_matched) });
				}
			}
		}
		r_match.exhaustive = !useful(rows, { Shape() }, { p_matched });
		const bool own_enum = (p_matched.kind == TypeKind::Enum && p_matched.values && !p_matched.values->is_engine) || p_matched.kind == TypeKind::PayloadEnum;
		if (r_match.exhaustive || !(own_enum || p_matched.is_optional() || p_matched.kind == TypeKind::Enum)) {
			return;
		}
		if (p_matched.kind == TypeKind::Enum && (!p_matched.values || p_matched.values->is_engine)) {
			error(r_match.value->range, "A match over " + enum_spelling(p_matched) + " needs a `_` branch: the engine's enums are open, it gives values outside them and later versions add more.");
			return;
		}
		// What is left of each constructor: all, or only some values.
		std::vector<std::string> missing;
		const std::vector<int64_t> all = *constructors(p_matched);
		for (int64_t constructor : all) {
			const size_t arity = constructor_fields(p_matched, constructor).size();
			if (!useful(specialized(rows, constructor, arity), std::vector<Shape>(arity), constructor_fields(p_matched, constructor))) {
				continue;
			}
			const bool partly = std::any_of(rows.begin(), rows.end(), [&](const std::vector<Shape> &p_row) { return p_row[0].kind == Shape::Kind::Constructor && p_row[0].constructor == constructor; });
			std::vector<std::string> names;
			if (p_matched.is_optional()) {
				names.push_back(constructor == 0 ? "None" : "Some(..)");
			} else {
				for (const auto &[name, value] : p_matched.values->list) {
					if (value == constructor) {
						names.push_back(arity > 0 ? name + "(..)" : name);
					}
				}
			}
			for (const std::string &name : names) {
				missing.push_back(partly ? "other " + name : name);
			}
		}
		std::string names;
		for (size_t i = 0; i < missing.size() && i < 8; i++) {
			names += (i == 0 ? "" : (i + 1 == missing.size() ? " and " : ", ")) + missing[i];
		}
		if (missing.size() > 8) {
			names += " and " + std::to_string(missing.size() - 8) + " more";
		}
		error(r_match.value->range, "This match over " + (own_enum ? enum_spelling(p_matched) : p_matched.to_string()) + " does not handle " + names + "; handle them or add a `_` branch.");
	}

	// GDScript compares the types first, String and StringName alike, so a
	// pattern that only converts to the value's type never matches there
	// (modules/gdscript/gdscript_compiler.cpp:1441-1477, 1543-1569; ed1daf0).
	bool pattern_fits(const Type &p_pattern, const Type &p_matched) const {
		const auto text = [](const Type &p_type) {
			return p_type.kind == TypeKind::String || p_type == Type::builtin("StringName");
		};
		if (p_matched.kind == TypeKind::Variant || p_pattern == p_matched || (text(p_pattern) && text(p_matched))) {
			return true;
		}
		const bool subtype = (p_pattern.kind == TypeKind::Object && p_matched.kind == TypeKind::Object) || (p_pattern.kind == TypeKind::Enum && p_matched.kind == TypeKind::Int);
		return subtype && assignable(p_pattern, p_matched);
	}

	// `for i in n`, over the elements of an array, over the keys of a
	// Dictionary, over the characters of a String, or over an object (spec §5.7).
	void check_for(Stmt &r_stmt) {
		const Type iterated = check_value(*r_stmt.value);
		Type element = Type::of(TypeKind::Error);
		const std::string &name = iterated.name;
		scopes.emplace_back();
		if (iterated.kind == TypeKind::Int) {
			element = iterated;
		} else if (iterated.kind == TypeKind::Float || (iterated.kind == TypeKind::Builtin && (name == "Vector2" || name == "Vector2i" || name == "Vector3" || name == "Vector3i"))) {
			error(r_stmt.value->range, "Cannot iterate over " + iterated.to_string() + "; iterate over range(...) instead.");
		} else if (const std::optional<std::pair<Type, Type>> entries = entries_of(iterated)) {
			element = entries->first;
		} else if (const std::optional<Type> elements = element_of(iterated)) {
			element = *elements;
		} else if (iterated.is_optional()) {
			error(r_stmt.value->range, "The value may be None (" + iterated.to_string() + "); check it before iterating over it.");
		} else if (iterated.kind == TypeKind::Object) {
			element = check_iteration(r_stmt, iterated);
		} else if (iterated.kind == TypeKind::Variant) {
			error(r_stmt.value->range, "Cannot iterate over a Variant." + variant_hint());
		} else if (iterated.kind == TypeKind::String) {
			element = Type::of(TypeKind::String);
		} else if (!iterated.is_error()) {
			error(r_stmt.value->range, "Cannot iterate over " + iterated.to_string() + ".");
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
		declare_local(r_stmt, type);
		loop_depth++;
		check_block(r_stmt.body);
		loop_depth--;
		scopes.pop_back();
	}

	// The type of the elements of an object whose class of this or another
	// language defines the three methods, and their calls, on locals without
	// names that the body does not see (spec §5.7). Object's own virtual
	// methods, which ClassDB lists in the editor (core/object/class_db.cpp:991-994;
	// ed1daf0), do not count.
	Type check_iteration(Stmt &r_stmt, const Type &p_object) {
		static const char *const methods[] = { "_iter_init", "_iter_next", "_iter_get" };
		std::vector<std::string> missing;
		for (const char *method : methods) {
			const NamedMember found = named_member(p_object, builtin::MemberName::Kind::Method, method);
			if (!found.found || found.id.kind == DeclId::Kind::Engine) {
				missing.push_back(std::string(method) + "()");
			}
		}
		if (!missing.empty()) {
			std::string names;
			for (size_t i = 0; i < missing.size(); i++) {
				names += (i == 0 ? "" : (i + 1 == missing.size() ? " and " : ", ")) + missing[i];
			}
			error(r_stmt.value->range, "Cannot iterate over " + p_object.to_string() + ", whose class lacks " + names + ".");
			return Type::of(TypeKind::Error);
		}
		static const char *const holders[] = { "@object", "@iter", "@state" };
		const Type types[] = { p_object, Type::builtin("Array"), Type::of(TypeKind::Variant) };
		for (int i = 0; i < 3; i++) {
			scopes.back()[holders[i]] = int(function->locals.size());
			function->locals.push_back({ std::string(), types[i] });
		}
		const auto expr = [&](ExprKind p_kind, const char *p_text) {
			auto made = std::make_unique<Expr>();
			made->kind = p_kind;
			made->text = p_text;
			made->range = r_stmt.value->range;
			made->name_token = NO_TOKEN;
			return made;
		};
		for (const char *method : methods) {
			auto call = expr(ExprKind::Call, method);
			call->base = expr(ExprKind::Name, holders[0]);
			call->args.push_back(expr(ExprKind::Name, method == methods[2] ? holders[2] : holders[1]));
			check_value(*call);
			// Another language's may declare another type.
			if (method != methods[2] && !call->type.is_error() && call->type.kind != TypeKind::Bool) {
				error(r_stmt.value->range, std::string(method) + "() of " + p_object.to_string() + " returns " + call->type.to_string() + ", where for needs a bool.");
			}
			r_stmt.iteration.push_back(std::move(call));
		}
		for (const char *holder : holders) {
			scopes.back().erase(holder);
		}
		return r_stmt.iteration[2]->type;
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
				Type value;
				if (r_stmt.value) {
					value = check_value(*r_stmt.value, type);
					if (r_stmt.declared_type) {
						expect_assignable(*r_stmt.value, value, type, "assign");
					} else if (value.kind == TypeKind::None) {
						error(r_stmt.name_token, "\"" + r_stmt.name + "\" needs a type: None does not say which T? it is.");
						type = Type::of(TypeKind::Error);
					} else {
						type = value;
						expect_scene_fits(*r_stmt.value, type);
					}
				} else if (!r_stmt.declared_type) {
					error(r_stmt.name_token, "\"" + r_stmt.name + "\" needs a type or an initial value.");
				} else if (!enum_has_default(type)) {
					error(r_stmt.name_token, "\"" + r_stmt.name + "\" needs an initial value: " + enum_spelling(type) + " has no value 0 to start at.");
				} else if (!has_value_without_initializer(type)) {
					error(r_stmt.name_token, "\"" + r_stmt.name + "\" of type " + type.to_string() + " needs an initial value.");
				}
				declare_local(r_stmt, type);
				narrow_by_value(r_stmt.local, value);
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
					target_type = check_subscript(target);
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
				const bool formats = r_stmt.assign_op == TokenKind::PercentEqual && is_format_string(read_type);
				Type value = check_value(*r_stmt.value, formats ? Type::builtin("Array") : (compound ? Type::of(TypeKind::Error) : target_type));
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
					if (!compound) {
						narrow_by_value(local, value);
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
					const Type value = check_value(*r_stmt.value, returns->expected);
					if (partial(value)) {
						returns->partial.push_back(r_stmt.value.get());
					}
					add_returned(value, r_stmt.value->range);
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
			case StmtKind::Assert: {
				// Without narrowing what follows: release builds remove it (spec §8.3).
				in_assert = true;
				expect_condition(*r_stmt.condition);
				if (r_stmt.value) {
					const Type message = check_value(*r_stmt.value, Type::of(TypeKind::String));
					if (!message.is_error() && message.kind != TypeKind::String) {
						error(r_stmt.value->range, "The message of an assert must be a String, found " + message.to_string() + ". Convert it with str(x).");
					}
				}
				in_assert = false;
				return;
			}
			case StmtKind::Pass:
			case StmtKind::Breakpoint:
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
				statement_value = r_stmt.value.get();
				if (r_stmt.value->kind != ExprKind::Call && r_stmt.value->kind != ExprKind::Await && r_stmt.value->kind != ExprKind::Try) {
					check_expr(*r_stmt.value);
					error(r_stmt.value->range, "This expression has no effect.");
					return;
				}
				r_stmt.value->discarded = r_stmt.value->kind != ExprKind::Try;
				if (check_expr(*r_stmt.value).kind == TypeKind::Task) {
					error(r_stmt.range, "An async call must be awaited, stored or explicitly detached with detach().");
				} else if (r_stmt.value->type.is_result()) {
					error(r_stmt.range, "A Result cannot be discarded: handle its error, or ignore it with .ok().");
				}
				return;
		}
	}

	/* Declarations */

	void declare_members() {
		for (size_t i = 0; i < cls.members.size(); i++) {
			VarDecl &member = cls.members[i];
			if (member_index.count(member.name) || static_index.count(member.name) || function_index.count(member.name) || signal_index.count(member.name)) {
				error(member.name_token, "The member \"" + member.name + "\" is already declared.");
				continue;
			}
			member_index[member.name] = int(cls.all_members.size());
			cls.all_members.push_back(&member);
			refer(member.name_token, script_id(options.path, member.name), SymbolKind::Property);
		}
		for (VarDecl &variable : cls.static_vars) {
			if (member_index.count(variable.name) || static_index.count(variable.name) || function_index.count(variable.name) || signal_index.count(variable.name)) {
				error(variable.name_token, "The member \"" + variable.name + "\" is already declared.");
				continue;
			}
			static_index[variable.name] = int(cls.all_static_vars.size());
			cls.all_static_vars.push_back(&variable);
			refer(variable.name_token, script_id(options.path, variable.name), SymbolKind::Property);
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
			error(r_arg.range, "An annotation argument must be known at compile time, and this one is computed when the class loads.");
			return nullptr;
		}
		return r_arg.constant.get();
	}

	static const HintAnnotation *hint_annotation(const std::string &p_name) {
		for (const HintAnnotation &annotation : HINT_ANNOTATIONS) {
			if (p_name == annotation.name) {
				return &annotation;
			}
		}
		return nullptr;
	}

	static bool fits(std::string_view p_target, const Type &p_type) {
		const std::string type = p_type.to_string();
		for (;;) {
			const size_t end = p_target.find(" and ");
			if (p_target.substr(0, end) == type) {
				return true;
			}
			if (end == std::string_view::npos) {
				return false;
			}
			p_target.remove_prefix(end + 5);
		}
	}

	// An annotation that gives the inspector a hint, its arguments joined by
	// commas into the hint string, each a String, or for @export_range numbers
	// first. On an Array[T] or a packed array it applies to the elements, and
	// the hint string says so (modules/gdscript/gdscript_parser.cpp:4679-4739,
	// 4752-4761, 4979-4989).
	void check_hint(VarDecl &r_member, const Annotation &p_annotation, const HintAnnotation &p_hint) {
		const std::string name = "@" + p_annotation.name;
		if (!p_hint.target) {
			if (!p_annotation.args.empty()) {
				error(p_annotation.range, name + " takes no arguments.");
			}
			if (p_annotation.name == "export_storage") {
				r_member.usage = 2; // PROPERTY_USAGE_STORAGE
			}
			return;
		}
		const std::optional<Type> element = element_of(r_member.type);
		const bool of_elements = element && element->kind != TypeKind::Variant;
		const Type &target = of_elements ? *element : r_member.type;
		if (!fits(p_hint.target, target)) {
			error(p_annotation.range, name + " applies to " + p_hint.target + ", not " + r_member.type.to_string() + ".");
			return;
		}
		const size_t count = p_annotation.args.size();
		if (count < p_hint.least || count > p_hint.most) {
			if (p_hint.hint == 1) {
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
	// (core/object/property_info.h:90-91, 120). The editor and scenes set an
	// export by name, so it must be pub (spec §10.5).
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
			if (!r_member.is_pub) {
				error(annotation.range, "An exported variable must be pub: the editor and scenes set it by name.");
			}
			if (const HintAnnotation *hint = hint_annotation(annotation.name)) {
				check_hint(r_member, annotation, *hint);
			} else if (annotation.name == "export_custom") {
				check_export_custom(r_member, annotation);
			} else if (annotation.name == "export_tool_button") {
				error(annotation.range, "@export_tool_button needs @tool, which is not in the first version.");
			} else {
				error(annotation.range, "The annotation @" + annotation.name + " is not supported yet.");
			}
		}
	}

	// @rpc(mode, sync, transfer_mode, channel): the strings in any order, the
	// channel fourth, as in GDScript (modules/gdscript/gdscript_parser.cpp:187,
	// 5238-5298). The peers call the method by name, so it must be pub (spec
	// §10.5).
	void check_rpc(FuncDecl &r_function, const Annotation &p_annotation) {
		if (r_function.rpc.mode != 0) {
			error(p_annotation.range, "A function takes @rpc once.");
			return;
		}
		if (!r_function.is_pub) {
			error(p_annotation.range, "An @rpc function must be pub: other peers call it by name.");
		}
		if (p_annotation.args.size() > 4) {
			error(p_annotation.range, "@rpc takes at most four arguments.");
			return;
		}
		bytecode::Rpc rpc;
		rpc.mode = 2; // RPC_MODE_AUTHORITY
		int seen[3] = {};
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
			static const struct {
				const char *name;
				int group;
				uint8_t val;
			} options[] = {
				{ "call_remote", 0, 0 },
				{ "call_local", 0, 1 },
				{ "any_peer", 1, 1 },
				{ "authority", 1, 2 },
				{ "unreliable", 2, 0 },
				{ "unreliable_ordered", 2, 1 },
				{ "reliable", 2, 2 },
			};
			const std::string text = value->kind == ConstValue::Kind::String ? value->text : std::string();
			const auto it = std::find_if(std::begin(options), std::end(options), [&](const auto &p_opt) { return text == p_opt.name; });
			if (it == std::end(options)) {
				error(arg.range, "@rpc takes \"call_local\" or \"call_remote\", \"any_peer\" or \"authority\", and \"reliable\", \"unreliable\" or \"unreliable_ordered\", then the channel.");
				return;
			}
			seen[it->group]++;
			if (it->group == 0) {
				rpc.call_local = it->val != 0;
			} else if (it->group == 1) {
				rpc.mode = it->val;
			} else {
				rpc.transfer_mode = it->val;
			}
		}
		static const char *const duplicate_messages[] = {
			"@rpc takes one of \"call_local\" and \"call_remote\".",
			"@rpc takes one of \"any_peer\" and \"authority\".",
			"@rpc takes one transfer mode.",
		};
		for (int g = 0; g < 3; g++) {
			if (seen[g] > 1) {
				error(p_annotation.range, duplicate_messages[g]);
				return;
			}
		}
		r_function.rpc = rpc;
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
		// First, since instance initializers may read them; the types written
		// before any initializer, whose lambdas may read later ones.
		for (VarDecl &variable : cls.static_vars) {
			if (variable.declared_type) {
				variable.type = resolve(*variable.declared_type, false);
			}
		}
		for (size_t i = 0; i < cls.static_vars.size(); i++) {
			check_static_declaration(i);
		}
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
				error(member.name_token, "@onready is for Node scripts; a " + cls.extends + " is never ready.");
			}
			if (member.is_export) {
				error(member.name_token, "@onready and @export do not go together: the node's ready would overwrite what the scene assigned.");
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
				expect_scene_fits(*member.initializer, type);
			}
			// An @onready member gets even a constant when the node is ready.
			if (!member.is_onready) {
				member.initial = reflected_initial(member.initializer.get(), type);
			}
		} else if (!member.declared_type) {
			error(member.name_token, "\"" + member.name + "\" needs a type or an initial value.");
		} else {
			// The scene, the creator or an ancestor assigns it, which is checked
			// before it is read (spec §4.3, §6.2).
			member.must_assign = !has_value_without_initializer(type);
			member.initial = member.must_assign ? nullptr : reflected_initial(nullptr, type);
		}
		const TypeKind kind = type.unwrapped().kind;
		if (member.is_export && is_typed_container(type.unwrapped()) && is_boxed(type.unwrapped())) {
			error(member.name_token, "Containers whose elements exist only in this language, such as " + type.to_string() + ", cannot be exported: exports go through reflection, which does not list them.");
		} else if (member.is_export && (kind == TypeKind::Struct || kind == TypeKind::PayloadEnum)) {
			std::unordered_set<std::string> seen;
			const std::string field = node_field(type.unwrapped(), std::string(), seen);
			if (!field.empty()) {
				error(member.name_token, field + " holds a node, which a scene does not store inside an exported value; export the node as a member of its own.");
			}
		}
		if (member.is_export && type.kind == TypeKind::Object && type.name == "PackedScene" && type.args.empty()) {
			error(member.name_token, "An exported scene is written PackedScene[T], with the type of its root.");
		}
		if (member.is_export && type.kind == TypeKind::Task) {
			error(member.name_token, "Task handles cannot be exported: a scene cannot save a running coroutine.");
		}
		member.type = type;
		check_annotations(member);
		// The inspector's Dictionary editor reads any hint string as the types
		// of the keys and the values; "0:" keeps the keys untyped, and names the
		// type after it (architecture §6.2).
		if (member.is_export && member.hint == 0 && member.hint_string.empty() && (kind == TypeKind::Struct || kind == TypeKind::PayloadEnum)) {
			member.hint_string = "0:" + type.to_string();
		}
		check_accessors(member);
	}

	// A member's first value as reflection shows it, when the compiler knows
	// it: the editor's default (spec §10.1). A constant, or a struct or a
	// variant built of constants and the defaults of the fields left out;
	// without an initializer, a struct's defaults. Null where code computes
	// it, or a part of it loads a resource.
	ConstPtr reflected_initial(const Expr *p_value, const Type &p_type) const {
		if (!p_value) {
			return p_type.kind == TypeKind::Struct ? encoded_fields(p_type, nullptr) : nullptr;
		}
		if (p_value->constant) {
			const ConstPtr value = converted(p_value->constant, p_type);
			return value && !loads(*value) ? encoded(value, p_type) : nullptr;
		}
		const bool constructed = p_value->target == Target::StructConstructor || p_value->target == Target::VariantConstructor;
		return constructed ? encoded_fields(p_value->type, p_value) : nullptr;
	}

	// The struct, or the variant of the enum with payloads, that `p_call`
	// constructs, as a Dictionary by field name (spec §10.1); without a call,
	// the struct's defaults.
	ConstPtr encoded_fields(const Type &p_type, const Expr *p_call) const {
		auto encoded = std::make_shared<ConstValue>();
		encoded->kind = ConstValue::Kind::Dictionary;
		encoded->type = Type::builtin("Dictionary");
		const auto add = [&](const std::string &p_key, const ConstPtr &p_value) {
			auto key = std::make_shared<ConstValue>();
			key->kind = ConstValue::Kind::String;
			key->type = Type::of(TypeKind::String);
			key->text = p_key;
			encoded->items.push_back(key);
			encoded->items.push_back(p_value);
		};
		std::vector<std::pair<std::string, Type>> fields;
		std::vector<const Expr *> values;
		if (const StructDecl *decl = p_type.kind == TypeKind::Struct ? struct_decl(p_type) : nullptr) {
			for (const VarDecl &field : decl->fields) {
				fields.emplace_back(field.name, field.type);
				values.push_back(field.initializer.get());
			}
		} else if (p_type.kind == TypeKind::PayloadEnum && p_call && p_type.values) {
			auto name = std::make_shared<ConstValue>();
			name->kind = ConstValue::Kind::String;
			name->type = Type::of(TypeKind::String);
			name->text = p_type.values->list[p_call->index].first;
			add("$variant", name);
			fields = p_type.values->fields[p_call->index];
			values.resize(fields.size());
		} else {
			return nullptr;
		}
		for (size_t i = 0; p_call && i < p_call->args.size(); i++) {
			// By name, or Ok(v) and Err(e) by position.
			for (size_t f = 0; f < fields.size(); f++) {
				if (p_call->arg_names.empty() ? f == i : fields[f].first == p_call->arg_names[i]) {
					values[f] = p_call->args[i].get();
				}
			}
		}
		for (size_t f = 0; f < fields.size(); f++) {
			const ConstPtr value = values[f] ? reflected_initial(values[f], fields[f].second) : nullptr;
			if (!value) {
				return nullptr;
			}
			add(fields[f].first, value);
		}
		return encoded;
	}

	// A constant as the engine sees a value of the type (spec §10.1): a
	// variant of an enum with payloads as a Dictionary, and a container that
	// keeps its elements boxed untyped, with them encoded.
	static ConstPtr encoded(const ConstPtr &p_value, const Type &p_type) {
		const Type &type = p_type.kind == TypeKind::Option ? p_type.args[0] : p_type;
		if (type.kind == TypeKind::PayloadEnum && p_value->kind == ConstValue::Kind::Int && type.values) {
			auto value = std::make_shared<ConstValue>();
			value->kind = ConstValue::Kind::Dictionary;
			value->type = Type::builtin("Dictionary");
			auto key = std::make_shared<ConstValue>();
			key->kind = ConstValue::Kind::String;
			key->type = Type::of(TypeKind::String);
			key->text = "$variant";
			auto name = std::make_shared<ConstValue>(*key);
			name->text = type.values->list[size_t(p_value->i)].first;
			value->items = { key, name };
			return value;
		}
		if (!p_value->is_container() || !is_boxed(type)) {
			return p_value;
		}
		auto value = std::make_shared<ConstValue>(*p_value);
		value->type = Type::builtin(type.builtin_name());
		const std::optional<Type> element = element_of(type);
		const std::optional<std::pair<Type, Type>> entries = entries_of(type);
		for (size_t i = 0; i < value->items.size(); i++) {
			const bool key = value->kind == ConstValue::Kind::Dictionary && i % 2 == 0;
			const Type item = value->kind == ConstValue::Kind::Array ? *element : (key ? entries->first : entries->second);
			value->items[i] = encoded(value->items[i], item);
		}
		return value;
	}

	// The field of an exported struct or enum with payloads that holds a
	// node at any depth, such as "Hit.source"; "" for none. The engine stores
	// as paths only the nodes of an exported member and of the typed
	// container in it (architecture §6.2).
	std::string node_field(const Type &p_type, const std::string &p_field, std::unordered_set<std::string> &r_seen) const {
		if (p_type.kind == TypeKind::Object) {
			const std::string &native = p_type.engine_class();
			return native == "Node" || engine.inherits(native, "Node") ? p_field : std::string();
		}
		// The type inside T?, or the keys and elements of a container.
		std::vector<std::pair<std::string, Type>> parts;
		const bool held = p_type.kind == TypeKind::Struct || p_type.kind == TypeKind::PayloadEnum;
		for (size_t i = 0; (p_type.kind == TypeKind::Option || p_type.kind == TypeKind::Builtin) && i < p_type.args.size(); i++) {
			parts.emplace_back(p_field, p_type.args[i]);
		}
		if (held && r_seen.insert(p_type.script + "::" + p_type.to_string()).second) {
			const std::string name = p_type.interface_name() + ".";
			if (const StructDecl *decl = p_type.kind == TypeKind::Struct ? struct_decl(p_type) : nullptr) {
				for (const VarDecl &field : decl->fields) {
					parts.emplace_back(name + field.name, field.type);
				}
			}
			for (size_t v = 0; p_type.kind == TypeKind::PayloadEnum && p_type.values && v < p_type.values->fields.size(); v++) {
				for (const auto &[field, type] : p_type.values->fields[v]) {
					parts.emplace_back(name + p_type.values->list[v].first + "." + field, type);
				}
			}
		}
		for (const auto &[field, type] : parts) {
			const std::string found = node_field(type, field, r_seen);
			if (!found.empty()) {
				return found;
			}
		}
		return std::string();
	}

	// A static variable (spec §4.8): typed as a member is (§4.3), but one of
	// a type without a default value needs an initializer. Without the bodies,
	// one whose type is written needs no initializer checked, which may read
	// the class being compiled.
	void check_static_declaration(size_t p_index) {
		VarDecl &variable = cls.static_vars[p_index];
		const auto registered = static_index.find(variable.name);
		if (registered == static_index.end() || cls.all_static_vars[registered->second] != &variable) {
			return;
		}
		Type type = variable.declared_type ? variable.type : Type::of(TypeKind::Error);
		if (variable.initializer && !(options.declarations_only && variable.declared_type)) {
			static_initializing = int(p_index);
			const Type value = check_value(*variable.initializer, type);
			static_initializing = -1;
			if (variable.declared_type) {
				expect_assignable(*variable.initializer, value, type, "assign");
			} else {
				type = value;
				expect_scene_fits(*variable.initializer, type);
			}
		} else if (!variable.declared_type) {
			error(variable.name_token, "\"" + variable.name + "\" needs a type or an initial value.");
		} else if (!variable.initializer && !enum_has_default(type)) {
			error(variable.name_token, "\"" + variable.name + "\" needs an initial value: " + enum_spelling(type) + " has no value 0 to start at.");
		} else if (!variable.initializer && !has_value_without_initializer(type)) {
			error(variable.name_token, "The static variable \"" + variable.name + "\" needs an initial value: " + type.to_string() + " has no default value. Give it one, or the type " + optional_of(type).to_string() + ".");
		}
		variable.type = type;
		check_accessors(variable);
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
				const bool fits = p_setter ? function->params.size() == 1 && !function->has_rest() && function->params[0].type == r_member.type && function->return_type.kind == TypeKind::Void : function->params.empty() && function->return_type == r_member.type;
				if (!fits && !r_member.type.is_error()) {
					error(function->name_token, "As the " + kind + " of \"" + r_member.name + "\", " + p_name + "() " + (p_setter ? "takes one argument of type " + r_member.type.to_string() + " and returns nothing" : "takes no argument and returns " + r_member.type.to_string()) + ".");
				}
			}
			if (r_member.is_static && (function->is_async || !function->is_static)) {
				error(function->name_token, "A " + kind + " of a static variable is static, and not async: " + (p_setter ? "writing" : "reading") + " the variable does not wait for it.");
			} else if (!r_member.is_static && (function->is_async || function->is_static)) {
				error(function->name_token, "A " + kind + " runs on the object, so it is not static, and not async: " + (p_setter ? "writing" : "reading") + " the member does not wait for it.");
			}
			const auto found = function_index.find(p_name);
			return found == function_index.end() ? -1 : found->second;
		};
		r_member.setter_index = accessor(r_member.setter, true);
		r_member.getter_index = accessor(r_member.getter, false);
	}

	// The values themselves are checked with the body (declare_params()).
	void check_defaults(const FuncDecl &p_function) {
		bool any = false;
		for (const ParamDecl &param : p_function.params) {
			any = any || param.default_value;
			if (any && !param.default_value && !param.is_rest) {
				error(param.name_token, "Parameter \"" + param.name + "\" follows one with a default value and needs one too.");
			}
		}
	}

	static int required_params(const FuncDecl &p_function) {
		int count = 0;
		while (count < int(p_function.params.size()) && !p_function.params[count].default_value && !p_function.params[count].is_rest) {
			count++;
		}
		return count;
	}

	// What a rest parameter of type Array or Array[T] takes as each argument
	// (spec §4.6).
	static Type rest_element(const Type &p_rest) {
		if (!p_rest.args.empty()) {
			return p_rest.args[0];
		}
		return p_rest.is_error() ? p_rest : Type::of(TypeKind::Variant);
	}

	// What the function takes as argument `p_index`, nullopt when it takes none.
	static std::optional<Type> argument_type(const FuncDecl &p_function, size_t p_index) {
		const size_t fixed = p_function.params.size() - (p_function.has_rest() ? 1 : 0);
		if (p_index < fixed) {
			return p_function.params[p_index].type;
		}
		return p_function.has_rest() ? std::optional<Type>(rest_element(p_function.params.back().type)) : std::nullopt;
	}

	static bool takes(const FuncDecl &p_function, size_t p_count) {
		return p_count >= size_t(required_params(p_function)) && (p_function.has_rest() || p_count <= p_function.params.size());
	}

	// A parameter whose type nothing gives (spec §4.6).
	void untyped(ParamDecl &r_param, const std::string &p_why = std::string()) {
		error(r_param.name_token, r_param.is_rest ? "The rest parameter \"" + r_param.name + "\" needs a type: Array, or Array[T] for arguments of type T." : "Parameter \"" + r_param.name + "\" needs a type" + p_why + ".");
		r_param.type = Type::of(TypeKind::Error);
	}

	void check_rest(const FuncDecl &p_function) {
		const ParamDecl *rest = p_function.has_rest() ? &p_function.params.back() : nullptr;
		if (rest && !rest->type.is_error() && rest->type.builtin_name() != "Array") {
			error(rest->declared_type ? rest->declared_type->range : rest->range, "The rest parameter takes the arguments as an Array or an Array[T], not as " + rest->type.to_string() + ".");
		}
	}

	// A static function has no instance (spec §4.8).
	bool refuse_in_static(const Expr &p_expr, const std::string &p_name) {
		if (structure) {
			error(p_expr.range, "\"" + p_name + "\" belongs to an object of " + display_name(cls, options.path) + ", which " + structure->name + " does not have: a struct's methods have no object of the class around it.");
			return true;
		}
		if (function ? !function->is_static : static_initializing < 0) {
			// A lambda that uses self holds it, and so do those it is in (spec §3.11).
			for (size_t i = 0; function && function->is_lambda && i <= enclosing.size(); i++) {
				FuncDecl *lambda = i < enclosing.size() ? enclosing[i].function : function;
				if (lambda && lambda->is_lambda) {
					lambda->uses_self = true;
				}
			}
			return false;
		}
		const FuncDecl *outer = outermost();
		error(p_expr.range, "\"" + p_name + "\" belongs to an instance, which " + (outer ? "the static function \"" + outer->name + "()\"" : std::string("a static variable's initializer")) + " does not have.");
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
			error(r_fn.name_token, "An override must preserve its base method's visibility.");
		}
		if (r_fn.is_static != p_base.is_static || r_fn.is_async != p_base.is_async) {
			error(r_fn.name_token, "An override must preserve static and async.");
		}
		const size_t base_fixed = p_base.params.size() - (p_base.has_rest() ? 1 : 0);
		for (size_t i = 0; i < r_fn.params.size(); ++i) {
			ParamDecl &param = r_fn.params[i];
			const std::optional<Type> base = argument_type(p_base, i);
			if (param.declared_type) {
				param.type = resolve(*param.declared_type, false);
			} else if (param.is_rest ? p_base.has_rest() : base.has_value()) {
				param.type = param.is_rest ? p_base.params.back().type : *base;
			} else {
				untyped(param);
			}
			// A rest parameter takes each argument of the base from here on, which
			// it holds as an Array of it.
			bool narrows = !param.is_rest && base && !override_assignable(*base, param.type);
			for (size_t j = i; param.is_rest && j < base_fixed; j++) {
				narrows = narrows || !override_assignable(Type::array(p_base.params[j].type), param.type);
			}
			if (param.is_rest && p_base.has_rest()) {
				narrows = narrows || !override_assignable(p_base.params.back().type, param.type);
			}
			if (narrows) {
				error(param.name_token, "The override narrows its base parameter type.");
			}
		}
		check_rest(r_fn);
		check_defaults(r_fn);
		if (p_base.has_rest() && !r_fn.has_rest()) {
			error(r_fn.name_token, "The override must keep the rest parameter of its base method.");
		} else if (!takes(r_fn, base_fixed) || required_params(r_fn) > required_params(p_base)) {
			error(r_fn.name_token, "The override must accept every argument count of its base method.");
		}
		r_fn.return_type = r_fn.return_type_ref ? resolve(*r_fn.return_type_ref, true) : p_base.return_type;
		if (!override_assignable(r_fn.return_type, p_base.return_type)) {
			error(r_fn.name_token, "The override widens its base return type.");
		}
	}

	// Its declaration, with what it overrides; an inline accessor has no name
	// of its own.
	void declare_function(const FuncDecl &p_function, const std::optional<DeclId> &p_overrides) {
		if (p_function.name.rfind('@', 0) != 0) {
			refer(p_function.name_token, script_id(options.path, p_function.name), SymbolKind::Method, p_overrides);
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

	// The class names the constants of the engine class it extends bare, so
	// it declares nothing else by their names, as in GDScript
	// (modules/gdscript/gdscript_analyzer.cpp:255-276; spec §4.1).
	void check_engine_constant_names() {
		const auto check = [&](const std::string &p_name, uint32_t p_token) {
			if (!p_name.empty() && engine.find_constant(cls.native_base, p_name)) {
				error(p_token, "\"" + p_name + "\" is already a constant of " + cls.native_base + ", which this class extends.");
			}
		};
		for (const VarDecl &member : cls.members) {
			check(member.name, member.name_token);
		}
		for (const VarDecl &variable : cls.static_vars) {
			check(variable.name, variable.name_token);
		}
		for (const ConstDecl &constant : cls.constants) {
			check(constant.name, constant.name_token);
		}
		for (const SignalDecl &signal : cls.signals) {
			check(signal.name, signal.name_token);
		}
		for (const EnumDecl &decl : cls.enums) {
			check(decl.name, decl.name_token);
			for (const EnumValueDecl &value : decl.values) {
				if (decl.name.empty()) {
					check(value.name, value.name_token);
				}
			}
		}
		for (const std::unique_ptr<ClassDecl> &inner : cls.classes) {
			check(inner->name, inner->name_token);
		}
	}

	// new() constructs the class (spec §4.1).
	void refuse_new(const FuncDecl &p_function) {
		if (p_function.name == "new") {
			error(p_function.name_token, "A function cannot be named \"new\": new() constructs the class.");
		}
	}

	void check_signature(FuncDecl &r_function) {
		if (!r_function.of_struct) {
			refuse_new(r_function);
		}
		check_abstract(r_function);
		// Each class's _init() has its own signature (spec §6.1).
		if (const FuncDecl *parent = r_function.name == "_init" ? nullptr : base_function(r_function.name)) {
			declare_function(r_function, script_id(declaring(*cls.base, *parent, &ClassDecl::functions).path, parent->name));
			check_script_override(r_function, *parent);
			return;
		}
		// Object binds _init() as a virtual (core/object/object.cpp:1928; ed1daf0),
		// but it is no override: the engine does not call it by name.
		const std::optional<EngineMethod> engine_method = r_function.name == "_init" ? std::nullopt : engine.find_method(cls.native_base, r_function.name);
		declare_function(r_function, engine_method && engine_method->is_virtual ? std::optional<DeclId>(engine_id(engine_method->owner, r_function.name)) : std::nullopt);
		if (engine_method && !engine_method->is_virtual) {
			error(r_function.name_token, "\"" + r_function.name + "()\" is a method of " + cls.extends + " that cannot be overridden.");
		}
		r_function.is_override = engine_method && engine_method->is_virtual;
		if (r_function.is_async) {
			if (r_function.name == "_init") {
				error(r_function.name_token, "_init() cannot be async: construction does not wait for it.");
			} else if (r_function.is_override && !engine_method->return_type.is_error() && engine_method->return_type.kind != TypeKind::Void) {
				error(r_function.name_token, "An engine virtual method returning " + engine_method->return_type.to_string() + " cannot be async: the engine takes the result at once, without waiting.");
			}
		}
		// It takes as many arguments as the engine passes (spec §4.6).
		if (r_function.is_override && !takes(r_function, engine_method->params.size())) {
			error(r_function.name_token, "\"" + r_function.name + "()\" overrides an engine method with " + std::to_string(engine_method->params.size()) + " parameter(s).");
		}
		for (size_t i = 0; i < r_function.params.size(); i++) {
			ParamDecl &param = r_function.params[i];
			// What the engine passes in it: an argument, or each from here on.
			std::vector<Type> engine_types;
			for (size_t j = i; r_function.is_override && j < engine_method->params.size() && (j == i || param.is_rest); j++) {
				Type engine_type = engine_method->params[j].type;
				// The engine passes its own objects to virtual methods.
				engine_type.nullable = false;
				if (!engine_type.is_storable()) {
					error(param.name_token, "Parameters of type " + engine_type.to_string() + " are not supported yet.");
					engine_type = Type::of(TypeKind::Error);
				}
				engine_types.push_back(engine_type);
			}
			if (param.declared_type) {
				param.type = resolve(*param.declared_type, false);
				// A parameter may accept more than the overridden one does, such as
				// T? for T (spec §4.6).
				const Type taken = param.is_rest ? rest_element(param.type) : param.type;
				for (const Type &engine_type : engine_types) {
					if (!engine_type.is_error() && !taken.is_error() && !assignable(engine_type, taken)) {
						error(param.declared_type->range, param.is_rest ? "The overridden method passes " + engine_type.to_string() + " in this rest parameter." : "The overridden method declares this parameter as " + engine_type.to_string() + ".");
					}
				}
			} else if (!param.is_rest && !engine_types.empty()) {
				// The type comes from the overridden declaration (spec §4.6).
				param.type = engine_types[0];
			} else if (!r_function.accessor_of.empty()) {
				// An inline setter takes the member's type (check_accessors()).
			} else {
				untyped(param);
			}
		}
		check_rest(r_function);
		check_defaults(r_function);
		if (r_function.is_static && r_function.is_override) {
			error(r_function.name_token, "\"" + r_function.name + "()\" overrides an engine method and cannot be static.");
		}

		if (r_function.name == "_init" && r_function.return_type_ref && r_function.return_type_ref->name != "void") {
			error(r_function.return_type_ref->range, "_init() returns nothing.");
		}
		// Called once when the class loads, as GDScript's
		// (modules/gdscript/gdscript_parser.cpp:1751-1757; spec §4.8).
		if (r_function.name == "_static_init" && (!r_function.is_static || !r_function.params.empty() || r_function.is_async || (r_function.return_type_ref && r_function.return_type_ref->name != "void"))) {
			error(r_function.name_token, "_static_init() is static, takes no parameters, returns nothing and cannot await.");
		}

		if (r_function.return_type_ref) {
			r_function.return_type = resolve(*r_function.return_type_ref, true);
			// Covariant (spec §4.6): the engine takes any value where it declares
			// a Variant, as for _iter_get() (§5.7).
			const Type &overridden = r_function.is_override ? engine_method->return_type : Type::of(TypeKind::Error);
			if (!override_assignable(r_function.return_type, overridden) && (overridden.kind != TypeKind::Variant || r_function.return_type.kind == TypeKind::Void)) {
				error(r_function.return_type_ref->range, "The overridden method returns " + engine_method->return_type.to_string() + ".");
			}
		} else if (r_function.is_override) {
			r_function.return_type = engine_method->return_type;
			if (r_function.return_type.kind != TypeKind::Void && !r_function.return_type.is_storable()) {
				error(r_function.name_token, "Return values of type " + r_function.return_type.to_string() + " are not supported yet.");
				r_function.return_type = Type::of(TypeKind::Error);
			}
		} else {
			r_function.return_type = Type::of(TypeKind::Void);
		}
	}

	// A function's default values see self and the parameters before them
	// (spec §4.6).
	void declare_params(FuncDecl &r_function, bool p_defaults = false) {
		for (const ParamDecl &param : r_function.params) {
			// What a syntax error left is reported.
			if (p_defaults && param.default_value && param.default_value->kind != ExprKind::Error) {
				expect_assignable(*param.default_value, check_value(*param.default_value, param.type), param.type, "use");
			}
			if (scopes.back().count(param.name)) {
				error(param.name_token, "Parameter \"" + param.name + "\" is declared twice.");
			}
			scopes.back()[param.name] = int(r_function.locals.size());
			r_function.locals.push_back({ param.name, param.type, param.name_token });
			refer_local(param.name_token, param.type);
		}
	}

	// Why a member that a scene names must be pub (spec §10.5).
	static std::string not_pub(bool p_method) {
		return p_method ? ", which is not pub; the engine calls only pub methods by name." : ", which is not pub; the engine finds only pub properties by name.";
	}

	// The members of this class that the scene's animation tracks and
	// synchronized properties name (spec §7.7), each resolved as $X is. The
	// class must have them, and the engine finds them by name, so they must be
	// pub (§10.5). An engine class may take names that it does not list: those
	// with a slash, as metadata/ (core/object/object.cpp:279; ed1daf0), and any
	// where the class has _set() or _get(). A path that leads to no node is
	// left to the engine at run time.
	void check_named_members() {
		const TokenRange header{ cls.extends_token, cls.extends_token };
		const bool takes_any = function_index.count("_set") || function_index.count("_get");
		for (const Scene &scene : scenes) {
			for (const SceneNamedMember &named : scene.named_members) {
				const ViewNode *node = walk_node_path(scene, named.from, named.node).node;
				if (!node || (node->node.script.empty() ? node->node.instance_script : node->node.script) != options.path) {
					continue;
				}
				const auto shown = [](const std::string &p_path) { return p_path == "." ? std::string("the root") : p_path; };
				const std::string what = named.method ? "calls " + named.name + "() on " + shown(node->path) : (named.animation.empty() ? "synchronizes \"" : "animates \"") + named.name + "\" of " + shown(node->path);
				const std::string where = scene.path + ": " + (named.animation.empty() ? shown(named.from) : "the animation \"" + named.animation + "\" of " + shown(named.from)) + " " + what;
				if (named.method) {
					const auto found = function_index.find(named.name);
					const FuncDecl *method = found == function_index.end() ? nullptr : cls.all_functions[found->second];
					const std::optional<EngineMethod> native = method ? std::nullopt : engine.find_method(cls.native_base, named.name);
					if (method && !method->is_pub && !method->is_override) {
						error_in_scene(own_name(*method, cls.functions, header), scene, node->path, where + not_pub(true));
					} else if (!method && (!native || native->is_virtual)) {
						error_in_scene(header, scene, node->path, where + ", which this class does not declare.");
					}
					continue;
				}
				const auto found = member_index.find(named.name);
				const VarDecl *member = found == member_index.end() ? nullptr : cls.all_members[found->second];
				if (member && !member->is_pub) {
					error_in_scene(own_name(*member, cls.members, header), scene, node->path, where + not_pub(false));
				} else if (!member && !static_index.count(named.name) && !takes_any && named.name.find('/') == std::string::npos && !engine.find_property(cls.native_base, named.name)) {
					error_in_scene(header, scene, node->path, where + ", which this class does not declare.");
				}
			}
		}
	}

	// Connections saved in scenes to methods of this class (spec §7.7), on a
	// node with it, also one inside a scene it instances, or on an instance of
	// a scene whose root has it: the method
	// must exist and take what the signal passes. The engine finds the method
	// and the signal by name, so both must be pub (§10.5). A method of the
	// engine class that is not virtual is found so too.
	void check_connections() {
		const TokenRange header{ cls.extends_token, cls.extends_token };
		for (const Scene &scene : scenes) {
			for (const SceneConnection &connection : scene.connections) {
				const ViewNode *target = view_node(scene, connection.target);
				if (!target || (target->node.script.empty() ? target->node.instance_script : target->node.script) != options.path) {
					continue;
				}
				const std::string where = scene.path + ": the signal \"" + connection.signal + "\" of " + connection.source + " is connected to " + connection.method + "()";
				const auto found = function_index.find(connection.method);
				const FuncDecl *method = found == function_index.end() ? nullptr : cls.all_functions[found->second];
				const std::optional<EngineMethod> native = method ? std::nullopt : engine.find_method(cls.native_base, connection.method);
				if (!method && (!native || native->is_virtual)) {
					error_in_scene(header, scene, connection.target, where + ", which this class does not declare.");
					continue;
				}
				const TokenRange at = method ? own_name(*method, cls.functions, header) : header;
				if (method && !method->is_pub) {
					error_in_scene(at, scene, connection.target, where + not_pub(true));
				}
				if (method && references) {
					Reference reference;
					reference.target = script_id(declaring(cls, *method, &ClassDecl::functions).path, method->name);
					reference.scene = scene.path;
					reference.connection = connection;
					references->push_back(std::move(reference));
				}
				const SceneNode *source = scene.find_node(connection.source);
				bool missing = false;
				bool hidden = false;
				const std::optional<std::vector<Type>> arguments = source ? signal_arguments(*source, connection.signal, missing, hidden) : std::nullopt;
				if (missing) {
					error_in_scene(header, scene, connection.target, where + ", but " + connection.source + " has no such signal.");
				} else if (hidden) {
					error_in_scene(header, scene, connection.target, where + ", but that signal is not pub; the engine connects only pub signals by name.");
				}
				if (!arguments) {
					continue;
				}
				const int passed = int(arguments->size()) - connection.unbinds + connection.binds;
				const int most = int(method ? method->params.size() : native->params.size());
				const int required = method ? required_params(*method) : most - native->default_count;
				const bool rest = method ? method->has_rest() : native->is_vararg;
				if (passed < required || (!rest && passed > most)) {
					error_in_scene(at, scene, connection.target, where + ", which passes " + std::to_string(passed) + " argument(s); " + connection.method + "() takes " + argument_count(required, most, rest) + ".");
					continue;
				}
				for (int i = 0; i < int(arguments->size()) - connection.unbinds; i++) {
					std::optional<Type> param = method ? argument_type(*method, size_t(i)) : std::nullopt;
					// The rest of a vararg engine method are Variant.
					if (!method && i < most) {
						param = native->params[i].type;
					}
					if (param && param->is_storable() && !assignable((*arguments)[i], *param)) {
						const bool own = method && is_own(*method, cls.functions);
						const ParamDecl *named = own ? &method->params[std::min(size_t(i), method->params.size() - 1)] : nullptr;
						error_in_scene(named ? TokenRange{ named->name_token, named->name_token } : header, scene, connection.target, where + ", which passes " + (*arguments)[i].to_string() + " here.");
					}
				}
			}
		}
	}

	// No node of a scene has an abstract class (spec §4.1), which the engine
	// refuses to set (core/object/object.cpp:976; ed1daf0).
	void check_abstract_scenes() {
		for (const Scene &scene : cls.is_abstract ? scenes : std::vector<Scene>()) {
			for (const SceneNode &node : scene.nodes) {
				if (node.script == options.path) {
					error_in_scene(TokenRange{ cls.extends_token, cls.extends_token }, scene, node.path, scene.path + ": " + (node.path == "." ? std::string("the root") : node.path) + " has " + display_name(cls, options.path) + ", which is abstract and cannot be set on a node.");
				}
			}
		}
	}

	// What a signal of a node in a scene passes: one its script of this
	// language declares (spec §4.7), or one of its engine class. nullopt when
	// that is not known: a script of another language, or one with errors;
	// `r_missing` when the node certainly has no such signal, `r_hidden` when
	// it is not pub.
	std::optional<std::vector<Type>> signal_arguments(const SceneNode &p_node, const std::string &p_signal, bool &r_missing, bool &r_hidden) {
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
					r_hidden = !signal.is_pub;
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
		for (InterfaceDecl &interface : cls.interfaces) {
			refer(interface.name_token, script_id(options.path, interface.name), SymbolKind::Class);
			for (FuncDecl &method : interface.methods) {
				refuse_new(method);
				refer(method.name_token, script_id(options.path, interface.name + "." + method.name), SymbolKind::Method);
				for (ParamDecl &param : method.params) {
					if (!param.declared_type) {
						untyped(param);
						continue;
					}
					param.type = resolve(*param.declared_type, false);
				}
				check_rest(method);
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
				refer(property.name_token, script_id(options.path, interface.name + "." + property.name), SymbolKind::Property);
				property.type = resolve(property.type_ref, false);
			}
			for (SignalDecl &signal : interface.signals) {
				if (!members.insert(signal.name).second) {
					error(signal.name_token, "\"" + signal.name + "\" is already declared in " + interface.name + ".");
				}
				refer(signal.name_token, script_id(options.path, interface.name + "." + signal.name), SymbolKind::Signal);
				type_signal_params(signal);
			}
			for (const TypeRef &ref : interface.extends) {
				const Type parent = resolve(ref, false);
				if (!parent.is_error() && !parent.is_interface) {
					error(ref.range, parent.to_string() + " is not an interface.");
				} else if (!parent.is_error()) {
					// It shows the parent's members wherever it is seen (spec §4.2).
					if (const std::optional<Type> hidden = interface.is_pub ? hidden_part(parent) : std::nullopt) {
						error(ref.range, hidden->to_string() + " is not pub, but the pub " + interface.name + " extends it.");
					}
					interface.parents.push_back(parent);
				}
			}
		}
		for (const InterfaceDecl &interface : cls.interfaces) {
			check_interface_parents(interface);
		}
	}

	// What an interface extends (spec §3.9): no cycle, and each name with one
	// signature, or a method redeclared with a narrower return type.
	void check_interface_parents(const InterfaceDecl &p_interface) {
		const Type self = Type::interface(display_name(cls, options.path) + "." + p_interface.name, options.path);
		const std::vector<Type> up = interface_order(p_interface.parents);
		if (std::any_of(up.begin(), up.end(), [&](const Type &p_up) { return p_up == self; })) {
			error(p_interface.name_token, p_interface.name + " extends itself.");
			return;
		}
		const std::vector<InterfaceMember> all = interface_members({ self });
		for (const Type &parent : p_interface.parents) {
			for (const InterfaceMember &inherited : interface_members({ parent })) {
				if (const std::optional<std::pair<InterfaceMember, uint32_t>> mine = own_interface_member(p_interface, self, inherited.name())) {
					if (!fits_inherited(mine->first, inherited, true)) {
						error(mine->second, "\"" + inherited.name() + "\" does not fit \"" + inherited.name() + "\" of " + inherited.owner.name + ".");
					}
					continue;
				}
				// Another parent's must agree, unless one extends the other.
				const auto found = std::find_if(all.begin(), all.end(), [&](const InterfaceMember &p_known) { return p_known.name() == inherited.name(); });
				if (found != all.end() && !(found->owner == inherited.owner) && !is_a(found->owner, inherited.owner) && !fits_inherited(*found, inherited, false)) {
					error(p_interface.name_token, found->owner.name + " and " + inherited.owner.name + " declare \"" + inherited.name() + "\" differently; redeclare it in " + p_interface.name + ".");
				}
			}
		}
	}

	// A member the interface itself declares, and its name's token.
	static std::optional<std::pair<InterfaceMember, uint32_t>> own_interface_member(const InterfaceDecl &p_interface, const Type &p_self, const std::string &p_name) {
		for (const FuncDecl &method : p_interface.methods) {
			if (method.name == p_name) {
				return std::pair(InterfaceMember{ &method, nullptr, nullptr, p_self }, method.name_token);
			}
		}
		for (const InterfaceProperty &property : p_interface.properties) {
			if (property.name == p_name) {
				return std::pair(InterfaceMember{ nullptr, &property, nullptr, p_self }, property.name_token);
			}
		}
		for (const SignalDecl &signal : p_interface.signals) {
			if (signal.name == p_name) {
				return std::pair(InterfaceMember{ nullptr, nullptr, &signal, p_self }, signal.name_token);
			}
		}
		return std::nullopt;
	}

	// Whether a member stands for one an interface inherits: a method with the
	// same parameters and a return type that is the same or narrower, when it
	// redeclares it; otherwise the same signature, the same property type, or
	// the same signal arguments. A property that must be writable stands for
	// one that need only be readable, not the other way.
	bool fits_inherited(const InterfaceMember &p_member, const InterfaceMember &p_inherited, bool p_redeclared) const {
		if (p_member.method && p_inherited.method) {
			const FuncDecl &a = *p_member.method;
			const FuncDecl &b = *p_inherited.method;
			bool same = a.params.size() == b.params.size() && a.is_async == b.is_async && (p_redeclared ? assignable(a.return_type, b.return_type) : a.return_type == b.return_type);
			for (size_t i = 0; same && i < a.params.size(); i++) {
				same = a.params[i].type == b.params[i].type && a.params[i].is_rest == b.params[i].is_rest;
			}
			return same;
		}
		if (p_member.property && p_inherited.property) {
			return p_member.property->type == p_inherited.property->type && (!p_member.property->is_readonly || p_inherited.property->is_readonly || !p_redeclared);
		}
		if (p_member.signal && p_inherited.signal) {
			return param_types(p_member.signal->params) == param_types(p_inherited.signal->params);
		}
		return false;
	}

	// The fields of the structs the class declares (spec §3.6); the types that
	// only default values give come with check_field_defaults().
	void declare_structs() {
		for (StructDecl &decl : cls.structs) {
			refer(decl.name_token, script_id(options.path, decl.name), SymbolKind::Class);
			std::unordered_set<std::string> fields;
			for (VarDecl &field : decl.fields) {
				refer(field.name_token, script_id(options.path, decl.name + "." + field.name), SymbolKind::Property);
				if (!fields.insert(field.name).second) {
					error(field.name_token, "The field \"" + field.name + "\" is already declared in " + decl.name + ".");
				}
				field.type = field.declared_type ? resolve(*field.declared_type, false) : Type::of(TypeKind::Error);
				if (!field.initializer && !field.declared_type) {
					error(field.name_token, "\"" + field.name + "\" needs a type or a default value.");
				}
			}
			for (FuncDecl &method : decl.methods) {
				const std::string name = method.name.substr(decl.name.size() + 1);
				if (fields.count(name) || function_index.count(method.name)) {
					error(method.name_token, "\"" + name + "\" is already declared in " + decl.name + ".");
					continue;
				}
				function_index[method.name] = int(cls.all_functions.size());
				cls.all_functions.push_back(&method);
			}
		}
	}

	// A struct or an enum with payloads that holds itself without a container
	// has no finite size (spec §3.6). Once the fields of both are resolved.
	void check_held_cycles() {
		std::vector<std::string> held;
		for (const StructDecl &decl : cls.structs) {
			held.push_back(decl.name);
		}
		for (const EnumDecl &decl : cls.enums) {
			if (decl.has_payloads && !decl.name.empty()) {
				held.push_back(decl.name);
			}
		}
		for (const std::string &name : held) {
			std::vector<std::string> cycle;
			std::unordered_set<std::string> seen;
			if (holds(cls, options.path, name, options.path + "::" + name, cycle, seen)) {
				std::string fields;
				for (const std::string &field : cycle) {
					fields += (fields.empty() ? "" : ", ") + field;
				}
				const StructDecl *found = cls.find_struct(name);
				error(found ? found->name_token : cls.find_enum(name)->name_token, name + " contains itself through " + fields + ", so it has no finite size; hold it in a container or a RefCounted class.");
			}
		}
	}

	// Once the signatures are known, as for member initializers. A default
	// value sees what a static method of the struct sees, which is no field.
	void check_field_defaults() {
		for (StructDecl &decl : cls.structs) {
			structure = &decl;
			structure_type = Type::structure(display_name(cls, options.path) + "." + decl.name, options.path);
			for (VarDecl &field : decl.fields) {
				if (!field.initializer) {
					continue;
				}
				field_default = &field;
				const Type value = check_value(*field.initializer, field.type);
				field_default = nullptr;
				if (field.declared_type) {
					expect_assignable(*field.initializer, value, field.type, "assign");
				} else if (value.kind == TypeKind::None) {
					error(field.name_token, "\"" + field.name + "\" needs a type: None does not say which T? it is.");
				} else {
					field.type = value;
				}
			}
		}
		structure = nullptr;
	}

	// The fields of the struct, or of the variants of the enum with payloads,
	// that the header declares by that name, each as an error names it.
	static std::vector<std::pair<std::string, const VarDecl *>> value_fields(const ClassDecl &p_header, const std::string &p_name) {
		std::vector<std::pair<std::string, const VarDecl *>> fields;
		if (const StructDecl *found = p_header.find_struct(p_name)) {
			for (const VarDecl &field : found->fields) {
				fields.push_back({ p_name + "." + field.name, &field });
			}
		} else if (const EnumDecl *found = p_header.find_enum(p_name)) {
			for (const EnumValueDecl &value : found->values) {
				for (const VarDecl &field : value.fields) {
					fields.push_back({ p_name + "." + value.name + "." + field.name, &field });
				}
			}
		}
		return fields;
	}

	// Whether the struct or the enum with payloads holds `p_target`
	// ("script::Name") in its slot, directly or through other ones, with the
	// field types their classes resolved; `r_cycle` lists the fields on the
	// way. A class whose declarations are being checked is not followed: the
	// check of the cycle's first class sees all the others.
	bool holds(const ClassDecl &p_decl, const std::string &p_path, const std::string &p_name, const std::string &p_target, std::vector<std::string> &r_cycle, std::unordered_set<std::string> &r_seen) {
		if (!r_seen.insert(p_path + "::" + p_name).second) {
			return false;
		}
		for (const auto &[label, field] : value_fields(p_decl, p_name)) {
			for (const Type &type : held_types(field->type)) {
				const ClassDecl *decl = type == Type::conversion_error() ? nullptr : script_decl(type.script);
				if (!decl) {
					continue;
				}
				r_cycle.push_back(label);
				if (type.script + "::" + type.interface_name() == p_target || holds(*decl, type.script, type.interface_name(), p_target, r_cycle, r_seen)) {
					return true;
				}
				r_cycle.pop_back();
			}
		}
		return false;
	}

	// The structs and enums with payloads whose values a value of the type
	// holds in its slot: T holds a T, as T? and Settled[T] do, and
	// Result[T, E] holds both; a container holds a reference.
	static std::vector<Type> held_types(const Type &p_type) {
		if (p_type.kind == TypeKind::Option || p_type.is_result() || p_type.is_settled()) {
			std::vector<Type> held;
			for (const Type &arg : p_type.args) {
				const std::vector<Type> inner = held_types(arg);
				held.insert(held.end(), inner.begin(), inner.end());
			}
			return held;
		}
		return p_type.kind == TypeKind::Struct || p_type.kind == TypeKind::PayloadEnum ? std::vector<Type>{ p_type } : std::vector<Type>();
	}

	// `implements I`: the class has every method of I and of the interfaces I
	// extends, pub and with the same signature, and is each of those
	// interfaces (spec §3.9).
	void check_implements() {
		std::vector<std::pair<Type, const TypeRef *>> named;
		for (const TypeRef &ref : cls.implements) {
			const Type type = resolve(ref, false);
			if (type.is_error()) {
				continue;
			}
			if (!type.is_interface) {
				error(ref.range, type.to_string() + " is not an interface.");
				continue;
			}
			named.push_back({ type, &ref });
			for (const Type &interface : interface_order({ type })) {
				if (std::find(cls.implemented.begin(), cls.implemented.end(), interface) == cls.implemented.end()) {
					cls.implemented.push_back(interface);
				}
			}
		}
		for (const auto &entry : named) {
			const Type &type = entry.first;
			const TypeRef &ref = *entry.second;
			// One that another named interface extends is checked with it.
			if (std::any_of(named.begin(), named.end(), [&](const auto &p_other) { return !(p_other.first == type) && is_a(p_other.first, type); })) {
				continue;
			}
			own_interfaces.push_back({ type, &ref });
			const size_t reported = errors.size();
			const InterfaceDecl *interface = interface_decl(type);
			if (!interface) {
				error(ref.range, type.script + " has errors.");
				note_implementation(type, ref, reported);
				continue;
			}
			for (const InterfaceMember &member : interface_members({ type })) {
				if (!member.method) {
					continue;
				}
				const FuncDecl &method = *member.method;
				const auto found = function_index.find(method.name);
				if (found == function_index.end()) {
					error(ref.range, "The class does not implement " + method.name + "() of " + member.owner.name + ".");
					continue;
				}
				const FuncDecl &function = *cls.all_functions[found->second];
				if (is_own(function, cls.functions)) {
					refer(function.name_token, script_id(options.path, function.name), SymbolKind::Method, interface_member_id(member));
				}
				if (!function.is_pub) {
					error(own_name(function, cls.functions, ref.range), "\"" + function.name + "()\" implements " + member.owner.name + " and must be pub.");
				}
				if (function.is_static) {
					error(own_name(function, cls.functions, ref.range), "\"" + function.name + "()\" implements " + member.owner.name + ", so it cannot be static.");
				}
				bool same = function.params.size() == method.params.size() && function.return_type == method.return_type && function.is_async == method.is_async;
				for (size_t i = 0; same && i < method.params.size(); i++) {
					same = function.params[i].type == method.params[i].type && function.params[i].is_rest == method.params[i].is_rest;
				}
				if (!same) {
					error(own_name(function, cls.functions, ref.range), "\"" + function.name + "()\" does not have the signature that " + member.owner.name + " declares, " + signature_text(method) + ".");
				}
			}
			note_implementation(type, ref, reported);
		}
	}

	// That the class has errors in implementing the interface, when some were
	// reported since `p_reported`.
	void note_implementation(const Type &p_interface, const TypeRef &p_ref, size_t p_reported) {
		const auto &noted = cls.implemented_with_errors;
		if (errors.size() == p_reported || std::any_of(noted.begin(), noted.end(), [&](const auto &p_noted) { return p_noted.first == p_interface; })) {
			return;
		}
		cls.implemented_with_errors.push_back({ p_interface, cls.source->position(cls.source->begin(p_ref.range)).line });
	}

	// A class of another file used as an interface it has errors in
	// implementing, as other declarations with errors are (architecture §2.1).
	void expect_implemented(const Expr &p_value, const Type &p_from, const Type &p_to) {
		if (!p_to.is_interface || p_from.is_interface || !is_script_path(p_from.script) || in_this_file(p_from.script)) {
			return;
		}
		const ClassDecl *decl = script_decl(p_from.script);
		if (!decl) {
			return;
		}
		Type interface = p_to;
		interface.nullable = false;
		for (const auto &[type, line] : decl->implemented_with_errors) {
			if (type == interface) {
				Type from = p_from;
				from.nullable = false;
				error(p_value.range, from.to_string() + " implements " + interface.to_string() + " with errors (" + script_file(p_from.script) + ", line " + std::to_string(line) + ").");
			}
		}
	}

	// @owner(T) and @parent(T): a class of this language, an interface or an
	// engine class, on a class that extends Node (spec §7.3).
	void declare_requirement(const std::optional<TypeRef> &p_ref, Type &r_type, const std::string &p_annotation) {
		if (!p_ref) {
			return;
		}
		r_type = resolve(*p_ref, false);
		if (r_type.is_error()) {
			return;
		}
		if (r_type.kind != TypeKind::Object || r_type.nullable) {
			error(p_ref->range, p_annotation + " takes a class or an interface, not " + r_type.to_string() + ".");
			r_type = Type::of(TypeKind::Error);
		} else if (!engine.inherits(cls.native_base, "Node")) {
			error(p_ref->range, p_annotation + " is for classes that extend Node.");
			r_type = Type::of(TypeKind::Error);
		}
	}

	// Each node of the scene whose check this is that has the class or a
	// subclass: attached, or as the root of a scene it instances.
	void for_each_placement(const std::function<void(const Scene &, const SceneNode &)> &p_check) {
		for (const Scene &scene : scenes) {
			for (const SceneNode &node : scene.nodes) {
				if (extends_script(node.script.empty() ? node.instance_script : node.script, options.path)) {
					p_check(scene, node);
				}
			}
		}
	}

	// That a node of a scene is what @owner(T) or @parent(T) requires; a class
	// of another language is left to the check at run time.
	void check_required(const TokenRange &p_at, const Scene &p_scene, const SceneNode &p_placed, const SceneNode &p_node, const Type &p_required, const std::string &p_what, const std::string &p_annotation) {
		const std::string &script = p_node.script.empty() ? p_node.instance_script : p_node.script;
		if (!script.empty() && !is_script_path(script) && !p_required.is_interface && !p_required.script.empty()) {
			return;
		}
		check_required_type(p_at, p_scene, p_placed, scene_node_type(p_at, p_node, p_scene.path + ": " + p_what), p_required, p_what, p_annotation);
	}

	void check_required_type(const TokenRange &p_at, const Scene &p_scene, const SceneNode &p_placed, const Type &p_found, const Type &p_required, const std::string &p_what, const std::string &p_annotation) {
		if (!p_found.is_error() && !assignable(p_found, p_required)) {
			error_in_scene(p_at, p_scene, p_placed.path, p_scene.path + ": " + p_what + " is " + p_found.to_string() + ", not " + p_required.to_string() + " (" + p_annotation + ").");
		}
	}

	// Where scenes place the script, its owner satisfies @owner(T) (spec §7.3):
	// that of a node is the root of the scene that stores it, and that of a
	// scene's root the root of each scene that instances it. The root of a
	// scene used as a root has none.
	void check_owner_scenes() {
		if (!cls.owner_ref || cls.owner_type.is_error()) {
			return;
		}
		for_each_placement([&](const Scene &p_scene, const SceneNode &p_node) {
			const ViewNode *placed = view_node(p_scene, p_node.path);
			const ViewNode *owner = placed && !placed->owner.empty() ? view_node(p_scene, placed->owner) : nullptr;
			const std::string use = placed && !owner ? classes.root_use(p_scene.path) : std::string();
			if (owner) {
				check_required(cls.owner_ref->range, p_scene, p_node, owner->node, cls.owner_type, "the owner of " + p_node.path, "@owner");
			} else if (!use.empty()) {
				const bool main = p_scene.path == options.project->main_scene();
				error_in_scene(cls.owner_ref->range, p_scene, p_node.path, p_scene.path + (main ? ": the main scene's root has no owner (@owner)." : ": the root has no owner where " + use + " (@owner)."));
			}
		});
	}

	// Where scenes place the script, its parent satisfies @parent(T) (spec
	// §7.3): the parent in the scene, that of a scene's root in each scene that
	// instances it, and the root viewport, a Window, for the main scene's root
	// (scene/main/scene_tree.cpp:1762-1765, 2068; ed1daf0). The root of a scene
	// used otherwise is checked at run time.
	void check_parent_scenes() {
		if (!cls.parent_ref || cls.parent_type.is_error()) {
			return;
		}
		const TokenRange &at = cls.parent_ref->range;
		for_each_placement([&](const Scene &p_scene, const SceneNode &p_node) {
			const NodeWalk walk = walk_node_path(p_scene, p_node.path, "..");
			if (walk.node) {
				check_required(at, p_scene, p_node, walk.node->node, cls.parent_type, "the parent of " + p_node.path, "@parent");
			} else if (walk.leaves && p_scene.path == options.project->main_scene()) {
				check_required_type(at, p_scene, p_node, Type::object("Window"), cls.parent_type, "the parent of the main scene's root", "@parent");
			}
		});
	}

	// The values that the scene stores for this class's exported structs and
	// enums with payloads, which the entry check decodes as the scene is
	// instantiated (spec §4.3), and for its exported scenes, which may leave
	// to instantiate(...) only what PackedScene[T] asks for (§7.4): on the
	// nodes with the class, and on the instances of scenes whose root has it.
	void check_stored_exports() {
		std::vector<const VarDecl *> exports;
		for (const VarDecl &member : cls.members) {
			const TypeKind kind = member.type.unwrapped().kind;
			if (member.is_export && (kind == TypeKind::Struct || kind == TypeKind::PayloadEnum || is_typed_scene(member.type))) {
				exports.push_back(&member);
			}
		}
		if (exports.empty()) {
			return;
		}
		for_each_placement([&](const Scene &p_scene, const SceneNode &p_node) {
			for (const VarDecl *member : exports) {
				for (const auto &[name, value] : p_node.properties) {
					if (name != member->name) {
						continue;
					}
					const TokenRange at{ member->name_token, member->name_token };
					if (is_typed_scene(member->type)) {
						const std::optional<Scene> scene = is_scene_file(value.path) ? options.project->scene(value.path) : std::nullopt;
						const std::string unfit = scene ? unfit_scene(*scene, member->type) : std::string();
						if (!unfit.empty()) {
							error_in_scene(at, p_scene, p_node.path, p_scene.path + ": \"" + name + "\" of " + p_node.path + " holds " + scene->path + ", which " + unfit);
						}
						continue;
					}
					const std::string why = undecodable(member->type, value, std::string());
					if (!why.empty()) {
						error_in_scene(at, p_scene, p_node.path, p_scene.path + ": the value stored for \"" + name + "\" of " + p_node.path + " does not decode: " + why + ".");
					}
				}
			}
		});
	}

	// Where the scene is used as a root, each node of its view with the class
	// gets a value for each of its exports that must be assigned, from any of
	// the scenes it is built from (spec §7.5); the root only in the main scene,
	// since whoever instantiates another may give its root the rest (§7.4). One
	// that nothing references only warns. The game runs all the same: a node
	// without one fails the check as it becomes ready.
	void check_scene_exports() {
		const Scene &scene = scenes[0];
		const bool main = scene.path == options.project->main_scene();
		if (!main && classes.root_use(scene.path).empty()) {
			return;
		}
		for (const ViewNode &node : view_of(scene)) {
			if ((!main && node.path == ".") || !extends_script(node.node.script.empty() ? node.node.instance_script : node.node.script, options.path)) {
				continue;
			}
			for (const VarDecl &member : cls.members) {
				if (member.is_export && member.must_assign && !stored_value(scene, node.path, member.name).value_or(false)) {
					error_in_scene(TokenRange{ member.name_token, member.name_token }, scene, node.path, scene.path + ": " + (node.path == "." ? std::string("the root") : node.path) + " has no value for \"" + member.name + "\", an export that must be assigned.");
					errors.back().runs = true;
				}
			}
		}
	}

	// Whether the node at `p_path` of a scene has a value other than null for
	// a member, as the last scene to set one gives it; nullopt if none does.
	// The engine sets an instanced or inherited scene's values first, then the
	// outer scene's (scene/resources/packed_scene.cpp:263-266, 494; ed1daf0).
	std::optional<bool> stored_value(const Scene &p_scene, const std::string &p_path, const std::string &p_name, int p_depth = 0) {
		const SceneNode *node = p_scene.find_node(p_path);
		for (const auto &[name, value] : node ? node->properties : std::vector<std::pair<std::string, SceneValue>>()) {
			if (name == p_name) {
				return value.type != "Nil";
			}
		}
		const std::optional<Scene> base = p_scene.base.empty() || p_depth > 32 ? std::nullopt : options.project->scene(p_scene.base);
		const std::optional<bool> inherited = base ? stored_value(*base, p_path, p_name, p_depth + 1) : std::nullopt;
		if (inherited) {
			return inherited;
		}
		// The innermost instance on the path.
		const SceneNode *instance = nullptr;
		for (const SceneNode &each : p_scene.nodes) {
			if (!each.instance.empty() && (each.path == p_path || p_path.rfind(each.path + "/", 0) == 0) && (!instance || each.path.size() > instance->path.size())) {
				instance = &each;
			}
		}
		const std::optional<Scene> inner = !instance || p_depth > 32 ? std::nullopt : options.project->scene(instance->instance);
		return inner ? stored_value(*inner, instance->path == p_path ? std::string(".") : p_path.substr(instance->path.size() + 1), p_name, p_depth + 1) : std::nullopt;
	}

	// Why a value that a scene stores does not decode as the type, as the
	// entry check says (spec §3.6, §10.1), after `p_at`, the path to where it
	// lies, such as "Combat.Hit.damage"; "" when it decodes.
	std::string undecodable(const Type &p_type, const SceneValue &p_value, const std::string &p_at) {
		const std::string at = p_at.empty() ? std::string() : p_at + ": ";
		const auto mismatch = [&]() {
			const std::string found = p_value.type == "Nil" ? std::string("null") : stored_type(p_value).to_string();
			return at + "expected " + p_type.to_string() + ", found " + found;
		};
		if (p_type.kind == TypeKind::Option && p_value.type != "Nil") {
			return undecodable(p_type.args[0], p_value, p_at);
		}
		if (p_type.kind == TypeKind::Struct || p_type.kind == TypeKind::PayloadEnum) {
			return p_value.type == "Dictionary" ? undecodable_fields(p_type, p_value, p_at.empty() ? p_type.to_string() : p_at) : mismatch();
		}
		// A container by its elements, as decoding builds a new one of them.
		if (is_typed_container(p_type)) {
			const bool array = p_type.args.size() == 1;
			if (p_value.type != (array ? "Array" : "Dictionary")) {
				return mismatch();
			}
			const std::string where = p_at.empty() ? p_type.to_string() : p_at;
			for (size_t i = 0; i < p_value.items.size(); i++) {
				const bool key = !array && i % 2 == 0;
				const std::string why = undecodable(p_type.args[key || array ? 0 : 1], p_value.items[i], where + (key ? " (a key)" : "[" + std::to_string(array ? i : i / 2) + "]"));
				if (!why.empty()) {
					return why;
				}
			}
			return std::string();
		}
		// An int also from a whole float, and an enum of this language only one
		// of its values (spec §3.7, §10.8).
		const bool integer = p_value.type == "int" || (p_value.type == "float" && p_value.whole);
		if (p_type.kind == TypeKind::Enum && integer) {
			const bool closed = p_type.values && !p_type.values->is_engine;
			return closed && !p_type.values->has(p_value.integer) ? at + "expected " + p_type.to_string() + ", found " + std::to_string(p_value.integer) + ", which is none of its values" : std::string();
		}
		if (p_type.kind == TypeKind::Int && integer) {
			return std::string();
		}
		return assignable(stored_type(p_value), p_type) ? std::string() : mismatch();
	}

	// The fields of a stored struct, or of the variant that "$variant" names,
	// each by its name as a key; a struct's field with a default may be left
	// out (spec §3.6, §10.1).
	std::string undecodable_fields(const Type &p_type, const SceneValue &p_value, const std::string &p_at) {
		const auto entry = [&](const std::string &p_key) -> const SceneValue * {
			for (size_t i = 0; i + 1 < p_value.items.size(); i += 2) {
				const SceneValue &key = p_value.items[i];
				if ((key.type == "String" || key.type == "StringName") && key.text == p_key) {
					return &p_value.items[i + 1];
				}
			}
			return nullptr;
		};
		std::vector<std::pair<std::string, Type>> fields;
		std::vector<bool> defaults;
		std::string at = p_at;
		if (p_type.kind == TypeKind::Struct) {
			const StructDecl *decl = struct_decl(p_type);
			for (size_t f = 0; decl && f < decl->fields.size(); f++) {
				fields.emplace_back(decl->fields[f].name, decl->fields[f].type);
				defaults.push_back(bool(decl->fields[f].initializer));
			}
		} else if (p_type.values) {
			const SceneValue *name = entry("$variant");
			const auto &variants = p_type.values->list;
			const auto named = std::find_if(variants.begin(), variants.end(), [&](const auto &p_variant) { return name && (name->type == "String" || name->type == "StringName") && name->text == p_variant.first; });
			if (named == variants.end()) {
				return p_at + ": expected " + p_type.to_string() + ", found a Dictionary whose \"$variant\" names none of its variants";
			}
			at += "." + named->first;
			fields = p_type.values->fields[named - variants.begin()];
			defaults.resize(fields.size(), false);
		}
		for (size_t f = 0; f < fields.size(); f++) {
			const SceneValue *value = entry(fields[f].first);
			const std::string why = value ? undecodable(fields[f].second, *value, at + "." + fields[f].first) : std::string();
			if (!value && !defaults[f]) {
				return at + "." + fields[f].first + " is missing";
			}
			if (!why.empty()) {
				return why;
			}
		}
		return std::string();
	}

	// The type of a value that a scene stores: an object of the class of its
	// script, as a node's (spec §7.1, §11.1).
	Type stored_type(const SceneValue &p_value) {
		if (p_value.type == "Nil") {
			return Type::of(TypeKind::None);
		}
		if (p_value.type != "Object") {
			return api_type(p_value.type, false);
		}
		if (is_script_path(p_value.script)) {
			const ClassDecl *header = script_header(p_value.script);
			return header ? script_class_type(*header, p_value.script) : Type::object(p_value.class_name);
		}
		const ForeignClass *foreign = p_value.script.empty() ? nullptr : foreign_class(p_value.script);
		return foreign && foreign->members_known && !foreign->native.empty() ? foreign_class_type(p_value.script, *foreign) : Type::object(p_value.class_name);
	}

	// The part of a type that is not seen everywhere: a struct, an enum, an
	// interface or an inner class without pub, or one inside a class without
	// it (spec §4.2); nullopt when there is none.
	std::optional<Type> hidden_part(const Type &p_type) const {
		for (const Type &arg : p_type.args) {
			if (std::optional<Type> part = hidden_part(arg)) {
				return part;
			}
		}
		if (p_type.values && p_type.values->is_engine) {
			return std::nullopt;
		}
		const ClassDecl *header = script_header(p_type.script);
		const std::string name = p_type.interface_name();
		bool pub = true;
		if (header && p_type.kind == TypeKind::Struct) {
			const StructDecl *decl = header->find_struct(name);
			pub = !decl || decl->is_pub;
		} else if (header && (p_type.kind == TypeKind::Enum || p_type.kind == TypeKind::PayloadEnum)) {
			const EnumDecl *decl = header->find_enum(name);
			pub = !decl || decl->is_pub;
		} else if (header && p_type.is_interface) {
			const auto decl = std::find_if(header->interfaces.begin(), header->interfaces.end(), [&](const InterfaceDecl &p_decl) { return p_decl.name == name; });
			pub = decl == header->interfaces.end() || decl->is_pub;
		}
		for (std::string path = p_type.script; pub && path.find("::") != std::string::npos; path = path.substr(0, path.rfind("::"))) {
			const ClassDecl *inner = script_header(path);
			pub = !inner || inner->is_pub;
		}
		Type shown = p_type;
		shown.nullable = false;
		return pub ? std::nullopt : std::optional<Type>(shown);
	}

	// A pub declaration shows the types of its signature wherever it is seen,
	// so each of them must be seen there too (spec §4.2).
	void check_pub_signatures() {
		const auto check = [&](bool p_pub, uint32_t p_token, const std::string &p_what, const std::vector<Type> &p_types) {
			for (const Type &type : p_types) {
				const std::optional<Type> part = p_pub ? hidden_part(type) : std::nullopt;
				if (part) {
					error(p_token, part->to_string() + " is not pub, but " + p_what + " shows it.");
					return;
				}
			}
		};
		const auto signature = [](const FuncDecl &p_function) {
			std::vector<Type> types = { p_function.return_type };
			for (const ParamDecl &param : p_function.params) {
				types.push_back(param.type);
			}
			return types;
		};
		for (const std::vector<VarDecl> *variables : { &cls.members, &cls.static_vars }) {
			for (const VarDecl &variable : *variables) {
				check(variable.is_pub, variable.name_token, "the pub \"" + variable.name + "\"", { variable.type });
			}
		}
		for (const ConstDecl &constant : cls.constants) {
			check(constant.is_pub, constant.name_token, "the pub \"" + constant.name + "\"", { constant.type });
		}
		for (const FuncDecl &function : cls.functions) {
			check(function.is_pub, function.name_token, "the pub \"" + function.name + "()\"", signature(function));
		}
		for (const SignalDecl &signal : cls.signals) {
			check(signal.is_pub, signal.name_token, "the pub signal \"" + signal.name + "\"", param_types(signal.params));
		}
		for (const StructDecl &decl : cls.structs) {
			for (const VarDecl &field : decl.fields) {
				check(decl.is_pub, field.name_token, "the field \"" + field.name + "\" of the pub " + decl.name, { field.type });
			}
			for (const FuncDecl &method : decl.methods) {
				check(decl.is_pub, method.name_token, "the method \"" + method.name.substr(decl.name.size() + 1) + "()\" of the pub " + decl.name, signature(method));
			}
		}
		for (const EnumDecl &decl : cls.enums) {
			for (size_t v = 0; decl.has_payloads && decl.type.values && v < decl.values.size(); v++) {
				std::vector<Type> fields;
				for (const auto &[name, type] : decl.type.values->fields[v]) {
					fields.push_back(type);
				}
				check(decl.is_pub, decl.values[v].name_token, "the variant " + decl.values[v].name + " of the pub " + decl.name, fields);
			}
		}
		for (const InterfaceDecl &decl : cls.interfaces) {
			for (const FuncDecl &method : decl.methods) {
				check(decl.is_pub, method.name_token, "the method \"" + method.name + "()\" of the pub " + decl.name, signature(method));
			}
			for (const InterfaceProperty &property : decl.properties) {
				check(decl.is_pub, property.name_token, "the property \"" + property.name + "\" of the pub " + decl.name, { property.type });
			}
			for (const SignalDecl &signal : decl.signals) {
				check(decl.is_pub, signal.name_token, "the signal \"" + signal.name + "\" of the pub " + decl.name, param_types(signal.params));
			}
		}
	}

	// Types whose None the engine would lose where it sees the value (spec
	// §10.1): of pub members, constants and functions, of signals, which
	// always pass through the engine, and what coroutines return (§9.8).
	void check_engine_options() {
		const auto check = [&](uint32_t p_token, const std::string &p_what, const Type &p_type) {
			const std::string loss = encoding_loss(p_type, "the engine", false);
			if (!loss.empty()) {
				error(p_token, p_what + " " + p_type.to_string() + ", but " + loss);
			}
		};
		for (const std::vector<VarDecl> *variables : { &cls.members, &cls.static_vars }) {
			for (const VarDecl &variable : *variables) {
				if (variable.is_pub) {
					check(variable.name_token, "The pub \"" + variable.name + "\" has type", variable.type);
				}
			}
		}
		for (const ConstDecl &constant : cls.constants) {
			if (constant.is_pub) {
				check(constant.name_token, "The pub \"" + constant.name + "\" has type", constant.type);
			}
		}
		for (const FuncDecl &function : cls.functions) {
			if (function.is_pub || function.is_async) {
				check(function.name_token, "\"" + function.name + "()\" returns", function.return_type);
			}
			for (size_t i = 0; function.is_pub && i < function.params.size(); i++) {
				check(function.params[i].name_token, "The parameter \"" + function.params[i].name + "\" of the pub \"" + function.name + "()\" has type", function.params[i].type);
			}
		}
		for (const SignalDecl &signal : cls.signals) {
			for (const ParamDecl &param : signal.params) {
				check(param.name_token, "The parameter \"" + param.name + "\" of the signal \"" + signal.name + "\" has type", param.type);
			}
		}
	}

	// The properties and signals the class's interfaces require, after the
	// members' types and the signals are known (spec §3.9): a pub member of the
	// same type, or an inherited engine property, which must have a setter
	// unless the requirement is @readonly; a pub signal with the same arguments,
	// its own, inherited or of the engine class.
	void check_interface_properties() {
		for (const auto &[type, ref] : own_interfaces) {
			const size_t reported = errors.size();
			for (const InterfaceMember &member : interface_members({ type })) {
				if (member.signal) {
					check_interface_signal(member, *ref);
				}
				if (!member.property || member.property->type.is_error()) {
					continue;
				}
				const InterfaceProperty &required = *member.property;
				const std::string &owner = member.owner.name;
				const auto found = member_index.find(required.name);
				if (found != member_index.end()) {
					const VarDecl &provided = *cls.all_members[found->second];
					if (is_own(provided, cls.members)) {
						refer(provided.name_token, script_id(options.path, provided.name), SymbolKind::Property, interface_member_id(member));
					}
					if (!provided.is_pub) {
						error(own_name(provided, cls.members, ref->range), "\"" + provided.name + "\" implements " + owner + " and must be pub.");
					} else if (!provided.type.is_error() && provided.type != required.type) {
						error(own_name(provided, cls.members, ref->range), "\"" + provided.name + "\" has type " + provided.type.to_string() + ", but " + owner + " requires " + required.type.to_string() + ".");
					}
					continue;
				}
				const auto property = engine.find_property(cls.native_base, required.name);
				if (!property) {
					error(ref->range, "The class does not implement the property \"" + required.name + "\" of " + owner + ".");
				} else if (property->type != required.type) {
					error(ref->range, "The engine property \"" + required.name + "\" has type " + property->type.to_string() + ", but " + owner + " requires " + required.type.to_string() + ".");
				} else if (!property->writable && !required.is_readonly) {
					error(ref->range, "The engine property \"" + required.name + "\" is read-only, so it satisfies only a @readonly property of " + owner + ".");
				}
			}
			note_implementation(type, *ref, reported);
		}
	}

	void check_interface_signal(const InterfaceMember &p_member, const TypeRef &p_ref) {
		const SignalDecl &required = *p_member.signal;
		const std::vector<Type> wanted = param_types(required.params);
		if (std::any_of(wanted.begin(), wanted.end(), [](const Type &p_type) { return p_type.is_error(); })) {
			return;
		}
		const std::string &owner = p_member.owner.name;
		const auto found = signal_index.find(required.name);
		if (found != signal_index.end()) {
			const SignalDecl &provided = *cls.all_signals[found->second];
			if (is_own(provided, cls.signals)) {
				refer(provided.name_token, script_id(options.path, provided.name), SymbolKind::Signal, interface_member_id(p_member));
			}
			const std::vector<Type> given = param_types(provided.params);
			if (!provided.is_pub) {
				error(own_name(provided, cls.signals, p_ref.range), "\"" + provided.name + "\" implements " + owner + " and must be pub.");
			} else if (std::none_of(given.begin(), given.end(), [](const Type &p_type) { return p_type.is_error(); }) && given != wanted) {
				error(own_name(provided, cls.signals, p_ref.range), "\"" + provided.name + "\" is " + Type::signal(given).to_string() + ", but " + owner + " requires " + Type::signal(wanted).to_string() + ".");
			}
			return;
		}
		const auto signal = engine.find_signal(cls.native_base, required.name);
		if (!signal) {
			error(p_ref.range, "The class does not implement the signal \"" + required.name + "\" of " + owner + ".");
		} else if (signal_type(*signal).args != wanted) {
			error(p_ref.range, "The engine signal \"" + required.name + "\" is " + signal_type(*signal).to_string() + ", but " + owner + " requires " + Type::signal(wanted).to_string() + ".");
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
			refer(signal.name_token, script_id(options.path, signal.name), SymbolKind::Signal);
			type_signal_params(signal);
		}
	}

	void type_signal_params(SignalDecl &r_signal) {
		for (ParamDecl &param : r_signal.params) {
			if (!param.declared_type) {
				error(param.name_token, "The argument \"" + param.name + "\" of a signal needs a type.");
				param.type = Type::of(TypeKind::Error);
				continue;
			}
			if (param.default_value) {
				error(param.default_value->range, "The arguments of a signal have no default values.");
			}
			param.type = resolve(*param.declared_type, false);
		}
	}

	void check_body(FuncDecl &r_function) {
		function = &r_function;
		scopes.clear();
		scopes.emplace_back();
		narrowed.clear();
		narrowed.emplace_back();
		if (r_function.of_struct && !r_function.is_static) {
			scopes.back()["self"] = 0;
			r_function.locals.push_back({ "self", structure_type, r_function.name_token });
		}
		declare_params(r_function, true);
		const size_t cursor = completion ? cls.source->text.find(CURSOR) : 0;
		if (options.declarations_only || !check_bodies || (completion && (cursor < cls.source->begin(r_function.range) || cursor >= cls.source->end(r_function.range)))) {
			function = nullptr;
			scopes.clear();
			return;
		}
		check_block(r_function.body);
		if (r_function.has_body && r_function.return_type.kind != TypeKind::Void && !r_function.return_type.is_error() && !always_ends(r_function.body, false)) {
			error(r_function.name_token, "Not all paths of \"" + r_function.name + "()\" return a value.");
		}
		if (r_function.name == "_init" && !r_function.is_static) {
			std::set<int> reported;
			const EarlyReads reads{ [&](int p_member) { return cls.all_members[p_member]->must_assign; },
				[&](const Expr &p_read) { error(p_read.range, "_init reads \"" + p_read.text + "\" before assigning it: a member that must be assigned gets its value from the scene or the creator only after _init."); } };
			init_assigned = init_block(r_function.body, std::set<int>(), reported, reads).value_or(std::set<int>());
		}
		if (r_function.name == "_ready" && !r_function.is_static && !r_function.of_struct) {
			ready = &r_function;
		}
		function = nullptr;
		scopes.clear();
	}

	/* _init and members that must be assigned (spec §6.2) */

	// What a walk of _init or _ready reports: a read of a member it tracks
	// before the function assigns it. _ready's walk ends at an await.
	struct EarlyReads {
		std::function<bool(int)> tracked;
		std::function<void(const Expr &)> report;
		bool ends_at_await = false;
	};

	// The node's members that ancestors assign, once they are ready (spec
	// §6.2, §6.4): where a scene gives the node an owner, those that must be
	// assigned and are not exported, unless _init assigns them. Its own
	// @onready initializers, which run first, may not read them, nor its
	// _ready before it awaits. A node has an owner unless it is a scene's
	// root, which gets one where another scene instances that scene.
	void check_ready_reads() {
		const bool initializes = std::any_of(cls.members.begin(), cls.members.end(), [](const VarDecl &p_member) { return p_member.is_onready && p_member.initializer; });
		if (!ready && !initializes) {
			return;
		}
		const auto check_at = [&](const Scene &p_scene, const std::string &p_node) {
			std::set<int> reported;
			bool initializer = true;
			const EarlyReads reads{ [&](int p_member) { return cls.all_members[p_member]->must_assign && !cls.all_members[p_member]->is_export && !init_assigned.count(p_member); },
				[&](const Expr &p_read) {
					const std::string where = initializer ? "in an @onready initializer" : "in _ready";
					const std::string fix = initializer ? "in _ready after" : "after";
					error_in_scene(p_read.range, p_scene, p_node, p_scene.path + ": " + p_node + " reads \"" + p_read.text + "\" " + where + ", before the ancestors that assign it are ready. Read it " + fix + " await get_parent().ready, or let the parent's _ready configure the node.");
				},
				true };
			for (const VarDecl &member : cls.members) {
				if (member.is_onready && member.initializer) {
					init_reads(*member.initializer, std::set<int>(), reported, reads);
				}
			}
			initializer = false;
			if (ready) {
				init_block(ready->body, std::set<int>(), reported, reads);
			}
		};
		for (const Scene &scene : scenes) {
			for (const SceneNode &node : scene.nodes) {
				if (!extends_script(node.script, options.path)) {
					continue;
				}
				if (node.path != ".") {
					check_at(scene, node.path);
					continue;
				}
				for (const Scene &outer : classes.scenes_instancing(scene.path)) {
					for (const SceneNode &instance : outer.nodes) {
						if (instance.instance == scene.path && instance.script.empty()) {
							check_at(outer, instance.path);
						}
					}
				}
			}
		}
	}

	// Whether evaluating an expression may suspend, outside its lambdas.
	static bool awaits(const Expr &p_expr) {
		bool found = p_expr.kind == ExprKind::Await;
		for_each_child(p_expr, [&](const auto &p_child) {
			if constexpr (std::is_same_v<std::decay_t<decltype(p_child)>, Expr>) {
				found = found || awaits(p_child);
			}
		});
		return found && p_expr.kind != ExprKind::Lambda;
	}

	// The members that must be assigned which _init has assigned on every path
	// so far; nullopt where no path goes on.
	using Assigned = std::optional<std::set<int>>;

	static Assigned joined(const Assigned &p_a, const Assigned &p_b) {
		if (!p_a || !p_b) {
			return p_a ? p_a : p_b;
		}
		std::set<int> both;
		std::set_intersection(p_a->begin(), p_a->end(), p_b->begin(), p_b->end(), std::inserter(both, both.begin()));
		return both;
	}

	Assigned init_block(const Block &p_block, Assigned p_assigned, std::set<int> &r_reported, const EarlyReads &p_reads) {
		for (const auto &stmt : p_block) {
			p_assigned = init_stmt(*stmt, std::move(p_assigned), r_reported, p_reads);
		}
		return p_assigned;
	}

	// The scene and the creator assign only after _init, so _init reading one
	// it has not assigned itself is an error. A read through a call is left to
	// the program error: methods may be overridden in other files.
	Assigned init_stmt(const Stmt &p_stmt, Assigned p_assigned, std::set<int> &r_reported, const EarlyReads &p_reads) {
		const auto suspends = [](const std::unique_ptr<Expr> &p_expr) { return p_expr && awaits(*p_expr); };
		if (!p_assigned || (p_reads.ends_at_await && (suspends(p_stmt.target) || suspends(p_stmt.value) || suspends(p_stmt.condition)))) {
			return std::nullopt;
		}
		const auto reads = [&](const std::unique_ptr<Expr> &p_expr) {
			if (p_expr) {
				init_reads(*p_expr, *p_assigned, r_reported, p_reads);
			}
		};
		switch (p_stmt.kind) {
			case StmtKind::Assign: {
				const Expr &target = *p_stmt.target;
				const bool whole = p_stmt.assign_op == TokenKind::Equal && target.target == Target::Member && (target.kind == ExprKind::Name || target.base->kind == ExprKind::Self);
				if (!whole) {
					reads(p_stmt.target);
				}
				reads(p_stmt.value);
				if (whole) {
					p_assigned->insert(target.index);
				}
				return p_assigned;
			}
			case StmtKind::If:
				reads(p_stmt.condition);
				return joined(init_block(p_stmt.body, p_assigned, r_reported, p_reads), init_block(p_stmt.else_body, p_assigned, r_reported, p_reads));
			case StmtKind::While:
			case StmtKind::For:
				reads(p_stmt.condition);
				reads(p_stmt.value);
				init_block(p_stmt.body, p_assigned, r_reported, p_reads);
				return p_assigned;
			case StmtKind::Match: {
				reads(p_stmt.value);
				Assigned after = p_stmt.exhaustive ? Assigned() : p_assigned;
				for (const MatchBranch &branch : p_stmt.branches) {
					reads(branch.guard);
					after = joined(after, init_block(branch.body, p_assigned, r_reported, p_reads));
				}
				return after;
			}
			case StmtKind::Return:
				reads(p_stmt.value);
				return std::nullopt;
			case StmtKind::Break:
			case StmtKind::Continue:
				return std::nullopt;
			default:
				reads(p_stmt.condition);
				reads(p_stmt.value);
				return p_assigned;
		}
	}

	void init_reads(const Expr &p_expr, const std::set<int> &p_assigned, std::set<int> &r_reported, const EarlyReads &p_reads) {
		if (p_expr.kind == ExprKind::Lambda) {
			return; // It runs later.
		}
		if (p_expr.target == Target::Member && p_expr.index >= 0 && p_reads.tracked(p_expr.index) && !p_assigned.count(p_expr.index) && r_reported.insert(p_expr.index).second) {
			p_reads.report(p_expr);
		}
		for_each_child(p_expr, [&](const auto &p_child) {
			if constexpr (std::is_same_v<std::decay_t<decltype(p_child)>, Expr>) {
				init_reads(p_child, p_assigned, r_reported, p_reads);
			}
		});
	}

	// Every function's name, before any type is resolved, which may need a
	// class of this file that uses them (spec §4.1).
	void declare_functions() {
		std::unordered_set<std::string> own_functions;
		for (FuncDecl &fn : cls.functions) {
			if (!own_functions.insert(fn.name).second || member_index.count(fn.name) || static_index.count(fn.name) || constant_index.count(fn.name) || signal_index.count(fn.name)) {
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
		}
	}

	// Whether the function is the one its name was declared for.
	bool is_declared(const FuncDecl &p_function) const {
		const auto found = function_index.find(p_function.name);
		return found != function_index.end() && cls.all_functions[found->second] == &p_function;
	}

	/* Completion (spec §13.2) */

	// Names that may be written somewhere, as types or as values: more than
	// can, since each is tried where completion asks.
	struct Candidates {
		std::set<std::string> types;
		std::set<std::string> values;
	};

	void engine_candidates(const std::string &p_class, Candidates &r_names) const {
		for (const std::string &name : engine.member_names(p_class)) {
			r_names.values.insert(name);
			if (const std::string tried = builtin::try_version(name); builtin::error_getter(p_class, tried)) {
				r_names.values.insert(tried);
			}
		}
		for (const std::string &name : engine.enum_names(p_class)) {
			r_names.types.insert(name);
		}
	}

	// What a class has, also through its bases: what code names in it, through
	// it or through its objects.
	void class_candidates(const ClassDecl &p_decl, Candidates &r_names) const {
		const auto add = [&](const auto &p_declarations) {
			for (const auto *declaration : p_declarations) {
				r_names.values.insert(declaration->name);
			}
		};
		add(p_decl.all_members);
		add(p_decl.all_static_vars);
		add(p_decl.all_constants);
		add(p_decl.all_functions);
		add(p_decl.all_signals);
		for (const ClassDecl *decl = &p_decl; decl; decl = decl->base) {
			for (const std::unique_ptr<ClassDecl> &inner : decl->classes) {
				r_names.types.insert(inner->name);
				r_names.values.insert(inner->name);
			}
			for (const StructDecl &found : decl->structs) {
				r_names.types.insert(found.name);
				r_names.values.insert(found.name);
			}
			for (const InterfaceDecl &found : decl->interfaces) {
				r_names.types.insert(found.name);
			}
			for (const EnumDecl &found : decl->enums) {
				if (!found.name.empty()) {
					r_names.types.insert(found.name);
					r_names.values.insert(found.name);
				}
				for (const EnumValueDecl &value : found.values) {
					r_names.values.insert(value.name);
				}
			}
		}
		engine_candidates(p_decl.native_base, r_names);
	}

	// What `Class.` and `object.` reach of a class of this language, of
	// another or of the engine.
	void script_candidates(const Type &p_class, Candidates &r_names) {
		if (p_class.is_interface) {
			for (const InterfaceMember &member : interface_members({ p_class })) {
				r_names.values.insert(member.name());
			}
		} else if (is_script_path(p_class.script)) {
			if (const ClassDecl *decl = script_decl(p_class.script)) {
				class_candidates(*decl, r_names);
			}
		} else if (const ForeignClass *foreign = p_class.script.empty() ? nullptr : foreign_class(p_class.script)) {
			for (const std::vector<EngineProperty> *properties : { &foreign->properties, &foreign->constants }) {
				for (const EngineProperty &property : *properties) {
					r_names.values.insert(property.name);
				}
			}
			for (const std::vector<EngineMethod> *methods : { &foreign->methods, &foreign->signals }) {
				for (const EngineMethod &method : *methods) {
					r_names.values.insert(method.name);
				}
			}
		}
		engine_candidates(p_class.engine_class(), r_names);
	}

	static void builtin_candidates(const builtin::Class &p_type, Candidates &r_names) {
		for (uint32_t i = 0; i < p_type.member_count; i++) {
			r_names.values.insert(p_type.members[i].name);
		}
		for (uint32_t i = 0; i < p_type.constant_count; i++) {
			r_names.values.insert(p_type.constants[i].name);
		}
		for (uint32_t i = 0; i < p_type.method_count; i++) {
			r_names.values.insert(p_type.methods[i].name);
		}
	}

	// What may follow `value.` for a value of the type.
	void value_candidates(const Type &p_type, Candidates &r_names) {
		if (p_type.is_optional() || p_type.is_result()) {
			const std::unordered_set<std::string> &methods = p_type.is_result() ? result_methods() : option_methods();
			r_names.values.insert(methods.begin(), methods.end());
			return;
		}
		if (p_type.kind == TypeKind::Task) {
			r_names.values.insert({ "cancel", "detach", "settled" });
		}
		if (p_type.kind == TypeKind::Struct) {
			if (const StructDecl *decl = struct_decl(p_type)) {
				struct_candidates(*decl, r_names);
			}
		}
		if (p_type.kind == TypeKind::Object) {
			script_candidates(p_type, r_names);
		}
		if (const builtin::Class *type = builtin::find_class(p_type.builtin_name())) {
			builtin_candidates(*type, r_names);
		}
	}

	static void struct_candidates(const StructDecl &p_decl, Candidates &r_names) {
		for (const VarDecl &field : p_decl.fields) {
			r_names.values.insert(field.name);
		}
		for (const FuncDecl &method : p_decl.methods) {
			r_names.values.insert(method.name.substr(p_decl.name.size() + 1));
		}
	}

	// The names code in this class sees bare: the locals, those of the classes
	// around it, the project's and the engine's.
	void scope_candidates(Candidates &r_names) const {
		const auto locals = [&](const std::vector<std::unordered_map<std::string, int>> &p_scopes) {
			for (const auto &scope : p_scopes) {
				for (const auto &local : scope) {
					r_names.values.insert(local.first);
				}
			}
		};
		locals(scopes);
		for (const Enclosing &outer : enclosing) {
			locals(outer.scopes);
		}
		if (structure) {
			struct_candidates(*structure, r_names);
		}
		for (const Checker *around = this; around; around = around->outer_checker) {
			class_candidates(around->cls, r_names);
			if (!around->cls.class_name.empty()) {
				r_names.types.insert(around->cls.class_name);
			}
		}
		for (const std::string &name : engine.class_list()) {
			r_names.types.insert(name);
		}
		std::vector<std::string> types;
		std::vector<std::string> values;
		builtin::global_names(types, values);
		r_names.types.insert(types.begin(), types.end());
		r_names.values.insert(values.begin(), values.end());
		if (options.project) {
			for (const std::string &name : options.project->autoload_names()) {
				r_names.values.insert(name);
			}
		}
	}

	// The project's global classes, or with `p_extendable` those of this
	// language other than this class, by the names the project lists them by:
	// not tried, since a name in that list can only fail to resolve when its
	// script cannot be read, and trying reads each script.
	void add_global_classes(std::map<std::string, CompletionItem> &r_found, bool p_extendable) const {
		if (!options.project) {
			return;
		}
		for (const auto &[name, path] : options.project->global_class_paths()) {
			if (p_extendable && (!is_script_path(path) || path == options.path)) {
				continue;
			}
			CompletionItem item;
			item.kind = CompletionKind::Class;
			item.name = name;
			item.insert = name;
			item.place = CompletionItem::Place::Project;
			r_found.emplace(name, item);
		}
	}

	void set_completions(std::map<std::string, CompletionItem> &r_found) {
		completion->items.clear();
		for (auto &entry : r_found) {
			completion->items.push_back(std::move(entry.second));
		}
	}

	// Gives completion the signature of `p_call` when the cursor is among its
	// arguments and not among those of a call within them. `p_signature` makes
	// it with the parameter at the cursor marked.
	void hint(const Expr &p_call, const std::function<std::string(int)> &p_signature) {
		if (!completion || probing || p_call.name_token >= cls.source->tokens.size()) {
			return;
		}
		const size_t cursor = cls.source->text.find(CURSOR);
		const uint32_t name_end = cls.source->tokens[p_call.name_token].end();
		if (cursor < name_end || cursor >= cls.source->end(p_call.range) || name_end < hint_at) {
			return;
		}
		int current = 0;
		for (const auto &arg : p_call.args) {
			current += cls.source->end(arg->range) < cursor ? 1 : 0;
		}
		hint_at = name_end;
		completion->call_hint = p_signature(current);
	}

	// `name(a: T, b: U = 1) -> R`, the parameter at `p_current` between two
	// U+FFFF; with `p_vararg`, a last `...args` takes the rest.
	static std::string signature(const std::string &p_name, std::vector<std::string> p_params, bool p_vararg, int p_current, const std::string &p_result) {
		if (p_vararg) {
			p_params.push_back("...args");
			p_current = std::min(p_current, int(p_params.size()) - 1);
		}
		std::string text = p_name + "(";
		for (size_t i = 0; i < p_params.size(); i++) {
			const std::string mark = int(i) == p_current ? CURSOR : "";
			text += (i > 0 ? ", " : "") + mark + p_params[i] + mark;
		}
		return text + ") -> " + p_result;
	}

	// The parameters of a function value or a signal, which have no names.
	static std::vector<std::string> type_names(const std::vector<Type> &p_types) {
		std::vector<std::string> names;
		for (const Type &type : p_types) {
			names.push_back(type.to_string());
		}
		return names;
	}

	// A function of this language's, its defaults as written in `p_source`.
	static std::string declared_signature(const std::string &p_name, const FuncDecl &p_function, const SourceFile &p_source, int p_current, const Type &p_result) {
		std::vector<std::string> params;
		for (const ParamDecl &param : p_function.params) {
			params.push_back((param.is_rest ? "..." : "") + param.name + ": " + param.type.to_string());
			if (param.default_value) {
				const uint32_t begin = p_source.begin(param.default_value->range);
				params.back() += " = " + p_source.text.substr(begin, p_source.end(param.default_value->range) - begin);
			}
		}
		// A rest parameter takes the arguments after the others.
		if (p_function.has_rest()) {
			p_current = std::min(p_current, int(params.size()) - 1);
		}
		return signature(p_name, params, false, p_current, p_result.to_string());
	}

	// A function of the engine's API (builtin_api.h).
	static std::string api_signature(const std::string &p_name, const builtin::Argument *p_args, uint32_t p_count, bool p_vararg, const char *p_result, int p_current) {
		std::vector<std::string> params;
		for (uint32_t i = 0; i < p_count; i++) {
			params.push_back(std::string(p_args[i].name) + ": " + api_type(p_args[i].type, true).to_string());
			if (p_args[i].has_default) {
				params.back() += std::string(" = ") + p_args[i].default_value;
			}
		}
		return signature(p_name, params, p_vararg, p_current, *p_result ? api_type(p_result, true).to_string() : "void");
	}

	// The classes a member may be declared in, nearest first: one of this
	// language and its bases, then the engine's; or a built-in type.
	std::vector<std::string> class_chain(const Type &p_class) {
		std::vector<std::string> chain;
		for (std::string path = p_class.script; is_script_path(path) && std::find(chain.begin(), chain.end(), path) == chain.end();) {
			chain.push_back(path);
			const ClassDecl *header = script_header(path);
			path = header ? classes.base_path(*header, path) : std::string();
		}
		if (p_class.kind == TypeKind::Object) {
			for (std::string name = p_class.engine_class(); engine.class_exists(name); name = engine.parent_class(name)) {
				chain.push_back(name);
			}
		} else if (!p_class.builtin_name().empty()) {
			chain.push_back(p_class.builtin_name());
		}
		return chain;
	}

	static CompletionKind value_kind(const Expr &p_checked) {
		switch (p_checked.target) {
			case Target::Local:
				return CompletionKind::Variable;
			case Target::MethodValue:
			case Target::EngineMethodValue:
			case Target::StaticValue:
			case Target::EngineStaticValue:
			case Target::StructMethodValue:
			case Target::NewValue:
				return CompletionKind::Function;
			case Target::SelfSignal:
			case Target::ObjectSignal:
				return CompletionKind::Signal;
			case Target::ClassScript:
			case Target::Singleton:
				return CompletionKind::Class;
			case Target::Constant:
				return p_checked.constant && p_checked.constant->names_enum ? CompletionKind::Enum : CompletionKind::Constant;
			case Target::EngineConstant:
			case Target::BuiltinConstant:
			case Target::Autoload:
				return CompletionKind::Constant;
			default:
				return CompletionKind::Member;
		}
	}

	// As what `p_name` checks in place of the name of `r_site`, the expression
	// at the cursor: as a value, else as a call, whose arguments are not
	// checked; nullopt if it does not. `p_chain` is class_chain() of the class
	// whose members are completed.
	std::optional<CompletionItem> probe(Expr &r_site, const std::string &p_name, const std::vector<std::string> &p_chain) {
		const bool on_self = r_site.base && r_site.base->kind == ExprKind::Self;
		const bool moves_base = r_site.base && !on_self;
		const uint32_t at = cls.source->tokens[r_site.name_token].offset;
		for (const bool call : { false, true }) {
			Expr probe;
			probe.kind = call ? ExprKind::Call : (r_site.base ? ExprKind::Member : ExprKind::Name);
			probe.text = p_name;
			probe.range = r_site.range;
			probe.name_token = r_site.name_token;
			if (on_self) {
				// A call drops it, as `self.f()` is `f()`.
				probe.base = std::make_unique<Expr>();
				probe.base->kind = ExprKind::Self;
				probe.base->range = r_site.base->range;
			} else if (moves_base) {
				probe.base = std::move(r_site.base);
			}
			const size_t error_count = errors.size();
			const size_t reference_count = references->size();
			probing = true;
			probed_params = -1;
			const Type type = type_expr(probe, Type::of(TypeKind::Error));
			probing = false;
			const bool accepted = errors.size() == error_count && !type.is_error();
			const auto named = std::find_if(references->begin() + reference_count, references->end(), [&](const Reference &p_reference) {
				return p_reference.begin == at;
			});
			const std::optional<DeclId> target = named == references->end() ? std::nullopt : std::optional<DeclId>(named->target);
			errors.erase(errors.begin() + error_count, errors.end());
			references->erase(references->begin() + reference_count, references->end());
			if (moves_base) {
				r_site.base = std::move(probe.base);
			}
			if (!accepted) {
				continue;
			}
			CompletionItem item;
			item.name = p_name;
			item.insert = p_name;
			item.kind = call ? CompletionKind::Function : value_kind(probe);
			if (item.kind == CompletionKind::Function) {
				const int params = call ? probed_params : int(type.args.size()) - 1;
				item.insert += params == 0 ? "()" : "(";
			}
			const bool declared = target && (target->kind == DeclId::Kind::Script || target->kind == DeclId::Kind::Engine);
			const auto in_chain = declared ? std::find(p_chain.begin(), p_chain.end(), target->scope) : p_chain.end();
			if (probe.target == Target::Local) {
				item.place = CompletionItem::Place::Local;
			} else if (in_chain != p_chain.end()) {
				item.place = CompletionItem::Place::Member;
				item.depth = int(in_chain - p_chain.begin());
			} else if (target && (target->kind == DeclId::Kind::Script || target->kind == DeclId::Kind::Foreign || target->kind == DeclId::Kind::Autoload)) {
				item.place = CompletionItem::Place::Project;
			}
			return item;
		}
		return std::nullopt;
	}

	// As what type `p_name` resolves where `p_site` is, at the cursor.
	std::optional<CompletionItem> probe_type(const TypeRef &p_site, const std::string &p_name, bool p_allow_void) {
		TypeRef ref;
		ref.name = p_name;
		ref.range = p_site.range;
		ref.name_token = NO_TOKEN;
		const size_t error_count = errors.size();
		const Type type = resolve_name(ref, p_allow_void);
		const bool accepted = errors.size() == error_count && !type.is_error();
		errors.erase(errors.begin() + error_count, errors.end());
		if (!accepted) {
			return std::nullopt;
		}
		CompletionItem item;
		item.kind = type.kind == TypeKind::Enum ? CompletionKind::Enum : CompletionKind::Class;
		item.name = p_name.substr(p_name.rfind('.') + 1);
		item.insert = item.name;
		item.place = type.script.empty() ? CompletionItem::Place::Engine : CompletionItem::Place::Project;
		return item;
	}

	// The keywords that can begin what is written at `r_site`, a bare name:
	// an expression, or a statement when it is the whole statement.
	void add_keywords(const Expr &r_site, std::map<std::string, CompletionItem> &r_found) {
		std::vector<std::pair<std::string, std::string>> words = { { "true", "" }, { "false", "" }, { "None", "" }, { "not", " " }, { "func", " " } };
		Expr self;
		self.kind = ExprKind::Self;
		self.range = r_site.range;
		const size_t error_count = errors.size();
		if (!type_expr(self, Type::of(TypeKind::Error)).is_error() && errors.size() == error_count) {
			words.insert(words.end(), { { "self", "" }, { "super", "" } });
		}
		errors.erase(errors.begin() + error_count, errors.end());
		if (no_await().empty()) {
			words.emplace_back("await", " ");
		}
		if (&r_site == statement_value) {
			words.insert(words.end(), { { "var", " " }, { "const", " " }, { "if", " " }, { "for", " " }, { "while", " " }, { "match", " " }, { "return", "" }, { "pass", "" } });
			if (loop_depth > 0) {
				words.insert(words.end(), { { "break", "" }, { "continue", "" } });
			}
			// After an if whose chain of elifs has no else yet.
			const Stmt *last = previous_statement && previous_statement->kind == StmtKind::If ? previous_statement : nullptr;
			while (last && last->else_body.size() == 1 && last->else_body[0]->kind == StmtKind::If) {
				last = last->else_body[0].get();
			}
			if (last && last->else_body.empty()) {
				words.insert(words.end(), { { "elif", " " }, { "else", "" } });
			}
		}
		for (const auto &[word, after] : words) {
			CompletionItem item;
			item.name = word;
			item.insert = word + after;
			r_found.emplace(word, item);
		}
		// Written as calls.
		for (const char *word : { "preload", "assert" }) {
			if (word == std::string("assert") && &r_site != statement_value) {
				continue;
			}
			CompletionItem item;
			item.kind = CompletionKind::Function;
			item.name = word;
			item.insert = item.name + "(";
			r_found.emplace(word, item);
		}
	}

	// Where an enum is expected at `r_site`, a bare name: its values (spec
	// §3.7), each written as the first that checks there, bare or after its
	// class or its name, to come first.
	void add_enum_values(const Expr &r_site, const Type &p_expected, std::map<std::string, CompletionItem> &r_found) {
		const Type expected = p_expected.kind == TypeKind::Option ? p_expected.args[0] : p_expected;
		if (expected.kind != TypeKind::Enum || !expected.values) {
			return;
		}
		const std::string spelled = enum_spelling(expected);
		const size_t dot = spelled.rfind('.');
		const std::string owner = !expected.values->is_engine ? spelled : (dot == std::string::npos ? std::string() : spelled.substr(0, dot));
		for (const auto &value : expected.values->list) {
			for (const std::string &written : { value.first, owner.empty() ? std::string() : owner + "." + value.first }) {
				if (written.empty()) {
					continue;
				}
				std::unique_ptr<Expr> probe;
				for (size_t start = 0; start <= written.size();) {
					const size_t end = std::min(written.find('.', start), written.size());
					auto part = std::make_unique<Expr>();
					part->kind = probe ? ExprKind::Member : ExprKind::Name;
					part->text = written.substr(start, end - start);
					part->range = r_site.range;
					part->name_token = r_site.name_token;
					part->base = std::move(probe);
					probe = std::move(part);
					start = end + 1;
				}
				const size_t error_count = errors.size();
				const size_t reference_count = references->size();
				const Type type = type_expr(*probe, Type::of(TypeKind::Error));
				const bool accepted = errors.size() == error_count && !type.is_error() && assignable(type, expected);
				errors.erase(errors.begin() + error_count, errors.end());
				references->erase(references->begin() + reference_count, references->end());
				if (accepted) {
					CompletionItem item;
					item.kind = CompletionKind::Enum;
					item.name = written;
					item.insert = written;
					item.place = CompletionItem::Place::Local;
					r_found.emplace(written, item);
					completion->force = true;
					break;
				}
			}
		}
	}

	void complete(Expr &r_site, const Type &p_expected) {
		const size_t error_count = errors.size();
		const size_t reference_count = references->size();
		Candidates names;
		// The class whose members are completed.
		Type owner = script_class_type(cls, options.path);
		if (!r_site.base) {
			scope_candidates(names);
		} else if (r_site.base->kind == ExprKind::Self) {
			class_candidates(cls, names);
		} else {
			Expr &base = *r_site.base;
			const EnumDecl *named_values = named_enum(base);
			const std::optional<Type> named = named_values ? std::nullopt : named_class(base);
			if (named_values) {
				for (const EnumValueDecl &value : named_values->values) {
					names.values.insert(value.name);
				}
			} else if (named && !named->is_error()) {
				owner = *named;
				script_candidates(owner, names);
				names.values.insert("new");
			} else if (names_type(base)) {
				if (const builtin::Class *type = builtin::find_class(base.text)) {
					owner = api_type(base.text, false);
					builtin_candidates(*type, names);
				} else {
					owner = Type::object(base.text);
					engine_candidates(base.text, names);
					names.values.insert("new");
				}
			} else {
				owner = check_value(base);
				value_candidates(owner, names);
				const ClassDecl *inner = base.target == Target::ClassScript ? script_header(base.owner) : nullptr;
				if (inner) {
					// An inner class reached through an object (spec §4.1).
					owner = script_class_type(*inner, base.owner);
					script_candidates(owner, names);
				}
			}
		}
		errors.erase(errors.begin() + error_count, errors.end());
		references->erase(references->begin() + reference_count, references->end());
		const std::vector<std::string> chain = class_chain(owner);
		std::map<std::string, CompletionItem> found;
		if (r_site.kind == ExprKind::Name && !r_site.base) {
			add_enum_values(r_site, p_expected, found);
		}
		if (!r_site.base) {
			add_global_classes(found, false);
			for (const std::string &name : names.types) {
				std::optional<CompletionItem> item = found.count(name) ? std::nullopt : probe_type({ name, r_site.range }, name, false);
				if (item) {
					found.emplace(name, *item);
				}
			}
		}
		for (const std::string &name : names.values) {
			std::optional<CompletionItem> item = found.count(name) ? std::nullopt : probe(r_site, name, chain);
			if (item) {
				found.emplace(name, *item);
			}
		}
		if (r_site.kind == ExprKind::Name && !r_site.base) {
			add_keywords(r_site, found);
		}
		set_completions(found);
	}

	// Of the type at the cursor, `p_site`, which may follow `Class.`.
	void complete_type(const TypeRef &p_site, bool p_allow_void) {
		const size_t dot = p_site.name.rfind('.', p_site.name.find(CURSOR));
		const std::string prefix = dot == std::string::npos ? std::string() : p_site.name.substr(0, dot + 1);
		Candidates names;
		std::map<std::string, CompletionItem> found;
		const size_t error_count = errors.size();
		if (prefix.empty()) {
			scope_candidates(names);
			names.types.insert({ "void", "Variant" });
			add_global_classes(found, false);
			if (options.project) {
				const std::vector<std::string> autoloads = options.project->autoload_names();
				names.types.insert(autoloads.begin(), autoloads.end());
			}
		} else {
			TypeRef outer;
			outer.name = prefix.substr(0, dot);
			outer.range = p_site.range;
			outer.name_token = NO_TOKEN;
			const Type type = resolve_name(outer, false);
			if (const ClassDecl *header = is_script_path(type.script) ? script_header(type.script) : nullptr) {
				for (const std::unique_ptr<ClassDecl> &inner : header->classes) {
					names.types.insert(inner->name);
				}
				for (const StructDecl &found : header->structs) {
					names.types.insert(found.name);
				}
				for (const InterfaceDecl &found : header->interfaces) {
					names.types.insert(found.name);
				}
				for (const EnumDecl &found : header->enums) {
					names.types.insert(found.name);
				}
			} else if (type.kind == TypeKind::Object) {
				for (const std::string &name : engine.enum_names(type.name)) {
					names.types.insert(name);
				}
			}
		}
		errors.erase(errors.begin() + error_count, errors.end());
		for (const std::string &name : names.types) {
			std::optional<CompletionItem> item = found.count(name) ? std::nullopt : probe_type(p_site, prefix + name, p_allow_void);
			if (item) {
				found.emplace(name, *item);
			}
		}
		set_completions(found);
	}

	// The classes that the class at the cursor, after `extends`, can extend:
	// of the engine or of this language (spec §4.1).
	void complete_extends() {
		std::map<std::string, CompletionItem> found;
		add_global_classes(found, true);
		std::vector<std::string> names = engine.class_list();
		for (const Checker *around = outer_checker; around; around = around->outer_checker) {
			for (const std::unique_ptr<ClassDecl> &inner : around->cls.classes) {
				names.push_back(inner->name);
			}
		}
		for (const std::string &name : names) {
			ClassDecl probe;
			probe.extends = name;
			const std::string base = classes.base_path(probe, options.path);
			if (classes.native_base(probe, options.path).empty() || base == options.path) {
				continue;
			}
			CompletionItem item;
			item.kind = CompletionKind::Class;
			item.name = name;
			item.insert = name;
			item.place = base.empty() ? CompletionItem::Place::Engine : CompletionItem::Place::Project;
			found.emplace(name, item);
		}
		set_completions(found);
	}

	// After `$` or `%` at the cursor: the paths that reach a node from the
	// nodes with this script, in the scenes that have one (spec §7.1). An
	// option replaces the editor's word at the cursor, or in a string what
	// follows its quote, which the editor quotes itself
	// (scene/gui/code_edit.cpp:3858-3860, 3916-3927; ed1daf0).
	void complete_node_path(Expr &r_site) {
		const std::string written = r_site.text;
		const size_t cursor = cls.source->text.find(CURSOR);
		size_t word = word_start(cls.source->text, cursor);
		bool quoted = false;
		for (uint32_t i = r_site.range.first; i <= r_site.range.last && i < cls.source->tokens.size(); i++) {
			const Token &token = cls.source->tokens[i];
			if (token.kind == TokenKind::String && token.offset < cursor && cursor < token.end()) {
				quoted = true;
				word = token.offset + 1;
			}
		}
		const size_t before = written.find(CURSOR);
		const std::string prefix = written.substr(0, before - std::min(before, cursor - word));
		std::set<std::string> paths;
		for (const Scene &scene : scenes) {
			const auto below = [&](const std::string &p_node, const std::string &p_written) {
				const std::string start = p_node == "." ? std::string() : p_node + "/";
				for (const SceneNode &node : scene.nodes) {
					if (node.path != "." && node.path.compare(0, start.size(), start) == 0) {
						paths.insert(p_written + node.path.substr(start.size()));
					}
				}
			};
			for (const SceneNode &node : scene.nodes) {
				if (extends_script(node.script, options.path)) {
					below(node.path, "");
				}
				if (node.unique) {
					const std::string name = "%" + node.path.substr(node.path.rfind('/') + 1);
					paths.insert(name);
					below(node.path, name + "/");
				}
			}
		}
		std::map<std::string, CompletionItem> found;
		for (const std::string &path : paths) {
			if (path.compare(0, prefix.size(), prefix) != 0) {
				continue;
			}
			r_site.text = path;
			const size_t error_count = errors.size();
			const size_t reference_count = references->size();
			const bool accepted = !check_get_node(r_site).is_error() && errors.size() == error_count;
			errors.erase(errors.begin() + error_count, errors.end());
			references->erase(references->begin() + reference_count, references->end());
			if (accepted) {
				CompletionItem item;
				item.kind = CompletionKind::NodePath;
				item.name = path.substr(prefix.size());
				item.insert = quoted || plain_path(item.name) ? item.name : "\"" + item.name + "\"";
				item.place = CompletionItem::Place::Local;
				found.emplace(item.name, item);
			}
		}
		r_site.text = written;
		set_completions(found);
	}

	// After `func` at the cursor, `p_site`: what a function with its modifiers
	// can be named, with the parameters of what it overrides and without
	// types, which come from there (spec §4.6). Each is tried by
	// check_signature(): the methods of the classes this one extends, and
	// _static_init(), which no class inherits (§4.8).
	void complete_override(const FuncDecl &p_site) {
		std::set<std::string> names = { "_static_init" };
		for (const std::string &name : engine.member_names(cls.native_base)) {
			names.insert(name);
		}
		for (size_t i = 0; cls.base && i < cls.base->all_functions.size(); i++) {
			if (!cls.base->all_functions[i]->of_struct) {
				names.insert(cls.base->all_functions[i]->name);
			}
		}
		const std::vector<std::string> chain = class_chain(script_class_type(cls, options.path));
		std::map<std::string, CompletionItem> found;
		for (const std::string &name : names) {
			if (std::any_of(cls.functions.begin(), cls.functions.end(), [&](const FuncDecl &p_function) { return p_function.name == name; })) {
				continue;
			}
			FuncDecl probe;
			probe.name = name;
			probe.name_token = p_site.name_token;
			probe.is_pub = p_site.is_pub;
			probe.is_static = p_site.is_static;
			probe.is_async = p_site.is_async;
			probe.is_abstract = p_site.is_abstract;
			probe.has_body = p_site.has_body;
			std::vector<std::string> params; // As they go in.
			std::string owner;
			const auto add_param = [&](const std::string &p_name) {
				probe.params.emplace_back().name = p_name;
				probe.params.back().name_token = NO_TOKEN;
				params.push_back(p_name);
			};
			const std::optional<EngineMethod> method = engine.find_method(cls.native_base, name);
			if (const FuncDecl *parent = name == "_init" ? nullptr : base_function(name)) {
				const ClassDecl &decl = declaring(*cls.base, *parent, &ClassDecl::functions);
				owner = decl.path;
				for (const ParamDecl &param : parent->params) {
					add_param(param.name);
					if (param.is_rest) {
						probe.params.back().is_rest = true;
						params.back() = "..." + params.back();
					}
					if (param.default_value) {
						// As checked where it is written.
						probe.params.back().default_value = std::make_unique<Expr>();
						probe.params.back().default_value->kind = ExprKind::Error;
						const uint32_t begin = decl.source->begin(param.default_value->range);
						params.back() += " = " + decl.source->text.substr(begin, decl.source->end(param.default_value->range) - begin);
					}
				}
			} else if (method && method->is_virtual) {
				owner = method->owner;
				for (const EngineParam &param : method->params) {
					add_param(param.name);
				}
			} else if (name != "_static_init") {
				continue;
			}
			const size_t error_count = errors.size();
			const size_t reference_count = references->size();
			check_signature(probe);
			const bool accepted = errors.size() == error_count;
			errors.erase(errors.begin() + error_count, errors.end());
			references->erase(references->begin() + reference_count, references->end());
			if (!accepted) {
				continue;
			}
			CompletionItem item;
			item.kind = CompletionKind::Function;
			item.name = name;
			item.insert = name + "(";
			for (size_t i = 0; i < params.size(); i++) {
				item.insert += (i > 0 ? ", " : "") + params[i];
			}
			item.insert += "):";
			const auto in_chain = std::find(chain.begin(), chain.end(), owner);
			item.place = in_chain == chain.end() ? CompletionItem::Place::Local : CompletionItem::Place::Member;
			item.depth = int(in_chain - chain.begin());
			found.emplace(name, item);
		}
		set_completions(found);
	}

	// A string at the cursor that names something by where it is (spec §10.4):
	// a file by its path, as preload() and an engine method's String `path`
	// take; an input action, as a StringName `action` names one, and the four
	// of Input.get_vector() (core/input/input.cpp:246-248); a pub method or
	// signal of this class, as `method` and `signal` name them in a call on
	// self, the engine finding no other (§10.5). A file the editor lists
	// loads, so each name goes in unquoted, which the editor quotes
	// (scene/gui/code_edit.cpp:3916-3927; ed1daf0).
	void complete_string(const EngineMethod *p_method, size_t p_index, bool p_on_self) {
		const EngineParam *param = p_method ? &p_method->params[p_index] : nullptr;
		const std::string name = param ? param->name : "path";
		const bool string_name = param && param->type.builtin_name() == "StringName";
		std::map<std::string, CompletionItem> found;
		const auto add = [&](const std::string &p_name, CompletionKind p_kind, CompletionItem::Place p_place) {
			CompletionItem item;
			item.kind = p_kind;
			item.name = p_name;
			item.insert = p_name;
			item.place = p_place;
			found.emplace(p_name, item);
		};
		if (name == "path" && (!param || param->type.kind == TypeKind::String) && options.project) {
			for (const std::string &file : options.project->resource_files()) {
				add(file, CompletionKind::FilePath, CompletionItem::Place::Project);
			}
		} else if (string_name && (name == "action" || (name.size() > 7 && name.compare(name.size() - 7, 7, "_action") == 0) || (p_method->owner == "Input" && p_method->name == "get_vector" && p_index < 4)) && options.project) {
			for (const std::string &action : options.project->input_actions()) {
				add(action, CompletionKind::PlainText, CompletionItem::Place::Project);
			}
		} else if (string_name && p_on_self && (name == "method" || name == "signal")) {
			if (name == "method") {
				for (const FuncDecl *function : cls.all_functions) {
					if (!function->of_struct && (function->is_pub || function->is_override)) {
						add(function->name, CompletionKind::Function, CompletionItem::Place::Member);
					}
				}
			} else {
				for (const SignalDecl *signal : cls.all_signals) {
					if (signal->is_pub) {
						add(signal->name, CompletionKind::Signal, CompletionItem::Place::Member);
					}
				}
			}
			// As the engine lists them (core/object/object.cpp:2396-2413).
			for (const std::string &member : engine.member_names(cls.native_base)) {
				const std::optional<EngineMethod> method = name == "method" ? engine.find_method(cls.native_base, member) : std::nullopt;
				if (method && (member[0] != '_' || method->is_virtual)) {
					add(member, CompletionKind::Function, CompletionItem::Place::Engine);
				} else if (name == "signal" && engine.find_signal(cls.native_base, member)) {
					add(member, CompletionKind::Signal, CompletionItem::Place::Engine);
				}
			}
		}
		set_completions(found);
	}

public:
	Checker(ClassDecl &r_class, const EngineApi &p_engine, const CompileOptions &p_options, std::vector<Diagnostic> &r_errors, ScriptClasses &r_classes, bool p_check_bodies, std::vector<Reference> *r_references, Completion *r_completion) :
			cls(r_class), engine(p_engine), options(p_options), errors(r_errors), check_bodies(p_check_bodies), classes(r_classes), references(r_references), completion(r_completion) {}

	void attach(FileCheck &r_file, Checker *p_outer) {
		file = &r_file;
		outer_checker = p_outer;
	}

	const std::string &path() const { return options.path; }
	const ClassDecl &decl() const { return cls; }
	bool is_declared_class() const { return state == State::Declared && declared; }

	// The class's declarations, once; when another class of this file needs
	// them first, then.
	void declare() {
		if (state != State::Unchecked) {
			return;
		}
		state = State::Declaring;
		declared = declare_class();
		cls.declared = declared;
		state = State::Declared;
	}

	void check_function_bodies() {
		if (!declared) {
			return;
		}
		for (FuncDecl &fn : cls.functions) {
			if (is_declared(fn)) {
				check_body(fn);
			}
		}
		// It needs what _init assigns, which only the check of its body finds.
		if (check_bodies) {
			check_ready_reads();
		}
		for (StructDecl &decl : cls.structs) {
			structure = &decl;
			structure_type = Type::structure(display_name(cls, options.path) + "." + decl.name, options.path);
			for (FuncDecl &method : decl.methods) {
				if (is_declared(method)) {
					check_body(method);
				}
			}
		}
		structure = nullptr;
	}

	// The checks of how the scene options.scene places the class.
	void check_scene() {
		const std::optional<Scene> scene = declared ? options.project->scene(options.scene) : std::nullopt;
		if (!scene) {
			return;
		}
		scenes = { *scene };
		check_abstract_scenes();
		check_connections();
		check_named_members();
		check_owner_scenes();
		check_parent_scenes();
		check_stored_exports();
		check_scene_exports();
	}

private:
	// Whether a class of `p_file` extends one of this file, directly or
	// through the classes of the other files `r_files` lists after it. Each
	// file loads its classes together, so neither file could load first.
	bool extends_back(const std::string &p_file, std::vector<std::string> &r_files, std::unordered_set<std::string> &r_seen) {
		if (!r_seen.insert(p_file).second) {
			return false;
		}
		r_files.push_back(p_file);
		std::vector<std::pair<const ClassDecl *, std::string>> in_file{ { script_header(p_file), p_file } };
		for (size_t i = 0; i < in_file.size() && in_file[i].first; ++i) {
			const auto [decl, path] = in_file[i];
			for (const std::unique_ptr<ClassDecl> &inner : decl->classes) {
				in_file.push_back({ inner.get(), path + "::" + inner->name });
			}
			const std::string base = script_file(classes.base_path(*decl, path));
			if (is_script_path(base) && (base == script_file(options.path) || (base != p_file && extends_back(base, r_files, r_seen)))) {
				return true;
			}
		}
		r_files.pop_back();
		return false;
	}

	bool declare_class() {
		if (completion && at_cursor(cls.extends)) {
			complete_extends();
		}
		cls.path = options.path;
		cls.native_base = classes.native_base(cls, options.path);
		cls.base_script = classes.base_path(cls, options.path);
		refer(cls.name.empty() ? cls.class_name_token : cls.name_token, script_id(options.path), SymbolKind::Class);
		if (!cls.extends_path && is_script_path(cls.base_script)) {
			// Each name of `Other.Inner` refers to its class (spec §4.1).
			std::string path = cls.base_script;
			for (size_t i = size_t(std::count(cls.extends.begin(), cls.extends.end(), '.')) + 1; i-- > 0 && cls.extends_token != NO_TOKEN;) {
				refer(cls.extends_token + uint32_t(2 * i), script_id(path));
				path = path.substr(0, path.rfind("::"));
			}
		} else if (!cls.extends_path) {
			refer(cls.extends_token, cls.base_script.empty() ? engine_id(cls.extends) : foreign_id(cls.base_script, std::string()));
		} else if (cls.extends_token < cls.source->tokens.size()) {
			const Token &token = cls.source->tokens[cls.extends_token];
			if (token.length == cls.extends.size() + 2) {
				refer_bytes(token.offset + 1, token.end() - 1, script_id(cls.base_script));
			}
		}
		if (!cls.base_script.empty()) {
			if (!is_script_path(cls.base_script)) {
				error(cls.extends_token, "Inheritance from a script of another language is not supported.");
				return false;
			}
			std::unordered_set<std::string> seen{ options.path };
			for (std::string path = cls.base_script; !path.empty();) {
				if (!seen.insert(path).second) {
					error(cls.extends_token, "Inheritance cycle involving " + path + ".");
					return false;
				}
				const ClassDecl *header = script_header(path);
				if (!header) {
					break;
				}
				path = classes.base_path(*header, path);
			}
			std::vector<std::string> files;
			std::unordered_set<std::string> seen_files;
			if (script_file(cls.base_script) != script_file(options.path) && extends_back(script_file(cls.base_script), files, seen_files)) {
				std::string through;
				for (size_t i = 1; i < files.size(); ++i) {
					through += (i == 1 ? " through " : ", ") + files[i];
				}
				error(cls.extends_token, "A class of " + files[0] + " extends one of this file" + through + "; files extending each other's classes are not supported.");
			}
			const ClassDecl *parent = nullptr;
			if (Checker *local = file->find(cls.base_script)) {
				local->declare();
				if (local->state != State::Declared) {
					error(cls.extends_token, display_name(local->cls, local->path()) + " uses " + display_name(cls, options.path) + " in its declarations, so it cannot be its base class.");
					return false;
				}
				parent = local->declared ? &local->cls : nullptr;
			} else {
				parent = classes.declarations(cls.base_script, !options.declarations_only);
				// A subclass holds all of its base's members, so it needs them all.
				if (const std::optional<int> line = parent ? classes.first_error(cls.base_script) : std::nullopt) {
					error(cls.extends_token, "Base script " + cls.base_script + " has errors (line " + std::to_string(*line) + ").");
					return false;
				}
			}
			if (!parent) {
				error(cls.extends_token, "Base script " + cls.base_script + " has errors or cannot be read.");
				return false;
			}
			cls.base = parent;
			cls.all_members = cls.base->all_members;
			cls.all_static_vars = cls.base->all_static_vars;
			cls.all_functions = cls.base->all_functions;
			cls.all_signals = cls.base->all_signals;
			cls.implemented = cls.base->implemented;
			cls.implemented_with_errors = cls.base->implemented_with_errors;
			for (size_t i = 0; i < cls.all_members.size(); ++i) {
				member_index[cls.all_members[i]->name] = int(i);
			}
			for (size_t i = 0; i < cls.all_static_vars.size(); ++i) {
				static_index[cls.all_static_vars[i]->name] = int(i);
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
			return false;
		}
		// Only a file's class is set on nodes in scenes (spec §4.1).
		if (options.project && !options.declarations_only && !options.path.empty() && cls.name.empty()) {
			scenes = classes.scenes_with_script(options.path);
		}
		declare_enums();
		declare_members();
		// Before the types that may name a class through a constant (spec §2.1).
		declare_constants();
		declare_functions();
		declare_interfaces();
		declare_structs();
		declare_requirement(cls.owner_ref, cls.owner_type, "@owner");
		declare_requirement(cls.parent_ref, cls.parent_type, "@parent");
		for (FuncDecl &fn : cls.functions) {
			if (completion && at_cursor(fn.name)) {
				complete_override(fn);
			}
			if (is_declared(fn)) {
				check_signature(fn);
			}
		}
		for (StructDecl &decl : cls.structs) {
			for (FuncDecl &method : decl.methods) {
				if (is_declared(method)) {
					check_signature(method);
				}
			}
		}
		check_field_defaults();
		check_implemented();
		check_implements();
		declare_signals();
		check_type_names();
		for (EnumDecl &decl : cls.enums) {
			check_enum(decl);
		}
		check_held_cycles();
		check_engine_constant_names();
		for (ConstDecl &constant : cls.constants) {
			const auto registered = constant_index.find(constant.name);
			if (registered != constant_index.end() && cls.all_constants[registered->second] == &constant) {
				check_constant(constant);
			}
		}
		check_members();
		check_pub_signatures();
		check_engine_options();
		check_interface_properties();
		check_groups();
		for (FuncDecl &fn : cls.functions) {
			for (const Annotation &annotation : fn.annotations) {
				if (annotation.name == "rpc") {
					check_rpc(fn, annotation);
				}
			}
		}
		check_warning_ignores();
		return true;
	}
};

Checker *FileCheck::find(const std::string &p_path) const {
	for (const std::unique_ptr<Checker> &checker : checkers) {
		if (checker->path() == p_path) {
			return checker.get();
		}
	}
	return nullptr;
}

void add_checkers(FileCheck &r_file, ClassDecl &r_class, const EngineApi &p_engine, const CompileOptions &p_options, std::vector<Diagnostic> &r_errors, ScriptClasses &r_classes, bool p_check_bodies, std::vector<Reference> *r_references, Completion *r_completion, Checker *p_outer) {
	r_file.checkers.push_back(std::make_unique<Checker>(r_class, p_engine, p_options, r_errors, r_classes, p_check_bodies, r_references, r_completion));
	Checker *checker = r_file.checkers.back().get();
	checker->attach(r_file, p_outer);
	for (std::unique_ptr<ClassDecl> &inner : r_class.classes) {
		CompileOptions options = p_options;
		options.path += "::" + inner->name;
		add_checkers(r_file, *inner, p_engine, options, r_errors, r_classes, p_check_bodies, r_references, r_completion, checker);
	}
}

} // namespace

void check(ClassDecl &r_class, const EngineApi &p_engine, const CompileOptions &p_options, std::vector<Diagnostic> &r_errors, ScriptClasses &r_classes, bool p_check_bodies, std::vector<Reference> *r_references, Completion *r_completion) {
	FileCheck file;
	add_checkers(file, r_class, p_engine, p_options, r_errors, r_classes, p_check_bodies, r_references, r_completion, nullptr);
	r_classes.enter(p_options.path, r_class, [&](const std::string &p_path) -> const ClassDecl * {
		Checker *checker = file.find(p_path);
		if (!checker) {
			return nullptr;
		}
		checker->declare();
		return checker->is_declared_class() ? &checker->decl() : nullptr;
	});
	for (const std::unique_ptr<Checker> &checker : file.checkers) {
		checker->declare();
	}
	if (p_options.scene.empty()) {
		for (const std::unique_ptr<Checker> &checker : file.checkers) {
			checker->check_function_bodies();
		}
	} else {
		r_errors.clear();
		if (r_references) {
			r_references->clear();
		}
		file.checkers.front()->check_scene();
	}
	r_classes.leave(p_options.path);
}

std::vector<std::pair<std::string, bool>> export_annotations() {
	std::vector<std::pair<std::string, bool>> names = { { "export_custom", true } };
	for (const HintAnnotation &annotation : HINT_ANNOTATIONS) {
		names.emplace_back(annotation.name, annotation.most > 0);
	}
	return names;
}

} // namespace petrichor::compiler
