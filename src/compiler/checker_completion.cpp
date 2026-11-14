#include "compiler/checker_internal.h"

namespace petrichor::compiler::checking {

// Where the script editor's word at the cursor begins, which an option
// replaces: after a symbol other than "/" (scene/gui/code_edit.cpp:3869-3875,
// core/string/char_utils.h:127-129; ed1daf0).
size_t word_start(const std::string &p_text, size_t p_cursor) {
	const auto symbol = [](char p_char) {
		return p_char != '_' && ((p_char >= '!' && p_char <= '/') || (p_char >= ':' && p_char <= '@') || (p_char >= '[' && p_char <= '`') || (p_char >= '{' && p_char <= '~'));
	};
	while (p_cursor > 0 && (p_text[p_cursor - 1] == '/' || ((unsigned char)p_text[p_cursor - 1] > ' ' && !symbol(p_text[p_cursor - 1])))) {
		p_cursor--;
	}
	return p_cursor;
}

/* Completion (spec §13.2) */

void Checker::engine_candidates(const std::string &p_class, Candidates &r_names) const {
	for (const std::string &name : engine.member_names(p_class)) {
		r_names.values.insert(name);
		if (const std::string tried = builtin::try_version(name); builtin::error_getter(p_class, tried)) {
			r_names.values.insert(tried);
		}
	}
	for (const std::string &name : engine.enum_names(p_class)) {
		r_names.types.insert(name);
	}
}

// What a class has, also through its bases: what code names in it, through
// it or through its objects.
void Checker::class_candidates(const ClassDecl &p_decl, Candidates &r_names) const {
	const auto add = [&](const auto &p_declarations) {
		for (const auto *declaration : p_declarations) {
			r_names.values.insert(declaration->name);
		}
	};
	add(p_decl.all_members);
	add(p_decl.all_static_vars);
	add(p_decl.all_constants);
	add(p_decl.all_functions);
	add(p_decl.all_signals);
	for (const ClassDecl *decl = &p_decl; decl; decl = decl->base) {
		for (const std::unique_ptr<ClassDecl> &inner : decl->classes) {
			r_names.types.insert(inner->name);
			r_names.values.insert(inner->name);
		}
		for (const StructDecl &found : decl->structs) {
			r_names.types.insert(found.name);
			r_names.values.insert(found.name);
		}
		for (const InterfaceDecl &found : decl->interfaces) {
			r_names.types.insert(found.name);
		}
		for (const EnumDecl &found : decl->enums) {
			if (!found.name.empty()) {
				r_names.types.insert(found.name);
				r_names.values.insert(found.name);
			}
			for (const EnumValueDecl &value : found.values) {
				r_names.values.insert(value.name);
			}
		}
	}
	engine_candidates(p_decl.native_base, r_names);
}

// What `Class.` and `object.` reach of a class of this language, of
// another or of the engine.
void Checker::script_candidates(const Type &p_class, Candidates &r_names) {
	if (p_class.is_interface) {
		for (const InterfaceMember &member : interface_members({ p_class })) {
			r_names.values.insert(member.name());
		}
	} else if (is_script_path(p_class.script)) {
		if (const ClassDecl *decl = script_decl(p_class.script)) {
			class_candidates(*decl, r_names);
		}
	} else if (const ForeignClass *foreign = p_class.script.empty() ? nullptr : foreign_class(p_class.script)) {
		for (const std::vector<EngineProperty> *properties : { &foreign->properties, &foreign->constants }) {
			for (const EngineProperty &property : *properties) {
				r_names.values.insert(property.name);
			}
		}
		for (const std::vector<EngineMethod> *methods : { &foreign->methods, &foreign->signals }) {
			for (const EngineMethod &method : *methods) {
				r_names.values.insert(method.name);
			}
		}
	}
	engine_candidates(p_class.engine_class(), r_names);
}

void Checker::builtin_candidates(const builtin::Class &p_type, Candidates &r_names) {
	for (uint32_t i = 0; i < p_type.member_count; i++) {
		r_names.values.insert(p_type.members[i].name);
	}
	for (uint32_t i = 0; i < p_type.constant_count; i++) {
		r_names.values.insert(p_type.constants[i].name);
	}
	for (uint32_t i = 0; i < p_type.method_count; i++) {
		r_names.values.insert(p_type.methods[i].name);
	}
}

// What may follow `value.` for a value of the type.
void Checker::value_candidates(const Type &p_type, Candidates &r_names) {
	if (p_type.is_optional() || p_type.is_result()) {
		for (const LanguageFunction &method : LANGUAGE_FUNCTIONS) {
			if (method.of == (p_type.is_result() ? LanguageFunction::Of::Result : LanguageFunction::Of::Option)) {
				r_names.values.insert(method.name);
			}
		}
		return;
	}
	if (p_type.kind == TypeKind::Task) {
		r_names.values.insert({ "cancel", "detach", "settled" });
	}
	if (p_type.kind == TypeKind::Struct) {
		if (const StructDecl *decl = struct_decl(p_type)) {
			struct_candidates(*decl, r_names);
		}
	}
	if (p_type.kind == TypeKind::Object) {
		script_candidates(p_type, r_names);
	}
	if (const builtin::Class *type = builtin::find_class(p_type.builtin_name())) {
		builtin_candidates(*type, r_names);
	}
}

void Checker::struct_candidates(const StructDecl &p_decl, Candidates &r_names) {
	for (const VarDecl &field : p_decl.fields) {
		r_names.values.insert(field.name);
	}
	for (const FuncDecl &method : p_decl.methods) {
		r_names.values.insert(method.name.substr(p_decl.name.size() + 1));
	}
}

// The names code in this class sees bare: the locals, those of the classes
// around it, the project's and the engine's.
void Checker::scope_candidates(Candidates &r_names) const {
	const auto locals = [&](const std::vector<std::unordered_map<std::string, int>> &p_scopes) {
		for (const auto &scope : p_scopes) {
			for (const auto &local : scope) {
				r_names.values.insert(local.first);
			}
		}
	};
	locals(scopes);
	for (const Enclosing &outer : enclosing) {
		locals(outer.scopes);
	}
	if (structure) {
		struct_candidates(*structure, r_names);
	}
	for (const Checker *around = this; around; around = around->outer_checker) {
		class_candidates(around->cls, r_names);
		if (!around->cls.class_name.empty()) {
			r_names.types.insert(around->cls.class_name);
		}
	}
	for (const std::string &name : engine.class_list()) {
		r_names.types.insert(name);
	}
	std::vector<std::string> types;
	std::vector<std::string> values;
	builtin::global_names(types, values);
	r_names.types.insert(types.begin(), types.end());
	r_names.values.insert(values.begin(), values.end());
	if (options.project) {
		for (const std::string &name : options.project->autoload_names()) {
			r_names.values.insert(name);
		}
	}
}

// The project's global classes, or with `p_extendable` those of this
// language other than this class, by the names the project lists them by:
// not tried, since a name in that list can only fail to resolve when its
// script cannot be read, and trying reads each script.
void Checker::add_global_classes(std::map<std::string, CompletionItem> &r_found, bool p_extendable) const {
	if (!options.project) {
		return;
	}
	for (const auto &[name, path] : options.project->global_class_paths()) {
		if (p_extendable && (!is_script_path(path) || path == options.path)) {
			continue;
		}
		CompletionItem item;
		item.kind = CompletionKind::Class;
		item.name = name;
		item.insert = name;
		item.place = CompletionItem::Place::Project;
		r_found.emplace(name, item);
	}
}

void Checker::set_completions(std::map<std::string, CompletionItem> &r_found) {
	completion->items.clear();
	for (auto &entry : r_found) {
		completion->items.push_back(std::move(entry.second));
	}
}

// Gives completion the signature of `p_call` when the cursor is among its
// arguments and not among those of a call within them. `p_signature` makes
// it with the parameter at the cursor marked.
void Checker::hint(const Expr &p_call, const std::function<std::string(int)> &p_signature) {
	if (!completion || probing || p_call.name_token >= cls.source->tokens.size()) {
		return;
	}
	const size_t cursor = cls.source->text.find(CURSOR);
	const uint32_t name_end = cls.source->tokens[p_call.name_token].end();
	if (cursor < name_end || cursor >= cls.source->end(p_call.range) || name_end < hint_at) {
		return;
	}
	int current = 0;
	for (const auto &arg : p_call.args) {
		current += cls.source->end(arg->range) < cursor ? 1 : 0;
	}
	hint_at = name_end;
	completion->call_hint = p_signature(current);
}

// `name(a: T, b: U = 1) -> R`, the parameter at `p_current` between two
// U+FFFF; with `p_vararg`, a last `...args` takes the rest.
std::string Checker::signature(const std::string &p_name, std::vector<std::string> p_params, bool p_vararg, int p_current, const std::string &p_result) {
	if (p_vararg) {
		p_params.push_back("...args");
		p_current = std::min(p_current, int(p_params.size()) - 1);
	}
	std::string text = p_name + "(";
	for (size_t i = 0; i < p_params.size(); i++) {
		const std::string mark = int(i) == p_current ? CURSOR : "";
		text += (i > 0 ? ", " : "") + mark + p_params[i] + mark;
	}
	return text + ") -> " + p_result;
}

// The parameters of a function value or a signal, which have no names.
std::vector<std::string> Checker::type_names(const std::vector<Type> &p_types) {
	std::vector<std::string> names;
	for (const Type &type : p_types) {
		names.push_back(type.to_string());
	}
	return names;
}

// A function of this language's, its defaults as written in `p_source`.
std::string Checker::declared_signature(const std::string &p_name, const FuncDecl &p_function, const SourceFile &p_source, int p_current, const Type &p_result) {
	std::vector<std::string> params;
	for (const ParamDecl &param : p_function.params) {
		params.push_back((param.is_rest ? "..." : "") + param.name + ": " + param.type.to_string());
		if (param.default_value) {
			const uint32_t begin = p_source.begin(param.default_value->range);
			params.back() += " = " + p_source.text.substr(begin, p_source.end(param.default_value->range) - begin);
		}
	}
	// A rest parameter takes the arguments after the others.
	if (p_function.has_rest()) {
		p_current = std::min(p_current, int(params.size()) - 1);
	}
	return signature(p_name, params, false, p_current, p_result.to_string());
}

// A function of the engine's API (builtin_api.h).
std::string Checker::api_signature(const std::string &p_name, const builtin::Argument *p_args, uint32_t p_count, bool p_vararg, const char *p_result, int p_current) {
	std::vector<std::string> params;
	for (uint32_t i = 0; i < p_count; i++) {
		params.push_back(std::string(p_args[i].name) + ": " + api_type(p_args[i].type, true).to_string());
		if (p_args[i].has_default) {
			params.back() += std::string(" = ") + p_args[i].default_value;
		}
	}
	return signature(p_name, params, p_vararg, p_current, *p_result ? api_type(p_result, true).to_string() : "void");
}

// The classes a member may be declared in, nearest first: one of this
// language and its bases, then the engine's; or a built-in type.
std::vector<std::string> Checker::class_chain(const Type &p_class) {
	std::vector<std::string> chain;
	for (std::string path = p_class.script; is_script_path(path) && std::find(chain.begin(), chain.end(), path) == chain.end();) {
		chain.push_back(path);
		const ClassDecl *header = script_header(path);
		path = header ? classes.base_path(*header, path) : std::string();
	}
	if (p_class.kind == TypeKind::Object) {
		for (std::string name = p_class.engine_class(); engine.class_exists(name); name = engine.parent_class(name)) {
			chain.push_back(name);
		}
	} else if (!p_class.builtin_name().empty()) {
		chain.push_back(p_class.builtin_name());
	}
	return chain;
}

CompletionKind Checker::value_kind(const Expr &p_checked) {
	switch (p_checked.target) {
		case Target::Local:
			return CompletionKind::Variable;
		case Target::MethodValue:
		case Target::EngineMethodValue:
		case Target::StaticValue:
		case Target::EngineStaticValue:
		case Target::StructMethodValue:
		case Target::NewValue:
			return CompletionKind::Function;
		case Target::SelfSignal:
		case Target::ObjectSignal:
			return CompletionKind::Signal;
		case Target::ClassScript:
		case Target::Singleton:
			return CompletionKind::Class;
		case Target::Constant:
			return p_checked.constant && p_checked.constant->names_enum ? CompletionKind::Enum : CompletionKind::Constant;
		case Target::EngineConstant:
		case Target::BuiltinConstant:
		case Target::Autoload:
			return CompletionKind::Constant;
		default:
			return CompletionKind::Member;
	}
}

// As what `p_name` checks in place of the name of `r_site`, the expression
// at the cursor: as a value, else as a call, whose arguments are not
// checked; nullopt if it does not. `p_chain` is class_chain() of the class
// whose members are completed.
std::optional<CompletionItem> Checker::probe(Expr &r_site, const std::string &p_name, const std::vector<std::string> &p_chain) {
	const bool on_self = r_site.base && r_site.base->kind == ExprKind::Self;
	const bool moves_base = r_site.base && !on_self;
	const uint32_t at = cls.source->tokens[r_site.name_token].offset;
	for (const bool call : { false, true }) {
		Expr probe;
		probe.kind = call ? ExprKind::Call : (r_site.base ? ExprKind::Member : ExprKind::Name);
		probe.text = p_name;
		probe.range = r_site.range;
		probe.name_token = r_site.name_token;
		if (on_self) {
			// A call drops it, as `self.f()` is `f()`.
			probe.base = synthetic(ExprKind::Self, std::string(), r_site.base->range);
		} else if (moves_base) {
			probe.base = std::move(r_site.base);
		}
		const size_t error_count = errors.size();
		const size_t reference_count = references->size();
		probing = true;
		probed_params = -1;
		const Type type = type_expr(probe, Type::of(TypeKind::Error));
		probing = false;
		const bool accepted = errors.size() == error_count && !type.is_error();
		const auto named = std::find_if(references->begin() + reference_count, references->end(), [&](const Reference &p_reference) {
			return p_reference.begin == at;
		});
		const std::optional<DeclId> target = named == references->end() ? std::nullopt : std::optional<DeclId>(named->target);
		errors.erase(errors.begin() + error_count, errors.end());
		references->erase(references->begin() + reference_count, references->end());
		if (moves_base) {
			r_site.base = std::move(probe.base);
		}
		if (!accepted) {
			continue;
		}
		CompletionItem item;
		item.name = p_name;
		item.insert = p_name;
		item.kind = call ? CompletionKind::Function : value_kind(probe);
		if (item.kind == CompletionKind::Function) {
			const int params = call ? probed_params : int(type.args.size()) - 1;
			item.insert += params == 0 ? "()" : "(";
		}
		const bool declared = target && (target->kind == DeclId::Kind::Script || target->kind == DeclId::Kind::Engine);
		const auto in_chain = declared ? std::find(p_chain.begin(), p_chain.end(), target->scope) : p_chain.end();
		if (probe.target == Target::Local) {
			item.place = CompletionItem::Place::Local;
		} else if (in_chain != p_chain.end()) {
			item.place = CompletionItem::Place::Member;
			item.depth = int(in_chain - p_chain.begin());
		} else if (target && (target->kind == DeclId::Kind::Script || target->kind == DeclId::Kind::Foreign || target->kind == DeclId::Kind::Autoload)) {
			item.place = CompletionItem::Place::Project;
		}
		return item;
	}
	return std::nullopt;
}

// As what type `p_name` resolves where `p_site` is, at the cursor.
std::optional<CompletionItem> Checker::probe_type(const TypeRef &p_site, const std::string &p_name, bool p_allow_void) {
	TypeRef ref;
	ref.name = p_name;
	ref.range = p_site.range;
	ref.name_token = NO_TOKEN;
	const size_t error_count = errors.size();
	const Type type = resolve_name(ref, p_allow_void);
	const bool accepted = errors.size() == error_count && !type.is_error();
	errors.erase(errors.begin() + error_count, errors.end());
	if (!accepted) {
		return std::nullopt;
	}
	CompletionItem item;
	item.kind = type.kind == TypeKind::Enum ? CompletionKind::Enum : CompletionKind::Class;
	item.name = p_name.substr(p_name.rfind('.') + 1);
	item.insert = item.name;
	item.place = type.script.empty() ? CompletionItem::Place::Engine : CompletionItem::Place::Project;
	return item;
}

// The keywords that can begin what is written at `r_site`, a bare name:
// an expression, or a statement when it is the whole statement.
void Checker::add_keywords(const Expr &r_site, std::map<std::string, CompletionItem> &r_found) {
	std::vector<std::pair<std::string, std::string>> words = { { "true", "" }, { "false", "" }, { "None", "" }, { "not", " " }, { "func", " " } };
	Expr self;
	self.kind = ExprKind::Self;
	self.range = r_site.range;
	const size_t error_count = errors.size();
	if (!type_expr(self, Type::of(TypeKind::Error)).is_error() && errors.size() == error_count) {
		words.insert(words.end(), { { "self", "" }, { "super", "" } });
	}
	errors.erase(errors.begin() + error_count, errors.end());
	if (no_await().empty()) {
		words.emplace_back("await", " ");
	}
	if (&r_site == statement_value) {
		words.insert(words.end(), { { "var", " " }, { "const", " " }, { "if", " " }, { "for", " " }, { "while", " " }, { "match", " " }, { "return", "" }, { "pass", "" } });
		if (loop_depth > 0) {
			words.insert(words.end(), { { "break", "" }, { "continue", "" } });
		}
		// After an if whose chain of elifs has no else yet.
		const Stmt *last = previous_statement && previous_statement->kind == StmtKind::If ? previous_statement : nullptr;
		while (last && last->else_body.size() == 1 && last->else_body[0]->kind == StmtKind::If) {
			last = last->else_body[0].get();
		}
		if (last && last->else_body.empty()) {
			words.insert(words.end(), { { "elif", " " }, { "else", "" } });
		}
	}
	for (const auto &[word, after] : words) {
		CompletionItem item;
		item.name = word;
		item.insert = word + after;
		r_found.emplace(word, item);
	}
	// Written as calls.
	for (const char *word : { "preload", "assert" }) {
		if (word == std::string("assert") && &r_site != statement_value) {
			continue;
		}
		CompletionItem item;
		item.kind = CompletionKind::Function;
		item.name = word;
		item.insert = item.name + "(";
		r_found.emplace(word, item);
	}
}

// Where an enum is expected at `r_site`, a bare name: its values (spec
// §3.7), each written as the first that checks there, bare or after its
// class or its name, to come first.
void Checker::add_enum_values(const Expr &r_site, const Type &p_expected, std::map<std::string, CompletionItem> &r_found) {
	const Type expected = p_expected.kind == TypeKind::Option ? p_expected.args[0] : p_expected;
	if (expected.kind != TypeKind::Enum || !expected.values) {
		return;
	}
	const std::string spelled = enum_spelling(expected);
	const size_t dot = spelled.rfind('.');
	const std::string owner = !expected.values->is_engine ? spelled : (dot == std::string::npos ? std::string() : spelled.substr(0, dot));
	for (const auto &value : expected.values->list) {
		for (const std::string &written : { value.first, owner.empty() ? std::string() : owner + "." + value.first }) {
			if (written.empty()) {
				continue;
			}
			std::unique_ptr<Expr> probe;
			for (size_t start = 0; start <= written.size();) {
				const size_t end = std::min(written.find('.', start), written.size());
				auto part = synthetic(probe ? ExprKind::Member : ExprKind::Name, written.substr(start, end - start), r_site.range);
				part->name_token = r_site.name_token;
				part->base = std::move(probe);
				probe = std::move(part);
				start = end + 1;
			}
			const size_t error_count = errors.size();
			const size_t reference_count = references->size();
			const Type type = type_expr(*probe, Type::of(TypeKind::Error));
			const bool accepted = errors.size() == error_count && !type.is_error() && assignable(type, expected);
			errors.erase(errors.begin() + error_count, errors.end());
			references->erase(references->begin() + reference_count, references->end());
			if (accepted) {
				CompletionItem item;
				item.kind = CompletionKind::Enum;
				item.name = written;
				item.insert = written;
				item.place = CompletionItem::Place::Local;
				r_found.emplace(written, item);
				completion->force = true;
				break;
			}
		}
	}
}

void Checker::complete(Expr &r_site, const Type &p_expected) {
	const size_t error_count = errors.size();
	const size_t reference_count = references->size();
	Candidates names;
	// The class whose members are completed.
	Type owner = script_class_type(cls, options.path);
	if (!r_site.base) {
		scope_candidates(names);
	} else if (r_site.base->kind == ExprKind::Self) {
		class_candidates(cls, names);
	} else {
		Expr &base = *r_site.base;
		const EnumDecl *named_values = named_enum(base);
		const std::optional<Type> named = named_values ? std::nullopt : named_class(base);
		if (named_values) {
			for (const EnumValueDecl &value : named_values->values) {
				names.values.insert(value.name);
			}
		} else if (named && !named->is_error()) {
			owner = *named;
			script_candidates(owner, names);
			names.values.insert("new");
		} else if (names_type(base)) {
			if (const builtin::Class *type = builtin::find_class(base.text)) {
				owner = api_type(base.text, false);
				builtin_candidates(*type, names);
			} else {
				owner = Type::object(base.text);
				engine_candidates(base.text, names);
				names.values.insert("new");
			}
		} else {
			owner = check_value(base);
			value_candidates(owner, names);
			const ClassDecl *inner = base.target == Target::ClassScript ? script_header(base.owner) : nullptr;
			if (inner) {
				// An inner class reached through an object (spec §4.1).
				owner = script_class_type(*inner, base.owner);
				script_candidates(owner, names);
			}
		}
	}
	errors.erase(errors.begin() + error_count, errors.end());
	references->erase(references->begin() + reference_count, references->end());
	const std::vector<std::string> chain = class_chain(owner);
	std::map<std::string, CompletionItem> found;
	if (r_site.kind == ExprKind::Name && !r_site.base) {
		add_enum_values(r_site, p_expected, found);
	}
	if (!r_site.base) {
		add_global_classes(found, false);
		for (const std::string &name : names.types) {
			std::optional<CompletionItem> item = found.count(name) ? std::nullopt : probe_type({ name, r_site.range }, name, false);
			if (item) {
				found.emplace(name, *item);
			}
		}
	}
	for (const std::string &name : names.values) {
		std::optional<CompletionItem> item = found.count(name) ? std::nullopt : probe(r_site, name, chain);
		if (item) {
			found.emplace(name, *item);
		}
	}
	if (r_site.kind == ExprKind::Name && !r_site.base) {
		add_keywords(r_site, found);
	}
	set_completions(found);
}

// Of the type at the cursor, `p_site`, which may follow `Class.`.
void Checker::complete_type(const TypeRef &p_site, bool p_allow_void) {
	const size_t dot = p_site.name.rfind('.', p_site.name.find(CURSOR));
	const std::string prefix = dot == std::string::npos ? std::string() : p_site.name.substr(0, dot + 1);
	Candidates names;
	std::map<std::string, CompletionItem> found;
	const size_t error_count = errors.size();
	if (prefix.empty()) {
		scope_candidates(names);
		names.types.insert({ "void", "Variant" });
		add_global_classes(found, false);
		if (options.project) {
			const std::vector<std::string> autoloads = options.project->autoload_names();
			names.types.insert(autoloads.begin(), autoloads.end());
		}
	} else {
		TypeRef outer;
		outer.name = prefix.substr(0, dot);
		outer.range = p_site.range;
		outer.name_token = NO_TOKEN;
		const Type type = resolve_name(outer, false);
		if (const ClassDecl *header = is_script_path(type.script) ? script_header(type.script) : nullptr) {
			for (const std::unique_ptr<ClassDecl> &inner : header->classes) {
				names.types.insert(inner->name);
			}
			for (const StructDecl &found : header->structs) {
				names.types.insert(found.name);
			}
			for (const InterfaceDecl &found : header->interfaces) {
				names.types.insert(found.name);
			}
			for (const EnumDecl &found : header->enums) {
				names.types.insert(found.name);
			}
		} else if (type.kind == TypeKind::Object) {
			for (const std::string &name : engine.enum_names(type.name)) {
				names.types.insert(name);
			}
		}
	}
	errors.erase(errors.begin() + error_count, errors.end());
	for (const std::string &name : names.types) {
		std::optional<CompletionItem> item = found.count(name) ? std::nullopt : probe_type(p_site, prefix + name, p_allow_void);
		if (item) {
			found.emplace(name, *item);
		}
	}
	set_completions(found);
}

// The classes that the class at the cursor, after `extends`, can extend:
// of the engine or of this language (spec §4.1).
void Checker::complete_extends() {
	std::map<std::string, CompletionItem> found;
	add_global_classes(found, true);
	std::vector<std::string> names = engine.class_list();
	for (const Checker *around = outer_checker; around; around = around->outer_checker) {
		for (const std::unique_ptr<ClassDecl> &inner : around->cls.classes) {
			names.push_back(inner->name);
		}
	}
	for (const std::string &name : names) {
		ClassDecl probe;
		probe.extends = name;
		const std::string base = classes.base_path(probe, options.path);
		if (classes.native_base(probe, options.path).empty() || base == options.path) {
			continue;
		}
		CompletionItem item;
		item.kind = CompletionKind::Class;
		item.name = name;
		item.insert = name;
		item.place = base.empty() ? CompletionItem::Place::Engine : CompletionItem::Place::Project;
		found.emplace(name, item);
	}
	set_completions(found);
}

// After `$` or `%` at the cursor: the paths that reach a node from the
// nodes with this script, in the scenes that have one (spec §7.1). An
// option replaces the editor's word at the cursor, or in a string what
// follows its quote, which the editor quotes itself
// (scene/gui/code_edit.cpp:3858-3860, 3916-3927; ed1daf0).
void Checker::complete_node_path(Expr &r_site) {
	const std::string written = r_site.text;
	const size_t cursor = cls.source->text.find(CURSOR);
	size_t word = word_start(cls.source->text, cursor);
	bool quoted = false;
	for (uint32_t i = r_site.range.first; i <= r_site.range.last && i < cls.source->tokens.size(); i++) {
		const Token &token = cls.source->tokens[i];
		if (token.kind == TokenKind::String && token.offset < cursor && cursor < token.end()) {
			quoted = true;
			word = token.offset + 1;
		}
	}
	const size_t before = written.find(CURSOR);
	const std::string prefix = written.substr(0, before - std::min(before, cursor - word));
	std::set<std::string> paths;
	for (const Scene &scene : scenes) {
		const auto below = [&](const std::string &p_node, const std::string &p_written) {
			const std::string start = p_node == "." ? std::string() : p_node + "/";
			for (const SceneNode &node : scene.nodes) {
				if (node.path != "." && node.path.compare(0, start.size(), start) == 0) {
					paths.insert(p_written + node.path.substr(start.size()));
				}
			}
		};
		for (const SceneNode &node : scene.nodes) {
			if (extends_script(node.script, options.path)) {
				below(node.path, "");
			}
			if (node.unique) {
				const std::string name = "%" + node.path.substr(node.path.rfind('/') + 1);
				paths.insert(name);
				below(node.path, name + "/");
			}
		}
	}
	std::map<std::string, CompletionItem> found;
	for (const std::string &path : paths) {
		if (path.compare(0, prefix.size(), prefix) != 0) {
			continue;
		}
		r_site.text = path;
		const size_t error_count = errors.size();
		const size_t reference_count = references->size();
		const bool accepted = !check_get_node(r_site).is_error() && errors.size() == error_count;
		errors.erase(errors.begin() + error_count, errors.end());
		references->erase(references->begin() + reference_count, references->end());
		if (accepted) {
			CompletionItem item;
			item.kind = CompletionKind::NodePath;
			item.name = path.substr(prefix.size());
			item.insert = quoted || plain_path(item.name) ? item.name : "\"" + item.name + "\"";
			item.place = CompletionItem::Place::Local;
			found.emplace(item.name, item);
		}
	}
	r_site.text = written;
	set_completions(found);
}

// After `func` at the cursor, `p_site`: what a function with its modifiers
// can be named, with the parameters of what it overrides and without
// types, which come from there (spec §4.6). Each is tried by
// check_signature(): the methods of the classes this one extends, and
// _static_init(), which no class inherits (§4.8).
void Checker::complete_override(const FuncDecl &p_site) {
	std::set<std::string> names = { "_static_init" };
	for (const std::string &name : engine.member_names(cls.native_base)) {
		names.insert(name);
	}
	for (size_t i = 0; cls.base && i < cls.base->all_functions.size(); i++) {
		if (!cls.base->all_functions[i]->of_struct) {
			names.insert(cls.base->all_functions[i]->name);
		}
	}
	const std::vector<std::string> chain = class_chain(script_class_type(cls, options.path));
	std::map<std::string, CompletionItem> found;
	for (const std::string &name : names) {
		if (std::any_of(cls.functions.begin(), cls.functions.end(), [&](const FuncDecl &p_function) { return p_function.name == name; })) {
			continue;
		}
		FuncDecl probe;
		probe.name = name;
		probe.name_token = p_site.name_token;
		probe.is_pub = p_site.is_pub;
		probe.is_static = p_site.is_static;
		probe.is_async = p_site.is_async;
		probe.is_abstract = p_site.is_abstract;
		probe.has_body = p_site.has_body;
		std::vector<std::string> params; // As they go in.
		std::string owner;
		const auto add_param = [&](const std::string &p_name) {
			probe.params.emplace_back().name = p_name;
			probe.params.back().name_token = NO_TOKEN;
			params.push_back(p_name);
		};
		const std::optional<EngineMethod> method = engine.find_method(cls.native_base, name);
		if (const FuncDecl *parent = name == "_init" ? nullptr : base_function(name)) {
			const ClassDecl &decl = declaring(*cls.base, *parent, &ClassDecl::functions);
			owner = decl.path;
			for (const ParamDecl &param : parent->params) {
				add_param(param.name);
				if (param.is_rest) {
					probe.params.back().is_rest = true;
					params.back() = "..." + params.back();
				}
				if (param.default_value) {
					// As checked where it is written.
					probe.params.back().default_value = std::make_unique<Expr>();
					probe.params.back().default_value->kind = ExprKind::Error;
					const uint32_t begin = decl.source->begin(param.default_value->range);
					params.back() += " = " + decl.source->text.substr(begin, decl.source->end(param.default_value->range) - begin);
				}
			}
		} else if (method && method->is_virtual) {
			owner = method->owner;
			for (const EngineParam &param : method->params) {
				add_param(param.name);
			}
		} else if (name != "_static_init") {
			continue;
		}
		const size_t error_count = errors.size();
		const size_t reference_count = references->size();
		check_signature(probe);
		const bool accepted = errors.size() == error_count;
		errors.erase(errors.begin() + error_count, errors.end());
		references->erase(references->begin() + reference_count, references->end());
		if (!accepted) {
			continue;
		}
		CompletionItem item;
		item.kind = CompletionKind::Function;
		item.name = name;
		item.insert = name + "(";
		for (size_t i = 0; i < params.size(); i++) {
			item.insert += (i > 0 ? ", " : "") + params[i];
		}
		item.insert += "):";
		const auto in_chain = std::find(chain.begin(), chain.end(), owner);
		item.place = in_chain == chain.end() ? CompletionItem::Place::Local : CompletionItem::Place::Member;
		item.depth = int(in_chain - chain.begin());
		found.emplace(name, item);
	}
	set_completions(found);
}

// A string at the cursor that names something by where it is (spec §10.4):
// a file by its path, as preload() and an engine method's String `path`
// take; an input action, as a StringName `action` names one, and the four
// of Input.get_vector() (core/input/input.cpp:246-248); a pub method or
// signal of this class, as `method` and `signal` name them in a call on
// self, the engine finding no other (§10.5). A file the editor lists
// loads, so each name goes in unquoted, which the editor quotes
// (scene/gui/code_edit.cpp:3916-3927; ed1daf0).
void Checker::complete_string(const EngineMethod *p_method, size_t p_index, bool p_on_self) {
	const EngineParam *param = p_method ? &p_method->params[p_index] : nullptr;
	const std::string name = param ? param->name : "path";
	const bool string_name = param && param->type.builtin_name() == "StringName";
	std::map<std::string, CompletionItem> found;
	const auto add = [&](const std::string &p_name, CompletionKind p_kind, CompletionItem::Place p_place) {
		CompletionItem item;
		item.kind = p_kind;
		item.name = p_name;
		item.insert = p_name;
		item.place = p_place;
		found.emplace(p_name, item);
	};
	if (name == "path" && (!param || param->type.kind == TypeKind::String) && options.project) {
		for (const std::string &file : options.project->resource_files()) {
			add(file, CompletionKind::FilePath, CompletionItem::Place::Project);
		}
	} else if (string_name && (name == "action" || (name.size() > 7 && name.compare(name.size() - 7, 7, "_action") == 0) || (p_method->owner == "Input" && p_method->name == "get_vector" && p_index < 4)) && options.project) {
		for (const std::string &action : options.project->input_actions()) {
			add(action, CompletionKind::PlainText, CompletionItem::Place::Project);
		}
	} else if (string_name && p_on_self && (name == "method" || name == "signal")) {
		if (name == "method") {
			for (const FuncDecl *function : cls.all_functions) {
				if (!function->of_struct && (function->is_pub || function->is_override)) {
					add(function->name, CompletionKind::Function, CompletionItem::Place::Member);
				}
			}
		} else {
			for (const SignalDecl *signal : cls.all_signals) {
				if (signal->is_pub) {
					add(signal->name, CompletionKind::Signal, CompletionItem::Place::Member);
				}
			}
		}
		// As the engine lists them (core/object/object.cpp:2396-2413).
		for (const std::string &member : engine.member_names(cls.native_base)) {
			const std::optional<EngineMethod> method = name == "method" ? engine.find_method(cls.native_base, member) : std::nullopt;
			if (method && (member[0] != '_' || method->is_virtual)) {
				add(member, CompletionKind::Function, CompletionItem::Place::Engine);
			} else if (name == "signal" && engine.find_signal(cls.native_base, member)) {
				add(member, CompletionKind::Signal, CompletionItem::Place::Engine);
			}
		}
	}
	set_completions(found);
}

} // namespace petrichor::compiler::checking
