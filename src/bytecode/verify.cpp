#include "bytecode/module.h"
#include "bytecode/opcodes.h"

#include <algorithm>
#include <unordered_map>

namespace petrichor::bytecode {

namespace {

// Larger frames or instances are not something the compiler writes; the limit
// keeps a corrupt size from becoming a huge allocation.
constexpr uint32_t MAX_STORAGE = 1u << 20;
// Variant::OP_MAX.
constexpr uint8_t OPERATOR_COUNT = 25;

bool is_8_byte(ValueType p_type) {
	return p_type == ValueType::Bool || p_type == ValueType::Int || p_type == ValueType::Float;
}

bool is_type(uint32_t p_word) {
	return p_word < uint32_t(ValueType::Count) && p_word != uint32_t(ValueType::Void);
}

// Values the engine reads and writes in their own layout: everything except
// Object, which a slot holds in a Variant, and Variant itself.
bool is_native(ValueType p_type) {
	return p_type != ValueType::Void && p_type != ValueType::Object && p_type != ValueType::Variant && p_type < ValueType::Count;
}

// What a typed array can hold.
bool is_element(ValueType p_type) {
	return p_type == ValueType::Object || is_native(p_type);
}

// What GET_INDEX reads from.
bool is_indexable(ValueType p_type) {
	return p_type == ValueType::Array || p_type == ValueType::String || (p_type >= ValueType::PackedByteArray && p_type <= ValueType::PackedVector4Array);
}

class Verifier {
	const Module &module;
	std::string &error;

	// The function being checked: slot types by offset, and instruction starts.
	const Function *function = nullptr;
	std::unordered_map<uint32_t, ValueType> frame;
	std::unordered_map<uint32_t, ValueType> instance;
	std::vector<bool> starts;
	size_t at = 0;

	bool fail(const std::string &p_message) {
		error = p_message;
		if (function) {
			error = "in " + function->name + "() at " + std::to_string(at) + ": " + error;
		}
		return false;
	}

	// The type of a slot, a member or a result, and what it says beyond it.
	template <typename T>
	bool description(const T &p_slot, bool p_void) {
		if (!is_type(uint32_t(p_slot.type)) && !(p_void && p_slot.type == ValueType::Void)) {
			return fail("slot " + p_slot.name + " has no type");
		}
		if (p_slot.option != ValueType::Void && (p_slot.type != ValueType::Variant || !is_native(p_slot.option))) {
			return fail("slot " + p_slot.name + " holds an option of no value type");
		}
		if (p_slot.element != ValueType::Void && (p_slot.type != ValueType::Array || !is_element(p_slot.element))) {
			return fail("slot " + p_slot.name + " holds an array of no element type");
		}
		if (!p_slot.interface_name.empty() && p_slot.type != ValueType::Object && p_slot.element != ValueType::Object) {
			return fail("slot " + p_slot.name + " has an interface but holds no objects");
		}
		return true;
	}

	template <typename T>
	bool layout(const std::vector<T> &p_slots, uint32_t p_size, std::unordered_map<uint32_t, ValueType> &r_types) {
		if (p_size > MAX_STORAGE) {
			return fail("storage of " + std::to_string(p_size) + " bytes");
		}
		std::vector<std::pair<uint32_t, uint32_t>> ranges;
		for (const T &slot : p_slots) {
			if (!description(slot, false)) {
				return false;
			}
			const uint64_t end = uint64_t(slot.offset) + slot_size(slot.type);
			if (slot.offset % 8 != 0 || end > p_size) {
				return fail("slot " + slot.name + " does not fit");
			}
			ranges.push_back({ slot.offset, uint32_t(end) });
			r_types[slot.offset] = slot.type;
		}
		std::sort(ranges.begin(), ranges.end());
		for (size_t i = 1; i < ranges.size(); i++) {
			if (ranges[i].first < ranges[i - 1].second) {
				return fail("slots overlap at " + std::to_string(ranges[i].first));
			}
		}
		return true;
	}

	/* Operands */

	bool slot(uint32_t p_offset, ValueType p_type) {
		const auto found = frame.find(p_offset);
		if (found == frame.end() || found->second != p_type) {
			return fail(std::string("operand ") + std::to_string(p_offset) + " is not a slot of type " + value_type_name(p_type));
		}
		return true;
	}

	bool slot_or_none(uint32_t p_offset, ValueType p_type) {
		return p_offset == NO_SLOT || slot(p_offset, p_type);
	}

