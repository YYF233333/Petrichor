#include "compiler/lower.h"

#include "compiler/builtin_api.h"
#include "compiler/ir.h"

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

// Variant::Operator (core/variant/variant.h:564-597).
enum EngineOperator : uint8_t {
	OP_EQUAL,
	OP_NOT_EQUAL,
	OP_LESS,
	OP_LESS_EQUAL,
	OP_GREATER,
	OP_GREATER_EQUAL,
	OP_ADD,
	OP_SUBTRACT,
	OP_MULTIPLY,
	OP_DIVIDE,
	OP_NEGATE,
	OP_POSITIVE,
	OP_MODULE,
	OP_UNKNOWN = 0xFF, // Not one of the engine's.
};

uint8_t engine_operator(TokenKind p_op) {
	switch (p_op) {
		case TokenKind::EqualEqual:
			return OP_EQUAL;
		case TokenKind::BangEqual:
			return OP_NOT_EQUAL;
		case TokenKind::Less:
			return OP_LESS;
		case TokenKind::LessEqual:
			return OP_LESS_EQUAL;
		case TokenKind::Greater:
			return OP_GREATER;
		case TokenKind::GreaterEqual:
			return OP_GREATER_EQUAL;
		case TokenKind::Plus:
			return OP_ADD;
		case TokenKind::Minus:
			return OP_SUBTRACT;
		case TokenKind::Star:
			return OP_MULTIPLY;
		case TokenKind::Slash:
			return OP_DIVIDE;
		case TokenKind::Percent:
			return OP_MODULE;
		default:
			return OP_UNKNOWN;
	}
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
		Member,
		MemberField, // A field of a struct in a member that has no accessors, at `offset` in it.
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
	Type type;
	std::optional<bytecode::IntRange> int_range; // An engine property: what its setter takes (spec §10.6).
	std::unique_ptr<Place> parent; // Field, StructCopy and Element: where the copy goes back.
};

class Lowerer {
	const ClassDecl &cls;
	bytecode::Module &module;
	std::vector<Diagnostic> &errors;
	const StructLookup &structs;
	std::unordered_map<uint32_t, Type> struct_types; // By their index in the type table.
	std::unordered_set<std::string> interning;

	ir::Function fn;
	const FuncDecl *decl = nullptr; // Null in the init function.
	uint32_t current = 0;
	int line = 0; // Of the statement, member or function being lowered.

	struct Loop {
		uint32_t continue_block;
		uint32_t break_block;
	};
	std::vector<Loop> loops;

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
			errors.push_back({ line, 1, p_struct.name + " cannot be compiled: " + p_struct.script + " has errors." });
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

	const StructDecl *struct_decl(const Type &p_struct) const {
		return p_struct.script == cls.path ? cls.find_struct(p_struct.interface_name()) : (structs ? structs(p_struct) : nullptr);
	}

	bool is_struct_reg(uint32_t p_reg) const { return module.types[fn.regs[p_reg].type].kind == bytecode::TypeKind::Struct; }

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

