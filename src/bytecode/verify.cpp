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
	return p_type == ValueType::Array || (p_type >= ValueType::PackedByteArray && p_type <= ValueType::PackedVector4Array);
}

class Verifier {
	const Module &module;
	std::string &error;

	// The function being checked: the types of slots and members by offset, and
	// instruction starts.
	const Function *function = nullptr;
	std::unordered_map<uint32_t, uint32_t> frame;
	std::unordered_map<uint32_t, uint32_t> instance;
	std::vector<bool> starts;
	size_t at = 0;

	bool fail(const std::string &p_message) {
		error = p_message;
		if (function) {
			error = "in " + function->name + "() at " + std::to_string(at) + ": " + error;
		}
		return false;
	}

	ValueType storage(uint32_t p_type) const { return storage_of(module.types, p_type); }
	bool is_option(uint32_t p_type) const { return p_type < module.types.size() && module.types[p_type].kind == TypeKind::Option; }
	// No value: the result of a function that gives none.
	bool is_void(uint32_t p_type) const { return !is_option(p_type) && storage(p_type) == ValueType::Void; }
	bool same_layout(uint32_t p_a, uint32_t p_b) const { return layout_key(module.types, p_a) == layout_key(module.types, p_b); }

	// The type as an error names it.
	std::string type_name(uint32_t p_type) const {
		if (p_type >= module.types.size()) {
			return "?";
		}
		const TypeDesc &type = module.types[p_type];
		switch (type.kind) {
			case TypeKind::Object:
				return type.object.name;
			case TypeKind::Option:
				return type_name(type.element) + "?";
			case TypeKind::Array:
				return "Array[" + type_name(type.element) + "]";
			default:
				return value_type_name(type.value);
		}
	}

	// Each type is whole, and refers only to types before it.
	bool types() {
		for (uint32_t i = 0; i < module.types.size(); i++) {
			const TypeDesc &type = module.types[i];
			const bool inner = type.kind == TypeKind::Option || type.kind == TypeKind::Array;
			if (type.kind >= TypeKind::Count || type.value >= ValueType::Count || (inner && type.element >= i)) {
				return fail("type " + std::to_string(i) + " is not a type");
			}
			if (!type.task_result.empty() && (type.kind != TypeKind::Object || type.object.name != "CoroutineTask")) {
				return fail("Task descriptor without a task handle");
			}
			if (type.kind == TypeKind::Option && !is_native(storage(type.element)) && !is_option(type.element)) {
				return fail("an option of no value type");
			}
			if (type.kind == TypeKind::Array && !is_element(storage(type.element))) {
				return fail("an array of no element type");
			}
		}
		return true;
	}

	// The type of a slot, a member or a result.
	bool description(const Slot &p_slot, bool p_void) {
		if (p_slot.type >= module.types.size() || (!is_option(p_slot.type) && !is_type(uint32_t(storage(p_slot.type))) && !(p_void && is_void(p_slot.type)))) {
			return fail("slot " + p_slot.name + " has no type");
		}
		return true;
	}

