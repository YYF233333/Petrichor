#include "compiler/checker_internal.h"

namespace petrichor::compiler::checking {

// At the start of the file for what was not written, such as the base
// class of a class without extends.
void Checker::error(const TokenRange &p_range, const std::string &p_message) {
	const TokenRange range = p_range.first < cls.source->tokens.size() ? p_range : TokenRange{ 0, 0 };
	errors.push_back({ cls.source->begin(range), cls.source->end(range), p_message });
}
void Checker::error_in_scene(const TokenRange &p_range, const Scene &p_scene, const std::string &p_node, const std::string &p_message) {
	error(p_range, p_message);
	errors.back().scene = p_scene.path;
	errors.back().node = p_node;
}

/* References (architecture §2.1) */

// A name of this file, unless the compiler made it or a syntax error left
// it empty; the entry, if one is recorded.
Reference *Checker::refer(uint32_t p_token, const DeclId &p_target, std::optional<SymbolKind> p_declares, const std::optional<DeclId> &p_overrides) {
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

Reference *Checker::refer_bytes(uint32_t p_begin, uint32_t p_end, const DeclId &p_target) {
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

void Checker::refer_local(uint32_t p_token, const Type &p_type, bool p_constant) {
	if (Reference *declared = refer(p_token, local_id(p_token), p_constant ? SymbolKind::LocalConstant : SymbolKind::Local)) {
		declared->type = p_type.is_error() ? std::string() : p_type.to_string();
	}
}

// What a string literal holds, when it is written without escapes.
void Checker::refer_string(const Expr &p_literal, const DeclId &p_target) {
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

/* Classes of this language (spec §11.1) */

// The declarations of a class of this language: this class itself, or
// another script's, checked without bodies.
// One of this file, while it is being declared too.
const ClassDecl *Checker::script_decl(const std::string &p_path) const {
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
const ClassDecl *Checker::script_header(const std::string &p_path) const {
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
std::optional<std::string> Checker::broken(const void *p_decl, const std::string &p_what) const {
	const auto found = classes.errors_in(p_decl);
	if (!found) {
		return std::nullopt;
	}
	return p_what + " has errors (" + found->first + ", line " + std::to_string(found->second) + ").";
}

// Why the declaration `p_name` of the class `p_class` cannot be used here:
// not pub where it is not seen (spec §4.2), named as `p_kind` and its
// name, or with errors.
std::optional<std::string> Checker::refused(const void *p_decl, bool p_visible, const std::string &p_kind, const std::string &p_class, const std::string &p_name) const {
	if (!p_visible) {
		return p_kind + "\"" + p_name + "\" of " + p_class + " is not pub.";
	}
	return broken(p_decl, p_class + "." + p_name);
}

// The message for a name that the class at `p_script` lacks, which a
// syntax error there or in a class it extends may have lost.
std::string Checker::missing(std::string p_message, const std::string &p_script) const {
	if (!is_script_path(p_script)) {
		return p_message;
	}
	if (const auto broken = classes.syntax_error(p_script)) {
		p_message.pop_back(); // The full stop.
		p_message += "; " + broken->first + " has syntax errors (line " + std::to_string(broken->second) + ").";
	}
	return p_message;
}

// The class name used in messages and types: class_name, or the file's
// name, followed by the names of the inner classes down to it, as
// Player.Shot.
std::string Checker::display_name(const ClassDecl &p_decl, const std::string &p_path) const {
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

/* Inner classes (spec §4.1) */

// The inner class `p_name` that the class at `p_path` declares or inherits;
// empty when there is none.
std::string Checker::inner_class_path(const std::string &p_path, const std::string &p_name) const {
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
bool Checker::declares(const std::string &p_name) const {
	return declares_in_script(p_name) || engine.find_property(cls.native_base, p_name) || engine.find_method(cls.native_base, p_name) || engine.find_signal(cls.native_base, p_name) ||
			engine.find_constant(cls.native_base, p_name) || engine.find_enum(cls.native_base, p_name);
}

// Whether this class, a base of this language or a local has the name.
bool Checker::declares_in_script(const std::string &p_name) const {
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

// A name of the engine class this one extends, used bare in an inner class
// whose classes around it declare it too: GDScript would take theirs, this
// language the inherited one, so it is an error (spec §4.1).
bool Checker::refuse_ambiguous_engine_name(const std::string &p_name, const TokenRange &p_at) {
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
Checker *Checker::class_around(const std::string &p_name) const {
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
std::string Checker::scope_class(const std::string &p_name) const {
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
std::string Checker::class_path_of(const Expr &p_expr, bool p_object) {
	if (p_expr.kind == ExprKind::Member && p_expr.base) {
		const std::string owner = class_path_of(*p_expr.base, true);
		return owner.empty() ? owner : inner_class_path(owner, p_expr.text);
	}
	if (p_expr.kind == ExprKind::Call && !p_expr.base && p_expr.text == "preload") {
		const std::string file = preload_file(preload_path(p_expr));
		return is_script_path(file) ? file : std::string();
	}
	const std::string path = p_expr.kind == ExprKind::Name ? class_named(p_expr.text, p_object) : std::string();
	return is_script_path(path) ? path : std::string();
}

// The class, of this language or another, that a bare name names when
// nothing closer has the name (spec §2.1, §4.1): through a constant that
// preloads its script, as an inner class of this class or of one around
// it, or by its class_name; with `p_autoload`, the class of an autoload,
// an object it is reached through. Empty for any other name.
std::string Checker::class_named(const std::string &p_name, bool p_autoload) {
	if (const ConstPtr alias = class_alias(p_name)) {
		return alias->script;
	}
	if (names_variable(p_name) || constant_index.count(p_name)) {
		return std::string();
	}
	const std::string inner = scope_class(p_name);
	if (!inner.empty() || !is_global(p_name)) {
		return inner;
	}
	const std::string path = class_name_path(p_name);
	return path.empty() && p_autoload ? autoload_script(p_name) : path;
}

// A class of this language as a value: its script, which for an inner
// class has no path of its own to preload (spec §2.1, §4.1).
Type Checker::class_value(Expr &r_expr, const std::string &p_path) {
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
Type Checker::value_around(Expr &r_expr, Checker &r_around) {
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

std::string Checker::file_name(const std::string &p_path) {
	const size_t slash = p_path.find_last_of('/');
	return p_path.substr(slash == std::string::npos ? 0 : slash + 1);
}

/* Classes of other languages (spec §11.1) */

// Without its members while only the declarations are checked, when the
// script being loaded may load no other.
const ForeignClass *Checker::foreign_class(const std::string &p_path) const {
	auto found = foreign_classes.find(p_path);
	if (found == foreign_classes.end()) {
		found = foreign_classes.emplace(p_path, options.project ? options.project->foreign_class(p_path, !options.declarations_only) : std::nullopt).first;
	}
	return found->second ? &*found->second : nullptr;
}

// The class an autoload's name stands for, as a value and in a type (spec
// §11.1): its script's, or for a scene its root's; nullopt for a name that
// is no autoload, Error after reporting one that cannot be read.
std::optional<Type> Checker::autoload_class(const std::string &p_name, const TokenRange &p_at) {
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
std::string Checker::autoload_script(const std::string &p_name) {
	const size_t reported = errors.size();
	const std::optional<Type> found = autoload_class(p_name, TokenRange{ 0, 0 });
	errors.resize(reported);
	return found && is_script_path(found->script) ? found->script : std::string();
}

// A class reflection names: an engine class, or a script's class by its
// global name, as GDScript names it (modules/gdscript/gdscript_parser.cpp:5457-5465).
Type Checker::localized(Type p_type) {
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
Type Checker::unknown_foreign(Expr &r_expr, const Type &p_class) {
	if (options.declarations_only) {
		return set(r_expr, Type::of(TypeKind::Error));
	}
	return fail(r_expr, p_class.name + " (" + p_class.script + ") cannot be read.");
}

// GDScript has no pub: its members are all reached by name.
std::optional<Type> Checker::foreign_member(Expr &r_expr, const Type &p_class) {
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

std::optional<Type> Checker::foreign_method(Expr &r_expr, const Type &p_class) {
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
std::optional<EngineMethod> Checker::foreign_method_named(Expr &r_expr, const Type &p_class, const ForeignClass &p_foreign) {
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

/* How classes relate */

// The script itself, or one it extends.
bool Checker::extends_script(const std::string &p_script, const std::string &p_base) const {
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

bool Checker::implements(const Type &p_class, const Type &p_interface) const {
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

bool Checker::is_a(const Type &p_from, const Type &p_to) const {
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

std::vector<Type> Checker::fields(const Type &p_struct) const {
	std::vector<Type> types;
	if (const StructDecl *decl = struct_decl(p_struct)) {
		for (const VarDecl &field : decl->fields) {
			types.push_back(field.type);
		}
	}
	return types;
}

/* Type resolution */

// A class of this language named by its class_name, or an interface or a
// struct in one, such as Combat.Damageable; nullopt if the name is not one.
std::optional<Type> Checker::script_type(const TypeRef &p_ref) {
	std::vector<std::string> names;
	for (size_t at = 0; at <= p_ref.name.size();) {
		const size_t dot = p_ref.name.find('.', at);
		names.push_back(p_ref.name.substr(at, dot == std::string::npos ? dot : dot - at));
		at = dot == std::string::npos ? p_ref.name.size() + 1 : dot + 1;
	}
	const std::string &head = names[0];
	// The token of each name of `Outer.Inner.Hit`.
	const auto token = [&](size_t p_index) { return p_ref.name_token == NO_TOKEN ? NO_TOKEN : p_ref.name_token + uint32_t(2 * p_index); };
	const ConstPtr alias = class_alias(head);
	std::string path = alias ? alias->script : scope_class(head);
	if (alias) {
		// GDScript takes the autoload in a type (spec §11.1).
		if (options.project && !options.project->autoload_path(head).empty()) {
			error(p_ref.range, "The constant " + head + " has the name of an autoload, which GDScript takes in a type. Rename the constant.");
			return Type::of(TypeKind::Error);
		}
		refer(p_ref.name_token, alias_id(head));
		if (alias->script_class.is_error()) {
			return alias->script_class; // Not known while only the declarations are checked.
		}
	} else if (path.empty() && !cls.class_name.empty() && head == cls.class_name) {
		path = options.path;
	} else if (path.empty() && options.project) {
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
std::optional<Type> Checker::own_type(const std::string &p_name, uint32_t p_token) {
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

// The declaration of a struct, with its fields checked; null when it or its
// script has errors, which its type's name reports.
const StructDecl *Checker::struct_decl(const Type &p_struct) const {
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
bool Checker::has_default(const Type &p_type) const {
	if (p_type.kind == TypeKind::PayloadEnum) {
		return false;
	}
	const StructDecl *decl = p_type.kind == TypeKind::Struct ? struct_decl(p_type) : nullptr;
	return !decl || decl->has_default();
}

// Whether a variable of the type starts at a value without an initializer
// (spec §4.3): not a non-null object, a function, an enum without 0 or
// with payloads, nor a struct with a field that has no default.
bool Checker::has_value_without_initializer(const Type &p_type) const {
	const bool reference = p_type.kind == TypeKind::Object || p_type.kind == TypeKind::Task;
	return !(reference && !p_type.nullable) && p_type.kind != TypeKind::Function && has_default(p_type) && enum_has_default(p_type);
}

Type Checker::resolve(const TypeRef &p_ref, bool p_allow_void) {
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

Type Checker::resolve_name(const TypeRef &p_ref, bool p_allow_void) {
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
	// A constant that preloads a script names its class before the
	// engine's names do (spec §2.1, §4.1).
	if (class_alias(p_ref.name.substr(0, p_ref.name.find('.')))) {
		return *script_type(p_ref);
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
Type Checker::resolve_generic(const TypeRef &p_ref) {
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
Type Checker::container_element(const TypeRef &p_ref, const std::string &p_what) {
	const Type element = resolve(p_ref, false);
	if (!element.is_error() && !element.args.empty() && !is_boxed(element) && !is_typed_container(element)) {
		error(p_ref.range, p_what + element.to_string() + " are not supported yet.");
		return Type::of(TypeKind::Error);
	}
	return element;
}

// Of a function with a rest parameter, a Callable of unknown signature
// (spec §4.6).
Type Checker::function_type(const FuncDecl &p_function) {
	if (p_function.has_rest()) {
		return Type::builtin("Callable");
	}
	Type type = Type::function(p_function.return_type, param_types(p_function.params));
	type.is_async = p_function.is_async;
	return type;
}

// The signature as a function type writes it, a rest parameter as
// `...Array[T]`.
std::string Checker::signature_text(const FuncDecl &p_function) {
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
Type Checker::signal_type(const EngineMethod &p_signal) {
	std::vector<Type> params;
	for (const EngineParam &param : p_signal.params) {
		Type type = param.type;
		type.nullable = type.kind == TypeKind::Object && builtin::signal_param_nullable(p_signal.owner, p_signal.name, uint32_t(params.size()));
		params.push_back(type);
	}
	return Type::signal(params);
}

/* Results of expressions */

Type Checker::set(Expr &r_expr, const Type &p_type) {
	r_expr.type = p_type;
	return p_type;
}

Type Checker::fail(Expr &r_expr, const std::string &p_message) {
	error(r_expr.range, p_message);
	return set(r_expr, Type::of(TypeKind::Error));
}

// A call that fails still has its arguments checked, so that their own
// errors are reported too.
Type Checker::fail_call(Expr &r_call, const std::string &p_message) {
	fail_call(r_call);
	return fail(r_call, p_message);
}

// The same, for a call whose error is reported already.
Type Checker::fail_call(Expr &r_call) {
	for (auto &arg : r_call.args) {
		check_value(*arg);
	}
	return set(r_call, Type::of(TypeKind::Error));
}

/* Locals and captures (spec §3.11) */

int Checker::find_in(const std::vector<std::unordered_map<std::string, int>> &p_scopes, const std::string &p_name) {
	for (auto scope = p_scopes.rbegin(); scope != p_scopes.rend(); ++scope) {
		const auto found = scope->find(p_name);
		if (found != scope->end()) {
			return found->second;
		}
	}
	return -1;
}

// A local of the function being checked, or of one around the lambda
// being checked, which a use would capture.
const LocalInfo *Checker::visible_local(const std::string &p_name) const {
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
int Checker::capture(const std::string &p_name) {
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
			auto read = synthetic(ExprKind::Name, p_name, lambda.range);
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

/* _init and members that must be assigned (spec §6.2) */

bool Checker::init_assigns(const ClassDecl &p_class, const std::string &p_member) {
	const FuncDecl *init = init_of(p_class);
	return init && std::find(init->assigned.begin(), init->assigned.end(), p_member) != init->assigned.end();
}

Checker::Assigned Checker::joined(const Assigned &p_a, const Assigned &p_b) {
	if (!p_a || !p_b) {
		return p_a ? p_a : p_b;
	}
	std::set<int> both;
	std::set_intersection(p_a->begin(), p_a->end(), p_b->begin(), p_b->end(), std::inserter(both, both.begin()));
	return both;
}

Checker::Assigned Checker::init_block(const Block &p_block, Assigned p_assigned, std::set<int> &r_reported) {
	for (const auto &stmt : p_block) {
		p_assigned = init_stmt(*stmt, std::move(p_assigned), r_reported);
	}
	return p_assigned;
}

// The scene and the creator assign only after _init, so _init reading one
// it has not assigned itself is an error. A read through a call is left to
// the program error: methods may be overridden in other files.
Checker::Assigned Checker::init_stmt(const Stmt &p_stmt, Assigned p_assigned, std::set<int> &r_reported) {
	if (!p_assigned) {
		return std::nullopt;
	}
	const auto reads = [&](const std::unique_ptr<Expr> &p_expr) {
		if (p_expr) {
			init_reads(*p_expr, *p_assigned, r_reported);
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
			return joined(init_block(p_stmt.body, p_assigned, r_reported), init_block(p_stmt.else_body, p_assigned, r_reported));
		case StmtKind::While:
		case StmtKind::For:
			reads(p_stmt.condition);
			reads(p_stmt.value);
			init_block(p_stmt.body, p_assigned, r_reported);
			return p_assigned;
		case StmtKind::Match: {
			reads(p_stmt.value);
			Assigned after = p_stmt.exhaustive ? Assigned() : p_assigned;
			for (const MatchBranch &branch : p_stmt.branches) {
				reads(branch.guard);
				after = joined(after, init_block(branch.body, p_assigned, r_reported));
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
			// super() runs the base's _init, which assigns what it does.
			if (const FuncDecl *base = p_stmt.value && p_stmt.value->target == Target::SuperMethod && p_stmt.value->text == "_init" ? base_function("_init") : nullptr) {
				for (const std::string &name : base->assigned) {
					p_assigned->insert(member_index.at(name));
				}
			}
			return p_assigned;
	}
}

void Checker::init_reads(const Expr &p_expr, const std::set<int> &p_assigned, std::set<int> &r_reported) {
	if (p_expr.kind == ExprKind::Lambda) {
		return; // It runs later.
	}
	if (p_expr.target == Target::Member && p_expr.index >= 0 && cls.all_members[p_expr.index]->must_assign && !p_assigned.count(p_expr.index) && r_reported.insert(p_expr.index).second) {
		error(p_expr.range, "_init reads \"" + p_expr.text + "\" before assigning it: a member that must be assigned gets its value from the scene or the creator only after _init.");
	}
	for_each_child(p_expr, [&](const auto &p_child) {
		if constexpr (std::is_same_v<std::decay_t<decltype(p_child)>, Expr>) {
			init_reads(p_child, p_assigned, r_reported);
		}
	});
}

/* Checking the class */

// The class's declarations, once; when another class of this file needs
// them first, then.
void Checker::declare() {
	if (state != State::Unchecked) {
		return;
	}
	state = State::Declaring;
	declared = declare_class();
	cls.declared = declared;
	state = State::Declared;
}

// _init's body, once, after those of the classes of this file it extends:
// the other bodies use what it assigns, and super() what theirs do.
void Checker::check_init() {
	if (init_checked || !declared) {
		return;
	}
	init_checked = true;
	if (Checker *base = file->find(cls.base_script)) {
		base->check_init();
	}
	for (FuncDecl &fn : cls.functions) {
		if (is_declared(fn) && fn.name == "_init") {
			check_body(fn);
		}
	}
}

void Checker::check_function_bodies() {
	if (!declared) {
		return;
	}
	check_init();
	for (FuncDecl &fn : cls.functions) {
		if (is_declared(fn) && fn.name != "_init") {
			check_body(fn);
		}
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
void Checker::check_scene() {
	const std::optional<Scene> scene = declared ? options.project->scene(options.scene) : std::nullopt;
	if (!scene) {
		return;
	}
	scenes = { *scene };
	check_attached();
	check_node_paths();
	check_connections();
	check_named_members();
	check_owner_scenes();
	check_parent_scenes();
	check_stored_exports();
	check_scene_exports();
	check_resource_values();
}
// Whether a class of `p_file` extends one of this file, directly or
// through the classes of the other files `r_files` lists after it. Each
// file loads its classes together, so neither file could load first.
bool Checker::extends_back(const std::string &p_file, std::vector<std::string> &r_files, std::unordered_set<std::string> &r_seen) {
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

bool Checker::declare_class() {
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

} // namespace petrichor::compiler::checking

namespace petrichor::compiler {

using namespace checking;

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
	for (const std::unique_ptr<Checker> &checker : file.checkers) {
		checker->check_function_bodies();
	}
	if (!p_options.scene.empty()) {
		r_errors.clear();
		if (r_references) {
			r_references->clear();
		}
		// An inner class too, which a node's script may extend.
		for (const std::unique_ptr<Checker> &checker : file.checkers) {
			checker->check_scene();
		}
	}
	r_classes.leave(p_options.path);
}

void for_each_node_path(const ClassDecl &p_class, const std::function<void(const Expr &)> &p_fn) {
	const auto visit = [&](const auto &p_self, const auto &p_node) -> void {
		using Node = std::decay_t<decltype(p_node)>;
		if constexpr (std::is_same_v<Node, ClassDecl>) {
			if (&p_node != &p_class) {
				return;
			}
		} else if constexpr (std::is_same_v<Node, FuncDecl>) {
			if (p_node.is_static) {
				return;
			}
		} else if constexpr (std::is_same_v<Node, Expr>) {
			const bool literal = p_node.kind == ExprKind::Call && (p_node.text == "get_node" || p_node.text == "get_node_or_null") && (!p_node.base || p_node.base->kind == ExprKind::Self) && p_node.args.size() == 1 && p_node.args[0]->kind == ExprKind::StringLiteral && p_node.arg_names.empty();
			if (p_node.kind == ExprKind::GetNode || literal) {
				p_fn(p_node);
			}
		}
		for_each_child(p_node, [&](const auto &p_child) { p_self(p_self, p_child); });
	};
	visit(visit, p_class);
}

std::vector<std::pair<std::string, bool>> export_annotations() {
	std::vector<std::pair<std::string, bool>> names = { { "export_custom", true } };
	for (const HintAnnotation &annotation : HINT_ANNOTATIONS) {
		names.emplace_back(annotation.name, annotation.most > 0);
	}
	return names;
}

} // namespace petrichor::compiler
