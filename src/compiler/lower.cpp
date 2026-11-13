#include "compiler/lower.h"

#include "compiler/builtin_api.h"
#include "compiler/ir.h"

#include <algorithm>
#include <cstring>
#include <map>
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
			return ValueType::Task;
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
	uint32_t index = 0; // Member, property table or field table; NarrowedLocal: the local.
	uint32_t at = NO_REG;
	uint32_t offset = 0;
	uint32_t whole = NO_REG;
	std::vector<uint32_t> fields; // StructCopy: the fields from `whole` down to this one.
	Type type;
	std::optional<bytecode::IntRange> int_range; // An engine property: what its setter takes (spec §10.6).
	const Expr *access = nullptr; // SelfProperty, ObjectProperty and Owner: the access, which may name a getter and setter.
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
	// The structs and enums with payloads whose fields are being interned,
	// and those interned, by "script::Name".
	std::vector<std::string> interning;
	std::unordered_map<std::string, uint32_t> named_types;
	// The elements and keys of containers that hold one being interned, as
	// Array[Tree] in Tree (spec §3.6). Each comes once that one has: until
	// then, the container holds DEFERRED less its place here.
	std::vector<std::pair<Type, bool>> deferred;
	static constexpr uint32_t DEFERRED = 0xFFFFFFFFu;
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
		size_t scope; // Of its body in `scopes`, the first that break and continue leave.
	};
	std::vector<Loop> loops;
	// The locals of each block being lowered, innermost last, released when
	// the block is left (spec §6.8).
	std::vector<std::vector<uint32_t>> scopes;
	// The function's registers of preset() by their type and value.
	std::map<std::pair<uint32_t, uint64_t>, uint32_t> presets;
	// A lambda's register for each of its locals, whose captures come right
	// after the parameters; empty where a local is its register.
	std::vector<uint32_t> local_regs;
	// Lambdas, which come after the class's functions (spec §3.11), in the order
	// they are made in.
	std::vector<bytecode::Function> lambdas;
	// The index of the function that computes each default of this class's
	// struct fields that is no constant; they follow the class's functions.
	std::unordered_map<const VarDecl *, uint32_t> field_defaults;

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
		if (fn.presets.count(p_instr.dst)) {
			internal_error("an instruction writes a preset constant");
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

	// An int, float or bool constant in a register that the frame starts with
	// it (bytecode::Preset): one register for each value of each type.
	uint32_t preset(uint32_t p_type, uint64_t p_bits) {
		const auto found = presets.find({ p_type, p_bits });
		if (found != presets.end()) {
			return found->second;
		}
		bytecode::Slot slot;
		slot.type = p_type;
		fn.regs.push_back(slot);
		const uint32_t reg = uint32_t(fn.regs.size() - 1);
		fn.presets.emplace(reg, p_bits);
		presets.emplace(std::make_pair(p_type, p_bits), reg);
		return reg;
	}

	uint32_t int_constant(int64_t p_value) {
		return preset(type_index(ValueType::Int), uint64_t(p_value));
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
			type.element = element_index(p_type.args[0]);
		} else if (is_typed_container(p_type)) {
			type.kind = bytecode::TypeKind::Dictionary;
			type.key = element_index(p_type.args[0]);
			type.element = element_index(p_type.args[1]);
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
			// A function type, which takes no null Callable (spec §4.3).
			if (p_type.kind == TypeKind::Function) {
				type.name = p_type.to_string();
			}
		}
		return type_index(type);
	}

	// A struct, "script::Name", with the types of its fields before it.
	uint32_t struct_index(const Type &p_struct) {
		bytecode::TypeDesc type;
		type.kind = bytecode::TypeKind::Struct;
		type.name = p_struct.script + "::" + p_struct.interface_name();
		if (const auto found = named_types.find(type.name); found != named_types.end()) {
			return found->second;
		}
		const StructDecl *decl = struct_decl(p_struct);
		// The checker refuses a struct that holds itself (spec §3.6), unless its
		// script failed to be checked.
		if (!decl || !begin_interning(type.name)) {
			error(p_struct.name + " cannot be compiled: " + p_struct.script + " has errors.");
			return type_index(ValueType::Void);
		}
		for (const VarDecl &field : decl->fields) {
			type.fields.push_back({ field.name, type_index(field.type) });
		}
		const uint32_t index = end_interning(type);
		struct_types.emplace(index, p_struct);
		return index;
	}

	bool begin_interning(const std::string &p_name) {
		if (std::find(interning.begin(), interning.end(), p_name) != interning.end()) {
			return false;
		}
		interning.push_back(p_name);
		return true;
	}

	// Interns the struct or enum whose fields are in, then the deferred types
	// that waited for it.
	uint32_t end_interning(const bytecode::TypeDesc &p_type) {
		interning.pop_back();
		const uint32_t index = type_index(p_type);
		named_types.emplace(p_type.name, index);
		for (size_t k = 0; k < deferred.size(); k++) {
			if (deferred[k].second || holds_interning(deferred[k].first)) {
				continue;
			}
			deferred[k].second = true;
			const uint32_t mark = DEFERRED - uint32_t(k);
			const uint32_t resolved = type_index(Type(deferred[k].first));
			for (bytecode::TypeDesc &type : module.types) {
				const bool container = type.kind == bytecode::TypeKind::Array || type.kind == bytecode::TypeKind::Dictionary;
				type.element = container && type.element == mark ? resolved : type.element;
				type.key = container && type.key == mark ? resolved : type.key;
			}
		}
		return index;
	}

	// A container's element or key, deferred while it holds a struct or an
	// enum being interned.
	uint32_t element_index(const Type &p_type) {
		if (!holds_interning(p_type)) {
			return type_index(p_type);
		}
		for (size_t k = 0; k < deferred.size(); k++) {
			if (!deferred[k].second && deferred[k].first == p_type) {
				return DEFERRED - uint32_t(k);
			}
		}
		deferred.push_back({ p_type, false });
		return DEFERRED - uint32_t(deferred.size() - 1);
	}

	// Whether a value of the type holds in its slot a struct or an enum with
	// payloads whose fields are being interned.
	bool holds_interning(const Type &p_type) const {
		if (p_type.kind == TypeKind::Option || p_type.is_result() || p_type.is_settled()) {
			return std::any_of(p_type.args.begin(), p_type.args.end(), [&](const Type &p_arg) { return holds_interning(p_arg); });
		}
		const std::string name = p_type.script + "::" + p_type.interface_name();
		return (p_type.kind == TypeKind::Struct || p_type.kind == TypeKind::PayloadEnum) && std::find(interning.begin(), interning.end(), name) != interning.end();
	}

	// An enum with payloads, "script::Name", or "Result" or "Settled", laid out
	// as a struct whose first field is the variant's index (bytecode/module.h).
	uint32_t payload_enum_index(const Type &p_enum) {
		bytecode::TypeDesc type;
		type.kind = bytecode::TypeKind::PayloadEnum;
		const bool generic = p_enum.is_result() || p_enum.is_settled();
		type.name = generic ? p_enum.name : p_enum.script + "::" + p_enum.interface_name();
		if (const auto found = generic ? named_types.end() : named_types.find(type.name); found != named_types.end()) {
			return found->second;
		}
		// The checker refuses one that holds itself (spec §3.6); a generic one
		// holds itself only through a struct or an enum, which is interned.
		if (!generic && !begin_interning(type.name)) {
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
		return generic ? type_index(type) : end_interning(type);
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
		const uint32_t argument = p_call.args.empty() ? NO_REG : convert(lower_expr(*p_call.args[0]), p_call.args[0]->type, p_call.param_types[0]);
		const uint32_t result = temp(p_call.type);
		if (name == "unwrap" || name == "expect") {
			instr(Op::UNWRAP_OK, result, value, argument);
			return result;
		}
		const uint32_t ok = temp(ValueType::Bool);
		const uint32_t zero = int_constant(0);
		instr(Op::EQ_INT, ok, field_view(value, 0), zero);
		const uint32_t blocks[2] = { new_block(), new_block() };
		const uint32_t end_block = new_block();
		branch(ok, blocks[0], blocks[1]);
		const bool on_error = name == "err" || name == "unwrap_or" || name == "unwrap_or_else" || name == "map_err" || name == "or_else";
		for (int variant = 0; variant < 2; variant++) {
			current = blocks[variant];
			const uint32_t inner = field_view(value, variant_field(type, variant, 0));
			const bool applies = (variant == 1) == on_error;
			const auto called = [&]() { return call_value(argument, p_call.param_types[0], inner, type.args[variant]); };
			if (name == "unwrap_or") {
				move(result, applies ? argument : inner);
			} else if (name == "unwrap_or_else") {
				move(result, applies ? convert(called(), p_call.param_types[0].args[0], p_call.type) : inner);
			} else if (name == "ok" || name == "err") {
				applies ? move(result, convert(inner, type.args[variant], p_call.type)) : (void)default_value(result);
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
	// filter() goes on as None when its function says no; flatten() gives the
	// T? inside.
	uint32_t option_method(const Expr &p_call) {
		const Type &type = p_call.base->type;
		const std::string &name = p_call.text;
		const uint32_t value = lower_expr(*p_call.base);
		const uint32_t argument = p_call.args.empty() ? NO_REG : convert(lower_expr(*p_call.args[0]), p_call.args[0]->type, p_call.param_types[0]);
		const uint32_t result = temp(p_call.type);
		const uint32_t none = temp(ValueType::Bool);
		instr(Op::IS_NONE, none, value, NO_REG, 0, fn.regs[value].type);
		const uint32_t some_block = new_block();
		const uint32_t none_block = new_block();
		const uint32_t end_block = new_block();
		branch(none, none_block, some_block);
		const auto called = [&](uint32_t p_inner) { return call_value(argument, p_call.param_types[0], p_inner, type.unwrapped()); };
		// What the function returns, as the method gives it.
		const auto returned = [&](uint32_t p_value) { return convert(p_value, p_call.param_types[0].args[0], p_call.type); };

		current = some_block;
		if (name == "or" || name == "or_else") {
			move(result, value);
		} else {
			const uint32_t inner = temp(type.unwrapped());
			instr(Op::UNWRAP, inner, value);
			if (name == "filter") {
				const uint32_t kept_block = new_block();
				branch(called(inner), kept_block, none_block);
				current = kept_block;
				move(result, value);
			} else if (name == "ok_or" || name == "ok_or_else") {
				make_variant(result, p_call.type, 0, { inner });
			} else {
				move(result, name == "map" || name == "and_then" ? returned(called(inner)) : inner);
			}
		}
		jump(end_block);

		current = none_block;
		if (name == "ok_or" || name == "ok_or_else") {
			make_variant(result, p_call.type, 1, { name == "ok_or" ? argument : called(NO_REG) });
		} else if (name == "unwrap_or" || name == "or") {
			move(result, argument);
		} else if (name == "unwrap_or_else" || name == "or_else") {
			move(result, returned(called(NO_REG)));
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
			const uint32_t zero = int_constant(0);
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
			instr(Op::UNWRAP, result, value);
		}
		return result;
	}

	// `f.call(x)` of a function value of the type `p_function` (spec §3.11),
	// with x, if any, converted to its parameter.
	uint32_t call_value(uint32_t p_value, const Type &p_function, uint32_t p_arg, const Type &p_arg_type) {
		ir::Instr i;
		i.op = Op::CALL_VALUE;
		i.base = p_value;
		bytecode::MethodCall method;
		method.method = "call";
		if (p_arg != NO_REG) {
			method.params.push_back(slot(std::string(), p_function.args[1]));
			i.args.push_back(convert(p_arg, p_arg_type, p_function.args[1]));
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
				const VarDecl &field = decl->fields[f];
				values[f] = computed(field) ? field_default(p_struct, *decl, field) : argument(*field.initializer, field.type, false, false);
			}
		}
		for (size_t f = 0; f < decl->fields.size(); f++) {
			move(field_view(result, uint32_t(f)), values[f]);
		}
		return result;
	}

	// A field's default that is no constant, which a function of the struct's
	// class computes, so that it runs as that class's code (spec §3.6).
	static bool computed(const VarDecl &p_field) {
		const ConstPtr value = p_field.initializer ? converted(p_field.initializer->constant, p_field.type) : nullptr;
		return p_field.initializer && (!value || loads(*value));
	}

	static std::string field_default_name(const StructDecl &p_decl, const VarDecl &p_field) {
		return "@" + p_decl.name + "." + p_field.name;
	}

	uint32_t field_default(const Type &p_struct, const StructDecl &p_decl, const VarDecl &p_field) {
		ir::Instr i;
		i.dst = temp(p_field.type);
		const auto own = field_defaults.find(&p_field);
		if (own != field_defaults.end()) {
			i.op = Op::CALL;
			i.imm = own->second;
		} else {
			i.op = Op::CALL_METHOD;
			bytecode::MethodCall method;
			method.method = field_default_name(p_decl, p_field);
			method.result = slot(std::string(), p_field.type);
			bytecode::ClassRef declaring; // Only its script matters.
			declaring.script = p_struct.script;
			method.static_class = int32_t(class_index(declaring));
			i.imm = method_call(method);
		}
		add(i);
		return i.dst;
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
	// literal is, the rest from its table entry, which the runtime computes
	// when the class loads.
	uint32_t load_constant(const ConstPtr &p_value) {
		using Kind = ConstValue::Kind;
		const ValueType stored = value_type(p_value->type);
		if (stored == ValueType::Int && p_value->kind == Kind::Int) {
			return preset(type_index(p_value->type), uint64_t(p_value->i));
		}
		if (stored == ValueType::Float && p_value->kind == Kind::Float) {
			uint64_t bits = 0;
			std::memcpy(&bits, &p_value->f, sizeof(bits));
			return preset(type_index(p_value->type), bits);
		}
		if (stored == ValueType::Bool && p_value->kind == Kind::Bool) {
			return preset(type_index(p_value->type), p_value->b ? 1 : 0);
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
					const uint32_t zero = int_constant(0);
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

	// Whether an expression is lowered as its value: a constant, a scalar the
	// compiler computed, such as `1 << 4`, or an engine value such as
	// `Vector2(1, 2)` or a preload that the class computes as it loads (spec
	// §4.5). A container's literal is not, each evaluation making a new one.
	static bool lowered_as_constant(const Expr &p_expr) {
		if (!p_expr.constant) {
			return false;
		}
		if (p_expr.target == Target::Constant || p_expr.target == Target::EngineConstant || p_expr.target == Target::BuiltinConstant || p_expr.constant->kind == ConstValue::Kind::Preload) {
			return true;
		}
		if (p_expr.kind == ExprKind::Name || p_expr.kind == ExprKind::Member) {
			return false;
		}
		const ValueType type = value_type(p_expr.type);
		return p_expr.constant->is_known() || (type >= ValueType::Vector2 && type <= ValueType::RID);
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

	bool tracks_assignment(uint32_t p_member) const {
		return cls.all_members[p_member]->must_assign || cls.all_members[p_member]->is_onready;
	}

	// A member that must be assigned is used only once it has been (spec
	// §6.2), an @onready one once its initializer has run (§6.3).
	void check_assigned(uint32_t p_member) {
		if (tracks_assignment(p_member)) {
			instr(Op::CHECK_MUST, NO_REG, NO_REG, NO_REG, p_member);
		}
	}

	uint32_t load_member(uint32_t p_member) {
		check_assigned(p_member);
		const uint32_t result = temp(cls.all_members[p_member]->type);
		instr(by_storage(reg_type(result), Op::LOAD_MEMBER_8, Op::LOAD_MEMBER_STRING, Op::LOAD_MEMBER_VALUE), result, NO_REG, NO_REG, p_member, fn.regs[result].type);
		return result;
	}

	void store_member(uint32_t p_member, uint32_t p_value) {
		instr(by_storage(reg_type(p_value), Op::STORE_MEMBER_8, Op::STORE_MEMBER_STRING, Op::STORE_MEMBER_VALUE), NO_REG, p_value, NO_REG, p_member, fn.regs[p_value].type);
		if (tracks_assignment(p_member)) {
			instr(Op::MARK_ASSIGNED, NO_REG, NO_REG, NO_REG, p_member);
		}
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

	// Reads, or with `p_value` writes, the member `p_place.variable` of the
	// object in `p_place.reg`: in its slot, or through its accessor called on
	// that object (spec §4.4, §10.5).
	uint32_t member_of(const Place &p_place, uint32_t p_value) {
		const VarDecl &member = *p_place.variable;
		const bool writes = p_value != NO_REG;
		const std::string &accessor = writes ? member.setter : member.getter;
		if (accessor.empty()) {
			const uint32_t result = writes ? NO_REG : temp(p_place.type);
			instr(writes ? Op::SET_MEMBER_OF : Op::GET_MEMBER_OF, result, p_place.reg, p_value, p_place.index);
			return result;
		}
		ir::Instr i;
		i.op = Op::CALL_METHOD;
		bytecode::MethodCall method;
		method.method = accessor;
		if (writes) {
			method.params.push_back(slot(std::string(), member.type));
			i.args.push_back(p_value);
		}
		method.result = slot(std::string(), writes ? Type::of(TypeKind::Void) : member.type);
		i.imm = method_call(method);
		i.base = p_place.reg;
		i.dst = writes ? NO_REG : temp(member.type);
		add(i);
		return i.dst;
	}

	// The getter of an engine property, or its setter with `p_value`, called
	// through its MethodBind (spec §10.5); one of a family passes its index first.
	uint32_t property_accessor(const Place &p_place, uint32_t p_value) {
		const Expr &access = *p_place.access;
		const bool writes = p_value != NO_REG;
		const EngineMethod &method = writes ? *access.setter : *access.getter;
		ir::Instr i;
		i.op = Op::CALL_ENGINE;
		i.base = p_place.kind == Place::Kind::ObjectProperty ? p_place.reg : NO_REG;
		if (access.property_index >= 0) {
			const uint32_t index = temp(ValueType::Int);
			load_int(index, access.property_index);
			i.args.push_back(index);
		}
		if (writes) {
			i.args.push_back(p_value);
		}
		i.dst = writes ? NO_REG : temp(p_place.type);
		bytecode::EngineCall call{ method.name, writes ? type_index(Type::of(TypeKind::Void)) : fn.regs[i.dst].type };
		call.owner = method.owner;
		call.hash = method.hash;
		for (const EngineParam &param : method.params) {
			call.params.push_back(value_type(param.type));
		}
		call.return_type = value_type(method.return_type);
		call.returns_reference = !writes && access.returns_reference;
		i.imm = engine_call(call);
		add(i);
		return i.dst;
	}

	// A try_ version (spec §10.3): Ok of what the engine method returned, or Err
	// of the error that its getter reads, which the call has just set.
	uint32_t tried_result(const Expr &p_call, uint32_t p_value) {
		const EngineMethod &getter = *p_call.getter;
		ir::Instr i;
		i.op = Op::CALL_ENGINE;
		i.dst = temp(p_call.type.args[1]);
		bytecode::EngineCall call{ getter.name, fn.regs[i.dst].type, getter.owner };
		call.owner = getter.owner;
		call.hash = getter.hash;
		call.return_type = value_type(getter.return_type);
		i.imm = engine_call(call);
		add(i);
		const uint32_t failed = temp(ValueType::Bool);
		instr(Op::NE_INT, failed, i.dst, int_constant(0));
		const uint32_t result = temp(p_call.type);
		const uint32_t ok_block = new_block();
		const uint32_t err_block = new_block();
		const uint32_t end_block = new_block();
		branch(failed, err_block, ok_block);
		current = ok_block;
		uint32_t value = p_value;
		if (p_call.engine_return.nullable) {
			value = temp(p_call.type.args[0]);
			instr(Op::UNWRAP, value, p_value);
		}
		make_variant(result, p_call.type, 0, { value });
		jump(end_block);
		current = err_block;
		make_variant(result, p_call.type, 1, { i.dst });
		jump(end_block);
		current = end_block;
		return result;
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

	uint32_t engine_call(const bytecode::EngineCall &p_call) {
		return intern(module.engine_calls, p_call, [](const auto &a, const auto &b) { return a == b; });
	}

	uint32_t method_call(const bytecode::MethodCall &p_call) {
		return intern(module.method_calls, p_call, [&](const auto &a, const auto &b) {
			return a.method == b.method && a.static_class == b.static_class && bytecode::signature_key(module.types, a.params, a.defaults, a.result, a.is_async, a.is_vararg) == bytecode::signature_key(module.types, b.params, b.defaults, b.result, b.is_async, b.is_vararg);
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

	// A value of `p_from` as a `p_to`, as the checker allowed (spec §3.14).
	uint32_t convert(uint32_t p_reg, const Type &p_from, const Type &p_to) {
		// The checker has made sure that the classes fit.
		struct Checked final : ClassRelation {
			bool is_a(const Type &, const Type &) const override { return true; }
		};
		if (p_reg == NO_REG) {
			return p_reg;
		}
		switch (conversion(p_from, p_to, Checked())) {
			case Conversion::Same:
				return p_reg;
			case Conversion::IntToFloat: {
				const uint32_t result = temp(ValueType::Float);
				instr(Op::INT_TO_FLOAT, result, p_reg);
				return result;
			}
			case Conversion::Construct: {
				ir::Instr i;
				i.op = Op::CONSTRUCT;
				i.imm = constructor(value_type(p_to), uint32_t(converting_constructor(p_from, p_to)), { value_type(p_from) });
				i.dst = temp(p_to);
				i.args = { p_reg };
				add(i);
				return i.dst;
			}
			case Conversion::Wrap: {
				const uint32_t inner = convert(p_reg, p_from, p_to.args[0]);
				const uint32_t result = temp(p_to);
				instr(Op::SOME, result, inner, NO_REG, 0, fn.regs[result].type);
				return result;
			}
			case Conversion::Nothing:
				return default_value(temp(p_to));
			case Conversion::ToVariant:
				return as_variant(p_reg); // A struct encoded (spec §3.13, §10.1).
			case Conversion::None:
				break;
		}
		return internal_error("no conversion from " + p_from.to_string() + " to " + p_to.to_string());
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
		return convert(p_outward ? outward(value, p_arg.type, p_takes_object) : value, p_arg.type, p_param);
	}

	std::vector<uint32_t> arguments(const Expr &p_call) {
		// A function of this language reads its T? parameters itself.
		const bool outward = p_call.target != Target::ScriptFunction && p_call.target != Target::ScriptMethod && p_call.target != Target::StructMethod && p_call.target != Target::SuperMethod && p_call.target != Target::CallValue && p_call.target != Target::NewObject;
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

	// range(to), range(from, to) or range(from, to, step) into `p_dst`: an
	// Array[int] of its values, or an int of how many there are.
	uint32_t range(const Expr &p_call, uint32_t p_dst, std::vector<uint32_t> p_bounds = {}) {
		if (p_bounds.empty()) {
			for (const auto &arg : p_call.args) {
				p_bounds.push_back(lower_expr(*arg));
			}
		}
		if (p_bounds.size() == 1) {
			p_bounds.insert(p_bounds.begin(), int_constant(0));
		}
		if (p_bounds.size() == 2) {
			p_bounds.push_back(int_constant(1));
		}
		ir::Instr i;
		i.op = Op::RANGE;
		i.imm = fn.regs[p_dst].type;
		i.dst = p_dst;
		i.args = std::move(p_bounds);
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
		const uint32_t value = convert(outward(lower_expr(p_value), p_value.type, true), p_value.type, p_as);
		const uint32_t converted = convert(value, p_as, p_element);
		return is_boxed(p_element) ? box(converted, p_key) : converted;
	}

	// A value of a boxed type as a container keeps it (architecture §2.2); a
	// key's box hashes the value.
	uint32_t box(uint32_t p_reg, bool p_key) {
		const uint32_t result = temp(ValueType::Variant);
		instr(Op::BOX, result, p_reg, NO_REG, p_key ? 1 : 0, fn.regs[p_reg].type);
		return result;
	}

	// The engine's resize(), then each new element set to its own default
	// value (spec §3.8).
	uint32_t resize(const Expr &p_call) {
		const uint32_t array = lower_expr(*p_call.base);
		const Type &element = p_call.base->type.args[0];
		const uint32_t size = argument(*p_call.args[0], Type::of(TypeKind::Int), false, false);
		const uint32_t index = temp(ValueType::Int);
		move(index, size_of(array));
		const uint32_t result = call_builtin(engine_builtin_method(ValueType::Array, "resize", { ValueType::Int }, ValueType::Int), array, { size }, temp(ValueType::Int));
		const uint32_t one = int_constant(1);
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
	uint32_t format(uint32_t p_text, const Type &p_text_type, uint32_t p_value, const Type &p_type) {
		const uint32_t text = convert(p_text, p_text_type, Type::of(TypeKind::String));
		const uint32_t result = temp(ValueType::String);
		instr(Op::FORMAT, result, text, engine_variant(p_value, p_type));
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

	// A value as a Variant that the engine works on (spec §5.1, §5.2).
	uint32_t engine_variant(uint32_t p_reg, const Type &p_type) {
		return as_variant(outward(p_reg, p_type, false));
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
		move(result, convert(get_index(dictionary, key, value_type), value_type, p_call.type));
		jump(end);
		current = missing;
		default_value(result);
		jump(end);
		current = end;
		return result;
	}

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
		const std::vector<Type> &element = p_call.base->type.args;
		instr(Op::SOME, result, element.empty() ? value : from_variant(value, element[0]), NO_REG, 0, fn.regs[result].type);
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
		method.is_vararg = p_call.is_vararg;
		method.result = slot(std::string(), Type::of(TypeKind::Void));
		i.imm = method_call(method);
		add(i);
		return i.base;
	}

	// The members that new(...) or instantiate(...) assigns by name, after the
	// call, in the order written and as the engine writes them, through setters
	// (spec §6.4).
	uint32_t assign_members(const Expr &p_call, uint32_t p_object) {
		for (size_t i = 0; i < p_call.member_values.size(); i++) {
			const Type &type = p_call.member_types[i];
			const Type &given = p_call.member_values[i]->type;
			const uint32_t value = convert(outward(lower_expr(*p_call.member_values[i]), given, type.kind == TypeKind::Object), given, type);
			instr(Op::SET_PROPERTY, NO_REG, p_object, value, property(p_call.member_names[i], fn.regs[value].type));
		}
		return p_object;
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
		if (is_option_reg(p_dst) || type == ValueType::Object || type == ValueType::Task || type == ValueType::Variant || module.types[fn.regs[p_dst].type].kind == bytecode::TypeKind::PayloadEnum) {
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
		const ValueType left_type = reg_type(p_left);
		const ValueType right_type = reg_type(p_right);
		// Containers too, whose `==` may walk into boxes too deep, which only
		// this instruction reports (spec §3.6), and tasks.
		const bool compared = (p_op == TokenKind::EqualEqual || p_op == TokenKind::BangEqual) && left_type == right_type && (left_type == ValueType::Array || left_type == ValueType::Dictionary || left_type == ValueType::Task);
		if (is_struct_reg(p_left) || is_option_reg(p_left) || compared) {
			// Field by field (spec §3.6), None equal to None, a task by identity (§5.2).
			const uint32_t result = temp(ValueType::Bool);
			instr(Op::EQUAL, result, p_left, p_right, 0, fn.regs[p_left].type);
			return p_op == TokenKind::BangEqual ? negate(result) : result;
		}
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
			const Type number = Type::of(TypeKind::Float);
			p_left = convert(p_left, left_type == ValueType::Int ? Type::of(TypeKind::Int) : number, number);
			p_right = convert(p_right, right_type == ValueType::Int ? Type::of(TypeKind::Int) : number, number);
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
		switch (p_call.target) {
			case Target::Noop:
				return NO_REG;
			case Target::NamedCallable:
			case Target::NamedSignal: {
				const Expr &object = *p_call.args[0];
				const uint32_t base = object.kind == ExprKind::Self ? NO_REG : lower_expr(object);
				const uint32_t result = temp(p_call.type);
				const Op op = p_call.target == Target::NamedSignal ? Op::GET_SIGNAL : p_call.index ? Op::METHOD_VALUE
																								   : Op::ENGINE_METHOD_VALUE;
				instr(op, result, base, NO_REG, text_constant(ValueType::StringName, p_call.args[1]->text));
				return result;
			}
			case Target::GetOrNone:
				return get_or_none(p_call);
			case Target::PopOrNone:
				return pop_or_none(p_call);
			case Target::ResizeWithDefaults:
				return resize(p_call);
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
				return convert(lower_expr(*p_call.args[0]), p_call.args[0]->type, Type::of(TypeKind::Float));
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
			case Target::Some: {
				const Type &value = p_call.param_types[0];
				return convert(convert(lower_expr(*p_call.args[0]), p_call.args[0]->type, value), value, p_call.type);
			}
			case Target::Unwrap: {
				const uint32_t value = lower_expr(*p_call.base);
				const uint32_t message = p_call.args.empty() ? NO_REG : convert(lower_expr(*p_call.args[0]), p_call.args[0]->type, Type::of(TypeKind::String));
				const uint32_t result = temp(p_call.type);
				instr(Op::UNWRAP, result, value, message);
				return result;
			}
			case Target::OptionMethod:
				return option_method(p_call);
			case Target::ResultMethod:
				return result_method(p_call);
			case Target::TryFrom: {
				const uint32_t value = convert(lower_expr(*p_call.args[0]), p_call.args[0]->type, Type::of(TypeKind::Variant));
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
		if (p_call.target == Target::TaskSettled) {
			// It owns a task started for it, as an await does (spec §9.3).
			const bool owned = p_call.base->kind == ExprKind::Call && p_call.base->is_async;
			const uint32_t member = lower_expr(*p_call.base);
			const uint32_t task = temp(p_call.type);
			instr(Op::TASK_SETTLED, task, member, NO_REG, owned ? 1 : 0, type_index(p_call.type.args[0]));
			return task;
		}
		if (p_call.target == Target::Range) {
			return range(p_call, temp(p_call.type));
		}
		if (p_call.target == Target::GDScriptUtility) {
			if (p_call.text == "print_stack") {
				instr(Op::PRINT_STACK, NO_REG);
				return NO_REG;
			}
			const uint32_t result = temp(p_call.type);
			if (p_call.text == "is_instance_of") {
				uint32_t value = lower_expr(*p_call.args[0]);
				if (reg_type(value) == ValueType::Variant) {
					value = variant_object(value, Type::object("Object"));
				}
				instr(Op::IS_SCRIPT, result, value, lower_expr(*p_call.args[1]));
				return result;
			}
			if (p_call.text == "get_stack") {
				instr(Op::GET_STACK, result, NO_REG, NO_REG, 0, fn.regs[result].type);
				return result;
			}
			const uint32_t value = lower_expr(*p_call.args[0]);
			if (p_call.text == "ord") {
				instr(Op::ORD, result, value);
				return result;
			}
			// len()
			const ValueType type = reg_type(value);
			const bool text = type == ValueType::String || type == ValueType::StringName;
			return call_builtin(engine_builtin_method(type, text ? "length" : "size", {}, ValueType::Int), value, {}, result);
		}
		if (p_call.target == Target::TaskAny || p_call.target == Target::TaskAll) {
			// Each task, with whether it was started for the join, which owns it
			// then, or an Array of tasks, none of them its own (spec §9.5).
			ir::Instr i;
			i.op = p_call.target == Target::TaskAny ? Op::TASK_ANY : Op::TASK_ALL;
			if (p_call.args[0]->kind != ExprKind::ArrayLiteral) {
				i.args.push_back(lower_expr(*p_call.args[0]));
			} else {
				uint32_t flags[2] = { NO_REG, NO_REG };
				for (const auto &member : p_call.args[0]->args) {
					const bool owned = member->kind == ExprKind::Call && member->is_async;
					i.args.push_back(lower_expr(*member));
					if (flags[owned] == NO_REG) {
						flags[owned] = int_constant(owned);
					}
					i.args.push_back(flags[owned]);
				}
			}
			i.dst = temp(p_call.type);
			i.imm = type_index(p_call.type.args[0]);
			add(i);
			return i.dst;
		}
		if (p_call.target == Target::TaskOf || p_call.target == Target::TaskDelay) {
			const uint32_t made = arguments(p_call)[0];
			const uint32_t task = temp(p_call.type);
			instr(p_call.target == Target::TaskOf ? Op::TASK_OF : Op::TASK_DELAY, task, made);
			return task;
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
		} else if ((p_call.target == Target::EngineMethod && p_call.base) || p_call.target == Target::ScriptMethod || p_call.target == Target::StructMethod || p_call.target == Target::BindArguments || p_call.target == Target::EmitSignal || p_call.target == Target::CallValue) {
			i.base = lower_expr(*p_call.base);
		} else if (p_call.target == Target::BuiltinMethod) {
			if (!p_call.is_const && !is_shared(p_call.base->type) && is_place(*p_call.base)) {
				// A method that changes its base changes the place it came from,
				// read once the arguments are known (spec §3.6).
				written_back = std::make_unique<Place>(place_of(*p_call.base));
			} else {
				i.base = lower_expr(*p_call.base);
			}
		}
		i.args = arguments(p_call);
		if (written_back) {
			open(*written_back);
			i.base = load(*written_back);
		}
		if (p_call.target == Target::StructMethod && i.base != NO_REG) {
			i.args.insert(i.args.begin(), i.base); // Self, the first parameter.
			i.base = NO_REG;
		}
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

		// An array of the class an argument named, or a try_ version (spec §10.3).
		const bool filtered = p_call.target == Target::EngineMethod && p_call.engine_return.builtin_name() == "Array" && p_call.type != p_call.engine_return;
		const bool tried = p_call.target == Target::EngineMethod && p_call.getter;
		const Type &returned = filtered || tried ? p_call.engine_return : p_call.type;
		i.dst = p_call.type.kind == TypeKind::Void ? NO_REG : temp(returned);
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
				method.is_vararg = p_call.is_vararg;
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
			case Target::StructMethod: {
				const auto own = std::find(cls.all_functions.begin(), cls.all_functions.end(), p_call.callee);
				if (own != cls.all_functions.end()) {
					i.op = Op::CALL;
					i.imm = uint32_t(own - cls.all_functions.begin());
					break;
				}
				i.op = Op::CALL_METHOD;
				i.imm = struct_method_call(*p_call.callee, p_call.owner, p_call.base ? p_call.base->type : Type());
				break;
			}
			case Target::EngineMethod: {
				i.op = Op::CALL_ENGINE;
				bytecode::EngineCall call{ tried ? std::string(builtin::tried_method(p_call.text)) : p_call.text, type_index(returned), p_call.is_static ? p_call.owner : std::string() };
				if (p_call.hash) {
					call.owner = p_call.owner;
					call.hash = p_call.hash;
					call.params = value_types(p_call.engine_params);
					call.defaults = p_call.engine_defaults;
					call.return_type = value_type(p_call.engine_return);
					call.returns_reference = p_call.returns_reference;
				}
				i.imm = engine_call(call);
				break;
			}
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
		if (tried) {
			return tried_result(p_call, i.dst);
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
		if (p_call.target == Target::EngineMethod && p_call.type.kind == TypeKind::Object && p_call.type.nullable && p_call.type != p_call.engine_return) {
			// A T? narrower than the engine's, as get_node_or_null() of a
			// literal path (spec §7.2), is checked unless it is None.
			const uint32_t none = temp(ValueType::Bool);
			instr(Op::IS_NONE, none, i.dst, NO_REG, 0, fn.regs[i.dst].type);
			const uint32_t check_block = new_block();
			const uint32_t end_block = new_block();
			branch(none, end_block, check_block);
			current = check_block;
			Type found = p_call.type;
			found.nullable = false;
			check_object(i.dst, found);
			jump(end_block);
			current = end_block;
		}
		return i.dst;
	}

	// A method of a struct of the class at `p_owner`, called on that class's
	// current image (spec §12.4), self first for an instance method.
	uint32_t struct_method_call(const FuncDecl &p_method, const std::string &p_owner, const Type &p_self) {
		bytecode::MethodCall method;
		method.method = p_method.name;
		if (!p_method.is_static) {
			method.params.push_back(slot(std::string(), p_self));
		}
		for (const ParamDecl &param : p_method.params) {
			method.params.push_back(slot(std::string(), param.type));
			method.defaults += param.default_value ? 1 : 0;
		}
		method.is_vararg = p_method.has_rest();
		method.is_async = p_method.is_async;
		method.result = slot(std::string(), p_method.return_type);
		bytecode::ClassRef declaring; // Only its script matters.
		declaring.script = p_owner;
		method.static_class = int32_t(class_index(declaring));
		return method_call(method);
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
		i.imm = uint32_t(cls.all_functions.size() + field_defaults.size() + own);
		add(i);

		lambdas.emplace_back();
		ir::Function outer_fn = std::move(fn);
		const FuncDecl *outer_decl = decl;
		const uint32_t outer_current = current;
		const TokenRange outer_at = at;
		std::vector<Loop> outer_loops = std::move(loops);
		std::vector<std::vector<uint32_t>> outer_scopes = std::move(scopes);
		std::map<std::pair<uint32_t, uint64_t>, uint32_t> outer_presets = std::move(presets);
		std::vector<uint32_t> outer_regs = std::move(local_regs);

		start(lambda.name.empty() ? "<lambda>" : lambda.name);
		decl = &lambda;
		at = lambda.range;
		fn.param_count = uint32_t(lambda.params.size() + lambda.captures.size());
		fn.result = slot(std::string(), lambda.return_type);
		fn.is_async = lambda.is_async;
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
		out.is_vararg = lambda.has_rest();
		out.captures = uint32_t(lambda.captures.size());
		out.is_static = !lambda.uses_self;
		lambdas[own] = std::move(out);

		fn = std::move(outer_fn);
		decl = outer_decl;
		current = outer_current;
		at = outer_at;
		loops = std::move(outer_loops);
		scopes = std::move(outer_scopes);
		presets = std::move(outer_presets);
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
			return p_expr.type.kind == TypeKind::Option ? convert(value, p_expr.constant->type, p_expr.type) : value;
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
				if (p_expr.target == Target::MethodValue || p_expr.target == Target::EngineMethodValue) {
					const uint32_t object = p_expr.base && p_expr.base->kind != ExprKind::Self ? lower_expr(*p_expr.base) : NO_REG;
					const uint32_t result = temp(p_expr.type);
					instr(p_expr.target == Target::MethodValue ? Op::METHOD_VALUE : Op::ENGINE_METHOD_VALUE, result, object, NO_REG, text_constant(ValueType::StringName, p_expr.text));
					return result;
				}
				if (p_expr.target == Target::StaticValue || p_expr.target == Target::EngineStaticValue || p_expr.target == Target::NewValue) {
					if (p_expr.static_on_value) {
						lower_expr(*p_expr.base);
					}
					bytecode::ClassRef owner;
					(p_expr.target == Target::EngineStaticValue ? owner.name : owner.script) = p_expr.owner;
					ir::Instr i;
					i.op = Op::STATIC_VALUE;
					i.field = class_index(owner);
					i.dst = temp(p_expr.type);
					// A struct's is named after it, as `Hit.make`.
					i.imm = text_constant(ValueType::StringName, p_expr.target == Target::StaticValue ? p_expr.callee->name : p_expr.text);
					add(i);
					return i.dst;
				}
				if (p_expr.target == Target::StructMethodValue) {
					const uint32_t self = lower_expr(*p_expr.base);
					const uint32_t result = temp(p_expr.type);
					instr(Op::STRUCT_METHOD_VALUE, result, self, NO_REG, struct_method_call(*p_expr.callee, p_expr.owner, p_expr.base->type));
					return result;
				}
				if (p_expr.target == Target::SelfSignal || p_expr.target == Target::ObjectSignal) {
					const uint32_t object = p_expr.target == Target::ObjectSignal ? lower_expr(*p_expr.base) : NO_REG;
					const uint32_t result = temp(p_expr.type);
					const std::string name = p_expr.signal ? bytecode::engine_signal_name(p_expr.text, p_expr.signal->is_pub) : p_expr.text;
					instr(Op::GET_SIGNAL, result, object, NO_REG, text_constant(ValueType::StringName, name));
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
					const uint32_t name = convert(lower_expr(*p_expr.left), p_expr.left->type, Type::of(TypeKind::String));
					const uint32_t object = lower_expr(*p_expr.right);
					const uint32_t result = temp(ValueType::Bool);
					instr(Op::HAS_MEMBER, result, object, name);
					return result;
				}
				if (p_expr.op == TokenKind::Percent && is_format_string(p_expr.left->type)) {
					const uint32_t text = lower_expr(*p_expr.left);
					return format(text, p_expr.left->type, lower_expr(*p_expr.right), p_expr.right->type);
				}
				if (p_expr.op == TokenKind::And || p_expr.op == TokenKind::Or) {
					return short_circuit(p_expr);
				} else {
					uint32_t left = lower_expr(*p_expr.left);
					uint32_t right = lower_expr(*p_expr.right);
					if (p_expr.target == Target::VariantEqual || p_expr.target == Target::VariantMatch) {
						const uint32_t result = temp(ValueType::Bool);
						instr(p_expr.target == Target::VariantMatch ? Op::MATCHES : Op::VARIANT_EQUAL, result, engine_variant(left, p_expr.left->type), engine_variant(right, p_expr.right->type));
						return p_expr.op == TokenKind::BangEqual ? negate(result) : result;
					}
					if (p_expr.left->type.kind == TypeKind::Object && p_expr.right->type.kind == TypeKind::Object) {
						// By ObjectID, not by the engine's operator, which takes two
						// freed objects for equal (spec §5.2).
						const uint32_t result = temp(ValueType::Bool);
						instr(Op::SAME_OBJECT, result, left, right, (p_expr.left->type.nullable ? 1u : 0u) | (p_expr.right->type.nullable ? 2u : 0u));
						return p_expr.op == TokenKind::BangEqual ? negate(result) : result;
					}
					if (!p_expr.param_types.empty()) {
						left = convert(left, p_expr.left->type, p_expr.param_types[0]);
						right = convert(right, p_expr.right->type, p_expr.param_types[1]);
					}
					return binary(p_expr.op, left, right, p_expr.type);
				}
			case ExprKind::Call:
				return p_expr.member_values.empty() ? call(p_expr) : assign_members(p_expr, call(p_expr));
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
				move(result, convert(lower_expr(*p_expr.left), p_expr.left->type, p_expr.type));
				jump(end_block);
				current = else_block;
				move(result, convert(lower_expr(*p_expr.right), p_expr.right->type, p_expr.type));
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
				if (p_expr.target == Target::Local) {
					return reg_of(p_expr.index); // A struct's, in its methods (spec §3.6).
				}
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
		bytecode::EngineCall call{ "get_node", fn.regs[i.dst].type };
		if (p_expr.hash) {
			call.owner = "Node";
			call.hash = p_expr.hash;
			call.params = { ValueType::NodePath };
			call.return_type = ValueType::Object;
		}
		i.imm = engine_call(call);
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

	// With `p_index` false, an element's own index is left for the caller.
	Place place_of(const Expr &p_expr, bool p_index = true) {
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
			if (p_index) {
				place.at = index_of(p_expr);
			}
			return place;
		}
		switch (p_expr.target) {
			case Target::Local: {
				place.reg = reg_of(p_expr.index);
				// Narrowed where the checker typed it otherwise than declared (spec §5.4).
				const Type &declared = decl->locals[p_expr.index].type;
				if (declared.kind == TypeKind::Option && p_expr.type != declared) {
					place.kind = Place::Kind::NarrowedLocal;
					place.index = uint32_t(p_expr.index);
				} else if (p_expr.type != declared && (declared.kind == TypeKind::Variant || (!is_typed_container(declared) && is_typed_container(p_expr.type)))) {
					place.kind = Place::Kind::Retyped;
				}
				break;
			}
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
				place.access = &p_expr;
				break;
			case Target::Owner:
				place.kind = Place::Kind::Owner;
				place.access = &p_expr;
				break;
			case Target::ObjectProperty:
				place.kind = Place::Kind::ObjectProperty;
				place.reg = lower_expr(*p_expr.base);
				place.access = &p_expr;
				if (p_expr.variable) {
					place.index = property(p_expr.text, type_index(p_expr.type));
				}
				place.variable = p_expr.variable;
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
				// A level of T? at a time, down to the type it is known to have; a
				// Variant? known to hold a T, as that T (spec §5.4).
				Type type = decl->locals[p_place.index].type;
				uint32_t value = p_place.reg;
				while (type.kind == TypeKind::Option && type != p_place.type) {
					type = Type(type.args[0]); // A copy: assigning would free what it reads.
					const uint32_t inner = temp(type);
					instr(Op::UNWRAP, inner, value);
					value = inner;
				}
				return type.kind == TypeKind::Variant && p_place.type.kind != TypeKind::Variant ? from_variant(value, p_place.type) : value;
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
				check_assigned(p_place.index);
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
				if (p_place.variable) {
					return member_of(p_place, NO_REG);
				}
				uint32_t result = NO_REG;
				if (p_place.access->getter) {
					result = property_accessor(p_place, NO_REG);
				} else {
					result = temp(p_place.type);
					instr(Op::GET_PROPERTY, result, p_place.kind == Place::Kind::SelfProperty ? NO_REG : p_place.reg, NO_REG, property(p_place.access->text, type_index(p_place.type)));
				}
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
				const Type &inner = decl->locals[p_place.index].type.args[0];
				instr(Op::SOME, p_place.reg, convert(p_value, p_place.type, inner), NO_REG, 0, fn.regs[p_place.reg].type);
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
				// A field of a struct that has no value yet has none either.
				check_assigned(p_place.index);
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
				if (p_place.variable) {
					member_of(p_place, p_value);
					return;
				}
				if (p_place.int_range) {
					instr(Op::CHECK_RANGE, NO_REG, p_value, NO_REG, uint32_t(*p_place.int_range));
				}
				if (p_place.access->setter) {
					property_accessor(p_place, p_value);
				} else {
					instr(Op::SET_PROPERTY, NO_REG, p_place.kind == Place::Kind::SelfProperty ? NO_REG : p_place.reg, p_value, property(p_place.access->text, type_index(p_place.type)));
				}
				return;
			case Place::Kind::Owner:
				if (p_place.access->setter) {
					property_accessor(p_place, p_value);
				} else {
					instr(Op::SET_PROPERTY, NO_REG, NO_REG, p_value, property("owner", fn.regs[p_value].type));
				}
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

	// Releases what the registers hold, of those that can hold a reference
	// (spec §6.8); a view goes with its root.
	void release(const std::vector<uint32_t> &p_regs) {
		ir::Instr i;
		i.op = Op::CLEAR;
		for (uint32_t reg : p_regs) {
			if (!fn.views.count(reg) && bytecode::holds_references(module.types, fn.regs[reg].type) && std::find(i.args.begin(), i.args.end(), reg) == i.args.end()) {
				i.args.push_back(reg);
			}
		}
		if (!i.args.empty() && !terminated()) {
			add(i);
		}
	}

	// The temporaries made since the register `p_first`, but `p_kept`.
	std::vector<uint32_t> temps_since(uint32_t p_first, uint32_t p_kept = NO_REG) const {
		std::vector<uint32_t> regs;
		for (uint32_t reg = p_first; reg < fn.regs.size(); reg++) {
			if (reg != p_kept) {
				regs.push_back(reg);
			}
		}
		return regs;
	}

	// The locals of the blocks from `p_scope` in, which a jump out leaves.
	void release_scopes(size_t p_scope) {
		std::vector<uint32_t> locals;
		for (size_t s = p_scope; s < scopes.size(); s++) {
			locals.insert(locals.end(), scopes[s].begin(), scopes[s].end());
		}
		release(locals);
	}

	// A block, its locals released at its end; those of a function's body go
	// with its frame. `p_locals` are the block's from its header, such as a
	// loop's variable.
	void lower_block(const Block &p_block, std::vector<uint32_t> p_locals = {}) {
		scopes.push_back(std::move(p_locals));
		for (const auto &stmt : p_block) {
			lower_stmt(*stmt);
		}
		if (scopes.size() > 1) {
			release(scopes.back());
		}
		scopes.pop_back();
	}

	// Whether the instruction has read its operands when it stores its result,
	// so that the result can go to one of them. The engine's operators and
	// constructors compute a value of its math types before storing it
	// (core/variant/variant_op.h:50-83), but not an Array's (:659-673).
	bool stores_last(const ir::Instr &p_instr) const {
		switch (p_instr.op) {
			case Op::EVALUATE:
			case Op::CONSTRUCT: {
				const ValueType type = reg_type(p_instr.dst);
				return is_8_byte(type) || (type >= ValueType::Vector2 && type <= ValueType::Color);
			}
			case Op::CONST_STRING:
			case Op::CONST_VALUE:
			case Op::MOVE_8:
			case Op::MOVE_STRING:
			case Op::MOVE_VALUE:
			case Op::LOAD_MEMBER_8:
			case Op::LOAD_MEMBER_STRING:
			case Op::LOAD_MEMBER_VALUE:
			case Op::GET_FIELD:
			case Op::INT_TO_FLOAT:
			case Op::FLOAT_TO_INT:
			case Op::ADD_INT:
			case Op::SUB_INT:
			case Op::MUL_INT:
			case Op::DIV_INT:
			case Op::MOD_INT:
			case Op::NEG_INT:
			case Op::AND_INT:
			case Op::OR_INT:
			case Op::XOR_INT:
			case Op::NOT_INT:
			case Op::SHL_INT:
			case Op::SHR_INT:
			case Op::POW_INT:
			case Op::ADD_FLOAT:
			case Op::SUB_FLOAT:
			case Op::MUL_FLOAT:
			case Op::DIV_FLOAT:
			case Op::POW_FLOAT:
			case Op::NEG_FLOAT:
			case Op::ADD_STRING:
			case Op::EQ_INT:
			case Op::NE_INT:
			case Op::LT_INT:
			case Op::LE_INT:
			case Op::GT_INT:
			case Op::GE_INT:
			case Op::EQ_FLOAT:
			case Op::NE_FLOAT:
			case Op::LT_FLOAT:
			case Op::LE_FLOAT:
			case Op::GT_FLOAT:
			case Op::GE_FLOAT:
			case Op::EQ_BOOL:
			case Op::NE_BOOL:
			case Op::EQ_STRING:
			case Op::NE_STRING:
			case Op::NOT:
				return true;
			default:
				return false;
		}
	}

	// `p_local = p_value`, where the value is a temporary that the statement
	// made from `p_first` on for this: the instruction that computed it stores
	// it in the local instead, when it can, without a copy.
	void assign_local(uint32_t p_local, uint32_t p_value, uint32_t p_first) {
		std::vector<ir::Instr> &instrs = fn.blocks[current].instrs;
		if (!terminated() && !instrs.empty() && instrs.back().dst == p_value && p_value >= p_first && !fn.views.count(p_value) && fn.regs[p_value].type == fn.regs[p_local].type && stores_last(instrs.back())) {
			instrs.back().dst = p_local;
			return;
		}
		move(p_local, p_value);
	}

	// Over an object through its _iter_init(), _iter_next() and _iter_get(),
	// as the engine calls them: each of the first two gets a new Array that
	// holds the state, and leaves the state there (spec §5.7).
	void lower_iteration(const Stmt &p_stmt, uint32_t p_first) {
		const Expr &get = *p_stmt.iteration[2];
		const uint32_t object = reg_of(get.base->index);
		const uint32_t iter = reg_of(p_stmt.iteration[0]->args[0]->index);
		const uint32_t state = reg_of(get.args[0]->index);
		move(object, lower_expr(*p_stmt.value));
		release(temps_since(p_first));
		const uint32_t append = engine_builtin_method(ValueType::Array, "append", { ValueType::Variant }, ValueType::Void);
		const uint32_t body_block = new_block();
		const uint32_t step_block = new_block();
		const uint32_t end_block = new_block();
		// _iter_init() or _iter_next(), then the body or the end.
		const auto step = [&](uint32_t p_which) {
			const uint32_t first = uint32_t(fn.regs.size());
			new_container(iter);
			call_builtin(append, iter, { state });
			const uint32_t more = lower_expr(*p_stmt.iteration[p_which]);
			instr(Op::ITER_STATE, state, iter, NO_REG, p_which);
			release(temps_since(first));
			branch(more, body_block, end_block);
		};
		step(0);
		current = body_block;
		const uint32_t local = reg_of(p_stmt.local);
		const uint32_t got = uint32_t(fn.regs.size());
		move(local, convert(lower_expr(get), get.type, decl->locals[p_stmt.local].type));
		release(temps_since(got));
		loops.push_back({ step_block, end_block, scopes.size() });
		lower_block(p_stmt.body, { local });
		loops.pop_back();
		jump(step_block);
		current = step_block;
		// An error in _iter_next() is the for's.
		instr(Op::LINE, NO_REG, NO_REG, NO_REG, uint32_t(cls.source->line(p_stmt.range.first)));
		step(1);
		current = end_block;
		release({ object, iter, state });
	}

	// Whether a statement in the block assigns the local.
	static bool assigns(const Block &p_block, int p_local) {
		for (const auto &stmt : p_block) {
			if (stmt->kind == StmtKind::Assign && stmt->target->kind == ExprKind::Name && stmt->target->target == Target::Local && stmt->target->index == p_local) {
				return true;
			}
			if (assigns(stmt->body, p_local) || assigns(stmt->else_body, p_local)) {
				return true;
			}
			for (const MatchBranch &arm : stmt->branches) {
				if (assigns(arm.body, p_local)) {
					return true;
				}
			}
		}
		return false;
	}

	// The locals that the pattern binds, or with `p_values`, those that hold
	// what its value patterns compare.
	static void pattern_locals(const Pattern &p_pattern, bool p_values, std::vector<int> &r_locals) {
		if (p_pattern.local >= 0 && (p_pattern.kind == PatternKind::Value) == p_values) {
			r_locals.push_back(p_pattern.local);
		}
		for (const Pattern &item : p_pattern.items) {
			pattern_locals(item, p_values, r_locals);
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
					instr(Op::UNWRAP, local, p_value);
				}
				break;
			}
			case PatternKind::Value: {
				const uint32_t local = reg_of(p_pattern.local);
				if (local != p_value) {
					move(local, p_value);
				}
				branch(lower_expr(*p_pattern.value), next_block, p_fail);
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
						instr(Op::UNWRAP, inner, p_value);
						lower_pattern(p_pattern.items[0], inner, p_fail);
					}
					return;
				}
				if (type.kind == TypeKind::PayloadEnum) {
					const uint32_t variant = int_constant(p_pattern.variant);
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
			case PatternKind::Array:
			case PatternKind::Dictionary: {
				// Of the kind, then of the size, then item by item; a key is
				// looked up before its value is read.
				uint32_t container = p_value;
				const Type &type = p_pattern.type;
				if (reg_type(p_value) == ValueType::Variant) {
					const uint32_t held = temp(ValueType::Bool);
					instr(Op::IS_TYPE, held, p_value, NO_REG, 0, type_index(type));
					const uint32_t held_block = new_block();
					branch(held, held_block, p_fail);
					current = held_block;
					container = from_variant(p_value, type);
				}
				const uint32_t fits = temp(ValueType::Bool);
				instr(p_pattern.rest ? Op::GE_INT : Op::EQ_INT, fits, size_of(container), int_constant(int64_t(p_pattern.items.size())));
				branch(fits, next_block, p_fail);
				current = next_block;
				const bool array = p_pattern.kind == PatternKind::Array;
				const auto [key_type, element] = array ? std::make_pair(Type::of(TypeKind::Int), *element_of(type)) : *entries_of(type);
				for (size_t i = 0; i < p_pattern.items.size(); i++) {
					const Pattern &item = p_pattern.items[i];
					if (array) {
						lower_pattern(item, get_index(container, int_constant(int64_t(i)), element), p_fail);
						continue;
					}
					const uint32_t key = stored(*item.key, key_type, key_type, true);
					const uint32_t has = call_builtin(engine_builtin_method(ValueType::Dictionary, "has", { ValueType::Variant }, ValueType::Bool, false, NO_REG, type.args.empty()), container, { key }, temp(ValueType::Bool));
					const uint32_t found = new_block();
					branch(has, found, p_fail);
					current = found;
					if (item.kind != PatternKind::Wildcard) {
						lower_pattern(item, get_index(container, key, element), p_fail);
					}
				}
				return;
			}
		}
		jump(next_block);
		current = next_block;
	}

	// A statement, its temporaries released at its end; those of a compound
	// statement's header once it has decided, that of a for loop's value at
	// the loop's end (spec §6.8).
	void lower_stmt(const Stmt &p_stmt) {
		const uint32_t first = uint32_t(fn.regs.size());
		lower_stmt_code(p_stmt);
		switch (p_stmt.kind) {
			case StmtKind::Var:
			case StmtKind::Const:
			case StmtKind::Assign:
			case StmtKind::Assert:
			case StmtKind::Expression:
				release(temps_since(first));
				break;
			default:
				break;
		}
	}

	void lower_stmt_code(const Stmt &p_stmt) {
		at = p_stmt.range;
		instr(Op::LINE, NO_REG, NO_REG, NO_REG, uint32_t(cls.source->line(p_stmt.range.first)));
		const uint32_t first = uint32_t(fn.regs.size());
		switch (p_stmt.kind) {
			case StmtKind::Var:
			case StmtKind::Const: {
				const uint32_t local = reg_of(p_stmt.local);
				if (p_stmt.value) {
					assign_local(local, convert(lower_expr(*p_stmt.value), p_stmt.value->type, decl->locals[p_stmt.local].type), first);
				} else {
					default_value(local);
				}
				scopes.back().push_back(local);
				return;
			}
			case StmtKind::Assign: {
				// The place, then the value and the place's last index, as GDScript
				// does, then reading, changing and writing back with nothing between
				// them (spec §3.6).
				const Expr &target = *p_stmt.target;
				Place place = place_of(target, false);
				uint32_t value = lower_expr(*p_stmt.value);
				if (target.kind == ExprKind::Subscript) {
					place.at = index_of(target);
				}
				Type type = p_stmt.value->type;
				open(place);
				if (p_stmt.assign_op == TokenKind::Equal) {
					if (place.kind == Place::Kind::SelfProperty || place.kind == Place::Kind::Owner || place.kind == Place::Kind::ObjectProperty) {
						value = outward(value, type, target.type.kind == TypeKind::Object); // The engine keeps it.
					} else if (place.kind == Place::Kind::Element) {
						value = outward(value, type, true);
					}
				} else if (p_stmt.assign_op == TokenKind::PercentEqual && is_format_string(target.type)) {
					value = format(load(place), target.type, value, type);
					type = Type::of(TypeKind::String);
				} else {
					value = binary(compound_operator(p_stmt.assign_op), load(place), value, p_stmt.compound_result);
					type = p_stmt.compound_result;
				}
				value = convert(value, type, target.type);
				if (place.kind == Place::Kind::Local) {
					assign_local(place.reg, value, first);
				} else {
					store(place, value);
				}
				return;
			}
			case StmtKind::If: {
				const uint32_t condition = lower_expr(*p_stmt.condition);
				release(temps_since(first));
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
				// The value matched and what the patterns and guards compute are
				// released once a branch is chosen; a branch's bindings belong to
				// its block, or are released when it fails.
				const uint32_t matched = reg_of(p_stmt.local);
				move(matched, lower_expr(*p_stmt.value));
				release(temps_since(first));
				std::vector<uint32_t> decision = { matched };
				const uint32_t end_block = new_block();
				for (const MatchBranch &arm : p_stmt.branches) {
					const uint32_t arm_first = uint32_t(fn.regs.size());
					std::vector<int> values;
					std::vector<int> bound;
					for (const Pattern &pattern : arm.patterns) {
						pattern_locals(pattern, true, values);
						pattern_locals(pattern, false, bound);
					}
					std::vector<uint32_t> bindings;
					for (int local : bound) {
						bindings.push_back(reg_of(local));
					}
					for (int local : values) {
						decision.push_back(reg_of(local));
					}
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
					const std::vector<uint32_t> computed = temps_since(arm_first);
					decision.insert(decision.end(), computed.begin(), computed.end());
					release(decision);
					lower_block(arm.body, bindings);
					jump(end_block);
					current = next_block;
					release(bindings);
				}
				if (p_stmt.exhaustive && p_stmt.value->type.kind == TypeKind::Enum && !p_stmt.value->type.values->is_engine) {
					// No value comes here, since values of the enum are checked where
					// they come in (spec §1.5); a definite error all the same, rather
					// than running off the end of a function with a return type.
					instr(Op::CHECK_ENUM, NO_REG, matched, NO_REG, fn.regs[matched].type);
				}
				release(decision);
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
				const uint32_t condition = lower_expr(*p_stmt.condition);
				release(temps_since(first));
				branch(condition, body_block, end_block);
				current = body_block;
				loops.push_back({ condition_block, end_block, scopes.size() });
				lower_block(p_stmt.body);
				loops.pop_back();
				jump(condition_block);
				current = end_block;
				return;
			}
			case StmtKind::For: {
				if (!p_stmt.iteration.empty()) {
					lower_iteration(p_stmt, first);
					return;
				}
				// Counts from 0 up to the int as it was on entry, or over the
				// container as it was on entry, by index and up to its current
				// size, as GDScript does. The loop variable gets the count or the
				// element afresh on each iteration.
				// Over range(...), it counts without making the array, as GDScript
				// does (modules/gdscript/gdscript_compiler.cpp:2044-2079; ed1daf0),
				// the variable being from + count * step, as they were on entry.
				const Expr *ranged = p_stmt.value->target == Target::Range ? p_stmt.value.get() : nullptr;
				const bool counting = ranged || p_stmt.value->type.kind == TypeKind::Int;
				const std::optional<std::pair<Type, Type>> entries = entries_of(p_stmt.value->type);
				const uint32_t source = counting ? temp(ValueType::Int) : temp(entries ? Type::array(entries->first) : p_stmt.value->type);
				uint32_t from = NO_REG;
				uint32_t by = NO_REG;
				if (ranged && ranged->args.size() > 1) {
					std::vector<uint32_t> bounds;
					for (const auto &arg : ranged->args) {
						bounds.push_back(lower_expr(*arg));
					}
					from = temp(ValueType::Int);
					move(from, bounds[0]);
					if (bounds.size() == 3) {
						by = temp(ValueType::Int);
						move(by, bounds[2]);
					}
					range(*ranged, source, bounds);
				} else if (ranged) {
					move(source, lower_expr(*ranged->args[0]));
				} else {
					// A Dictionary is iterated over its keys as they were on entry.
					move(source, entries ? keys_of(lower_expr(*p_stmt.value), entries->first) : lower_expr(*p_stmt.value));
				}
				release(temps_since(first, source));
				// A loop variable that the body does not assign is the count itself.
				const uint32_t local = reg_of(p_stmt.local);
				const bool counts_in_local = counting && from == NO_REG && fn.regs[local].type == type_index(ValueType::Int) && !assigns(p_stmt.body, p_stmt.local);
				const uint32_t counter = counts_in_local ? local : temp(ValueType::Int);
				load_int(counter, -1);
				const uint32_t step_block = new_block();
				const uint32_t body_block = new_block();
				const uint32_t end_block = new_block();
				jump(step_block);
				ir::Block &step = fn.blocks[step_block];
				step.terminator = ir::Terminator::Iterate;
				step.target = body_block;
				step.else_target = end_block;
				step.step.op = counting ? Op::ITERATE_INT : Op::ITERATE;
				step.step.dst = counting ? counter : local;
				step.step.a = source;
				step.step.b = counting ? NO_REG : counter;
				current = body_block;
				if (from != NO_REG) {
					uint32_t offset = counter;
					if (by != NO_REG) {
						offset = temp(ValueType::Int);
						instr(Op::MUL_INT, offset, counter, by);
					}
					instr(Op::ADD_INT, local, from, offset);
				} else if (counting && !counts_in_local) {
					move(local, counter);
				}
				if (!counting) {
					check_object(local, p_stmt.element);
				}
				loops.push_back({ step_block, end_block, scopes.size() });
				lower_block(p_stmt.body, { local });
				loops.pop_back();
				jump(step_block);
				current = end_block;
				release({ source });
				return;
			}
			case StmtKind::Return: {
				if (terminated()) {
					current = new_block();
				}
				if (p_stmt.value) {
					const uint32_t value = convert(lower_expr(*p_stmt.value), p_stmt.value->type, decl->return_type);
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
				release_scopes(loops.back().scope);
				jump(loops.back().break_block);
				return;
			case StmtKind::Continue:
				release_scopes(loops.back().scope);
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
			case StmtKind::Breakpoint:
				instr(Op::BREAKPOINT, NO_REG);
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
		scopes.clear();
		presets.clear();
		current = new_block();
	}

	// The defaults of the arguments a call left out, first in the function (spec
	// §4.6): the call wrote into `fn.given` how many it passed.
	void lower_defaults(const FuncDecl &p_function) {
		const uint32_t self = p_function.of_struct && !p_function.is_static ? 1 : 0;
		for (size_t p = 0; p < p_function.params.size(); p++) {
			const ParamDecl &param = p_function.params[p];
			if (!param.default_value) {
				continue;
			}
			const uint32_t local = reg_of(int(self + p));
			const uint32_t passed = temp(ValueType::Bool);
			instr(Op::LT_INT, passed, int_constant(self + p), fn.given);
			const uint32_t compute = new_block();
			const uint32_t next = new_block();
			branch(passed, next, compute);
			current = compute;
			at = param.default_value->range;
			instr(Op::LINE, NO_REG, NO_REG, NO_REG, uint32_t(cls.source->line(param.default_value->range.first)));
			const uint32_t first = uint32_t(fn.regs.size());
			assign_local(local, convert(lower_expr(*param.default_value), param.default_value->type, param.type), first);
			release(temps_since(first));
			jump(next);
			current = next;
		}
	}

	void lower_field_default(const StructDecl &p_struct, const VarDecl &p_field, bool p_declarations_only) {
		start(field_default_name(p_struct, p_field));
		decl = nullptr;
		at = p_field.range;
		fn.result = slot(std::string(), p_field.type);
		fn.line = uint32_t(cls.source->line(p_field.name_token));
		if (!p_declarations_only) {
			instr(Op::LINE, NO_REG, NO_REG, NO_REG, fn.line);
			const uint32_t value = convert(lower_expr(*p_field.initializer), p_field.initializer->type, p_field.type);
			if (terminated()) {
				current = new_block();
			}
			fn.blocks[current].terminator = ir::Terminator::Return;
			fn.blocks[current].value = value;
		}
		finish();
		bytecode::Function out = ir::emit(fn, module);
		out.is_static = true;
		module.functions.push_back(std::move(out));
	}

	// The member initializers in declaration order (spec §6.1): those that run
	// on a new instance, or the @onready ones (§6.3). Lowered before the
	// functions, so that their code and the table
	// entries they use stay the same when only a function body changes.
	bool lower_initializers(bool p_onready, bytecode::Function &r_out) {
		bool any = false;
		start(p_onready ? "@ready" : "@init");
		decl = nullptr;
		// A node that becomes ready without the members the scene or the creator
		// must assign stops there, before the @onready initializers (spec §6.4).
		const auto must_assign = [](const VarDecl *p_member) { return p_member->must_assign; };
		if (p_onready && cls.is_node && std::any_of(cls.all_members.begin() + module.inherited_members, cls.all_members.end(), must_assign)) {
			any = true;
			instr(Op::CHECK_PROVIDED, NO_REG);
		}
		for (size_t m = module.inherited_members; m < cls.all_members.size(); m++) {
			const VarDecl &member = *cls.all_members[m];
			if (member.is_onready != p_onready || member.must_assign || (!member.initializer && !is_typed_container(member.type) && member.type.kind != TypeKind::Struct)) {
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
			store_member(uint32_t(m), convert(lower_expr(*member.initializer), member.initializer->type, member.type));
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
				init_static(v, with_initializer ? convert(lower_expr(*variable.initializer), variable.initializer->type, variable.type) : default_value(temp(variable.type)));
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
		return declares(cls, p_function);
	}

	// Whether the class declares the function, as one of its own or a method
	// of one of its structs.
	static bool declares(const ClassDecl &p_class, const FuncDecl &p_function) {
		const auto is = [&](const FuncDecl &p_own) { return &p_own == &p_function; };
		return std::any_of(p_class.functions.begin(), p_class.functions.end(), is) || std::any_of(p_class.structs.begin(), p_class.structs.end(), [&](const StructDecl &p_struct) {
			return std::any_of(p_struct.methods.begin(), p_struct.methods.end(), is);
		});
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
		if (cls.parent_ref) {
			module.parent_class = int32_t(class_index(cls.parent_type));
			module.parent_line = uint32_t(cls.source->line(cls.parent_ref->range.first));
		}
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
			member.must_assign = decl_member.must_assign;
			offset += member.tracks_assignment() ? 8 : 0;
			member.is_pub = decl_member.is_pub;
			member.is_export = decl_member.is_export;
			member.hint = decl_member.hint;
			member.hint_string = decl_member.hint_string;
			member.usage = decl_member.usage;
			// Its type's default (spec §4.3) when it has no initializer; an empty
			// Array[T] is typed too.
			member.initial.type = member.type;
			if (decl_member.initial) {
				member.has_initial = true;
				member.initial = constant_of(*decl_member.initial);
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
			// One naming an enum with payloads has no value (spec §3.7).
			if (constant->value && !(constant->value->names_enum && constant->type.kind == TypeKind::PayloadEnum)) {
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

		// Every default has its function, which decoding runs for a field left
		// out (spec §3.6); a construction runs only those that are no constant.
		for (const StructDecl &structure : cls.structs) {
			for (const VarDecl &field : structure.fields) {
				if (field.initializer) {
					field_defaults.emplace(&field, uint32_t(cls.all_functions.size() + field_defaults.size()));
				}
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
			// An instance method of a struct takes self first.
			fn.param_count = uint32_t(function.params.size()) + (function.of_struct && !function.is_static ? 1 : 0);
			fn.result = slot(std::string(), function.return_type);
			fn.is_async = function.is_async;
			fn.is_pub = function.is_pub;
			fn.is_override = function.is_override;
			for (const ClassDecl *owner = &cls; owner; owner = owner->base) {
				if (declares(*owner, function)) {
					fn.line = uint32_t(owner->source->line(function.name_token));
					break;
				}
			}
			for (const LocalInfo &local : function.locals) {
				fn.regs.push_back(slot(local.name, local.type));
			}
			if (std::any_of(function.params.begin(), function.params.end(), [](const ParamDecl &p_param) { return bool(p_param.default_value); })) {
				fn.given = temp(ValueType::Int);
			}
			if (!p_declarations_only && !inherited) {
				lower_defaults(function);
				lower_block(function.body);
			}
			finish();
			bytecode::Function out = ir::emit(fn, module);
			out.is_static = function.is_static || function.of_struct; // A struct's methods have no object.
			out.rpc = function.rpc;
			out.is_abstract = function.is_abstract;
			out.is_inherited = inherited;
			if (inherited || function.is_abstract) {
				out.code.clear();
			}
			const auto declared_signature = [&](const FuncDecl &p_decl) {
				std::vector<bytecode::Slot> params;
				if (p_decl.of_struct && !p_decl.is_static) {
					params.push_back(slot("self", p_decl.locals[0].type));
				}
				size_t defaults = 0;
				for (const ParamDecl &param : p_decl.params) {
					params.push_back(slot(param.name, param.type));
					if (param.default_value) {
						++defaults;
					}
				}
				return bytecode::signature_key(module.types, params, defaults, slot("", p_decl.return_type), p_decl.is_async, p_decl.has_rest());
			};
			if (cls.base) {
				for (const FuncDecl *parent : cls.base->all_functions) {
					if (parent->name == function.name) {
						out.base_signature = declared_signature(*parent);
					}
				}
			}
			for (const FuncDecl *required : cls.interface_methods) {
				if (required->name == function.name && declared_signature(*required) != declared_signature(function)) {
					out.interface_signatures.push_back(declared_signature(*required));
				}
			}
			out.is_vararg = function.has_rest();
			for (const ParamDecl &param : function.params) {
				if (param.default_value) {
					// NIL for one that is no constant, as GDScript lists it
					// (modules/gdscript/gdscript_analyzer.cpp:1795-1798).
					const ConstPtr value = converted(param.default_value->constant, param.type);
					out.defaults.push_back(value && !loads(*value) ? constant_of(*value) : bytecode::Constant{ type_index(Type::of(TypeKind::Variant)) });
				}
			}
			module.functions.push_back(std::move(out));
		}
		for (const StructDecl &structure : cls.structs) {
			for (const VarDecl &field : structure.fields) {
				if (field_defaults.count(&field)) {
					lower_field_default(structure, field, p_declarations_only);
				}
			}
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
