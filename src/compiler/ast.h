#pragma once

#include "bytecode/module.h"
#include "bytecode/opcodes.h"
#include "compiler/constant.h"
#include "compiler/lexer.h"
#include "compiler/types.h"

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace petrichor::compiler {

// The name token of a node the compiler made, which has none.
constexpr uint32_t NO_TOKEN = UINT32_MAX;

struct TypeRef {
	std::string name; // Empty where a syntax error left no type.
	TokenRange range;
	// Of the first part of the name; each further part of Combat.Damageable
	// is two tokens on.
	uint32_t name_token = 0;
	bool nullable = false; // `T?` (spec §3.3, §3.4).
	std::vector<TypeRef> args; // `Array[T]`; a function type's parameters.
	// `func(T) -> R` (spec §2.4), with its return type in `result` when written.
	bool is_function = false;
	bool is_async = false;
	std::vector<TypeRef> result;
};

struct FuncDecl;
struct VarDecl;

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
	GetNode, // `$Head/Camera`, `$"../Level"` or `%Name`; the path in `text`, such as "../Level" or "%Name".
	Cast, // `left as cast_type` (spec §5.3).
	ArrayLiteral, // `[a, b]`; the elements in `args`.
	// `{k: v}`, or `{name = v}` when `bool_value`, whose keys are names as
	// strings; each key and its value one after another in `args`.
	DictionaryLiteral,
	Subscript, // `base[right]`.
	Conditional, // `left if base else right` (spec §5.5).
	NoneLiteral, // `None`, or `null` (spec §2.2).
	TypeTest, // `left is cast_type`, or `left is not cast_type` when `bool_value`.
	// `left?` (spec §8.2): the value inside the Result or the T? in `left`, or a
	// return of its Err or None from the function.
	Try,
	// `func(x): body` (spec §3.11), in `function`; the checker puts what it
	// captures, as read where it is made, in `args`.
	Lambda,
	Error, // What a syntax error left of an expression (architecture §2.1).
};

