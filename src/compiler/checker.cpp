#include "compiler/checker.h"

#include "compiler/builtin_api.h"
#include "compiler/parser.h"
#include "compiler/script_classes.h"

#include <iomanip>
#include <locale>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace petrichor::compiler {

static bool is_string_like(const Type &p_type) {
	return p_type.kind == TypeKind::String || (p_type.kind == TypeKind::Builtin && (p_type.name == "StringName" || p_type.name == "NodePath"));
}

bool is_assignable(const Type &p_from, const Type &p_to, const ClassRelation &p_classes) {
	if (p_from.is_error() || p_to.is_error()) {
		return true;
	}
	if (p_from == p_to) {
		return true;
	}
	if (p_from.kind == TypeKind::None) {
		return p_to.is_optional() || p_to.kind == TypeKind::Variant;
	}
	if (p_from.kind == TypeKind::Int && p_to.kind == TypeKind::Float) {
		return true;
	}
	// Converted where needed, as in GDScript (spec §10.7).
	if (is_string_like(p_from) && is_string_like(p_to) && (p_from.kind == TypeKind::String || p_to.kind == TypeKind::String)) {
		return true;
	}
	if (p_from.kind == TypeKind::Object && p_to.kind == TypeKind::Object) {
		if (!p_to.args.empty()) {
			// PackedScene[T] is covariant in T, and a scene of unknown root is
			// not one (spec §7.4).
			if (p_from.args.empty() || !p_classes.is_a(p_from.args[0], p_to.args[0])) {
				return false;
			}
		}
		return (p_to.nullable || !p_from.nullable) && p_classes.is_a(p_from, p_to);
	}
	if (p_to.kind == TypeKind::Option) {
		// A T is wrapped implicitly; the T inside does not convert (spec §3.4).
		return p_from.kind == TypeKind::Option ? p_from.args == p_to.args : is_assignable(p_from, p_to.args[0], p_classes);
	}
	if (p_from.kind == TypeKind::Function && p_to.kind == TypeKind::Function) {
		// Parameters contravariant, the result covariant (spec §3.11).
		if (p_from.args.size() != p_to.args.size()) {
			return false;
		}
		for (size_t i = 1; i < p_to.args.size(); i++) {
			if (!is_assignable(p_to.args[i], p_from.args[i], p_classes)) {
				return false;
			}
		}
		return p_to.args[0].kind == TypeKind::Void ? p_from.args[0].kind == TypeKind::Void : is_assignable(p_from.args[0], p_to.args[0], p_classes);
	}
	// The engine takes them as its Callable and Signal.
	if ((p_from.kind == TypeKind::Function || p_from.kind == TypeKind::Signal) && p_to.kind == TypeKind::Builtin && p_to.name == p_from.builtin_name()) {
		return true;
	}
	return p_to.kind == TypeKind::Variant && p_from.is_storable();
}

