#include "compiler/checker_internal.h"

namespace petrichor::compiler::checking {

/* Declarations */

void Checker::declare_members() {
	for (size_t i = 0; i < cls.members.size(); i++) {
		VarDecl &member = cls.members[i];
		if (member_index.count(member.name) || static_index.count(member.name) || function_index.count(member.name) || signal_index.count(member.name)) {
			error(member.name_token, "The member \"" + member.name + "\" is already declared.");
			continue;
		}
		member_index[member.name] = int(cls.all_members.size());
		cls.all_members.push_back(&member);
		refer(member.name_token, script_id(options.path, member.name), SymbolKind::Property);
	}
	for (VarDecl &variable : cls.static_vars) {
		if (member_index.count(variable.name) || static_index.count(variable.name) || function_index.count(variable.name) || signal_index.count(variable.name)) {
			error(variable.name_token, "The member \"" + variable.name + "\" is already declared.");
			continue;
		}
		static_index[variable.name] = int(cls.all_static_vars.size());
		cls.all_static_vars.push_back(&variable);
		refer(variable.name_token, script_id(options.path, variable.name), SymbolKind::Property);
	}
}

// Every function's name, before any type is resolved, which may need a
// class of this file that uses them (spec §4.1).
void Checker::declare_functions() {
	std::unordered_set<std::string> own_functions;
	for (FuncDecl &fn : cls.functions) {
		if (!own_functions.insert(fn.name).second || member_index.count(fn.name) || static_index.count(fn.name) || constant_index.count(fn.name) || signal_index.count(fn.name)) {
			error(fn.name_token, "\"" + fn.name + "\" is already declared.");
			continue;
		}
		const auto found = function_index.find(fn.name);
		if (found == function_index.end()) {
			function_index[fn.name] = int(cls.all_functions.size());
			cls.all_functions.push_back(&fn);
		} else {
			cls.all_functions[found->second] = &fn;
		}
	}
}

// Whether the function is the one its name was declared for.
bool Checker::is_declared(const FuncDecl &p_function) const {
	const auto found = function_index.find(p_function.name);
	return found != function_index.end() && cls.all_functions[found->second] == &p_function;
}

/* Annotations (spec §2.3) */

// A number of @export_range as GDScript writes it into the hint string,
// having converted it to float (modules/gdscript/gdscript_analyzer.cpp:1700-1722,
// core/variant/variant.cpp:1605-1606): 0.0, -5.0, 0.1.
std::string Checker::number_text(const ConstValue &p_value) {
	std::ostringstream stream;
	stream.imbue(std::locale::classic());
	stream << std::setprecision(15) << (p_value.kind == ConstValue::Kind::Int ? double(p_value.i) : p_value.f);
	std::string text = stream.str();
	if (text.find_first_of(".einf") == std::string::npos) {
		text += ".0";
	}
	return text;
}

// An argument of an annotation, which the compiler must know (spec §2.3,
// §4.5); null after reporting.
const ConstValue *Checker::annotation_argument(Expr &r_arg) {
	const bool was_in_constant = in_constant;
	in_constant = true;
	const Type type = check_value(r_arg);
	in_constant = was_in_constant;
	if (type.is_error()) {
		return nullptr;
	}
	if (!r_arg.constant) {
		require_constant(r_arg);
		return nullptr;
	}
	if (!r_arg.constant->is_known()) {
		error(r_arg.range, "An annotation argument must be known at compile time, and this one is computed when the class loads.");
		return nullptr;
	}
	return r_arg.constant.get();
}

const HintAnnotation *Checker::hint_annotation(const std::string &p_name) {
	for (const HintAnnotation &annotation : HINT_ANNOTATIONS) {
		if (p_name == annotation.name) {
			return &annotation;
		}
	}
	return nullptr;
}

bool Checker::fits(std::string_view p_target, const Type &p_type) {
	const std::string type = p_type.to_string();
	for (;;) {
		const size_t end = p_target.find(" and ");
		if (p_target.substr(0, end) == type) {
			return true;
		}
		if (end == std::string_view::npos) {
			return false;
		}
		p_target.remove_prefix(end + 5);
	}
}

// An annotation that gives the inspector a hint, its arguments joined by
// commas into the hint string, each a String, or for @export_range numbers
// first. On an Array[T] or a packed array it applies to the elements, and
// the hint string says so (modules/gdscript/gdscript_parser.cpp:4679-4739,
// 4752-4761, 4979-4989).
void Checker::check_hint(VarDecl &r_member, const Annotation &p_annotation, const HintAnnotation &p_hint) {
	const std::string name = "@" + p_annotation.name;
	if (!p_hint.target) {
		if (!p_annotation.args.empty()) {
			error(p_annotation.range, name + " takes no arguments.");
		}
		if (p_annotation.name == "export_storage") {
			r_member.usage = 2; // PROPERTY_USAGE_STORAGE
		}
		return;
	}
	const std::optional<Type> element = element_of(r_member.type);
	const bool of_elements = element && element->kind != TypeKind::Variant;
	const Type &target = of_elements ? *element : r_member.type;
	if (!fits(p_hint.target, target)) {
		error(p_annotation.range, name + " applies to " + p_hint.target + ", not " + r_member.type.to_string() + ".");
		return;
	}
	const size_t count = p_annotation.args.size();
	if (count < p_hint.least || count > p_hint.most) {
		if (p_hint.hint == 1) {
			error(p_annotation.range, "@export_range takes a minimum, a maximum and an optional step.");
		} else if (p_hint.least == 1 && p_hint.most == UINT32_MAX) {
			error(p_annotation.range, name + (p_hint.hint == 2 ? " takes the names of the values." : " takes at least one argument."));
		} else {
			error(p_annotation.range, name + (p_hint.most == 0 ? " takes no arguments." : " takes one argument."));
		}
		return;
	}
	std::string hint;
	size_t numbers = 0;
	for (size_t i = 0; i < count; i++) {
		const Expr &arg = *p_annotation.args[i];
		const ConstValue *value = annotation_argument(*p_annotation.args[i]);
		if (!value) {
			return;
		}
		const bool number = value->kind == ConstValue::Kind::Int || value->kind == ConstValue::Kind::Float;
		if (p_hint.hint == 1 && (number ? numbers < i : value->kind != ConstValue::Kind::String)) {
			error(arg.range, "@export_range takes numbers, then options such as \"or_greater\".");
			return;
		}
		if (p_hint.hint != 1 && value->kind != ConstValue::Kind::String) {
			error(arg.range, name + " takes strings.");
			return;
		}
		numbers += number ? 1 : 0;
		const std::string text = number ? number_text(*value) : value->text;
		if (p_hint.hint != 20 && (text.empty() || text.find(',') != std::string::npos)) {
			error(arg.range, text.empty() ? name + " takes no empty strings." : "An argument of " + name + " contains a comma; give the parts as separate arguments.");
			return;
		}
		if ((p_hint.hint == 6 && !check_flag(arg, text, i)) || (p_hint.hint == 26 && !check_node_class(arg, text))) {
			return;
		}
		hint += (i ? "," : "") + text;
	}
	if (p_hint.hint == 1 && (numbers < 2 || numbers > 3)) {
		error(p_annotation.range, "@export_range takes a minimum, a maximum and an optional step.");
		return;
	}
	r_member.hint = p_hint.hint;
	r_member.hint_string = hint;
	if (of_elements) {
		// PROPERTY_HINT_TYPE_STRING: "type/hint:hint string" of the elements.
		r_member.hint = 23;
		r_member.hint_string = std::to_string(uint32_t(bytecode::value_type_from_name(element->to_string()))) + "/" + std::to_string(p_hint.hint) + ":" + hint;
	}
}

// `"Name"` or `"Name:value"` of @export_flags, whose value is a bit set
// (modules/gdscript/gdscript_parser.cpp:4695-4719).
bool Checker::check_flag(const Expr &p_arg, const std::string &p_text, size_t p_index) {
	const size_t colon = p_text.find(':');
	if (colon == 0) {
		error(p_arg.range, "A flag of @export_flags needs a name.");
		return false;
	}
	if (colon == std::string::npos) {
		if (p_index >= 32) {
			error(p_arg.range, "From the 33rd flag on, @export_flags needs the value of each, as in \"Name:value\".");
			return false;
		}
		return true;
	}
	const std::string digits = p_text.substr(colon + 1);
	const bool number = !digits.empty() && digits.size() <= 10 && std::all_of(digits.begin(), digits.end(), [](char p_char) { return p_char >= '0' && p_char <= '9'; });
	if (!number || std::stoll(digits) < 1 || std::stoll(digits) >= (int64_t(1) << 32)) {
		error(p_arg.range, "The value of a flag of @export_flags is an integer from 1 to 2 ** 32 - 1.");
		return false;
	}
	return true;
}

// A class of @export_node_path, which must be a node
// (modules/gdscript/gdscript_parser.cpp:4720-4731).
bool Checker::check_node_class(const Expr &p_arg, const std::string &p_class) {
	const std::string path = options.project ? options.project->global_class_path(p_class) : std::string();
	std::string native = p_class;
	if (is_script_path(path)) {
		const ClassDecl *header = script_header(path);
		native = header ? script_class_type(*header, path).engine_class() : std::string();
	} else if (const ForeignClass *foreign = path.empty() ? nullptr : foreign_class(path)) {
		native = foreign->native;
	}
	if (!engine.class_exists(native) || !(native == "Node" || engine.inherits(native, "Node"))) {
		error(p_arg.range, "@export_node_path takes classes of nodes, and " + p_class + " is not one.");
		return false;
	}
	return true;
}

// @export_custom(hint, hint_string, usage), which GDScript passes to the
// engine as written (modules/gdscript/gdscript_parser.cpp:5019-5045).
void Checker::check_export_custom(VarDecl &r_member, const Annotation &p_annotation) {
	if (p_annotation.args.size() < 2 || p_annotation.args.size() > 3) {
		error(p_annotation.range, "@export_custom takes a hint, a hint string and optional usage flags.");
		return;
	}
	std::vector<const ConstValue *> values;
	for (const auto &arg : p_annotation.args) {
		values.push_back(annotation_argument(*arg));
		if (!values.back()) {
			return;
		}
	}
	const bool has_usage = values.size() == 3;
	if (values[0]->kind != ConstValue::Kind::Int || values[1]->kind != ConstValue::Kind::String || (has_usage && values[2]->kind != ConstValue::Kind::Int)) {
		error(p_annotation.range, "@export_custom takes a PropertyHint, a String and PropertyUsageFlags.");
		return;
	}
	r_member.hint = uint32_t(values[0]->i);
	r_member.hint_string = values[1]->text;
	if (has_usage) {
		r_member.usage = uint32_t(values[2]->i);
	}
}

// The inspector's view of an export (spec §2.3): @export alone shows it as
// its type, an annotation of the family adds a hint, @export_storage stores
// it without showing it. PropertyUsageFlags as the engine numbers them
// (core/object/property_info.h:90-91, 120). The editor and scenes set an
// export by name, so it must be pub (spec §10.5).
void Checker::check_annotations(VarDecl &r_member) {
	const Annotation *exported = nullptr;
	if (r_member.is_export) {
		r_member.usage = 6; // PROPERTY_USAGE_DEFAULT
	}
	for (const Annotation &annotation : r_member.annotations) {
		if (annotation.name.rfind("export", 0) != 0) {
			if (!annotation.args.empty()) {
				error(annotation.range, "@" + annotation.name + " takes no arguments.");
			}
			continue;
		}
		if (exported) {
			error(annotation.range, "@" + annotation.name + " cannot be used with another @export annotation.");
			continue;
		}
		exported = &annotation;
		if (!r_member.is_pub) {
			error(annotation.range, "An exported variable must be pub: the editor and scenes set it by name.");
		}
		if (const HintAnnotation *hint = hint_annotation(annotation.name)) {
			check_hint(r_member, annotation, *hint);
		} else if (annotation.name == "export_custom") {
			check_export_custom(r_member, annotation);
		} else if (annotation.name == "export_tool_button") {
			error(annotation.range, "@export_tool_button needs @tool, which is not in the first version.");
		} else {
			error(annotation.range, "The annotation @" + annotation.name + " is not supported yet.");
		}
	}
}

// @rpc(mode, sync, transfer_mode, channel): the strings in any order, the
// channel fourth, as in GDScript (modules/gdscript/gdscript_parser.cpp:187,
// 5238-5298). The peers call the method by name, so it must be pub (spec
// §10.5).
void Checker::check_rpc(FuncDecl &r_function, const Annotation &p_annotation) {
	if (r_function.rpc.mode != 0) {
		error(p_annotation.range, "A function takes @rpc once.");
		return;
	}
	if (!r_function.is_pub) {
		error(p_annotation.range, "An @rpc function must be pub: other peers call it by name.");
	}
	if (p_annotation.args.size() > 4) {
		error(p_annotation.range, "@rpc takes at most four arguments.");
		return;
	}
	bytecode::Rpc rpc;
	rpc.mode = 2; // RPC_MODE_AUTHORITY
	int seen[3] = {};
	for (size_t i = 0; i < p_annotation.args.size(); i++) {
		const Expr &arg = *p_annotation.args[i];
		const ConstValue *value = annotation_argument(*p_annotation.args[i]);
		if (!value) {
			return;
		}
		if (i == 3) {
			if (value->kind != ConstValue::Kind::Int || value->i < 0 || value->i > INT32_MAX) {
				error(arg.range, "The fourth argument of @rpc is the channel, an int that is not negative.");
				return;
			}
			rpc.channel = int32_t(value->i);
			continue;
		}
		static const struct {
			const char *name;
			int group;
			uint8_t val;
		} options[] = {
			{ "call_remote", 0, 0 },
			{ "call_local", 0, 1 },
			{ "any_peer", 1, 1 },
			{ "authority", 1, 2 },
			{ "unreliable", 2, 0 },
			{ "unreliable_ordered", 2, 1 },
			{ "reliable", 2, 2 },
		};
		const std::string text = value->kind == ConstValue::Kind::String ? value->text : std::string();
		const auto it = std::find_if(std::begin(options), std::end(options), [&](const auto &p_opt) { return text == p_opt.name; });
		if (it == std::end(options)) {
			error(arg.range, "@rpc takes \"call_local\" or \"call_remote\", \"any_peer\" or \"authority\", and \"reliable\", \"unreliable\" or \"unreliable_ordered\", then the channel.");
			return;
		}
		seen[it->group]++;
		if (it->group == 0) {
			rpc.call_local = it->val != 0;
		} else if (it->group == 1) {
			rpc.mode = it->val;
		} else {
			rpc.transfer_mode = it->val;
		}
	}
	static const char *const duplicate_messages[] = {
		"@rpc takes one of \"call_local\" and \"call_remote\".",
		"@rpc takes one of \"any_peer\" and \"authority\".",
		"@rpc takes one transfer mode.",
	};
	for (int g = 0; g < 3; g++) {
		if (seen[g] > 1) {
			error(p_annotation.range, duplicate_messages[g]);
			return;
		}
	}
	r_function.rpc = rpc;
}

// Their arguments name warnings, as strings; which names suppress anything
// is for the lints to say (spec §13.2).
void Checker::check_warning_ignores() {
	for (const Annotation &annotation : cls.warning_ignores) {
		if (annotation.args.empty()) {
			error(annotation.range, "@" + annotation.name + " takes the names of warnings.");
		}
		for (const auto &arg : annotation.args) {
			const ConstValue *value = annotation_argument(*arg);
			if (value && value->kind != ConstValue::Kind::String) {
				error(arg->range, "@" + annotation.name + " takes the names of warnings, as strings.");
			}
		}
	}
}

// `@export_group(name, prefix)`, `@export_subgroup(name, prefix)`, `@export_category(name)`.
void Checker::check_groups() {
	for (ExportGroupDecl &group : cls.groups) {
		const Annotation &annotation = group.annotation;
		const size_t most = annotation.name == "export_category" ? 1 : 2;
		bool ok = !annotation.args.empty() && annotation.args.size() <= most;
		std::vector<std::string> texts;
		for (const auto &arg : annotation.args) {
			const ConstValue *value = annotation_argument(*arg);
			if (!value) {
				ok = false;
				break;
			}
			ok = ok && value->kind == ConstValue::Kind::String;
			texts.push_back(value->text);
		}
		if (!ok) {
			if (texts.size() == annotation.args.size()) {
				error(annotation.range, "@" + annotation.name + " takes a name" + (most == 2 ? " and an optional prefix" : "") + ", as strings.");
			}
			continue;
		}
		group.name = texts[0];
		group.prefix = texts.size() > 1 ? texts[1] : std::string();
	}
}

/* Members (spec §4.3) */

// After the signatures, since an initializer can call this class's functions.
// The other members first, then the @onready ones, whose initializers may
// read any of the others (spec §6.3), so that a member's type is known
// before an initializer reads it.
void Checker::check_members() {
	cls.is_node = engine.inherits(cls.native_base, "Node");
	// First, since instance initializers may read them; the types written
	// before any initializer, whose lambdas may read later ones.
	for (VarDecl &variable : cls.static_vars) {
		if (variable.declared_type) {
			variable.type = resolve(*variable.declared_type, false);
		}
	}
	for (size_t i = 0; i < cls.static_vars.size(); i++) {
		check_static_declaration(i);
	}
	for (const bool onready : { false, true }) {
		for (size_t i = 0; i < cls.members.size(); i++) {
			if (cls.members[i].is_onready == onready) {
				check_member_declaration(i);
			}
		}
	}
}

void Checker::check_member_declaration(size_t p_index) {
	// A name declared twice is reported where it is registered.
	VarDecl &member = cls.members[p_index];
	const auto registered = member_index.find(member.name);
	if (registered == member_index.end() || cls.all_members[registered->second] != &member) {
		return;
	}
	Type type = Type::of(TypeKind::Error);
	if (member.declared_type) {
		type = resolve(*member.declared_type, false);
	}
	if (member.is_onready) {
		if (!cls.is_node) {
			error(member.name_token, "@onready is for Node scripts; a " + cls.extends + " is never ready.");
		}
		if (member.is_export) {
			error(member.name_token, "@onready and @export do not go together: the node's ready would overwrite what the scene assigned.");
		}
		if (!member.initializer) {
			error(member.name_token, "An @onready member needs an initializer.");
		}
	}
	if (member.initializer) {
		initializing = registered->second;
		initializing_onready = member.is_onready;
		const Type value = check_value(*member.initializer, type);
		initializing = -1;
		initializing_onready = false;
		if (member.declared_type) {
			expect_assignable(*member.initializer, value, type, "assign");
		} else {
			type = value;
			expect_scene_fits(*member.initializer, type);
		}
		// An @onready member gets even a constant when the node is ready.
		if (!member.is_onready) {
			member.initial = reflected_initial(member.initializer.get(), type);
		}
	} else if (!member.declared_type) {
		error(member.name_token, "\"" + member.name + "\" needs a type or an initial value.");
	} else {
		// The scene, the creator or an ancestor assigns it, which is checked
		// before it is read (spec §4.3, §6.2).
		member.must_assign = !has_value_without_initializer(type);
		member.initial = member.must_assign ? nullptr : reflected_initial(nullptr, type);
	}
	const TypeKind kind = type.unwrapped().kind;
	if (member.is_export && is_typed_container(type.unwrapped()) && is_boxed(type.unwrapped())) {
		error(member.name_token, "Containers whose elements exist only in this language, such as " + type.to_string() + ", cannot be exported: exports go through reflection, which does not list them.");
	} else if (member.is_export && (kind == TypeKind::Struct || kind == TypeKind::PayloadEnum)) {
		std::unordered_set<std::string> seen;
		const std::string field = node_field(type.unwrapped(), std::string(), seen);
		if (!field.empty()) {
			error(member.name_token, field + " holds a node, which a scene does not store inside an exported value; export the node as a member of its own.");
		}
	}
	if (member.is_export && type.kind == TypeKind::Object && type.name == "PackedScene" && type.args.empty()) {
		error(member.name_token, "An exported scene is written PackedScene[T], with the type of its root.");
	}
	if (member.is_export && type.kind == TypeKind::Task) {
		error(member.name_token, "Task handles cannot be exported: a scene cannot save a running coroutine.");
	}
	member.type = type;
	check_annotations(member);
	// The inspector's Dictionary editor reads any hint string as the types
	// of the keys and the values; "0:" keeps the keys untyped, and names the
	// type after it (architecture §6.2).
	if (member.is_export && member.hint == 0 && member.hint_string.empty() && (kind == TypeKind::Struct || kind == TypeKind::PayloadEnum)) {
		member.hint_string = "0:" + type.to_string();
	}
	check_accessors(member);
}

// A member's first value as reflection shows it, when the compiler knows
// it: the editor's default (spec §10.1). A constant, or a struct or a
// variant built of constants and the defaults of the fields left out;
// without an initializer, a struct's defaults. Null where code computes
// it, or a part of it loads a resource.
ConstPtr Checker::reflected_initial(const Expr *p_value, const Type &p_type) const {
	if (!p_value) {
		return p_type.kind == TypeKind::Struct ? encoded_fields(p_type, nullptr) : nullptr;
	}
	if (p_value->constant) {
		const ConstPtr value = converted(p_value->constant, p_type);
		return value && !loads(*value) ? encoded(value, p_type) : nullptr;
	}
	const bool constructed = p_value->target == Target::StructConstructor || p_value->target == Target::VariantConstructor;
	return constructed ? encoded_fields(p_value->type, p_value) : nullptr;
}

// The struct, or the variant of the enum with payloads, that `p_call`
// constructs, as a Dictionary by field name (spec §10.1); without a call,
// the struct's defaults.
ConstPtr Checker::encoded_fields(const Type &p_type, const Expr *p_call) const {
	auto encoded = std::make_shared<ConstValue>();
	encoded->kind = ConstValue::Kind::Dictionary;
	encoded->type = Type::builtin("Dictionary");
	const auto add = [&](const std::string &p_key, const ConstPtr &p_value) {
		auto key = std::make_shared<ConstValue>();
		key->kind = ConstValue::Kind::String;
		key->type = Type::of(TypeKind::String);
		key->text = p_key;
		encoded->items.push_back(key);
		encoded->items.push_back(p_value);
	};
	std::vector<std::pair<std::string, Type>> fields;
	std::vector<const Expr *> values;
	if (const StructDecl *decl = p_type.kind == TypeKind::Struct ? struct_decl(p_type) : nullptr) {
		for (const VarDecl &field : decl->fields) {
			fields.emplace_back(field.name, field.type);
			values.push_back(field.initializer.get());
		}
	} else if (p_type.kind == TypeKind::PayloadEnum && p_call && p_type.values) {
		auto name = std::make_shared<ConstValue>();
		name->kind = ConstValue::Kind::String;
		name->type = Type::of(TypeKind::String);
		name->text = p_type.values->list[p_call->index].first;
		add("$variant", name);
		fields = p_type.values->fields[p_call->index];
		values.resize(fields.size());
	} else {
		return nullptr;
	}
	for (size_t i = 0; p_call && i < p_call->args.size(); i++) {
		// By name, or Ok(v) and Err(e) by position.
		for (size_t f = 0; f < fields.size(); f++) {
			if (p_call->arg_names.empty() ? f == i : fields[f].first == p_call->arg_names[i]) {
				values[f] = p_call->args[i].get();
			}
		}
	}
	for (size_t f = 0; f < fields.size(); f++) {
		const ConstPtr value = values[f] ? reflected_initial(values[f], fields[f].second) : nullptr;
		if (!value) {
			return nullptr;
		}
		add(fields[f].first, value);
	}
	return encoded;
}

// A constant as the engine sees a value of the type (spec §10.1): a
// variant of an enum with payloads as a Dictionary, and a container that
// keeps its elements boxed untyped, with them encoded.
ConstPtr Checker::encoded(const ConstPtr &p_value, const Type &p_type) {
	const Type &type = p_type.kind == TypeKind::Option ? p_type.args[0] : p_type;
	if (type.kind == TypeKind::PayloadEnum && p_value->kind == ConstValue::Kind::Int && type.values) {
		auto value = std::make_shared<ConstValue>();
		value->kind = ConstValue::Kind::Dictionary;
		value->type = Type::builtin("Dictionary");
		auto key = std::make_shared<ConstValue>();
		key->kind = ConstValue::Kind::String;
		key->type = Type::of(TypeKind::String);
		key->text = "$variant";
		auto name = std::make_shared<ConstValue>(*key);
		name->text = type.values->list[size_t(p_value->i)].first;
		value->items = { key, name };
		return value;
	}
	if (!p_value->is_container() || !is_boxed(type)) {
		return p_value;
	}
	auto value = std::make_shared<ConstValue>(*p_value);
	value->type = Type::builtin(type.builtin_name());
	const std::optional<Type> element = element_of(type);
	const std::optional<std::pair<Type, Type>> entries = entries_of(type);
	for (size_t i = 0; i < value->items.size(); i++) {
		const bool key = value->kind == ConstValue::Kind::Dictionary && i % 2 == 0;
		const Type item = value->kind == ConstValue::Kind::Array ? *element : (key ? entries->first : entries->second);
		value->items[i] = encoded(value->items[i], item);
	}
	return value;
}

// The field of an exported struct or enum with payloads that holds a
// node at any depth, such as "Hit.source"; "" for none. The engine stores
// as paths only the nodes of an exported member and of the typed
// container in it (architecture §6.2).
std::string Checker::node_field(const Type &p_type, const std::string &p_field, std::unordered_set<std::string> &r_seen) const {
	if (p_type.kind == TypeKind::Object) {
		const std::string &native = p_type.engine_class();
		return native == "Node" || engine.inherits(native, "Node") ? p_field : std::string();
	}
	// The type inside T?, or the keys and elements of a container.
	std::vector<std::pair<std::string, Type>> parts;
	const bool held = p_type.kind == TypeKind::Struct || p_type.kind == TypeKind::PayloadEnum;
	for (size_t i = 0; (p_type.kind == TypeKind::Option || p_type.kind == TypeKind::Builtin) && i < p_type.args.size(); i++) {
		parts.emplace_back(p_field, p_type.args[i]);
	}
	if (held && r_seen.insert(p_type.script + "::" + p_type.to_string()).second) {
		const std::string name = p_type.interface_name() + ".";
		if (const StructDecl *decl = p_type.kind == TypeKind::Struct ? struct_decl(p_type) : nullptr) {
			for (const VarDecl &field : decl->fields) {
				parts.emplace_back(name + field.name, field.type);
			}
		}
		for (size_t v = 0; p_type.kind == TypeKind::PayloadEnum && p_type.values && v < p_type.values->fields.size(); v++) {
			for (const auto &[field, type] : p_type.values->fields[v]) {
				parts.emplace_back(name + p_type.values->list[v].first + "." + field, type);
			}
		}
	}
	for (const auto &[field, type] : parts) {
		const std::string found = node_field(type, field, r_seen);
		if (!found.empty()) {
			return found;
		}
	}
	return std::string();
}

// A static variable (spec §4.8): typed as a member is (§4.3), but one of
// a type without a default value needs an initializer. Without the bodies,
// one whose type is written needs no initializer checked, which may read
// the class being compiled.
void Checker::check_static_declaration(size_t p_index) {
	VarDecl &variable = cls.static_vars[p_index];
	const auto registered = static_index.find(variable.name);
	if (registered == static_index.end() || cls.all_static_vars[registered->second] != &variable) {
		return;
	}
	Type type = variable.declared_type ? variable.type : Type::of(TypeKind::Error);
	if (variable.initializer && !(options.declarations_only && variable.declared_type)) {
		static_initializing = int(p_index);
		const Type value = check_value(*variable.initializer, type);
		static_initializing = -1;
		if (variable.declared_type) {
			expect_assignable(*variable.initializer, value, type, "assign");
		} else {
			type = value;
			expect_scene_fits(*variable.initializer, type);
		}
	} else if (!variable.declared_type) {
		error(variable.name_token, "\"" + variable.name + "\" needs a type or an initial value.");
	} else if (!variable.initializer && !enum_has_default(type)) {
		error(variable.name_token, "\"" + variable.name + "\" needs an initial value: " + enum_spelling(type) + " has no value 0 to start at.");
	} else if (!variable.initializer && !has_value_without_initializer(type)) {
		error(variable.name_token, "The static variable \"" + variable.name + "\" needs an initial value: " + type.to_string() + " has no default value. Give it one, or the type " + optional_of(type).to_string() + ".");
	}
	variable.type = type;
	check_accessors(variable);
}

// A property's setter and getter (spec §4.4): its own inline ones, which
// take and give its type, or functions of the class that do.
void Checker::check_accessors(VarDecl &r_member) {
	const auto accessor = [&](const std::string &p_name, bool p_setter) {
		if (p_name.empty()) {
			return -1;
		}
		const std::string kind = p_setter ? "setter" : "getter";
		FuncDecl *function = nullptr;
		for (FuncDecl &own : cls.functions) {
			if (own.name == p_name) {
				function = &own;
			}
		}
		if (!function) {
			error(r_member.accessor_token, "The " + kind + " " + p_name + "() is not a function of this class.");
			return -1;
		}
		if (p_name[0] == '@') {
			if (p_setter) {
				function->params[0].type = r_member.type;
			} else {
				function->return_type = r_member.type;
			}
		} else {
			if (!function->accessor_of.empty()) {
				error(r_member.accessor_token, p_name + "() is already an accessor of \"" + function->accessor_of + "\".");
			}
			function->accessor_of = r_member.name;
			refer(p_setter ? r_member.setter_token : r_member.getter_token, script_id(options.path, p_name));
			const bool fits = p_setter ? function->params.size() == 1 && !function->has_rest() && function->params[0].type == r_member.type && function->return_type.kind == TypeKind::Void : function->params.empty() && function->return_type == r_member.type;
			if (!fits && !r_member.type.is_error()) {
				error(function->name_token, "As the " + kind + " of \"" + r_member.name + "\", " + p_name + "() " + (p_setter ? "takes one argument of type " + r_member.type.to_string() + " and returns nothing" : "takes no argument and returns " + r_member.type.to_string()) + ".");
			}
		}
		if (r_member.is_static && (function->is_async || !function->is_static)) {
			error(function->name_token, "A " + kind + " of a static variable is static, and not async: " + (p_setter ? "writing" : "reading") + " the variable does not wait for it.");
		} else if (!r_member.is_static && (function->is_async || function->is_static)) {
			error(function->name_token, "A " + kind + " runs on the object, so it is not static, and not async: " + (p_setter ? "writing" : "reading") + " the member does not wait for it.");
		}
		const auto found = function_index.find(p_name);
		return found == function_index.end() ? -1 : found->second;
	};
	r_member.setter_index = accessor(r_member.setter, true);
	r_member.getter_index = accessor(r_member.getter, false);
}

/* Functions (spec §4.6) */

// The values themselves are checked with the body (declare_params()).
void Checker::check_defaults(const FuncDecl &p_function) {
	bool any = false;
	for (const ParamDecl &param : p_function.params) {
		any = any || param.default_value;
		if (any && !param.default_value && !param.is_rest) {
			error(param.name_token, "Parameter \"" + param.name + "\" follows one with a default value and needs one too.");
		}
	}
}

int Checker::required_params(const FuncDecl &p_function) {
	int count = 0;
	while (count < int(p_function.params.size()) && !p_function.params[count].default_value && !p_function.params[count].is_rest) {
		count++;
	}
	return count;
}

// What a rest parameter of type Array or Array[T] takes as each argument
// (spec §4.6).
Type Checker::rest_element(const Type &p_rest) {
	if (!p_rest.args.empty()) {
		return p_rest.args[0];
	}
	return p_rest.is_error() ? p_rest : Type::of(TypeKind::Variant);
}

// What the function takes as argument `p_index`, nullopt when it takes none.
std::optional<Type> Checker::argument_type(const FuncDecl &p_function, size_t p_index) {
	const size_t fixed = p_function.params.size() - (p_function.has_rest() ? 1 : 0);
	if (p_index < fixed) {
		return p_function.params[p_index].type;
	}
	return p_function.has_rest() ? std::optional<Type>(rest_element(p_function.params.back().type)) : std::nullopt;
}

// A parameter whose type nothing gives (spec §4.6).
void Checker::untyped(ParamDecl &r_param, const std::string &p_why) {
	error(r_param.name_token, r_param.is_rest ? "The rest parameter \"" + r_param.name + "\" needs a type: Array, or Array[T] for arguments of type T." : "Parameter \"" + r_param.name + "\" needs a type" + p_why + ".");
	r_param.type = Type::of(TypeKind::Error);
}

void Checker::check_rest(const FuncDecl &p_function) {
	const ParamDecl *rest = p_function.has_rest() ? &p_function.params.back() : nullptr;
	if (rest && !rest->type.is_error() && rest->type.builtin_name() != "Array") {
		error(rest->declared_type ? rest->declared_type->range : rest->range, "The rest parameter takes the arguments as an Array or an Array[T], not as " + rest->type.to_string() + ".");
	}
}

// A static function has no instance (spec §4.8).
bool Checker::refuse_in_static(const Expr &p_expr, const std::string &p_name) {
	if (structure) {
		error(p_expr.range, "\"" + p_name + "\" belongs to an object of " + display_name(cls, options.path) + ", which " + structure->name + " does not have: a struct's methods have no object of the class around it.");
		return true;
	}
	if (function ? !function->is_static : static_initializing < 0) {
		// A lambda that uses self holds it, and so do those it is in (spec §3.11).
		for (size_t i = 0; function && function->is_lambda && i <= enclosing.size(); i++) {
			FuncDecl *lambda = i < enclosing.size() ? enclosing[i].function : function;
			if (lambda && lambda->is_lambda) {
				lambda->uses_self = true;
			}
		}
		return false;
	}
	const FuncDecl *outer = outermost();
	error(p_expr.range, "\"" + p_name + "\" belongs to an instance, which " + (outer ? "the static function \"" + outer->name + "()\"" : std::string("a static variable's initializer")) + " does not have.");
	return true;
}

// The function being checked, as messages name it.
std::string Checker::function_label() const {
	if (!function->is_lambda) {
		return "\"" + function->name + "()\"";
	}
	return function->name.empty() ? std::string("The lambda") : "The lambda \"" + function->name + "\"";
}

const FuncDecl *Checker::base_function(const std::string &p_name) const {
	if (cls.base) {
		for (const FuncDecl *fn : cls.base->all_functions) {
			if (fn->name == p_name) {
				return fn;
			}
		}
	}
	return nullptr;
}

bool Checker::override_assignable(const Type &p_from, const Type &p_to) const {
	if (p_from == p_to || p_from.is_error() || p_to.is_error()) {
		return true;
	}
	if (p_to.kind == TypeKind::Option) {
		return p_from.kind == TypeKind::Option ? p_from.args == p_to.args : override_assignable(p_from, p_to.args[0]);
	}
	if (p_from.kind == TypeKind::Object && p_to.kind == TypeKind::Object) {
		return assignable(p_from, p_to);
	}
	if (p_from.kind == TypeKind::Function && p_to.kind == TypeKind::Function) {
		return assignable(p_from, p_to);
	}
	if (p_from.kind == TypeKind::Task && p_to.kind == TypeKind::Task) {
		return assignable(p_from, p_to);
	}
	return p_from.kind == TypeKind::Builtin && p_to.kind == TypeKind::Builtin && p_from.name == "Array" && p_to.name == "Array" && p_to.args.empty();
}

void Checker::check_script_override(FuncDecl &r_fn, const FuncDecl &p_base) {
	r_fn.is_override = p_base.is_override;
	if (r_fn.is_pub != p_base.is_pub) {
		error(r_fn.name_token, "An override must preserve its base method's visibility.");
	}
	if (r_fn.is_static != p_base.is_static || r_fn.is_async != p_base.is_async) {
		error(r_fn.name_token, "An override must preserve static and async.");
	}
	const size_t base_fixed = p_base.params.size() - (p_base.has_rest() ? 1 : 0);
	for (size_t i = 0; i < r_fn.params.size(); ++i) {
		ParamDecl &param = r_fn.params[i];
		const std::optional<Type> base = argument_type(p_base, i);
		if (param.declared_type) {
			param.type = resolve(*param.declared_type, false);
		} else if (param.is_rest ? p_base.has_rest() : base.has_value()) {
			param.type = param.is_rest ? p_base.params.back().type : *base;
		} else {
			untyped(param);
		}
		// A rest parameter takes each argument of the base from here on, which
		// it holds as an Array of it.
		bool narrows = !param.is_rest && base && !override_assignable(*base, param.type);
		for (size_t j = i; param.is_rest && j < base_fixed; j++) {
			narrows = narrows || !override_assignable(Type::array(p_base.params[j].type), param.type);
		}
		if (param.is_rest && p_base.has_rest()) {
			narrows = narrows || !override_assignable(p_base.params.back().type, param.type);
		}
		if (narrows) {
			error(param.name_token, "The override narrows its base parameter type.");
		}
	}
	check_rest(r_fn);
	check_defaults(r_fn);
	if (p_base.has_rest() && !r_fn.has_rest()) {
		error(r_fn.name_token, "The override must keep the rest parameter of its base method.");
	} else if (!takes(r_fn, base_fixed) || required_params(r_fn) > required_params(p_base)) {
		error(r_fn.name_token, "The override must accept every argument count of its base method.");
	}
	r_fn.return_type = r_fn.return_type_ref ? resolve(*r_fn.return_type_ref, true) : p_base.return_type;
	if (!override_assignable(r_fn.return_type, p_base.return_type)) {
		error(r_fn.name_token, "The override widens its base return type.");
	}
}

// Its declaration, with what it overrides; an inline accessor has no name
// of its own.
void Checker::declare_function(const FuncDecl &p_function, const std::optional<DeclId> &p_overrides) {
	if (p_function.name.rfind('@', 0) != 0) {
		refer(p_function.name_token, script_id(options.path, p_function.name), SymbolKind::Method, p_overrides);
	}
}

// @abstract as GDScript checks it (modules/gdscript/gdscript_analyzer.cpp:1532-1556,
// 2008-2022; modules/gdscript/gdscript_parser.cpp:4513-4516).
void Checker::check_abstract(const FuncDecl &p_function) {
	if (p_function.is_abstract && p_function.is_static) {
		error(p_function.name_token, "A static function cannot be @abstract.");
	} else if (p_function.is_abstract && p_function.has_body) {
		error(p_function.name_token, "An abstract function has no body; a class that extends this one gives it one.");
	} else if (p_function.is_abstract && !cls.is_abstract) {
		error(p_function.name_token, display_name(cls, options.path) + " is not abstract but declares the abstract \"" + p_function.name + "()\"; mark the class @abstract.");
	}
}

// A class that is not abstract implements every abstract method it inherits.
void Checker::check_implemented() {
	if (cls.is_abstract || !cls.base) {
		return;
	}
	for (const FuncDecl *function : cls.all_functions) {
		if (function->is_abstract && std::none_of(cls.functions.begin(), cls.functions.end(), [&](const FuncDecl &p_own) { return &p_own == function; })) {
			const ClassDecl &owner = declaring(*cls.base, *function, &ClassDecl::functions);
			error(cls.extends_token, display_name(cls, options.path) + " must implement \"" + display_name(owner, owner.path) + "." + function->name + "()\", and the other abstract methods it inherits, or be marked @abstract.");
			return;
		}
	}
}

// The class names the constants of the engine class it extends bare, so
// it declares nothing else by their names, as in GDScript
// (modules/gdscript/gdscript_analyzer.cpp:255-276; spec §4.1).
void Checker::check_engine_constant_names() {
	const auto check = [&](const std::string &p_name, uint32_t p_token) {
		if (!p_name.empty() && engine.find_constant(cls.native_base, p_name)) {
			error(p_token, "\"" + p_name + "\" is already a constant of " + cls.native_base + ", which this class extends.");
		}
	};
	for (const VarDecl &member : cls.members) {
		check(member.name, member.name_token);
	}
	for (const VarDecl &variable : cls.static_vars) {
		check(variable.name, variable.name_token);
	}
	for (const ConstDecl &constant : cls.constants) {
		check(constant.name, constant.name_token);
	}
	for (const SignalDecl &signal : cls.signals) {
		check(signal.name, signal.name_token);
	}
	for (const EnumDecl &decl : cls.enums) {
		check(decl.name, decl.name_token);
		for (const EnumValueDecl &value : decl.values) {
			if (decl.name.empty()) {
				check(value.name, value.name_token);
			}
		}
	}
	for (const std::unique_ptr<ClassDecl> &inner : cls.classes) {
		check(inner->name, inner->name_token);
	}
}

// new() constructs the class (spec §4.1).
void Checker::refuse_new(const FuncDecl &p_function) {
	if (p_function.name == "new") {
		error(p_function.name_token, "A function cannot be named \"new\": new() constructs the class.");
	}
}

void Checker::check_signature(FuncDecl &r_function) {
	if (!r_function.of_struct) {
		refuse_new(r_function);
	}
	check_abstract(r_function);
	// Each class's _init() has its own signature (spec §6.1).
	if (const FuncDecl *parent = r_function.name == "_init" ? nullptr : base_function(r_function.name)) {
		declare_function(r_function, script_id(declaring(*cls.base, *parent, &ClassDecl::functions).path, parent->name));
		check_script_override(r_function, *parent);
		return;
	}
	// Object binds _init() as a virtual (core/object/object.cpp:1928; ed1daf0),
	// but it is no override: the engine does not call it by name.
	const std::optional<EngineMethod> engine_method = r_function.name == "_init" ? std::nullopt : engine.find_method(cls.native_base, r_function.name);
	declare_function(r_function, engine_method && engine_method->is_virtual ? std::optional<DeclId>(engine_id(engine_method->owner, r_function.name)) : std::nullopt);
	if (engine_method && !engine_method->is_virtual) {
		error(r_function.name_token, "\"" + r_function.name + "()\" is a method of " + cls.extends + " that cannot be overridden.");
	}
	r_function.is_override = engine_method && engine_method->is_virtual;
	if (r_function.is_async) {
		if (r_function.name == "_init") {
			error(r_function.name_token, "_init() cannot be async: construction does not wait for it.");
		} else if (r_function.is_override && !engine_method->return_type.is_error() && engine_method->return_type.kind != TypeKind::Void) {
			error(r_function.name_token, "An engine virtual method returning " + engine_method->return_type.to_string() + " cannot be async: the engine takes the result at once, without waiting.");
		}
	}
	// It takes as many arguments as the engine passes (spec §4.6).
	if (r_function.is_override && !takes(r_function, engine_method->params.size())) {
		error(r_function.name_token, "\"" + r_function.name + "()\" overrides an engine method with " + std::to_string(engine_method->params.size()) + " parameter(s).");
	}
	for (size_t i = 0; i < r_function.params.size(); i++) {
		ParamDecl &param = r_function.params[i];
		// What the engine passes in it: an argument, or each from here on.
		std::vector<Type> engine_types;
		for (size_t j = i; r_function.is_override && j < engine_method->params.size() && (j == i || param.is_rest); j++) {
			Type engine_type = engine_method->params[j].type;
			// The engine passes its own objects to virtual methods.
			engine_type.nullable = false;
			if (!engine_type.is_storable()) {
				error(param.name_token, "Parameters of type " + engine_type.to_string() + " are not supported yet.");
				engine_type = Type::of(TypeKind::Error);
			}
			engine_types.push_back(engine_type);
		}
		if (param.declared_type) {
			param.type = resolve(*param.declared_type, false);
			// A parameter may accept more than the overridden one does, such as
			// T? for T (spec §4.6).
			const Type taken = param.is_rest ? rest_element(param.type) : param.type;
			for (const Type &engine_type : engine_types) {
				if (!engine_type.is_error() && !taken.is_error() && !assignable(engine_type, taken)) {
					error(param.declared_type->range, param.is_rest ? "The overridden method passes " + engine_type.to_string() + " in this rest parameter." : "The overridden method declares this parameter as " + engine_type.to_string() + ".");
				}
			}
		} else if (!param.is_rest && !engine_types.empty()) {
			// The type comes from the overridden declaration (spec §4.6).
			param.type = engine_types[0];
		} else if (!r_function.accessor_of.empty()) {
			// An inline setter takes the member's type (check_accessors()).
		} else {
			untyped(param);
		}
	}
	check_rest(r_function);
	check_defaults(r_function);
	if (r_function.is_static && r_function.is_override) {
		error(r_function.name_token, "\"" + r_function.name + "()\" overrides an engine method and cannot be static.");
	}

	if (r_function.name == "_init" && r_function.return_type_ref && r_function.return_type_ref->name != "void") {
		error(r_function.return_type_ref->range, "_init() returns nothing.");
	}
	// Called once when the class loads, as GDScript's
	// (modules/gdscript/gdscript_parser.cpp:1751-1757; spec §4.8).
	if (r_function.name == "_static_init" && (!r_function.is_static || !r_function.params.empty() || r_function.is_async || (r_function.return_type_ref && r_function.return_type_ref->name != "void"))) {
		error(r_function.name_token, "_static_init() is static, takes no parameters, returns nothing and cannot await.");
	}

	if (r_function.return_type_ref) {
		r_function.return_type = resolve(*r_function.return_type_ref, true);
		// Covariant (spec §4.6): the engine takes any value where it declares
		// a Variant, as for _iter_get() (§5.7).
		const Type &overridden = r_function.is_override ? engine_method->return_type : Type::of(TypeKind::Error);
		if (!override_assignable(r_function.return_type, overridden) && (overridden.kind != TypeKind::Variant || r_function.return_type.kind == TypeKind::Void)) {
			error(r_function.return_type_ref->range, "The overridden method returns " + engine_method->return_type.to_string() + ".");
		}
	} else if (r_function.is_override) {
		r_function.return_type = engine_method->return_type;
		if (r_function.return_type.kind != TypeKind::Void && !r_function.return_type.is_storable()) {
			error(r_function.name_token, "Return values of type " + r_function.return_type.to_string() + " are not supported yet.");
			r_function.return_type = Type::of(TypeKind::Error);
		}
	} else {
		r_function.return_type = Type::of(TypeKind::Void);
	}
}

// A function's default values see self and the parameters before them
// (spec §4.6).
void Checker::declare_params(FuncDecl &r_function, bool p_defaults) {
	for (const ParamDecl &param : r_function.params) {
		// What a syntax error left is reported.
		if (p_defaults && param.default_value && param.default_value->kind != ExprKind::Error) {
			expect_assignable(*param.default_value, check_value(*param.default_value, param.type), param.type, "use");
		}
		if (scopes.back().count(param.name)) {
			error(param.name_token, "Parameter \"" + param.name + "\" is declared twice.");
		}
		scopes.back()[param.name] = int(r_function.locals.size());
		r_function.locals.push_back({ param.name, param.type, param.name_token });
		refer_local(param.name_token, param.type);
	}
}

void Checker::check_body(FuncDecl &r_function) {
	function = &r_function;
	scopes.clear();
	scopes.emplace_back();
	narrowed.clear();
	narrowed.emplace_back();
	if (r_function.of_struct && !r_function.is_static) {
		scopes.back()["self"] = 0;
		r_function.locals.push_back({ "self", structure_type, r_function.name_token });
	}
	declare_params(r_function, true);
	const size_t cursor = completion ? cls.source->text.find(CURSOR) : 0;
	// Other files use what _init assigns (architecture §6.4), so a check of
	// the declarations alone checks its body for that, without its errors.
	const bool init = r_function.name == "_init" && !r_function.is_static;
	const bool for_others = init && !check_bodies;
	if (!for_others && (options.declarations_only || !check_bodies || (completion && (cursor < cls.source->begin(r_function.range) || cursor >= cls.source->end(r_function.range))))) {
		function = nullptr;
		scopes.clear();
		return;
	}
	const size_t earlier_errors = errors.size();
	check_block(r_function.body);
	if (r_function.has_body && r_function.return_type.kind != TypeKind::Void && !r_function.return_type.is_error() && !always_ends(r_function.body, false)) {
		error(r_function.name_token, "Not all paths of \"" + r_function.name + "()\" return a value.");
	}
	if (init) {
		std::set<int> reported;
		for (const int member : init_block(r_function.body, std::set<int>(), reported).value_or(std::set<int>())) {
			r_function.assigned.push_back(cls.all_members[member]->name);
		}
	}
	if (for_others) {
		errors.erase(errors.begin() + std::ptrdiff_t(earlier_errors), errors.end());
	}
	function = nullptr;
	scopes.clear();
}

/* Interfaces (spec §3.9) */

// The signatures of the interfaces the class declares (spec §3.9).
void Checker::declare_interfaces() {
	for (InterfaceDecl &interface : cls.interfaces) {
		refer(interface.name_token, script_id(options.path, interface.name), SymbolKind::Class);
		for (FuncDecl &method : interface.methods) {
			refuse_new(method);
			refer(method.name_token, script_id(options.path, interface.name + "." + method.name), SymbolKind::Method);
			for (ParamDecl &param : method.params) {
				if (!param.declared_type) {
					untyped(param);
					continue;
				}
				param.type = resolve(*param.declared_type, false);
			}
			check_rest(method);
			method.return_type = method.return_type_ref ? resolve(*method.return_type_ref, true) : Type::of(TypeKind::Void);
		}
		std::unordered_set<std::string> members;
		for (const FuncDecl &method : interface.methods) {
			members.insert(method.name);
		}
		for (InterfaceProperty &property : interface.properties) {
			if (!members.insert(property.name).second) {
				error(property.name_token, "\"" + property.name + "\" is already declared in " + interface.name + ".");
			}
			refer(property.name_token, script_id(options.path, interface.name + "." + property.name), SymbolKind::Property);
			property.type = resolve(property.type_ref, false);
		}
		for (SignalDecl &signal : interface.signals) {
			if (!members.insert(signal.name).second) {
				error(signal.name_token, "\"" + signal.name + "\" is already declared in " + interface.name + ".");
			}
			refer(signal.name_token, script_id(options.path, interface.name + "." + signal.name), SymbolKind::Signal);
			type_signal_params(signal);
		}
		for (const TypeRef &ref : interface.extends) {
			const Type parent = resolve(ref, false);
			if (!parent.is_error() && !parent.is_interface) {
				error(ref.range, parent.to_string() + " is not an interface.");
			} else if (!parent.is_error()) {
				// It shows the parent's members wherever it is seen (spec §4.2).
				if (const std::optional<Type> hidden = interface.is_pub ? hidden_part(parent) : std::nullopt) {
					error(ref.range, hidden->to_string() + " is not pub, but the pub " + interface.name + " extends it.");
				}
				interface.parents.push_back(parent);
			}
		}
	}
	for (const InterfaceDecl &interface : cls.interfaces) {
		check_interface_parents(interface);
	}
}

// What an interface extends (spec §3.9): no cycle, and each name with one
// signature, or a method redeclared with a narrower return type.
void Checker::check_interface_parents(const InterfaceDecl &p_interface) {
	const Type self = Type::interface(display_name(cls, options.path) + "." + p_interface.name, options.path);
	const std::vector<Type> up = interface_order(p_interface.parents);
	if (std::any_of(up.begin(), up.end(), [&](const Type &p_up) { return p_up == self; })) {
		error(p_interface.name_token, p_interface.name + " extends itself.");
		return;
	}
	const std::vector<InterfaceMember> all = interface_members({ self });
	for (const Type &parent : p_interface.parents) {
		for (const InterfaceMember &inherited : interface_members({ parent })) {
			if (const std::optional<std::pair<InterfaceMember, uint32_t>> mine = own_interface_member(p_interface, self, inherited.name())) {
				if (!fits_inherited(mine->first, inherited, true)) {
					error(mine->second, "\"" + inherited.name() + "\" does not fit \"" + inherited.name() + "\" of " + inherited.owner.name + ".");
				}
				continue;
			}
			// Another parent's must agree, unless one extends the other.
			const auto found = std::find_if(all.begin(), all.end(), [&](const InterfaceMember &p_known) { return p_known.name() == inherited.name(); });
			if (found != all.end() && !(found->owner == inherited.owner) && !is_a(found->owner, inherited.owner) && !fits_inherited(*found, inherited, false)) {
				error(p_interface.name_token, found->owner.name + " and " + inherited.owner.name + " declare \"" + inherited.name() + "\" differently; redeclare it in " + p_interface.name + ".");
			}
		}
	}
}

// A member the interface itself declares, and its name's token.
std::optional<std::pair<Checker::InterfaceMember, uint32_t>> Checker::own_interface_member(const InterfaceDecl &p_interface, const Type &p_self, const std::string &p_name) {
	for (const FuncDecl &method : p_interface.methods) {
		if (method.name == p_name) {
			return std::pair(InterfaceMember{ &method, nullptr, nullptr, p_self }, method.name_token);
		}
	}
	for (const InterfaceProperty &property : p_interface.properties) {
		if (property.name == p_name) {
			return std::pair(InterfaceMember{ nullptr, &property, nullptr, p_self }, property.name_token);
		}
	}
	for (const SignalDecl &signal : p_interface.signals) {
		if (signal.name == p_name) {
			return std::pair(InterfaceMember{ nullptr, nullptr, &signal, p_self }, signal.name_token);
		}
	}
	return std::nullopt;
}

// Whether a member stands for one an interface inherits: a method with the
// same parameters and a return type that is the same or narrower, when it
// redeclares it; otherwise the same signature, the same property type, or
// the same signal arguments. A property that must be writable stands for
// one that need only be readable, not the other way.
bool Checker::fits_inherited(const InterfaceMember &p_member, const InterfaceMember &p_inherited, bool p_redeclared) const {
	if (p_member.method && p_inherited.method) {
		const FuncDecl &a = *p_member.method;
		const FuncDecl &b = *p_inherited.method;
		bool same = a.params.size() == b.params.size() && a.is_async == b.is_async && (p_redeclared ? assignable(a.return_type, b.return_type) : a.return_type == b.return_type);
		for (size_t i = 0; same && i < a.params.size(); i++) {
			same = a.params[i].type == b.params[i].type && a.params[i].is_rest == b.params[i].is_rest;
		}
		return same;
	}
	if (p_member.property && p_inherited.property) {
		return p_member.property->type == p_inherited.property->type && (!p_member.property->is_readonly || p_inherited.property->is_readonly || !p_redeclared);
	}
	if (p_member.signal && p_inherited.signal) {
		return param_types(p_member.signal->params) == param_types(p_inherited.signal->params);
	}
	return false;
}

// `implements I`: the class has every method of I and of the interfaces I
// extends, pub and with the same signature, and is each of those
// interfaces (spec §3.9).
void Checker::check_implements() {
	std::vector<std::pair<Type, const TypeRef *>> named;
	for (const TypeRef &ref : cls.implements) {
		const Type type = resolve(ref, false);
		if (type.is_error()) {
			continue;
		}
		if (!type.is_interface) {
			error(ref.range, type.to_string() + " is not an interface.");
			continue;
		}
		named.push_back({ type, &ref });
		for (const Type &interface : interface_order({ type })) {
			if (std::find(cls.implemented.begin(), cls.implemented.end(), interface) == cls.implemented.end()) {
				cls.implemented.push_back(interface);
			}
		}
	}
	for (const Type &interface : cls.implemented) {
		if (const InterfaceDecl *decl = interface_decl(interface)) {
			for (const FuncDecl &method : decl->methods) {
				cls.interface_methods.push_back(&method);
			}
		}
	}
	for (const auto &entry : named) {
		const Type &type = entry.first;
		const TypeRef &ref = *entry.second;
		// One that another named interface extends is checked with it.
		if (std::any_of(named.begin(), named.end(), [&](const auto &p_other) { return !(p_other.first == type) && is_a(p_other.first, type); })) {
			continue;
		}
		own_interfaces.push_back({ type, &ref });
		const size_t reported = errors.size();
		const InterfaceDecl *interface = interface_decl(type);
		if (!interface) {
			error(ref.range, type.script + " has errors.");
			note_implementation(type, ref, reported);
			continue;
		}
		for (const InterfaceMember &member : interface_members({ type })) {
			if (!member.method) {
				continue;
			}
			const FuncDecl &method = *member.method;
			const auto found = function_index.find(method.name);
			if (found == function_index.end()) {
				error(ref.range, "The class does not implement " + method.name + "() of " + member.owner.name + ".");
				continue;
			}
			const FuncDecl &function = *cls.all_functions[found->second];
			if (is_own(function, cls.functions)) {
				refer(function.name_token, script_id(options.path, function.name), SymbolKind::Method, interface_member_id(member));
			}
			if (!function.is_pub) {
				error(own_name(function, cls.functions, ref.range), "\"" + function.name + "()\" implements " + member.owner.name + " and must be pub.");
			}
			if (function.is_static) {
				error(own_name(function, cls.functions, ref.range), "\"" + function.name + "()\" implements " + member.owner.name + ", so it cannot be static.");
			}
			bool same = function.params.size() == method.params.size() && function.return_type == method.return_type && function.is_async == method.is_async;
			for (size_t i = 0; same && i < method.params.size(); i++) {
				same = function.params[i].type == method.params[i].type && function.params[i].is_rest == method.params[i].is_rest;
			}
			if (!same) {
				error(own_name(function, cls.functions, ref.range), "\"" + function.name + "()\" does not have the signature that " + member.owner.name + " declares, " + signature_text(method) + ".");
			}
		}
		note_implementation(type, ref, reported);
	}
}

// That the class has errors in implementing the interface, when some were
// reported since `p_reported`.
void Checker::note_implementation(const Type &p_interface, const TypeRef &p_ref, size_t p_reported) {
	const auto &noted = cls.implemented_with_errors;
	if (errors.size() == p_reported || std::any_of(noted.begin(), noted.end(), [&](const auto &p_noted) { return p_noted.first == p_interface; })) {
		return;
	}
	cls.implemented_with_errors.push_back({ p_interface, cls.source->position(cls.source->begin(p_ref.range)).line });
}

// A class of another file used as an interface it has errors in
// implementing, as other declarations with errors are (architecture §2.1).
void Checker::expect_implemented(const Expr &p_value, const Type &p_from, const Type &p_to) {
	if (!p_to.is_interface || p_from.is_interface || !is_script_path(p_from.script) || in_this_file(p_from.script)) {
		return;
	}
	const ClassDecl *decl = script_decl(p_from.script);
	if (!decl) {
		return;
	}
	Type interface = p_to;
	interface.nullable = false;
	for (const auto &[type, line] : decl->implemented_with_errors) {
		if (type == interface) {
			Type from = p_from;
			from.nullable = false;
			error(p_value.range, from.to_string() + " implements " + interface.to_string() + " with errors (" + script_file(p_from.script) + ", line " + std::to_string(line) + ").");
		}
	}
}

// The properties and signals the class's interfaces require, after the
// members' types and the signals are known (spec §3.9): a pub member of the
// same type, or an inherited engine property, which must have a setter
// unless the requirement is @readonly; a pub signal with the same arguments,
// its own, inherited or of the engine class.
void Checker::check_interface_properties() {
	for (const auto &[type, ref] : own_interfaces) {
		const size_t reported = errors.size();
		for (const InterfaceMember &member : interface_members({ type })) {
			if (member.signal) {
				check_interface_signal(member, *ref);
			}
			if (!member.property || member.property->type.is_error()) {
				continue;
			}
			const InterfaceProperty &required = *member.property;
			const std::string &owner = member.owner.name;
			const auto found = member_index.find(required.name);
			if (found != member_index.end()) {
				const VarDecl &provided = *cls.all_members[found->second];
				if (is_own(provided, cls.members)) {
					refer(provided.name_token, script_id(options.path, provided.name), SymbolKind::Property, interface_member_id(member));
				}
				if (!provided.is_pub) {
					error(own_name(provided, cls.members, ref->range), "\"" + provided.name + "\" implements " + owner + " and must be pub.");
				} else if (!provided.type.is_error() && provided.type != required.type) {
					error(own_name(provided, cls.members, ref->range), "\"" + provided.name + "\" has type " + provided.type.to_string() + ", but " + owner + " requires " + required.type.to_string() + ".");
				}
				continue;
			}
			const auto property = engine.find_property(cls.native_base, required.name);
			if (!property) {
				error(ref->range, "The class does not implement the property \"" + required.name + "\" of " + owner + ".");
			} else if (property->type != required.type) {
				error(ref->range, "The engine property \"" + required.name + "\" has type " + property->type.to_string() + ", but " + owner + " requires " + required.type.to_string() + ".");
			} else if (!property->writable && !required.is_readonly) {
				error(ref->range, "The engine property \"" + required.name + "\" is read-only, so it satisfies only a @readonly property of " + owner + ".");
			}
		}
		note_implementation(type, *ref, reported);
	}
}

void Checker::check_interface_signal(const InterfaceMember &p_member, const TypeRef &p_ref) {
	const SignalDecl &required = *p_member.signal;
	const std::vector<Type> wanted = param_types(required.params);
	if (std::any_of(wanted.begin(), wanted.end(), [](const Type &p_type) { return p_type.is_error(); })) {
		return;
	}
	const std::string &owner = p_member.owner.name;
	const auto found = signal_index.find(required.name);
	if (found != signal_index.end()) {
		const SignalDecl &provided = *cls.all_signals[found->second];
		if (is_own(provided, cls.signals)) {
			refer(provided.name_token, script_id(options.path, provided.name), SymbolKind::Signal, interface_member_id(p_member));
		}
		const std::vector<Type> given = param_types(provided.params);
		if (!provided.is_pub) {
			error(own_name(provided, cls.signals, p_ref.range), "\"" + provided.name + "\" implements " + owner + " and must be pub.");
		} else if (std::none_of(given.begin(), given.end(), [](const Type &p_type) { return p_type.is_error(); }) && given != wanted) {
			error(own_name(provided, cls.signals, p_ref.range), "\"" + provided.name + "\" is " + Type::signal(given).to_string() + ", but " + owner + " requires " + Type::signal(wanted).to_string() + ".");
		}
		return;
	}
	const auto signal = engine.find_signal(cls.native_base, required.name);
	if (!signal) {
		error(p_ref.range, "The class does not implement the signal \"" + required.name + "\" of " + owner + ".");
	} else if (signal_type(*signal).args != wanted) {
		error(p_ref.range, "The engine signal \"" + required.name + "\" is " + signal_type(*signal).to_string() + ", but " + owner + " requires " + Type::signal(wanted).to_string() + ".");
	}
}

/* Structs (spec §3.6) */

// The fields of the structs the class declares (spec §3.6); the types that
// only default values give come with check_field_defaults().
void Checker::declare_structs() {
	for (StructDecl &decl : cls.structs) {
		refer(decl.name_token, script_id(options.path, decl.name), SymbolKind::Class);
		std::unordered_set<std::string> fields;
		for (VarDecl &field : decl.fields) {
			refer(field.name_token, script_id(options.path, decl.name + "." + field.name), SymbolKind::Property);
			if (!fields.insert(field.name).second) {
				error(field.name_token, "The field \"" + field.name + "\" is already declared in " + decl.name + ".");
			}
			field.type = field.declared_type ? resolve(*field.declared_type, false) : Type::of(TypeKind::Error);
			if (!field.initializer && !field.declared_type) {
				error(field.name_token, "\"" + field.name + "\" needs a type or a default value.");
			}
		}
		for (FuncDecl &method : decl.methods) {
			const std::string name = method.name.substr(decl.name.size() + 1);
			if (fields.count(name) || function_index.count(method.name)) {
				error(method.name_token, "\"" + name + "\" is already declared in " + decl.name + ".");
				continue;
			}
			function_index[method.name] = int(cls.all_functions.size());
			cls.all_functions.push_back(&method);
		}
	}
}

// A struct or an enum with payloads that holds itself without a container
// has no finite size (spec §3.6). Once the fields of both are resolved.
void Checker::check_held_cycles() {
	std::vector<std::string> held;
	for (const StructDecl &decl : cls.structs) {
		held.push_back(decl.name);
	}
	for (const EnumDecl &decl : cls.enums) {
		if (decl.has_payloads && !decl.name.empty()) {
			held.push_back(decl.name);
		}
	}
	for (const std::string &name : held) {
		std::vector<std::string> cycle;
		std::unordered_set<std::string> seen;
		if (holds(cls, options.path, name, options.path + "::" + name, cycle, seen)) {
			std::string fields;
			for (const std::string &field : cycle) {
				fields += (fields.empty() ? "" : ", ") + field;
			}
			const StructDecl *found = cls.find_struct(name);
			error(found ? found->name_token : cls.find_enum(name)->name_token, name + " contains itself through " + fields + ", so it has no finite size; hold it in a container or a RefCounted class.");
		}
	}
}

// Once the signatures are known, as for member initializers. A default
// value sees what a static method of the struct sees, which is no field.
void Checker::check_field_defaults() {
	for (StructDecl &decl : cls.structs) {
		structure = &decl;
		structure_type = Type::structure(display_name(cls, options.path) + "." + decl.name, options.path);
		for (VarDecl &field : decl.fields) {
			if (!field.initializer) {
				continue;
			}
			field_default = &field;
			const Type value = check_value(*field.initializer, field.type);
			field_default = nullptr;
			if (field.declared_type) {
				expect_assignable(*field.initializer, value, field.type, "assign");
			} else if (value.kind == TypeKind::None) {
				error(field.name_token, "\"" + field.name + "\" needs a type: None does not say which T? it is.");
			} else {
				field.type = value;
			}
		}
	}
	structure = nullptr;
}

// The fields of the struct, or of the variants of the enum with payloads,
// that the header declares by that name, each as an error names it.
std::vector<std::pair<std::string, const VarDecl *>> Checker::value_fields(const ClassDecl &p_header, const std::string &p_name) {
	std::vector<std::pair<std::string, const VarDecl *>> fields;
	if (const StructDecl *found = p_header.find_struct(p_name)) {
		for (const VarDecl &field : found->fields) {
			fields.push_back({ p_name + "." + field.name, &field });
		}
	} else if (const EnumDecl *found = p_header.find_enum(p_name)) {
		for (const EnumValueDecl &value : found->values) {
			for (const VarDecl &field : value.fields) {
				fields.push_back({ p_name + "." + value.name + "." + field.name, &field });
			}
		}
	}
	return fields;
}

// Whether the struct or the enum with payloads holds `p_target`
// ("script::Name") in its slot, directly or through other ones, with the
// field types their classes resolved; `r_cycle` lists the fields on the
// way. A class whose declarations are being checked is not followed: the
// check of the cycle's first class sees all the others.
bool Checker::holds(const ClassDecl &p_decl, const std::string &p_path, const std::string &p_name, const std::string &p_target, std::vector<std::string> &r_cycle, std::unordered_set<std::string> &r_seen) {
	if (!r_seen.insert(p_path + "::" + p_name).second) {
		return false;
	}
	for (const auto &[label, field] : value_fields(p_decl, p_name)) {
		for (const Type &type : held_types(field->type)) {
			const ClassDecl *decl = type == Type::conversion_error() ? nullptr : script_decl(type.script);
			if (!decl) {
				continue;
			}
			r_cycle.push_back(label);
			if (type.script + "::" + type.interface_name() == p_target || holds(*decl, type.script, type.interface_name(), p_target, r_cycle, r_seen)) {
				return true;
			}
			r_cycle.pop_back();
		}
	}
	return false;
}

// The structs and enums with payloads whose values a value of the type
// holds in its slot: T holds a T, as T? and Settled[T] do, and
// Result[T, E] holds both; a container holds a reference.
std::vector<Type> Checker::held_types(const Type &p_type) {
	if (p_type.kind == TypeKind::Option || p_type.is_result() || p_type.is_settled()) {
		std::vector<Type> held;
		for (const Type &arg : p_type.args) {
			const std::vector<Type> inner = held_types(arg);
			held.insert(held.end(), inner.begin(), inner.end());
		}
		return held;
	}
	return p_type.kind == TypeKind::Struct || p_type.kind == TypeKind::PayloadEnum ? std::vector<Type>{ p_type } : std::vector<Type>();
}

/* pub signatures and what the engine sees (spec §4.2, §10.1) */

// The part of a type that is not seen everywhere: a struct, an enum, an
// interface or an inner class without pub, or one inside a class without
// it (spec §4.2); nullopt when there is none.
std::optional<Type> Checker::hidden_part(const Type &p_type) const {
	for (const Type &arg : p_type.args) {
		if (std::optional<Type> part = hidden_part(arg)) {
			return part;
		}
	}
	if (p_type.values && p_type.values->is_engine) {
		return std::nullopt;
	}
	const ClassDecl *header = script_header(p_type.script);
	const std::string name = p_type.interface_name();
	bool pub = true;
	if (header && p_type.kind == TypeKind::Struct) {
		const StructDecl *decl = header->find_struct(name);
		pub = !decl || decl->is_pub;
	} else if (header && (p_type.kind == TypeKind::Enum || p_type.kind == TypeKind::PayloadEnum)) {
		const EnumDecl *decl = header->find_enum(name);
		pub = !decl || decl->is_pub;
	} else if (header && p_type.is_interface) {
		const auto decl = std::find_if(header->interfaces.begin(), header->interfaces.end(), [&](const InterfaceDecl &p_decl) { return p_decl.name == name; });
		pub = decl == header->interfaces.end() || decl->is_pub;
	}
	for (std::string path = p_type.script; pub && path.find("::") != std::string::npos; path = path.substr(0, path.rfind("::"))) {
		const ClassDecl *inner = script_header(path);
		pub = !inner || inner->is_pub;
	}
	Type shown = p_type;
	shown.nullable = false;
	return pub ? std::nullopt : std::optional<Type>(shown);
}

// A pub declaration shows the types of its signature wherever it is seen,
// so each of them must be seen there too (spec §4.2).
void Checker::check_pub_signatures() {
	const auto check = [&](bool p_pub, uint32_t p_token, const std::string &p_what, const std::vector<Type> &p_types) {
		for (const Type &type : p_types) {
			const std::optional<Type> part = p_pub ? hidden_part(type) : std::nullopt;
			if (part) {
				error(p_token, part->to_string() + " is not pub, but " + p_what + " shows it.");
				return;
			}
		}
	};
	const auto signature = [](const FuncDecl &p_function) {
		std::vector<Type> types = { p_function.return_type };
		for (const ParamDecl &param : p_function.params) {
			types.push_back(param.type);
		}
		return types;
	};
	for (const std::vector<VarDecl> *variables : { &cls.members, &cls.static_vars }) {
		for (const VarDecl &variable : *variables) {
			check(variable.is_pub, variable.name_token, "the pub \"" + variable.name + "\"", { variable.type });
		}
	}
	for (const ConstDecl &constant : cls.constants) {
		check(constant.is_pub, constant.name_token, "the pub \"" + constant.name + "\"", { constant.type });
	}
	for (const FuncDecl &function : cls.functions) {
		check(function.is_pub, function.name_token, "the pub \"" + function.name + "()\"", signature(function));
	}
	for (const SignalDecl &signal : cls.signals) {
		check(signal.is_pub, signal.name_token, "the pub signal \"" + signal.name + "\"", param_types(signal.params));
	}
	for (const StructDecl &decl : cls.structs) {
		for (const VarDecl &field : decl.fields) {
			check(decl.is_pub, field.name_token, "the field \"" + field.name + "\" of the pub " + decl.name, { field.type });
		}
		for (const FuncDecl &method : decl.methods) {
			check(decl.is_pub, method.name_token, "the method \"" + method.name.substr(decl.name.size() + 1) + "()\" of the pub " + decl.name, signature(method));
		}
	}
	for (const EnumDecl &decl : cls.enums) {
		for (size_t v = 0; decl.has_payloads && decl.type.values && v < decl.values.size(); v++) {
			std::vector<Type> fields;
			for (const auto &[name, type] : decl.type.values->fields[v]) {
				fields.push_back(type);
			}
			check(decl.is_pub, decl.values[v].name_token, "the variant " + decl.values[v].name + " of the pub " + decl.name, fields);
		}
	}
	for (const InterfaceDecl &decl : cls.interfaces) {
		for (const FuncDecl &method : decl.methods) {
			check(decl.is_pub, method.name_token, "the method \"" + method.name + "()\" of the pub " + decl.name, signature(method));
		}
		for (const InterfaceProperty &property : decl.properties) {
			check(decl.is_pub, property.name_token, "the property \"" + property.name + "\" of the pub " + decl.name, { property.type });
		}
		for (const SignalDecl &signal : decl.signals) {
			check(decl.is_pub, signal.name_token, "the signal \"" + signal.name + "\" of the pub " + decl.name, param_types(signal.params));
		}
	}
}

// Types whose None the engine would lose where it sees the value (spec
// §10.1): of pub members, constants and functions, of signals, which
// always pass through the engine, and what coroutines return (§9.8).
void Checker::check_engine_options() {
	const auto check = [&](uint32_t p_token, const std::string &p_what, const Type &p_type) {
		const std::string loss = encoding_loss(p_type, "the engine", false);
		if (!loss.empty()) {
			error(p_token, p_what + " " + p_type.to_string() + ", but " + loss);
		}
	};
	for (const std::vector<VarDecl> *variables : { &cls.members, &cls.static_vars }) {
		for (const VarDecl &variable : *variables) {
			if (variable.is_pub) {
				check(variable.name_token, "The pub \"" + variable.name + "\" has type", variable.type);
			}
		}
	}
	for (const ConstDecl &constant : cls.constants) {
		if (constant.is_pub) {
			check(constant.name_token, "The pub \"" + constant.name + "\" has type", constant.type);
		}
	}
	for (const FuncDecl &function : cls.functions) {
		if (function.is_pub || function.is_async) {
			check(function.name_token, "\"" + function.name + "()\" returns", function.return_type);
		}
		for (size_t i = 0; function.is_pub && i < function.params.size(); i++) {
			check(function.params[i].name_token, "The parameter \"" + function.params[i].name + "\" of the pub \"" + function.name + "()\" has type", function.params[i].type);
		}
	}
	for (const SignalDecl &signal : cls.signals) {
		for (const ParamDecl &param : signal.params) {
			check(param.name_token, "The parameter \"" + param.name + "\" of the signal \"" + signal.name + "\" has type", param.type);
		}
	}
}

/* Signals (spec §4.7) */

// The class's signals, whose arguments are typed (spec §4.7).
void Checker::declare_signals() {
	for (size_t i = 0; i < cls.signals.size(); i++) {
		SignalDecl &signal = cls.signals[i];
		if (signal_index.count(signal.name) || member_index.count(signal.name) || constant_index.count(signal.name) || function_index.count(signal.name) || engine.find_signal(cls.native_base, signal.name)) {
			error(signal.name_token, "\"" + signal.name + "\" is already declared.");
			continue;
		}
		signal_index[signal.name] = int(cls.all_signals.size());
		cls.all_signals.push_back(&signal);
		refer(signal.name_token, script_id(options.path, signal.name), SymbolKind::Signal);
		type_signal_params(signal);
	}
}

void Checker::type_signal_params(SignalDecl &r_signal) {
	for (ParamDecl &param : r_signal.params) {
		if (!param.declared_type) {
			error(param.name_token, "The argument \"" + param.name + "\" of a signal needs a type.");
			param.type = Type::of(TypeKind::Error);
			continue;
		}
		if (param.default_value) {
			error(param.default_value->range, "The arguments of a signal have no default values.");
		}
		param.type = resolve(*param.declared_type, false);
	}
}

} // namespace petrichor::compiler::checking