	template <typename T>
	bool layout(const std::vector<T> &p_slots, uint32_t p_size, std::unordered_map<uint32_t, uint32_t> &r_types) {
		if (p_size > MAX_STORAGE) {
			return fail("storage of " + std::to_string(p_size) + " bytes");
		}
		std::vector<std::pair<uint32_t, uint32_t>> ranges;
		for (const T &slot : p_slots) {
			if (!description(slot, false)) {
				return false;
			}
			const uint64_t end = uint64_t(slot.offset) + type_size(module.types, slot.type);
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

	// A slot of the engine type `p_type`, in its layout.
	bool slot(uint32_t p_offset, ValueType p_type) {
		const auto found = frame.find(p_offset);
		if (found == frame.end() || is_option(found->second) || storage(found->second) != p_type) {
			return fail(std::string("operand ") + std::to_string(p_offset) + " is not a slot of type " + value_type_name(p_type));
		}
		return true;
	}

	bool slot_or_none(uint32_t p_offset, ValueType p_type) {
		return p_offset == NO_SLOT || slot(p_offset, p_type);
	}

	// A slot laid out as the type `p_type` is.
	bool slot_like(uint32_t p_offset, uint32_t p_type) {
		const auto found = frame.find(p_offset);
		if (found == frame.end() || !same_layout(found->second, p_type)) {
			return fail(std::string("operand ") + std::to_string(p_offset) + " is not a slot of type " + type_name(p_type));
		}
		return true;
	}

	// A result slot: none for void, otherwise none or a slot of the type.
	bool result(uint32_t p_offset, uint32_t p_type) {
		if (is_void(p_type)) {
			return p_offset == NO_SLOT || fail("a void call has a result slot");
		}
		return p_offset == NO_SLOT || slot_like(p_offset, p_type);
	}

	bool slot_type(uint32_t p_offset, ValueType &r_type) {
		const auto found = frame.find(p_offset);
		if (found == frame.end()) {
			return fail("operand " + std::to_string(p_offset) + " is not a slot");
		}
		r_type = is_option(found->second) ? ValueType::Void : storage(found->second);
		return true;
	}

	bool member(uint32_t p_offset, ValueType p_type) {
		const auto found = instance.find(p_offset);
		if (found == instance.end() || is_option(found->second) || storage(found->second) != p_type) {
			return fail(std::string("member operand ") + std::to_string(p_offset) + " is not a member of type " + value_type_name(p_type));
		}
		return true;
	}

	bool member_like(uint32_t p_offset, uint32_t p_type) {
		const auto found = instance.find(p_offset);
		if (found == instance.end() || !same_layout(found->second, p_type)) {
			return fail(std::string("member operand ") + std::to_string(p_offset) + " is not a member of type " + type_name(p_type));
		}
		return true;
	}

	bool member_type(uint32_t p_offset, ValueType &r_type) {
		const auto found = instance.find(p_offset);
		if (found == instance.end()) {
			return fail("member operand " + std::to_string(p_offset) + " is not a member");
		}
		r_type = is_option(found->second) ? ValueType::Void : storage(found->second);
		return true;
	}

	bool index(uint32_t p_index, size_t p_count, const char *p_table) {
		return p_index < p_count || fail(std::string("index ") + std::to_string(p_index) + " is outside the " + p_table + " table");
	}

	bool constant(uint32_t p_index, ValueType p_type) {
		if (!index(p_index, module.constants.size(), "constant")) {
			return false;
		}
		return (!is_option(module.constants[p_index].type) && storage(module.constants[p_index].type) == p_type) || fail("the constant does not have the slot's type");
	}

	bool constant_like(uint32_t p_index, uint32_t p_type) {
		if (!index(p_index, module.constants.size(), "constant")) {
			return false;
		}
		return same_layout(module.constants[p_index].type, p_type) || fail("the constant does not have the slot's type");
	}

	// A constant, and what it is built from, have types of the table.
	bool typed_constant(const Constant &p_constant) {
		if (p_constant.type >= module.types.size()) {
			return fail("a constant has no type");
		}
		for (const Constant &arg : p_constant.args) {
			if (!typed_constant(arg)) {
				return false;
			}
		}
		return true;
	}

	// A type operand: an index into the type table, of a type a slot can hold.
	bool type_operand(uint32_t p_type) {
		return (p_type < module.types.size() && (is_option(p_type) || is_type(uint32_t(storage(p_type))))) || fail("no type");
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
			if (type >= module.types.size() || is_void(type) || !slot_like(p_call[5 + 2 * i], type)) {
				return fail("argument " + std::to_string(i) + " does not name a slot of its type");
			}
		}
		return true;
	}

	// The argument holds its value as the parameter does.
	bool argument_fits(const uint32_t *p_call, size_t p_index, uint32_t p_param) {
		return same_layout(p_call[4 + 2 * p_index], p_param) || fail("argument " + std::to_string(p_index) + " does not have the parameter's type");
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
			if (param != ValueType::Variant && (!is_native(param) || is_option(p_call[4 + 2 * i]) || storage(p_call[4 + 2 * i]) != param)) {
				return fail("argument " + std::to_string(i) + " does not have the parameter's type");
			}
		}
		return arguments(p_call);
	}

	// The engine writes a native return value straight into the result slot.
	bool engine_result(uint32_t p_dst, ValueType p_return, uint32_t p_result) {
		if (p_result >= module.types.size()) {
			return fail("the result has no type");
		}
		if (p_return != ValueType::Variant && (is_option(p_result) || p_return != storage(p_result))) {
			return fail("the result slot does not have the return type");
		}
		if (p_return != ValueType::Void && p_return != ValueType::Variant && !is_native(p_return)) {
			return fail("an engine function returns an object in its own layout");
		}
		return result(p_dst, p_result);
	}

