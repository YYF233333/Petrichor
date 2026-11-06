#include "compiler/lower.h"

#include "compiler/builtin_api.h"
#include "compiler/ir.h"

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace petrichor::compiler {

using bytecode::Op;
using bytecode::ValueType;
using ir::NO_REG;

namespace {

ValueType value_type(const Type &p_type) {
	switch (p_type.kind) {
		case TypeKind::Bool:
			return ValueType::Bool;
		case TypeKind::Int:
		case TypeKind::Enum:
			return ValueType::Int;
		case TypeKind::Float:
			return ValueType::Float;
		case TypeKind::String:
			return ValueType::String;
		case TypeKind::Builtin: {
			const ValueType type = bytecode::value_type_from_name(p_type.name);
			return type == ValueType::Count ? ValueType::Void : type;
		}
		case TypeKind::Task:
		case TypeKind::Object:
			return ValueType::Object;
		case TypeKind::None:
		case TypeKind::Variant:
			return ValueType::Variant;
		case TypeKind::Function:
			return ValueType::Callable;
		case TypeKind::Signal:
			return ValueType::Signal;
		default:
			return ValueType::Void;
	}
}

// An interface as the runtime names it, "script::Name" (spec §3.9).
std::string interface_id(const Type &p_interface) {
	return p_interface.script + "::" + p_interface.interface_name();
}

// The engine's class, and for a class of this language its script; for an
// interface, the script that declares it and its name.
bytecode::ClassRef class_ref(const Type &p_type) {
	bytecode::ClassRef ref;
	ref.name = p_type.engine_class();
	ref.script = p_type.script;
	if (p_type.is_interface) {
		ref.interface_name = p_type.interface_name();
	}
	return ref;
}

bool is_typed_array(const Type &p_type) {
	return p_type.kind == TypeKind::Builtin && p_type.name == "Array" && !p_type.args.empty();
}

bool is_8_byte(ValueType p_type) {
	return p_type == ValueType::Bool || p_type == ValueType::Int || p_type == ValueType::Float;
}

bool is_primitive(ValueType p_type) {
	return is_8_byte(p_type) || p_type == ValueType::String;
}

// A place that can be read and written. The base of a member access is
// evaluated once; a member of a built-in value is changed in a copy of the
// value, which is then written back to where it came from. A field of a
// struct in a register or in a member is a place of its own inside it; one
// read through an accessor or the engine is changed in a copy of the struct,
// written back as a member of a built-in value is (spec §3.6).
struct Place {
	enum class Kind {
		Local, // A register: a local, or a field of a struct in one.
		NarrowedLocal, // A local of type T? where it is known to hold a T (spec §5.4).
		// A Variant known to hold a T, or an untyped container known to be typed
		// so (spec §3.8, §5.4): read converted to `type`, written back as declared.
		Retyped,
		Member,
		MemberField, // A field of a struct in a member that has no accessors, at `offset` in it.
		Static, // A static variable (spec §4.8), `variable`, the `index`th of the class at `owner`.
		SelfProperty,
		Owner, // owner with @owner(T): read with its check (spec §7.3).
		ObjectProperty,
		Field,
		StructCopy, // A field of a copy of a struct, `whole`.
		Element, // An element of a container, at the index or key `at`; one of a packed array goes back to `parent`.
	};
	Kind kind = Kind::Local;
	uint32_t reg = NO_REG; // Local: the register; ObjectProperty: the object; Field: the copy of the base; StructCopy: the field in the copy; Element: the container.
	uint32_t index = 0; // Member, property table or field table.
	uint32_t at = NO_REG;
	uint32_t offset = 0;
	uint32_t whole = NO_REG;
	std::vector<uint32_t> fields; // StructCopy: the fields from `whole` down to this one.
	Type type;
	std::optional<bytecode::IntRange> int_range; // An engine property: what its setter takes (spec §10.6).
	std::unique_ptr<Place> parent; // Field, StructCopy and Element: where the copy goes back.
	const VarDecl *variable = nullptr;
	std::string owner;
};

class Lowerer {
	const ClassDecl &cls;
	bytecode::Module &module;
	std::vector<Diagnostic> &errors;
	const StructLookup &structs;
	std::unordered_map<uint32_t, Type> struct_types; // By their index in the type table.
	std::unordered_set<std::string> interning;
	// The entry of each constant value that is not a scalar, so that every use
	// of a constant reads the same container (spec §4.5).
	std::unordered_map<const ConstValue *, uint32_t> constant_entries;

	ir::Function fn;
	const FuncDecl *decl = nullptr; // Null in the init function.
	uint32_t current = 0;
	TokenRange at; // The statement, member or function being lowered, where errors go.

	struct Loop {
		uint32_t continue_block;
		uint32_t break_block;
	};
	std::vector<Loop> loops;
	// A lambda's register for each of its locals, whose captures come right
	// after the parameters; empty where a local is its register.
	std::vector<uint32_t> local_regs;
	// Lambdas, which come after the class's functions (spec §3.11), in the order
	// they are made in.
	std::vector<bytecode::Function> lambdas;

	uint32_t reg_of(int p_local) const {
		return local_regs.empty() ? uint32_t(p_local) : local_regs[p_local];
	}

	/* Blocks and registers */

	uint32_t new_block() {
		fn.blocks.emplace_back();
		return uint32_t(fn.blocks.size() - 1);
	}

	bool terminated() const { return fn.blocks[current].terminator != ir::Terminator::None; }

	void add(const ir::Instr &p_instr) {
		if (terminated()) {
			current = new_block(); // Unreachable code after a return or jump.
		}
		fn.blocks[current].instrs.push_back(p_instr);
	}

	void jump(uint32_t p_target) {
		if (!terminated()) {
			fn.blocks[current].terminator = ir::Terminator::Jump;
			fn.blocks[current].target = p_target;
		}
	}

	void branch(uint32_t p_condition, uint32_t p_then, uint32_t p_else) {
		if (terminated()) {
			current = new_block();
		}
		ir::Block &block = fn.blocks[current];
		block.terminator = ir::Terminator::Branch;
		block.value = p_condition;
		block.target = p_then;
		block.else_target = p_else;
	}

	uint32_t temp(const Type &p_type) {
		fn.regs.push_back(slot(std::string(), p_type));
		return uint32_t(fn.regs.size() - 1);
	}

	uint32_t temp(ValueType p_type) {
		bytecode::Slot slot;
		slot.type = type_index(p_type);
		fn.regs.push_back(slot);
		return uint32_t(fn.regs.size() - 1);
	}

	// How the register holds its value in the engine's layout; Void for T?.
	ValueType reg_type(uint32_t p_reg) const { return bytecode::storage_of(module.types, fn.regs[p_reg].type); }

	bool is_option_reg(uint32_t p_reg) const { return module.types[fn.regs[p_reg].type].kind == bytecode::TypeKind::Option; }

	/* Types */

	// The type's entry in the module's type table, added once (architecture §2.2).
	uint32_t type_index(const bytecode::TypeDesc &p_type) {
		return intern(module.types, p_type, [](const auto &a, const auto &b) { return a == b; });
	}

	uint32_t type_index(ValueType p_type) {
		bytecode::TypeDesc type;
		type.value = p_type;
		return type_index(type);
	}

	uint32_t type_index(const Type &p_type) {
		bytecode::TypeDesc type;
		if (p_type.kind == TypeKind::Task) {
			type.kind = bytecode::TypeKind::Object;
			type.object.name = "CoroutineTask";
			type.nullable = p_type.nullable;
			type.task_result = bytecode::task_result_key(module.types, type_index(p_type.args[0]));
		} else if (p_type.kind == TypeKind::Object) {
			type.kind = bytecode::TypeKind::Object;
			type.object = class_ref(p_type);
			type.nullable = p_type.nullable;
		} else if (p_type.kind == TypeKind::Option) {
			type.kind = bytecode::TypeKind::Option;
			type.element = type_index(p_type.args[0]);
		} else if (is_typed_array(p_type)) {
			type.kind = bytecode::TypeKind::Array;
			type.element = type_index(p_type.args[0]);
		} else if (is_typed_container(p_type)) {
			type.kind = bytecode::TypeKind::Dictionary;
			type.key = type_index(p_type.args[0]);
			type.element = type_index(p_type.args[1]);
		} else if (p_type.kind == TypeKind::Struct) {
			return struct_index(p_type);
		} else if (p_type.kind == TypeKind::PayloadEnum) {
			return payload_enum_index(p_type);
		} else if (p_type.kind == TypeKind::Enum) {
			// "script::Name" for an enum of this language, as for a struct.
			type.kind = bytecode::TypeKind::Enum;
			type.value = ValueType::Int;
			type.name = p_type.values && p_type.values->is_engine ? p_type.name : p_type.script + "::" + p_type.interface_name();
			for (const auto &[name, value] : p_type.values ? p_type.values->list : std::vector<std::pair<std::string, int64_t>>()) {
				type.values.push_back({ name, value });
			}
		} else {
			type.value = value_type(p_type);
		}
		return type_index(type);
	}

	// A struct, "script::Name", with the types of its fields before it.
	uint32_t struct_index(const Type &p_struct) {
		bytecode::TypeDesc type;
		type.kind = bytecode::TypeKind::Struct;
		type.name = p_struct.script + "::" + p_struct.interface_name();
		const StructDecl *decl = struct_decl(p_struct);
		// The checker refuses a struct that holds itself (spec §3.6), unless its
		// script failed to be checked.
		if (!decl || !interning.insert(type.name).second) {
			error(p_struct.name + " cannot be compiled: " + p_struct.script + " has errors.");
			return type_index(ValueType::Void);
		}
		for (const VarDecl &field : decl->fields) {
			type.fields.push_back({ field.name, type_index(field.type) });
		}
		interning.erase(type.name);
		const uint32_t index = type_index(type);
		struct_types.emplace(index, p_struct);
		return index;
	}

	// An enum with payloads, "script::Name", or "Result", laid out as a struct
	// whose first field is the variant's index (bytecode/module.h).
	uint32_t payload_enum_index(const Type &p_enum) {
		bytecode::TypeDesc type;
		type.kind = bytecode::TypeKind::PayloadEnum;
		type.name = p_enum.is_result() ? p_enum.name : p_enum.script + "::" + p_enum.interface_name();
		// The checker refuses one that holds itself (spec §3.6); a Result holds
		// itself only through a struct or an enum, which is interned.
		if (!p_enum.is_result() && !interning.insert(type.name).second) {
			error(p_enum.name + " cannot be compiled: it contains itself.");
			return type_index(ValueType::Void);
		}
		type.fields.push_back({ std::string(), type_index(ValueType::Int) });
		for (size_t v = 0; v < p_enum.values->list.size(); v++) {
			const auto &fields = p_enum.values->fields[v];
			type.values.push_back({ p_enum.values->list[v].first, int64_t(fields.size()) });
			for (const auto &[name, field_type] : fields) {
				type.fields.push_back({ name, type_index(field_type) });
			}
		}
		interning.erase(type.name);
		return type_index(type);
	}

	// The field of a variant in the slot of an enum with payloads: after the
	// variant's index and the fields of the variants before it.
	static uint32_t variant_field(const Type &p_enum, int p_variant, size_t p_field) {
		size_t index = 1;
		for (int v = 0; v < p_variant; v++) {
			index += p_enum.values->fields[v].size();
		}
		return uint32_t(index + p_field);
	}

	// `Command.MOVE(target = p)`, or `Ok(v)` by position (spec §3.5): the
	// arguments in the order written, each into its field once all are
	// evaluated (§3.7).
	uint32_t construct_variant(const Expr &p_call) {
		const auto &fields = p_call.type.values->fields[p_call.index];
		std::vector<uint32_t> values(fields.size(), NO_REG);
		for (size_t i = 0; i < p_call.args.size(); i++) {
			size_t f = p_call.arg_names.empty() ? i : 0;
			while (!p_call.arg_names.empty() && fields[f].first != p_call.arg_names[i]) {
				f++;
			}
			values[f] = argument(*p_call.args[i], p_call.param_types[i], false, false);
		}
		const uint32_t result = temp(p_call.type);
		make_variant(result, p_call.type, p_call.index, values);
		return result;
	}

	// The variant `p_variant` in `p_dst`, with every field at its default, so
	// that a register reused by a loop holds nothing of before, then the fields
	// of the variant from `p_fields`.
	void make_variant(uint32_t p_dst, const Type &p_enum, int p_variant, const std::vector<uint32_t> &p_fields) {
		bytecode::Constant blank{ fn.regs[p_dst].type };
		blank.i = p_variant;
		instr(Op::CONST_VALUE, p_dst, NO_REG, NO_REG, add_constant(blank), fn.regs[p_dst].type);
		for (size_t f = 0; f < p_fields.size(); f++) {
			move(field_view(p_dst, variant_field(p_enum, p_variant, f)), p_fields[f]);
		}
	}

	// A method of Result (spec §3.5): its argument evaluated first, as a call's
	// are, then the variant decides, a function passed being called only on
	// the variant it is for.
	uint32_t result_method(const Expr &p_call) {
		const Type &type = p_call.base->type;
		const std::string &name = p_call.text;
		const uint32_t value = lower_expr(*p_call.base);
		const uint32_t argument = p_call.args.empty() ? NO_REG : convert(lower_expr(*p_call.args[0]), p_call.param_types[0]);
		const uint32_t result = temp(p_call.type);
		if (name == "unwrap" || name == "expect") {
			instr(Op::UNWRAP_OK, result, value, argument);
			return result;
		}
		const uint32_t ok = temp(ValueType::Bool);
		const uint32_t zero = temp(ValueType::Int);
		load_int(zero, 0);
		instr(Op::EQ_INT, ok, field_view(value, 0), zero);
		const uint32_t blocks[2] = { new_block(), new_block() };
		const uint32_t end_block = new_block();
		branch(ok, blocks[0], blocks[1]);
		const bool on_error = name == "err" || name == "unwrap_or" || name == "unwrap_or_else" || name == "map_err" || name == "or_else";
		for (int variant = 0; variant < 2; variant++) {
			current = blocks[variant];
			const uint32_t inner = field_view(value, variant_field(type, variant, 0));
			const bool applies = (variant == 1) == on_error;
			const auto called = [&]() { return call_value(argument, p_call.param_types[0], { inner }); };
			if (name == "unwrap_or" || name == "unwrap_or_else") {
				move(result, !applies ? inner : convert(name == "unwrap_or" ? argument : called(), p_call.type));
			} else if (name == "ok" || name == "err") {
				applies ? move(result, convert(inner, p_call.type)) : (void)default_value(result);
			} else if (applies && (name == "and_then" || name == "or_else")) {
				move(result, called());
			} else {
				make_variant(result, p_call.type, variant, { applies ? called() : inner });
			}
			jump(end_block);
		}
		current = end_block;
		return result;
	}

	// A method of T? (spec §3.4), as result_method() lowers those of Result.
	// filter() goes on as None when its function says no.
	uint32_t option_method(const Expr &p_call) {
		const Type &type = p_call.base->type;
		const std::string &name = p_call.text;
		const uint32_t value = lower_expr(*p_call.base);
		const uint32_t argument = convert(lower_expr(*p_call.args[0]), p_call.param_types[0]);
		const uint32_t result = temp(p_call.type);
		const uint32_t none = temp(ValueType::Bool);
		instr(Op::IS_NONE, none, value, NO_REG, 0, fn.regs[value].type);
		const uint32_t some_block = new_block();
		const uint32_t none_block = new_block();
		const uint32_t end_block = new_block();
		branch(none, none_block, some_block);
		const auto called = [&](const std::vector<uint32_t> &p_args) { return call_value(argument, p_call.param_types[0], p_args); };

		current = some_block;
		if (name == "or" || name == "or_else") {
			move(result, value);
		} else {
			const uint32_t inner = temp(type.unwrapped());
			instr(Op::UNWRAP, inner, value, NO_REG, 0, fn.regs[inner].type);
			if (name == "filter") {
				const uint32_t kept_block = new_block();
				branch(called({ inner }), kept_block, none_block);
				current = kept_block;
				move(result, value);
			} else if (name == "ok_or" || name == "ok_or_else") {
				make_variant(result, p_call.type, 0, { inner });
			} else {
				move(result, name == "map" || name == "and_then" ? convert(called({ inner }), p_call.type) : inner);
			}
		}
		jump(end_block);

		current = none_block;
		if (name == "ok_or" || name == "ok_or_else") {
			make_variant(result, p_call.type, 1, { name == "ok_or" ? argument : called({}) });
		} else if (name == "unwrap_or" || name == "or") {
			move(result, argument);
		} else if (name == "unwrap_or_else" || name == "or_else") {
			move(result, convert(called({}), p_call.type));
		} else {
			default_value(result);
		}
		jump(end_block);
		current = end_block;
		return result;
	}

	// `x?` (spec §8.2): the value inside the Result or the T?, or a return of
	// the Err or the None it holds.
	uint32_t try_value(const Expr &p_expr) {
		const Type &type = p_expr.left->type;
		const uint32_t value = lower_expr(*p_expr.left);
		const uint32_t failed = temp(ValueType::Bool);
		if (type.is_result()) {
			const uint32_t zero = temp(ValueType::Int);
			load_int(zero, 0);
			instr(Op::NE_INT, failed, field_view(value, 0), zero);
		} else {
			instr(Op::IS_NONE, failed, value, NO_REG, 0, fn.regs[value].type);
		}
		const uint32_t fail_block = new_block();
		const uint32_t next_block = new_block();
		branch(failed, fail_block, next_block);
		current = fail_block;
		const uint32_t returned = temp(decl->return_type);
		if (type.is_result()) {
			make_variant(returned, decl->return_type, 1, { field_view(value, variant_field(type, 1, 0)) });
		} else {
			default_value(returned);
		}
		fn.blocks[current].terminator = ir::Terminator::Return;
		fn.blocks[current].value = returned;
		current = next_block;
		const uint32_t result = temp(p_expr.type);
		if (type.is_result()) {
			move(result, field_view(value, variant_field(type, 0, 0)));
		} else {
			instr(Op::UNWRAP, result, value, NO_REG, 0, fn.regs[result].type);
		}
		return result;
	}

	// `f.call(...)` of a function value of the type `p_function` (spec §3.11),
	// with the arguments converted to its parameters.
	uint32_t call_value(uint32_t p_value, const Type &p_function, const std::vector<uint32_t> &p_args) {
		ir::Instr i;
		i.op = Op::CALL_VALUE;
		i.base = p_value;
		bytecode::MethodCall method;
		method.method = "call";
		for (size_t a = 0; a < p_args.size(); a++) {
			method.params.push_back(slot(std::string(), p_function.args[a + 1]));
			i.args.push_back(convert(p_args[a], p_function.args[a + 1]));
		}
		method.result = slot(std::string(), p_function.args[0]);
		i.dst = temp(p_function.args[0]);
		i.imm = method_call(method);
		add(i);
		return i.dst;
	}

	const StructDecl *struct_decl(const Type &p_struct) const {
		if (p_struct == Type::conversion_error()) {
			return &conversion_error_decl();
		}
		return p_struct.script == cls.path ? cls.find_struct(p_struct.interface_name()) : (structs ? structs(p_struct) : nullptr);
	}

	// A struct or an enum with payloads, whose fields are registers over its bytes.
	bool is_struct_reg(uint32_t p_reg) const { return bytecode::has_fields(module.types, fn.regs[p_reg].type); }

	// A field of the struct in a register, as a register over its bytes.
	uint32_t field_view(uint32_t p_struct, uint32_t p_field) {
		const uint32_t type = fn.regs[p_struct].type;
		ir::View view{ p_struct, bytecode::field_offsets(module.types, type)[p_field] };
		const auto outer = fn.views.find(p_struct);
		if (outer != fn.views.end()) {
			view.root = outer->second.root;
			view.offset += outer->second.offset;
		}
		bytecode::Slot field;
		field.type = module.types[type].fields[p_field].type;
		fn.regs.push_back(field);
		fn.views.emplace(uint32_t(fn.regs.size() - 1), view);
		return uint32_t(fn.regs.size() - 1);
	}

	// A new struct with every field written, so that a register reused by a
	// loop holds nothing of before: the arguments in the order written, then
	// the defaults of the fields they leave out (spec §3.6). The fields are
	// written once all are evaluated.
	uint32_t construct_struct(const Type &p_struct, const Expr *p_call) {
		const uint32_t result = temp(p_struct);
		const StructDecl *decl = struct_decl(p_struct);
		if (!decl) {
			return result;
		}
		std::vector<uint32_t> values(decl->fields.size(), NO_REG);
		for (size_t i = 0; p_call && i < p_call->args.size(); i++) {
			for (size_t f = 0; f < decl->fields.size(); f++) {
				if (decl->fields[f].name == p_call->arg_names[i]) {
					values[f] = argument(*p_call->args[i], p_call->param_types[i], false, false);
				}
			}
		}
		for (size_t f = 0; f < decl->fields.size(); f++) {
			if (values[f] == NO_REG) {
				values[f] = argument(*decl->fields[f].initializer, decl->fields[f].type, false, false);
			}
		}
		for (size_t f = 0; f < decl->fields.size(); f++) {
			move(field_view(result, uint32_t(f)), values[f]);
		}
		return result;
	}

	bytecode::Slot slot(const std::string &p_name, const Type &p_type) {
		bytecode::Slot slot;
		slot.name = p_name;
		slot.type = type_index(p_type);
		return slot;
	}

	// The table entry of a constant expression's value (spec §4.5): what the
	// compiler knows, and what the engine computes when the class loads.
	bytecode::Constant constant_of(const ConstValue &p_value) {
		using Kind = ConstValue::Kind;
		bytecode::Constant constant;
		constant.type = type_index(p_value.type);
		switch (p_value.kind) {
			case Kind::Int:
				constant.i = p_value.i;
				constant.f = double(p_value.i);
				return constant;
			case Kind::Float:
				constant.f = p_value.f;
				return constant;
			case Kind::Bool:
				constant.b = p_value.b;
				return constant;
			case Kind::String:
			case Kind::Preload:
				constant.s = p_value.text;
				constant.load = p_value.kind == Kind::Preload;
				return constant;
			case Kind::None:
				return constant;
			case Kind::Builtin:
				constant.builtin = p_value.text;
				return constant;
			case Kind::Construct:
				constant.constructor = p_value.index;
				break;
			case Kind::Operator:
				constant.op = p_value.index;
				break;
			case Kind::Utility: {
				// A utility typed by its arguments takes them all as Variant (spec §10.3).
				const bool argument_typed = p_value.returns.kind == TypeKind::Variant && p_value.type.kind != TypeKind::Variant;
				const std::vector<ValueType> params = argument_typed ? std::vector<ValueType>(p_value.items.size(), ValueType::Variant) : value_types(p_value.params);
				constant.utility = int32_t(utility({ p_value.text, p_value.hash, p_value.b, params, value_type(p_value.returns), type_index(p_value.type) }));
				break;
			}
			case Kind::Field:
				constant.field = p_value.text;
				break;
			case Kind::Array:
			case Kind::Dictionary:
				break;
		}
		for (const ConstPtr &item : p_value.items) {
			constant.args.push_back(constant_of(*item));
		}
		return constant;
	}

	uint32_t shared_constant(const ConstValue &p_value) {
		const auto found = constant_entries.find(&p_value);
		if (found != constant_entries.end()) {
			return found->second;
		}
		const uint32_t entry = add_constant(constant_of(p_value));
		constant_entries.emplace(&p_value, entry);
		return entry;
	}

	// A constant's value in a new register: what the compiler knows as a
	// literal is, a preload where it is used, the rest from its table entry,
	// which the runtime computes when the class loads.
	uint32_t load_constant(const ConstPtr &p_value) {
		using Kind = ConstValue::Kind;
		if (p_value->kind == Kind::Preload) {
			return lower_expr(*p_value->source);
		}
		const uint32_t result = temp(p_value->type);
		const ValueType type = reg_type(result);
		const auto entry = [&]() { return p_value->is_known() ? add_constant(constant_of(*p_value)) : shared_constant(*p_value); };
		switch (type) {
			case ValueType::Bool:
				if (p_value->kind == Kind::Bool) {
					instr(Op::CONST_BOOL, result, NO_REG, NO_REG, p_value->b ? 1 : 0);
				} else {
					// Read as an int, bool having no constant instruction.
					const uint32_t number = load_constant(converted(p_value, Type::of(TypeKind::Int)));
					const uint32_t zero = temp(ValueType::Int);
					load_int(zero, 0);
					instr(Op::NE_INT, result, number, zero);
				}
				return result;
			case ValueType::Int:
				instr(Op::CONST_INT, result, NO_REG, NO_REG, entry());
				return result;
			case ValueType::Float:
				instr(Op::CONST_FLOAT, result, NO_REG, NO_REG, entry());
				return result;
			case ValueType::String:
				instr(Op::CONST_STRING, result, NO_REG, NO_REG, entry());
				return result;
			default:
				if (p_value->kind == Kind::None) {
					return default_value(result);
				}
				instr(Op::CONST_VALUE, result, NO_REG, NO_REG, entry(), fn.regs[result].type);
				return result;
		}
	}

	// Whether an expression is lowered as its value: a constant, or a scalar
	// the compiler computed, such as `1 << 4` (spec §4.5).
	static bool lowered_as_constant(const Expr &p_expr) {
		if (!p_expr.constant) {
			return false;
		}
		if (p_expr.target == Target::Constant || p_expr.target == Target::EngineConstant || p_expr.target == Target::BuiltinConstant) {
			return true;
		}
		return p_expr.kind != ExprKind::Name && p_expr.kind != ExprKind::Member && p_expr.constant->is_known();
	}

	void error(const std::string &p_message) {
		const TokenRange range = at.first < cls.source->tokens.size() ? at : TokenRange{ 0, 0 };
		errors.push_back({ cls.source->begin(range), cls.source->end(range), p_message });
	}

	// The line of a declaration's name, in the file of the class of the chain
	// that declares it.
	template <typename T>
	int decl_line(const T &p_decl, std::vector<T> ClassDecl::*p_own) const {
		for (const ClassDecl *owner = &cls; owner; owner = owner->base) {
			const std::vector<T> &own = owner->*p_own;
			if (std::any_of(own.begin(), own.end(), [&](const T &p_item) { return &p_item == &p_decl; })) {
				return owner->source->line(p_decl.name_token);
			}
		}
		return 0;
	}

	// What the checker passed but lowering has no code for is a defect of the
	// compiler: it is reported instead of compiled, and lowering goes on with
	// a register of its own.
	uint32_t internal_error(const std::string &p_what) {
		error("Internal compiler error: " + p_what + ".");
		return temp(ValueType::Variant);
	}

	void instr(Op p_op, uint32_t p_dst, uint32_t p_a = NO_REG, uint32_t p_b = NO_REG, uint32_t p_imm = 0, uint32_t p_type = 0) {
		ir::Instr i;
		i.op = p_op;
		i.type = p_type;
		i.imm = p_imm;
		i.dst = p_dst;
		i.a = p_a;
		i.b = p_b;
		add(i);
	}

	void move(uint32_t p_dst, uint32_t p_src) {
		if (p_dst == p_src) {
			return;
		}
		const ValueType type = reg_type(p_dst);
		if (is_8_byte(type)) {
			instr(Op::MOVE_8, p_dst, p_src);
		} else if (type == ValueType::String) {
			instr(Op::MOVE_STRING, p_dst, p_src);
		} else {
			instr(Op::MOVE_VALUE, p_dst, p_src, NO_REG, 0, fn.regs[p_dst].type);
		}
	}

	// The instruction of a family for how a value of this type is stored: in
	// 8 bytes, as a String, or as another value.
	static Op by_storage(ValueType p_type, Op p_8_byte, Op p_string, Op p_value) {
		return is_8_byte(p_type) ? p_8_byte : (p_type == ValueType::String ? p_string : p_value);
	}

	uint32_t load_member(uint32_t p_member) {
		const uint32_t result = temp(cls.all_members[p_member]->type);
		instr(by_storage(reg_type(result), Op::LOAD_MEMBER_8, Op::LOAD_MEMBER_STRING, Op::LOAD_MEMBER_VALUE), result, NO_REG, NO_REG, p_member, fn.regs[result].type);
		// An export the scene did not assign (spec §7.5), or an @onready
		// member read before the node is ready (§6.3), is still None.
		const VarDecl &member = *cls.all_members[p_member];
		if (member.must_assign || (member.is_onready && member.type.kind == TypeKind::Object && !member.type.nullable)) {
			instr(Op::CHECK_ASSIGNED, NO_REG, result, NO_REG, p_member);
		}
		return result;
	}

	void store_member(uint32_t p_member, uint32_t p_value) {
		instr(by_storage(reg_type(p_value), Op::STORE_MEMBER_8, Op::STORE_MEMBER_STRING, Op::STORE_MEMBER_VALUE), NO_REG, p_value, NO_REG, p_member, fn.regs[p_value].type);
	}

	// The setter or getter that reading or writing a member calls, or -1: in
	// its own accessors the member's name is its field (spec §4.4).
	int accessor(uint32_t p_member, bool p_setter) const {
		const VarDecl &member = *cls.all_members[p_member];
		const int index = p_setter ? member.setter_index : member.getter_index;
		return decl && decl->accessor_of == member.name ? -1 : index;
	}

	/* Static variables (spec §4.8) */

	static constexpr int NO_ACCESSOR = -2;

	// The class operand of a static variable's instructions.
	uint32_t static_class(const Place &p_place) {
		if (p_place.owner == cls.path) {
			return bytecode::NO_SLOT;
		}
		bytecode::ClassRef declaring; // Only its script matters.
		declaring.script = p_place.owner;
		return class_index(declaring);
	}

	// The setter or getter that using a static variable calls: its index in
	// this class's functions, -1 for one of another class, or NO_ACCESSOR when
	// it has none or this is it (spec §4.4).
	int static_accessor(const Place &p_place, bool p_setter) const {
		const VarDecl &variable = *p_place.variable;
		if ((p_setter ? variable.setter : variable.getter).empty() || (decl && decl->accessor_of == variable.name && p_place.owner == cls.path)) {
			return NO_ACCESSOR;
		}
		for (const ClassDecl *owner = &cls; owner; owner = owner->base) {
			if (owner->path == p_place.owner) {
				return p_setter ? variable.setter_index : variable.getter_index;
			}
		}
		return -1;
	}

	uint32_t call_static_accessor(const Place &p_place, bool p_setter, std::vector<uint32_t> p_args) {
		const VarDecl &variable = *p_place.variable;
		const Type result = p_setter ? Type::of(TypeKind::Void) : variable.type;
		const int index = static_accessor(p_place, p_setter);
		if (index >= 0) {
			return call_function(uint32_t(index), std::move(p_args), result);
		}
		// Another class's, called on its script.
		ir::Instr i;
		i.op = Op::CALL_METHOD;
		bytecode::MethodCall method;
		method.method = p_setter ? variable.setter : variable.getter;
		if (p_setter) {
			method.params.push_back(slot(std::string(), variable.type));
		}
		method.result = slot(std::string(), result);
		method.static_class = int32_t(static_class(p_place));
		i.imm = method_call(method);
		i.args = std::move(p_args);
		i.dst = p_setter ? NO_REG : temp(result);
		add(i);
		return i.dst;
	}

	uint32_t call_function(uint32_t p_function, std::vector<uint32_t> p_args, const Type &p_result) {
		ir::Instr i;
		i.op = Op::CALL;
		i.imm = p_function;
		i.args = std::move(p_args);
		i.dst = p_result.kind == TypeKind::Void ? NO_REG : temp(p_result);
		add(i);
		return i.dst;
	}

	/* Tables */

	template <typename T, typename Same>
	static uint32_t intern(std::vector<T> &r_table, const T &p_entry, Same p_same) {
		for (uint32_t i = 0; i < r_table.size(); i++) {
			if (p_same(r_table[i], p_entry)) {
				return i;
			}
		}
		r_table.push_back(p_entry);
		return uint32_t(r_table.size() - 1);
	}

	uint32_t add_constant(const bytecode::Constant &p_constant) {
		module.constants.push_back(p_constant);
		return uint32_t(module.constants.size() - 1);
	}

	// A StringName or NodePath constant with this text.
	uint32_t text_constant(ValueType p_type, const std::string &p_text) {
		bytecode::Constant constant;
		constant.type = type_index(p_type);
		constant.s = p_text;
		return add_constant(constant);
	}

	uint32_t engine_call(const std::string &p_method, uint32_t p_result_type, const std::string &p_static_class = std::string()) {
		return intern(module.engine_calls, bytecode::EngineCall{ p_method, p_result_type, p_static_class }, [](const auto &a, const auto &b) {
			return a.method == b.method && a.result_type == b.result_type && a.static_class == b.static_class;
		});
	}

	uint32_t method_call(const bytecode::MethodCall &p_call) {
		return intern(module.method_calls, p_call, [&](const auto &a, const auto &b) {
			return a.method == b.method && a.static_class == b.static_class && bytecode::signature_key(module.types, a.params, a.defaults, a.result, a.is_async) == bytecode::signature_key(module.types, b.params, b.defaults, b.result, b.is_async);
		});
	}

	uint32_t property(const std::string &p_name, uint32_t p_type) {
		return intern(module.properties, bytecode::PropertyAccess{ p_name, p_type }, [](const auto &a, const auto &b) {
			return a.name == b.name && a.type == b.type;
		});
	}

	uint32_t utility(const bytecode::UtilityCall &p_call) {
		return intern(module.utility_calls, p_call, [](const auto &a, const auto &b) {
			return a.function == b.function && a.params == b.params && a.result_type == b.result_type;
		});
	}

	uint32_t operator_call(uint8_t p_op, ValueType p_left, ValueType p_right, ValueType p_result) {
		return intern(module.operators, bytecode::OperatorCall{ p_op, p_left, p_right, p_result }, [](const auto &a, const auto &b) {
			return a.op == b.op && a.left == b.left && a.right == b.right && a.result == b.result;
		});
	}

	uint32_t field(ValueType p_base, const std::string &p_name, ValueType p_type) {
		return intern(module.fields, bytecode::FieldAccess{ p_base, p_name, p_type }, [](const auto &a, const auto &b) {
			return a.base == b.base && a.name == b.name;
		});
	}

	uint32_t constructor(ValueType p_type, uint32_t p_index, const std::vector<ValueType> &p_params) {
		return intern(module.constructors, bytecode::ConstructorCall{ p_type, p_index, p_params }, [](const auto &a, const auto &b) {
			return a.type == b.type && a.index == b.index;
		});
	}

	uint32_t builtin_method(const bytecode::BuiltinMethodCall &p_call) {
		return intern(module.builtin_methods, p_call, [](const auto &a, const auto &b) {
			return a.base == b.base && a.method == b.method && a.hash == b.hash && a.is_vararg == b.is_vararg && a.is_static == b.is_static && a.checks_elements == b.checks_elements && a.params == b.params && a.result_type == b.result_type;
		});
	}

	uint32_t class_index(const bytecode::ClassRef &p_ref) {
		return intern(module.classes, p_ref, [](const auto &a, const auto &b) { return a == b; });
	}

	uint32_t class_index(const std::string &p_engine_class) {
		bytecode::ClassRef ref;
		ref.name = p_engine_class;
		return class_index(ref);
	}

	// A class of this language is tested by its script, an interface by what
	// the object's class implements.
	uint32_t class_index(const Type &p_type) {
		return class_index(class_ref(p_type));
	}

	static std::vector<ValueType> value_types(const std::vector<Type> &p_types) {
		std::vector<ValueType> result;
		for (const Type &type : p_types) {
			result.push_back(value_type(type));
		}
		return result;
	}

	/* Conversions */

	// Converts int to float and integer vectors to float ones (spec §3.14), and
	// between String, StringName and NodePath (§10.7), where the target type
	// asks for it.
	uint32_t convert(uint32_t p_reg, const Type &p_to) {
		if (p_reg == NO_REG) {
			return p_reg;
		}
		const ValueType from = reg_type(p_reg);
		const ValueType to = value_type(p_to);
		if (p_to.kind == TypeKind::Option && !is_option_reg(p_reg)) {
			// Only a Variant goes into Variant?; another T? takes a Variant only
			// as the None literal.
			if (from == ValueType::Variant && p_to.args[0].kind != TypeKind::Variant) {
				return default_value(temp(p_to));
			}
			// T becomes Some(T) (spec §3.4).
			const uint32_t inner = convert(p_reg, p_to.args[0]);
			const uint32_t result = temp(p_to);
			instr(Op::SOME, result, inner, NO_REG, 0, fn.regs[result].type);
			return result;
		}
		if (from == to || to == ValueType::Void) {
			return p_reg;
		}
		if (to == ValueType::Variant) {
			return as_variant(p_reg); // A struct encoded (spec §3.13, §10.1).
		}
		if (from == ValueType::Int && to == ValueType::Float) {
			const uint32_t result = temp(ValueType::Float);
			instr(Op::INT_TO_FLOAT, result, p_reg);
			return result;
		}
		const builtin::Class *target = builtin::find_class(bytecode::value_type_name(to));
		for (uint32_t i = 0; target && i < target->constructor_count; i++) {
			const builtin::Constructor &candidate = target->constructors[i];
			if (candidate.argument_count == 1 && std::string(candidate.arguments[0].type) == bytecode::value_type_name(from)) {
				const uint32_t result = temp(to);
				ir::Instr i_construct;
				i_construct.op = Op::CONSTRUCT;
				i_construct.imm = constructor(to, i, { from });
				i_construct.dst = result;
				i_construct.args = { p_reg };
				add(i_construct);
				return result;
			}
		}
		return p_reg; // The checker has made sure the types fit.
	}

	static int option_depth(const Type &p_type) {
		return p_type.kind == TypeKind::Option ? 1 + option_depth(p_type.args[0]) : 0;
	}

	// The value as a `p_type`, wrapped in Some as many times as it takes (spec §5.2).
	uint32_t wrapped(uint32_t p_reg, const Type &p_type) {
		if (fn.regs[p_reg].type == type_index(p_type)) {
			return p_reg;
		}
		if (p_type.kind != TypeKind::Option) {
			return convert(p_reg, p_type);
		}
		const uint32_t inner = wrapped(p_reg, p_type.args[0]);
		const uint32_t result = temp(p_type);
		instr(Op::SOME, result, inner, NO_REG, 0, fn.regs[result].type);
		return result;
	}

	// A T? that goes where this language no longer follows it, to the engine
	// or into its storage, is read there, so a freed object gives None, as
	// every read of a T? does. A T must hold a live object where the engine
	// takes an object, which it uses without checking (spec §3.3).
	uint32_t outward(uint32_t p_reg, const Type &p_type, bool p_takes_object) {
		if (p_type.kind != TypeKind::Object) {
			return p_reg;
		}
		if (!p_type.nullable) {
			if (p_takes_object) {
				instr(Op::CHECK_LIVE, NO_REG, p_reg);
			}
			return p_reg;
		}
		const uint32_t result = temp(p_type);
		instr(Op::CAST_OBJECT, result, p_reg, NO_REG, class_index(p_type));
		return result;
	}

	uint32_t argument(const Expr &p_arg, const Type &p_param, bool p_outward, bool p_takes_object) {
		const uint32_t value = lower_expr(p_arg);
		if (p_outward && p_param.kind == TypeKind::Variant) {
			// The call makes the Variant from the slot and its type.
			return outward(value, p_arg.type, p_takes_object);
		}
		return convert(p_outward ? outward(value, p_arg.type, p_takes_object) : value, p_param);
	}

	std::vector<uint32_t> arguments(const Expr &p_call) {
		// A function of this language reads its T? parameters itself.
		const bool outward = p_call.target != Target::ScriptFunction && p_call.target != Target::ScriptMethod && p_call.target != Target::SuperMethod && p_call.target != Target::CallValue && p_call.target != Target::NewObject;
		// Callable.bind() and Signal.emit() take Variant, whatever the types here.
		const bool as_variant = p_call.target == Target::BindArguments || p_call.target == Target::EmitSignal;
		std::vector<uint32_t> args;
		for (size_t i = 0; i < p_call.args.size(); i++) {
			const Type &param = p_call.param_types[i];
			args.push_back(argument(*p_call.args[i], param, outward, !as_variant && param.kind == TypeKind::Object));
			// A key or an element of a container that keeps it boxed.
			if (p_call.target == Target::BuiltinMethod && i < p_call.engine_params.size() && p_call.engine_params[i].kind == TypeKind::Variant && is_boxed(param)) {
				const bytecode::ContainerMethod *m = p_call.owner == "Dictionary" ? bytecode::find_container_method(ValueType::Dictionary, p_call.text) : nullptr;
				args.back() = box(args.back(), m && i < 2 && m->args[i] == bytecode::ElementRole::Key);
			}
		}
		return args;
	}

	// A constant of the type that has only its default value, such as 0 or None.
	uint32_t default_constant(uint32_t p_type) {
		return add_constant({ p_type });
	}

	void load_int(uint32_t p_dst, int64_t p_value) {
		bytecode::Constant constant{ type_index(ValueType::Int) };
		constant.i = p_value;
		instr(Op::CONST_INT, p_dst, NO_REG, NO_REG, add_constant(constant));
	}

	// What the engine declares as an object or a Variant, typed T here: a T
	// that may not be None is checked to be one (spec §3.8, §10.2).
	void check_object(uint32_t p_reg, const Type &p_type) {
		if (p_reg != NO_REG && p_type.kind == TypeKind::Object && !p_type.nullable) {
			instr(Op::CHECK_OBJECT, NO_REG, p_reg, NO_REG, class_index(p_type));
		}
	}

	/* Containers */

	void new_container(uint32_t p_dst) {
		instr(Op::NEW_CONTAINER, p_dst, NO_REG, NO_REG, 0, fn.regs[p_dst].type);
	}

	bool is_typed_container_reg(uint32_t p_reg) const {
		const bytecode::TypeKind kind = module.types[fn.regs[p_reg].type].kind;
		return kind == bytecode::TypeKind::Array || kind == bytecode::TypeKind::Dictionary;
	}

	// A method of a built-in type by name, as the engine declares it.
	// `p_result_type` is the slot's type, such as Array[K] for keys() of a
	// Dictionary[K, V]; by default the return type.
	uint32_t engine_builtin_method(ValueType p_base, const char *p_method, std::vector<ValueType> p_params, ValueType p_return, bool p_vararg = false, uint32_t p_result_type = NO_REG, bool p_checks_elements = false) {
		const builtin::Class *type = builtin::find_class(bytecode::value_type_name(p_base));
		const builtin::Method *method = type ? builtin::find_method(*type, p_method) : nullptr;
		bytecode::BuiltinMethodCall call;
		call.base = p_base;
		call.method = p_method;
		call.hash = method ? method->hash : 0;
		call.is_vararg = p_vararg;
		call.is_const = !method || method->is_const;
		call.params = std::move(p_params);
		call.return_type = p_return;
		call.result_type = p_result_type == NO_REG ? type_index(p_return) : p_result_type;
		call.checks_elements = p_checks_elements;
		return builtin_method(call);
	}

	uint32_t call_builtin(uint32_t p_method, uint32_t p_base, std::vector<uint32_t> p_args, uint32_t p_dst = NO_REG) {
		ir::Instr i;
		i.op = Op::CALL_BUILTIN;
		i.imm = p_method;
		i.base = p_base;
		i.args = std::move(p_args);
		i.dst = p_dst;
		add(i);
		return p_dst;
	}

	uint32_t size_of(uint32_t p_container) {
		return call_builtin(engine_builtin_method(reg_type(p_container), "size", {}, ValueType::Int), p_container, {}, temp(ValueType::Int));
	}

	// An element is checked as it is read: the engine's arrays can hold null
	// where T is not nullable (spec §3.8).
	uint32_t get_index(uint32_t p_container, uint32_t p_index, const Type &p_element) {
		const uint32_t result = temp(p_element);
		instr(Op::GET_INDEX, result, p_container, p_index);
		check_object(result, p_element);
		return result;
	}

	uint32_t array_literal(const Expr &p_expr) {
		const uint32_t result = temp(p_expr.type);
		new_container(result);
		const uint32_t append = engine_builtin_method(ValueType::Array, "append", { ValueType::Variant }, ValueType::Void);
		for (size_t e = 0; e < p_expr.args.size(); e++) {
			const Type &element = p_expr.param_types[e];
			const uint32_t value = argument(*p_expr.args[e], element, true, element.kind == TypeKind::Object);
			call_builtin(append, result, { is_boxed(element) ? box(value, false) : value });
		}
		return result;
	}

	uint32_t dictionary_literal(const Expr &p_expr) {
		const uint32_t result = temp(p_expr.type);
		new_container(result);
		const auto [key_type, value_type] = *entries_of(p_expr.type);
		for (size_t e = 0; e + 1 < p_expr.args.size(); e += 2) {
			const uint32_t key = stored(*p_expr.args[e], p_expr.param_types[e], key_type, true);
			instr(Op::SET_INDEX, result, key, stored(*p_expr.args[e + 1], value_type, value_type, false));
		}
		return result;
	}

	// A value as a container keeps it, as a key or an element: of the type
	// `p_as` is converted to, boxed when the container keeps it boxed, or a
	// Variant in a container without types (spec §10.1). The engine keeps it,
	// so a T must hold a live object, and a T? whose object was freed is None
	// (§3.3).
	uint32_t stored(const Expr &p_value, const Type &p_as, const Type &p_element, bool p_key) {
		const uint32_t value = argument(p_value, p_as, true, true);
		if (p_element.kind == TypeKind::Variant) {
			return as_variant(value);
		}
		const uint32_t converted = convert(value, p_element);
		return is_boxed(p_element) ? box(converted, p_key) : converted;
	}

	// A value of a boxed type as a container keeps it (architecture §2.2); a
	// key's box hashes the value.
	uint32_t box(uint32_t p_reg, bool p_key) {
		const uint32_t result = temp(ValueType::Variant);
		instr(Op::BOX, result, p_reg, NO_REG, p_key ? 1 : 0, fn.regs[p_reg].type);
		return result;
	}

	// resize() of an array whose elements' default the engine does not know,
	// boxed or a typed container: the engine's, then each new element set to
	// its own default value (spec §3.8).
	uint32_t resize(const Expr &p_call) {
		const uint32_t array = lower_expr(*p_call.base);
		const Type &element = p_call.base->type.args[0];
		const uint32_t size = argument(*p_call.args[0], Type::of(TypeKind::Int), false, false);
		const uint32_t index = temp(ValueType::Int);
		move(index, size_of(array));
		const uint32_t result = call_builtin(engine_builtin_method(ValueType::Array, "resize", { ValueType::Int }, ValueType::Int), array, { size }, temp(ValueType::Int));
		const uint32_t one = temp(ValueType::Int);
		load_int(one, 1);
		const uint32_t condition = new_block();
		const uint32_t body = new_block();
		const uint32_t end = new_block();
		jump(condition);
		current = condition;
		const uint32_t more = temp(ValueType::Bool);
		instr(Op::LT_INT, more, index, size);
		branch(more, body, end);
		current = body;
		const uint32_t value = default_value(temp(element));
		instr(Op::SET_INDEX, array, index, is_boxed(element) ? box(value, false) : value);
		instr(Op::ADD_INT, index, index, one);
		jump(condition);
		current = end;
		return result;
	}

	// `text % value` (spec §5.1): the value enters the engine as an argument
	// does (§10.1).
	uint32_t format(uint32_t p_text, uint32_t p_value, const Type &p_type) {
		const uint32_t text = convert(p_text, Type::of(TypeKind::String));
		const uint32_t value = as_variant(outward(p_value, p_type, false));
		const uint32_t result = temp(ValueType::String);
		instr(Op::FORMAT, result, text, value);
		return result;
	}

	// The value of a Variant known to hold a T (spec §5.4).
	uint32_t from_variant(uint32_t p_variant, const Type &p_type) {
		const uint32_t result = temp(p_type);
		instr(Op::FROM_VARIANT, result, p_variant, NO_REG, 0, fn.regs[result].type);
		return result;
	}

	// The object a Variant holds, as a T? to test or convert; None for any
	// other value.
	uint32_t variant_object(uint32_t p_variant, const Type &p_class) {
		const uint32_t result = temp(optional_of(p_class));
		instr(Op::VARIANT_OBJECT, result, p_variant);
		return result;
	}

	uint32_t as_variant(uint32_t p_reg) {
		if (reg_type(p_reg) == ValueType::Variant) {
			return p_reg;
		}
		const uint32_t result = temp(ValueType::Variant);
		instr(Op::TO_VARIANT, result, p_reg, NO_REG, 0, fn.regs[p_reg].type);
		return result;
	}

	// The index of `a[i]`, or the key of `d[k]`.
	uint32_t index_of(const Expr &p_subscript) {
		if (p_subscript.param_types.empty()) {
			return lower_expr(*p_subscript.right);
		}
		return stored(*p_subscript.right, p_subscript.param_types[0], p_subscript.param_types[0], true);
	}

	// `x in c` (spec §5.1): c.has(x) or c.contains(x), x as an element or key.
	uint32_t contains(const Expr &p_expr) {
		const Type &element = p_expr.param_types[0];
		const uint32_t value = stored(*p_expr.left, element, element, p_expr.owner == "Dictionary");
		const uint32_t container = lower_expr(*p_expr.right);
		const uint32_t method = engine_builtin_method(reg_type(container), p_expr.text.c_str(), value_types(p_expr.engine_params), ValueType::Bool, false, NO_REG, p_expr.checks_elements);
		return call_builtin(method, container, { value }, temp(ValueType::Bool));
	}

	// get(k) of Dictionary[K, V]: V?, None for a missing key (spec §3.8).
	uint32_t get_or_none(const Expr &p_call) {
		const uint32_t dictionary = lower_expr(*p_call.base);
		const auto [key_type, value_type] = *entries_of(p_call.base->type);
		const uint32_t key = stored(*p_call.args[0], key_type, key_type, true);
		const bool untyped = p_call.base->type.args.empty();
		const uint32_t has = call_builtin(engine_builtin_method(ValueType::Dictionary, "has", { ValueType::Variant }, ValueType::Bool, false, NO_REG, untyped), dictionary, { key }, temp(ValueType::Bool));
		const uint32_t result = temp(p_call.type);
		const uint32_t found = new_block();
		const uint32_t missing = new_block();
		const uint32_t end = new_block();
		branch(has, found, missing);
		current = found;
		move(result, convert(get_index(dictionary, key, value_type), p_call.type));
		jump(end);
		current = missing;
		default_value(result);
		jump(end);
		current = end;
		return result;
	}

	// pop_front() and pop_back() of an untyped Array: Variant?, None only for
	// an empty one, since the engine gives null for an empty array and for a
	// null element (spec §3.4, §3.8).
	uint32_t pop_or_none(const Expr &p_call) {
		const uint32_t array = lower_expr(*p_call.base);
		const uint32_t empty = call_builtin(engine_builtin_method(ValueType::Array, "is_empty", {}, ValueType::Bool), array, {}, temp(ValueType::Bool));
		const uint32_t result = temp(p_call.type);
		const uint32_t found = new_block();
		const uint32_t missing = new_block();
		const uint32_t end = new_block();
		branch(empty, missing, found);
		current = found;
		const uint32_t value = call_builtin(engine_builtin_method(ValueType::Array, p_call.text.c_str(), {}, ValueType::Variant), array, {}, temp(ValueType::Variant));
		instr(Op::SOME, result, value, NO_REG, 0, fn.regs[result].type);
		jump(end);
		current = missing;
		default_value(result);
		jump(end);
		current = end;
		return result;
	}

	// `Class.new(...)` of a class whose _init() takes parameters: the
	// arguments, the object with its initializers run, then its _init()
	// (spec §6.1).
	uint32_t new_with_arguments(const Expr &p_call) {
		ir::Instr i;
		i.args = arguments(p_call);
		i.base = temp(p_call.type);
		instr(Op::NEW_WITHOUT_INIT, i.base, NO_REG, NO_REG, class_index(p_call.type));
		i.op = Op::CALL_METHOD;
		bytecode::MethodCall method;
		method.method = "_init";
		for (const Type &param : p_call.engine_params) {
			method.params.push_back(slot(std::string(), param));
		}
		method.defaults = uint32_t(p_call.index);
		method.result = slot(std::string(), Type::of(TypeKind::Void));
		i.imm = method_call(method);
		add(i);
		return i.base;
	}

	// The keys of a Dictionary[K, V], as an Array[K] (spec §5.7).
	uint32_t keys_of(uint32_t p_dictionary, const Type &p_key) {
		const uint32_t keys = temp(Type::array(p_key));
		return call_builtin(engine_builtin_method(ValueType::Dictionary, "keys", {}, ValueType::Array, false, fn.regs[keys].type), p_dictionary, {}, keys);
	}

	/* Expressions */

	uint32_t default_value(uint32_t p_dst) {
		const ValueType type = reg_type(p_dst);
		if (is_typed_container_reg(p_dst)) {
			// An empty Array[T] or Dictionary[K, V] is typed too.
			new_container(p_dst);
			return p_dst;
		}
		if (is_option_reg(p_dst) || type == ValueType::Object || type == ValueType::Variant || module.types[fn.regs[p_dst].type].kind == bytecode::TypeKind::PayloadEnum) {
			// None; a constant of an object type or of T? is always None. An enum
			// with payloads has no default (spec §3.7); where one is needed, its
			// first variant.
			instr(Op::CONST_VALUE, p_dst, NO_REG, NO_REG, default_constant(fn.regs[p_dst].type), fn.regs[p_dst].type);
			return p_dst;
		}
		if (is_struct_reg(p_dst)) {
			// Each field's default (spec §4.3).
			move(p_dst, construct_struct(struct_types.at(fn.regs[p_dst].type), nullptr));
			return p_dst;
		}
		switch (type) {
			case ValueType::Int:
				load_int(p_dst, 0);
				break;
			case ValueType::Float:
				instr(Op::CONST_FLOAT, p_dst, NO_REG, NO_REG, default_constant(fn.regs[p_dst].type));
				break;
			case ValueType::Bool:
				instr(Op::CONST_BOOL, p_dst, NO_REG, NO_REG, 0);
				break;
			case ValueType::String:
				instr(Op::CONST_STRING, p_dst, NO_REG, NO_REG, default_constant(fn.regs[p_dst].type));
				break;
			case ValueType::Void:
				break;
			default: {
				// The engine's constructor without arguments is the first.
				ir::Instr i;
				i.op = Op::CONSTRUCT;
				i.imm = constructor(type, 0, {});
				i.dst = p_dst;
				add(i);
				break;
			}
		}
		return p_dst;
	}

	uint32_t binary(TokenKind p_op, uint32_t p_left, uint32_t p_right, const Type &p_result) {
		const bool ordering = p_op == TokenKind::Less || p_op == TokenKind::LessEqual || p_op == TokenKind::Greater || p_op == TokenKind::GreaterEqual;
		if (ordering && (is_struct_reg(p_left) || is_option_reg(p_left) || bytecode::is_boxed(module.types, fn.regs[p_left].type))) {
			// `a > b` is `b < a`, `a <= b` is `a < b or a == b` (spec §5.2).
			const bool swapped = p_op == TokenKind::Greater || p_op == TokenKind::GreaterEqual;
			const uint32_t result = temp(ValueType::Bool);
			instr(Op::LESS, result, swapped ? p_right : p_left, swapped ? p_left : p_right, 0, fn.regs[p_left].type);
			if (p_op == TokenKind::LessEqual || p_op == TokenKind::GreaterEqual) {
				const uint32_t equal = new_block();
				const uint32_t end = new_block();
				branch(result, end, equal);
				current = equal;
				instr(Op::EQUAL, result, p_left, p_right, 0, fn.regs[p_left].type);
				jump(end);
				current = end;
			}
			return result;
		}
		if (is_struct_reg(p_left) || is_option_reg(p_left)) {
			// Field by field (spec §3.6), None equal to None (§5.2).
			const uint32_t result = temp(ValueType::Bool);
			instr(Op::EQUAL, result, p_left, p_right, 0, fn.regs[p_left].type);
			return p_op == TokenKind::BangEqual ? negate(result) : result;
		}
		const ValueType left_type = reg_type(p_left);
		const ValueType right_type = reg_type(p_right);
		if (!is_primitive(left_type) || !is_primitive(right_type)) {
			// The engine's operator for these two types (spec §5.1).
			const uint8_t op = engine_operator(p_op);
			if (op == OP_UNKNOWN) {
				return internal_error("no engine operator for this binary operator");
			}
			const uint32_t result = temp(p_result);
			instr(Op::EVALUATE, result, p_left, p_right, operator_call(op, left_type, right_type, value_type(p_result)));
			return result;
		}
		ValueType operand = left_type;
		if (left_type == ValueType::Float || right_type == ValueType::Float) {
			operand = ValueType::Float;
			p_left = convert(p_left, Type::of(TypeKind::Float));
			p_right = convert(p_right, Type::of(TypeKind::Float));
		}
		const bool f = operand == ValueType::Float;
		Op op;
		switch (p_op) {
			case TokenKind::Plus:
				op = operand == ValueType::String ? Op::ADD_STRING : (f ? Op::ADD_FLOAT : Op::ADD_INT);
				break;
			case TokenKind::Minus:
				op = f ? Op::SUB_FLOAT : Op::SUB_INT;
				break;
			case TokenKind::Star:
				op = f ? Op::MUL_FLOAT : Op::MUL_INT;
				break;
			case TokenKind::Slash:
				op = f ? Op::DIV_FLOAT : Op::DIV_INT;
				break;
			case TokenKind::StarStar:
				op = f ? Op::POW_FLOAT : Op::POW_INT;
				break;
			case TokenKind::Percent:
				op = Op::MOD_INT;
				break;
			case TokenKind::Ampersand:
				op = Op::AND_INT;
				break;
			case TokenKind::Pipe:
				op = Op::OR_INT;
				break;
			case TokenKind::Caret:
				op = Op::XOR_INT;
				break;
			case TokenKind::ShiftLeft:
				op = Op::SHL_INT;
				break;
			case TokenKind::ShiftRight:
				op = Op::SHR_INT;
				break;
			case TokenKind::EqualEqual:
				op = operand == ValueType::String ? Op::EQ_STRING : (operand == ValueType::Bool ? Op::EQ_BOOL : (f ? Op::EQ_FLOAT : Op::EQ_INT));
				break;
			case TokenKind::BangEqual:
				op = operand == ValueType::String ? Op::NE_STRING : (operand == ValueType::Bool ? Op::NE_BOOL : (f ? Op::NE_FLOAT : Op::NE_INT));
				break;
			case TokenKind::Less:
				op = f ? Op::LT_FLOAT : Op::LT_INT;
				break;
			case TokenKind::LessEqual:
				op = f ? Op::LE_FLOAT : Op::LE_INT;
				break;
			case TokenKind::Greater:
				op = f ? Op::GT_FLOAT : Op::GT_INT;
				break;
			case TokenKind::GreaterEqual:
				op = f ? Op::GE_FLOAT : Op::GE_INT;
				break;
			default:
				return internal_error("no instruction for this binary operator");
		}
		const uint32_t result = temp(p_result);
		instr(op, result, p_left, p_right);
		return result;
	}

	uint32_t negate(uint32_t p_bool) {
		const uint32_t result = temp(ValueType::Bool);
		instr(Op::NOT, result, p_bool);
		return result;
	}

	uint32_t short_circuit(const Expr &p_expr) {
		const bool is_and = p_expr.op == TokenKind::And;
		const uint32_t result = temp(ValueType::Bool);
		move(result, lower_expr(*p_expr.left));
		const uint32_t right_block = new_block();
		const uint32_t end_block = new_block();
		if (is_and) {
			branch(result, right_block, end_block);
		} else {
			branch(result, end_block, right_block);
		}
		current = right_block;
		move(result, lower_expr(*p_expr.right));
		jump(end_block);
		current = end_block;
		return result;
	}

	uint32_t call(const Expr &p_call) {
		if (p_call.target == Target::Noop) {
			return NO_REG;
		}
		if (p_call.target == Target::BuiltinMethod && p_call.owner == "Dictionary" && p_call.text == "get" && p_call.args.size() == 1 && p_call.type.is_optional()) {
			return get_or_none(p_call);
		}
		if (p_call.target == Target::BuiltinMethod && p_call.text == "resize" && is_typed_array(p_call.base->type) && (is_boxed(p_call.base->type) || is_typed_container(p_call.base->type.args[0]))) {
			return resize(p_call);
		}
		if (p_call.target == Target::BuiltinMethod && p_call.owner == "Array" && (p_call.text == "pop_front" || p_call.text == "pop_back") && p_call.base->type.args.empty()) {
			return pop_or_none(p_call);
		}
		switch (p_call.target) {
			case Target::Encode: {
				const uint32_t value = lower_expr(*p_call.args[0]);
				const uint32_t result = temp(p_call.type);
				instr(Op::ENCODE, result, value, NO_REG, 0, fn.regs[value].type);
				return result;
			}
			case Target::ConvertToInt: {
				const uint32_t arg = lower_expr(*p_call.args[0]);
				if (reg_type(arg) == ValueType::Int) {
					return arg;
				}
				const uint32_t result = temp(ValueType::Int);
				instr(Op::FLOAT_TO_INT, result, arg);
				return result;
			}
			case Target::ConvertToFloat:
				return convert(lower_expr(*p_call.args[0]), Type::of(TypeKind::Float));
			case Target::ConstructFromVariant: {
				const uint32_t value = lower_expr(*p_call.args[0]);
				const uint32_t result = temp(p_call.type);
				instr(Op::CONSTRUCT_FROM, result, value, NO_REG, 0, fn.regs[result].type);
				return result;
			}
			case Target::EnumFromInt: {
				// One of the values, or a program error; any int for an enum of the
				// engine, which is open (spec §3.7).
				const uint32_t result = temp(p_call.type);
				move(result, lower_expr(*p_call.args[0]));
				if (!p_call.type.values || !p_call.type.values->is_engine) {
					instr(Op::CHECK_ENUM, NO_REG, result, NO_REG, fn.regs[result].type);
				}
				return result;
			}
			case Target::NewObject: {
				if (!p_call.engine_params.empty()) {
					return new_with_arguments(p_call);
				}
				const uint32_t result = temp(p_call.type);
				instr(Op::NEW_OBJECT, result, NO_REG, NO_REG, class_index(p_call.type));
				return result;
			}
			case Target::StructConstructor:
				return construct_struct(p_call.type, &p_call);
			case Target::VariantConstructor:
				return construct_variant(p_call);
			case Target::Unwrap: {
				const uint32_t value = lower_expr(*p_call.base);
				const uint32_t message = p_call.args.empty() ? NO_REG : convert(lower_expr(*p_call.args[0]), Type::of(TypeKind::String));
				const uint32_t result = temp(p_call.type);
				instr(Op::UNWRAP, result, value, message, 0, fn.regs[result].type);
				return result;
			}
			case Target::OptionMethod:
				return option_method(p_call);
			case Target::ResultMethod:
				return result_method(p_call);
			case Target::TryFrom: {
				const uint32_t value = convert(lower_expr(*p_call.args[0]), Type::of(TypeKind::Variant));
				const uint32_t result = temp(p_call.type);
				instr(Op::TRY_FROM, result, value, NO_REG, 0, fn.regs[result].type);
				return result;
			}
			default:
				break;
		}

		if (p_call.target == Target::TaskCancel) {
			instr(Op::TASK_CANCEL, NO_REG, lower_expr(*p_call.base));
			return NO_REG;
		}
		if (p_call.target == Target::TaskDetach) {
			// Only says that the task is not awaited (spec §9.1); it runs either way.
			lower_expr(*p_call.base);
			return NO_REG;
		}
		ir::Instr i;
		std::unique_ptr<Place> written_back;
		// The base before the arguments, as it reads in the source.
		if (p_call.is_static) {
			// The base names the class or type, or is a value whose static
			// method this is (spec §4.8).
			if (p_call.static_on_value) {
				lower_expr(*p_call.base);
			}
		} else if ((p_call.target == Target::EngineMethod && p_call.base) || p_call.target == Target::ScriptMethod || p_call.target == Target::BindArguments || p_call.target == Target::EmitSignal || p_call.target == Target::CallValue) {
			i.base = lower_expr(*p_call.base);
		} else if (p_call.target == Target::BuiltinMethod) {
			if (!p_call.is_const && !is_shared(p_call.base->type) && is_place(*p_call.base)) {
				// A method that changes its base changes the place it came from.
				written_back = std::make_unique<Place>(place_of(*p_call.base));
				open(*written_back);
				i.base = load(*written_back);
			} else {
				i.base = lower_expr(*p_call.base);
			}
		}
		i.args = arguments(p_call);
		// A value the engine would truncate or wrap is a program error (spec §10.6).
		for (size_t a = 0; a < p_call.arg_ranges.size() && a < i.args.size(); a++) {
			if (p_call.arg_ranges[a]) {
				instr(Op::CHECK_RANGE, NO_REG, i.args[a], NO_REG, uint32_t(*p_call.arg_ranges[a]));
			}
		}
		if (p_call.target == Target::BuiltinMethod && p_call.text == "pop_at" && reg_type(i.base) == ValueType::Array) {
			// An index out of bounds is a program error, as for a[i] (spec §3.8),
			// where the engine returns null.
			instr(Op::GET_INDEX, temp(ValueType::Variant), i.base, i.args[0]);
		}

		// An array of the class an argument named (spec §10.3).
		const bool filtered = p_call.target == Target::EngineMethod && p_call.engine_return.builtin_name() == "Array" && p_call.type != p_call.engine_return;
		i.dst = p_call.type.kind == TypeKind::Void ? NO_REG : temp(filtered ? p_call.engine_return : p_call.type);
		switch (p_call.target) {
			case Target::ScriptFunction:
				i.op = Op::CALL;
				i.imm = uint32_t(p_call.index);
				break;
			case Target::SuperMethod:
			case Target::ScriptMethod: {
				i.op = p_call.target == Target::SuperMethod ? Op::CALL_SUPER : Op::CALL_METHOD;
				bytecode::MethodCall method;
				method.method = p_call.text;
				for (const Type &param : p_call.engine_params) {
					method.params.push_back(slot(std::string(), param));
				}
				method.defaults = uint32_t(p_call.index);
				method.is_async = p_call.is_async;
				method.result = slot(std::string(), p_call.is_async ? p_call.type.args[0] : p_call.type);
				if (p_call.is_static) {
					bytecode::ClassRef declaring; // Only its script matters.
					declaring.script = p_call.owner;
					method.static_class = int32_t(class_index(declaring));
				}
				i.imm = method_call(method);
				break;
			}
			case Target::EngineMethod:
				i.op = Op::CALL_ENGINE;
				i.imm = engine_call(p_call.text, type_index(filtered ? p_call.engine_return : p_call.type), p_call.is_static ? p_call.owner : std::string());
				break;
			case Target::Utility: {
				i.op = Op::CALL_UTILITY;
				bytecode::UtilityCall utility_call;
				utility_call.function = p_call.text;
				utility_call.hash = p_call.hash;
				utility_call.is_vararg = p_call.is_vararg;
				// A utility typed by its arguments takes them all as Variant (spec §10.3).
				const bool argument_typed = p_call.engine_return.kind == TypeKind::Variant && p_call.type.kind != TypeKind::Variant;
				utility_call.params = argument_typed ? std::vector<ValueType>(p_call.args.size(), ValueType::Variant) : value_types(p_call.param_types);
				utility_call.return_type = value_type(p_call.engine_return);
				utility_call.result_type = type_index(p_call.type);
				i.imm = utility(utility_call);
				break;
			}
			case Target::BuiltinMethod: {
				i.op = Op::CALL_BUILTIN;
				bytecode::BuiltinMethodCall method;
				method.base = p_call.is_static ? value_type(api_type(p_call.owner, false)) : reg_type(i.base);
				method.method = p_call.text;
				method.hash = p_call.hash;
				method.is_vararg = p_call.is_vararg;
				method.is_const = p_call.is_const;
				method.is_static = p_call.is_static;
				method.checks_elements = p_call.checks_elements;
				method.params = value_types(p_call.engine_params);
				method.return_type = value_type(p_call.engine_return);
				method.result_type = type_index(p_call.type);
				i.imm = builtin_method(method);
				break;
			}
			case Target::CallValue: {
				// With the signature it is called with (spec §3.11).
				i.op = Op::CALL_VALUE;
				bytecode::MethodCall method;
				method.method = p_call.text;
				for (const Type &param : p_call.param_types) {
					method.params.push_back(slot(std::string(), param));
				}
				method.result = slot(std::string(), p_call.type);
				i.imm = method_call(method);
				break;
			}
			case Target::BindArguments:
			case Target::EmitSignal: {
				// Callable.bind() and Signal.emit() take their arguments as Variant.
				const bool bind = p_call.target == Target::BindArguments;
				i.op = Op::CALL_BUILTIN;
				i.imm = engine_builtin_method(bind ? ValueType::Callable : ValueType::Signal, bind ? "bind" : "emit", {}, bind ? ValueType::Callable : ValueType::Void, true);
				break;
			}
			case Target::BuiltinConstructor:
				i.op = Op::CONSTRUCT;
				i.imm = constructor(value_type(p_call.type), uint32_t(p_call.index), value_types(p_call.param_types));
				break;
			default:
				return internal_error("no instruction for this call");
		}
		if (p_call.discarded && p_call.target == Target::EngineMethod) {
			i.dst = NO_REG;
		}
		if (p_call.awaited && p_call.target == Target::EngineMethod) {
			i.op = Op::CALL_AWAIT_ENGINE;
		}
		if (p_call.awaited && p_call.target == Target::BuiltinMethod && p_call.base->type.builtin_name() == "Callable" && (p_call.text == "call" || p_call.text == "callv")) {
			i.op = Op::CALL_AWAIT_BUILTIN;
		}
		add(i);
		if (written_back) {
			store(*written_back, i.base);
		}
		if (filtered) {
			// The engine made a new array, so a new array typed by that class
			// with the same elements is not told apart from it.
			const uint32_t typed = temp(p_call.type);
			new_container(typed);
			call_builtin(engine_builtin_method(ValueType::Array, "assign", { ValueType::Array }, ValueType::Void), typed, { i.dst });
			return typed;
		}
		if (p_call.target == Target::BuiltinMethod || p_call.target == Target::EngineMethod) {
			// Such as an element of Array[T], or the root of PackedScene[T].
			check_object(i.dst, p_call.type);
		}
		return i.dst;
	}

	// A lambda (spec §3.11): its function, lowered here, and the Callable that
	// runs it with the values it captures, read now.
	uint32_t make_lambda(const Expr &p_expr) {
		const FuncDecl &lambda = *p_expr.function;
		ir::Instr i;
		i.op = Op::MAKE_LAMBDA;
		for (const auto &value : p_expr.args) {
			i.args.push_back(lower_expr(*value));
		}
		i.dst = temp(p_expr.type);
		const size_t own = lambdas.size();
		i.imm = uint32_t(cls.all_functions.size() + own);
		add(i);

		lambdas.emplace_back();
		ir::Function outer_fn = std::move(fn);
		const FuncDecl *outer_decl = decl;
		const uint32_t outer_current = current;
		const TokenRange outer_at = at;
		std::vector<Loop> outer_loops = std::move(loops);
		std::vector<uint32_t> outer_regs = std::move(local_regs);

		start(lambda.name.empty() ? "<lambda>" : lambda.name);
		decl = &lambda;
		at = lambda.range;
		fn.param_count = uint32_t(lambda.params.size() + lambda.captures.size());
		fn.result = slot(std::string(), lambda.return_type);
		fn.line = uint32_t(cls.source->line(lambda.name_token));
		// The parameters, then the captures, then the other locals.
		std::vector<int> order;
		for (size_t p = 0; p < lambda.params.size(); p++) {
			order.push_back(int(p));
		}
		order.insert(order.end(), lambda.captures.begin(), lambda.captures.end());
		for (size_t l = lambda.params.size(); l < lambda.locals.size(); l++) {
			if (std::find(lambda.captures.begin(), lambda.captures.end(), int(l)) == lambda.captures.end()) {
				order.push_back(int(l));
			}
		}
		local_regs.assign(lambda.locals.size(), NO_REG);
		for (int local : order) {
			local_regs[local] = uint32_t(fn.regs.size());
			fn.regs.push_back(slot(lambda.locals[local].name, lambda.locals[local].type));
		}
		lower_block(lambda.body);
		finish();
		bytecode::Function out = ir::emit(fn, module);
		out.is_lambda = true;
		out.captures = uint32_t(lambda.captures.size());
		out.is_static = !lambda.uses_self;
		lambdas[own] = std::move(out);

		fn = std::move(outer_fn);
		decl = outer_decl;
		current = outer_current;
		at = outer_at;
		loops = std::move(outer_loops);
		local_regs = std::move(outer_regs);
		return i.dst;
	}

	uint32_t lower_expr(const Expr &p_expr) {
		if (lowered_as_constant(p_expr)) {
			if (p_expr.kind == ExprKind::Member && p_expr.base && p_expr.base->kind == ExprKind::Call) {
				lower_expr(*p_expr.base); // For what the call does; the constant is the class's.
			}
			// A constant of T? keeps the value inside Some (converted()).
			const uint32_t value = load_constant(p_expr.constant);
			return p_expr.type.kind == TypeKind::Option ? wrapped(value, p_expr.type) : value;
		}
		switch (p_expr.kind) {
			case ExprKind::Lambda:
				return make_lambda(p_expr);
			case ExprKind::Try:
				return try_value(p_expr);
			case ExprKind::Await: {
				const Expr &operand = *p_expr.left;
				const uint32_t src = lower_expr(operand);
				if (operand.kind == ExprKind::Call && operand.type.kind != TypeKind::Task && operand.type.builtin_name() != "Signal") {
					return src;
				}
				const uint32_t dst = p_expr.type.kind == TypeKind::Void || p_expr.discarded ? NO_REG : temp(p_expr.type);
				instr(operand.type.kind == TypeKind::Task ? Op::AWAIT_TASK : Op::AWAIT_SIGNAL, dst, src, NO_REG, operand.kind == ExprKind::Call && operand.is_async ? 1 : 0);
				return dst;
			}
			case ExprKind::Name:
			case ExprKind::Member: {
				if (p_expr.target == Target::Autoload) {
					const uint32_t result = temp(p_expr.type);
					instr(Op::GET_AUTOLOAD, result, NO_REG, NO_REG, text_constant(ValueType::StringName, p_expr.text));
					return result;
				}
				if (p_expr.target == Target::ClassScript) {
					bytecode::ClassRef script; // Only its script matters.
					script.script = p_expr.owner;
					const uint32_t result = temp(p_expr.type);
					instr(Op::GET_SCRIPT, result, NO_REG, NO_REG, class_index(script));
					return result;
				}
				if (p_expr.target == Target::Singleton) {
					const uint32_t result = temp(p_expr.type);
					instr(Op::GET_SINGLETON, result, NO_REG, NO_REG, class_index(p_expr.text));
					return result;
				}
				if (p_expr.target == Target::MethodValue) {
					const uint32_t result = temp(p_expr.type);
					instr(Op::METHOD_VALUE, result, NO_REG, NO_REG, uint32_t(p_expr.index));
					return result;
				}
				if (p_expr.target == Target::SelfSignal || p_expr.target == Target::ObjectSignal) {
					const uint32_t object = p_expr.target == Target::ObjectSignal ? lower_expr(*p_expr.base) : NO_REG;
					const uint32_t result = temp(p_expr.type);
					instr(Op::GET_SIGNAL, result, object, NO_REG, text_constant(ValueType::StringName, p_expr.text));
					return result;
				}
				// A local, narrowed or not, a member, a property or a field.
				Place place = place_of(p_expr);
				open(place);
				return load(place);
			}
			case ExprKind::Unary: {
				const uint32_t operand = lower_expr(*p_expr.left);
				const ValueType type = reg_type(operand);
				const uint32_t result = temp(p_expr.type);
				if (p_expr.op == TokenKind::Not) {
					instr(Op::NOT, result, operand);
				} else if (p_expr.op == TokenKind::Tilde) {
					instr(Op::NOT_INT, result, operand);
				} else if (type == ValueType::Float || type == ValueType::Int) {
					instr(type == ValueType::Float ? Op::NEG_FLOAT : Op::NEG_INT, result, operand);
				} else {
					instr(Op::EVALUATE, result, operand, NO_REG, operator_call(OP_NEGATE, type, ValueType::Void, reg_type(result)));
				}
				return result;
			}
			case ExprKind::Binary:
				if (p_expr.target == Target::NoneComparison) {
					const Expr &value = p_expr.left->kind == ExprKind::NoneLiteral ? *p_expr.right : *p_expr.left;
					const uint32_t source = lower_expr(value);
					const uint32_t result = temp(ValueType::Bool);
					instr(Op::IS_NONE, result, source, NO_REG, 0, fn.regs[source].type);
					return p_expr.op == TokenKind::BangEqual ? negate(result) : result;
				}
				if (p_expr.target == Target::Contains) {
					return contains(p_expr);
				}
				if (p_expr.target == Target::HasMember) {
					const uint32_t name = convert(lower_expr(*p_expr.left), Type::of(TypeKind::String));
					const uint32_t object = lower_expr(*p_expr.right);
					const uint32_t result = temp(ValueType::Bool);
					instr(Op::HAS_MEMBER, result, object, name);
					return result;
				}
				if (p_expr.op == TokenKind::Percent && is_format_string(p_expr.left->type)) {
					const uint32_t text = lower_expr(*p_expr.left);
					return format(text, lower_expr(*p_expr.right), p_expr.right->type);
				}
				if (p_expr.op == TokenKind::And || p_expr.op == TokenKind::Or) {
					return short_circuit(p_expr);
				} else {
					uint32_t left = lower_expr(*p_expr.left);
					uint32_t right = lower_expr(*p_expr.right);
					if (p_expr.left->type.kind == TypeKind::Variant || p_expr.right->type.kind == TypeKind::Variant) {
						// By the engine's operator table (spec §5.2).
						const uint32_t result = temp(ValueType::Bool);
						instr(Op::VARIANT_EQUAL, result, as_variant(outward(left, p_expr.left->type, false)), as_variant(outward(right, p_expr.right->type, false)));
						return p_expr.op == TokenKind::BangEqual ? negate(result) : result;
					}
					if (p_expr.left->type.kind == TypeKind::Object && p_expr.right->type.kind == TypeKind::Object) {
						// By ObjectID, not by the engine's operator, which takes two
						// freed objects for equal (spec §5.2).
						const uint32_t result = temp(ValueType::Bool);
						instr(Op::SAME_OBJECT, result, left, right, (p_expr.left->type.nullable ? 1u : 0u) | (p_expr.right->type.nullable ? 2u : 0u));
						return p_expr.op == TokenKind::BangEqual ? negate(result) : result;
					}
					if (p_expr.left->type.kind == TypeKind::Option || p_expr.right->type.kind == TypeKind::Option) {
						// The side with fewer levels of T? is wrapped (spec §5.2).
						const Type &deeper = option_depth(p_expr.left->type) >= option_depth(p_expr.right->type) ? p_expr.left->type : p_expr.right->type;
						left = wrapped(left, deeper);
						right = wrapped(right, deeper);
					}
					return binary(p_expr.op, left, right, p_expr.type);
				}
			case ExprKind::Call:
				return call(p_expr);
			case ExprKind::GetNode:
				return get_node(p_expr);
			case ExprKind::ArrayLiteral:
				return array_literal(p_expr);
			case ExprKind::DictionaryLiteral:
				return dictionary_literal(p_expr);
			case ExprKind::TypeTest: {
				uint32_t source = lower_expr(*p_expr.left);
				const uint32_t result = temp(ValueType::Bool);
				if (p_expr.left->type.kind != TypeKind::Object) {
					// What a Variant holds, or how an untyped container is typed
					// (spec §3.8, §3.13).
					source = as_variant(source);
					if (p_expr.tested.kind != TypeKind::Object) {
						instr(Op::IS_TYPE, result, source, NO_REG, 0, type_index(p_expr.tested));
						return p_expr.bool_value ? negate(result) : result;
					}
					source = variant_object(source, p_expr.tested);
				}
				instr(Op::IS_INSTANCE, result, source, NO_REG, class_index(p_expr.tested));
				return p_expr.bool_value ? negate(result) : result;
			}
			case ExprKind::Conditional: {
				const uint32_t result = temp(p_expr.type);
				const uint32_t then_block = new_block();
				const uint32_t else_block = new_block();
				const uint32_t end_block = new_block();
				branch(lower_expr(*p_expr.base), then_block, else_block);
				current = then_block;
				move(result, convert(lower_expr(*p_expr.left), p_expr.type));
				jump(end_block);
				current = else_block;
				move(result, convert(lower_expr(*p_expr.right), p_expr.type));
				jump(end_block);
				current = end_block;
				return result;
			}
			case ExprKind::Subscript: {
				const uint32_t container = lower_expr(*p_expr.base);
				return get_index(container, index_of(p_expr), p_expr.type);
			}
			case ExprKind::Cast: {
				uint32_t value = lower_expr(*p_expr.left);
				if (p_expr.left->type.kind == TypeKind::Variant) {
					value = variant_object(value, p_expr.tested); // None also for what is no object (spec §5.3).
				} else if (!p_expr.type.nullable) {
					return value; // Already a T.
				}
				const uint32_t result = temp(p_expr.type);
				instr(Op::CAST_OBJECT, result, value, NO_REG, class_index(p_expr.tested));
				return result;
			}
			case ExprKind::Self: {
				const uint32_t result = temp(p_expr.type);
				instr(Op::GET_SELF, result);
				return result;
			}
		}
		return internal_error("no code for this expression");
	}

	// get_node() on self, then a check that the node is there and has the
	// class the scenes gave it (spec §7.1), in case the tree changed since.
	uint32_t get_node(const Expr &p_expr) {
		const uint32_t path_reg = temp(ValueType::NodePath);
		instr(Op::CONST_VALUE, path_reg, NO_REG, NO_REG, text_constant(ValueType::NodePath, p_expr.text), fn.regs[path_reg].type);
		ir::Instr i;
		i.op = Op::CALL_ENGINE;
		i.args = { path_reg };
		i.dst = temp(p_expr.type);
		i.imm = engine_call("get_node", fn.regs[i.dst].type);
		add(i);
		check_object(i.dst, p_expr.type);
		return i.dst;
	}

	/* Places */

	static bool is_place(const Expr &p_expr) {
		if (p_expr.kind == ExprKind::Subscript) {
			return is_shared(p_expr.base->type) || is_place(*p_expr.base);
		}
		switch (p_expr.target) {
			case Target::Local:
			case Target::Member:
			case Target::StaticVar:
			case Target::SelfProperty:
			case Target::Owner:
			case Target::ObjectProperty:
				return true;
			case Target::Field:
			case Target::StructField:
				return is_place(*p_expr.base);
			default:
				return false;
		}
	}

	// A field of a struct at `p_base`.
	Place struct_field(Place p_base, uint32_t p_field, const Type &p_type) {
		Place place;
		place.type = p_type;
		const auto offset_in = [&](uint32_t p_struct) {
			return bytecode::field_offsets(module.types, p_struct)[p_field];
		};
		switch (p_base.kind) {
			case Place::Kind::Local:
				place.reg = field_view(p_base.reg, p_field);
				return place;
			case Place::Kind::Member:
				if (accessor(p_base.index, false) >= 0 || accessor(p_base.index, true) >= 0) {
					break;
				}
				place.kind = Place::Kind::MemberField;
				place.index = p_base.index;
				place.offset = offset_in(type_index(p_base.type));
				return place;
			case Place::Kind::MemberField:
				place.kind = Place::Kind::MemberField;
				place.index = p_base.index;
				place.offset = p_base.offset + offset_in(type_index(p_base.type));
				return place;
			case Place::Kind::StructCopy:
				place.kind = Place::Kind::StructCopy;
				place.fields = std::move(p_base.fields);
				place.fields.push_back(p_field);
				place.parent = std::move(p_base.parent);
				return place;
			default:
				break;
		}
		place.kind = Place::Kind::StructCopy;
		place.fields = { p_field };
		place.parent = std::make_unique<Place>(std::move(p_base));
		return place;
	}

	// Reads the copies that a place is changed in, outermost first. An
	// assignment does it once the value is known, so that no suspension comes
	// between the read and the write back (spec §3.6).
	void open(Place &r_place) {
		if (!r_place.parent) {
			return;
		}
		open(*r_place.parent);
		if (r_place.kind != Place::Kind::StructCopy) {
			r_place.reg = load(*r_place.parent); // Field or Element: the value it is in.
			return;
		}
		r_place.whole = load(*r_place.parent);
		r_place.reg = r_place.whole;
		for (const uint32_t field : r_place.fields) {
			r_place.reg = field_view(r_place.reg, field);
		}
	}

	Place place_of(const Expr &p_expr) {
		Place place;
		place.type = p_expr.type;
		place.int_range = p_expr.int_range;
		if (p_expr.kind == ExprKind::Subscript) {
			// An Array or a Dictionary changes in place; a packed array is a
			// value, written back to where it came from.
			place.kind = Place::Kind::Element;
			if (!is_shared(p_expr.base->type) && is_place(*p_expr.base)) {
				place.parent = std::make_unique<Place>(place_of(*p_expr.base));
			} else {
				place.reg = lower_expr(*p_expr.base);
			}
			place.at = index_of(p_expr);
			return place;
		}
		switch (p_expr.target) {
			case Target::Local:
				place.reg = reg_of(p_expr.index);
				place.kind = is_option_reg(place.reg) && p_expr.type.kind != TypeKind::Option ? Place::Kind::NarrowedLocal : Place::Kind::Local;
				if (place.kind == Place::Kind::Local && fn.regs[place.reg].type != type_index(p_expr.type) && (reg_type(place.reg) == ValueType::Variant || (!is_typed_container_reg(place.reg) && is_typed_container(p_expr.type)))) {
					place.kind = Place::Kind::Retyped;
				}
				break;
			case Target::Member:
				place.kind = Place::Kind::Member;
				place.index = uint32_t(p_expr.index);
				break;
			case Target::StaticVar:
				if (p_expr.static_on_value) {
					lower_expr(*p_expr.base); // Evaluated, and not used (spec §4.8).
				}
				place.kind = Place::Kind::Static;
				place.index = uint32_t(p_expr.index);
				place.variable = p_expr.variable;
				place.owner = p_expr.owner;
				break;
			case Target::SelfProperty:
				place.kind = Place::Kind::SelfProperty;
				place.index = property(p_expr.text, type_index(p_expr.type));
				break;
			case Target::Owner:
				place.kind = Place::Kind::Owner;
				break;
			case Target::ObjectProperty:
				place.kind = Place::Kind::ObjectProperty;
				place.reg = lower_expr(*p_expr.base);
				place.index = property(p_expr.text, type_index(p_expr.type));
				break;
			case Target::Field:
				place.kind = Place::Kind::Field;
				if (is_place(*p_expr.base)) {
					place.parent = std::make_unique<Place>(place_of(*p_expr.base));
				} else {
					place.reg = lower_expr(*p_expr.base); // Only read: a member of a temporary value.
				}
				place.index = field(value_type(p_expr.base->type), p_expr.text, value_type(p_expr.type));
				break;
			case Target::StructField:
				if (is_place(*p_expr.base)) {
					return struct_field(place_of(*p_expr.base), uint32_t(p_expr.index), p_expr.type);
				}
				place.reg = field_view(lower_expr(*p_expr.base), uint32_t(p_expr.index)); // Only read: a field of a temporary value.
				break;
			default:
				place.reg = internal_error("this expression is not a place");
				break;
		}
		return place;
	}

	uint32_t load(const Place &p_place) {
		switch (p_place.kind) {
			case Place::Kind::Local:
				return p_place.reg;
			case Place::Kind::NarrowedLocal: {
				const uint32_t element = module.types[fn.regs[p_place.reg].type].element;
				if (module.types[element].kind == bytecode::TypeKind::Value && module.types[element].value == ValueType::Variant && p_place.type.kind != TypeKind::Variant) {
					// A Variant? known to hold a T (spec §5.4).
					const uint32_t variant = temp(ValueType::Variant);
					instr(Op::UNWRAP, variant, p_place.reg, NO_REG, 0, fn.regs[variant].type);
					return from_variant(variant, p_place.type);
				}
				const uint32_t result = temp(p_place.type);
				instr(Op::UNWRAP, result, p_place.reg, NO_REG, 0, fn.regs[result].type);
				return result;
			}
			case Place::Kind::Retyped: {
				if (reg_type(p_place.reg) == ValueType::Variant) {
					return from_variant(p_place.reg, p_place.type);
				}
				const uint32_t result = temp(p_place.type);
				move(result, p_place.reg);
				return result;
			}
			case Place::Kind::Member: {
				const int getter = accessor(p_place.index, false);
				return getter >= 0 ? call_function(uint32_t(getter), {}, p_place.type) : load_member(p_place.index);
			}
			case Place::Kind::MemberField: {
				const uint32_t result = temp(p_place.type);
				ir::Instr i;
				i.op = by_storage(reg_type(result), Op::LOAD_MEMBER_8, Op::LOAD_MEMBER_STRING, Op::LOAD_MEMBER_VALUE);
				i.type = fn.regs[result].type;
				i.dst = result;
				i.imm = p_place.index;
				i.field = p_place.offset;
				add(i);
				return result;
			}
			case Place::Kind::StructCopy:
				return p_place.reg;
			case Place::Kind::Static: {
				if (const int getter = static_accessor(p_place, false); getter != NO_ACCESSOR) {
					return call_static_accessor(p_place, false, {});
				}
				ir::Instr i;
				i.op = Op::LOAD_STATIC;
				i.dst = temp(p_place.type);
				i.type = fn.regs[i.dst].type;
				i.imm = p_place.index;
				i.field = static_class(p_place);
				add(i);
				return i.dst;
			}
			case Place::Kind::SelfProperty:
			case Place::Kind::ObjectProperty: {
				const uint32_t result = temp(p_place.type);
				instr(Op::GET_PROPERTY, result, p_place.kind == Place::Kind::SelfProperty ? NO_REG : p_place.reg, NO_REG, p_place.index);
				// The binding metadata says whether it may be None (spec §10.2).
				check_object(result, p_place.type);
				return result;
			}
			case Place::Kind::Owner: {
				const uint32_t result = temp(p_place.type);
				instr(Op::GET_OWNER, result, NO_REG, NO_REG, class_index(p_place.type));
				return result;
			}
			case Place::Kind::Field: {
				const uint32_t result = temp(p_place.type);
				instr(Op::GET_FIELD, result, p_place.reg, NO_REG, p_place.index);
				return result;
			}
			case Place::Kind::Element:
				return get_index(p_place.reg, p_place.at, p_place.type);
		}
		return internal_error("no code to read this place");
	}

	void store(const Place &p_place, uint32_t p_value) {
		switch (p_place.kind) {
			case Place::Kind::Local:
				move(p_place.reg, p_value);
				return;
			case Place::Kind::NarrowedLocal: {
				const bytecode::TypeDesc &element = module.types[module.types[fn.regs[p_place.reg].type].element];
				const bool variant = element.kind == bytecode::TypeKind::Value && element.value == ValueType::Variant;
				instr(Op::SOME, p_place.reg, variant ? as_variant(p_value) : p_value, NO_REG, 0, fn.regs[p_place.reg].type);
				return;
			}
			case Place::Kind::Retyped:
				move(p_place.reg, reg_type(p_place.reg) == ValueType::Variant ? as_variant(p_value) : p_value);
				return;
			case Place::Kind::Member: {
				const int setter = accessor(p_place.index, true);
				if (setter >= 0) {
					call_function(uint32_t(setter), { p_value }, Type::of(TypeKind::Void));
				} else {
					store_member(p_place.index, p_value);
				}
				return;
			}
			case Place::Kind::MemberField: {
				ir::Instr i;
				i.op = by_storage(reg_type(p_value), Op::STORE_MEMBER_8, Op::STORE_MEMBER_STRING, Op::STORE_MEMBER_VALUE);
				i.type = fn.regs[p_value].type;
				i.a = p_value;
				i.imm = p_place.index;
				i.field = p_place.offset;
				add(i);
				return;
			}
			case Place::Kind::StructCopy:
				move(p_place.reg, p_value);
				store(*p_place.parent, p_place.whole);
				return;
			case Place::Kind::Static: {
				if (static_accessor(p_place, true) != NO_ACCESSOR) {
					call_static_accessor(p_place, true, { p_value });
					return;
				}
				ir::Instr i;
				i.op = Op::STORE_STATIC;
				i.type = fn.regs[p_value].type;
				i.a = p_value;
				i.imm = p_place.index;
				i.field = static_class(p_place);
				add(i);
				return;
			}
			case Place::Kind::SelfProperty:
			case Place::Kind::ObjectProperty:
				if (p_place.int_range) {
					instr(Op::CHECK_RANGE, NO_REG, p_value, NO_REG, uint32_t(*p_place.int_range));
				}
				instr(Op::SET_PROPERTY, NO_REG, p_place.kind == Place::Kind::SelfProperty ? NO_REG : p_place.reg, p_value, p_place.index);
				return;
			case Place::Kind::Owner:
				instr(Op::SET_PROPERTY, NO_REG, NO_REG, p_value, property("owner", fn.regs[p_value].type));
				return;
			case Place::Kind::Field:
				instr(Op::SET_FIELD, NO_REG, p_place.reg, p_value, p_place.index);
				if (p_place.parent) {
					store(*p_place.parent, p_place.reg);
				}
				return;
			case Place::Kind::Element:
				instr(Op::SET_INDEX, p_place.reg, p_place.at, p_place.type.kind == TypeKind::Variant ? as_variant(p_value) : (is_boxed(p_place.type) ? box(p_value, false) : p_value));
				if (p_place.parent) {
					store(*p_place.parent, p_place.reg);
				}
				return;
		}
	}

	/* Statements */

	void lower_block(const Block &p_block) {
		for (const auto &stmt : p_block) {
			lower_stmt(*stmt);
		}
	}

	// Goes on in the current block when the value in `p_value` fits the
	// pattern, its names bound, and jumps to `p_fail` when it does not (spec
	// §5.6).
	void lower_pattern(const Pattern &p_pattern, uint32_t p_value, uint32_t p_fail) {
		const uint32_t next_block = new_block();
		switch (p_pattern.kind) {
			case PatternKind::Wildcard:
				break;
			case PatternKind::Bind: {
				const uint32_t local = reg_of(p_pattern.local);
				if (fn.regs[local].type == fn.regs[p_value].type) {
					move(local, p_value);
				} else {
					// The T inside a T? that a branch before took None of.
					instr(Op::UNWRAP, local, p_value, NO_REG, 0, fn.regs[local].type);
				}
				break;
			}
			case PatternKind::Value: {
				const uint32_t local = reg_of(p_pattern.local);
				if (local != p_value) {
					move(local, p_value);
				}
				const Expr &compare = *p_pattern.value;
				if (compare.target != Target::NoneComparison && compare.left->type.kind == TypeKind::Variant) {
					// Of the pattern's type first, as in GDScript.
					const uint32_t matches = temp(ValueType::Bool);
					const uint32_t value = lower_expr(*compare.left);
					instr(Op::MATCHES, matches, value, as_variant(lower_expr(*compare.right)));
					branch(matches, next_block, p_fail);
				} else {
					branch(lower_expr(compare), next_block, p_fail);
				}
				current = next_block;
				return;
			}
			case PatternKind::Constructor: {
				const Type &type = p_pattern.type;
				if (type.is_optional()) {
					const uint32_t none = temp(ValueType::Bool);
					instr(Op::IS_NONE, none, p_value, NO_REG, 0, fn.regs[p_value].type);
					branch(none, p_pattern.variant == 0 ? next_block : p_fail, p_pattern.variant == 0 ? p_fail : next_block);
					current = next_block;
					if (p_pattern.variant == 1) {
						const uint32_t inner = temp(type.unwrapped());
						instr(Op::UNWRAP, inner, p_value, NO_REG, 0, fn.regs[inner].type);
						lower_pattern(p_pattern.items[0], inner, p_fail);
					}
					return;
				}
				if (type.kind == TypeKind::PayloadEnum) {
					const uint32_t variant = temp(ValueType::Int);
					load_int(variant, p_pattern.variant);
					const uint32_t same = temp(ValueType::Bool);
					instr(Op::EQ_INT, same, field_view(p_value, 0), variant);
					branch(same, next_block, p_fail);
				} else {
					jump(next_block);
				}
				current = next_block;
				for (size_t i = 0; i < p_pattern.items.size(); i++) {
					const uint32_t field = type.kind == TypeKind::PayloadEnum ? variant_field(type, p_pattern.variant, i) : uint32_t(i);
					lower_pattern(p_pattern.items[i], field_view(p_value, field), p_fail);
				}
				return;
			}
		}
		jump(next_block);
		current = next_block;
	}

	void lower_stmt(const Stmt &p_stmt) {
		at = p_stmt.range;
		instr(Op::LINE, NO_REG, NO_REG, NO_REG, uint32_t(cls.source->line(p_stmt.range.first)));
		switch (p_stmt.kind) {
			case StmtKind::Var:
			case StmtKind::Const: {
				const uint32_t local = reg_of(p_stmt.local);
				if (p_stmt.value) {
					move(local, convert(lower_expr(*p_stmt.value), decl->locals[p_stmt.local].type));
				} else {
					default_value(local);
				}
				return;
			}
			case StmtKind::Assign: {
				// The place, then the value, then reading, changing and writing back
				// with nothing between them (spec §3.6).
				const Expr &target = *p_stmt.target;
				Place place = place_of(target);
				uint32_t value = lower_expr(*p_stmt.value);
				open(place);
				if (p_stmt.assign_op == TokenKind::Equal) {
					if (place.kind == Place::Kind::SelfProperty || place.kind == Place::Kind::Owner || place.kind == Place::Kind::ObjectProperty) {
						value = outward(value, p_stmt.value->type, target.type.kind == TypeKind::Object); // The engine keeps it.
					} else if (place.kind == Place::Kind::Element) {
						value = outward(value, p_stmt.value->type, true);
					}
				} else if (p_stmt.assign_op == TokenKind::PercentEqual && is_format_string(target.type)) {
					value = format(load(place), value, p_stmt.value->type);
				} else {
					value = binary(compound_operator(p_stmt.assign_op), load(place), value, p_stmt.compound_result);
				}
				store(place, convert(value, target.type));
				return;
			}
			case StmtKind::If: {
				const uint32_t condition = lower_expr(*p_stmt.condition);
				const uint32_t then_block = new_block();
				const uint32_t end_block = new_block();
				const uint32_t else_block = p_stmt.else_body.empty() ? end_block : new_block();
				branch(condition, then_block, else_block);
				current = then_block;
				lower_block(p_stmt.body);
				jump(end_block);
				if (else_block != end_block) {
					current = else_block;
					lower_block(p_stmt.else_body);
					jump(end_block);
				}
				current = end_block;
				return;
			}
			case StmtKind::Match: {
				// The first branch with a pattern that matches and a guard that
				// holds runs, and no other (spec §5.6).
				const uint32_t matched = reg_of(p_stmt.local);
				move(matched, lower_expr(*p_stmt.value));
				const uint32_t end_block = new_block();
				for (const MatchBranch &arm : p_stmt.branches) {
					const uint32_t body_block = new_block();
					const uint32_t next_block = new_block();
					for (const Pattern &pattern : arm.patterns) {
						const uint32_t fail_block = new_block();
						lower_pattern(pattern, matched, fail_block);
						jump(body_block);
						current = fail_block;
					}
					jump(next_block);
					current = body_block;
					if (arm.guard) {
						const uint32_t guarded_block = new_block();
						branch(lower_expr(*arm.guard), guarded_block, next_block);
						current = guarded_block;
					}
					lower_block(arm.body);
					jump(end_block);
					current = next_block;
				}
				if (p_stmt.exhaustive && p_stmt.value->type.kind == TypeKind::Enum && !p_stmt.value->type.values->is_engine) {
					// No value comes here, since values of the enum are checked where
					// they come in (spec §1.5); a definite error all the same, rather
					// than running off the end of a function with a return type.
					instr(Op::CHECK_ENUM, NO_REG, matched, NO_REG, fn.regs[matched].type);
				}
				jump(end_block);
				current = end_block;
				return;
			}
			case StmtKind::While: {
				const uint32_t condition_block = new_block();
				const uint32_t body_block = new_block();
				const uint32_t end_block = new_block();
				jump(condition_block);
				current = condition_block;
				branch(lower_expr(*p_stmt.condition), body_block, end_block);
				current = body_block;
				loops.push_back({ condition_block, end_block });
				lower_block(p_stmt.body);
				loops.pop_back();
				jump(condition_block);
				current = end_block;
				return;
			}
			case StmtKind::For: {
				// Counts from 0 up to the int as it was on entry, or over the
				// container as it was on entry, by index and up to its current
				// size, as GDScript does. The loop variable gets the count or the
				// element afresh on each iteration.
				const bool counting = p_stmt.value->type.kind == TypeKind::Int;
				const std::optional<std::pair<Type, Type>> entries = entries_of(p_stmt.value->type);
				const uint32_t source = temp(entries ? Type::array(entries->first) : p_stmt.value->type);
				// A Dictionary is iterated over its keys as they were on entry.
				move(source, entries ? keys_of(lower_expr(*p_stmt.value), entries->first) : lower_expr(*p_stmt.value));
				const uint32_t counter = temp(ValueType::Int);
				load_int(counter, 0);
				const uint32_t condition_block = new_block();
				const uint32_t body_block = new_block();
				const uint32_t step_block = new_block();
				const uint32_t end_block = new_block();
				jump(condition_block);
				current = condition_block;
				const uint32_t limit = counting ? source : size_of(source);
				const uint32_t more = temp(ValueType::Bool);
				instr(Op::LT_INT, more, counter, limit);
				branch(more, body_block, end_block);
				current = body_block;
				move(reg_of(p_stmt.local), counting ? counter : get_index(source, counter, p_stmt.element));
				loops.push_back({ step_block, end_block });
				lower_block(p_stmt.body);
				loops.pop_back();
				jump(step_block);
				current = step_block;
				const uint32_t step = temp(ValueType::Int);
				load_int(step, 1);
				instr(Op::ADD_INT, counter, counter, step);
				jump(condition_block);
				current = end_block;
				return;
			}
			case StmtKind::Return: {
				if (terminated()) {
					current = new_block();
				}
				if (p_stmt.value) {
					const uint32_t value = convert(lower_expr(*p_stmt.value), decl->return_type);
					if (terminated()) {
						current = new_block();
					}
					fn.blocks[current].terminator = ir::Terminator::Return;
					fn.blocks[current].value = value;
				} else {
					fn.blocks[current].terminator = ir::Terminator::ReturnVoid;
				}
				return;
			}
			case StmtKind::Break:
				jump(loops.back().break_block);
				return;
			case StmtKind::Continue:
				jump(loops.back().continue_block);
				return;
			case StmtKind::Assert: {
				// Release builds evaluate neither the condition nor the message, and
				// debug builds the message only when the condition is false (spec §8.3).
				const uint32_t debug = temp(ValueType::Bool);
				instr(Op::DEBUG_BUILD, debug);
				const uint32_t check_block = new_block();
				const uint32_t fail_block = new_block();
				const uint32_t end_block = new_block();
				branch(debug, check_block, end_block);
				current = check_block;
				branch(lower_expr(*p_stmt.condition), end_block, fail_block);
				current = fail_block;
				instr(Op::ASSERT_FAIL, NO_REG, p_stmt.value ? lower_expr(*p_stmt.value) : NO_REG);
				jump(end_block);
				current = end_block;
				return;
			}
			case StmtKind::Pass:
				return;
			case StmtKind::Expression:
				// A string on a line of its own is a comment, with no code.
				if (p_stmt.value->kind != ExprKind::StringLiteral) {
					lower_expr(*p_stmt.value);
				}
				return;
		}
	}

	// Only the current block can still be open: every other block was left
	// through a jump, a branch or a return. Running off the end returns; in a
	// function with a return type the checker has made sure that this end is
	// unreachable.
	void finish() {
		if (!terminated()) {
			fn.blocks[current].terminator = ir::Terminator::ReturnVoid;
		}
	}

	void start(const std::string &p_name) {
		fn = ir::Function();
		fn.name = p_name;
		loops.clear();
		current = new_block();
	}

	// The member initializers in declaration order (spec §6.1): those that run
	// on a new instance, or the @onready ones (§6.3). Lowered before the
	// functions, so that their code and the table
	// entries they use stay the same when only a function body changes.
	bool lower_initializers(bool p_onready, bytecode::Function &r_out) {
		bool any = false;
		start(p_onready ? "@ready" : "@init");
		decl = nullptr;
		if (p_onready && cls.is_node) {
			// A node that becomes ready without the exports the scene must
			// assign stops there, before the @onready initializers (spec §7.5).
			for (size_t m = module.inherited_members; m < cls.all_members.size(); m++) {
				if (cls.all_members[m]->must_assign) {
					any = true;
					at = cls.all_members[m]->range;
					instr(Op::LINE, NO_REG, NO_REG, NO_REG, uint32_t(cls.source->line(cls.all_members[m]->name_token)));
					load_member(uint32_t(m));
				}
			}
		}
		for (size_t m = module.inherited_members; m < cls.all_members.size(); m++) {
			const VarDecl &member = *cls.all_members[m];
			if (member.is_onready != p_onready || (!member.initializer && !is_typed_container(member.type) && member.type.kind != TypeKind::Struct)) {
				continue;
			}
			any = true;
			at = member.range;
			instr(Op::LINE, NO_REG, NO_REG, NO_REG, uint32_t(cls.source->line(member.name_token)));
			if (!member.initializer) {
				// An empty Array[T] or Dictionary[K, V] is typed too, and a struct
				// has its fields' defaults (spec §4.3).
				store_member(uint32_t(m), default_value(temp(member.type)));
				continue;
			}
			store_member(uint32_t(m), convert(lower_expr(*member.initializer), member.type));
		}
		finish();
		if (any) {
			r_out = ir::emit(fn, module);
		}
		return any;
	}

	// The static variables' first values in declaration order, then the
	// class's own _static_init(), once when the class loads (spec §4.8).
	bool lower_static_initializer(bytecode::Function &r_out) {
		bool any = false;
		start("@static_init");
		decl = nullptr;
		const auto init_static = [&](size_t p_variable, uint32_t p_value) {
			ir::Instr i;
			i.op = Op::INIT_STATIC;
			i.type = fn.regs[p_value].type;
			i.a = p_value;
			i.imm = uint32_t(p_variable);
			add(i);
		};
		for (const bool with_initializer : { false, true }) {
			for (size_t v = 0; v < cls.static_vars.size(); v++) {
				const VarDecl &variable = cls.static_vars[v];
				// Without one, a variable holds its type's default from the start;
				// an empty Array[T] is typed too, and a struct has its fields'.
				if (bool(variable.initializer) != with_initializer || (!with_initializer && !is_typed_container(variable.type) && variable.type.kind != TypeKind::Struct)) {
					continue;
				}
				any = true;
				at = variable.range;
				instr(Op::LINE, NO_REG, NO_REG, NO_REG, uint32_t(cls.source->line(variable.name_token)));
				init_static(v, with_initializer ? convert(lower_expr(*variable.initializer), variable.type) : default_value(temp(variable.type)));
			}
		}
		for (size_t f = 0; f < cls.all_functions.size(); f++) {
			const FuncDecl &function = *cls.all_functions[f];
			if (function.name == "_static_init" && function.is_static && is_own_function(function)) {
				any = true;
				at = function.range;
				instr(Op::LINE, NO_REG, NO_REG, NO_REG, uint32_t(cls.source->line(function.name_token)));
				call_function(uint32_t(f), {}, Type::of(TypeKind::Void));
			}
		}
		finish();
		if (any) {
			r_out = ir::emit(fn, module);
			r_out.is_static = true;
		}
		return any;
	}

	bool is_own_function(const FuncDecl &p_function) const {
		return std::any_of(cls.functions.begin(), cls.functions.end(), [&](const FuncDecl &p_own) { return &p_own == &p_function; });
	}

	void record_init_tables() {
		bytecode::TablePrefix &prefix = module.init_tables;
		prefix.types = uint32_t(module.types.size());
		prefix.constants = uint32_t(module.constants.size());
		prefix.engine_calls = uint32_t(module.engine_calls.size());
		prefix.properties = uint32_t(module.properties.size());
		prefix.utility_calls = uint32_t(module.utility_calls.size());
		prefix.operators = uint32_t(module.operators.size());
		prefix.fields = uint32_t(module.fields.size());
		prefix.constructors = uint32_t(module.constructors.size());
		prefix.builtin_methods = uint32_t(module.builtin_methods.size());
		prefix.classes = uint32_t(module.classes.size());
		prefix.method_calls = uint32_t(module.method_calls.size());
	}

public:
	Lowerer(const ClassDecl &p_class, bytecode::Module &r_module, std::vector<Diagnostic> &r_errors, const StructLookup &p_structs) :
			cls(p_class), module(r_module), errors(r_errors), structs(p_structs) {}

	void run(bool p_declarations_only) {
		// Type 0, which a result that is not there has.
		type_index(ValueType::Void);
		module.global_name = cls.class_name;
		module.is_abstract = cls.is_abstract;
		module.base_class = cls.native_base;
		module.base_script = cls.base_script;
		module.inherited_members = cls.base ? uint32_t(cls.base->all_members.size()) : 0;
		for (const Type &interface : cls.implemented) {
			module.interfaces.push_back(interface_id(interface));
		}
		module.declarations_only = p_declarations_only;
		uint32_t offset = 0;
		for (const VarDecl *entry : cls.all_members) {
			const VarDecl &decl_member = *entry;
			bytecode::Member member;
			static_cast<bytecode::Slot &>(member) = slot(decl_member.name, decl_member.type);
			// Without the scenes, a type that comes from them is unknown; the
			// module only describes the class then (architecture §2.4).
			if (p_declarations_only && member.type == type_index(ValueType::Void)) {
				member.type = type_index(ValueType::Variant);
			}
			member.is_onready = decl_member.is_onready;
			member.setter = decl_member.setter_index;
			member.getter = decl_member.getter_index;
			member.line = uint32_t(decl_line(decl_member, &ClassDecl::members));
			member.offset = offset;
			offset += bytecode::type_size(module.types, member.type);
			member.is_pub = decl_member.is_pub;
			member.is_export = decl_member.is_export;
			member.hint = decl_member.hint;
			member.hint_string = decl_member.hint_string;
			member.usage = decl_member.usage;
			// Its type's default (spec §4.3) when it has no initializer; an empty
			// Array[T] is typed too.
			member.initial.type = member.type;
			if (decl_member.constant_initializer) {
				member.has_initial = true;
				member.initial = constant_of(*converted(decl_member.initializer->constant, decl_member.type));
			} else if (!decl_member.initializer && !decl_member.must_assign && decl_member.type.kind != TypeKind::Struct) {
				member.has_initial = true;
			}
			module.members.push_back(member);
		}
		module.instance_size = offset;
		uint32_t static_offset = 0;
		for (const VarDecl &variable : cls.static_vars) {
			bytecode::Member out;
			static_cast<bytecode::Slot &>(out) = slot(variable.name, variable.type);
			if (p_declarations_only && out.type == type_index(ValueType::Void)) {
				out.type = type_index(ValueType::Variant);
			}
			out.line = uint32_t(cls.source->line(variable.name_token));
			out.offset = static_offset;
			static_offset += bytecode::type_size(module.types, out.type);
			out.is_pub = variable.is_pub;
			out.setter = variable.setter_index;
			out.getter = variable.getter_index;
			out.has_initial = !variable.initializer;
			out.initial.type = out.type;
			module.statics.push_back(out);
		}
		module.statics_size = static_offset;
		module.static_unload = cls.static_unload;
		for (const SignalDecl *entry : cls.all_signals) {
			const SignalDecl &signal = *entry;
			bytecode::SignalInfo info;
			info.name = signal.name;
			info.is_pub = signal.is_pub;
			for (const ParamDecl &param : signal.params) {
				info.params.push_back(slot(param.name, param.type));
			}
			module.signals.push_back(info);
		}
		std::vector<const ClassDecl *> chain;
		for (const ClassDecl *owner = &cls; owner; owner = owner->base) {
			chain.push_back(owner);
		}
		for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
			const ClassDecl &owner = **it;
			const uint32_t prefix = owner.base ? uint32_t(owner.base->all_members.size()) : 0;
			if (owner.base) {
				module.groups.push_back({ "", "", 1, prefix });
			}
			for (const ExportGroupDecl &group : owner.groups) {
				bytecode::ExportGroup out;
				out.name = group.name;
				out.prefix = group.prefix;
				out.level = group.annotation.name == "export_category" ? 0 : (group.annotation.name == "export_group" ? 1 : 2);
				out.before_member = prefix + uint32_t(group.before_member);
				module.groups.push_back(out);
			}
		}

		// The constants the engine and GDScript read by name (spec §10.5, §11.3).
		for (const ConstDecl *constant : cls.all_constants) {
			if (constant->value) {
				module.named_constants.push_back({ constant->name, shared_constant(*constant->value), constant->is_pub });
			}
		}
		// An enum's name as GDScript's, a Dictionary, and the values of an unnamed one (spec §3.7).
		for (const EnumDecl *decl : cls.all_enums) {
			if (!decl->name.empty() && decl->dictionary) {
				module.named_constants.push_back({ decl->name, shared_constant(*decl->dictionary), decl->is_pub });
			}
			for (size_t i = 0; decl->name.empty() && i < decl->constants.size(); i++) {
				module.named_constants.push_back({ decl->values[i].name, shared_constant(*decl->constants[i]), decl->is_pub });
			}
		}

		bytecode::Function initializers;
		bytecode::Function ready_initializers;
		const bool has_initializers = !p_declarations_only && lower_initializers(false, initializers);
		const bool has_ready_initializers = !p_declarations_only && lower_initializers(true, ready_initializers);
		record_init_tables();

		for (const FuncDecl *entry : cls.all_functions) {
			const FuncDecl &function = *entry;
			const bool inherited = !is_own_function(function);
			start(function.name);
			decl = &function;
			at = inherited ? TokenRange{ cls.extends_token, cls.extends_token } : function.range;
			fn.param_count = uint32_t(function.params.size());
			fn.result = slot(std::string(), function.return_type);
			fn.is_async = function.is_async;
			fn.is_pub = function.is_pub;
			fn.is_override = function.is_override;
			fn.is_exposed = cls.exposed_methods.count(function.name);
			fn.line = uint32_t(decl_line(function, &ClassDecl::functions));
			for (const LocalInfo &local : function.locals) {
				fn.regs.push_back(slot(local.name, local.type));
			}
			if (!p_declarations_only && !inherited) {
				lower_block(function.body);
			}
			finish();
			bytecode::Function out = ir::emit(fn, module);
			out.is_static = function.is_static;
			out.rpc = function.rpc;
			out.is_abstract = function.is_abstract;
			out.is_inherited = inherited;
			if (inherited || function.is_abstract) {
				out.code.clear();
			}
			if (cls.base) {
				for (const FuncDecl *parent : cls.base->all_functions) {
					if (parent->name != function.name) {
						continue;
					}
					std::vector<bytecode::Slot> params;
					size_t defaults = 0;
					for (const ParamDecl &param : parent->params) {
						params.push_back(slot(param.name, param.type));
						if (param.default_value) {
							++defaults;
						}
					}
					out.base_signature = bytecode::signature_key(module.types, params, defaults, slot("", parent->return_type), parent->is_async);
				}
			}
			for (const ParamDecl &param : function.params) {
				if (param.default_value) {
					const ConstPtr value = converted(param.default_value->constant, param.type);
					out.defaults.push_back(value ? constant_of(*value) : bytecode::Constant{ type_index(param.type) });
				}
			}
			module.functions.push_back(std::move(out));
		}
		bytecode::Function static_initializer;
		const bool has_static_initializer = !p_declarations_only && lower_static_initializer(static_initializer);
		for (bytecode::Function &lambda : lambdas) {
			module.functions.push_back(std::move(lambda));
		}

		if (has_initializers) {
			module.functions.push_back(initializers);
			module.init_function = int32_t(module.functions.size() - 1);
		}
		if (has_ready_initializers) {
			module.functions.push_back(ready_initializers);
			module.ready_function = int32_t(module.functions.size() - 1);
		}
		if (has_static_initializer) {
			module.functions.push_back(static_initializer);
			module.static_init_function = int32_t(module.functions.size() - 1);
		}
	}
};

} // namespace

bytecode::Module lower(const ClassDecl &p_class, bool p_declarations_only, std::vector<Diagnostic> &r_errors, const StructLookup &p_structs) {
	bytecode::Module module;
	Lowerer(p_class, module, r_errors, p_structs).run(p_declarations_only);
	module.name = p_class.name;
	module.is_pub = p_class.is_pub;
	for (const std::unique_ptr<ClassDecl> &inner : p_class.classes) {
		module.inner_classes.push_back(lower(*inner, p_declarations_only, r_errors, p_structs));
	}
	return module;
}

} // namespace petrichor::compiler