	// A result slot: none for void, otherwise none or a slot of the type.
	bool result(uint32_t p_offset, ValueType p_type) {
		if (p_type == ValueType::Void) {
			return p_offset == NO_SLOT || fail("a void call has a result slot");
		}
		return slot_or_none(p_offset, p_type);
	}

	bool slot_type(uint32_t p_offset, ValueType &r_type) {
		const auto found = frame.find(p_offset);
		if (found == frame.end()) {
			return fail("operand " + std::to_string(p_offset) + " is not a slot");
		}
		r_type = found->second;
		return true;
	}

	bool member(uint32_t p_offset, ValueType p_type) {
		const auto found = instance.find(p_offset);
		if (found == instance.end() || found->second != p_type) {
			return fail(std::string("member operand ") + std::to_string(p_offset) + " is not a member of type " + value_type_name(p_type));
		}
		return true;
	}

	bool member_type(uint32_t p_offset, ValueType &r_type) {
		const auto found = instance.find(p_offset);
		if (found == instance.end()) {
			return fail("member operand " + std::to_string(p_offset) + " is not a member");
		}
		r_type = found->second;
		return true;
	}

	bool index(uint32_t p_index, size_t p_count, const char *p_table) {
		return p_index < p_count || fail(std::string("index ") + std::to_string(p_index) + " is outside the " + p_table + " table");
	}

	bool constant(uint32_t p_index, ValueType p_type) {
		if (!index(p_index, module.constants.size(), "constant")) {
			return false;
		}
		return module.constants[p_index].type == p_type || fail("the constant does not have the slot's type");
	}

	bool target(uint32_t p_pc) {
		return (p_pc < starts.size() && starts[p_pc]) || fail("jump to " + std::to_string(p_pc) + ", which is not an instruction");
	}

	bool same(uint32_t p_a, uint32_t p_b, ValueType p_type) {
		return slot(p_a, p_type) && slot(p_b, p_type);
	}

	bool three(const uint32_t *p_operands, ValueType p_result, ValueType p_operand) {
		return slot(p_operands[0], p_result) && same(p_operands[1], p_operands[2], p_operand);
	}

	/* Calls: target, dst, base, argc, then (type, offset) pairs */

	// Every argument names a slot of the type it states.
	bool arguments(const uint32_t *p_call) {
		const uint32_t argc = p_call[3];
		for (uint32_t i = 0; i < argc; i++) {
			const uint32_t type = p_call[4 + 2 * i];
			if (!is_type(type) || !slot(p_call[5 + 2 * i], ValueType(type))) {
				return fail("argument " + std::to_string(i) + " does not name a slot of its type");
			}
		}
		return true;
	}

	// Arguments passed to an engine function: Variant parameters and extra
	// vararg arguments take any value; others need exactly the parameter's
	// layout.
	bool engine_arguments(const uint32_t *p_call, const std::vector<ValueType> &p_params, bool p_vararg) {
		const uint32_t argc = p_call[3];
		if (argc < p_params.size() || (!p_vararg && argc != p_params.size())) {
			return fail("wrong number of arguments");
		}
		for (size_t i = 0; i < p_params.size(); i++) {
			const ValueType param = p_params[i];
			if (param != ValueType::Variant && (!is_native(param) || ValueType(p_call[4 + 2 * i]) != param)) {
				return fail("argument " + std::to_string(i) + " does not have the parameter's type");
			}
		}
		return arguments(p_call);
	}

	// The engine writes a native return value straight into the result slot.
	bool engine_result(uint32_t p_dst, ValueType p_return, ValueType p_result) {
		if (p_return != ValueType::Variant && p_return != p_result) {
			return fail("the result slot does not have the return type");
		}
		if (p_return != ValueType::Void && p_return != ValueType::Variant && !is_native(p_return)) {
			return fail("an engine function returns an object in its own layout");
		}
		return result(p_dst, p_result);
	}