// What a name, a member access or a call refers to, decided by the checker.
enum class Target {
	None,
	TaskCancel,
	TaskDetach,
	TaskOf, // `Task.of(signal)` (spec §9.5).
	TaskDelay, // `Task.delay(seconds)`.
	TaskSettled, // `t.settled()` (spec §9.2).
	TaskAny, // `Task.any([...])`, the tasks the array literal in args[0] lists (spec §9.5).
	TaskAll, // `Task.all([...])`, likewise.
	Local,
	Member,
	SelfProperty, // A property of the engine class this class extends.
	Owner, // `owner` with @owner(T) (spec §7.3).
	ObjectProperty, // A property of an engine object.
	Field, // A member of a built-in value, such as x of Vector3.
	StructField, // A field of a struct, by its index in `index` (spec §3.6).
	StructConstructor, // `Hit(damage = 3)`, the struct being the call's type.
	// `Command.MOVE(target = p)`: the variant `index` of the call's type, an
	// enum with payloads (spec §3.7).
	VariantConstructor,
	BuiltinConstant, // Such as Vector3.UP.
	ScriptFunction,
	// A method of a class of this language on `base`, of another class or
	// another instance, or of an interface (spec §3.9, §8.1).
	ScriptMethod,
	// `callee`, a method of a struct of the class of this language at
	// `owner`, on `base` unless static (spec §3.6).
	StructMethod,
	SuperMethod,
	Noop,
	EngineMethod, // On self, or on `base` when there is one.
	BuiltinMethod,
	GetOrNone, // get(k) of a Dictionary: V?, None for a missing key (spec §3.8).
	// pop_front() or pop_back() of an untyped Array: Variant?, None only for
	// an empty one, since the engine gives null also for a null element (spec
	// §3.4, §3.8).
	PopOrNone,
	// resize() of an Array[T] whose T has a default the engine does not know,
	// boxed or a typed container (spec §3.8).
	ResizeWithDefaults,
	BuiltinConstructor,
	NewObject, // `Class.new()`, the class being the call's type.
	Utility,
	ConvertToInt,
	ConvertToFloat,
	Encode, // `Array(hits)` or `Dictionary(d)` of a container that keeps boxes (spec §3.8).
	Unwrap, // `unwrap()` or `expect(message)` of T? (spec §3.4).
	OptionMethod, // A method of T? other than those, named by `text`.
	ResultMethod, // A method of Result, named by `text` (spec §3.5).
	TryFrom, // `T.try_from(v)`: Result[T, ConversionError] of the call's type (spec §5.3).
	Singleton, // An engine singleton, such as Input, by its class name.
	Autoload, // An autoload by its global name (spec §7.3).
	MethodValue, // A function of this class used as a value (spec §3.11).
	SelfSignal, // A signal of the engine class this class extends, as a value.
	ObjectSignal, // A signal of an engine object.
	BindArguments, // `f.bind(...)` of a function value.
	CallValue, // `f.call(...)` or `f.callv(args)` of a function value (spec §3.11, §8.1).
	EmitSignal, // `s.emit(...)` of a signal value.
	EngineConstant, // An integer constant of an engine class, or a global one, in `int_value`.
	NoneComparison, // `x == None` or `x != None`: `left` or `right` is the None literal.
	VariantEqual, // `==` or `!=` with a Variant, by the engine's operator table (spec §5.2).
	VariantMatch, // A value pattern matching a Variant, of the pattern's type first, as GDScript matches (§5.6).
	Contains, // `x in c`: the method `text` of the built-in type `owner`, has or contains, taking `param_types[0]`.
	HasMember, // `name in obj`: whether the engine finds a member by that name.
	Constant, // A constant of a class (spec §4.5), whose value is `constant`.
	EnumFromInt, // `State(x)`: the int, checked to be one of the values of the call's type (spec §3.7).
	ConstructFromVariant, // `T(v)` of a Variant: the engine's construction of the call's type (spec §3.13).
	ClassScript, // The script of the class of this language at `owner`, as a value (spec §2.1, §4.1).
	// A static variable, `variable`, of the class of this language at `owner`,
	// the `index`th it declares; `base`, when `static_on_value`, is an object
	// evaluated first (spec §4.8).
	StaticVar,
};

struct Expr {
	ExprKind kind = ExprKind::IntLiteral;
	TokenRange range;
	uint32_t name_token = 0; // Of the name of a Name, a Member or a Call.
	uint32_t op_token = 0; // Of the operator of a Unary or a Binary.

	int64_t int_value = 0;
	bool int_overflows = false; // 9223372036854775808 without a unary minus.
	double float_value = 0.0;
	bool bool_value = false;
	std::string text; // String literal, name, called method, or node path.

	// Unary and binary operators; StringName or NodePath for a string literal
	// with its prefix (spec §2.2).
	TokenKind op = TokenKind::EndOfFile;
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
	std::vector<uint32_t> arg_name_tokens; // Beside `arg_names`; 0 for an argument by position.
	// The members that new(...) or instantiate(...) assigns by name, after
	// the call and in the order written (spec §6.4): their names, the tokens
	// of the names, the values and, once checked, the members' types.
	std::vector<std::string> member_names;
	std::vector<uint32_t> member_name_tokens;
	std::vector<std::unique_ptr<Expr>> member_values;
	std::vector<Type> member_types;
	TypeRef cast_type;
	std::unique_ptr<FuncDecl> function; // Lambda.