namespace {

const char *operator_name(TokenKind p_op) {
	switch (p_op) {
		case TokenKind::Plus:
			return "+";
		case TokenKind::Minus:
			return "-";
		case TokenKind::Star:
			return "*";
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

// Values that refer to shared engine storage: changing one through a method
// changes it for everyone who holds it, so it is not written back.
bool is_shared(const Type &p_type) {
	// A Signal names an object's signal; connecting changes the object.
	return (p_type.kind == TypeKind::Builtin && (p_type.name == "Array" || p_type.name == "Dictionary")) || p_type.kind == TypeKind::Signal;
}

// The elements of a value that can be indexed and iterated (spec §3.8, §5.7);
// Variant for an untyped Array.
std::optional<Type> element_of(const Type &p_type) {
	if (p_type.kind != TypeKind::Builtin) {
		return std::nullopt;
	}
	if (p_type.name == "Array") {
		return p_type.args.empty() ? Type::of(TypeKind::Variant) : p_type.args[0];
	}
	static const std::unordered_map<std::string, Type> packed = {
		{ "PackedByteArray", Type::of(TypeKind::Int) },
		{ "PackedInt32Array", Type::of(TypeKind::Int) },
		{ "PackedInt64Array", Type::of(TypeKind::Int) },
		{ "PackedFloat32Array", Type::of(TypeKind::Float) },
		{ "PackedFloat64Array", Type::of(TypeKind::Float) },
		{ "PackedStringArray", Type::of(TypeKind::String) },
		{ "PackedVector2Array", Type::builtin("Vector2") },
		{ "PackedVector3Array", Type::builtin("Vector3") },
		{ "PackedColorArray", Type::builtin("Color") },
		{ "PackedVector4Array", Type::builtin("Vector4") },
	};
	const auto found = packed.find(p_type.name);
	return found == packed.end() ? std::nullopt : std::optional<Type>(found->second);
}

// T? of a type: nullable for an object, an Option otherwise.
Type optional_of(const Type &p_type) {
	if (p_type.kind == TypeKind::Object) {
		Type type = p_type;
		type.nullable = true;
		return type;
	}
	return Type::option(p_type);
}

// Gives an expression made by the compiler the position of the code it belongs to.
void relocate(Expr &r_expr, int p_line, int p_column) {
	r_expr.line = p_line;
	r_expr.column = p_column;
	for (Expr *child : { r_expr.left.get(), r_expr.right.get(), r_expr.base.get() }) {
		if (child) {
			relocate(*child, p_line, p_column);
		}
	}
	for (auto &arg : r_expr.args) {
		relocate(*arg, p_line, p_column);
	}
}

bool is_literal(const Expr &p_expr) {
	switch (p_expr.kind) {
		case ExprKind::IntLiteral:
		case ExprKind::FloatLiteral:
		case ExprKind::StringLiteral:
		case ExprKind::BoolLiteral:
			return true;
		case ExprKind::Unary:
			return p_expr.op == TokenKind::Minus && (p_expr.left->kind == ExprKind::IntLiteral || p_expr.left->kind == ExprKind::FloatLiteral);
		default:
			return false;
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
	ScriptClasses &classes; // The project's other scripts of this language.
	std::vector<Scene> scenes; // The scenes that use this script.

	FuncDecl *function = nullptr;
	std::vector<std::unordered_map<std::string, int>> scopes;
	// Types of locals narrowed by conditions (spec §5.4), innermost last.
	std::vector<std::unordered_map<int, Type>> narrowed;
	int loop_depth = 0;
	// The member whose initializer is being checked: it and the members after
	// it are not initialized yet (spec §6.1), nor are @onready members unless
	// it is one too (§6.3).
	int initializing = -1;
	bool initializing_onready = false;

	std::unordered_map<std::string, int> member_index;
	std::unordered_map<std::string, int> function_index;
	std::unordered_map<std::string, int> signal_index;

	void error(int p_line, int p_column, const std::string &p_message) {
		errors.push_back({ p_line, p_column, p_message });
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

	// The class name used in messages and types: class_name, or the file's name.
	std::string display_name(const ClassDecl &p_decl, const std::string &p_path) const {
		if (!p_decl.class_name.empty()) {
			return p_decl.class_name;
		}
		const size_t slash = p_path.find_last_of('/');
		return p_path.substr(slash == std::string::npos ? 0 : slash + 1);
	}

	bool implements(const Type &p_class, const Type &p_interface) const {
		if (p_class.script.empty() || p_class.is_interface) {
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
			return p_from.script == p_to.script; // Classes of this language extend engine classes only, so far.
		}
		return engine.inherits(p_from.engine_class(), p_to.name);
	}

	// Whether an object can be both: `is` and `as` between them are not always false.
	bool related(const Type &p_a, const Type &p_b) const {
		return is_a(p_a, p_b) || is_a(p_b, p_a) || p_a.is_interface || p_b.is_interface;
	}

	// A class of this language named by its class_name, or an interface in
	// one, such as Combat.Damageable; nullopt if the name is not one.
	std::optional<Type> script_type(const TypeRef &p_ref) {
		const size_t dot = p_ref.name.find('.');
		const std::string head = p_ref.name.substr(0, dot);
		std::string path;
		if (!cls.class_name.empty() && head == cls.class_name) {
			path = options.path;
		} else if (options.project) {
			path = options.project->global_class_path(head);
		}
		if (path.empty()) {
			// An interface of this class, by its own name.
			if (dot == std::string::npos) {
				for (const InterfaceDecl &interface : cls.interfaces) {
					if (interface.name == p_ref.name) {
						return Type::interface(display_name(cls, options.path) + "." + interface.name, options.path);
					}
				}
			}
			return std::nullopt;
		}
		if (!is_script_path(path)) {
			error(p_ref.line, p_ref.column, head + " is a class of another language, which is not supported as a type yet (spec §11.1).");
			return Type::of(TypeKind::Error);
		}
		const ClassDecl *header = path == options.path ? &cls : nullptr;
		if (!header) {
			const ScriptClass *script = classes.header(path);
			header = script ? &script->decl : nullptr;
		}
		if (!header) {
			error(p_ref.line, p_ref.column, head + " (" + path + ") cannot be read.");
			return Type::of(TypeKind::Error);
		}
		if (dot == std::string::npos) {
			return Type::script_class(head, path, header->extends);
		}
		const std::string inner = p_ref.name.substr(dot + 1);
		for (const InterfaceDecl &interface : header->interfaces) {
			if (interface.name == inner) {
				if (!interface.is_pub && path != options.path) {
					error(p_ref.line, p_ref.column, p_ref.name + " is not pub (spec §4.2).");
					return Type::of(TypeKind::Error);
				}
				return Type::interface(p_ref.name, path);
			}
		}
		error(p_ref.line, p_ref.column, head + " declares no interface \"" + inner + "\".");
		return Type::of(TypeKind::Error);
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
				error(p_ref.line, p_ref.column, "void cannot be optional.");
				return Type::of(TypeKind::Error);
			}
			// The entry checks would not see its elements' type.
			if (type.kind == TypeKind::Builtin && !type.args.empty()) {
				error(p_ref.line, p_ref.column, type.to_string() + "? is not supported yet; an empty array can stand for none.");
				return Type::of(TypeKind::Error);
			}
			type = optional_of(type);
		}
		return type;
	}

	Type resolve_name(const TypeRef &p_ref, bool p_allow_void) {
		if (!p_ref.args.empty()) {
			return resolve_generic(p_ref);
		}
		if (p_ref.name == "void") {
			if (p_allow_void) {
				return Type::of(TypeKind::Void);
			}
			error(p_ref.line, p_ref.column, "void is only valid as a return type.");
			return Type::of(TypeKind::Error);
		}
		if (p_ref.name == "Variant" || p_ref.name == "Nil") {
			error(p_ref.line, p_ref.column, "The type " + p_ref.name + " is not supported yet.");
			return Type::of(TypeKind::Error);
		}
		if (builtin::find_class(p_ref.name)) {
			return api_type(p_ref.name, false);
		}
		if (engine.class_exists(p_ref.name)) {
			return Type::object(p_ref.name);
		}
		if (const std::optional<Type> script = script_type(p_ref)) {
			return *script;
		}
		error(p_ref.line, p_ref.column, "Unknown type \"" + p_ref.name + "\".");
		return Type::of(TypeKind::Error);
	}

	// `Array[T]`, the engine's typed array (spec §3.8).
	Type resolve_generic(const TypeRef &p_ref) {
		if (p_ref.name == "Dictionary") {
			error(p_ref.line, p_ref.column, "Dictionary[K, V] is not supported yet.");
			return Type::of(TypeKind::Error);
		}
		if (p_ref.name == "PackedScene" && p_ref.args.size() == 1) {
			// A scene whose root has type T (spec §7.4).
			const Type root = resolve(p_ref.args[0], false);
			if (root.is_error()) {
				return root;
			}
			if (root.kind != TypeKind::Object || root.nullable || !is_a(root, Type::object("Node"))) {
				error(p_ref.args[0].line, p_ref.args[0].column, "The root of a scene is a node, not " + root.to_string() + ".");
				return Type::of(TypeKind::Error);
			}
			Type scene = Type::object("PackedScene");
			scene.args.push_back(root);
			return scene;
		}
		if (p_ref.name != "Array") {
			error(p_ref.line, p_ref.column, p_ref.name + " takes no type arguments.");
			return Type::of(TypeKind::Error);
		}
		if (p_ref.args.size() != 1) {
			error(p_ref.line, p_ref.column, "Array takes one element type.");
			return Type::of(TypeKind::Error);
		}
		const TypeRef &element_ref = p_ref.args[0];
		const Type element = resolve(element_ref, false);
		if (element.is_error()) {
			return element;
		}
		// The engine types an array with one built-in type or one class.
		if (element.kind == TypeKind::Option || !element.args.empty()) {
			error(element_ref.line, element_ref.column, "Arrays of " + element.to_string() + " are not supported yet.");
			return Type::of(TypeKind::Error);
		}
		return Type::array(element);
	}

	static Type function_type(const FuncDecl &p_function) {
		return Type::function(p_function.return_type, param_types(p_function.params));
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
		error(r_expr.line, r_expr.column, p_message);
		return set(r_expr, Type::of(TypeKind::Error));
	}

	int find_local(const std::string &p_name) const {
		for (auto scope = scopes.rbegin(); scope != scopes.rend(); ++scope) {
			const auto found = scope->find(p_name);
			if (found != scope->end()) {
				return found->second;
			}
		}
		return -1;
	}

	// The type a name refers to when it is the base of `Name.x` or
	// `Name.f()`: not when a local, a member or a property has that name.
	bool names_type(const Expr &p_expr) const {
		return p_expr.kind == ExprKind::Name && find_local(p_expr.text) < 0 && !member_index.count(p_expr.text) &&
				!engine.find_property(cls.extends, p_expr.text) && is_type_name(p_expr.text) && !engine.is_singleton(p_expr.text);
	}

	Type check_name(Expr &r_expr) {
		const int local = find_local(r_expr.text);
		if (local >= 0) {
			r_expr.target = Target::Local;
			r_expr.index = local;
			const Type *narrowed_to = narrowed_type(local);
			return set(r_expr, narrowed_to ? *narrowed_to : function->locals[local].type);
		}
		const auto member = member_index.find(r_expr.text);
		if (member != member_index.end()) {
			const VarDecl &read = cls.members[member->second];
			if (initializing >= 0) {
				// @onready initializers run when the node is ready, after the
				// others (spec §6.3); each kind runs in declaration order (§6.1).
				const bool ready = read.is_onready ? initializing_onready && member->second < initializing : !initializing_onready ? member->second < initializing : true;
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
			return set(r_expr, cls.members[member->second].type);
		}
		if (const auto property = engine.find_property(cls.extends, r_expr.text)) {
			if (!property->type.is_storable()) {
				return fail(r_expr, "The property \"" + r_expr.text + "\" has type " + property->type.to_string() + ", which is not supported yet.");
			}
			if (refuse_in_static(r_expr, r_expr.text)) {
				return set(r_expr, Type::of(TypeKind::Error));
			}
			r_expr.target = Target::SelfProperty;
			return set(r_expr, property_type(*property));
		}
		const auto function_found = function_index.find(r_expr.text);
		if (function_found != function_index.end()) {
			// A method value (spec §3.11), which calls the function on this instance.
			if (refuse_in_static(r_expr, r_expr.text)) {
				return set(r_expr, Type::of(TypeKind::Error));
			}
			r_expr.target = Target::MethodValue;
			r_expr.index = function_found->second;
			return set(r_expr, function_type(cls.functions[function_found->second]));
		}
		const auto own_signal = signal_index.find(r_expr.text);
		if (own_signal != signal_index.end()) {
			if (refuse_in_static(r_expr, r_expr.text)) {
				return set(r_expr, Type::of(TypeKind::Error));
			}
			r_expr.target = Target::SelfSignal;
			return set(r_expr, Type::signal(param_types(cls.signals[own_signal->second].params)));
		}
		if (const auto signal = engine.find_signal(cls.extends, r_expr.text)) {
			if (refuse_in_static(r_expr, r_expr.text)) {
				return set(r_expr, Type::of(TypeKind::Error));
			}
			r_expr.target = Target::SelfSignal;
			return set(r_expr, signal_type(*signal));
		}
		if (engine.is_singleton(r_expr.text)) {
			r_expr.target = Target::Singleton;
			return set(r_expr, Type::object(r_expr.text));
		}
		const std::string autoload = options.project ? options.project->autoload_path(r_expr.text) : std::string();
		if (!autoload.empty()) {
			// Its type is its script's class (spec §7.3).
			if (!is_script_path(autoload)) {
				return fail(r_expr, "The autoload " + r_expr.text + " is a script of another language, which is not supported as a type yet (spec §11.1).");
			}
			const ScriptClass *script = autoload == options.path ? nullptr : classes.header(autoload);
			const ClassDecl *header = autoload == options.path ? &cls : (script ? &script->decl : nullptr);
			if (!header) {
				return fail(r_expr, "The autoload " + r_expr.text + " (" + autoload + ") cannot be read.");
			}
			r_expr.target = Target::Autoload;
			return set(r_expr, Type::script_class(display_name(*header, autoload), autoload, header->extends));
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
		const ClassDecl *decl = script_decl(p_class.script);
		if (!decl) {
			return fail(r_expr, p_class.script + " has errors.");
		}
		if (p_class.is_interface) {
			return std::nullopt; // Interfaces declare methods only, so far.
		}
		for (const VarDecl &member : decl->members) {
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
			return member.type;
		}
		for (const SignalDecl &signal : decl->signals) {
			if (signal.name != r_expr.text) {
				continue;
			}
			// A signal is reached by name like a method, and the engine knows
			// every signal a class declares, so only other classes are refused.
			if (!signal.is_pub && p_class.script != options.path) {
				return fail(r_expr, not_pub(p_class, "The signal \"" + signal.name + "\""));
			}
			r_expr.target = Target::ObjectSignal;
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
		const std::string name = p_interface.name.substr(p_interface.name.find_last_of('.') + 1);
		for (const InterfaceDecl &interface : decl->interfaces) {
			if (interface.name == name) {
				return &interface;
			}
		}
		return nullptr;
	}

	// A pub method of an object of a class of this language, or a method of an
	// interface, which this language calls directly (spec §8.1); an override
	// of an engine virtual method is called through the engine, which runs it.
	std::optional<Type> script_method(Expr &r_expr, const Type &p_class) {
		const FuncDecl *callee = nullptr;
		if (p_class.is_interface) {
			if (const InterfaceDecl *interface = interface_decl(p_class)) {
				for (const FuncDecl &method : interface->methods) {
					if (method.name == r_expr.text) {
						callee = &method;
					}
				}
			}
		} else if (const ClassDecl *decl = script_decl(p_class.script)) {
			for (const FuncDecl &function : decl->functions) {
				if (function.name == r_expr.text) {
					callee = &function;
				}
			}
		} else {
			return fail(r_expr, p_class.script + " has errors.");
		}
		if (!callee) {
			return std::nullopt;
		}
		if (!callee->is_pub && !callee->is_override) {
			for (auto &arg : r_expr.args) {
				check_value(*arg);
			}
			return fail(r_expr, not_pub(p_class, "\"" + callee->name + "()\""));
		}
		check_arguments(r_expr, param_types(callee->params), required_params(*callee), false);
		if (callee->is_override) {
			r_expr.target = Target::EngineMethod;
		} else {
			r_expr.target = Target::ScriptMethod;
			r_expr.engine_params = param_types(callee->params);
			r_expr.index = int(callee->params.size()) - required_params(*callee);
		}
		return set(r_expr, callee->return_type);
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
		return set(r_expr, Type::of(TypeKind::Int));
	}

	Type check_member(Expr &r_expr) {
		Expr &base = *r_expr.base;
		if (names_type(base)) {
			base.target = Target::TypeName;
			if (const builtin::Class *type = builtin::find_class(base.text)) {
				if (const builtin::Constant *constant = builtin::find_constant(*type, r_expr.text)) {
					r_expr.target = Target::BuiltinConstant;
					r_expr.owner = base.text;
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
		const std::string builtin_name = base_type.builtin_name();
		if (!builtin_name.empty()) {
			const builtin::Class *type = builtin::find_class(builtin_name);
			const builtin::Member *member = type ? builtin::find_member(*type, r_expr.text) : nullptr;
			if (!member) {
				return fail(r_expr, base_type.to_string() + " has no member \"" + r_expr.text + "\".");
			}
			r_expr.target = Target::Field;
			r_expr.owner = builtin_name;
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
					return set(r_expr, signal_type(*signal));
				}
				if (base.target == Target::Singleton && engine.find_constant(engine_class, r_expr.text)) {
					return check_engine_constant(r_expr, engine_class); // Such as Input.MOUSE_MODE_CAPTURED.
				}
				return fail(r_expr, base_type.name + " has no property \"" + r_expr.text + "\".");
			}
			if (!property->type.is_storable()) {
				return fail(r_expr, "The property \"" + r_expr.text + "\" has type " + property->type.to_string() + ", which is not supported yet.");
			}
			r_expr.target = Target::ObjectProperty;
			r_expr.owner = engine_class;
			return set(r_expr, property_type(*property));
		}
		return fail(r_expr, "Values of type " + base_type.to_string() + " have no members.");
	}

	// `$X` (spec §7.1): the node at the path from every node that has this
	// script, in every scene; they must agree on its class.
	Type check_get_node(Expr &r_expr) {
		if (options.declarations_only) {
			return set(r_expr, Type::of(TypeKind::Error)); // Scenes are not read while loading.
		}
		if (refuse_in_static(r_expr, "$" + r_expr.text)) {
			return set(r_expr, Type::of(TypeKind::Error));
		}
		const std::string &path = r_expr.text;
		if (path.empty() || path[0] == '/' || path.find('%') != std::string::npos || path == ".." || path.rfind("../", 0) == 0 || path.find("/..") != std::string::npos) {
			return fail(r_expr, "Only paths down from this node, such as $Head/Camera, are supported yet.");
		}
		bool attached = false;
		std::optional<Type> type;
		std::string type_scene;
		for (const Scene &scene : scenes) {
			for (const SceneNode &node : scene.nodes) {
				if (node.script != options.path) {
					continue;
				}
				attached = true;
				const std::string full = node.path == "." ? path : node.path + "/" + path;
				const SceneNode *found = scene.find_node(full);
				if (!found) {
					for (size_t slash = full.find('/'); slash != std::string::npos; slash = full.find('/', slash + 1)) {
						const SceneNode *outer = scene.find_node(full.substr(0, slash));
						if (outer && outer->is_instance) {
							return fail(r_expr, scene.path + ": paths into an instanced scene, such as " + outer->path + ", are not supported yet.");
						}
					}
					error(r_expr.line, r_expr.column, scene.path + ": " + (node.path == "." ? std::string("the root") : node.path) + " has no node at \"" + path + "\" (spec §7.1).");
					continue;
				}
				const Type found_type = scene_node_type(r_expr, *found, "$" + path);
				if (found_type.is_error()) {
					return set(r_expr, found_type);
				}
				if (!type) {
					type = found_type;
					type_scene = scene.path;
				} else if (found_type != *type) {
					return fail(r_expr, "$" + path + " is a " + type->to_string() + " in " + type_scene + " but a " + found_type.to_string() + " in " + scene.path + "; assign it to a variable typed with a class both extend (spec §7.1).");
				}
			}
		}
		if (!attached) {
			return fail(r_expr, "$" + path + " is typed from the scenes that use this script, and no scene does (spec §7.2).");
		}
		if (!type) {
			return set(r_expr, Type::of(TypeKind::Error)); // Reported for each scene above.
		}
		r_expr.owner = type->engine_class();
		return set(r_expr, *type);
	}

	// The type of a node of a scene (spec §7.1, §7.4): the class of its script
	// when that is a script of this language, or else its engine class; Error
	// after reporting. `p_what` names the node in messages.
	Type scene_node_type(const Expr &p_at, const SceneNode &p_node, const std::string &p_what) {
		const std::string &script = p_node.script.empty() ? p_node.instance_script : p_node.script;
		if (is_script_path(script)) {
			const ScriptClass *other = script == options.path ? nullptr : classes.header(script);
			const ClassDecl *header = script == options.path ? &cls : (other ? &other->decl : nullptr);
			if (!header) {
				error(p_at.line, p_at.column, p_what + " has the script " + script + ", which cannot be read.");
				return Type::of(TypeKind::Error);
			}
			return Type::script_class(display_name(*header, script), script, header->extends);
		}
		if (!engine.class_exists(p_node.type)) {
			error(p_at.line, p_at.column, p_what + " is a " + p_node.type + ", which is not supported yet.");
			return Type::of(TypeKind::Error);
		}
		return Type::object(p_node.type);
	}

	// preload() of a scene, typed PackedScene[T] by the scene's root (spec
	// §7.4). It loads when it runs, through the engine's resource cache, as
	// load() does, and the result is checked to be a scene.
	Type check_preload(Expr &r_expr) {
		if (r_expr.args.size() != 1 || r_expr.args[0]->kind != ExprKind::StringLiteral) {
			for (auto &arg : r_expr.args) {
				check_value(*arg);
			}
			return fail(r_expr, "preload() takes one literal path.");
		}
		std::string path = r_expr.args[0]->text;
		if (path.rfind("res://", 0) != 0) {
			// Relative to the script's directory, as in GDScript.
			path = options.path.substr(0, options.path.find_last_of('/') + 1) + path;
		}
		path = simplify_path(path);
		r_expr.args[0]->text = path;
		if (options.declarations_only) {
			return set(r_expr, Type::of(TypeKind::Error)); // Scenes are not read while loading.
		}
		const bool is_scene = path.size() > 5 && (path.compare(path.size() - 5, 5, ".tscn") == 0 || path.compare(path.size() - 4, 4, ".scn") == 0);
		if (!is_scene) {
			return fail(r_expr, "preload() of a resource other than a scene is not supported yet.");
		}
		const std::optional<Scene> scene = options.project ? options.project->scene(path) : std::nullopt;
		const SceneNode *root = scene ? scene->find_node(".") : nullptr;
		if (!root) {
			return fail(r_expr, "There is no scene at " + path + ".");
		}
		const Type root_type = scene_node_type(r_expr, *root, "The root of " + path);
		if (root_type.is_error()) {
			return set(r_expr, root_type);
		}
		if (!engine.is_singleton("ResourceLoader")) {
			return fail(r_expr, "preload() needs the engine's ResourceLoader.");
		}
		auto loader = std::make_unique<Expr>();
		loader->kind = ExprKind::Name;
		loader->text = "ResourceLoader";
		loader->line = r_expr.line;
		loader->column = r_expr.column;
		r_expr.base = std::move(loader);
		r_expr.text = "load";
		if (check_call(r_expr).is_error()) {
			return set(r_expr, Type::of(TypeKind::Error));
		}
		Type type = Type::object("PackedScene");
		type.args.push_back(root_type);
		return set(r_expr, type);
	}

	Type check_unary(Expr &r_expr) {
		const Type operand = check_value(*r_expr.left);
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
		if (operand.kind == TypeKind::Bool) {
			return set(r_expr, operand);
		}
		return fail(r_expr, "\"not\" requires bool, found " + operand.to_string() + ". Compare explicitly, for example x == 0.");
	}

	// The result of `p_left op p_right` (spec §5.1), or Error after reporting.
	Type binary_type(TokenKind p_op, const Type &p_left, const Type &p_right, int p_line, int p_column) {
		if (p_left.is_error() || p_right.is_error()) {
			return Type::of(TypeKind::Error);
		}
		if (p_left.kind == TypeKind::Option || p_right.kind == TypeKind::Option) {
			const Type &optional = p_left.kind == TypeKind::Option ? p_left : p_right;
			error(p_line, p_column, "The value may be None (" + optional.to_string() + "); unwrap it first.");
			return Type::of(TypeKind::Error);
		}
		if (p_op == TokenKind::And || p_op == TokenKind::Or) {
			if (p_left.kind == TypeKind::Bool && p_right.kind == TypeKind::Bool) {
				return p_left;
			}
			error(p_line, p_column, std::string(token_name(p_op)) + " requires bool operands, found " + p_left.to_string() + " and " + p_right.to_string() + ".");
			return Type::of(TypeKind::Error);
		}
		if (p_left.is_primitive() && p_right.is_primitive()) {
			return primitive_binary_type(p_op, p_left, p_right, p_line, p_column);
		}
		// Other value types follow the engine's operator table.
		const std::string left = p_left.builtin_name();
		const std::string right = p_right.builtin_name();
		const bool equality = p_op == TokenKind::EqualEqual || p_op == TokenKind::BangEqual;
		if (equality && left == "Array" && right == "Array" && !p_left.args.empty() && !p_right.args.empty() && p_left.args[0] != p_right.args[0]) {
			// The engine compares elements only of the same type (spec §5.2).
			error(p_line, p_column, p_left.to_string() + " and " + p_right.to_string() + " are equal only when both are empty, so comparing them is an error.");
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
		error(p_line, p_column, std::string("Invalid operands ") + p_left.to_string() + " and " + p_right.to_string() + " for " + token_name(p_op) + ".");
		return Type::of(TypeKind::Error);
	}

	Type primitive_binary_type(TokenKind p_op, const Type &p_left, const Type &p_right, int p_line, int p_column) {
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
					error(p_line, p_column, "\"%\" requires int operands; use fmod() for float.");
					return Type::of(TypeKind::Error);
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
				error(p_line, p_column, "Cannot compare " + p_left.to_string() + " with " + p_right.to_string() + ".");
				return Type::of(TypeKind::Error);
			default:
				break;
		}
		error(p_line, p_column, std::string("Invalid operands ") + p_left.to_string() + " and " + p_right.to_string() + " for " + token_name(p_op) + ".");
		return Type::of(TypeKind::Error);
	}

	Type check_binary(Expr &r_expr) {
		const bool equality = r_expr.op == TokenKind::EqualEqual || r_expr.op == TokenKind::BangEqual;
		if (equality && (r_expr.left->kind == ExprKind::NoneLiteral || r_expr.right->kind == ExprKind::NoneLiteral)) {
			return check_none_comparison(r_expr);
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
		return set(r_expr, binary_type(r_expr.op, left, right, r_expr.line, r_expr.column));
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
	const Type *narrowed_type(int p_local) const {
		for (auto map = narrowed.rbegin(); map != narrowed.rend(); ++map) {
			const auto found = map->find(p_local);
			if (found != map->end()) {
				return &found->second;
			}
		}
		return nullptr;
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
		}
	}

	// Leaves the block on every path: return, break or continue.
	static bool always_exits(const Block &p_block) {
		if (p_block.empty()) {
			return false;
		}
		const Stmt &last = *p_block.back();
		if (last.kind == StmtKind::Return || last.kind == StmtKind::Break || last.kind == StmtKind::Continue) {
			return true;
		}
		return last.kind == StmtKind::If && always_exits(last.body) && always_exits(last.else_body);
	}

	void report_argument(const Expr &p_arg, const Type &p_arg_type, const Type &p_param) {
		std::string message = "Cannot pass a value of type " + p_arg_type.to_string() + " as " + p_param.to_string() + ".";
		if (p_arg_type.kind == TypeKind::Float && p_param.kind == TypeKind::Int) {
			message += " Convert explicitly with int(x).";
		}
		error(p_arg.line, p_arg.column, message);
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
			error(r_call.line, r_call.column, "Function \"" + r_call.text + "()\" takes " + expected + " argument(s), " + std::to_string(count) + " given.");
		}
		for (int i = 0; i < count; i++) {
			Expr &arg = *r_call.args[i];
			const Type param = i < max ? p_params[i] : Type::of(TypeKind::Variant);
			const Type arg_type = check_value(arg, param);
			r_call.param_types.push_back(param);
			if (!assignable(arg_type, param)) {
				report_argument(arg, arg_type, param);
			}
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

	static Type property_type(const EngineProperty &p_property) {
		Type type = p_property.type;
		if (type.kind == TypeKind::Object && builtin::property_non_null(p_property.owner, p_property.name)) {
			type.nullable = false;
		}
		return type;
	}

	Type check_engine_call(Expr &r_expr, const EngineMethod &p_method) {
		if (p_method.is_static) {
			return fail(r_expr, "Static engine methods are not supported yet.");
		}
		if (p_method.is_virtual) {
			return fail(r_expr, "\"" + r_expr.text + "()\" is a virtual engine method.");
		}
		if (p_method.return_type.kind == TypeKind::Unsupported) {
			return fail(r_expr, "\"" + r_expr.text + "()\" returns " + p_method.return_type.to_string() + ", which is not supported yet.");
		}
		bool ok = false;
		const std::vector<Type> params = engine_params(p_method, r_expr, ok);
		if (!ok) {
			return Type::of(TypeKind::Error);
		}
		check_arguments(r_expr, params, int(params.size()) - p_method.default_count, p_method.is_vararg);
		r_expr.target = Target::EngineMethod;
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
		} else if (is_script_path(path)) {
			TypeRef ref;
			ref.name = name;
			ref.line = arg.line;
			ref.column = arg.column;
			const std::optional<Type> script = script_type(ref);
			if (!script || script->is_error()) {
				return Type::of(TypeKind::Error);
			}
			element = *script;
		} else if (!path.empty()) {
			return result; // A class of another language.
		} else {
			error(arg.line, arg.column, p_method.name + "() filters by the class \"" + name + "\", which does not exist.");
			return Type::of(TypeKind::Error);
		}
		if (!is_a(element, result.args[0])) {
			error(arg.line, arg.column, p_method.name + "() returns objects of " + result.args[0].to_string() + ", and " + element.to_string() + " is not one.");
			return Type::of(TypeKind::Error);
		}
		return Type::array(element);
	}

	Type check_utility(Expr &r_expr, const builtin::Utility &p_utility) {
		r_expr.target = Target::Utility;
		r_expr.hash = p_utility.hash;
		r_expr.is_vararg = p_utility.is_vararg;
		r_expr.engine_return = api_type(p_utility.return_type, true);
		const ArgumentTyped rule = argument_typed(r_expr.text);
		if (rule == ArgumentTyped::None) {
			std::vector<Type> params;
			for (uint32_t i = 0; i < p_utility.argument_count; i++) {
				params.push_back(api_type(p_utility.arguments[i].type, true));
			}
			for (const Type &type : params) {
				if (type.kind == TypeKind::Object || !type.is_storable()) {
					return fail(r_expr, "\"" + r_expr.text + "()\" takes " + type.to_string() + ", which is not supported yet.");
				}
			}
			if (r_expr.engine_return.kind == TypeKind::Object || r_expr.engine_return.kind == TypeKind::Unsupported) {
				return fail(r_expr, "\"" + r_expr.text + "()\" returns " + r_expr.engine_return.to_string() + ", which is not supported yet.");
			}
			int required = 0;
			for (uint32_t i = 0; i < p_utility.argument_count; i++) {
				required += p_utility.arguments[i].has_default ? 0 : 1;
			}
			check_arguments(r_expr, params, required, p_utility.is_vararg);
			return set(r_expr, r_expr.engine_return);
		}

		// Typed by the arguments (spec §10.3); the engine takes them as Variant.
		const size_t expected = rule == ArgumentTyped::Lerp ? 3 : p_utility.argument_count;
		if (r_expr.args.size() < expected || (!p_utility.is_vararg && r_expr.args.size() > expected)) {
			for (auto &arg : r_expr.args) {
				check_value(*arg);
			}
			return fail(r_expr, "Function \"" + r_expr.text + "()\" takes " + (p_utility.is_vararg ? "at least " : "") + std::to_string(expected) + " argument(s).");
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
		std::vector<Type> args;
		for (auto &arg : r_expr.args) {
			args.push_back(check_value(*arg));
		}
		for (const Type &type : args) {
			if (type.is_error()) {
				return set(r_expr, type);
			}
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
			r_expr.param_types.push_back(api_type(constructor.arguments[a].type, false));
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

	Type check_builtin_method(Expr &r_expr, const Type &p_base) {
		const std::string base_name = p_base.builtin_name();
		const builtin::Class *type = builtin::find_class(base_name);
		const builtin::Method *method = type ? builtin::find_method(*type, r_expr.text) : nullptr;
		if (!method) {
			return fail(r_expr, p_base.to_string() + " has no method \"" + r_expr.text + "()\".");
		}
		if (method->is_vararg || method->is_static) {
			return fail(r_expr, "\"" + r_expr.text + "()\" of " + base_name + " is not supported yet.");
		}
		std::vector<Type> params;
		int required = 0;
		for (uint32_t i = 0; i < method->argument_count; i++) {
			const Type param = api_type(method->arguments[i].type, true);
			if (param.kind == TypeKind::Object || !param.is_storable()) {
				return fail(r_expr, "\"" + r_expr.text + "()\" takes " + param.to_string() + ", which is not supported yet.");
			}
			params.push_back(param);
			required += method->arguments[i].has_default ? 0 : 1;
		}
		// The engine's default values are source, such as `true` or
		// `Vector3(0, 1, 0)`, checked and passed as if written at the call.
		for (size_t i = r_expr.args.size(); int(r_expr.args.size()) >= required && i < params.size(); i++) {
			std::vector<Diagnostic> ignored;
			std::unique_ptr<Expr> value = parse_expression(method->arguments[i].default_value, ignored);
			if (!value) {
				return fail(r_expr, "The default value of \"" + std::string(method->arguments[i].name) + "\" of \"" + r_expr.text + "()\" is not supported yet; pass it.");
			}
			relocate(*value, r_expr.line, r_expr.column);
			r_expr.args.push_back(std::move(value));
		}
		Type result = api_type(method->return_type, true);
		if (result.kind == TypeKind::Object || result.kind == TypeKind::Unsupported) {
			return fail(r_expr, "\"" + r_expr.text + "()\" returns " + result.to_string() + ", which is not supported yet.");
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
		// A method that changes a value changes it where it came from, which
		// must be a place that can be written; Array and Dictionary are shared
		// and change in place.
		if (!method->is_const && !is_shared(p_base)) {
			const Expr &base = *r_expr.base;
			const bool place = base.target == Target::Local || base.target == Target::Member || base.target == Target::SelfProperty ||
					base.target == Target::ObjectProperty || base.target == Target::Field;
			if (!place) {
				return fail(r_expr, "\"" + r_expr.text + "()\" changes the " + base_name + " it is called on, which is a temporary value here.");
			}
			if (!check_place(base)) {
				return set(r_expr, Type::of(TypeKind::Error));
			}
		}
		check_arguments(r_expr, params, int(params.size()), false);
		r_expr.target = Target::BuiltinMethod;
		r_expr.owner = base_name;
		r_expr.hash = method->hash;
		r_expr.is_const = method->is_const;
		return set(r_expr, result);
	}

	// `f.bind(a, b)` drops the last two parameters of f, which the engine
	// passes after the caller's arguments (spec §3.11,
	// core/variant/callable_bind.cpp:141-150).
	Type check_bind(Expr &r_expr, const Type &p_function) {
		const size_t param_count = p_function.args.size() - 1;
		const size_t bound = r_expr.args.size();
		if (bound > param_count) {
			for (auto &arg : r_expr.args) {
				check_value(*arg);
			}
			return fail(r_expr, "bind() binds " + std::to_string(bound) + " argument(s), but " + p_function.to_string() + " takes " + std::to_string(param_count) + ".");
		}
		const std::vector<Type> rest(p_function.args.begin() + 1, p_function.args.end() - bound);
		const std::vector<Type> bound_params(p_function.args.end() - bound, p_function.args.end());
		check_arguments(r_expr, bound_params, int(bound), false);
		r_expr.target = Target::BindArguments;
		return set(r_expr, Type::function(p_function.args[0], rest));
	}

	// A function connected to a signal takes what the signal passes (spec §4.7).
	void check_handler(const Expr &p_handler, const Type &p_signal) {
		const Type &handler = p_handler.type;
		if (handler.is_error()) {
			return;
		}
		if (handler.kind != TypeKind::Function) {
			error(p_handler.line, p_handler.column, "A signal is connected to a function value, such as a method name, whose parameters it can check; found " + handler.to_string() + ".");
			return;
		}
		if (handler.args.size() - 1 != p_signal.args.size()) {
			error(p_handler.line, p_handler.column, p_signal.to_string() + " passes " + std::to_string(p_signal.args.size()) + " argument(s), but " + handler.to_string() + " takes " + std::to_string(handler.args.size() - 1) + ".");
			return;
		}
		for (size_t i = 0; i < p_signal.args.size(); i++) {
			if (!assignable(p_signal.args[i], handler.args[i + 1])) {
				error(p_handler.line, p_handler.column, p_signal.to_string() + " passes " + p_signal.args[i].to_string() + " as argument " + std::to_string(i + 1) + ", which " + handler.to_string() + " does not take.");
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
			return set(r_expr, p_base.unwrapped());
		}
		static const std::unordered_set<std::string> later = { "unwrap_or", "unwrap_or_else", "map", "and_then", "or_else", "filter", "ok_or", "ok_or_else", "flatten" };
		for (auto &arg : r_expr.args) {
			check_value(*arg);
		}
		if (later.count(name)) {
			return fail(r_expr, name + "() of " + p_base.to_string() + " is not supported yet.");
		}
		return fail(r_expr, "The value may be None (" + p_base.to_string() + "); check it before calling \"" + name + "()\".");
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
			// Classes of this language extend engine classes only, so far.
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

	Type check_subscript(Expr &r_expr) {
		const Type base = check_value(*r_expr.base);
		const Type index = check_value(*r_expr.right);
		if (base.is_error() || index.is_error()) {
			return set(r_expr, Type::of(TypeKind::Error));
		}
		if (base.is_optional()) {
			return fail(r_expr, "The value may be None (" + base.to_string() + "); check it before indexing it.");
		}
		const std::optional<Type> element = element_of(base);
		if (!element) {
			return fail(r_expr, "Values of type " + base.to_string() + " cannot be indexed" + (base.name == "Dictionary" || base.kind == TypeKind::String ? " yet." : "."));
		}
		if (index.kind != TypeKind::Int) {
			return fail(*r_expr.right, "An index must be int, found " + index.to_string() + ".");
		}
		if (element->kind == TypeKind::Variant) {
			return fail(r_expr, "The elements of an untyped Array are Variant, which is not supported yet; declare it as Array[T].");
		}
		return set(r_expr, *element);
	}

	// `[a, b]` takes its type from where it goes (spec §4.3).
	Type check_array_literal(Expr &r_expr, const Type &p_expected) {
		const Type expected = p_expected.kind == TypeKind::Option ? p_expected.args[0] : p_expected;
		if (expected.kind != TypeKind::Builtin || expected.name != "Array") {
			for (auto &arg : r_expr.args) {
				check_value(*arg);
			}
			if (r_expr.args.empty()) {
				return fail(r_expr, "An empty array needs a declared type, for example var a: Array[int] = [].");
			}
			return fail(r_expr, "An array literal needs a declared type for now, for example var a: Array[int] = [1, 2].");
		}
		const Type element = expected.args.empty() ? Type::of(TypeKind::Variant) : expected.args[0];
		for (auto &arg : r_expr.args) {
			const Type type = check_value(*arg, element);
			r_expr.param_types.push_back(element);
			if (!assignable(type, element)) {
				error(arg->line, arg->column, "Cannot put a value of type " + type.to_string() + " in " + expected.to_string() + ".");
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

	Type check_call(Expr &r_expr) {
		const std::string &name = r_expr.text;

		if (r_expr.base && r_expr.base->kind != ExprKind::Self) {
			Expr &base = *r_expr.base;
			if (names_type(base)) {
				base.target = Target::TypeName;
				if (name == "new" && engine.class_exists(base.text) && !builtin::find_class(base.text)) {
					if (!engine.can_instantiate(base.text)) {
						return fail(r_expr, base.text + " cannot be instantiated.");
					}
					if (!r_expr.args.empty()) {
						return fail(r_expr, "new() of an engine class takes no arguments.");
					}
					r_expr.target = Target::NewObject;
					r_expr.owner = base.text;
					return set(r_expr, Type::object(base.text));
				}
				return fail(r_expr, "Static methods such as " + base.text + "." + name + "() are not supported yet.");
			}
			const Type base_type = check_value(base);
			if (base_type.is_error()) {
				for (auto &arg : r_expr.args) {
					check_value(*arg);
				}
				return set(r_expr, base_type);
			}
			if (base_type.is_optional()) {
				return check_option_method(r_expr, base_type);
			}
			if (base_type.kind == TypeKind::Function && (name == "bind" || name == "call" || name == "callv")) {
				return name == "bind" ? check_bind(r_expr, base_type) : fail(r_expr, name + "() of a function value is not supported yet.");
			}
			if (base_type.kind == TypeKind::Signal && name == "emit") {
				// What the connected functions take (spec §4.7).
				check_arguments(r_expr, base_type.args, int(base_type.args.size()), false);
				r_expr.target = Target::EmitSignal;
				return set(r_expr, Type::of(TypeKind::Void));
			}
			if (base_type.kind == TypeKind::Signal && (name == "connect" || name == "disconnect" || name == "is_connected")) {
				const Type result = check_builtin_method(r_expr, base_type);
				if (!result.is_error() && !r_expr.args.empty()) {
					check_handler(*r_expr.args[0], base_type);
				}
				return result;
			}
			if (!base_type.builtin_name().empty()) {
				return check_builtin_method(r_expr, base_type);
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

		const auto script = function_index.find(name);
		if (script != function_index.end()) {
			const FuncDecl &callee = cls.functions[script->second];
			if (!callee.is_static && refuse_in_static(r_expr, name + "()")) {
				for (auto &arg : r_expr.args) {
					check_value(*arg);
				}
				return set(r_expr, Type::of(TypeKind::Error));
			}
			check_arguments(r_expr, param_types(callee.params), required_params(callee), false);
			r_expr.target = Target::ScriptFunction;
			r_expr.index = script->second;
			return set(r_expr, callee.return_type);
		}

		if (const auto method = engine.find_method(cls.extends, name)) {
			if (method->is_virtual) {
				return fail(r_expr, "\"" + name + "()\" is a virtual engine method that this class does not define.");
			}
			if (refuse_in_static(r_expr, name + "()")) {
				return set(r_expr, Type::of(TypeKind::Error));
			}
			r_expr.base = nullptr; // `self.f()` is `f()`.
			return check_engine_call(r_expr, *method);
		}

		if (!r_expr.base) {
			if (name == "int" || name == "float") {
				const TypeKind result = name == "int" ? TypeKind::Int : TypeKind::Float;
				r_expr.target = name == "int" ? Target::ConvertToInt : Target::ConvertToFloat;
				if (r_expr.args.size() != 1) {
					return fail(r_expr, name + "() takes one argument.");
				}
				const Type arg = check_value(*r_expr.args[0]);
				if (!arg.is_error() && !arg.is_numeric()) {
					return fail(*r_expr.args[0], name + "() of " + arg.to_string() + " is not supported yet.");
				}
				r_expr.param_types.push_back(arg);
				return set(r_expr, Type::of(result));
			}
			if (const builtin::Utility *utility = builtin::find_utility(name)) {
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
				auto loader = std::make_unique<Expr>();
				loader->kind = ExprKind::Name;
				loader->text = "ResourceLoader";
				loader->line = r_expr.line;
				loader->column = r_expr.column;
				r_expr.base = std::move(loader);
				return check_call(r_expr);
			}
		}

		error(r_expr.line, r_expr.column, "Function \"" + name + "()\" not found in the class or its base " + cls.extends + ".");
		for (auto &arg : r_expr.args) {
			check_value(*arg);
		}
		return set(r_expr, Type::of(TypeKind::Error));
	}

	// `p_expected` is the type the value goes to, if known; only array literals
	// take their type from it.
	Type check_expr(Expr &r_expr, const Type &p_expected = Type::of(TypeKind::Error)) {
		switch (r_expr.kind) {
			case ExprKind::IntLiteral:
				if (r_expr.int_overflows) {
					return fail(r_expr, "Integer literal is too large for int.");
				}
				return set(r_expr, Type::of(TypeKind::Int));
			case ExprKind::FloatLiteral:
				return set(r_expr, Type::of(TypeKind::Float));
			case ExprKind::StringLiteral:
				return set(r_expr, Type::of(TypeKind::String));
			case ExprKind::BoolLiteral:
				return set(r_expr, Type::of(TypeKind::Bool));
			case ExprKind::Name:
				return check_name(r_expr);
			case ExprKind::Self:
				return fail(r_expr, "self as a value is not supported yet.");
			case ExprKind::Unary:
				return check_unary(r_expr);
			case ExprKind::Binary:
				return check_binary(r_expr);
			case ExprKind::Call:
				return check_call(r_expr);
			case ExprKind::Member:
				return check_member(r_expr);
			case ExprKind::GetNode:
				return check_get_node(r_expr);
			case ExprKind::Cast:
				return check_cast(r_expr);
			case ExprKind::ArrayLiteral:
				return check_array_literal(r_expr, p_expected);
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
		}
		return set(r_expr, Type::of(TypeKind::Error));
	}

	// Checks an expression whose value is stored, passed or returned.
	Type check_value(Expr &r_expr, const Type &p_expected = Type::of(TypeKind::Error)) {
		const Type type = check_expr(r_expr, p_expected);
		if (type.kind == TypeKind::Void) {
			error(r_expr.line, r_expr.column, "This call returns void and has no value.");
			return Type::of(TypeKind::Error);
		}
		if (type.kind == TypeKind::Unsupported || type.kind == TypeKind::Variant) {
			error(r_expr.line, r_expr.column, "Values of type " + type.to_string() + " are not supported yet.");
			return Type::of(TypeKind::Error);
		}
		return type;
	}

	// Whether the expression is a place that can be written: a local, a
	// member, a property, or a member of a built-in value in such a place,
	// which is then written back (architecture §2.1).
	bool check_place(const Expr &p_target) {
		switch (p_target.target) {
			case Target::Local:
			case Target::Member:
				return true;
			case Target::SelfProperty:
			case Target::ObjectProperty: {
				const std::string &owner = p_target.target == Target::SelfProperty ? cls.extends : p_target.owner;
				const auto property = engine.find_property(owner, p_target.text);
				if (property && !property->writable) {
					error(p_target.line, p_target.column, "The property \"" + p_target.text + "\" is read-only.");
					return false;
				}
				return true;
			}
			case Target::Field:
				if (p_target.base->kind == ExprKind::Name || p_target.base->kind == ExprKind::Member) {
					return check_place(*p_target.base);
				}
				error(p_target.line, p_target.column, "Cannot assign to a member of a temporary value.");
				return false;
			default:
				if (!p_target.type.is_error()) {
					error(p_target.line, p_target.column, "Cannot assign to this expression.");
				}
				return false;
		}
	}

	void expect_assignable(const Expr &p_value, const Type &p_from, const Type &p_to, const char *p_action) {
		if (assignable(p_from, p_to)) {
			return;
		}
		std::string message = std::string("Cannot ") + p_action + " a value of type " + p_from.to_string() + " as " + p_to.to_string() + ".";
		if (p_from.kind == TypeKind::Float && p_to.kind == TypeKind::Int) {
			message += " Convert explicitly with int(x).";
		}
		error(p_value.line, p_value.column, message);
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
		error(r_condition.line, r_condition.column, "A condition must be bool, found " + type.to_string() + "." + hint);
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

	void declare_local(Stmt &r_stmt, const Type &p_type) {
		if (scopes.back().count(r_stmt.name)) {
			error(r_stmt.line, r_stmt.column, "\"" + r_stmt.name + "\" is already declared in this block.");
		}
		r_stmt.local = int(function->locals.size());
		function->locals.push_back({ r_stmt.name, p_type });
		scopes.back()[r_stmt.name] = r_stmt.local;
	}

	static TokenKind compound_operator(TokenKind p_assign) {
		switch (p_assign) {
			case TokenKind::MinusEqual:
				return TokenKind::Minus;
			case TokenKind::StarEqual:
				return TokenKind::Star;
			case TokenKind::SlashEqual:
				return TokenKind::Slash;
			case TokenKind::PercentEqual:
				return TokenKind::Percent;
			default:
				return TokenKind::Plus;
		}
	}

	// `for i in n` and over the elements of an array (spec §5.7).
	void check_for(Stmt &r_stmt) {
		const Type iterated = check_value(*r_stmt.value);
		Type element = Type::of(TypeKind::Error);
		const std::string &name = iterated.name;
		if (iterated.kind == TypeKind::Int) {
			element = iterated;
		} else if (iterated.kind == TypeKind::Float || (iterated.kind == TypeKind::Builtin && (name == "Vector2" || name == "Vector2i" || name == "Vector3" || name == "Vector3i"))) {
			error(r_stmt.value->line, r_stmt.value->column, "Cannot iterate over " + iterated.to_string() + "; iterate over range(...) instead.");
		} else if (const std::optional<Type> elements = element_of(iterated)) {
			if (elements->kind == TypeKind::Variant) {
				error(r_stmt.value->line, r_stmt.value->column, "The elements of an untyped Array are Variant, which is not supported yet; declare it as Array[T].");
			} else {
				element = *elements;
			}
		} else if (!iterated.is_error()) {
			error(r_stmt.value->line, r_stmt.value->column, "Iterating over " + iterated.to_string() + " is not supported yet.");
		}
		r_stmt.element = element;
		Type type = element;
		if (r_stmt.declared_type) {
			const Type declared = resolve(*r_stmt.declared_type, false);
			// The elements' type or a class they extend, without conversions.
			const bool fits = declared == element || (declared.kind == TypeKind::Object && element.kind == TypeKind::Object && assignable(element, declared));
			if (!declared.is_error() && !element.is_error() && !fits) {
				error(r_stmt.declared_type->line, r_stmt.declared_type->column, "The elements are " + element.to_string() + ", which the loop variable of type " + declared.to_string() + " cannot hold.");
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
						error(r_stmt.line, r_stmt.column, "\"" + r_stmt.name + "\" needs a type: None does not say which T? it is (spec §4.3).");
						type = Type::of(TypeKind::Error);
					} else {
						type = value;
					}
				} else if (!r_stmt.declared_type) {
					error(r_stmt.line, r_stmt.column, "\"" + r_stmt.name + "\" needs a type or an initial value.");
				} else if (type.kind == TypeKind::Object && !type.nullable) {
					error(r_stmt.line, r_stmt.column, "\"" + r_stmt.name + "\" of type " + type.to_string() + " needs an initial value.");
				}
				declare_local(r_stmt, type);
				return;
			}
			case StmtKind::Assign: {
				Expr &target = *r_stmt.target;
				Type target_type = target.kind == ExprKind::Name || target.kind == ExprKind::Member ? check_expr(target) : Type::of(TypeKind::Error);
				if (target.kind != ExprKind::Name && target.kind != ExprKind::Member) {
					check_expr(target);
					error(target.line, target.column, "Cannot assign to this expression.");
					check_value(*r_stmt.value);
					return;
				}
				const bool compound = r_stmt.assign_op != TokenKind::Equal;
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
					value = binary_type(compound_operator(r_stmt.assign_op), read_type, value, r_stmt.line, r_stmt.column);
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
				const std::unordered_map<int, Type> then_end = check_block(r_stmt.body, facts.when_true);
				const std::unordered_map<int, Type> else_end = check_block(r_stmt.else_body, facts.when_false);
				// Where one branch always leaves, what holds at the end of the
				// other, which may have assigned what the condition tested.
				if (always_exits(r_stmt.body)) {
					add_facts(else_end);
				}
				if (always_exits(r_stmt.else_body)) {
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
			case StmtKind::Return: {
				const Type expected = function->return_type;
				if (!r_stmt.value) {
					if (expected.kind != TypeKind::Void && !expected.is_error()) {
						error(r_stmt.line, r_stmt.column, "\"" + function->name + "()\" must return a value of type " + expected.to_string() + ".");
					}
					return;
				}
				const Type value = check_value(*r_stmt.value, expected);
				if (expected.kind == TypeKind::Void) {
					error(r_stmt.value->line, r_stmt.value->column, "\"" + function->name + "()\" returns void. Declare its return type, for example -> " + (value.is_error() ? std::string("int") : value.to_string()) + ".");
					return;
				}
				expect_assignable(*r_stmt.value, value, expected, "return");
				return;
			}
			case StmtKind::Pass:
				return;
			case StmtKind::Break:
			case StmtKind::Continue:
				if (loop_depth == 0) {
					error(r_stmt.line, r_stmt.column, std::string(r_stmt.kind == StmtKind::Break ? "break" : "continue") + " is only valid in a loop.");
				}
				return;
			case StmtKind::Expression:
				if (r_stmt.value->kind != ExprKind::Call) {
					check_expr(*r_stmt.value);
					error(r_stmt.value->line, r_stmt.value->column, "This expression has no effect.");
					return;
				}
				check_expr(*r_stmt.value);
				return;
		}
	}

	static bool always_returns(const Block &p_block) {
		if (p_block.empty()) {
			return false;
		}
		const Stmt &last = *p_block.back();
		if (last.kind == StmtKind::Return) {
			return true;
		}
		if (last.kind == StmtKind::If) {
			return always_returns(last.body) && always_returns(last.else_body);
		}
		return false;
	}

	/* Declarations */

	void declare_members() {
		for (size_t i = 0; i < cls.members.size(); i++) {
			VarDecl &member = cls.members[i];
			if (member_index.count(member.name)) {
				error(member.line, member.column, "The member \"" + member.name + "\" is already declared.");
				continue;
			}
			member_index[member.name] = int(i);
		}
	}

	/* Annotations (spec §2.3) */

	// A number as GDScript writes it into a hint string: 1, -5, 0.1, 1.0.
	static std::string number_text(const Expr &p_literal) {
		const bool negative = p_literal.kind == ExprKind::Unary;
		const Expr &value = negative ? *p_literal.left : p_literal;
		if (value.kind == ExprKind::IntLiteral) {
			return (negative ? "-" : "") + std::to_string(value.int_value);
		}
		std::ostringstream stream;
		stream.imbue(std::locale::classic());
		stream << std::setprecision(15) << (negative ? -value.float_value : value.float_value);
		std::string text = stream.str();
		if (text.find_first_of(".einf") == std::string::npos) {
			text += ".0";
		}
		return text;
	}

	// `@export_range(min, max, step, "or_greater", …)`: GDScript's hint string
	// is the arguments joined by commas.
	void check_export_range(VarDecl &r_member, const Annotation &p_annotation) {
		if (!r_member.type.is_numeric()) {
			error(p_annotation.line, p_annotation.column, "@export_range applies to int and float, not " + r_member.type.to_string() + ".");
			return;
		}
		std::string hint;
		size_t numbers = 0;
		bool options = false;
		for (const auto &arg : p_annotation.args) {
			const bool number = is_literal(*arg) && arg->kind != ExprKind::StringLiteral && arg->kind != ExprKind::BoolLiteral;
			if ((number && options) || (!number && arg->kind != ExprKind::StringLiteral)) {
				error(arg->line, arg->column, "@export_range takes numbers, then options such as \"or_greater\".");
				return;
			}
			numbers += number ? 1 : 0;
			options = options || !number;
			hint += (hint.empty() ? "" : ",") + (number ? number_text(*arg) : arg->text);
		}
		if (numbers < 2 || numbers > 3) {
			error(p_annotation.line, p_annotation.column, "@export_range takes a minimum, a maximum and an optional step.");
			return;
		}
		r_member.hint = 1; // PROPERTY_HINT_RANGE (core/object/object.h).
		r_member.hint_string = hint;
	}

	void check_annotations(VarDecl &r_member) {
		for (const Annotation &annotation : r_member.annotations) {
			if (annotation.name == "export_range") {
				check_export_range(r_member, annotation);
			} else if (!annotation.args.empty()) {
				error(annotation.line, annotation.column, "@" + annotation.name + " takes no arguments.");
			}
		}
	}

	// `@export_group(name, prefix)`, `@export_subgroup(name, prefix)`, `@export_category(name)`.
	void check_groups() {
		for (ExportGroupDecl &group : cls.groups) {
			const Annotation &annotation = group.annotation;
			const size_t most = annotation.name == "export_category" ? 1 : 2;
			bool ok = !annotation.args.empty() && annotation.args.size() <= most;
			for (const auto &arg : annotation.args) {
				ok = ok && arg->kind == ExprKind::StringLiteral;
			}
			if (!ok) {
				error(annotation.line, annotation.column, "@" + annotation.name + " takes a name" + (most == 2 ? " and an optional prefix" : "") + ", as strings.");
				continue;
			}
			group.name = annotation.args[0]->text;
			group.prefix = annotation.args.size() > 1 ? annotation.args[1]->text : std::string();
		}
	}

	// After the signatures, since an initializer can call this class's functions.
	// The other members first, then the @onready ones, whose initializers may
	// read any of the others (spec §6.3), so that a member's type is known
	// before an initializer reads it.
	void check_members() {
		cls.is_node = engine.inherits(cls.extends, "Node");
		for (const bool onready : { false, true }) {
			for (size_t i = 0; i < cls.members.size(); i++) {
				if (cls.members[i].is_onready == onready) {
					check_member(i);
				}
			}
		}
	}

	void check_member(size_t p_index) {
		// A name declared twice is reported where it is registered.
		VarDecl &member = cls.members[p_index];
		const auto registered = member_index.find(member.name);
		if (registered == member_index.end() || registered->second != int(p_index)) {
			return;
		}
		Type type = Type::of(TypeKind::Error);
		if (member.declared_type) {
			type = resolve(*member.declared_type, false);
		}
		if (member.is_onready) {
			if (!cls.is_node) {
				error(member.line, member.column, "@onready is for Node scripts; a " + cls.extends + " is never ready (spec §6.3).");
			}
			if (member.is_export) {
				error(member.line, member.column, "@onready and @export do not go together: the node's ready would overwrite what the scene assigned (spec §6.3).");
			}
			if (!member.initializer) {
				error(member.line, member.column, "An @onready member needs an initializer.");
			}
		}
		if (member.initializer) {
			initializing = int(p_index);
			initializing_onready = member.is_onready;
			const Type value = check_value(*member.initializer, type);
			initializing = -1;
			initializing_onready = false;
			if (member.declared_type) {
				expect_assignable(*member.initializer, value, type, "assign");
			} else {
				type = value;
			}
			// An @onready member gets even a literal when the node is ready.
			member.literal_initializer = !member.is_onready && is_literal(*member.initializer) && type.is_primitive();
		} else if (!member.declared_type) {
			error(member.line, member.column, "\"" + member.name + "\" needs a type or an initial value.");
		} else if (type.kind == TypeKind::Object && !type.nullable) {
			// An export that the scene or resource must assign (spec §7.5); a
			// member that the creator or an ancestor must assign (§6.2) is not
			// supported yet.
			member.must_assign = member.is_export;
			if (!member.is_export) {
				error(member.line, member.column, "Members that must be assigned, such as \"" + member.name + "\" of type " + type.to_string() + ", are not supported yet unless exported; give it an initial value.");
			}
		}
		if (member.is_export && type.kind == TypeKind::Object && type.name == "PackedScene" && type.args.empty()) {
			error(member.line, member.column, "An exported scene is written PackedScene[T], with the type of its root (spec §7.4).");
		}
		member.type = type;
		check_annotations(member);
	}

	// Default values of parameters (spec §4.6): the last parameters, and
	// literals so far.
	void check_defaults(FuncDecl &r_function) {
		bool any = false;
		for (ParamDecl &param : r_function.params) {
			if (!param.default_value) {
				if (any) {
					error(param.line, param.column, "Parameter \"" + param.name + "\" follows one with a default value and needs one too.");
				}
				continue;
			}
			any = true;
			if (!is_literal(*param.default_value)) {
				error(param.default_value->line, param.default_value->column, "Default values other than literals are not supported yet.");
				continue;
			}
			const Type value = check_value(*param.default_value);
			expect_assignable(*param.default_value, value, param.type, "use");
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
			return false;
		}
		error(p_expr.line, p_expr.column, "\"" + p_name + "\" belongs to an instance, which the static function \"" + function->name + "()\" does not have.");
		return true;
	}

	void check_signature(FuncDecl &r_function) {
		const auto engine_method = engine.find_method(cls.extends, r_function.name);
		if (engine_method && !engine_method->is_virtual) {
			error(r_function.line, r_function.column, "\"" + r_function.name + "()\" is a method of " + cls.extends + " that cannot be overridden.");
		}
		r_function.is_override = engine_method && engine_method->is_virtual;

		if (r_function.is_override) {
			if (r_function.params.size() != engine_method->params.size()) {
				error(r_function.line, r_function.column, "\"" + r_function.name + "()\" overrides an engine method with " + std::to_string(engine_method->params.size()) + " parameter(s).");
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
					error(param.line, param.column, "Parameters of type " + engine_type.to_string() + " are not supported yet.");
					engine_type = Type::of(TypeKind::Error);
				}
			}
			if (param.declared_type) {
				param.type = resolve(*param.declared_type, false);
				// A parameter may accept more than the overridden one does, such as
				// T? for T (spec §4.6).
				if (r_function.is_override && !engine_type.is_error() && !param.type.is_error() && !assignable(engine_type, param.type)) {
					error(param.declared_type->line, param.declared_type->column, "The overridden method declares this parameter as " + engine_type.to_string() + ".");
				}
			} else if (r_function.is_override) {
				// The type comes from the overridden declaration (spec §4.6).
				param.type = engine_type;
			} else {
				error(param.line, param.column, "Parameter \"" + param.name + "\" needs a type.");
				param.type = Type::of(TypeKind::Error);
			}
		}
		check_defaults(r_function);
		if (r_function.is_static && r_function.is_override) {
			error(r_function.line, r_function.column, "\"" + r_function.name + "()\" overrides an engine method and cannot be static.");
		}

		// The engine constructs Node and Resource scripts with no arguments
		// (spec §6.2); RefCounted constructors with arguments come with new(...).
		if (r_function.name == "_init" && !r_function.params.empty()) {
			const bool engine_made = engine.inherits(cls.extends, "Node") || engine.inherits(cls.extends, "Resource");
			error(r_function.line, r_function.column, engine_made ? "_init() of a " + cls.extends + " script cannot take parameters: the engine constructs it with none. Use members that must be assigned instead (spec §6.2)." : "_init() with parameters is not supported yet.");
		}
		if (r_function.name == "_init" && r_function.return_type_ref && r_function.return_type_ref->name != "void") {
			error(r_function.return_type_ref->line, r_function.return_type_ref->column, "_init() returns nothing.");
		}

		if (r_function.return_type_ref) {
			r_function.return_type = resolve(*r_function.return_type_ref, true);
			if (r_function.is_override && !engine_method->return_type.is_error() && r_function.return_type != engine_method->return_type) {
				error(r_function.return_type_ref->line, r_function.return_type_ref->column, "The overridden method returns " + engine_method->return_type.to_string() + ".");
			}
		} else if (r_function.is_override) {
			r_function.return_type = engine_method->return_type;
			if (r_function.return_type.kind != TypeKind::Void && (!r_function.return_type.is_storable() || r_function.return_type.kind == TypeKind::Variant)) {
				error(r_function.line, r_function.column, "Return values of type " + r_function.return_type.to_string() + " are not supported yet.");
				r_function.return_type = Type::of(TypeKind::Error);
			}
		} else {
			r_function.return_type = Type::of(TypeKind::Void);
		}
	}

	void declare_params(FuncDecl &r_function) {
		for (const ParamDecl &param : r_function.params) {
			if (scopes.back().count(param.name)) {
				error(param.line, param.column, "Parameter \"" + param.name + "\" is declared twice.");
			}
			scopes.back()[param.name] = int(r_function.locals.size());
			r_function.locals.push_back({ param.name, param.type });
		}
	}

	// Connections saved in scenes to methods of this class (spec §7.7): the
	// method must exist and take what the signal passes; the engine finds it
	// by name (§10.5).
	void check_connections() {
		for (const Scene &scene : scenes) {
			for (const SceneConnection &connection : scene.connections) {
				const SceneNode *target = scene.find_node(connection.target);
				if (!target || target->script != options.path) {
					continue;
				}
				const std::string where = scene.path + ": the signal \"" + connection.signal + "\" of " + connection.source + " is connected to " + connection.method + "()";
				const auto found = function_index.find(connection.method);
				if (found == function_index.end()) {
					error(cls.extends_line, cls.extends_column, where + ", which this class does not declare.");
					continue;
				}
				FuncDecl &method = cls.functions[found->second];
				method.is_exposed = true;
				const SceneNode *source = scene.find_node(connection.source);
				bool missing = false;
				const std::optional<std::vector<Type>> arguments = source ? signal_arguments(*source, connection.signal, missing) : std::nullopt;
				if (missing) {
					error(cls.extends_line, cls.extends_column, where + ", but " + connection.source + " has no such signal.");
				}
				if (!arguments) {
					continue;
				}
				const int passed = int(arguments->size()) - connection.unbinds + connection.binds;
				if (int(method.params.size()) != passed) {
					error(method.line, method.column, where + ", which passes " + std::to_string(passed) + " argument(s); " + method.name + "() takes " + std::to_string(method.params.size()) + ".");
					continue;
				}
				for (int i = 0; i < int(arguments->size()) - connection.unbinds; i++) {
					if (!assignable((*arguments)[i], method.params[i].type)) {
						error(method.params[i].line, method.params[i].column, where + ", which passes " + (*arguments)[i].to_string() + " here.");
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
			for (const SignalDecl &signal : decl->signals) {
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
		std::vector<Type> types;
		for (const EngineParam &param : signal->params) {
			Type type = param.type;
			type.nullable = false; // The engine's own signal arguments.
			types.push_back(type);
		}
		return types;
	}

	// The signatures of the interfaces the class declares (spec §3.9).
	void declare_interfaces() {
		std::unordered_set<std::string> names;
		for (InterfaceDecl &interface : cls.interfaces) {
			if (!names.insert(interface.name).second) {
				error(interface.line, interface.column, "The interface \"" + interface.name + "\" is already declared.");
			}
			for (FuncDecl &method : interface.methods) {
				for (ParamDecl &param : method.params) {
					if (!param.declared_type) {
						error(param.line, param.column, "Parameter \"" + param.name + "\" needs a type.");
						param.type = Type::of(TypeKind::Error);
						continue;
					}
					param.type = resolve(*param.declared_type, false);
				}
				method.return_type = method.return_type_ref ? resolve(*method.return_type_ref, true) : Type::of(TypeKind::Void);
			}
		}
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
				error(ref.line, ref.column, type.to_string() + " is not an interface.");
				continue;
			}
			cls.implemented.push_back(type);
			const InterfaceDecl *interface = interface_decl(type);
			if (!interface) {
				error(ref.line, ref.column, type.script + " has errors.");
				continue;
			}
			for (const FuncDecl &method : interface->methods) {
				const auto found = function_index.find(method.name);
				if (found == function_index.end()) {
					error(ref.line, ref.column, "The class does not implement " + method.name + "() of " + type.name + ".");
					continue;
				}
				const FuncDecl &function = cls.functions[found->second];
				if (!function.is_pub) {
					error(function.line, function.column, "\"" + function.name + "()\" implements " + type.name + " and must be pub (spec §3.9).");
				}
				bool same = function.params.size() == method.params.size() && function.return_type == method.return_type;
				for (size_t i = 0; same && i < method.params.size(); i++) {
					same = function.params[i].type == method.params[i].type;
				}
				if (!same) {
					error(function.line, function.column, "\"" + function.name + "()\" does not have the signature that " + type.name + " declares, " + function_type(method).to_string() + ".");
				}
			}
		}
	}

	// The class's signals, whose arguments are typed (spec §4.7).
	void declare_signals() {
		for (size_t i = 0; i < cls.signals.size(); i++) {
			SignalDecl &signal = cls.signals[i];
			if (signal_index.count(signal.name) || member_index.count(signal.name) || function_index.count(signal.name) || engine.find_signal(cls.extends, signal.name)) {
				error(signal.line, signal.column, "\"" + signal.name + "\" is already declared.");
				continue;
			}
			signal_index[signal.name] = int(i);
			for (ParamDecl &param : signal.params) {
				if (!param.declared_type) {
					error(param.line, param.column, "The argument \"" + param.name + "\" of a signal needs a type (spec §4.7).");
					param.type = Type::of(TypeKind::Error);
					continue;
				}
				if (param.default_value) {
					error(param.default_value->line, param.default_value->column, "The arguments of a signal have no default values.");
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
		if (options.declarations_only) {
			function = nullptr;
			scopes.clear();
			return;
		}
		check_block(r_function.body);
		if (r_function.return_type.kind != TypeKind::Void && !r_function.return_type.is_error() && !always_returns(r_function.body)) {
			error(r_function.line, r_function.column, "Not all paths of \"" + r_function.name + "()\" return a value.");
		}
		function = nullptr;
		scopes.clear();
	}

public:
	Checker(ClassDecl &r_class, const EngineApi &p_engine, const CompileOptions &p_options, std::vector<Diagnostic> &r_errors, ScriptClasses &r_classes) :
			cls(r_class), engine(p_engine), options(p_options), errors(r_errors), classes(r_classes) {}

	void run() {
		if (!engine.class_exists(cls.extends)) {
			error(cls.extends_line, cls.extends_column, "Unknown base class \"" + cls.extends + "\".");
			return;
		}
		if (options.project && !options.declarations_only && !options.path.empty()) {
			scenes = options.project->scenes_with_script(options.path);
		}
		declare_interfaces();
		declare_members();
		for (size_t i = 0; i < cls.functions.size(); i++) {
			FuncDecl &function_decl = cls.functions[i];
			if (function_index.count(function_decl.name)) {
				error(function_decl.line, function_decl.column, "The function \"" + function_decl.name + "()\" is already declared.");
				continue;
			}
			if (member_index.count(function_decl.name)) {
				error(function_decl.line, function_decl.column, "\"" + function_decl.name + "\" is already declared as a member.");
				continue;
			}
			function_index[function_decl.name] = int(i);
			check_signature(function_decl);
		}
		check_implements();
		declare_signals();
		check_members();
		check_groups();
		check_connections();
		// In declaration order, so errors come out in a stable order.
		for (size_t i = 0; i < cls.functions.size(); i++) {
			const auto registered = function_index.find(cls.functions[i].name);
			if (registered != function_index.end() && registered->second == int(i)) {
				check_body(cls.functions[i]);
			}
		}
	}
};

} // namespace

void check(ClassDecl &r_class, const EngineApi &p_engine, const CompileOptions &p_options, std::vector<Diagnostic> &r_errors, ScriptClasses &r_classes) {
	Checker(r_class, p_engine, p_options, r_errors, r_classes).run();
}

} // namespace petrichor::compiler
