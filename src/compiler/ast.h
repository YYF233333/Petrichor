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
};

// What a name or a call refers to, decided by the checker.
enum class Target {
	None,
	Local,
	Member,
	ScriptFunction,
	EngineMethod,
	Utility,
	ConvertToInt,
	ConvertToFloat,
};

struct Expr {
	ExprKind kind = ExprKind::IntLiteral;
	int line = 0;
	int column = 0;

	int64_t int_value = 0;
	double float_value = 0.0;
	bool bool_value = false;
	std::string text; // String literal, name, or called method.

	TokenKind op = TokenKind::EndOfFile; // Unary and binary operators.
	std::unique_ptr<Expr> left;          // Operand of a unary, left of a binary.
	std::unique_ptr<Expr> right;
	std::unique_ptr<Expr> base;          // `self` in `self.f()`; null for `f()`.
	std::vector<std::unique_ptr<Expr>> args;

	// Filled by the checker.
	Type type;
	Target target = Target::None;
	int index = -1;                    // Local, member or script function.
	std::vector<Type> param_types;     // For calls: the type each argument converts to.
};

enum class StmtKind {
	Var,
	Assign,
	If,
	While,
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

	// Var.
	std::string name;
	std::optional<TypeRef> declared_type;
	// Assign: `=` or a compound operator such as `+=`.
	TokenKind assign_op = TokenKind::Equal;

	std::unique_ptr<Expr> target;
	std::unique_ptr<Expr> value; // Initializer, assigned value, returned value, or expression.
	std::unique_ptr<Expr> condition;
	std::vector<std::unique_ptr<Stmt>> body;
	std::vector<std::unique_ptr<Stmt>> else_body; // `elif` becomes a nested `if` here.

	int local = -1; // Var: the declared local, filled by the checker.
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
	Type type; // Filled by the checker.
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