	// Filled by the checker.
	Type type;
	Target target = Target::None;
	// Local, member, script function or constructor; for a script method, how
	// many of its last parameters have default values.
	int index = -1;
	// For calls: the type each argument converts to; for comparisons of T? with
	// T, the type each side converts to (spec §5.2).
	std::vector<Type> param_types;
	// Built-in methods and utility functions: the engine's signature, which
	// the runtime needs to pass arguments in the right layout.
	uint32_t hash = 0;
	bool is_async = false;
	// A static method called through the name of its class or type in `owner`,
	// a script's path for a class of this language, with `base` naming it.
	bool is_static = false;
	// A static method of an engine class or a built-in type called on a value,
	// which is evaluated first and not used (spec §4.8).
	bool static_on_value = false;
	bool is_foreign = false;
	bool awaited = false;
	bool discarded = false;
	bool is_vararg = false;
	bool is_const = true; // Built-in methods that do not change their base.
	// A built-in method or `in` through an untyped view of a container, or
	// filling a typed one from an untyped one: the elements are checked as the
	// program runs (spec §3.8).
	bool checks_elements = false;
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
	const VarDecl *variable = nullptr; // StaticVar.
	const FuncDecl *callee = nullptr; // StructMethod.
	// Its value when it is a constant expression (spec §4.5); otherwise null,
	// and when that is because computing it would fail, why, and at which of
	// its parts if not at itself.
	ConstPtr constant;
	std::string constant_error;
	const Expr *constant_error_at = nullptr;
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
	Const, // `const name = value`: a local whose value is a constant expression (spec §4.5).
	Assert, // `assert(condition, value)`, the message `value` optional (spec §8.3).
	Error, // An indented block that no statement opens, in `body`.
};

struct MatchBranch;

struct Stmt {
	StmtKind kind = StmtKind::Pass;
	TokenRange range;

	// Var, Const and For: the declared variable.
	std::string name;
	uint32_t name_token = 0;
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
	int local = -1; // Var, Const and For: the declared local; Match: the matched value.
	Type compound_result; // Assign with a compound operator: the type of `target op value`.
	Type element; // For: the type of what is iterated over, before it goes into the variable.
	// Match: some branch without `when` takes each value, so that the match
	// takes every path (spec §3.7, §5.6).
	bool exhaustive = false;
};

using Block = std::vector<std::unique_ptr<Stmt>>;

enum class PatternKind {
	Wildcard, // `_`
	Bind, // `var name`
	Value, // A literal, a constant expression, a name or an attribute access, compared with ==.
	// `Command.MOVE(target = var t, ..)`, `Hit(var damage)`, `Some(var x)` or
	// `None`: a variant, a struct or a built-in constructor, with patterns of
	// its fields (spec §5.6).
	Constructor,
};

// A pattern of match (spec §5.6); patterns that nest keep theirs in `items`.
struct Pattern {
	PatternKind kind = PatternKind::Wildcard;
	TokenRange range;
	std::string name; // Bind.
	uint32_t name_token = 0;
	// Value: the expression, which the checker makes into `value == pattern`;
	// Constructor: what names it, such as `Command.MOVE`, or None.
	std::unique_ptr<Expr> value;
	// Constructor: the patterns of its fields, each written `field = pattern`
	// with the name in `field`, or by position; `..` leaves out the rest.
	std::vector<Pattern> items;
	std::string field;
	uint32_t field_token = 0;
	bool rest = false;
	// Filled by the checker. Bind: its local; Value: the local that holds the
	// value compared; Constructor: what it matches, `type`, and of an enum
	// with payloads the variant, of T? 0 for None and 1 for Some, with
	// `items` then one for each field in order, `_` for those left out.
	int local = -1;
	Type type;
	int variant = 0;
};

// The call that a constructor pattern reads as, such as `Vector2(1, 2)`, its
// items' values moved into it; null, and the pattern left as it is, when an
// item is no value or names a field, or `..` follows them.
inline std::unique_ptr<Expr> pattern_call(Pattern &r_pattern) {
	for (const Pattern &item : r_pattern.items) {
		if (item.kind != PatternKind::Value || !item.field.empty()) {
			return nullptr;
		}
	}
	if (r_pattern.rest) {
		return nullptr;
	}
	auto call = std::make_unique<Expr>();
	call->kind = ExprKind::Call;
	call->range = r_pattern.range;
	call->text = r_pattern.value->text;
	call->name_token = r_pattern.value->name_token;
	call->base = std::move(r_pattern.value->base);
	for (Pattern &item : r_pattern.items) {
		call->args.push_back(std::move(item.value));
	}
	r_pattern.items.clear();
	r_pattern.value.reset();
	return call;
}

