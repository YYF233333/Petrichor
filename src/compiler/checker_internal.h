#pragma once

#include "compiler/builtin_api.h"
#include "compiler/checker.h"
#include "compiler/navigation.h"
#include "compiler/parser.h"
#include "compiler/script_classes.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <iterator>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace petrichor::compiler::checking {

// Gives an expression made by the compiler the position of the code it belongs to.
inline void relocate(Expr &r_expr, const TokenRange &p_range) {
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

// An expression made by the compiler, at the position of the code it belongs to.
inline std::unique_ptr<Expr> synthetic(ExprKind p_kind, const std::string &p_text, const TokenRange &p_range) {
	auto expr = std::make_unique<Expr>();
	expr->kind = p_kind;
	expr->text = p_text;
	expr->range = p_range;
	expr->name_token = expr->op_token = NO_TOKEN;
	return expr;
}

// The declaration of a name in one of a class's lists, which holds each name
// once; null when it has none.
template <typename T>
const T *find_named(const std::vector<const T *> &p_decls, const std::string &p_name) {
	const auto found = std::find_if(p_decls.begin(), p_decls.end(), [&](const T *p_decl) { return p_decl->name == p_name; });
	return found == p_decls.end() ? nullptr : *found;
}

inline std::vector<Type> param_types(const std::vector<ParamDecl> &p_params) {
	std::vector<Type> types;
	for (const ParamDecl &param : p_params) {
		types.push_back(param.type);
	}
	return types;
}

// The methods of T? (spec §3.4), as Rust's; flatten() only of T??.
inline const std::unordered_set<std::string> &option_methods() {
	static const std::unordered_set<std::string> methods = { "unwrap", "expect", "unwrap_or", "unwrap_or_else", "map", "and_then", "or", "or_else", "filter", "ok_or", "ok_or_else", "flatten" };
	return methods;
}

// The methods of Result (spec §3.5), as Rust's.
inline const std::unordered_set<std::string> &result_methods() {
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

inline bool at_cursor(const std::string &p_name) {
	return p_name.find(CURSOR) != std::string::npos;
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
	bool init_checked = false;

	FuncDecl *function = nullptr;
	std::vector<std::unordered_map<std::string, int>> scopes;
	// Types of locals narrowed by conditions (spec §5.4), innermost last.
	std::vector<std::unordered_map<int, Type>> narrowed;
	int loop_depth = 0;
	bool in_predelete = false;
	bool in_assert = false;
	// The interfaces this class's own `implements` names, with where.
	std::vector<std::pair<Type, const TypeRef *>> own_interfaces;
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
		// The values returned. Those that leave part of their type to the
		// others are typed as the lambda's return type once that is known.
		std::vector<Expr *> values;
	};
	Returns *returns = nullptr;

	std::unordered_map<std::string, int> member_index;
	std::unordered_map<std::string, int> static_index; // In all_static_vars.
	std::unordered_map<std::string, int> constant_index; // In all_constants.
	std::unordered_map<std::string, int> function_index;
	std::unordered_map<std::string, int> signal_index;
	mutable std::unordered_map<std::string, std::optional<ForeignClass>> foreign_classes; // By path.

	void error(uint32_t p_token, const std::string &p_message) {
		error(TokenRange{ p_token, p_token }, p_message);
	}

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

	// The type of the objects of a script of this language.
	Type script_class_type(const ClassDecl &p_header, const std::string &p_path) const {
		return Type::script_class(display_name(p_header, p_path), p_path, classes.native_base(p_header, p_path));
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

	// Whether a bare name reaches the global names: no local, declaration of
	// this class or of one around it, or member of the engine class it
	// extends has it (spec §4.1).
	bool is_global(const std::string &p_name) const {
		return !declares(p_name) && !class_around(p_name);
	}

	// Whether a bare name is declared by this class or one around it, which
	// then comes before the engine's global names (spec §4.1).
	bool declared_in_scope(const std::string &p_name) const {
		return p_name.find('.') == std::string::npos && (declares_in_script(p_name) || class_around(p_name));
	}

	// `Outer.Inner` and the like as written, from its first name.
	static std::string dotted_name(const Expr &p_expr) {
		return p_expr.kind == ExprKind::Member && p_expr.base ? dotted_name(*p_expr.base) + "." + p_expr.text : p_expr.text;
	}

	static uint32_t first_name_token(const Expr &p_expr) {
		return p_expr.kind == ExprKind::Member && p_expr.base ? first_name_token(*p_expr.base) : p_expr.name_token;
	}

	std::string no_instance(const std::string &p_name, const Checker &p_around) const {
		return "\"" + p_name + "\" belongs to an object of " + display_name(p_around.cls, p_around.path()) + ", which " + display_name(cls, options.path) + " does not have: an inner class has no object of the class around it.";
	}

	// Named as one of this language is.
	static Type foreign_class_type(const std::string &p_path, const ForeignClass &p_foreign) {
		return Type::script_class(p_foreign.name.empty() ? file_name(p_path) : p_foreign.name, p_path, p_foreign.native);
	}

	// Whether an object can be both: `is` and `as` between them are not always false.
	bool related(const Type &p_a, const Type &p_b) const {
		return is_a(p_a, p_b) || is_a(p_b, p_a) || p_a.is_interface || p_b.is_interface;
	}

	bool assignable(const Type &p_from, const Type &p_to) const {
		return is_assignable(p_from, p_to, *this);
	}

	bool is_type_name(const std::string &p_name) const {
		return p_name == "int" || p_name == "float" || p_name == "bool" || builtin::find_class(p_name) || engine.class_exists(p_name);
	}

	// Makes a call a call of the engine's ResourceLoader, as GDScript's load()
	// and preload() are.
	void on_resource_loader(Expr &r_call) {
		r_call.base = synthetic(ExprKind::Name, "ResourceLoader", r_call.range);
	}

	int find_local(const std::string &p_name) const {
		return find_in(scopes, p_name);
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

	// Whether the base of `Name.x` or `Name.f()` names a type of the engine,
	// whose name any closer one hides (spec §4.1). A singleton's name is its
	// object, except where it names the class (`p_singleton`).
	bool names_type(const Expr &p_expr, bool p_singleton = false) const {
		return p_expr.kind == ExprKind::Name && is_type_name(p_expr.text) && is_global(p_expr.text) && (p_singleton || !engine.is_singleton(p_expr.text));
	}

	bool names_variable(const std::string &p_name) const {
		return visible_local(p_name) || member_index.count(p_name) || static_index.count(p_name) || engine.find_property(cls.native_base, p_name);
	}

	// The _init that new() of the class calls: its own or the nearest it inherits.
	static const FuncDecl *init_of(const ClassDecl &p_class) {
		return find_named(p_class.all_functions, "_init");
	}

	// The members that must be assigned which _init has assigned on every path
	// so far; nullopt where no path goes on.
	using Assigned = std::optional<std::set<int>>;

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

	// A constant initialized with the name of an enum with payloads.
	static bool names_payload_enum(const ConstDecl &p_decl) {
		return p_decl.value && p_decl.value->names_enum && p_decl.type.kind == TypeKind::PayloadEnum;
	}

	// What a value is called in references: State.IDLE, or A of an unnamed enum.
	static std::string enum_value_id(const EnumDecl &p_decl, const std::string &p_value) {
		return p_decl.name.empty() ? p_value : p_decl.name + "." + p_value;
	}

	bool is_own(const EnumDecl &p_decl) const {
		return &p_decl >= cls.enums.data() && &p_decl < cls.enums.data() + cls.enums.size();
	}

	// Whether an expression may name a type, as `State` and `Player.State`
	// do, or a call's callee, as in `State(x)`.
	static bool may_name(const Expr &p_name, bool p_call) {
		return p_call || p_name.kind == ExprKind::Name || (p_name.kind == ExprKind::Member && p_name.base);
	}

	// How this file writes an enum: State for its own, Player.State for another class's.
	std::string enum_spelling(const Type &p_enum) const {
		return p_enum.values && !p_enum.values->is_engine && !p_enum.is_result() && p_enum.script == options.path ? p_enum.interface_name() : p_enum.to_string();
	}

	// Whether a type has a default value (spec §4.3): an enum when 0 is one of
	// its values (§3.7).
	bool enum_has_default(const Type &p_enum) const {
		return p_enum.kind != TypeKind::Enum || !p_enum.values || p_enum.values->has(0);
	}

	// Self, unchecked, as the base that a field or a method of the struct
	// named bare in its methods is on.
	static std::unique_ptr<Expr> struct_self(const Expr &p_at) {
		return synthetic(ExprKind::Self, std::string(), p_at.range);
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

	// The id of an interface's member, at the interface that declares it.
	static DeclId interface_member_id(const InterfaceMember &p_member) {
		return script_id(p_member.owner.script, p_member.owner.interface_name() + "." + p_member.name());
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

	const ViewNode *view_node(const Scene &p_scene, const std::string &p_path) {
		return view_node(p_scene, [&](const ViewNode &p_node) { return p_node.path == p_path; });
	}

	static bool is_script_file(const std::string &p_file) {
		return has_extension(p_file, { ".pet", ".gd", ".cs" });
	}

	static bool is_scene_file(const std::string &p_file) {
		return has_extension(p_file, { ".tscn", ".scn" });
	}

	// Why a member that a scene names must be pub (spec §10.5).
	static std::string not_pub(bool p_method) {
		return p_method ? ", which is not pub; the engine calls only pub methods by name." : ", which is not pub; the engine finds only pub properties by name.";
	}

	// The class of a node or a resource with the script that extends this
	// one; null when the script cannot be read or declared. Its own check
	// reports why, and the checks of the values it is given wait for it.
	const ClassDecl *placed_class(const std::string &p_script) const {
		return is_script_path(p_script) ? script_decl(p_script) : &cls;
	}

	// An enum's value takes part in operations as the int it is (spec §3.7).
	static Type as_int(const Type &p_type) {
		return p_type.kind == TypeKind::Enum ? Type::of(TypeKind::Int) : p_type;
	}

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

	const Type *narrowed_type(int p_local) const {
		return narrowed_in(narrowed, p_local);
	}

	// Whether a type a lambda returns leaves out what its T? or Result holds,
	// as None does, or Ok(v) going to and_then() (spec §4.6).
	static bool partial(const Type &p_type) {
		return p_type.kind == TypeKind::None || (p_type.kind == TypeKind::Option && p_type.args[0].is_error()) || (p_type.is_result() && (p_type.args[0].is_error() || p_type.args[1].is_error()));
	}

	static bool is_typed_scene(const Type &p_type) {
		return p_type.kind == TypeKind::Object && p_type.name == "PackedScene" && !p_type.args.empty();
	}

	static std::string argument_count(const FuncDecl &p_function) {
		return argument_count(required_params(p_function), int(p_function.params.size()), p_function.has_rest());
	}

	// A method value keeps no reference to its object (spec §3.11), so one of
	// a new RefCounted, which nothing else holds, is refused.
	bool receiver_kept(Expr &r_expr) {
		return receiver_kept(r_expr.base.get(), r_expr.range);
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

	static int option_depth(const Type &p_type) {
		return p_type.kind == TypeKind::Option ? 1 + option_depth(p_type.args[0]) : 0;
	}

	// `Command.MOVE(...)`: the enum with payloads whose variant the call
	// makes, if any.
	const EnumDecl *called_variant(Expr &r_call) {
		return r_call.base ? payload_enum_named(*r_call.base) : nullptr;
	}

	void declare_local(Stmt &r_stmt, const Type &p_type) {
		r_stmt.local = declare_local(r_stmt.name, r_stmt.name_token, p_type, r_stmt.kind == StmtKind::Const);
	}

	static bool binds(const Pattern &p_pattern) {
		return p_pattern.kind == PatternKind::Bind || std::any_of(p_pattern.items.begin(), p_pattern.items.end(), [](const Pattern &p_item) { return binds(p_item); });
	}

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

	static bool takes(const FuncDecl &p_function, size_t p_count) {
		return p_count >= size_t(required_params(p_function)) && (p_function.has_rest() || p_count <= p_function.params.size());
	}

	// Names that may be written somewhere, as types or as values: more than
	// can, since each is tried where completion asks.
	struct Candidates {
		std::set<std::string> types;
		std::set<std::string> values;
	};

	/* checker.cpp */

	void error(const TokenRange &p_range, const std::string &p_message);
	void error_in_scene(const TokenRange &p_range, const Scene &p_scene, const std::string &p_node, const std::string &p_message);
	Reference *refer(uint32_t p_token, const DeclId &p_target, std::optional<SymbolKind> p_declares = std::nullopt, const std::optional<DeclId> &p_overrides = std::nullopt);
	Reference *refer_bytes(uint32_t p_begin, uint32_t p_end, const DeclId &p_target);
	void refer_local(uint32_t p_token, const Type &p_type, bool p_constant = false);
	void refer_string(const Expr &p_literal, const DeclId &p_target);
	const ClassDecl *script_decl(const std::string &p_path) const;
	const ClassDecl *script_header(const std::string &p_path) const;
	std::optional<std::string> broken(const void *p_decl, const std::string &p_what) const;
	std::optional<std::string> refused(const void *p_decl, bool p_visible, const std::string &p_kind, const std::string &p_class, const std::string &p_name) const;
	std::string missing(std::string p_message, const std::string &p_script) const;
	std::string display_name(const ClassDecl &p_decl, const std::string &p_path) const;
	std::string inner_class_path(const std::string &p_path, const std::string &p_name) const;
	bool declares(const std::string &p_name) const;
	bool declares_in_script(const std::string &p_name) const;
	bool refuse_ambiguous_engine_name(const std::string &p_name, const TokenRange &p_at);
	Checker *class_around(const std::string &p_name) const;
	std::string scope_class(const std::string &p_name) const;
	std::string class_path_of(const Expr &p_expr, bool p_object = false);
	std::string class_named(const std::string &p_name, bool p_autoload = false);
	Type class_value(Expr &r_expr, const std::string &p_path);
	Type value_around(Expr &r_expr, Checker &r_around);
	static std::string file_name(const std::string &p_path);
	const ForeignClass *foreign_class(const std::string &p_path) const;
	std::optional<Type> autoload_class(const std::string &p_name, const TokenRange &p_at);
	std::string autoload_script(const std::string &p_name);
	Type localized(Type p_type);
	Type unknown_foreign(Expr &r_expr, const Type &p_class);
	std::optional<Type> foreign_member(Expr &r_expr, const Type &p_class);
	std::optional<Type> foreign_method(Expr &r_expr, const Type &p_class);
	std::optional<EngineMethod> foreign_method_named(Expr &r_expr, const Type &p_class, const ForeignClass &p_foreign);
	bool extends_script(const std::string &p_script, const std::string &p_base) const;
	bool implements(const Type &p_class, const Type &p_interface) const;
	bool is_a(const Type &p_from, const Type &p_to) const override;
	std::vector<Type> fields(const Type &p_struct) const override;
	std::optional<Type> script_type(const TypeRef &p_ref);
	std::optional<Type> own_type(const std::string &p_name, uint32_t p_token);
	const StructDecl *struct_decl(const Type &p_struct) const;
	bool has_default(const Type &p_type) const;
	bool has_value_without_initializer(const Type &p_type) const;
	Type resolve(const TypeRef &p_ref, bool p_allow_void);
	Type resolve_name(const TypeRef &p_ref, bool p_allow_void);
	Type resolve_generic(const TypeRef &p_ref);
	Type container_element(const TypeRef &p_ref, const std::string &p_what);
	static Type function_type(const FuncDecl &p_function);
	static std::string signature_text(const FuncDecl &p_function);
	static Type signal_type(const EngineMethod &p_signal);
	Type set(Expr &r_expr, const Type &p_type);
	Type fail(Expr &r_expr, const std::string &p_message);
	Type fail_call(Expr &r_call, const std::string &p_message);
	Type fail_call(Expr &r_call);
	static int find_in(const std::vector<std::unordered_map<std::string, int>> &p_scopes, const std::string &p_name);
	const LocalInfo *visible_local(const std::string &p_name) const;
	int capture(const std::string &p_name);
	static bool init_assigns(const ClassDecl &p_class, const std::string &p_member);
	static Assigned joined(const Assigned &p_a, const Assigned &p_b);
	Assigned init_block(const Block &p_block, Assigned p_assigned, std::set<int> &r_reported);
	Assigned init_stmt(const Stmt &p_stmt, Assigned p_assigned, std::set<int> &r_reported);
	void init_reads(const Expr &p_expr, const std::set<int> &p_assigned, std::set<int> &r_reported);
	bool extends_back(const std::string &p_file, std::vector<std::string> &r_files, std::unordered_set<std::string> &r_seen);
	bool declare_class();

	/* checker_names.cpp */

	void declare_constants();
	static ConstPtr loading_value(const Expr &p_expr);
	ConstPtr constant_value(Expr &r_initializer, const std::optional<TypeRef> &p_declared, Type &r_type);
	void check_constant(ConstDecl &r_decl);
	Type use_constant(Expr &r_expr, const ConstDecl &p_decl, const std::string &p_path);
	const EnumDecl *initializer_payload_enum(Expr &r_initializer);
	const ConstDecl *own_constant(const std::string &p_name);
	void declare_enums();
	void check_type_names();
	void check_enum(EnumDecl &r_decl);
	void check_variant_fields(EnumDecl &r_decl, EnumValueDecl &r_value);
	const EnumDecl *own_enum(const std::string &p_name);
	std::optional<std::pair<const EnumDecl *, int>> bare_enum_value(const std::string &p_name);
	Type use_enum_value(Expr &r_expr, const EnumDecl &p_decl, int p_index);
	Type use_enum_dictionary(Expr &r_expr, const EnumDecl &p_decl, bool p_refer = true);
	Type use_engine_enum_dictionary(Expr &r_expr, const Type &p_enum);
	const EnumDecl *named_enum(Expr &r_name, bool p_call = false);
	const EnumDecl *aliased_enum(const std::string &p_name);
	std::optional<Type> aliased_enum_type(const std::string &p_name);
	std::optional<Type> named_engine_enum(const Expr &p_name, bool p_call);
	Type check_engine_enum_value(Expr &r_expr, const Type &p_enum);
	bool names_enum_of_class(const Expr &p_class, const std::string &p_name);
	const EnumDecl *scope_enum(const std::string &p_name);
	static const EnumDecl &cancel_reason_decl();
	static DeclId enum_id(const EnumDecl &p_decl, const std::string &p_value = std::string());
	std::optional<Type> named_engine_enum(const std::string &p_name) const;
	void refer_engine_enum(const std::string &p_name, uint32_t p_first, uint32_t p_last);
	Type check_enum_conversion(Expr &r_expr, const Type &p_enum);
	Type check_enum_value(Expr &r_expr, const EnumDecl &p_decl);
	ConstPtr class_alias(const std::string &p_name);
	std::pair<const ConstDecl *, const ClassDecl *> alias_decl(const std::string &p_name);
	DeclId alias_id(const std::string &p_name);
	std::string class_name_path(const std::string &p_name) const;
	std::optional<Type> named_class(Expr &r_base);
	std::optional<Type> class_constant(Expr &r_expr, const std::string &p_path, const std::string &p_class);
	Type use_static(Expr &r_expr, const VarDecl &p_variable, const ClassDecl &p_owner);
	std::optional<Type> class_static(Expr &r_expr, const std::string &p_path, const std::string &p_class);
	std::optional<Type> check_self_name(Expr &r_expr);
	Type check_self(Expr &r_expr);
	std::optional<Type> struct_name(Expr &r_expr);
	Type struct_method_value(Expr &r_expr, const Type &p_struct, bool p_on_value);
	Type check_name(Expr &r_expr);
	std::optional<Type> script_member(Expr &r_expr, const Type &p_class);
	Type class_member_value(Expr &r_expr, const std::string &p_path, const std::string &p_class);
	Type new_value(Expr &r_expr, const std::string &p_path);
	Type static_value(Expr &r_expr, const FuncDecl &p_function, const std::string &p_owner);
	Type method_value(Expr &r_expr, const FuncDecl &p_method);
	const InterfaceDecl *interface_decl(const Type &p_interface) const;
	std::vector<Type> interface_order(const std::vector<Type> &p_roots) const;
	std::vector<InterfaceMember> interface_members(const std::vector<Type> &p_roots) const;
	std::optional<InterfaceMember> interface_member(const Type &p_interface, const std::string &p_name) const;
	const InterfaceProperty *interface_property(const Type &p_interface, const std::string &p_name) const;
	std::optional<Type> script_method(Expr &r_expr, const Type &p_class);
	Type call_declared(Expr &r_expr, const FuncDecl &p_callee, const SourceFile &p_source, const Type &p_result);
	Type check_engine_constant(Expr &r_expr, const std::string &p_class);
	Type check_member_access(Expr &r_expr);

	/* checker_scenes.cpp */

	static std::string shown_path(const std::string &p_path);
	static bool plain_path(const std::string &p_path);
	static std::vector<std::string> path_parts(const std::string &p_path);
	static std::string parent_path(const std::string &p_path);
	const std::vector<ViewNode> &view_of(const Scene &p_scene, int p_depth = 0);
	const ViewNode *view_node(const Scene &p_scene, const std::function<bool(const ViewNode &)> &p_is);
	NodeWalk walk_node_path(const Scene &p_scene, const std::string &p_from, const std::string &p_path);
	void walk_from(const Expr &p_at, const Scene &p_scene, const std::string &p_from, const std::string &p_marked, const std::string &p_shown, bool p_optional, std::vector<NodeWalk> &r_found, int p_depth = 0);
	std::string through_autoload(const std::string &p_path, const std::string &p_written) const;
	std::string autoload_way(const std::string &p_path) const;
	Type check_get_node(Expr &r_expr);
	bool walk_placements(const std::vector<Scene> &p_scenes, const Expr &p_path_expr, const std::string &p_shown, bool p_optional, std::vector<NodeWalk> &r_found);
	void check_node_paths();
	Type scene_node_at(const Expr &p_path_expr, const std::string &p_shown, bool p_optional);
	std::optional<Type> literal_node_type(const Expr &p_call, const EngineMethod &p_method);
	void refer_node_path(const Expr &p_get_node, const NodeWalk &p_walk);
	Type scene_node_type(const TokenRange &p_at, const SceneNode &p_node, const std::string &p_what);
	static bool has_extension(const std::string &p_path, std::initializer_list<const char *> p_extensions);
	std::string preload_path(const Expr &p_call) const;
	void check_load_cycle(const Expr &p_preload, const std::string &p_file);
	bool loads_this_script(const std::string &p_file, std::set<std::string> &r_seen, std::vector<std::string> &r_chain);
	const std::vector<std::string> &loaded_with(const std::string &p_file);
	std::string preload_file(const std::string &p_path) const;
	std::optional<Type> content_type(const Expr &p_at, const std::string &p_file);
	std::string literal_load_file(const Expr &p_call) const;
	std::optional<Scene> known_scene(const Expr &p_value);
	std::optional<Type> literal_load(const Expr &p_call);
	Type check_preload(Expr &r_expr);
	Type preloaded_class(const Expr &p_preload, const std::string &p_file);
	Type script_value_type(const std::string &p_file) const;
	Type resource_type(const Expr &p_preload, const std::string &p_path, const ResourceInfo &p_resource);
	void check_named_members();
	void check_connections();
	void for_each_attached(const std::function<void(const std::string &, const std::string &)> &p_check);
	void check_attached();
	std::optional<std::vector<Type>> signal_arguments(const SceneNode &p_node, const std::string &p_signal, bool &r_missing, bool &r_hidden);
	void declare_requirement(const std::optional<TypeRef> &p_ref, Type &r_type, const std::string &p_annotation);
	void for_each_placement(const std::function<void(const Scene &, const SceneNode &)> &p_check);
	void check_required(const TokenRange &p_at, const Scene &p_scene, const SceneNode &p_placed, const SceneNode &p_node, const Type &p_required, const std::string &p_what, const std::string &p_annotation);
	void check_required_type(const TokenRange &p_at, const Scene &p_scene, const SceneNode &p_placed, const Type &p_found, const Type &p_required, const std::string &p_what, const std::string &p_annotation);
	void check_owner_scenes();
	void check_parent_scenes();
	void check_stored_exports();
	void check_resource_values();
	void check_scene_exports();
	std::optional<bool> stored_value(const Scene &p_scene, const std::string &p_path, const std::string &p_name, int p_depth = 0);
	std::string undecodable(const Type &p_type, const SceneValue &p_value, const std::string &p_at);
	std::string undecodable_fields(const Type &p_type, const SceneValue &p_value, const std::string &p_at);
	Type stored_type(const SceneValue &p_value);

	/* checker_expressions.cpp */

	Type check_unary(Expr &r_expr);
	static std::string compare_hint(const Type &p_type, bool p_negated);
	Type binary_type(TokenKind p_op, const Type &p_left, const Type &p_right, const TokenRange &p_at, Type *r_compared = nullptr);
	Type primitive_binary_type(TokenKind p_op, const Type &p_left, const Type &p_right, const TokenRange &p_at);
	Type check_binary(Expr &r_expr);
	Type check_none_comparison(Expr &r_expr);
	Type check_in(Expr &r_expr);
	Type check_type_test(Expr &r_expr);
	static bool testable(const Type &p_type);
	Facts facts_of(const Expr &p_condition) const;
	static const Type *narrowed_in(const std::vector<std::unordered_map<int, Type>> &p_narrowed, int p_local);
	void forget(int p_local);
	void narrow_by_value(int p_local, const Type &p_value);
	void add_facts(const std::unordered_map<int, Type> &p_facts);
	void forget_assigned(const Block &p_block);
	static bool always_ends(const Block &p_block, bool p_or_jump);
	Type check_lambda(Expr &r_expr, const Type &p_expected);
	static const FuncDecl *rest_function(const Expr &p_value);
	void check_handler(const Expr &p_handler, const Type &p_signal);
	Type check_option_method(Expr &r_expr, const Type &p_base, const Type &p_expected);
	Type check_some(Expr &r_expr, const Type &p_expected);
	Type check_result_construction(Expr &r_expr, const Type &p_expected);
	Type check_result_method(Expr &r_expr, const Type &p_base);
	std::optional<Type> check_function_argument(Expr &r_call, const std::vector<Type> &p_params, const Type &p_returns = Type::of(TypeKind::Error));
	Type common_type(const Type &p_a, const Type &p_b) const;
	Type check_conditional(Expr &r_expr, const Type &p_expected);
	Type check_subscript(Expr &r_expr);
	Type check_array_literal(Expr &r_expr, const Type &p_expected);
	std::optional<Type> common_element(std::vector<std::unique_ptr<Expr>> &r_values, size_t p_first, size_t p_step, const std::string &p_what);
	Type check_dictionary_literal(Expr &r_expr, const Type &p_expected);
	Type check_cast(Expr &r_expr);
	Type check_super(Expr &r_expr);
	std::string no_await() const;
	Type check_task_function(Expr &r_expr);
	Type check_task_join(Expr &r_expr);
	Type check_await(Expr &r_expr);
	static Type completed(const Type &p_partial, const Type &p_other);
	void add_returned(Type p_value, const TokenRange &p_at);
	static std::string returned_text(const Type &p_type);
	Type check_try(Expr &r_expr);
	Type check_expr(Expr &r_expr, const Type &p_expected = Type::of(TypeKind::Error));
	void report_constant_error(const Expr &p_expr);
	static const Expr &not_constant_part(const Expr &p_expr);
	void require_constant(const Expr &p_expr);
	Type literal_type(const Expr &p_literal, const Type &p_to) const;
	Type type_expr(Expr &r_expr, const Type &p_expected);
	Type check_value(Expr &r_expr, const Type &p_expected = Type::of(TypeKind::Error));
	static std::string variant_hint(const Type &p_to = Type::of(TypeKind::Error));
	bool check_place(const Expr &p_target);
	std::string constant_root(const Expr &p_expr) const;
	bool check_written_back(const Expr &p_target);
	void expect_scene_fits(const Expr &p_value, const Type &p_to);
	std::string unfit_scene(const Scene &p_scene, const Type &p_to);
	std::string encoding_loss(const Type &p_type, const std::string &p_who, bool p_fix, bool p_copied = false, const std::string &p_what = std::string()) const;
	std::string variant_refusal(const Type &p_type) const;
	std::string callable_refusal(const Type &p_function) const;
	void expect_assignable(const Expr &p_value, const Type &p_from, const Type &p_to, const char *p_action);
	void expect_condition(Expr &r_condition);

	/* checker_calls.cpp */

	static std::string argument_count(int p_required, int p_max, bool p_vararg);
	void check_arguments(Expr &r_call, const std::vector<Type> &p_params, int p_required, bool p_vararg, const Type &p_rest = Type::of(TypeKind::Variant));
	std::vector<Type> engine_params(const EngineMethod &p_method, Expr &r_call, bool &r_ok);
	static Type engine_result(const EngineMethod &p_method);
	Type property_type(const EngineProperty &p_property, Expr &r_expr);
	Type engine_method_value(Expr &r_expr, const EngineMethod &p_method);
	Type tried_value(Expr &r_expr, const EngineMethod &p_method, const EngineMethod &p_getter);
	bool receiver_kept(const Expr *p_base, const TokenRange &p_range);
	Type check_engine_call(Expr &r_expr, const EngineMethod &p_method, std::string p_static_class = std::string());
	NamedMember named_member(const Type &p_class, builtin::MemberName::Kind p_kind, const std::string &p_name);
	std::optional<NamedMember> literal_member(Expr &r_call, const builtin::MemberName &p_named, std::vector<std::string> &r_path);
	void check_member_name(Expr &r_call, const EngineMethod &p_method, const builtin::MemberName &p_named);
	std::optional<Type> indexed_type(const Type &p_type, const std::string &p_name);
	Type filtered_result(const Expr &p_call, const EngineMethod &p_method);
	std::optional<std::vector<Type>> api_params(Expr &r_call, const builtin::Argument *p_args, uint32_t p_count, int &r_required);
	bool api_result_supported(Expr &r_call, const Type &p_result);
	Type check_utility(Expr &r_expr, const builtin::Utility &p_utility);
	Type construct_from_variant(Expr &r_expr, const Type &p_type);
	Type check_constructor(Expr &r_expr, const builtin::Class &p_type);
	Type check_named_value(Expr &r_expr, bool p_signal);
	static Type role_type(bytecode::ElementRole p_role, const Type &p_container);
	static void container_typed(const bytecode::ContainerMethod &p_method, const Type &p_container, std::vector<Type> &r_params, Type &r_result);
	bool has_less(const Type &p_type) const;
	bool has_less(const Type &p_type, std::set<std::string> &r_visiting) const;
	bool wraps_to(const Type &p_from, const Type &p_to) const;
	bool check_container_method(const Expr &p_call, const Type &p_base, const bytecode::ContainerMethod *p_container);
	static Type element_function(const std::string &p_method, const Type &p_array);
	Type check_builtin_method(Expr &r_expr, const Type &p_base, bool p_static = false);
	Type check_bind(Expr &r_expr, const Type &p_function);
	Type check_function_call(Expr &r_expr, const Type &p_function);
	std::optional<Type> constructed_struct(const Expr &p_name, bool p_call);
	std::optional<Type> named_type(const Expr &p_base);
	static bool type_ref_of(const Expr &p_expr, TypeRef &r_ref);
	Type check_try_from(Expr &r_expr, const Type &p_type);
	const EnumDecl *payload_enum_named(Expr &r_base);
	Type check_variant_construction(Expr &r_expr, const EnumDecl &p_decl);
	void check_named_fields(Expr &r_expr, const std::vector<VarDecl> &p_fields, const std::string &p_kind, const std::string &p_written, const std::string &p_script, const std::string &p_id);
	Type check_struct_construction(Expr &r_expr, const Type &p_struct);
	Type check_static_call(Expr &r_expr);
	static Type tried_result(const EngineMethod &p_method, const EngineMethod &p_getter);
	bool find_init(Expr &r_expr, const ClassDecl &p_header, const std::string &p_path, const FuncDecl *&r_init);
	Type check_class_call(Expr &r_expr, const std::string &p_path);
	Type check_struct_call(Expr &r_expr, const Type &p_struct, bool p_on_value);
	Type script_base(Expr &r_access, const std::string &p_path);
	Type check_foreign_class_call(Expr &r_expr, const Type &p_class, bool p_value = false);
	Type check_foreign_constant(Expr &r_expr, const Type &p_class);
	Type check_call(Expr &r_expr, const Type &p_expected = Type::of(TypeKind::Error));
	bool split_members(Expr &r_call);
	void check_member_arguments(Expr &r_call, const Type &p_made, bool p_instantiate, const Scene *p_scene = nullptr);
	Type type_call(Expr &r_expr, const Type &p_expected);
	Type type_method_call(Expr &r_expr, const Type &p_expected);
	Type check_range(Expr &r_expr);
	Type check_gdscript_utility(Expr &r_expr);
	Type check_is_instance_of(Expr &r_expr);
	bool written_type(const Expr &p_expr) const;
	Type check_script_test(Expr &r_expr);

	/* checker_statements.cpp */

	std::unordered_map<int, Type> check_block(Block &r_block, const std::unordered_map<int, Type> &p_facts = {});
	int declare_local(const std::string &p_name, uint32_t p_at, const Type &p_type, bool p_constant = false);
	void check_match(Stmt &r_stmt);
	void check_pattern(Pattern &r_pattern, const Type &p_matched, int p_local);
	void check_constructor_pattern(Pattern &r_pattern, const Type &p_matched, int p_local);
	void check_container_pattern(Pattern &r_pattern, const Type &p_matched);
	void check_items(Pattern &r_pattern, const std::vector<std::string> &p_names, const std::vector<Type> &p_types, const std::string &p_written, const std::string &p_field_id);
	std::optional<std::vector<int64_t>> constructors(const Type &p_type) const;
	std::vector<Type> constructor_fields(const Type &p_type, int64_t p_constructor) const;
	Shape shape_of(const Pattern &p_pattern, const Type &p_type) const;
	static std::vector<std::vector<Shape>> specialized(const std::vector<std::vector<Shape>> &p_rows, int64_t p_constructor, size_t p_arity);
	bool useful(const std::vector<std::vector<Shape>> &p_rows, const std::vector<Shape> &p_query, const std::vector<Type> &p_types) const;
	void check_exhaustive(Stmt &r_match, const Type &p_matched);
	bool pattern_fits(const Type &p_pattern, const Type &p_matched) const;
	void check_for(Stmt &r_stmt);
	Type check_iteration(Stmt &r_stmt, const Type &p_object);
	bool predelete_branch(const Expr &p_condition, bool p_true) const;
	bool predelete_branch(const Expr &p_matched, const MatchBranch &p_branch) const;
	void check_stmt(Stmt &r_stmt);

	/* checker_declarations.cpp */

	void declare_members();
	void declare_functions();
	bool is_declared(const FuncDecl &p_function) const;
	static std::string number_text(const ConstValue &p_value);
	const ConstValue *annotation_argument(Expr &r_arg);
	static const HintAnnotation *hint_annotation(const std::string &p_name);
	static bool fits(std::string_view p_target, const Type &p_type);
	void check_hint(VarDecl &r_member, const Annotation &p_annotation, const HintAnnotation &p_hint);
	bool check_flag(const Expr &p_arg, const std::string &p_text, size_t p_index);
	bool check_node_class(const Expr &p_arg, const std::string &p_class);
	void check_export_custom(VarDecl &r_member, const Annotation &p_annotation);
	void check_annotations(VarDecl &r_member);
	void check_rpc(FuncDecl &r_function, const Annotation &p_annotation);
	void check_warning_ignores();
	void check_groups();
	void check_members();
	void check_member_declaration(size_t p_index);
	ConstPtr reflected_initial(const Expr *p_value, const Type &p_type) const;
	ConstPtr encoded_fields(const Type &p_type, const Expr *p_call) const;
	static ConstPtr encoded(const ConstPtr &p_value, const Type &p_type);
	std::string node_field(const Type &p_type, const std::string &p_field, std::unordered_set<std::string> &r_seen) const;
	void check_static_declaration(size_t p_index);
	void check_accessors(VarDecl &r_member);
	void check_defaults(const FuncDecl &p_function);
	static int required_params(const FuncDecl &p_function);
	static Type rest_element(const Type &p_rest);
	static std::optional<Type> argument_type(const FuncDecl &p_function, size_t p_index);
	void untyped(ParamDecl &r_param, const std::string &p_why = std::string());
	void check_rest(const FuncDecl &p_function);
	bool refuse_in_static(const Expr &p_expr, const std::string &p_name);
	std::string function_label() const;
	const FuncDecl *base_function(const std::string &p_name) const;
	bool override_assignable(const Type &p_from, const Type &p_to) const;
	void check_script_override(FuncDecl &r_fn, const FuncDecl &p_base);
	void declare_function(const FuncDecl &p_function, const std::optional<DeclId> &p_overrides);
	void check_abstract(const FuncDecl &p_function);
	void check_implemented();
	void check_engine_constant_names();
	void refuse_new(const FuncDecl &p_function);
	void check_signature(FuncDecl &r_function);
	void declare_params(FuncDecl &r_function, bool p_defaults = false);
	void check_body(FuncDecl &r_function);
	void declare_interfaces();
	void check_interface_parents(const InterfaceDecl &p_interface);
	static std::optional<std::pair<InterfaceMember, uint32_t>> own_interface_member(const InterfaceDecl &p_interface, const Type &p_self, const std::string &p_name);
	bool fits_inherited(const InterfaceMember &p_member, const InterfaceMember &p_inherited, bool p_redeclared) const;
	void check_implements();
	void note_implementation(const Type &p_interface, const TypeRef &p_ref, size_t p_reported);
	void expect_implemented(const Expr &p_value, const Type &p_from, const Type &p_to);
	void check_interface_properties();
	void check_interface_signal(const InterfaceMember &p_member, const TypeRef &p_ref);
	void declare_structs();
	void check_held_cycles();
	void check_field_defaults();
	static std::vector<std::pair<std::string, const VarDecl *>> value_fields(const ClassDecl &p_header, const std::string &p_name);
	bool holds(const ClassDecl &p_decl, const std::string &p_path, const std::string &p_name, const std::string &p_target, std::vector<std::string> &r_cycle, std::unordered_set<std::string> &r_seen);
	static std::vector<Type> held_types(const Type &p_type);
	std::optional<Type> hidden_part(const Type &p_type) const;
	void check_pub_signatures();
	void check_engine_options();
	void declare_signals();
	void type_signal_params(SignalDecl &r_signal);

	/* checker_completion.cpp */

	void engine_candidates(const std::string &p_class, Candidates &r_names) const;
	void class_candidates(const ClassDecl &p_decl, Candidates &r_names) const;
	void script_candidates(const Type &p_class, Candidates &r_names);
	static void builtin_candidates(const builtin::Class &p_type, Candidates &r_names);
	void value_candidates(const Type &p_type, Candidates &r_names);
	static void struct_candidates(const StructDecl &p_decl, Candidates &r_names);
	void scope_candidates(Candidates &r_names) const;
	void add_global_classes(std::map<std::string, CompletionItem> &r_found, bool p_extendable) const;
	void set_completions(std::map<std::string, CompletionItem> &r_found);
	void hint(const Expr &p_call, const std::function<std::string(int)> &p_signature);
	static std::string signature(const std::string &p_name, std::vector<std::string> p_params, bool p_vararg, int p_current, const std::string &p_result);
	static std::vector<std::string> type_names(const std::vector<Type> &p_types);
	static std::string declared_signature(const std::string &p_name, const FuncDecl &p_function, const SourceFile &p_source, int p_current, const Type &p_result);
	static std::string api_signature(const std::string &p_name, const builtin::Argument *p_args, uint32_t p_count, bool p_vararg, const char *p_result, int p_current);
	std::vector<std::string> class_chain(const Type &p_class);
	static CompletionKind value_kind(const Expr &p_checked);
	std::optional<CompletionItem> probe(Expr &r_site, const std::string &p_name, const std::vector<std::string> &p_chain);
	std::optional<CompletionItem> probe_type(const TypeRef &p_site, const std::string &p_name, bool p_allow_void);
	void add_keywords(const Expr &r_site, std::map<std::string, CompletionItem> &r_found);
	void add_enum_values(const Expr &r_site, const Type &p_expected, std::map<std::string, CompletionItem> &r_found);
	void complete(Expr &r_site, const Type &p_expected);
	void complete_type(const TypeRef &p_site, bool p_allow_void);
	void complete_extends();
	void complete_node_path(Expr &r_site);
	void complete_override(const FuncDecl &p_site);
	void complete_string(const EngineMethod *p_method, size_t p_index, bool p_on_self);

public:
	Checker(ClassDecl &r_class, const EngineApi &p_engine, const CompileOptions &p_options, std::vector<Diagnostic> &r_errors, ScriptClasses &r_classes, bool p_check_bodies, std::vector<Reference> *r_references, Completion *r_completion) :
			cls(r_class), engine(p_engine), options(p_options), errors(r_errors), check_bodies(p_check_bodies), classes(r_classes), references(r_references), completion(r_completion) {}

	void attach(FileCheck &r_file, Checker *p_outer) {
		file = &r_file;
		outer_checker = p_outer;
	}

	const std::string &path() const { return options.path; }
	const ClassDecl &decl() const { return cls; }
	bool is_declared_class() const { return declared; }

	void declare();
	void check_init();
	void check_function_bodies();
	void check_scene();
};

} // namespace petrichor::compiler::checking