	bool call(Op p_op, const uint32_t *p_call) {
		const uint32_t callee = p_call[0];
		const uint32_t dst = p_call[1];
		const uint32_t base = p_call[2];
		switch (p_op) {
			case Op::CALL: {
				if (!index(callee, module.functions.size(), "function") || (base != NO_SLOT && !fail("a function call has a base"))) {
					return false;
				}
				const Function &target_function = module.functions[callee];
				if (p_call[3] != target_function.params.size()) {
					return fail("wrong number of arguments");
				}
				for (size_t i = 0; i < target_function.params.size(); i++) {
					if (ValueType(p_call[4 + 2 * i]) != target_function.params[i].type) {
						return fail("argument " + std::to_string(i) + " does not have the parameter's type");
					}
				}
				return arguments(p_call) && result(dst, target_function.result.type);
			}
			case Op::CALL_ENGINE: {
				if (!index(callee, module.engine_calls.size(), "engine call") || !slot_or_none(base, ValueType::Object)) {
					return false;
				}
				return arguments(p_call) && result(dst, module.engine_calls[callee].return_type);
			}
			case Op::CALL_METHOD: {
				if (!index(callee, module.method_calls.size(), "method call") || !slot(base, ValueType::Object)) {
					return false;
				}
				// The arguments a call may leave out have default values.
				const MethodCall &method = module.method_calls[callee];
				const uint32_t argc = p_call[3];
				if (argc > method.params.size() || argc < method.params.size() - method.defaults) {
					return fail("wrong number of arguments");
				}
				for (uint32_t i = 0; i < argc; i++) {
					if (ValueType(p_call[4 + 2 * i]) != method.params[i].type) {
						return fail("argument " + std::to_string(i) + " does not have the parameter's type");
					}
				}
				return arguments(p_call) && result(dst, method.result.type);
			}
			case Op::CALL_UTILITY: {
				if (!index(callee, module.utility_calls.size(), "utility") || (base != NO_SLOT && !fail("a utility call has a base"))) {
					return false;
				}
				const UtilityCall &utility = module.utility_calls[callee];
				return engine_arguments(p_call, utility.params, utility.is_vararg) && engine_result(dst, utility.return_type, utility.result_type);
			}
			case Op::CALL_BUILTIN: {
				if (!index(callee, module.builtin_methods.size(), "built-in method")) {
					return false;
				}
				const BuiltinMethodCall &method = module.builtin_methods[callee];
				if (!is_native(method.base) || !slot(base, method.base)) {
					return false;
				}
				return engine_arguments(p_call, method.params, method.is_vararg) && engine_result(dst, method.return_type, method.result_type);
			}
			case Op::CONSTRUCT: {
				if (!index(callee, module.constructors.size(), "constructor") || (base != NO_SLOT && !fail("a constructor has a base"))) {
					return false;
				}
				const ConstructorCall &constructor = module.constructors[callee];
				if (!is_native(constructor.type) || !slot(dst, constructor.type)) {
					return false;
				}
				for (ValueType param : constructor.params) {
					if (!is_native(param)) {
						return fail("a constructor takes an object in its own layout");
					}
				}
				// The old value of dst is destroyed before the engine reads the arguments.
				for (uint32_t i = 0; i < p_call[3]; i++) {
					if (p_call[5 + 2 * i] == dst) {
						return fail("a constructor writes over one of its arguments");
					}
				}
				return engine_arguments(p_call, constructor.params, false);
			}
			default:
				return fail("not a call");
		}
	}

	/* Instructions */

	// Whether the instruction needs the instance: members, or self as its base.
	bool uses_instance(Op p_op, const uint32_t *o) const {
		switch (p_op) {
			case Op::LOAD_MEMBER_8:
			case Op::LOAD_MEMBER_STRING:
			case Op::LOAD_MEMBER_VALUE:
			case Op::STORE_MEMBER_8:
			case Op::STORE_MEMBER_STRING:
			case Op::STORE_MEMBER_VALUE:
			case Op::CHECK_ASSIGNED:
			case Op::METHOD_VALUE:
				return true;
			case Op::GET_PROPERTY:
			case Op::GET_SIGNAL:
				return o[2] == NO_SLOT;
			case Op::SET_PROPERTY:
				return o[1] == NO_SLOT;
			case Op::CALL_ENGINE:
				return o[2] == NO_SLOT;
			case Op::CALL:
				return o[0] < module.functions.size() && !module.functions[o[0]].is_static;
			default:
				return false;
		}
	}