struct MatchBranch {
	TokenRange range;
	std::vector<Pattern> patterns; // Separated by commas.
	std::unique_ptr<Expr> guard; // `when`.
	Block body;
};

// `@name` or `@name(args)` (spec §2.3).
struct Annotation {
	std::string name;
	TokenRange range;
	uint32_t name_token = 0;
	std::vector<std::unique_ptr<Expr>> args;
};

struct VarDecl {
	std::string name;
	TokenRange range;
	uint32_t name_token = 0;
	bool is_pub = false;
	bool is_export = false;
	bool is_onready = false; // Initialized when the node is ready (spec §6.3).
	bool is_static = false; // Of the class, not of an instance (spec §4.8).
	std::vector<Annotation> annotations;
	std::optional<TypeRef> declared_type;
	std::unique_ptr<Expr> initializer;
	// Its setter and getter, by function name (spec §4.4): `@set_x` and
	// `@get_x` for inline ones, which the parser adds to the functions.
	std::string setter;
	std::string getter;
	uint32_t accessor_token = 0; // The first token after the colon that opens them.
	uint32_t setter_token = NO_TOKEN; // Of the function's name in `set = f`.
	uint32_t getter_token = NO_TOKEN;
	// Filled by the checker.
	Type type;
	int setter_index = -1; // In the class's functions.
	int getter_index = -1;
	bool constant_initializer = false; // Also stored as the member's initial value, the editor's default.
	bool must_assign = false; // A member without an initial value whose type has no default (spec §4.3, §6.2).
	uint32_t hint = 0; // For the inspector: PropertyHint as the engine numbers it.
	std::string hint_string;
	uint32_t usage = 0; // PropertyUsageFlags as the engine numbers them, for an export.
};

// `const NAME = value` in a class (spec §4.5).
struct ConstDecl {
	std::string name;
	TokenRange range;
	uint32_t name_token = 0;
	bool is_pub = false;
	std::optional<TypeRef> declared_type;
	std::unique_ptr<Expr> initializer;
	// Filled by the checker, which checks a constant when it is first used.
	Type type;
	ConstPtr value; // Null when the initializer has errors.
	enum class State {
		Unchecked,
		Checking,
		Checked,
	};
	State state = State::Unchecked;
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
	TokenRange range;
	uint32_t name_token = 0;
	std::optional<TypeRef> declared_type;
	std::unique_ptr<Expr> default_value;
	Type type; // Filled by the checker.
};

struct LocalInfo {
	std::string name;
	Type type;
	uint32_t token = NO_TOKEN; // Of its name where declared.
	ConstPtr constant; // A local constant's value (spec §4.5).
};

struct FuncDecl {
	std::string name;
	TokenRange range;
	uint32_t name_token = 0;
	bool is_pub = false;
	bool is_static = false;
	bool is_async = false;
	bool is_abstract = false; // @abstract, without a body (spec §4.1).
	bool has_body = true;
	// A method of a struct (spec §3.6); unless static, its first local is the
	// struct, self, which it does not change.
	bool of_struct = false;
	std::vector<Annotation> annotations; // @rpc.
	std::vector<ParamDecl> params;
	std::optional<TypeRef> return_type_ref;
	Block body;
	// The member whose setter or getter this is (spec §4.4): an inline one
	// from the parser, a named one from the checker.
	std::string accessor_of;

	// Filled by the checker.
	Type return_type = Type::of(TypeKind::Void);
	bool is_override = false;
	std::vector<LocalInfo> locals; // Parameters first.
	bytecode::Rpc rpc;
	// A lambda (spec §3.11): the local each value it captures goes in, in the
	// order of the lambda's `args`, and whether it uses self.
	bool is_lambda = false;
	std::vector<int> captures;
	bool uses_self = false;
	bool checked = false;
};

