#pragma once

#include "compiler/lexer.h"
#include "compiler/types.h"

#include <memory>
#include <optional>
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
	Unary,
	Binary,
	Call,
	Member, // `base.text`
	GetNode, // `$Head/Camera` or `$"Head/Camera"`; the path in `text`.
	Cast, // `left as cast_type` (spec §5.3).
	ArrayLiteral, // `[a, b]`; the elements in `args`.
	Subscript, // `base[right]`.
};

// What a name, a member access or a call refers to, decided by the checker.
enum class Target {
	None,
	Local,
	Member,
	SelfProperty, // A property of the engine class this class extends.
	ObjectProperty, // A property of an engine object.
	Field, // A member of a built-in value, such as x of Vector3.
	BuiltinConstant, // Such as Vector3.UP.
	TypeName, // A type used as the base of a member access or a call.
	ScriptFunction,
	EngineMethod, // On self, or on `base` when there is one.
	BuiltinMethod,
	BuiltinConstructor,
	NewObject, // `Class.new()` of an engine class.
	Utility,
	ConvertToInt,
	ConvertToFloat,
	Unwrap, // `unwrap()` or `expect(message)` of T? (spec §3.4).
	Singleton, // An engine singleton, such as Input, by its class name.
	MethodValue, // A function of this class used as a value (spec §3.11).
	SelfSignal, // A signal of the engine class this class extends, as a value.
	ObjectSignal, // A signal of an engine object.
	BindArguments, // `f.bind(...)` of a function value.
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
	std::unique_ptr<Expr> left;          // Operand of a unary, left of a binary.
	std::unique_ptr<Expr> right;
	std::unique_ptr<Expr> base;          // `self` in `self.f()`; null for `f()`.
	std::vector<std::unique_ptr<Expr>> args;
	TypeRef cast_type;

	// Filled by the checker.
	Type type;
	Target target = Target::None;
	int index = -1;                    // Local, member, script function or constructor.
	std::vector<Type> param_types;     // For calls: the type each argument converts to.
	// Built-in methods and utility functions: the engine's signature, which
	// the runtime needs to pass arguments in the right layout.
	uint32_t hash = 0;
	bool is_vararg = false;
	bool is_const = true;              // Built-in methods that do not change their base.
	Type engine_return;                // Variant where the engine returns a Variant.
	std::vector<Type> engine_params;   // Built-in methods: the engine's parameter types.
	std::string owner;                 // The type a field, constant, method or constructor belongs to.
};

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
};

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
	std::unique_ptr<Expr> value; // Initializer, assigned value, returned value, iterated value, or expression.
	std::unique_ptr<Expr> condition;
	std::vector<std::unique_ptr<Stmt>> body;
	std::vector<std::unique_ptr<Stmt>> else_body; // `elif` becomes a nested `if` here.

	// Filled by the checker.
	int local = -1; // Var and For: the declared local.
	Type compound_result; // Assign with a compound operator: the type of `target op value`.
	Type element; // For: the type of what is iterated over, before it goes into the variable.
};

using Block = std::vector<std::unique_ptr<Stmt>>;

struct VarDecl {
	std::string name;
	int line = 0;
	int column = 0;
	bool is_pub = false;
	bool is_export = false;
	std::optional<TypeRef> declared_type;
	std::unique_ptr<Expr> initializer;
	// Filled by the checker.
	Type type;
	bool literal_initializer = false; // Stored as the member's initial value.
};

struct ParamDecl {
	std::string name;
	int line = 0;
	int column = 0;
	std::optional<TypeRef> declared_type;
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
	std::vector<ParamDecl> params;
	std::optional<TypeRef> return_type_ref;
	Block body;

	// Filled by the checker.
	Type return_type = Type::of(TypeKind::Void);
	bool is_override = false;
	bool is_exposed = false; // Connected in a scene (spec §10.5).
	std::vector<LocalInfo> locals; // Parameters first.
};

struct ClassDecl {
	std::string class_name;
	std::string extends = "RefCounted";
	int extends_line = 1;
	int extends_column = 1;
	std::vector<VarDecl> members;
	std::vector<FuncDecl> functions;
};

} // namespace petrichor::compiler