	bool instruction(Op p_op, const uint32_t *o) {
		ValueType type = ValueType::Void;
		// A static function runs without an instance (spec §4.8).
		if (function->is_static && uses_instance(p_op, o)) {
			return fail("a static function uses the instance");
		}
		switch (p_op) {
			case Op::LINE:
				return true;
			case Op::CONST_INT:
				return slot(o[0], ValueType::Int) && constant(o[1], ValueType::Int);
			case Op::CONST_FLOAT:
				return slot(o[0], ValueType::Float) && constant(o[1], ValueType::Float);
			case Op::CONST_BOOL:
				// A bool slot holds 0 or 1 in all 8 bytes.
				return slot(o[0], ValueType::Bool) && (o[1] <= 1 || fail("a bool other than 0 and 1"));
			case Op::CONST_STRING:
				return slot(o[0], ValueType::String) && constant(o[1], ValueType::String);
			case Op::CONST_VALUE: {
				const ValueType value_type = ValueType(o[0]);
				if (value_type == ValueType::Bool || value_type == ValueType::Int || value_type == ValueType::Float || value_type == ValueType::String) {
					return fail("a constant of a type that has an instruction of its own");
				}
				return is_type(o[0]) && slot(o[1], value_type) && constant(o[2], value_type);
			}
			case Op::MOVE_8:
				return slot_type(o[0], type) && (is_8_byte(type) || fail("an 8-byte move of another type")) && slot(o[1], type);
			case Op::MOVE_STRING:
				return same(o[0], o[1], ValueType::String);
			case Op::MOVE_VALUE:
				return (is_type(o[0]) || fail("no type")) && same(o[1], o[2], ValueType(o[0]));
			case Op::LOAD_MEMBER_8:
				return slot_type(o[0], type) && (is_8_byte(type) || fail("an 8-byte load of another type")) && member(o[1], type);
			case Op::LOAD_MEMBER_STRING:
				return slot(o[0], ValueType::String) && member(o[1], ValueType::String);
			case Op::LOAD_MEMBER_VALUE:
				return (is_type(o[0]) || fail("no type")) && slot(o[1], ValueType(o[0])) && member(o[2], ValueType(o[0]));
			case Op::STORE_MEMBER_8:
				return member_type(o[0], type) && (is_8_byte(type) || fail("an 8-byte store of another type")) && slot(o[1], type);
			case Op::STORE_MEMBER_STRING:
				return member(o[0], ValueType::String) && slot(o[1], ValueType::String);
			case Op::STORE_MEMBER_VALUE:
				return (is_type(o[0]) || fail("no type")) && member(o[1], ValueType(o[0])) && slot(o[2], ValueType(o[0]));
			case Op::INT_TO_FLOAT:
				return slot(o[0], ValueType::Float) && slot(o[1], ValueType::Int);
			case Op::FLOAT_TO_INT:
				return slot(o[0], ValueType::Int) && slot(o[1], ValueType::Float);
			case Op::ADD_INT:
			case Op::SUB_INT:
			case Op::MUL_INT:
			case Op::DIV_INT:
			case Op::MOD_INT:
				return three(o, ValueType::Int, ValueType::Int);
			case Op::NEG_INT:
				return same(o[0], o[1], ValueType::Int);
			case Op::ADD_FLOAT:
			case Op::SUB_FLOAT:
			case Op::MUL_FLOAT:
			case Op::DIV_FLOAT:
				return three(o, ValueType::Float, ValueType::Float);
			case Op::NEG_FLOAT:
				return same(o[0], o[1], ValueType::Float);
			case Op::ADD_STRING:
				return three(o, ValueType::String, ValueType::String);
			case Op::EQ_INT:
			case Op::NE_INT:
			case Op::LT_INT:
			case Op::LE_INT:
			case Op::GT_INT:
			case Op::GE_INT:
				return three(o, ValueType::Bool, ValueType::Int);
			case Op::EQ_FLOAT:
			case Op::NE_FLOAT:
			case Op::LT_FLOAT:
			case Op::LE_FLOAT:
			case Op::GT_FLOAT:
			case Op::GE_FLOAT:
				return three(o, ValueType::Bool, ValueType::Float);
			case Op::EQ_BOOL:
			case Op::NE_BOOL:
				return three(o, ValueType::Bool, ValueType::Bool);
			case Op::EQ_STRING:
			case Op::NE_STRING:
				return three(o, ValueType::Bool, ValueType::String);
			case Op::NOT:
				return same(o[0], o[1], ValueType::Bool);
			case Op::EVALUATE: {
				if (!index(o[0], module.operators.size(), "operator")) {
					return false;
				}
				const OperatorCall &op = module.operators[o[0]];
				if (op.op >= OPERATOR_COUNT || !is_native(op.left) || !is_native(op.result) || (op.right != ValueType::Void && !is_native(op.right))) {
					return fail("an operator outside the engine's table");
				}
				if (!slot(o[1], op.result) || !slot(o[2], op.left)) {
					return false;
				}
				return op.right == ValueType::Void ? (o[3] == NO_SLOT || fail("a unary operator has a right operand")) : slot(o[3], op.right);
			}
			case Op::GET_FIELD:
			case Op::SET_FIELD: {
				if (!index(o[0], module.fields.size(), "field")) {
					return false;
				}
				const FieldAccess &field = module.fields[o[0]];
				if (!is_native(field.base) || !is_native(field.type)) {
					return fail("a field outside the built-in types");
				}
				return p_op == Op::GET_FIELD ? slot(o[1], field.type) && slot(o[2], field.base) : slot(o[1], field.base) && slot(o[2], field.type);
			}
			case Op::GET_PROPERTY:
			case Op::SET_PROPERTY: {
				if (!index(o[0], module.properties.size(), "property")) {
					return false;
				}
				const ValueType property = module.properties[o[0]].type;
				if (!is_type(uint32_t(property))) {
					return fail("a property without a type");
				}
				return p_op == Op::GET_PROPERTY ? slot(o[1], property) && slot_or_none(o[2], ValueType::Object) : slot_or_none(o[1], ValueType::Object) && slot(o[2], property);
			}
			case Op::NEW_OBJECT:
			case Op::GET_SINGLETON:
			case Op::CHECK_OBJECT:
				return index(o[0], module.classes.size(), "class") && slot(o[1], ValueType::Object);
			case Op::NEW_ARRAY: {
				const ValueType element = ValueType(o[0]);
				if (element != ValueType::Void && !is_element(element)) {
					return fail("an array of no element type");
				}
				if ((element == ValueType::Object) != (o[1] != NO_SLOT)) {
					return fail("an array of objects needs a class, and only it");
				}
				return (o[1] == NO_SLOT || index(o[1], module.classes.size(), "class")) && slot(o[2], ValueType::Array);
			}
			case Op::GET_INDEX:
				if (!is_indexable(ValueType(o[0])) || !is_type(o[1])) {
					return fail("an index into a value that has none");
				}
				return slot(o[2], ValueType(o[1])) && slot(o[3], ValueType(o[0])) && slot(o[4], ValueType::Int);
			case Op::CAST_OBJECT:
				return index(o[0], module.classes.size(), "class") && same(o[1], o[2], ValueType::Object);
			case Op::IS_INSTANCE:
				return index(o[0], module.classes.size(), "class") && slot(o[1], ValueType::Bool) && slot(o[2], ValueType::Object);
			case Op::IS_NONE:
				return slot(o[0], ValueType::Bool) && slot_type(o[1], type) && (type == ValueType::Object || type == ValueType::Variant || fail("None is only tested on objects and options"));
			case Op::CHECK_ASSIGNED:
				return index(o[0], module.members.size(), "member") && slot(o[1], ValueType::Object);
			case Op::GET_AUTOLOAD:
				if (!index(o[0], module.constants.size(), "constant") || module.constants[o[0]].type != ValueType::StringName) {
					return fail("an autoload without a name");
				}
				return slot(o[1], ValueType::Object);
			case Op::METHOD_VALUE:
				return index(o[0], module.functions.size(), "function") && slot(o[1], ValueType::Callable);
			case Op::GET_SIGNAL:
				if (!index(o[0], module.constants.size(), "constant") || module.constants[o[0]].type != ValueType::StringName) {
					return fail("a signal without a name");
				}
				return slot(o[1], ValueType::Signal) && slot_or_none(o[2], ValueType::Object);
			case Op::TO_VARIANT:
				return (is_native(ValueType(o[0])) || fail("no value type")) && slot(o[1], ValueType::Variant) && slot(o[2], ValueType(o[0]));
			case Op::UNWRAP: {
				// An object's T? is an Object slot; a value's is a Variant slot.
				const ValueType unwrapped = ValueType(o[0]);
				if (unwrapped != ValueType::Object && !is_native(unwrapped)) {
					return fail("no type to unwrap to");
				}
				return slot(o[1], unwrapped) && slot(o[2], unwrapped == ValueType::Object ? ValueType::Object : ValueType::Variant) && slot_or_none(o[3], ValueType::String);
			}
			case Op::JUMP:
				return target(o[0]);
			case Op::JUMP_IF_FALSE:
			case Op::JUMP_IF_TRUE:
				return slot(o[0], ValueType::Bool) && target(o[1]);
			case Op::RETURN:
				return (function->result.type != ValueType::Void || fail("a void function returns a value")) && slot(o[0], function->result.type);
			case Op::RETURN_VOID:
				// Also at the unreachable end of a function with a return type;
				// the caller's slot then keeps its default value.
				return true;
			default:
				return call(p_op, o);
		}
	}