// `signal name(params)` (spec §4.7).
struct SignalDecl {
	std::string name;
	TokenRange range;
	uint32_t name_token = 0;
	bool is_pub = false;
	std::vector<ParamDecl> params;
};

// `var name: T` in an interface, `@readonly` when it need only be readable
// (spec §3.9).
struct InterfaceProperty {
	std::string name;
	TokenRange range;
	uint32_t name_token = 0;
	TypeRef type_ref;
	bool is_readonly = false;
	Type type; // Filled by the checker.
};

// `struct Name:` with its fields (spec §3.6), whose initializers are their
// default values, and its methods. A method is named after the struct,
// `Hit.total`, as it goes among the class's functions; an instance method
// takes the struct as its first local, `self`.
struct StructDecl {
	std::string name;
	TokenRange range;
	uint32_t name_token = 0;
	bool is_pub = false;
	std::vector<VarDecl> fields;
	std::vector<FuncDecl> methods;

	const FuncDecl *find_method(const std::string &p_name) const {
		for (const FuncDecl &method : methods) {
			if (method.name.size() == name.size() + 1 + p_name.size() && method.name.compare(name.size() + 1, std::string::npos, p_name) == 0) {
				return &method;
			}
		}
		return nullptr;
	}

	bool has_default() const {
		for (const VarDecl &field : fields) {
			if (!field.initializer) {
				return false;
			}
		}
		return true;
	}
};

// The declaration of ConversionError (spec §5.3): `message: String`.
inline const StructDecl &conversion_error_decl() {
	static const StructDecl decl = [] {
		StructDecl result;
		result.name = "ConversionError";
		result.is_pub = true;
		VarDecl message;
		message.name = "message";
		message.type = Type::of(TypeKind::String);
		result.fields.push_back(std::move(message));
		return result;
	}();
	return decl;
}

// A value of an enum, with the value written for it, if any, or a variant
// with the fields of its payload, `MOVE(target: Vector3)` (spec §3.7).
struct EnumValueDecl {
	std::string name;
	TokenRange range;
	uint32_t name_token = 0;
	std::unique_ptr<Expr> value;
	std::vector<VarDecl> fields;
};

// `enum Name { A, B = 2 }`, or an unnamed one, whose values are int constants
// of the class; or one with payloads, `enum Command { IDLE, MOVE(target:
// Vector3) }` (spec §3.7).
struct EnumDecl {
	std::string name; // Empty when unnamed.
	TokenRange range;
	uint32_t name_token = 0;
	bool is_pub = false;
	bool has_payloads = false; // Some variant has fields.
	std::vector<EnumValueDecl> values;
	// Filled by the checker when the enum is first needed.
	enum class State {
		Unchecked,
		Checking,
		Checked,
	};
	State state = State::Unchecked;
	std::string script; // That declares it.
	Type type; // A named enum's, which shares `numbers`; int for an unnamed one.
	std::shared_ptr<EnumValues> numbers; // Filled as `constants` are.
	std::vector<ConstPtr> constants; // Each value's, typed as `type`.
	ConstPtr dictionary; // A named enum's name as a value: its names and values.

	int find(const std::string &p_name) const {
		for (size_t i = 0; i < values.size(); i++) {
			if (values[i].name == p_name) {
				return int(i);
			}
		}
		return -1;
	}
};

// `interface Name:` with the signatures of its methods and its properties (spec §3.9).
struct InterfaceDecl {
	std::string name;
	TokenRange range;
	uint32_t name_token = 0;
	bool is_pub = false;
	std::vector<FuncDecl> methods; // Without bodies.
	std::vector<InterfaceProperty> properties;
};

// The path of an inner class is its file's followed by "::" and the names of
// the classes down to it, as structs and interfaces are named (spec §2.1).
inline std::string script_file(const std::string &p_path) {
	return p_path.substr(0, p_path.find("::"));
}

