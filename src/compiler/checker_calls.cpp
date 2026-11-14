#include "compiler/checker_internal.h"

namespace petrichor::compiler::checking {

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

/* Calls */

// "2", "1 to 3" or "at least 1".
std::string Checker::argument_count(int p_required, int p_max, bool p_vararg) {
	if (p_vararg) {
		return "at least " + std::to_string(p_required);
	}
	return (p_required == p_max ? std::string() : std::to_string(p_required) + " to ") + std::to_string(p_max);
}

// Checks the arguments of a call against parameter types.
// With `p_vararg`, those after `p_params` are of type `p_rest`.
void Checker::check_arguments(Expr &r_call, const std::vector<Type> &p_params, int p_required, bool p_vararg, const Type &p_rest) {
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

std::vector<Type> Checker::engine_params(const EngineMethod &p_method, Expr &r_call, bool &r_ok) {
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
Type Checker::engine_result(const EngineMethod &p_method) {
	Type type = p_method.return_type;
	if (type.kind == TypeKind::Object && builtin::returns_non_null(p_method.owner, p_method.name)) {
		type.nullable = false;
	}
	return type;
}

// The property's type, and the range its setter takes (spec §10.6); Error
// after reporting a type this language cannot hold yet.
Type Checker::property_type(const EngineProperty &p_property, Expr &r_expr) {
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
Type Checker::engine_method_value(Expr &r_expr, const EngineMethod &p_method) {
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
Type Checker::tried_value(Expr &r_expr, const EngineMethod &p_method, const EngineMethod &p_getter) {
	bool ok = false;
	const std::vector<Type> params = engine_params(p_method, r_expr, ok);
	if (!ok) {
		return set(r_expr, Type::of(TypeKind::Error));
	}
	const Type type = Type::function(tried_result(p_method, p_getter), params);
	if (in_constant) {
		return set(r_expr, type); // No constant, as no other function value is (spec §4.5).
	}
	std::unique_ptr<Expr> call = synthetic(ExprKind::Call, r_expr.text, r_expr.range);
	call->name_token = r_expr.name_token;
	call->base = synthetic(ExprKind::Name, r_expr.base->text, r_expr.range);
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
		call->args.push_back(synthetic(ExprKind::Name, param.name, r_expr.range));
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

bool Checker::receiver_kept(const Expr *p_base, const TokenRange &p_range) {
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
Type Checker::check_engine_call(Expr &r_expr, const EngineMethod &p_method, std::string p_static_class) {
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

// The member of the kind named `p_name` that `p_class` has, as `object.name`
// finds it: a member of a class of this language, of an interface, of
// another language's class, or of the engine class.
Checker::NamedMember Checker::named_member(const Type &p_class, builtin::MemberName::Kind p_kind, const std::string &p_name) {
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
				if (const VarDecl *member = find_named(decl->all_members, p_name)) {
					out.found = true;
					out.hidden = !member->is_pub;
					out.id = script_id(declaring(*decl, *member, &ClassDecl::members).path, member->name);
					out.type = member->type;
					return out;
				}
			} else if (p_kind == Kind::Method) {
				if (const FuncDecl *function = find_named(decl->all_functions, p_name)) {
					from_function(*function, declaring(*decl, *function, &ClassDecl::functions).path);
					return out;
				}
			} else if (const SignalDecl *signal = find_named(decl->all_signals, p_name)) {
				out.found = true;
				out.hidden = !signal->is_pub;
				out.id = script_id(declaring(*decl, *signal, &ClassDecl::signals).path, signal->name);
				out.params = param_types(signal->params);
				return out;
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
std::optional<Checker::NamedMember> Checker::literal_member(Expr &r_call, const builtin::MemberName &p_named, std::vector<std::string> &r_path) {
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
void Checker::check_member_name(Expr &r_call, const EngineMethod &p_method, const builtin::MemberName &p_named) {
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
			} else {
				expect_scene_fits(value, *type);
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
			} else {
				expect_scene_fits(arg, param);
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
std::optional<Type> Checker::indexed_type(const Type &p_type, const std::string &p_name) {
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
Type Checker::filtered_result(const Expr &p_call, const EngineMethod &p_method) {
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
std::optional<std::vector<Type>> Checker::api_params(Expr &r_call, const builtin::Argument *p_args, uint32_t p_count, int &r_required) {
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
bool Checker::api_result_supported(Expr &r_call, const Type &p_result) {
	if (p_result.kind == TypeKind::Unsupported) {
		fail(r_call, "\"" + r_call.text + "()\" returns " + p_result.to_string() + ", which is not supported yet.");
		return false;
	}
	return true;
}

Type Checker::check_utility(Expr &r_expr, const builtin::Utility &p_utility) {
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
Type Checker::construct_from_variant(Expr &r_expr, const Type &p_type) {
	r_expr.target = Target::ConstructFromVariant;
	r_expr.param_types = { Type::of(TypeKind::Variant) };
	return set(r_expr, p_type);
}

Type Checker::check_constructor(Expr &r_expr, const builtin::Class &p_type) {
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
Type Checker::check_named_value(Expr &r_expr, bool p_signal) {
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

Type Checker::role_type(bytecode::ElementRole p_role, const Type &p_container) {
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
void Checker::container_typed(const bytecode::ContainerMethod &p_method, const Type &p_container, std::vector<Type> &r_params, Type &r_result) {
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
bool Checker::has_less(const Type &p_type) const {
	std::set<std::string> visiting;
	return has_less(p_type, visiting);
}

bool Checker::has_less(const Type &p_type, std::set<std::string> &r_visiting) const {
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

// Whether a value of `p_from` becomes a `p_to` when wrapped in Some as
// many times as needed (spec §5.2).
bool Checker::wraps_to(const Type &p_from, const Type &p_to) const {
	if (p_from == p_to || (p_from.kind != TypeKind::Option && p_to.kind != TypeKind::Option && assignable(p_from, p_to))) {
		return true;
	}
	return p_to.kind == TypeKind::Option && wraps_to(p_from, p_to.args[0]);
}

// Methods that order a container's elements, or a Dictionary's keys, need
// `<` on them, and resize() a default value for the elements (spec §3.8).
bool Checker::check_container_method(const Expr &p_call, const Type &p_base, const bytecode::ContainerMethod *p_container) {
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
Type Checker::element_function(const std::string &p_method, const Type &p_array) {
	const Type element = p_array.args.empty() ? Type::of(TypeKind::Variant) : p_array.args[0];
	if (p_method == "sort_custom" || p_method == "bsearch_custom") {
		return Type::function(Type::of(TypeKind::Bool), { element, element });
	}
	return Type::function(p_method == "map" ? Type::of(TypeKind::Error) : Type::of(TypeKind::Bool), { element });
}

// A method of a value of a built-in type, or with `p_static` a static one
// called through the type's name, as Color.from_hsv().
Type Checker::check_builtin_method(Expr &r_expr, const Type &p_base, bool p_static) {
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
Type Checker::check_bind(Expr &r_expr, const Type &p_function) {
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
Type Checker::check_function_call(Expr &r_expr, const Type &p_function) {
	const bool spread = r_expr.text == "callv";
	const std::vector<Type> params(p_function.args.begin() + 1, p_function.args.end());
	hint(r_expr, [&](int p_current) { return signature(r_expr.text, spread ? std::vector<std::string>{ "arguments: Array" } : type_names(params), false, p_current, p_function.args[0].to_string()); });
	check_arguments(r_expr, spread ? std::vector<Type>{ Type::builtin("Array") } : params, spread ? 1 : int(params.size()), false);
	r_expr.target = Target::CallValue;
	refer(r_expr.name_token, engine_id("Callable", r_expr.text));
	r_expr.is_async = p_function.is_async;
	return set(r_expr, p_function.is_async ? Type::task(p_function.args[0]) : p_function.args[0]);
}

// The struct that a base or, with `p_call`, a call names (spec §2.4):
// `Hit` in the file that declares it, when no local or function has the
// name, `Combat.Hit` anywhere; Error after reporting one that may not be
// used here.
std::optional<Type> Checker::constructed_struct(const Expr &p_name, bool p_call) {
	if (!may_name(p_name, p_call)) {
		return std::nullopt;
	}
	const std::string &name = p_name.text;
	TypeRef ref;
	ref.range = p_name.range;
	ref.name_token = p_name.name_token;
	if (!p_name.base) {
		if (visible_local(name) || function_index.count(name)) {
			return std::nullopt;
		}
		if (!cls.find_struct(name)) {
			// One of a class around this one (spec §4.1).
			const Checker *around = class_around(name);
			if (!around || !around->cls.find_struct(name)) {
				if (name == "ConversionError" && !declares_in_script(name)) {
					refer(p_name.name_token, language_id(name, std::string()));
					return Type::conversion_error();
				}
				return std::nullopt;
			}
		}
		ref.name = name;
	} else {
		const Expr &base = *p_name.base;
		const std::string path = class_path_of(base, true);
		const ClassDecl *header = path.empty() ? nullptr : script_header(path);
		if (!header || !header->find_struct(name)) {
			return std::nullopt;
		}
		ref.name_token = first_name_token(base); // Each name two tokens after the one before.
		ref.name = dotted_name(base) + "." + name;
	}
	return script_type(ref);
}

// The type that the base of `T.try_from(v)` names: `int`, `Combat.Hit`,
// `Array[Hit]` or `Dictionary[String, int]`; nullopt for a value, with
// nothing referred to or reported.
std::optional<Type> Checker::named_type(const Expr &p_base) {
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
bool Checker::type_ref_of(const Expr &p_expr, TypeRef &r_ref) {
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
Type Checker::check_try_from(Expr &r_expr, const Type &p_type) {
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
const EnumDecl *Checker::payload_enum_named(Expr &r_base) {
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
Type Checker::check_variant_construction(Expr &r_expr, const EnumDecl &p_decl) {
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
void Checker::check_named_fields(Expr &r_expr, const std::vector<VarDecl> &p_fields, const std::string &p_kind, const std::string &p_written, const std::string &p_script, const std::string &p_id) {
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
Type Checker::check_struct_construction(Expr &r_expr, const Type &p_struct) {
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
Type Checker::check_static_call(Expr &r_expr) {
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
Type Checker::tried_result(const EngineMethod &p_method, const EngineMethod &p_getter) {
	Type value = engine_result(p_method);
	value.nullable = false;
	return Type::result(value, p_getter.return_type);
}

// What new(...) of the class at `p_path` calls: the nearest _init(), whose
// arguments it takes and whose visibility it has, or null for none (spec
// §4.2, §6.1). False after reporting why the class cannot be made here.
bool Checker::find_init(Expr &r_expr, const ClassDecl &p_header, const std::string &p_path, const FuncDecl *&r_init) {
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
		r_init = init_of(*decl);
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
Type Checker::check_class_call(Expr &r_expr, const std::string &p_path) {
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
		check_member_arguments(r_expr, type, false);
		return set(r_expr, type);
	}
	const ClassDecl *decl = script_decl(p_path);
	if (!decl) {
		return fail_call(r_expr, class_name + " has errors.");
	}
	const FuncDecl *found = find_named(decl->all_functions, r_expr.text);
	if (!found) {
		return fail_call(r_expr, missing(class_name + " has no method \"" + r_expr.text + "()\".", p_path));
	}
	const FuncDecl &callee = *found;
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
		r_expr.index = function_index.at(callee.name);
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
Type Checker::check_struct_call(Expr &r_expr, const Type &p_struct, bool p_on_value) {
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
Type Checker::script_base(Expr &r_access, const std::string &p_path) {
	if (!r_access.base->constant) {
		auto path = synthetic(ExprKind::StringLiteral, p_path, r_access.base->range);
		path->op = TokenKind::String;
		auto load = synthetic(ExprKind::Call, "preload", r_access.base->range);
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
Type Checker::check_foreign_class_call(Expr &r_expr, const Type &p_class, bool p_value) {
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
Type Checker::check_foreign_constant(Expr &r_expr, const Type &p_class) {
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

Type Checker::check_call(Expr &r_expr, const Type &p_expected) {
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
bool Checker::split_members(Expr &r_call) {
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
// must give those that _init does not assign and the scene does not
// export, and of a scene statically known, `p_scene`, the exports it does
// not assign (§7.4).
void Checker::check_member_arguments(Expr &r_call, const Type &p_made, bool p_instantiate, const Scene *p_scene) {
	r_call.member_types.clear();
	const ClassDecl *decl = p_made.kind == TypeKind::Object && !p_made.script.empty() ? script_decl(p_made.script) : nullptr;
	std::unordered_set<std::string> given;
	for (size_t i = 0; i < r_call.member_values.size(); i++) {
		Expr &value = *r_call.member_values[i];
		const std::string &name = r_call.member_names[i];
		const VarDecl *found = decl ? find_named(decl->all_members, name) : nullptr;
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
		if (!member->must_assign || given.count(member->name) || init_assigns(*decl, member->name) || (member->is_export && (!p_scene || stored_value(*p_scene, ".", member->name).value_or(false)))) {
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

Type Checker::type_call(Expr &r_expr, const Type &p_expected) {
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
	if (const std::optional<Type> constructed = constructed_struct(r_expr, true)) {
		return check_struct_construction(r_expr, *constructed);
	}
	if (const EnumDecl *decl = named_enum(r_expr, true)) {
		if (decl->has_payloads) {
			return fail_call(r_expr, "No int converts to " + enum_spelling(decl->type) + ", an enum with payloads.");
		}
		return check_enum_conversion(r_expr, decl->type);
	}
	if (const std::optional<Type> found = named_engine_enum(r_expr, true)) {
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
		return type_method_call(r_expr, p_expected);
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
			const std::string where = "\n   At: " + options.path + ":" + std::to_string(cls.source->line(r_expr.range.first)) + ":" + outermost()->name + "()";
			r_expr.args.push_back(synthetic(ExprKind::StringLiteral, where, r_expr.range));
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
		r_expr.language = language_function(LanguageFunction::Of::GDScript, name);
		if (r_expr.language) {
			return r_expr.language->id == LanguageFunction::Id::IsInstanceOf ? check_is_instance_of(r_expr) : check_gdscript_utility(r_expr);
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

// A call on a base other than self in a class's function: a method of
// the value, or a function of the class or type that the base names.
Type Checker::type_method_call(Expr &r_expr, const Type &p_expected) {
	const std::string &name = r_expr.text;
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
	if (const std::optional<Type> named = constructed_struct(base, false)) {
		return named->is_error() ? fail_call(r_expr) : check_struct_call(r_expr, *named, false);
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
	if (base_type.is_optional() || base_type.is_result()) {
		return check_language_method(r_expr, base_type, p_expected);
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
				check_member_arguments(r_expr, base_type.args[0], true, scene ? &*scene : nullptr);
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

// GDScript's range(to), range(from, to) and range(from, to, step) of ints,
// without converting floats as GDScript does (spec §3.14).
Type Checker::check_range(Expr &r_expr) {
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
Type Checker::check_gdscript_utility(Expr &r_expr) {
	using Id = LanguageFunction::Id;
	const LanguageFunction &utility = *r_expr.language;
	refer(r_expr.name_token, engine_id("@GDScript", utility.name));
	if (utility.id == Id::Char) {
		// String.chr() of a code that fits a char32, as GDScript checks it.
		r_expr.base = synthetic(ExprKind::Name, "String", r_expr.range);
		r_expr.text = "chr";
		r_expr.name_token = NO_TOKEN;
		const Type result = check_call(r_expr);
		r_expr.arg_ranges.assign(1, bytecode::IntRange::CHAR32);
		return result;
	}
	r_expr.target = Target::GDScriptUtility;
	if (utility.id == Id::Len) {
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
	if (utility.id == Id::Ord) {
		check_arguments(r_expr, { Type::of(TypeKind::String) }, 1, false);
		return set(r_expr, Type::of(TypeKind::Int));
	}
	check_arguments(r_expr, {}, 0, false);
	return set(r_expr, utility.id == Id::GetStack ? Type::array(Type::builtin("Dictionary")) : Type::of(TypeKind::Void));
}

// is_instance_of(v, T) of a class name or a TYPE_* constant, as `v is T`;
// of a Script value, by the rules of `is` without narrowing (spec §2.6).
Type Checker::check_is_instance_of(Expr &r_expr) {
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
bool Checker::written_type(const Expr &p_expr) const {
	if (p_expr.kind == ExprKind::Member) {
		return p_expr.base && written_type(*p_expr.base);
	}
	return p_expr.kind == ExprKind::Name && !names_variable(p_expr.text) && !(options.project && !options.project->autoload_path(p_expr.text).empty());
}

Type Checker::check_script_test(Expr &r_expr) {
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

} // namespace petrichor::compiler::checking