	bool code(const Function &p_function) {
		const std::vector<uint32_t> &code = p_function.code;
		starts.assign(code.size(), false);
		std::vector<size_t> order;
		for (at = 0; at < code.size();) {
			if (code[at] >= uint32_t(Op::COUNT)) {
				return fail("unknown instruction " + std::to_string(code[at]));
			}
			const int count = operand_count(Op(code[at]));
			size_t length = size_t(1 + count);
			if (count < 0) {
				if (code.size() - at < 5 || code[at + 4] > (code.size() - at - 5) / 2) {
					return fail("a call is cut off");
				}
				length = 5 + 2 * size_t(code[at + 4]);
			}
			if (code.size() - at < length) {
				return fail("an instruction is cut off");
			}
			starts[at] = true;
			order.push_back(at);
			at += length;
		}
		if (order.empty()) {
			return fail("no code");
		}
		for (size_t pc : order) {
			at = pc;
			if (!instruction(Op(code[pc]), code.data() + pc + 1)) {
				return false;
			}
		}
		// Only these leave without going on to the next instruction.
		at = order.back();
		const Op last = Op(code[at]);
		if (last != Op::RETURN && last != Op::RETURN_VOID && last != Op::JUMP) {
			return fail("code can run past its end");
		}
		return true;
	}

public:
	Verifier(const Module &p_module, std::string &r_error) :
			module(p_module), error(r_error) {}