// "res://a/./b/../c" as "res://a/c", as the engine's String::simplify_path()
// leaves it, so that a path matches the one the project knows the file by.
inline std::string simplify_path(const std::string &p_path) {
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

struct ClassDecl {
	std::shared_ptr<const SourceFile> source; // What the token indices of its nodes refer to.
	// An inner class's name, declared in the class that encloses it (spec
	// §2.1, §4.1); empty for the file's class.
	std::string name;
	uint32_t name_token = NO_TOKEN;
	bool is_pub = false;
	TokenRange range;
	std::string class_name;
	uint32_t class_name_token = NO_TOKEN;
	std::string icon; // @icon("…") on the file's class.
	bool is_abstract = false; // @abstract (spec §4.1).
	std::string extends = "RefCounted";
	bool extends_path = false;
	std::string path;
	std::string native_base;
	std::string base_script;
	const ClassDecl *base = nullptr;
	// Checked declaration views; their owners live through lowering.
	std::vector<const VarDecl *> all_members;
	std::vector<const VarDecl *> all_static_vars; // With those it inherits, first.
	std::vector<const ConstDecl *> all_constants;
	std::vector<const FuncDecl *> all_functions;
	std::vector<const SignalDecl *> all_signals;
	std::set<std::string> exposed_methods;
	uint32_t extends_token = NO_TOKEN; // The base's name or path, if written.
	std::vector<TypeRef> implements;
	std::optional<TypeRef> owner_ref; // @owner(T) (spec §7.3).
	Type owner_type; // Filled by the checker.
	std::vector<std::unique_ptr<ClassDecl>> classes; // Its inner classes.
	std::vector<InterfaceDecl> interfaces;
	std::vector<StructDecl> structs;
	std::vector<EnumDecl> enums;
	std::vector<const EnumDecl *> all_enums; // With those it inherits; filled by the checker.
	std::vector<Type> implemented; // Filled by the checker.
	// Filled by the checker: the interfaces of `implemented` it has errors in
	// implementing, with the line of their `implements`, which other scripts
	// report where they use the class as one (architecture §2.1).
	std::vector<std::pair<Type, int>> implemented_with_errors;
	bool is_node = false; // Filled by the checker: it extends Node, and becomes ready.
	// Filled by the checker: its declarations were checked, whether or not
	// some have errors, as they are not when its base has errors.
	bool declared = false;
	std::vector<ConstDecl> constants;
	std::vector<VarDecl> members;
	std::vector<VarDecl> static_vars; // In declaration order (spec §4.8).
	bool static_unload = false; // @static_unload on the file's class.
	std::vector<FuncDecl> functions;
	std::vector<SignalDecl> signals;
	std::vector<ExportGroupDecl> groups;
	// @warning_ignore, @warning_ignore_start and @warning_ignore_restore,
	// wherever they are.
	std::vector<Annotation> warning_ignores;

	const StructDecl *find_struct(const std::string &p_name) const {
		for (const StructDecl &decl : structs) {
			if (decl.name == p_name) {
				return &decl;
			}
		}
		return nullptr;
	}

	// A named enum, its own or inherited once checked.
	const EnumDecl *find_enum(const std::string &p_name) const {
		for (const EnumDecl &decl : enums) {
			if (decl.name == p_name) {
				return &decl;
			}
		}
		for (const EnumDecl *decl : all_enums) {
			if (decl->name == p_name) {
				return decl;
			}
		}
		return nullptr;
	}

	const ClassDecl *find_class(const std::string &p_name) const {
		for (const std::unique_ptr<ClassDecl> &decl : classes) {
			if (decl->name == p_name) {
				return decl.get();
			}
		}
		return nullptr;
	}
};

// Calls `p_fn` on each direct child AST node of `p_node`, in source order
// within each list. If `p_fn` accepts a second `bool` argument, it is true for
// the first node of each separately stored sibling list.
template <typename Node, typename Fn>
void for_each_child(const Node &p_node, Fn &&p_fn) {
	bool group = true;
	const auto emit = [&](const auto &p_child) {
		if constexpr (std::is_invocable_v<Fn, decltype(p_child), bool>) {
			p_fn(p_child, group);
		} else {
			p_fn(p_child);
		}
		group = false;
	};
	const auto opt = [&](const auto &...p_ptrs) {
		((p_ptrs ? void(emit(*p_ptrs)) : void()), ...);
	};
	const auto list = [&](const auto &...p_vecs) {
		(
				[&](const auto &p_vec) {
					for (const auto &item : p_vec) {
						emit(item);
					}
				}(p_vecs),
				...);
	};
	const auto separate = [&](const auto &...p_vecs) {
		(
				[&](const auto &p_vec) {
					group = true;
					list(p_vec);
				}(p_vecs),
				...);
	};
	const auto ptrs = [&](const auto &...p_vecs) {
		(
				[&](const auto &p_vec) {
					for (const auto &ptr : p_vec) {
						if (ptr) {
							emit(*ptr);
						}
					}
				}(p_vecs),
				...);
	};
	if constexpr (std::is_same_v<Node, TypeRef>) {
		list(p_node.args, p_node.result);
	} else if constexpr (std::is_same_v<Node, Expr>) {
		if (p_node.kind == ExprKind::Conditional) {
			opt(p_node.left, p_node.base, p_node.right);
		} else if (p_node.kind == ExprKind::Cast || p_node.kind == ExprKind::TypeTest) {
			opt(p_node.left);
			emit(p_node.cast_type);
		} else {
			opt(p_node.base, p_node.left, p_node.right);
			ptrs(p_node.args, p_node.member_values);
			opt(p_node.function);
		}
	} else if constexpr (std::is_same_v<Node, Pattern>) {
		opt(p_node.value);
		list(p_node.items);
	} else if constexpr (std::is_same_v<Node, MatchBranch>) {
		list(p_node.patterns);
		opt(p_node.guard);
		ptrs(p_node.body);
	} else if constexpr (std::is_same_v<Node, Stmt>) {
		opt(p_node.declared_type, p_node.target, p_node.condition, p_node.value);
		ptrs(p_node.body, p_node.else_body);
		list(p_node.branches);
	} else if constexpr (std::is_same_v<Node, Annotation>) {
		ptrs(p_node.args);
	} else if constexpr (std::is_same_v<Node, VarDecl>) {
		list(p_node.annotations);
		opt(p_node.declared_type, p_node.initializer);
	} else if constexpr (std::is_same_v<Node, ConstDecl>) {
		opt(p_node.declared_type, p_node.initializer);
	} else if constexpr (std::is_same_v<Node, ParamDecl>) {
		opt(p_node.declared_type, p_node.default_value);
	} else if constexpr (std::is_same_v<Node, FuncDecl>) {
		list(p_node.annotations, p_node.params);
		opt(p_node.return_type_ref);
		ptrs(p_node.body);
	} else if constexpr (std::is_same_v<Node, SignalDecl>) {
		list(p_node.params);
	} else if constexpr (std::is_same_v<Node, InterfaceProperty>) {
		emit(p_node.type_ref);
	} else if constexpr (std::is_same_v<Node, StructDecl>) {
		separate(p_node.fields, p_node.methods);
	} else if constexpr (std::is_same_v<Node, EnumValueDecl>) {
		opt(p_node.value);
		list(p_node.fields);
	} else if constexpr (std::is_same_v<Node, EnumDecl>) {
		list(p_node.values);
	} else if constexpr (std::is_same_v<Node, InterfaceDecl>) {
		separate(p_node.properties, p_node.methods);
	} else if constexpr (std::is_same_v<Node, ClassDecl>) {
		opt(p_node.owner_ref);
		separate(p_node.implements);
		group = true;
		for (const ExportGroupDecl &decl : p_node.groups) {
			emit(decl.annotation);
		}
		separate(p_node.warning_ignores, p_node.members, p_node.static_vars, p_node.constants, p_node.functions, p_node.signals, p_node.structs, p_node.enums, p_node.interfaces);
		group = true;
		ptrs(p_node.classes);
	}
}

} // namespace petrichor::compiler
