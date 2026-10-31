#pragma once

#include "bytecode/opcodes.h"
#include "compiler/lexer.h"
#include "compiler/types.h"

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace petrichor::compiler {

struct TypeRef {
	std::string name;
	int line = 0;
	int column = 0;
	bool nullable = false; // `T?` (spec §3.3, §3.4).
	std::vector<TypeRef> args; // `Array[T]`.
};

enum class ExprKind {
	IntLiteral,
	FloatLiteral,
	StringLiteral,
	BoolLiteral,
	Name,
	Self,
	Await,
	Unary,
	Binary,
	Call,
	Member, // `base.text`
	GetNode, // `$Head/Camera` or `$"Head/Camera"`; the path in `text`.
	Cast, // `left as cast_type` (spec §5.3).
	ArrayLiteral, // `[a, b]`; the elements in `args`.
	// `{k: v}`, or `{name = v}` when `bool_value`, whose keys are names as
	// strings; each key and its value one after another in `args`.
	DictionaryLiteral,
	Subscript, // `base[right]`.
	Conditional, // `left if base else right` (spec §5.5).
	NoneLiteral, // `None`, or `null` (spec §2.2).
	TypeTest, // `left is cast_type`, or `left is not cast_type` when `bool_value`.
};

// What a name, a member access or a call refers to, decided by the checker.
enum class Target {
	None,
	TaskCancel,
	TaskDetach,
	Local,
	Member,
	SelfProperty, // A property of the engine class this class extends.
	Owner, // `owner` with @owner(T) (spec §7.3).
	ObjectProperty, // A property of an engine object.
	Field, // A member of a built-in value, such as x of Vector3.
	StructField, // A field of a struct, by its index in `index` (spec §3.6).
	StructConstructor, // `Hit(damage = 3)`, the struct being the call's type.
	BuiltinConstant, // Such as Vector3.UP.
	ScriptFunction,
	// A method of a class of this language on `base`, of another class or
	// another instance, or of an interface (spec §3.9, §8.1).
	ScriptMethod,
	SuperMethod,
	Noop,
	EngineMethod, // On self, or on `base` when there is one.
	BuiltinMethod,
	BuiltinConstructor,
	NewObject, // `Class.new()` of an engine class.
	Utility,
	ConvertToInt,
	ConvertToFloat,
	Encode, // `Array(hits)` or `Dictionary(d)` of a container that keeps boxes (spec §3.8).
	Unwrap, // `unwrap()` or `expect(message)` of T? (spec §3.4).
	Singleton, // An engine singleton, such as Input, by its class name.
	Autoload, // An autoload by its global name (spec §7.3).
	MethodValue, // A function of this class used as a value (spec §3.11).
	SelfSignal, // A signal of the engine class this class extends, as a value.
	ObjectSignal, // A signal of an engine object.
	BindArguments, // `f.bind(...)` of a function value.
	EmitSignal, // `s.emit(...)` of a signal value.
	EngineConstant, // An integer constant of an engine class, in `int_value`.
	NoneComparison, // `x == None` or `x != None`: `left` or `right` is the None literal.
};

struct Expr {
	ExprKind kind = ExprKind::IntLiteral;
	int line = 0;
	int column = 0;

	int64_t int_value = 0;
	bool int_overflows = false; // 9223372036854775808 without a unary minus.
	double float_value = 0.0;
	bool bool_value = false;
	std::string text; // String literal, name, called method, or node path.

	TokenKind op = TokenKind::EndOfFile; // Unary and binary operators.
	// The operand of a unary, a cast or a type test, the left of a binary,
	// or the value of a conditional when it holds.
	std::unique_ptr<Expr> left;
	// The right of a binary, the value of a conditional when it does not
	// hold, or the index of a subscript.
	std::unique_ptr<Expr> right;
	// What a member is read from, a method called on (null for `f()`) or a
	// subscript indexes; the condition of a conditional.
	std::unique_ptr<Expr> base;
	std::vector<std::unique_ptr<Expr>> args;
	// The name of each argument passed by name, as in `Hit(damage = 3)`, or "";
	// empty when none is.
	std::vector<std::string> arg_names;
	TypeRef cast_type;

	// Filled by the checker.
	Type type;
	Target target = Target::None;
	// Local, member, script function or constructor; for a script method, how
	// many of its last parameters have default values.
	int index = -1;
	std::vector<Type> param_types; // For calls: the type each argument converts to.
	// Built-in methods and utility functions: the engine's signature, which
	// the runtime needs to pass arguments in the right layout.
	uint32_t hash = 0;
	bool is_async = false;
	bool is_foreign = false;
	bool awaited = false;
	bool discarded = false;
	bool is_vararg = false;
	bool is_const = true; // Built-in methods that do not change their base.
	Type engine_return; // Variant where the engine returns a Variant.
	// Built-in methods: the engine's parameter types; script methods: all the
	// method's parameters, also those a call leaves out.
	std::vector<Type> engine_params;
	// Engine methods: for each argument, the range of an engine integer
	// narrower than int64 or unsigned that it goes to (spec §10.6); engine
	// properties: the range of the value their setter takes.
	std::vector<std::optional<bytecode::IntRange>> arg_ranges;
	std::optional<bytecode::IntRange> int_range;
	std::string owner; // The type a field, constant, method or constructor belongs to.
	Type tested; // TypeTest and Cast: the class after is or as.
};

// The type an element of an array literal is stored as: the array's element
// type, or in an untyped array its own.
inline const Type &stored_element_type(const Expr &p_array, size_t p_index) {
	const Type &element = p_array.param_types[p_index];
	return element.kind == TypeKind::Variant ? p_array.args[p_index]->type : element;
}

enum class StmtKind {
	Var,
	Assign,
	If,
	While,
	For, // `for name in value:`, optionally `for name: Type in value:`.
	Return,
	Pass,
	Break,
	Continue,
	Expression,
	Match,
};

struct MatchBranch;

struct Stmt {
	StmtKind kind = StmtKind::Pass;
	int line = 0;
	int column = 0;

	// Var and For: the declared variable.
	std::string name;
	std::optional<TypeRef> declared_type;
	// Assign: `=` or a compound operator such as `+=`.
	TokenKind assign_op = TokenKind::Equal;

	std::unique_ptr<Expr> target;
	std::unique_ptr<Expr> value; // Initializer, assigned value, returned value, iterated value, expression, or what match matches.
	std::unique_ptr<Expr> condition;
	std::vector<std::unique_ptr<Stmt>> body;
	std::vector<std::unique_ptr<Stmt>> else_body; // `elif` becomes a nested `if` here.
	std::vector<MatchBranch> branches;

	// Filled by the checker.
	int local = -1; // Var and For: the declared local; Match: the matched value.
	Type compound_result; // Assign with a compound operator: the type of `target op value`.
	Type element; // For: the type of what is iterated over, before it goes into the variable.
};

using Block = std::vector<std::unique_ptr<Stmt>>;

enum class PatternKind {
	Wildcard, // `_`
	Bind, // `var name`
	Value, // A literal or a constant expression, compared with ==.
};

// A pattern of match (spec §5.6); patterns that nest, such as those of
// arrays, keep theirs in `items`.
struct Pattern {
	PatternKind kind = PatternKind::Wildcard;
	int line = 0;
	int column = 0;
	std::string name; // Bind.
	// Value: the expression, which the checker makes into `matched == value`.
	std::unique_ptr<Expr> value;
	std::vector<Pattern> items;
	int local = -1; // Bind: filled by the checker.
};

struct MatchBranch {
	int line = 0;
	int column = 0;
	std::vector<Pattern> patterns; // Separated by commas.
	std::unique_ptr<Expr> guard; // `when`.
	Block body;
};

// `@name` or `@name(args)` (spec §2.3).
struct Annotation {
	std::string name;
	int line = 0;
	int column = 0;
	std::vector<std::unique_ptr<Expr>> args;
};

struct VarDecl {
	std::string name;
	int line = 0;
	int column = 0;
	bool is_pub = false;
	bool is_export = false;
	bool is_onready = false; // Initialized when the node is ready (spec §6.3).
	std::vector<Annotation> annotations;
	std::optional<TypeRef> declared_type;
	std::unique_ptr<Expr> initializer;
	// Its setter and getter, by function name (spec §4.4): `@set_x` and
	// `@get_x` for inline ones, which the parser adds to the functions.
	std::string setter;
	std::string getter;
	int accessor_line = 0;
	int accessor_column = 0;
	// Filled by the checker.
	Type type;
	int setter_index = -1; // In the class's functions.
	int getter_index = -1;
	bool constant_initializer = false; // Also stored as the member's initial value, the editor's default.
	bool must_assign = false; // An export of a non-null object type without an initial value (spec §7.5).
	uint32_t hint = 0; // For the inspector: PropertyHint as the engine numbers it.
	std::string hint_string;
};

// `@export_group`, `@export_subgroup` or `@export_category`, which stand
// alone and group the exports after them in the inspector.
struct ExportGroupDecl {
	Annotation annotation;
	size_t before_member = 0;
	// Filled by the checker.
	std::string name;
	std::string prefix;
};

struct ParamDecl {
	std::string name;
	int line = 0;
	int column = 0;
	std::optional<TypeRef> declared_type;
	std::unique_ptr<Expr> default_value;
	Type type; // Filled by the checker.
};

struct LocalInfo {
	std::string name;
	Type type;
};

struct FuncDecl {
	std::string name;
	int line = 0;
	int column = 0;
	bool is_pub = false;
	bool is_static = false;
	bool is_async = false;
	std::vector<ParamDecl> params;
	std::optional<TypeRef> return_type_ref;
	Block body;
	// The member whose setter or getter this is (spec §4.4): an inline one
	// from the parser, a named one from the checker.
	std::string accessor_of;

	// Filled by the checker.
	Type return_type = Type::of(TypeKind::Void);
	bool is_override = false;
	bool is_exposed = false; // Connected in a scene (spec §10.5).
	std::vector<LocalInfo> locals; // Parameters first.
};

// `signal name(params)` (spec §4.7).
struct SignalDecl {
	std::string name;
	int line = 0;
	int column = 0;
	bool is_pub = false;
	std::vector<ParamDecl> params;
};

// `var name: T` in an interface, `@readonly` when it need only be readable
// (spec §3.9).
struct InterfaceProperty {
	std::string name;
	int line = 0;
	int column = 0;
	TypeRef type_ref;
	bool is_readonly = false;
	Type type; // Filled by the checker.
};

// `struct Name:` with its fields (spec §3.6), whose initializers are their
// default values.
struct StructDecl {
	std::string name;
	int line = 0;
	int column = 0;
	bool is_pub = false;
	std::vector<VarDecl> fields;

	bool has_default() const {
		for (const VarDecl &field : fields) {
			if (!field.initializer) {
				return false;
			}
		}
		return true;
	}
};

// `interface Name:` with the signatures of its methods and its properties (spec §3.9).
struct InterfaceDecl {
	std::string name;
	int line = 0;
	int column = 0;
	bool is_pub = false;
	std::vector<FuncDecl> methods; // Without bodies.
	std::vector<InterfaceProperty> properties;
};

struct ClassDecl {
	std::string class_name;
	std::string extends = "RefCounted";
	bool extends_path = false;
	std::string path;
	std::string native_base;
	std::string base_script;
	const ClassDecl *base = nullptr;
	// Checked declaration views; their owners live through lowering.
	std::vector<const VarDecl *> all_members;
	std::vector<const FuncDecl *> all_functions;
	std::vector<const SignalDecl *> all_signals;
	std::set<std::string> exposed_methods;
	int extends_line = 1;
	int extends_column = 1;
	std::vector<TypeRef> implements;
	std::optional<TypeRef> owner_ref; // @owner(T) (spec §7.3).
	Type owner_type; // Filled by the checker.
	std::vector<InterfaceDecl> interfaces;
	std::vector<StructDecl> structs;
	std::vector<Type> implemented; // Filled by the checker.
	bool is_node = false; // Filled by the checker: it extends Node, and becomes ready.
	std::vector<VarDecl> members;
	std::vector<FuncDecl> functions;
	std::vector<SignalDecl> signals;
	std::vector<ExportGroupDecl> groups;

	const StructDecl *find_struct(const std::string &p_name) const {
		for (const StructDecl &decl : structs) {
			if (decl.name == p_name) {
				return &decl;
			}
		}
		return nullptr;
	}
};

} // namespace petrichor::compiler
