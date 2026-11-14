#include "compiler/checker_internal.h"

namespace petrichor::compiler::checking {

/* Constants (spec §4.5) */

// The class's own constants, after those it inherits.
void Checker::declare_constants() {
	for (ConstDecl &constant : cls.constants) {
		if (name_taken(constant.name)) {
			error(constant.name_token, "\"" + constant.name + "\" is already declared.");
			continue;
		}
		constant_index[constant.name] = int(cls.all_constants.size());
		cls.all_constants.push_back(&constant);
		refer(constant.name_token, script_id(options.path, constant.name), SymbolKind::Constant);
	}
}

// The value of an expression whose type is unknown while loading because
// of the preloads in it: a preload, or the literal of an untyped container
// of such values and of known ones.
ConstPtr Checker::loading_value(const Expr &p_expr) {
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
ConstPtr Checker::constant_value(Expr &r_initializer, const std::optional<TypeRef> &p_declared, Type &r_type) {
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
void Checker::check_constant(ConstDecl &r_decl) {
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
Type Checker::use_constant(Expr &r_expr, const ConstDecl &p_decl, const std::string &p_path) {
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
const EnumDecl *Checker::initializer_payload_enum(Expr &r_initializer) {
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

// A constant this class declares or inherits, checked; null if none.
const ConstDecl *Checker::own_constant(const std::string &p_name) {
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
void Checker::declare_enums() {
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

// What a type of this name would hide, as the end of a message; empty if
// nothing (spec §4.1). The file's class_name is in the engine's global class
// table, which lists one script for each name, as GDScript reads it
// (modules/gdscript/gdscript_analyzer.cpp:396-406; ed1daf0). A type the class
// declares may take the name of a standard type or of an autoload, being the
// nearer name (spec §11.1).
std::string Checker::taken_type_name(const std::string &p_name, bool p_class_name) const {
	if (p_name == "int" || p_name == "float" || p_name == "bool" || builtin::find_class(p_name)) {
		return "is the name of a built-in type.";
	}
	if (engine.class_exists(p_name)) {
		return "is the name of an engine class.";
	}
	if (p_class_name && (p_name == "Result" || p_name == "Task" || p_name == "Settled" || p_name == "ConversionError")) {
		return "is the name of a standard type.";
	}
	const std::string path = options.project ? options.project->global_class_path(p_name) : std::string();
	if (!path.empty() && !(p_class_name && path == options.path)) {
		return "is already the class_name of " + path + ".";
	}
	if (p_class_name && options.project && !options.project->autoload_path(p_name).empty()) {
		return "is the name of an autoload.";
	}
	return std::string();
}

// The names of the types the class declares and of its unnamed enums'
// values (spec §4.1), after everything else is declared: each once among
// the class's declarations and those it inherits.
void Checker::check_type_names() {
	if (!cls.class_name.empty()) {
		const std::string taken = taken_type_name(cls.class_name, true);
		if (!taken.empty()) {
			error(cls.class_name_token, "\"" + cls.class_name + "\" " + taken);
		}
	}
	std::unordered_set<std::string> names;
	const auto declare = [&](const std::string &p_name, uint32_t p_at, bool p_type) {
		const std::string taken = p_type ? taken_type_name(p_name, false) : std::string();
		if (!taken.empty()) {
			error(p_at, "\"" + p_name + "\" " + taken);
		} else if (!names.insert(p_name).second || name_taken(p_name)) {
			error(p_at, "\"" + p_name + "\" is already declared.");
		}
	};
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
void Checker::check_enum(EnumDecl &r_decl) {
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
void Checker::check_variant_fields(EnumDecl &r_decl, EnumValueDecl &r_value) {
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
const EnumDecl *Checker::own_enum(const std::string &p_name) {
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
std::optional<std::pair<const EnumDecl *, int>> Checker::bare_enum_value(const std::string &p_name) {
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
Type Checker::use_enum_value(Expr &r_expr, const EnumDecl &p_decl, int p_index) {
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
Type Checker::use_enum_dictionary(Expr &r_expr, const EnumDecl &p_decl, bool p_refer) {
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
Type Checker::use_engine_enum_dictionary(Expr &r_expr, const Type &p_enum) {
	r_expr.target = Target::Constant;
	r_expr.constant = enum_dictionary(*p_enum.values, p_enum);
	return set(r_expr, r_expr.constant->type);
}

// The enum that the base of `State.IDLE` or, with `p_call`, the callee of
// `State(x)` names: by its bare name, when no local or member has it, one
// of this class or of one around it, or one a constant names; a pub one of
// another class, as in `Player.State`; null for any other name. A bare
// callee that is a function's name calls the function.
const EnumDecl *Checker::named_enum(Expr &r_name, bool p_call) {
	if (!may_name(r_name, p_call)) {
		return nullptr;
	}
	const std::string &name = r_name.text;
	if (!r_name.base) {
		if (visible_local(name) || member_index.count(name) || (p_call && function_index.count(name))) {
			return nullptr;
		}
		const bool aliased = constant_index.count(name);
		const EnumDecl *decl = aliased ? aliased_enum(name) : scope_enum(name);
		if (decl) {
			refer(r_name.name_token, aliased ? alias_id(name) : enum_id(*decl));
		}
		return decl;
	}
	Expr &base = *r_name.base;
	if (!names_enum_of_class(base, name)) {
		return nullptr;
	}
	std::optional<Type> named = named_class(base);
	if (!named && base.kind == ExprKind::Name) {
		named = check_value(base); // An autoload, which leads to its class (spec §11.1).
	}
	if (!named || named->is_error() || !is_script_path(named->script)) {
		return nullptr;
	}
	const ClassDecl *decl = named->script == options.path ? &cls : script_decl(named->script);
	const EnumDecl *found = decl ? decl->find_enum(name) : nullptr;
	if (!found) {
		return nullptr;
	}
	Checker *local = file->find(named->script);
	// Checked when first used, also while that class is declared.
	if (const EnumDecl *checked = local ? local->own_enum(name) : nullptr) {
		found = checked;
	}
	if (!found->is_pub && !sees(found->script)) {
		error(r_name.range, "The enum " + named->name + "." + found->name + " is not pub.");
	} else if (const auto message = local ? std::nullopt : broken(found, named->name + "." + found->name)) {
		error(r_name.range, *message);
	}
	refer(r_name.name_token, script_id(found->script, found->name));
	return found;
}

// The enum that a constant of this class names, being initialized with
// the enum's name, as in `const STATE = Inspector.STATE` (spec §3.7).
const EnumDecl *Checker::aliased_enum(const std::string &p_name) {
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
std::optional<Type> Checker::aliased_enum_type(const std::string &p_name) {
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
// `EASE.EASE_IN` or, with `p_call`, the callee of `Tween.EaseType(x)`
// names (spec §3.7): by the engine's name, unless a closer one hides it,
// by a constant that names it, or one of the engine class this one
// extends, such as ProcessMode (§4.1). The class may be a singleton's, as
// Input of `Input.MouseMode`.
std::optional<Type> Checker::named_engine_enum(const Expr &p_name, bool p_call) {
	if (!may_name(p_name, p_call)) {
		return std::nullopt;
	}
	const std::string &name = p_name.text;
	if (p_name.base) {
		const std::string dotted = p_name.base->text + "." + name;
		const std::optional<Type> found = names_type(*p_name.base, true) ? named_engine_enum(dotted) : std::nullopt;
		if (found) {
			refer_engine_enum(dotted, p_name.base->name_token, p_name.name_token);
		}
		return found;
	}
	if (visible_local(name) || member_index.count(name) || engine.find_property(cls.native_base, name)) {
		return std::nullopt;
	}
	if (constant_index.count(name)) {
		const std::optional<Type> aliased = aliased_enum_type(name);
		if (!aliased || !aliased->values->is_engine) {
			return std::nullopt;
		}
		refer(p_name.name_token, alias_id(name));
		return aliased;
	}
	if (declared_in_scope(name)) {
		return std::nullopt;
	}
	if (const std::optional<Type> found = named_engine_enum(name)) {
		refer_engine_enum(name, p_name.name_token, p_name.name_token);
		return found;
	}
	const std::optional<Type> found = engine.find_enum(cls.native_base, name);
	if (found) {
		refer(p_name.name_token, engine_id(found->name.substr(0, found->name.find('.')), name));
	}
	return found;
}

// A value of the engine's enum by its name, as `Tween.EaseType.EASE_IN`.
Type Checker::check_engine_enum_value(Expr &r_expr, const Type &p_enum) {
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
bool Checker::names_enum_of_class(const Expr &p_class, const std::string &p_name) {
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
const EnumDecl *Checker::scope_enum(const std::string &p_name) {
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
const EnumDecl &Checker::cancel_reason_decl() {
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
DeclId Checker::enum_id(const EnumDecl &p_decl, const std::string &p_value) {
	if (&p_decl == &cancel_reason_decl()) {
		return language_id(p_decl.name, p_value);
	}
	return script_id(p_decl.script, p_value.empty() ? p_decl.name : enum_value_id(p_decl, p_value));
}

// An enum of the engine by the name code writes: a global one, such as
// MouseButton or Variant.Type, one of a built-in type, such as
// Vector3.Axis, or one of a class, such as Node.ProcessMode (spec §3.7).
std::optional<Type> Checker::named_engine_enum(const std::string &p_name) const {
	const size_t dot = p_name.find('.');
	if (dot != std::string::npos && p_name.find('.', dot + 1) == std::string::npos && engine.class_exists(p_name.substr(0, dot))) {
		return engine.find_enum(p_name.substr(0, dot), p_name.substr(dot + 1));
	}
	return builtin_enum(p_name);
}

// MouseButton, or Node and ProcessMode of Node.ProcessMode, each by its token.
void Checker::refer_engine_enum(const std::string &p_name, uint32_t p_first, uint32_t p_last) {
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
Type Checker::check_enum_conversion(Expr &r_expr, const Type &p_enum) {
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
Type Checker::check_enum_value(Expr &r_expr, const EnumDecl &p_decl) {
	const int index = p_decl.find(r_expr.text);
	if (index < 0) {
		return fail(r_expr, missing(p_decl.name + " has no value \"" + r_expr.text + "\".", p_decl.script));
	}
	return use_enum_value(r_expr, p_decl, index);
}

/* Named classes, names and members */

// A constant initialized with a preload of a script, which names its class
// where its bare name is used (spec §2.1): a local constant or a constant
// of this class; null for any other name.
ConstPtr Checker::class_alias(const std::string &p_name) {
	if (const LocalInfo *local = visible_local(p_name)) {
		return local->constant && !local->constant->script.empty() ? local->constant : nullptr;
	}
	const ConstDecl *decl = alias_decl(p_name).first;
	return decl && decl->value && !decl->value->script.empty() ? decl->value : nullptr;
}

// A constant of this class or of one around it (spec §4.1), and the class
// it is reached in.
std::pair<const ConstDecl *, const ClassDecl *> Checker::alias_decl(const std::string &p_name) {
	if (const ConstDecl *decl = own_constant(p_name)) {
		return { decl, &cls };
	}
	Checker *around = constant_index.count(p_name) ? nullptr : class_around(p_name);
	const ConstDecl *decl = around ? around->own_constant(p_name) : nullptr;
	return { decl, decl ? &around->cls : nullptr };
}

// The constant that the name of a class alias refers to.
DeclId Checker::alias_id(const std::string &p_name) {
	if (const LocalInfo *local = visible_local(p_name)) {
		return local_id(local->token);
	}
	const auto [decl, owner] = alias_decl(p_name);
	return script_id(declaring(*owner, *decl, &ClassDecl::constants).path, decl->name);
}

// The file whose class_name the name is: this one, or another of the
// project; empty if none.
std::string Checker::class_name_path(const std::string &p_name) const {
	if (!cls.class_name.empty() && p_name == cls.class_name) {
		return options.path;
	}
	return options.project ? options.project->global_class_path(p_name) : std::string();
}

// The class that the base of `Base.x` or `Base.f()` names, of this
// language or another (spec §2.1, §11.1): the preload of its script,
// `Outer.Inner`, or a name as class_named() finds it; nullopt for any
// other base, and Error after reporting.
std::optional<Type> Checker::named_class(Expr &r_base) {
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
	const std::string path = class_named(r_base.text);
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
std::optional<Type> Checker::class_constant(Expr &r_expr, const std::string &p_path, const std::string &p_class) {
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
	if (const ConstDecl *constant = find_named(decl->all_constants, r_expr.text)) {
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
Type Checker::use_static(Expr &r_expr, const VarDecl &p_variable, const ClassDecl &p_owner) {
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
std::optional<Type> Checker::class_static(Expr &r_expr, const std::string &p_path, const std::string &p_class) {
	const ClassDecl *decl = script_decl(p_path);
	if (!decl) {
		return fail(r_expr, p_class + " has errors.");
	}
	if (const VarDecl *variable = find_named(decl->all_static_vars, r_expr.text)) {
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
std::optional<Type> Checker::check_self_name(Expr &r_expr) {
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
	if ((engine.find_constant(cls.native_base, r_expr.text) || engine.find_enum(cls.native_base, r_expr.text)) && refuse_ambiguous_engine_name(r_expr.text, r_expr.range)) {
		return set(r_expr, Type::of(TypeKind::Error));
	}
	return engine_class_value(r_expr, cls.native_base);
}

// A name of the engine class `p_class` that needs no instance (spec §3.7,
// §4.1): a constant, such as NOTIFICATION_PREDELETE, or an enum's name, such
// as ProcessMode, as a value.
std::optional<Type> Checker::engine_class_value(Expr &r_expr, const std::string &p_class) {
	if (engine.find_constant(p_class, r_expr.text)) {
		return check_engine_constant(r_expr, p_class);
	}
	const std::optional<Type> found = engine.find_enum(p_class, r_expr.text);
	if (!found) {
		return std::nullopt;
	}
	refer(r_expr.name_token, engine_id(found->name.substr(0, found->name.find('.')), r_expr.text));
	return use_engine_enum_dictionary(r_expr, *found);
}

Type Checker::check_self(Expr &r_expr) {
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

// A field or a method of the struct whose method is being checked, by its
// bare name (spec §3.6): a field of self.
std::optional<Type> Checker::struct_name(Expr &r_expr) {
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
Type Checker::struct_method_value(Expr &r_expr, const Type &p_struct, bool p_on_value) {
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

Type Checker::check_name(Expr &r_expr) {
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
std::optional<Type> Checker::script_member(Expr &r_expr, const Type &p_class) {
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
	if (const VarDecl *entry = find_named(decl->all_members, r_expr.text)) {
		const VarDecl &member = *entry;
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
	if (const SignalDecl *entry = find_named(decl->all_signals, r_expr.text)) {
		const SignalDecl &signal = *entry;
		// The engine knows every signal a class declares.
		if (const auto message = refused(&signal, signal.is_pub || sees(declaring(*decl, signal, &ClassDecl::signals).path), "The signal ", p_class.name, signal.name)) {
			return fail(r_expr, *message);
		}
		r_expr.target = Target::ObjectSignal;
		r_expr.signal = &signal;
		refer(r_expr.name_token, script_id(declaring(*decl, signal, &ClassDecl::signals).path, signal.name));
		return Type::signal(param_types(signal.params));
	}
	if (const FuncDecl *entry = find_named(decl->all_functions, r_expr.text)) {
		const FuncDecl &function = *entry;
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
Type Checker::class_member_value(Expr &r_expr, const std::string &p_path, const std::string &p_class) {
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
	if (const FuncDecl *entry = find_named(decl->all_functions, r_expr.text)) {
		const FuncDecl &function = *entry;
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
Type Checker::new_value(Expr &r_expr, const std::string &p_path) {
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
Type Checker::static_value(Expr &r_expr, const FuncDecl &p_function, const std::string &p_owner) {
	r_expr.target = Target::StaticValue;
	r_expr.callee = &p_function;
	r_expr.owner = p_owner;
	return set(r_expr, function_type(p_function));
}

// A method of this language as a value (spec §3.11), found by name on the
// object's script when it is called.
Type Checker::method_value(Expr &r_expr, const FuncDecl &p_method) {
	if (!receiver_kept(r_expr)) {
		return Type::of(TypeKind::Error);
	}
	r_expr.target = Target::MethodValue;
	r_expr.callee = &p_method;
	return function_type(p_method);
}

// The methods an interface declares (spec §3.9).
const InterfaceDecl *Checker::interface_decl(const Type &p_interface) const {
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

// The interfaces from `p_roots` up through those they extend, each once
// and before those it extends.
std::vector<Type> Checker::interface_order(const std::vector<Type> &p_roots) const {
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
std::vector<Checker::InterfaceMember> Checker::interface_members(const std::vector<Type> &p_roots) const {
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

std::optional<Checker::InterfaceMember> Checker::interface_member(const Type &p_interface, const std::string &p_name) const {
	for (const InterfaceMember &member : interface_members({ p_interface })) {
		if (member.name() == p_name) {
			return member;
		}
	}
	return std::nullopt;
}

const InterfaceProperty *Checker::interface_property(const Type &p_interface, const std::string &p_name) const {
	const std::optional<InterfaceMember> member = interface_member(p_interface, p_name);
	return member ? member->property : nullptr;
}

// A pub method of an object of a class of this language, or a method of an
// interface, which this language calls directly (spec §8.1), also when it
// overrides an engine virtual method.
std::optional<Type> Checker::script_method(Expr &r_expr, const Type &p_class) {
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
		callee = find_named(decl->all_functions, r_expr.text);
		if (callee) {
			const ClassDecl &declarer = declaring(*decl, *callee, &ClassDecl::functions);
			source = declarer.source.get();
			owner = declarer.path;
			id = script_id(owner, callee->name);
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
Type Checker::call_declared(Expr &r_expr, const FuncDecl &p_callee, const SourceFile &p_source, const Type &p_result) {
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
Type Checker::check_engine_constant(Expr &r_expr, const std::string &p_class) {
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

Type Checker::check_member_access(Expr &r_expr) {
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
	if (const std::optional<Type> engine_enum = named_engine_enum(base, false)) {
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
	// Such as Tween.TransitionType, Vector3.Axis or Input.MouseMode, an enum's name as a value (spec §3.7).
	const std::optional<Type> enum_named = names_type(base, true) ? named_engine_enum(base.text + "." + r_expr.text) : std::nullopt;
	if (enum_named || names_type(base)) {
		refer(base.name_token, engine_id(base.text));
		if (enum_named) {
			refer(r_expr.name_token, engine_id(base.text, r_expr.text));
			return use_engine_enum_dictionary(r_expr, *enum_named);
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
	if (const std::optional<Type> named = constructed_struct(base, false)) {
		return named->is_error() ? set(r_expr, *named) : struct_method_value(r_expr, *named, false);
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

} // namespace petrichor::compiler::checking
