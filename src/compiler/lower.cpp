#include "compiler/lower.h"

#include "compiler/builtin_api.h"
#include "compiler/ir.h"

#include <memory>

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
		case TypeKind::Object:
			return ValueType::Object;
		case TypeKind::Option: // A Variant that is NIL for None (spec §3.4).
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

bytecode::Slot slot_of(const std::string &p_name, const Type &p_type) {
	bytecode::Slot slot;
	slot.name = p_name;
	slot.type = value_type(p_type);
	// The engine's class, and for a class of this language its script; for an
	// interface, the script that declares it and its name, as the runtime
	// names interfaces.
	const auto object_class = [&](const Type &p_object) {
		slot.class_name = p_object.engine_class();
		if (p_object.is_interface) {
			slot.interface_name = interface_id(p_object);
		} else {
			slot.script = p_object.script;
		}
	};
	if (p_type.kind == TypeKind::Object) {
		object_class(p_type);
		slot.nullable = p_type.nullable;
	} else if (p_type.kind == TypeKind::Option) {
		slot.nullable = true;
		slot.option = value_type(p_type.args[0]);
	} else if (p_type.kind == TypeKind::Builtin && !p_type.args.empty()) {
		// Array[T].
		slot.element = value_type(p_type.args[0]);
		if (p_type.args[0].kind == TypeKind::Object) {
			object_class(p_type.args[0]);
		}
	}
	return slot;
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

// An array constant is typed as the slot it goes to.
void type_array(bytecode::Constant &r_constant, const bytecode::Slot &p_slot) {
	r_constant.element = p_slot.element;
	r_constant.class_name = p_slot.class_name;
	r_constant.script = p_slot.script;
}

// A literal, or a constant the checker found (is_constant()), as the target's type.
bytecode::Constant constant_of(const Expr &p_expr, const Type &p_target) {
	bytecode::Constant constant;
	constant.type = value_type(p_target);
	if (p_expr.target == Target::BuiltinConstant) {
		constant.builtin = p_expr.text;
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
		type_array(constant, slot_of(std::string(), p_target));
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

// A place that can be read and written. The base of a member access is
// evaluated once; a member of a built-in value is changed in a copy of the
// value, which is then written back to where it came from.
struct Place {
	enum class Kind {
		Local,
		NarrowedLocal, // A local of type T? where it is known to hold a T (spec §5.4).
		Member,
		SelfProperty,
		ObjectProperty,
		Field,
	};
	Kind kind = Kind::Local;
	uint32_t reg = NO_REG; // Local: the local; ObjectProperty: the object; Field: the copy of the base.
	uint32_t index = 0; // Member, property table or field table.
	Type type;
	std::optional<bytecode::IntRange> int_range; // An engine property: what its setter takes (spec §10.6).
	std::unique_ptr<Place> parent; // Field: where the copy goes back.
};

class Lowerer {
	const ClassDecl &cls;
	bytecode::Module &module;
	std::vector<Diagnostic> &errors;

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
		fn.regs.push_back(slot_of(std::string(), p_type));
		return uint32_t(fn.regs.size() - 1);
	}

	uint32_t temp(ValueType p_type) {
		bytecode::Slot slot;
		slot.type = p_type;
		fn.regs.push_back(slot);
		return uint32_t(fn.regs.size() - 1);
	}

	ValueType reg_type(uint32_t p_reg) const { return fn.regs[p_reg].type; }

	// What the checker passed but lowering has no code for is a defect of the
	// compiler: it is reported instead of compiled, and lowering goes on with
	// a register of its own.
	uint32_t internal_error(const std::string &p_what) {
		errors.push_back({ line, 1, "Internal compiler error: " + p_what + "." });
		return temp(ValueType::Variant);
	}

	void instr(Op p_op, uint32_t p_dst, uint32_t p_a = NO_REG, uint32_t p_b = NO_REG, uint32_t p_imm = 0, ValueType p_type = ValueType::Void) {
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
			instr(Op::MOVE_VALUE, p_dst, p_src, NO_REG, 0, type);
		}
	}

	// The instruction of a family for how a value of this type is stored: in
	// 8 bytes, as a String, or as another value.
	static Op by_storage(ValueType p_type, Op p_8_byte, Op p_string, Op p_value) {
		return is_8_byte(p_type) ? p_8_byte : (p_type == ValueType::String ? p_string : p_value);
	}

	uint32_t load_member(uint32_t p_member) {
		const uint32_t result = temp(cls.members[p_member].type);
		const ValueType type = reg_type(result);
		instr(by_storage(type, Op::LOAD_MEMBER_8, Op::LOAD_MEMBER_STRING, Op::LOAD_MEMBER_VALUE), result, NO_REG, NO_REG, p_member, type);
		// An export the scene did not assign (spec §7.5), or an @onready
		// member read before the node is ready (§6.3), is still None.
		const VarDecl &member = cls.members[p_member];
		if (member.must_assign || (member.is_onready && member.type.kind == TypeKind::Object && !member.type.nullable)) {
			instr(Op::CHECK_ASSIGNED, NO_REG, result, NO_REG, p_member);
		}
		return result;
	}

	void store_member(uint32_t p_member, uint32_t p_value) {
		const ValueType type = reg_type(p_value);
		instr(by_storage(type, Op::STORE_MEMBER_8, Op::STORE_MEMBER_STRING, Op::STORE_MEMBER_VALUE), NO_REG, p_value, NO_REG, p_member, type);
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
		bytecode::Constant constant{ p_type };
		constant.s = p_text;
		return add_constant(constant);
	}

	uint32_t engine_call(const std::string &p_method, ValueType p_return) {
		return intern(module.engine_calls, bytecode::EngineCall{ p_method, p_return }, [](const auto &a, const auto &b) {
			return a.method == b.method && a.return_type == b.return_type;
		});
	}

	uint32_t method_call(const bytecode::MethodCall &p_call) {
		return intern(module.method_calls, p_call, [](const auto &a, const auto &b) {
			return a.method == b.method && bytecode::signature_key(a.params, a.defaults, a.result) == bytecode::signature_key(b.params, b.defaults, b.result);
		});
	}

	uint32_t property(const std::string &p_name, ValueType p_type) {
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
		bytecode::ClassRef ref;
		ref.name = p_type.engine_class();
		ref.script = p_type.script;
		if (p_type.is_interface) {
			ref.interface_name = p_type.interface_name();
		}
		return class_index(ref);
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
		if (p_to.kind == TypeKind::Option && from != ValueType::Variant) {
			// T becomes Some(T) (spec §3.4).
			const uint32_t inner = convert(p_reg, p_to.args[0]);
			const uint32_t result = temp(p_to);
			instr(Op::TO_VARIANT, result, inner, NO_REG, 0, reg_type(inner));
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
	// every read of a T? does (spec §3.3).
	uint32_t live_or_none(uint32_t p_reg, const Type &p_type) {
		if (p_type.kind != TypeKind::Object || !p_type.nullable) {
			return p_reg;
		}
		const uint32_t result = temp(p_type);
		instr(Op::CAST_OBJECT, result, p_reg, NO_REG, class_index(p_type));
		return result;
	}

	uint32_t argument(const Expr &p_arg, const Type &p_param, bool p_outward) {
		const ValueType to = value_type(p_param);
		if (p_arg.kind == ExprKind::StringLiteral && (to == ValueType::StringName || to == ValueType::NodePath)) {
			return load_text(to, p_arg.text);
		}
		const uint32_t value = lower_expr(p_arg);
		return convert(p_outward ? live_or_none(value, p_arg.type) : value, p_param);
	}

	std::vector<uint32_t> arguments(const Expr &p_call) {
		// A function of this language reads its T? parameters itself.
		const bool outward = p_call.target != Target::ScriptFunction && p_call.target != Target::ScriptMethod;
		std::vector<uint32_t> args;
		for (size_t i = 0; i < p_call.args.size(); i++) {
			args.push_back(argument(*p_call.args[i], p_call.param_types[i], outward));
		}
		return args;
	}

	uint32_t load_text(ValueType p_type, const std::string &p_text) {
		const uint32_t result = temp(p_type);
		instr(Op::CONST_VALUE, result, NO_REG, NO_REG, text_constant(p_type, p_text), p_type);
		return result;
	}

	void load_int(uint32_t p_dst, int64_t p_value) {
		bytecode::Constant constant{ ValueType::Int };
		constant.i = p_value;
		instr(Op::CONST_INT, p_dst, NO_REG, NO_REG, add_constant(constant));
	}

	// What the engine declares as an object or a Variant, typed T here: a T
	// that may not be None is checked to be one (spec §3.8, §10.2).
	void check_object(uint32_t p_reg, const Type &p_type) {
		if (p_type.kind == TypeKind::Object && !p_type.nullable) {
			instr(Op::CHECK_OBJECT, NO_REG, p_reg, NO_REG, class_index(p_type));
		}
	}

	/* Arrays */

	void new_array(uint32_t p_dst, const bytecode::Slot &p_type) {
		bytecode::ClassRef element;
		element.name = p_type.class_name;
		element.script = p_type.script;
		const uint32_t class_operand = p_type.element == ValueType::Object ? class_index(element) : bytecode::NO_SLOT;
		instr(Op::NEW_ARRAY, p_dst, NO_REG, NO_REG, class_operand, p_type.element);
	}

	// A method of a built-in type by name, as the engine declares it.
	uint32_t engine_builtin_method(ValueType p_base, const char *p_method, std::vector<ValueType> p_params, ValueType p_return, bool p_vararg = false) {
		const builtin::Class *type = builtin::find_class(bytecode::value_type_name(p_base));
		const builtin::Method *method = type ? builtin::find_method(*type, p_method) : nullptr;
		bytecode::BuiltinMethodCall call;
		call.base = p_base;
		call.method = p_method;
		call.hash = method ? method->hash : 0;
		call.is_vararg = p_vararg;
		call.params = std::move(p_params);
		call.return_type = p_return;
		call.result_type = p_return;
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
		new_array(result, fn.regs[result]);
		const uint32_t append = engine_builtin_method(ValueType::Array, "append", { ValueType::Variant }, ValueType::Void);
		for (size_t e = 0; e < p_expr.args.size(); e++) {
			call_builtin(append, result, { argument(*p_expr.args[e], p_expr.param_types[e], true) });
		}
		return result;
	}

	/* Expressions */

	uint32_t default_value(uint32_t p_dst) {
		const ValueType type = reg_type(p_dst);
		if (fn.regs[p_dst].element != ValueType::Void) {
			// An empty Array[T] is typed too.
			new_array(p_dst, fn.regs[p_dst]);
			return p_dst;
		}
		switch (type) {
			case ValueType::Int:
				load_int(p_dst, 0);
				break;
			case ValueType::Float:
				instr(Op::CONST_FLOAT, p_dst, NO_REG, NO_REG, add_constant(bytecode::Constant{ ValueType::Float }));
				break;
			case ValueType::Bool:
				instr(Op::CONST_BOOL, p_dst, NO_REG, NO_REG, 0);
				break;
			case ValueType::String:
				instr(Op::CONST_STRING, p_dst, NO_REG, NO_REG, add_constant(bytecode::Constant{ ValueType::String }));
				break;
			case ValueType::Object:
			case ValueType::Variant:
				// None; a constant of type Object or Variant is always None.
				instr(Op::CONST_VALUE, p_dst, NO_REG, NO_REG, add_constant(bytecode::Constant{ type }), type);
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
		switch (p_call.target) {
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
			case Target::Unwrap: {
				const uint32_t value = lower_expr(*p_call.base);
				const uint32_t message = p_call.args.empty() ? NO_REG : convert(lower_expr(*p_call.args[0]), Type::of(TypeKind::String));
				const uint32_t result = temp(p_call.type);
				instr(Op::UNWRAP, result, value, message, 0, reg_type(result));
				return result;
			}
			default:
				break;
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
		if (p_call.target == Target::ScriptFunction) {
			// A call within the class passes the default values itself; one of a
			// method of another class leaves them to the callee (Function::defaults).
			const FuncDecl &callee = cls.functions[p_call.index];
			for (size_t a = i.args.size(); a < callee.params.size(); a++) {
				i.args.push_back(convert(lower_expr(*callee.params[a].default_value), callee.params[a].type));
			}
		}
		// An array of the class an argument named (spec §10.3).
		const bool filtered = p_call.target == Target::EngineMethod && p_call.engine_return.builtin_name() == "Array" && p_call.type != p_call.engine_return;
		i.dst = p_call.type.kind == TypeKind::Void ? NO_REG : temp(filtered ? p_call.engine_return : p_call.type);
		switch (p_call.target) {
			case Target::ScriptFunction:
				i.op = Op::CALL;
				i.imm = uint32_t(p_call.index);
				break;
			case Target::ScriptMethod: {
				i.op = Op::CALL_METHOD;
				bytecode::MethodCall method;
				method.method = p_call.text;
				for (const Type &param : p_call.engine_params) {
					method.params.push_back(slot_of(std::string(), param));
				}
				method.defaults = uint32_t(p_call.index);
				method.result = slot_of(std::string(), p_call.type);
				i.imm = method_call(method);
				break;
			}
			case Target::EngineMethod:
				i.op = Op::CALL_ENGINE;
				i.imm = engine_call(p_call.text, value_type(p_call.type));
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
				utility_call.result_type = value_type(p_call.type);
				i.imm = utility(utility_call);
				break;
			}
			case Target::BuiltinMethod: {
				i.op = Op::CALL_BUILTIN;
				bytecode::BuiltinMethodCall method;
				method.base = reg_type(i.base);
				method.method = p_call.text;
				method.hash = p_call.hash;
				method.params = value_types(p_call.engine_params);
				method.return_type = value_type(p_call.engine_return);
				method.result_type = value_type(p_call.type);
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
		add(i);
		if (written_back) {
			store(*written_back, i.base);
		}
		if (filtered) {
			// The engine made a new array, so a new array typed by that class
			// with the same elements is not told apart from it.
			const uint32_t typed = temp(p_call.type);
			new_array(typed, fn.regs[typed]);
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
					const ValueType type = value_type(p_expr.type);
					const uint32_t result = temp(type);
					instr(Op::CONST_VALUE, result, NO_REG, NO_REG, add_constant(constant_of(p_expr, p_expr.type)), type);
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
					instr(Op::IS_NONE, result, source);
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
				return get_index(container, lower_expr(*p_expr.right), p_expr.type);
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
			case ExprKind::Self:
				// Only the base of a call: the instance, as a missing slot.
				return NO_REG;
		}
		return internal_error("no code for this expression");
	}

	// get_node() on self, then a check that the node is there and has the
	// class the scenes gave it (spec §7.1), in case the tree changed since.
	uint32_t get_node(const Expr &p_expr) {
		const uint32_t path_reg = load_text(ValueType::NodePath, p_expr.text);
		ir::Instr i;
		i.op = Op::CALL_ENGINE;
		i.imm = engine_call("get_node", ValueType::Object);
		i.args = { path_reg };
		i.dst = temp(p_expr.type);
		add(i);
		check_object(i.dst, p_expr.type);
		return i.dst;
	}

	/* Places */

	static bool is_place(const Expr &p_expr) {
		switch (p_expr.target) {
			case Target::Local:
			case Target::Member:
			case Target::SelfProperty:
			case Target::ObjectProperty:
				return true;
			case Target::Field:
				return is_place(*p_expr.base);
			default:
				return false;
		}
	}

	Place place_of(const Expr &p_expr) {
		Place place;
		place.type = p_expr.type;
		place.int_range = p_expr.int_range;
		switch (p_expr.target) {
			case Target::Local:
				place.reg = uint32_t(p_expr.index);
				place.kind = value_type(p_expr.type) != reg_type(place.reg) ? Place::Kind::NarrowedLocal : Place::Kind::Local;
				break;
			case Target::Member:
				place.kind = Place::Kind::Member;
				place.index = uint32_t(p_expr.index);
				break;
			case Target::SelfProperty:
				place.kind = Place::Kind::SelfProperty;
				place.index = property(p_expr.text, value_type(p_expr.type));
				break;
			case Target::ObjectProperty:
				place.kind = Place::Kind::ObjectProperty;
				place.reg = lower_expr(*p_expr.base);
				place.index = property(p_expr.text, value_type(p_expr.type));
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
				instr(Op::UNWRAP, result, p_place.reg, NO_REG, 0, reg_type(result));
				return result;
			}
			case Place::Kind::Member:
				return load_member(p_place.index);
			case Place::Kind::SelfProperty:
			case Place::Kind::ObjectProperty: {
				const uint32_t result = temp(p_place.type);
				instr(Op::GET_PROPERTY, result, p_place.kind == Place::Kind::SelfProperty ? NO_REG : p_place.reg, NO_REG, p_place.index);
				// The binding metadata says whether it may be None (spec §10.2).
				check_object(result, p_place.type);
				return result;
			}
			case Place::Kind::Field: {
				const uint32_t result = temp(p_place.type);
				instr(Op::GET_FIELD, result, p_place.reg, NO_REG, p_place.index);
				return result;
			}
		}
		return internal_error("no code to read this place");
	}

	void store(const Place &p_place, uint32_t p_value) {
		switch (p_place.kind) {
			case Place::Kind::Local:
				move(p_place.reg, p_value);
				return;
			case Place::Kind::NarrowedLocal:
				instr(Op::TO_VARIANT, p_place.reg, p_value, NO_REG, 0, reg_type(p_value));
				return;
			case Place::Kind::Member:
				store_member(p_place.index, p_value);
				return;
			case Place::Kind::SelfProperty:
			case Place::Kind::ObjectProperty:
				if (p_place.int_range) {
					instr(Op::CHECK_RANGE, NO_REG, p_value, NO_REG, uint32_t(*p_place.int_range));
				}
				instr(Op::SET_PROPERTY, NO_REG, p_place.kind == Place::Kind::SelfProperty ? NO_REG : p_place.reg, p_value, p_place.index);
				return;
			case Place::Kind::Field:
				instr(Op::SET_FIELD, NO_REG, p_place.reg, p_value, p_place.index);
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
					if (place.kind == Place::Kind::SelfProperty || place.kind == Place::Kind::ObjectProperty) {
						value = live_or_none(value, p_stmt.value->type); // The engine keeps it.
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
				const uint32_t source = temp(p_stmt.value->type);
				move(source, lower_expr(*p_stmt.value));
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
			for (size_t m = 0; m < cls.members.size(); m++) {
				if (cls.members[m].must_assign) {
					any = true;
					line = cls.members[m].line;
					instr(Op::LINE, NO_REG, NO_REG, NO_REG, uint32_t(cls.members[m].line));
					load_member(uint32_t(m));
				}
			}
		}
		for (size_t m = 0; m < cls.members.size(); m++) {
			const VarDecl &member = cls.members[m];
			if (member.is_onready != p_onready || (!member.initializer && !is_typed_array(member.type))) {
				continue;
			}
			any = true;
			line = member.line;
			instr(Op::LINE, NO_REG, NO_REG, NO_REG, uint32_t(member.line));
			if (!member.initializer) {
				// An empty Array[T] is typed too.
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
	Lowerer(const ClassDecl &p_class, bytecode::Module &r_module, std::vector<Diagnostic> &r_errors) :
			cls(p_class), module(r_module), errors(r_errors) {}

	void run(bool p_declarations_only) {
		module.global_name = cls.class_name;
		module.base_class = cls.extends;
		for (const Type &interface : cls.implemented) {
			module.interfaces.push_back(interface_id(interface));
		}
		module.declarations_only = p_declarations_only;
		uint32_t offset = 0;
		for (const VarDecl &decl_member : cls.members) {
			bytecode::Member member;
			static_cast<bytecode::Slot &>(member) = slot_of(decl_member.name, decl_member.type);
			// Without the scenes, a type that comes from them is unknown; the
			// module only describes the class then (architecture §2.4).
			if (p_declarations_only && member.type == ValueType::Void) {
				member.type = ValueType::Variant;
			}
			member.is_onready = decl_member.is_onready;
			member.line = uint32_t(decl_member.line);
			member.offset = offset;
			offset += bytecode::slot_size(member.type);
			member.is_pub = decl_member.is_pub;
			member.is_export = decl_member.is_export;
			member.hint = decl_member.hint;
			member.hint_string = decl_member.hint_string;
			member.initial = bytecode::Constant{ member.type };
			if (decl_member.constant_initializer) {
				member.has_initial = true;
				member.initial = constant_of(*decl_member.initializer, decl_member.type);
			} else if (!decl_member.initializer && !decl_member.must_assign) {
				// Its type's default (spec §4.3); an empty Array[T] is typed too.
				member.has_initial = true;
				if (member.type == ValueType::Array) {
					type_array(member.initial, member);
				}
			}
			module.members.push_back(member);
		}
		module.instance_size = offset;
		for (const SignalDecl &signal : cls.signals) {
			bytecode::SignalInfo info;
			info.name = signal.name;
			info.is_pub = signal.is_pub;
			for (const ParamDecl &param : signal.params) {
				info.params.push_back(slot_of(param.name, param.type));
			}
			module.signals.push_back(info);
		}
		for (const ExportGroupDecl &group : cls.groups) {
			bytecode::ExportGroup out;
			out.name = group.name;
			out.prefix = group.prefix;
			out.level = group.annotation.name == "export_category" ? 0 : (group.annotation.name == "export_group" ? 1 : 2);
			out.before_member = uint32_t(group.before_member);
			module.groups.push_back(out);
		}

		bytecode::Function initializers;
		bytecode::Function ready_initializers;
		const bool has_initializers = !p_declarations_only && lower_initializers(false, initializers);
		const bool has_ready_initializers = !p_declarations_only && lower_initializers(true, ready_initializers);
		record_init_tables();

		for (const FuncDecl &function : cls.functions) {
			start(function.name);
			decl = &function;
			line = function.line;
			fn.param_count = uint32_t(function.params.size());
			fn.result = slot_of(std::string(), function.return_type);
			fn.is_pub = function.is_pub;
			fn.is_override = function.is_override;
			fn.is_exposed = function.is_exposed;
			fn.line = uint32_t(function.line);
			for (const LocalInfo &local : function.locals) {
				fn.regs.push_back(slot_of(local.name, local.type));
			}
			if (!p_declarations_only) {
				lower_block(function.body);
			}
			finish();
			bytecode::Function out = ir::emit(fn, module);
			out.is_static = function.is_static;
			for (const ParamDecl &param : function.params) {
				if (param.default_value) {
					// For an Option, the value inside; the entry check wraps it.
					out.defaults.push_back(constant_of(*param.default_value, param.type.kind == TypeKind::Option ? param.type.args[0] : param.type));
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

bytecode::Module lower(const ClassDecl &p_class, bool p_declarations_only, std::vector<Diagnostic> &r_errors) {
	bytecode::Module module;
	Lowerer(p_class, module, r_errors).run(p_declarations_only);
	return module;
}

} // namespace petrichor::compiler