	// A literal, or a constant the checker found (is_constant()), as the target's type.
	bytecode::Constant constant_of(const Expr &p_expr, const Type &p_target) {
		if (p_target.kind == TypeKind::Option && p_expr.kind != ExprKind::NoneLiteral) {
			return constant_of(p_expr, p_target.args[0]);
		}
		bytecode::Constant constant;
		constant.type = type_index(p_target);
		if (p_expr.target == Target::BuiltinConstant) {
			constant.builtin = p_expr.text;
			return constant;
		}
		if (p_expr.target == Target::EngineConstant) {
			constant.i = p_expr.int_value;
			constant.f = double(constant.i);
			return constant;
		}
		if (p_expr.target == Target::BuiltinConstructor) {
			constant.constructor = p_expr.index;
			for (size_t i = 0; i < p_expr.args.size(); i++) {
				constant.args.push_back(constant_of(*p_expr.args[i], p_expr.param_types[i]));
			}
			return constant;
		}
		if (p_expr.kind == ExprKind::ArrayLiteral) {
			for (size_t i = 0; i < p_expr.args.size(); i++) {
				constant.args.push_back(constant_of(*p_expr.args[i], stored_element_type(p_expr, i)));
			}
			return constant;
		}
		const Expr *literal = &p_expr;
		bool negate = false;
		if (literal->kind == ExprKind::Unary) {
			negate = true;
			literal = literal->left.get();
		}
		switch (literal->kind) {
			case ExprKind::IntLiteral:
				// Negated through unsigned, so that the smallest int wraps as at run time.
				constant.i = negate ? int64_t(0ull - uint64_t(literal->int_value)) : literal->int_value;
				constant.f = double(constant.i);
				break;
			case ExprKind::FloatLiteral:
				constant.f = negate ? -literal->float_value : literal->float_value;
				break;
			case ExprKind::StringLiteral:
				constant.s = literal->text;
				break;
			case ExprKind::BoolLiteral:
				constant.b = literal->bool_value;
				break;
			default:
				break;
		}
		return constant;
	}