	bool run() {
		// The init and ready functions run with no arguments and give nothing.
		for (const int32_t index : { module.init_function, module.ready_function }) {
			if (index < -1 || index >= int32_t(module.functions.size())) {
				return fail("an initializer function is not a function of the module");
			}
			if (index >= 0 && (module.functions[index].result.type != ValueType::Void || !module.functions[index].params.empty())) {
				return fail("an initializer function takes or returns a value");
			}
		}
		if (!layout(module.members, module.instance_size, instance)) {
			return false;
		}
		for (const SignalInfo &signal : module.signals) {
			for (const Slot &param : signal.params) {
				if (!is_type(uint32_t(param.type))) {
					return fail("an argument of the signal " + signal.name + " has no type");
				}
			}
		}
		for (const ExportGroup &group : module.groups) {
			if (group.level > 2 || group.before_member > module.members.size()) {
				return fail("an export group outside the members");
			}
		}
		for (const MethodCall &call : module.method_calls) {
			for (const Slot &param : call.params) {
				if (!description(param, false)) {
					return false;
				}
			}
			if (call.defaults > call.params.size()) {
				return fail("more default values than parameters in a method call");
			}
			if (!description(call.result, true)) {
				return false;
			}
		}
		for (const Function &each : module.functions) {
			function = &each;
			frame.clear();
			at = 0;
			if (!description(each.result, true) || !layout(each.slots, each.frame_size, frame)) {
				return false;
			}
			if (each.params.size() > each.slots.size()) {
				return fail("more parameters than slots");
			}
			if (each.defaults.size() > each.params.size()) {
				return fail("more default values than parameters");
			}
			for (size_t i = 0; i < each.params.size(); i++) {
				if (each.params[i].offset != each.slots[i].offset || each.params[i].type != each.slots[i].type) {
					return fail("parameter " + std::to_string(i) + " is not its slot");
				}
			}
			if (!code(each)) {
				return false;
			}
		}
		function = nullptr;
		const TablePrefix &prefix = module.init_tables;
		if (prefix.constants > module.constants.size() || prefix.engine_calls > module.engine_calls.size() || prefix.properties > module.properties.size() ||
				prefix.utility_calls > module.utility_calls.size() || prefix.operators > module.operators.size() || prefix.fields > module.fields.size() ||
				prefix.constructors > module.constructors.size() || prefix.builtin_methods > module.builtin_methods.size() || prefix.classes > module.classes.size() ||
				prefix.method_calls > module.method_calls.size()) {
			return fail("the initializer functions' tables are larger than the module's");
		}
		return true;
	}
};

} // namespace

bool verify(const Module &p_module, std::string &r_error) {
	return Verifier(p_module, r_error).run();
}

} // namespace petrichor::bytecode