	// The result of a call of a coroutine, its Task handle (spec §9.1), or of
	// another function.
	bool call_result(uint32_t p_dst, bool p_async, uint32_t p_result) {
		return p_async ? slot_or_none(p_dst, ValueType::Object) : result(p_dst, p_result);
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
				if (p_call[3] > target_function.params.size() || p_call[3] < target_function.params.size() - target_function.defaults.size()) {
					return fail("wrong number of arguments");
				}
				for (size_t i = 0; i < p_call[3]; i++) {
					if (!argument_fits(p_call, i, target_function.params[i].type)) {
						return false;
					}
				}
				return arguments(p_call) && call_result(dst, target_function.is_async, target_function.result.type);
			}
			case Op::CALL_AWAIT_ENGINE:
				if (!function->is_async) {
					return fail("await in a synchronous function");
				}
				[[fallthrough]];
			case Op::CALL_ENGINE: {
				if (!index(callee, module.engine_calls.size(), "engine call") || !slot_or_none(base, ValueType::Object)) {
					return false;
				}
				return arguments(p_call) && result(dst, module.engine_calls[callee].result_type);
			}
			case Op::CALL_SUPER:
			case Op::CALL_METHOD: {
				if (!index(callee, module.method_calls.size(), "method call") || !(p_op == Op::CALL_SUPER ? base == NO_SLOT && !module.base_script.empty() : slot(base, ValueType::Object))) {
					return false;
				}
				// The arguments a call may leave out have default values.
				const MethodCall &method = module.method_calls[callee];
				const uint32_t argc = p_call[3];
				if (argc > method.params.size() || argc < method.params.size() - method.defaults) {
					return fail("wrong number of arguments");
				}
				for (uint32_t i = 0; i < argc; i++) {
					if (!argument_fits(p_call, i, method.params[i].type)) {
						return false;
					}
				}
				return arguments(p_call) && call_result(dst, method.is_async, method.result.type);
			}
			case Op::CALL_UTILITY: {
				if (!index(callee, module.utility_calls.size(), "utility") || (base != NO_SLOT && !fail("a utility call has a base"))) {
					return false;
				}
				const UtilityCall &utility = module.utility_calls[callee];
				return engine_arguments(p_call, utility.params, utility.is_vararg) && engine_result(dst, utility.return_type, utility.result_type);
			}
			case Op::CALL_AWAIT_BUILTIN:
				if (!function->is_async) {
					return fail("await in a synchronous function");
				}
				[[fallthrough]];
			case Op::CALL_BUILTIN: {
				if (!index(callee, module.builtin_methods.size(), "built-in method")) {
					return false;
				}
				const BuiltinMethodCall &method = module.builtin_methods[callee];
				if (p_op == Op::CALL_AWAIT_BUILTIN && (method.base != ValueType::Callable || (method.method != "call" && method.method != "callv"))) {
					return fail("awaited built-in call is not a Callable invocation");
				}
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
			case Op::GET_OWNER:
			case Op::GET_SELF:
				return true;
			case Op::GET_PROPERTY:
			case Op::GET_SIGNAL:
				return o[2] == NO_SLOT;
			case Op::SET_PROPERTY:
				return o[1] == NO_SLOT;
			case Op::CALL_AWAIT_ENGINE:
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
				if (!type_operand(o[0])) {
					return false;
				}
				const ValueType value_type = storage(o[0]);
				if (value_type == ValueType::Bool || value_type == ValueType::Int || value_type == ValueType::Float || value_type == ValueType::String) {
					return fail("a constant of a type that has an instruction of its own");
				}
				return slot_like(o[1], o[0]) && constant_like(o[2], o[0]);
			}
			case Op::MOVE_8:
				return slot_type(o[0], type) && (is_8_byte(type) || fail("an 8-byte move of another type")) && slot(o[1], type);
			case Op::MOVE_STRING:
				return same(o[0], o[1], ValueType::String);
			case Op::MOVE_VALUE:
				return type_operand(o[0]) && slot_like(o[1], o[0]) && slot_like(o[2], o[0]);
			case Op::LOAD_MEMBER_8:
				return slot_type(o[0], type) && (is_8_byte(type) || fail("an 8-byte load of another type")) && member(o[1], type);
			case Op::LOAD_MEMBER_STRING:
				return slot(o[0], ValueType::String) && member(o[1], ValueType::String);
			case Op::LOAD_MEMBER_VALUE:
				return type_operand(o[0]) && slot_like(o[1], o[0]) && member_like(o[2], o[0]);
			case Op::STORE_MEMBER_8:
				return member_type(o[0], type) && (is_8_byte(type) || fail("an 8-byte store of another type")) && slot(o[1], type);
			case Op::STORE_MEMBER_STRING:
				return member(o[0], ValueType::String) && slot(o[1], ValueType::String);
			case Op::STORE_MEMBER_VALUE:
				return type_operand(o[0]) && member_like(o[1], o[0]) && slot_like(o[2], o[0]);
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
				const uint32_t property = module.properties[o[0]].type;
				if (is_void(property)) {
					return fail("a property without a type");
				}
				return p_op == Op::GET_PROPERTY ? slot_like(o[1], property) && slot_or_none(o[2], ValueType::Object) : slot_or_none(o[1], ValueType::Object) && slot_like(o[2], property);
			}
			case Op::NEW_OBJECT:
			case Op::GET_OWNER:
			case Op::GET_SINGLETON:
			case Op::CHECK_OBJECT:
				return index(o[0], module.classes.size(), "class") && slot(o[1], ValueType::Object);
			case Op::NEW_ARRAY:
				return type_operand(o[0]) && (storage(o[0]) == ValueType::Array || fail("an array of a type that is not one")) && slot(o[1], ValueType::Array);
			case Op::GET_INDEX:
				if (o[0] >= module.types.size() || !is_indexable(storage(o[0])) || !type_operand(o[1])) {
					return fail("an index into a value that has none");
				}
				return slot_like(o[2], o[1]) && slot(o[3], storage(o[0])) && slot(o[4], ValueType::Int);
			case Op::CAST_OBJECT:
				return index(o[0], module.classes.size(), "class") && same(o[1], o[2], ValueType::Object);
			case Op::IS_INSTANCE:
				return index(o[0], module.classes.size(), "class") && slot(o[1], ValueType::Bool) && slot(o[2], ValueType::Object);
			case Op::IS_NONE:
				if (!type_operand(o[0]) || !(is_option(o[0]) || storage(o[0]) == ValueType::Object || storage(o[0]) == ValueType::Variant)) {
					return fail("None is only tested on objects and options");
				}
				return slot(o[1], ValueType::Bool) && slot_like(o[2], o[0]);
			case Op::SOME:
				if (!is_option(o[0])) {
					return fail("Some of a type that is not T?");
				}
				return slot_like(o[1], o[0]) && slot_like(o[2], module.types[o[0]].element);
			case Op::CHECK_ASSIGNED:
				return index(o[0], module.members.size(), "member") && slot(o[1], ValueType::Object);
			case Op::CHECK_LIVE:
			case Op::GET_SELF:
				return slot(o[0], ValueType::Object);
			case Op::CHECK_RANGE:
				return (o[0] < uint32_t(IntRange::COUNT) || fail("no integer range")) && slot(o[1], ValueType::Int);
			case Op::GET_AUTOLOAD:
				if (!index(o[0], module.constants.size(), "constant") || storage(module.constants[o[0]].type) != ValueType::StringName) {
					return fail("an autoload without a name");
				}
				return slot(o[1], ValueType::Object);
			case Op::METHOD_VALUE:
				return index(o[0], module.functions.size(), "function") && slot(o[1], ValueType::Callable);
			case Op::GET_SIGNAL:
				if (!index(o[0], module.constants.size(), "constant") || storage(module.constants[o[0]].type) != ValueType::StringName) {
					return fail("a signal without a name");
				}
				return slot(o[1], ValueType::Signal) && slot_or_none(o[2], ValueType::Object);
			case Op::TO_VARIANT:
				return (o[0] < module.types.size() && (is_option(o[0]) || is_native(storage(o[0]))) || fail("no value type")) && slot(o[1], ValueType::Variant) && slot_like(o[2], o[0]);
			case Op::UNWRAP: {
				// An object's T? is an Object slot like T's; that of another type is laid out as T? is.
				if (!type_operand(o[0]) || storage(o[0]) == ValueType::Variant) {
					return fail("no type to unwrap to");
				}
				if (!slot_like(o[1], o[0]) || !slot_or_none(o[3], ValueType::String)) {
					return false;
				}
				if (storage(o[0]) == ValueType::Object && !is_option(o[0])) {
					return slot(o[2], ValueType::Object);
				}
				const auto source = frame.find(o[2]);
				return (source != frame.end() && is_option(source->second) && same_layout(module.types[source->second].element, o[0])) || fail("unwrapping what is not T?");
			}
			case Op::JUMP:
				return target(o[0]);
			case Op::JUMP_IF_FALSE:
			case Op::JUMP_IF_TRUE:
				return slot(o[0], ValueType::Bool) && target(o[1]);
			case Op::TASK_CANCEL:
				return slot(o[0], ValueType::Object);
			case Op::AWAIT_TASK:
			case Op::AWAIT_SIGNAL:
				return (function->is_async || fail("await in a synchronous function")) && slot(o[1], p_op == Op::AWAIT_TASK ? ValueType::Object : ValueType::Signal) && (o[0] == NO_SLOT || frame.count(o[0]) || fail("unknown await result slot")) && (p_op == Op::AWAIT_SIGNAL || o[2] <= 1 || fail("invalid await ownership"));
			case Op::RETURN:
				return (!is_void(function->result.type) || fail("a void function returns a value")) && slot_like(o[0], function->result.type);
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
		if (module.inherited_members > module.members.size() || (module.base_script.empty() && module.inherited_members)) {
			return fail("invalid inherited member prefix");
		}
		if (!types()) {
			return false;
		}
		for (const Constant &each : module.constants) {
			if (!typed_constant(each)) {
				return false;
			}
		}
		// The init and ready functions run with no arguments and give nothing.
		for (const int32_t index : { module.init_function, module.ready_function }) {
			if (index < -1 || index >= int32_t(module.functions.size())) {
				return fail("an initializer function is not a function of the module");
			}
			if (index >= 0 && (!is_void(module.functions[index].result.type) || !module.functions[index].params.empty())) {
				return fail("an initializer function takes or returns a value");
			}
		}
		if (!layout(module.members, module.instance_size, instance)) {
			return false;
		}
		// The engine passes the value a setter takes, and takes what a getter gives.
		for (const Member &member : module.members) {
			for (const int32_t index : { member.setter, member.getter }) {
				if (index < -1 || index >= int32_t(module.functions.size())) {
					return fail("an accessor of " + member.name + " is not a function of the module");
				}
			}
			if (member.setter >= 0) {
				const Function &setter = module.functions[member.setter];
				if (setter.is_async || !is_void(setter.result.type) || setter.params.size() != 1 || setter.params[0].type != member.type) {
					return fail("the setter of " + member.name + " does not take its type");
				}
			}
			if (member.getter >= 0) {
				const Function &getter = module.functions[member.getter];
				if (getter.is_async || !getter.params.empty() || getter.result.type != member.type) {
					return fail("the getter of " + member.name + " does not give its type");
				}
			}
			if (!typed_constant(member.initial)) {
				return false;
			}
		}
		for (const SignalInfo &signal : module.signals) {
			for (const Slot &param : signal.params) {
				if (param.type >= module.types.size() || (!is_option(param.type) && !is_type(uint32_t(storage(param.type))))) {
					return fail("an argument of the signal " + signal.name + " has no type");
				}
			}
		}
		for (const EngineCall &call : module.engine_calls) {
			if (call.result_type >= module.types.size()) {
				return fail("an engine call without a result type");
			}
		}
		for (const PropertyAccess &property : module.properties) {
			if (property.type >= module.types.size()) {
				return fail("a property without a type");
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
			for (const Constant &value : each.defaults) {
				if (!typed_constant(value)) {
					return false;
				}
			}
			for (size_t i = 0; i < each.params.size(); i++) {
				if (each.params[i].offset != each.slots[i].offset || each.params[i].type != each.slots[i].type) {
					return fail("parameter " + std::to_string(i) + " is not its slot");
				}
			}
			if (each.is_inherited ? (!each.code.empty() || module.base_script.empty() || each.base_signature.empty()) : !code(each)) {
				return false;
			}
		}
		function = nullptr;
		const TablePrefix &prefix = module.init_tables;
		if (prefix.types > module.types.size() || prefix.constants > module.constants.size() || prefix.engine_calls > module.engine_calls.size() || prefix.properties > module.properties.size() ||
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