	// What the checker passed but lowering has no code for is a defect of the
	// compiler: it is reported instead of compiled, and lowering goes on with
	// a register of its own.
	uint32_t internal_error(const std::string &p_what) {
		errors.push_back({ line, 1, "Internal compiler error: " + p_what + "." });
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

	uint32_t engine_call(const std::string &p_method, uint32_t p_result_type) {
		return intern(module.engine_calls, bytecode::EngineCall{ p_method, p_result_type }, [](const auto &a, const auto &b) {
			return a.method == b.method && a.result_type == b.result_type;
		});
	}

	uint32_t method_call(const bytecode::MethodCall &p_call) {
		return intern(module.method_calls, p_call, [&](const auto &a, const auto &b) {
			return a.method == b.method && bytecode::signature_key(module.types, a.params, a.defaults, a.result, a.is_async) == bytecode::signature_key(module.types, b.params, b.defaults, b.result, b.is_async);
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
			return a.base == b.base && a.method == b.method && a.hash == b.hash && a.is_vararg == b.is_vararg && a.params == b.params && a.result_type == b.result_type;
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

	// Converts int to float (spec §3.2), and between String, StringName and
	// NodePath (§10.7), where the target type asks for it.
	uint32_t convert(uint32_t p_reg, const Type &p_to) {
		if (p_reg == NO_REG) {
			return p_reg;
		}
		const ValueType from = reg_type(p_reg);
		const ValueType to = value_type(p_to);
		if (p_to.kind == TypeKind::Option && !is_option_reg(p_reg)) {
			if (from == ValueType::Variant) {
				return default_value(temp(p_to)); // The None literal.
			}
			// T becomes Some(T) (spec §3.4).
			const uint32_t inner = convert(p_reg, p_to.args[0]);
			const uint32_t result = temp(p_to);
			instr(Op::SOME, result, inner, NO_REG, 0, fn.regs[result].type);
			return result;
		}
		if (from == to || to == ValueType::Variant || to == ValueType::Void) {
			return p_reg;
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

	// A string literal where a StringName or a NodePath is expected is built
	// at compile time (spec §10.7).
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
		const ValueType to = value_type(p_param);
		if (p_arg.kind == ExprKind::StringLiteral && (to == ValueType::StringName || to == ValueType::NodePath)) {
			return load_text(to, p_arg.text);
		}
		const uint32_t value = lower_expr(p_arg);
		return convert(p_outward ? outward(value, p_arg.type, p_takes_object) : value, p_param);
	}

	std::vector<uint32_t> arguments(const Expr &p_call) {
		// A function of this language reads its T? parameters itself.
		const bool outward = p_call.target != Target::ScriptFunction && p_call.target != Target::ScriptMethod && p_call.target != Target::SuperMethod;
		// Callable.bind() and Signal.emit() take Variant, whatever the types here.
		const bool as_variant = p_call.target == Target::BindArguments || p_call.target == Target::EmitSignal;
		std::vector<uint32_t> args;
		for (size_t i = 0; i < p_call.args.size(); i++) {
			const Type &param = p_call.param_types[i];
			args.push_back(argument(*p_call.args[i], param, outward, !as_variant && param.kind == TypeKind::Object));
			// A key or an element of a container that keeps it boxed.
			if (p_call.target == Target::BuiltinMethod && i < p_call.engine_params.size() && p_call.engine_params[i].kind == TypeKind::Variant && is_boxed(param)) {
				args.back() = box(args.back(), is_key_argument(p_call, i));
			}
		}
		return args;
	}

	uint32_t load_text(ValueType p_type, const std::string &p_text) {
		const uint32_t result = temp(p_type);
		instr(Op::CONST_VALUE, result, NO_REG, NO_REG, text_constant(p_type, p_text), fn.regs[result].type);
		return result;
	}

	// A constant of the type that has only its default value, such as 0 or None.
	uint32_t default_constant(uint32_t p_type) {
		bytecode::Constant constant;
		constant.type = p_type;
		return add_constant(constant);
	}

	void load_int(uint32_t p_dst, int64_t p_value) {
		bytecode::Constant constant;
		constant.type = type_index(ValueType::Int);
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
	uint32_t engine_builtin_method(ValueType p_base, const char *p_method, std::vector<ValueType> p_params, ValueType p_return, bool p_vararg = false, uint32_t p_result_type = NO_REG) {
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

	// Whether the argument goes to a Dictionary as a key.
	static bool is_key_argument(const Expr &p_call, size_t p_index) {
		const builtin::Class *type = p_call.owner == "Dictionary" ? builtin::find_class("Dictionary") : nullptr;
		const builtin::Method *method = type ? builtin::find_method(*type, p_call.text) : nullptr;
		return method && p_index < method->argument_count && std::string(method->arguments[p_index].name) == "key";
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

	// get(k) of Dictionary[K, V]: V?, None for a missing key (spec §3.8).
	uint32_t get_or_none(const Expr &p_call) {
		const uint32_t dictionary = lower_expr(*p_call.base);
		const auto [key_type, value_type] = *entries_of(p_call.base->type);
		const uint32_t key = stored(*p_call.args[0], key_type, key_type, true);
		const uint32_t has = call_builtin(engine_builtin_method(ValueType::Dictionary, "has", { ValueType::Variant }, ValueType::Bool), dictionary, { key }, temp(ValueType::Bool));
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
		if (is_option_reg(p_dst)) {
			instr(Op::CONST_VALUE, p_dst, NO_REG, NO_REG, default_constant(fn.regs[p_dst].type), fn.regs[p_dst].type); // None.
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
			case ValueType::Object:
			case ValueType::Variant:
				// None; a constant of an object type or of T? is always None.
				instr(Op::CONST_VALUE, p_dst, NO_REG, NO_REG, default_constant(fn.regs[p_dst].type), fn.regs[p_dst].type);
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
		if (is_struct_reg(p_left)) {
			// Field by field (spec §3.6).
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
			case TokenKind::Percent:
				op = Op::MOD_INT;
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
			case Target::NewObject: {
				const uint32_t result = temp(p_call.type);
				instr(Op::NEW_OBJECT, result, NO_REG, NO_REG, class_index(p_call.owner));
				return result;
			}
			case Target::StructConstructor:
				return construct_struct(p_call.type, &p_call);
			case Target::Unwrap: {
				const uint32_t value = lower_expr(*p_call.base);
				const uint32_t message = p_call.args.empty() ? NO_REG : convert(lower_expr(*p_call.args[0]), Type::of(TypeKind::String));
				const uint32_t result = temp(p_call.type);
				instr(Op::UNWRAP, result, value, message, 0, fn.regs[result].type);
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
		if ((p_call.target == Target::EngineMethod && p_call.base) || p_call.target == Target::ScriptMethod || p_call.target == Target::BindArguments || p_call.target == Target::EmitSignal) {
			i.base = lower_expr(*p_call.base);
		} else if (p_call.target == Target::BuiltinMethod) {
			if (!p_call.is_const && !is_shared(p_call.base->type) && is_place(*p_call.base)) {
				// A method that changes its base changes the place it came from.
				written_back = std::make_unique<Place>(place_of(*p_call.base));
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
				i.imm = method_call(method);
				break;
			}
			case Target::EngineMethod:
				i.op = Op::CALL_ENGINE;
				i.imm = engine_call(p_call.text, type_index(filtered ? p_call.engine_return : p_call.type));
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
				method.base = reg_type(i.base);
				method.method = p_call.text;
				method.hash = p_call.hash;
				method.is_vararg = p_call.is_vararg;
				method.is_const = p_call.is_const;
				method.params = value_types(p_call.engine_params);
				method.return_type = value_type(p_call.engine_return);
				method.result_type = type_index(p_call.type);
				i.imm = builtin_method(method);
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

	uint32_t lower_expr(const Expr &p_expr) {
		switch (p_expr.kind) {
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
			case ExprKind::IntLiteral:
			case ExprKind::FloatLiteral:
			case ExprKind::StringLiteral: {
				const ValueType type = value_type(p_expr.type);
				const uint32_t result = temp(type);
				const Op op = type == ValueType::Int ? Op::CONST_INT : (type == ValueType::Float ? Op::CONST_FLOAT : Op::CONST_STRING);
				instr(op, result, NO_REG, NO_REG, add_constant(constant_of(p_expr, p_expr.type)));
				return result;
			}
			case ExprKind::BoolLiteral: {
				const uint32_t result = temp(ValueType::Bool);
				instr(Op::CONST_BOOL, result, NO_REG, NO_REG, p_expr.bool_value ? 1 : 0);
				return result;
			}
			case ExprKind::Name:
			case ExprKind::Member: {
				if (p_expr.target == Target::EngineConstant) {
					const uint32_t result = temp(ValueType::Int);
					load_int(result, p_expr.int_value);
					return result;
				}
				if (p_expr.target == Target::Autoload) {
					const uint32_t result = temp(p_expr.type);
					instr(Op::GET_AUTOLOAD, result, NO_REG, NO_REG, text_constant(ValueType::StringName, p_expr.text));
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
				if (p_expr.target == Target::BuiltinConstant) {
					const uint32_t result = temp(value_type(p_expr.type));
					instr(Op::CONST_VALUE, result, NO_REG, NO_REG, add_constant(constant_of(p_expr, p_expr.type)), fn.regs[result].type);
					return result;
				}
				// A local, narrowed or not, a member, a property or a field.
				return load(place_of(p_expr));
			}
			case ExprKind::Unary: {
				const uint32_t operand = lower_expr(*p_expr.left);
				const ValueType type = reg_type(operand);
				const uint32_t result = temp(p_expr.type);
				if (p_expr.op == TokenKind::Not) {
					instr(Op::NOT, result, operand);
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
				if (p_expr.op == TokenKind::And || p_expr.op == TokenKind::Or) {
					return short_circuit(p_expr);
				} else {
					const uint32_t left = lower_expr(*p_expr.left);
					const uint32_t right = lower_expr(*p_expr.right);
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
			case ExprKind::NoneLiteral:
				return default_value(temp(p_expr.type));
			case ExprKind::TypeTest: {
				const uint32_t source = lower_expr(*p_expr.left);
				const uint32_t result = temp(ValueType::Bool);
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
				const uint32_t value = lower_expr(*p_expr.left);
				if (!p_expr.type.nullable) {
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
		const uint32_t path_reg = load_text(ValueType::NodePath, p_expr.text);
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
				place.reg = field_view(p_base.reg, p_field);
				place.whole = p_base.whole;
				place.parent = std::move(p_base.parent);
				return place;
			default:
				break;
		}
		place.kind = Place::Kind::StructCopy;
		place.whole = load(p_base);
		place.reg = field_view(place.whole, p_field);
		place.parent = std::make_unique<Place>(std::move(p_base));
		return place;
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
				place.reg = load(*place.parent);
			} else {
				place.reg = lower_expr(*p_expr.base);
			}
			place.at = index_of(p_expr);
			return place;
		}
		switch (p_expr.target) {
			case Target::Local:
				place.reg = uint32_t(p_expr.index);
				place.kind = is_option_reg(place.reg) && p_expr.type.kind != TypeKind::Option ? Place::Kind::NarrowedLocal : Place::Kind::Local;
				break;
			case Target::Member:
				place.kind = Place::Kind::Member;
				place.index = uint32_t(p_expr.index);
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
					place.reg = load(*place.parent);
				} else {
					place.reg = lower_expr(*p_expr.base); // Only read: a member of a temporary value.
				}
				place.index = field(reg_type(place.reg), p_expr.text, value_type(p_expr.type));
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
				const uint32_t result = temp(p_place.type);
				instr(Op::UNWRAP, result, p_place.reg, NO_REG, 0, fn.regs[result].type);
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
			case Place::Kind::NarrowedLocal:
				instr(Op::SOME, p_place.reg, p_value, NO_REG, 0, fn.regs[p_place.reg].type);
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

	// Jumps to `p_matched` when the matched value fits the pattern, and goes on
	// in a new block when it does not.
	void lower_pattern(const Pattern &p_pattern, uint32_t p_matched) {
		if (p_pattern.kind != PatternKind::Value) {
			jump(p_matched);
			current = new_block();
			return;
		}
		const uint32_t next_block = new_block();
		branch(lower_expr(*p_pattern.value), p_matched, next_block);
		current = next_block;
	}

	void lower_stmt(const Stmt &p_stmt) {
		line = p_stmt.line;
		instr(Op::LINE, NO_REG, NO_REG, NO_REG, uint32_t(p_stmt.line));
		switch (p_stmt.kind) {
			case StmtKind::Var: {
				const uint32_t local = uint32_t(p_stmt.local);
				if (p_stmt.value) {
					move(local, convert(lower_expr(*p_stmt.value), decl->locals[local].type));
				} else {
					default_value(local);
				}
				return;
			}
			case StmtKind::Assign: {
				const Expr &target = *p_stmt.target;
				const Place place = place_of(target);
				uint32_t value;
				if (p_stmt.assign_op == TokenKind::Equal) {
					value = lower_expr(*p_stmt.value);
					if (place.kind == Place::Kind::SelfProperty || place.kind == Place::Kind::Owner || place.kind == Place::Kind::ObjectProperty) {
						value = outward(value, p_stmt.value->type, target.type.kind == TypeKind::Object); // The engine keeps it.
					} else if (place.kind == Place::Kind::Element) {
						value = outward(value, p_stmt.value->type, true);
					}
				} else {
					const uint32_t current_value = load(place);
					value = binary(compound_operator(p_stmt.assign_op), current_value, lower_expr(*p_stmt.value), p_stmt.compound_result);
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
				const uint32_t matched = uint32_t(p_stmt.local);
				move(matched, lower_expr(*p_stmt.value));
				const uint32_t end_block = new_block();
				for (const MatchBranch &arm : p_stmt.branches) {
					const uint32_t body_block = new_block();
					const uint32_t next_block = new_block();
					for (const Pattern &pattern : arm.patterns) {
						lower_pattern(pattern, body_block);
					}
					jump(next_block);
					current = body_block;
					for (const Pattern &pattern : arm.patterns) {
						if (pattern.kind == PatternKind::Bind) {
							move(uint32_t(pattern.local), matched);
						}
					}
					if (arm.guard) {
						const uint32_t guarded_block = new_block();
						branch(lower_expr(*arm.guard), guarded_block, next_block);
						current = guarded_block;
					}
					lower_block(arm.body);
					jump(end_block);
					current = next_block;
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
				move(uint32_t(p_stmt.local), counting ? counter : get_index(source, counter, p_stmt.element));
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
			case StmtKind::Pass:
				return;
			case StmtKind::Expression:
				lower_expr(*p_stmt.value);
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
					line = cls.all_members[m]->line;
					instr(Op::LINE, NO_REG, NO_REG, NO_REG, uint32_t(cls.all_members[m]->line));
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
			line = member.line;
			instr(Op::LINE, NO_REG, NO_REG, NO_REG, uint32_t(member.line));
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
			member.line = uint32_t(decl_member.line);
			member.offset = offset;
			offset += bytecode::type_size(module.types, member.type);
			member.is_pub = decl_member.is_pub;
			member.is_export = decl_member.is_export;
			member.hint = decl_member.hint;
			member.hint_string = decl_member.hint_string;
			// Its type's default (spec §4.3) when it has no initializer; an empty
			// Array[T] is typed too.
			member.initial.type = member.type;
			if (decl_member.constant_initializer) {
				member.has_initial = true;
				member.initial = constant_of(*decl_member.initializer, decl_member.type);
			} else if (!decl_member.initializer && !decl_member.must_assign && decl_member.type.kind != TypeKind::Struct) {
				member.has_initial = true;
			}
			module.members.push_back(member);
		}
		module.instance_size = offset;
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

		bytecode::Function initializers;
		bytecode::Function ready_initializers;
		const bool has_initializers = !p_declarations_only && lower_initializers(false, initializers);
		const bool has_ready_initializers = !p_declarations_only && lower_initializers(true, ready_initializers);
		record_init_tables();

		for (const FuncDecl *entry : cls.all_functions) {
			const FuncDecl &function = *entry;
			bool inherited = true;
			for (const FuncDecl &own : cls.functions) {
				if (&own == entry) {
					inherited = false;
				}
			}
			start(function.name);
			decl = &function;
			line = function.line;
			fn.param_count = uint32_t(function.params.size());
			fn.result = slot(std::string(), function.return_type);
			fn.is_async = function.is_async;
			fn.is_pub = function.is_pub;
			fn.is_override = function.is_override;
			fn.is_exposed = function.is_exposed || cls.exposed_methods.count(function.name);
			fn.line = uint32_t(function.line);
			for (const LocalInfo &local : function.locals) {
				fn.regs.push_back(slot(local.name, local.type));
			}
			if (!p_declarations_only && !inherited) {
				lower_block(function.body);
			}
			finish();
			bytecode::Function out = ir::emit(fn, module);
			out.is_static = function.is_static;
			out.is_inherited = inherited;
			if (inherited) {
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
					out.defaults.push_back(constant_of(*param.default_value, param.type));
				}
			}
			module.functions.push_back(std::move(out));
		}

		if (has_initializers) {
			module.functions.push_back(initializers);
			module.init_function = int32_t(module.functions.size() - 1);
		}
		if (has_ready_initializers) {
			module.functions.push_back(ready_initializers);
			module.ready_function = int32_t(module.functions.size() - 1);
		}
	}
};

} // namespace

bytecode::Module lower(const ClassDecl &p_class, bool p_declarations_only, std::vector<Diagnostic> &r_errors, const StructLookup &p_structs) {
	bytecode::Module module;
	Lowerer(p_class, module, r_errors, p_structs).run(p_declarations_only);
	return module;
}

} // namespace petrichor::compiler
