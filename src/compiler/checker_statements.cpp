#include "compiler/checker_internal.h"

namespace petrichor::compiler::checking {

/* Blocks and match (spec §5.6) */

// Returns what is known at the end of the block, starting from `p_facts`.
std::unordered_map<int, Type> Checker::check_block(Block &r_block, const std::unordered_map<int, Type> &p_facts) {
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

int Checker::declare_local(const std::string &p_name, uint32_t p_at, const Type &p_type, bool p_constant) {
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

// `match` (spec §5.6). The matched value goes in a local without a name,
// which the debugger shows as a temporary.
void Checker::check_match(Stmt &r_stmt) {
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

// A pattern of the value in the local `p_local`, which a value pattern
// becomes the comparison of with the pattern, checked as == is.
void Checker::check_pattern(Pattern &r_pattern, const Type &p_matched, int p_local) {
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
			auto compare = synthetic(ExprKind::Binary, std::string(), r_pattern.value->range);
			compare->op = TokenKind::EqualEqual;
			compare->left = synthetic(ExprKind::Name, std::string(), r_pattern.value->range);
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
void Checker::check_constructor_pattern(Pattern &r_pattern, const Type &p_matched, int p_local) {
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
	if (const std::optional<Type> constructed = constructed_struct(callee, true)) {
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
void Checker::check_container_pattern(Pattern &r_pattern, const Type &p_matched) {
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
void Checker::check_items(Pattern &r_pattern, const std::vector<std::string> &p_names, const std::vector<Type> &p_types, const std::string &p_written, const std::string &p_field_id) {
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

// The constructors of a type whose values they all make: its variants,
// None and Some, the values of an enum of this language, false and true,
// a struct's one; nullopt when there are too many to list.
std::optional<std::vector<int64_t>> Checker::constructors(const Type &p_type) const {
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
std::vector<Type> Checker::constructor_fields(const Type &p_type, int64_t p_constructor) const {
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

Checker::Shape Checker::shape_of(const Pattern &p_pattern, const Type &p_type) const {
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
std::vector<std::vector<Checker::Shape>> Checker::specialized(const std::vector<std::vector<Shape>> &p_rows, int64_t p_constructor, size_t p_arity) {
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
bool Checker::useful(const std::vector<std::vector<Shape>> &p_rows, const std::vector<Shape> &p_query, const std::vector<Type> &p_types) const {
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
void Checker::check_exhaustive(Stmt &r_match, const Type &p_matched) {
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
bool Checker::pattern_fits(const Type &p_pattern, const Type &p_matched) const {
	const auto text = [](const Type &p_type) {
		return p_type.kind == TypeKind::String || p_type == Type::builtin("StringName");
	};
	if (p_matched.kind == TypeKind::Variant || p_pattern == p_matched || (text(p_pattern) && text(p_matched))) {
		return true;
	}
	const bool subtype = (p_pattern.kind == TypeKind::Object && p_matched.kind == TypeKind::Object) || (p_pattern.kind == TypeKind::Enum && p_matched.kind == TypeKind::Int);
	return subtype && assignable(p_pattern, p_matched);
}

/* Loops and other statements (spec §5.7) */

// `for i in n`, over the elements of an array, over the keys of a
// Dictionary, over the characters of a String, or over an object (spec §5.7).
void Checker::check_for(Stmt &r_stmt) {
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
Type Checker::check_iteration(Stmt &r_stmt, const Type &p_object) {
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
	const TokenRange &at = r_stmt.value->range;
	for (const char *method : methods) {
		auto call = synthetic(ExprKind::Call, method, at);
		call->base = synthetic(ExprKind::Name, holders[0], at);
		call->args.push_back(synthetic(ExprKind::Name, method == methods[2] ? holders[2] : holders[1], at));
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

bool Checker::predelete_branch(const Expr &p_condition, bool p_true) const {
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
bool Checker::predelete_branch(const Expr &p_matched, const MatchBranch &p_branch) const {
	for (const Pattern &pattern : p_branch.patterns) {
		if (pattern.kind == PatternKind::Value && is_notification(p_matched) && is_predelete(*pattern.value->right)) {
			return true;
		}
	}
	return false;
}

void Checker::check_stmt(Stmt &r_stmt) {
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
				returns->values.push_back(r_stmt.value.get());
				add_returned(check_value(*r_stmt.value, returns->expected), r_stmt.value->range);
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

} // namespace petrichor::compiler::checking
