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

bool is_type(uint32_t p_word) {
	return p_word < uint32_t(ValueType::Count) && p_word != uint32_t(ValueType::Void);
}

// Values the engine reads and writes in their own layout: everything except
// Object, which a slot holds in a Variant, Variant itself and Task, which is
// the runtime's.
bool is_native(ValueType p_type) {
	return p_type != ValueType::Void && p_type != ValueType::Object && p_type != ValueType::Variant && p_type < ValueType::Variant;
}

// What a typed array can hold.
bool is_element(ValueType p_type) {
	return p_type == ValueType::Object || p_type == ValueType::Task || is_native(p_type);
}

// What GET_INDEX reads from and SET_INDEX writes to.
bool is_indexable(ValueType p_type) {
	return p_type == ValueType::Array || p_type == ValueType::Dictionary || packed_element(p_type) != ValueType::Void;
}

// The elements that GET_INDEX and ITERATE read, besides an Array's: a packed
// array's, or a String's characters, each a String; Void for other values.
ValueType read_element(ValueType p_type) {
	return p_type == ValueType::String ? ValueType::String : packed_element(p_type);
}

// Laid out as Result[T, E] is: two variants of one field each.
bool is_result(const TypeDesc &p_type) {
	return p_type.kind == TypeKind::PayloadEnum && p_type.values.size() == 2 && p_type.values[0].value == 1 && p_type.values[1].value == 1;
}

// The types of the slots that start at each offset: a struct and its first
// field start at the same one.
using Slots = std::unordered_map<uint32_t, std::vector<uint32_t>>;

class Verifier {
	const Module &module;
	std::string &error;

	// The function being checked: the slots of its frame and of the instance,
	// and instruction starts.
	const Function *function = nullptr;
	Slots frame;
	Slots instance;
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
	bool is_struct(uint32_t p_type) const { return has_fields(module.types, p_type); }
	// A type of the table that values have.
	bool has_values(uint32_t p_type) const { return is_option(p_type) || is_struct(p_type) || (p_type < module.types.size() && is_type(uint32_t(storage(p_type)))); }
	// No value: the result of a function that gives none.
	bool is_void(uint32_t p_type) const { return p_type >= module.types.size() || (module.types[p_type].kind == TypeKind::Value && module.types[p_type].value == ValueType::Void); }
	bool same_layout(uint32_t p_a, uint32_t p_b) const { return layout_key(module.types, p_a) == layout_key(module.types, p_b); }
	bool is_boxed(uint32_t p_type) const { return bytecode::is_boxed(module.types, p_type); }
	// What a typed container can keep: Variant keeps a side untyped (spec §3.13).
	bool kept(uint32_t p_type) const { return is_element(storage(p_type)) || storage(p_type) == ValueType::Variant || is_boxed(p_type); }

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
			case TypeKind::Dictionary:
				return "Dictionary[" + type_name(type.key) + ", " + type_name(type.element) + "]";
			case TypeKind::Struct:
			case TypeKind::Enum:
			case TypeKind::PayloadEnum:
				return type.name.substr(type.name.rfind(':') + 1);
			default:
				return value_type_name(type.value);
		}
	}

	// Each type is whole, and refers only to types before it.
	bool types() {
		for (uint32_t i = 0; i < module.types.size(); i++) {
			const TypeDesc &type = module.types[i];
			const bool inner = type.kind == TypeKind::Option || type.kind == TypeKind::Array || type.kind == TypeKind::Dictionary;
			if (type.kind >= TypeKind::Count || type.value >= ValueType::Count || (inner && type.element >= i) || (type.kind == TypeKind::Dictionary && type.key >= i)) {
				return fail("type " + std::to_string(i) + " is not a type");
			}
			if (!type.task_result.empty() && (type.kind != TypeKind::Object || type.object.name != "CoroutineTask")) {
				return fail("Task descriptor without a task handle");
			}
			if (type.kind == TypeKind::Option && !is_native(storage(type.element)) && storage(type.element) != ValueType::Variant && !is_option(type.element) && !is_struct(type.element)) {
				return fail("an option of no value type");
			}
			if (type.kind == TypeKind::Array && !kept(type.element)) {
				return fail("an array of no element type");
			}
			if (type.kind == TypeKind::Dictionary && (!kept(type.key) || !kept(type.element))) {
				return fail("a dictionary of no key or value type");
			}
			for (const FieldDesc &field : type.fields) {
				if (!is_struct(i) || field.type >= i || !has_values(field.type)) {
					return fail("a field of type " + std::to_string(i) + " has no type");
				}
			}
			if ((type.kind == TypeKind::Enum || type.kind == TypeKind::PayloadEnum) && type.name.empty()) {
				return fail("an enum without a name");
			}
			if (!type.values.empty() && type.kind != TypeKind::Enum && type.kind != TypeKind::PayloadEnum) {
				return fail("values of type " + std::to_string(i) + ", which is no enum");
			}
			if (type.kind == TypeKind::PayloadEnum) {
				// The variant's index, then each variant's fields.
				uint64_t fields = 1;
				for (const EnumValueDesc &value : type.values) {
					fields += value.value >= 0 ? uint64_t(value.value) : uint64_t(1) << 32;
				}
				if (type.values.empty() || fields != type.fields.size() || storage(type.fields[0].type) != ValueType::Int) {
					return fail("an enum with payloads whose variants do not have its fields");
				}
			}
		}
		return true;
	}

	// The type of a slot, a member or a result.
	bool description(const Slot &p_slot, bool p_void) {
		if (p_slot.type >= module.types.size() || (!has_values(p_slot.type) && !(p_void && is_void(p_slot.type)))) {
			return fail("slot " + p_slot.name + " has no type");
		}
		return true;
	}

	// The slot and, inside a struct, the slots of its fields.
	void add_slot(Slots &r_slots, uint32_t p_offset, uint32_t p_type) {
		r_slots[p_offset].push_back(p_type);
		if (is_struct(p_type)) {
			const std::vector<uint32_t> offsets = field_offsets(module.types, p_type);
			for (size_t i = 0; i < offsets.size(); i++) {
				add_slot(r_slots, p_offset + offsets[i], module.types[p_type].fields[i].type);
			}
		}
	}

	template <typename T>
	bool layout(const std::vector<T> &p_slots, uint32_t p_size, Slots &r_slots) {
		if (p_size > MAX_STORAGE) {
			return fail("storage of " + std::to_string(p_size) + " bytes");
		}
		std::vector<std::pair<uint32_t, uint32_t>> ranges;
		for (const T &slot : p_slots) {
			if (!description(slot, false)) {
				return false;
			}
			uint64_t end = uint64_t(slot.offset) + type_size(module.types, slot.type);
			if constexpr (std::is_same_v<T, Member>) {
				end += slot.tracks_assignment() ? 8 : 0;
			}
			if (slot.offset % 8 != 0 || end > p_size) {
				return fail("slot " + slot.name + " does not fit");
			}
			ranges.push_back({ slot.offset, uint32_t(end) });
			add_slot(r_slots, slot.offset, slot.type);
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

	// Whether some slot at the offset has a type that `p_fits`.
	template <typename Fits>
	static bool any(const Slots &p_slots, uint32_t p_offset, Fits p_fits) {
		const auto found = p_slots.find(p_offset);
		return found != p_slots.end() && std::any_of(found->second.begin(), found->second.end(), p_fits);
	}

	auto of_storage(ValueType p_type) const {
		return [this, p_type](uint32_t p_slot) { return !is_option(p_slot) && !is_struct(p_slot) && storage(p_slot) == p_type; };
	}

	auto laid_out_as(uint32_t p_type) const {
		return [this, p_type](uint32_t p_slot) { return same_layout(p_slot, p_type); };
	}

	// A slot of the engine type `p_type`, in its layout.
	bool slot(uint32_t p_offset, ValueType p_type) {
		return any(frame, p_offset, of_storage(p_type)) || fail(std::string("operand ") + std::to_string(p_offset) + " is not a slot of type " + value_type_name(p_type));
	}

	bool slot_or_none(uint32_t p_offset, ValueType p_type) {
		return p_offset == NO_SLOT || slot(p_offset, p_type);
	}

	// A slot laid out as the type `p_type` is.
	bool slot_like(uint32_t p_offset, uint32_t p_type) {
		return any(frame, p_offset, laid_out_as(p_type)) || fail(std::string("operand ") + std::to_string(p_offset) + " is not a slot of type " + type_name(p_type));
	}

	// A result slot: none for void, otherwise none or a slot of the type.
	bool result(uint32_t p_offset, uint32_t p_type) {
		if (is_void(p_type)) {
			return p_offset == NO_SLOT || fail("a void call has a result slot");
		}
		return p_offset == NO_SLOT || slot_like(p_offset, p_type);
	}

	// Slots of the same type of 8 bytes, of the frame or of the instance.
	bool same_8_byte(const Slots &p_a, uint32_t p_offset_a, const Slots &p_b, uint32_t p_offset_b, const char *p_what) {
		for (ValueType type : { ValueType::Bool, ValueType::Int, ValueType::Float }) {
			if (any(p_a, p_offset_a, of_storage(type)) && any(p_b, p_offset_b, of_storage(type))) {
				return true;
			}
		}
		return fail(p_what);
	}

	bool member(uint32_t p_offset, ValueType p_type) {
		return any(instance, p_offset, of_storage(p_type)) || fail(std::string("member operand ") + std::to_string(p_offset) + " is not a member of type " + value_type_name(p_type));
	}

	bool member_like(uint32_t p_offset, uint32_t p_type) {
		return any(instance, p_offset, laid_out_as(p_type)) || fail(std::string("member operand ") + std::to_string(p_offset) + " is not a member of type " + type_name(p_type));
	}

	// A static variable of this class, of the type; one of another class is
	// looked up when the code runs (spec §4.8).
	bool static_variable(uint32_t p_class, uint32_t p_variable, uint32_t p_type) {
		if (p_class != NO_SLOT) {
			return index(p_class, module.classes.size(), "class") && (!module.classes[p_class].script.empty() || fail("a static variable of an engine class"));
		}
		return index(p_variable, module.statics.size(), "static variable") && (same_layout(module.statics[p_variable].type, p_type) || fail("static variable " + module.statics[p_variable].name + " is not of type " + type_name(p_type)));
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

	// A constant, and what it is built from, have types of the table; an
	// operator has its operands, a field its base.
	bool typed_constant(const Constant &p_constant) {
		if (p_constant.type >= module.types.size()) {
			return fail("a constant has no type");
		}
		// A variant by its index, with its fields at their defaults.
		const TypeDesc &type = module.types[p_constant.type];
		if (type.kind == TypeKind::PayloadEnum && (p_constant.i < 0 || uint64_t(p_constant.i) >= type.values.size())) {
			return fail("a constant of an enum with payloads names no variant");
		}
		if (p_constant.op >= 0 && (p_constant.op > 24 || p_constant.args.empty() || p_constant.args.size() > 2)) {
			return fail("a constant applies an invalid operator");
		}
		if (p_constant.utility >= 0 && !index(uint32_t(p_constant.utility), module.utility_calls.size(), "utility call")) {
			return false;
		}
		if (!p_constant.field.empty() && p_constant.args.size() != 1) {
			return fail("a constant reads a field of no value");
		}
		if (p_constant.load && storage(p_constant.type) != ValueType::Object) {
			return fail("a constant loads a resource into a type that holds no object");
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
		return has_values(p_type) || fail("no type");
	}

	// A key or an element of a typed container, as it goes in.
	bool kept_operand(uint32_t p_offset, uint32_t p_type) {
		return is_boxed(p_type) ? slot(p_offset, ValueType::Variant) : slot_like(p_offset, p_type);
	}

	// The index of GET_INDEX and SET_INDEX: an int, or a key of the Dictionary.
	bool index_operand(uint32_t p_base, uint32_t p_index) {
		const TypeDesc &base = module.types[p_base];
		if (base.kind == TypeKind::Dictionary) {
			return kept_operand(p_index, base.key);
		}
		return slot(p_index, storage(p_base) == ValueType::Dictionary ? ValueType::Variant : ValueType::Int);
	}

	// What GET_INDEX and ITERATE read into: from a packed array or a String,
	// its element in the element's own layout, which the engine's getter writes there.
	bool element_operand(uint32_t p_base, uint32_t p_type, uint32_t p_dst) {
		const ValueType packed = read_element(storage(p_base));
		if (!type_operand(p_type) || !slot_like(p_dst, p_type)) {
			return false;
		}
		return packed == ValueType::Void || storage(p_type) == packed || fail("an element read as another type");
	}

	bool target(uint32_t p_pc) {
		return (p_pc < starts.size() && starts[p_pc]) || fail("jump to " + std::to_string(p_pc) + ", which is not an instruction");
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

	// A call through a MethodBind: the engine reads each argument and writes
	// the result in the layout the table gives, an object as its pointer; the
	// arguments a call leaves out have defaults.
	bool native_call(const uint32_t *p_call, const EngineCall &p_method) {
		const auto passes = [&](ValueType p_type) { return p_type == ValueType::Variant || p_type == ValueType::Object || is_native(p_type); };
		const uint32_t argc = p_call[3];
		if (p_method.defaults.size() > p_method.params.size() || argc > p_method.params.size() || argc + p_method.defaults.size() < p_method.params.size()) {
			return fail("wrong number of arguments");
		}
		for (size_t i = 0; i < p_method.params.size(); i++) {
			const ValueType param = p_method.params[i];
			if (!passes(param) || (i < argc && param != ValueType::Variant && (is_option(p_call[4 + 2 * i]) || storage(p_call[4 + 2 * i]) != param))) {
				return fail("argument " + std::to_string(i) + " does not have the parameter's type");
			}
		}
		const uint32_t dst = p_call[1];
		if (p_method.return_type == ValueType::Void ? dst != NO_SLOT : !passes(p_method.return_type) || is_option(p_method.result_type) || storage(p_method.result_type) != p_method.return_type) {
			return fail("the result slot does not have the return type");
		}
		return true;
	}

	// The result of a call of a coroutine, its Task handle (spec §9.1), or of
	// another function.
	bool call_result(uint32_t p_dst, bool p_async, uint32_t p_result) {
		return p_async ? slot_or_none(p_dst, ValueType::Task) : result(p_dst, p_result);
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
				if (!target_function.takes(p_call[3])) {
					return fail("wrong number of arguments");
				}
				// Those that go into a rest parameter take any value.
				for (size_t i = 0; i < std::min<size_t>(p_call[3], target_function.fixed_params()); i++) {
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
				const EngineCall &call = module.engine_calls[callee];
				if (!call.static_class.empty() && base != NO_SLOT) {
					return fail("a static engine call has a base");
				}
				return (call.owner.empty() || native_call(p_call, call)) && arguments(p_call) && result(dst, call.result_type);
			}
			case Op::CALL_SUPER:
			case Op::CALL_METHOD: {
				if (!index(callee, module.method_calls.size(), "method call")) {
					return false;
				}
				// The arguments a call may leave out have default values.
				const MethodCall &method = module.method_calls[callee];
				if (p_op == Op::CALL_SUPER) {
					if (module.base_script.empty() || method.static_class >= 0 || base != NO_SLOT) {
						return fail("a super call without a base script, or with a base");
					}
				} else if (method.static_class >= 0 ? base != NO_SLOT && !fail("a static method call has a base") : !slot(base, ValueType::Object)) {
					return false;
				}
				const uint32_t argc = p_call[3];
				const size_t fixed = method.params.size() - (method.is_vararg ? 1 : 0);
				if ((!method.is_vararg && argc > fixed) || argc + method.defaults < fixed) {
					return fail("wrong number of arguments");
				}
				for (uint32_t i = 0; i < std::min<size_t>(argc, fixed); i++) {
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
				if (!is_native(method.base) || !(method.is_static ? base == NO_SLOT || fail("a static built-in method has a base") : slot(base, method.base))) {
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
			case Op::CALL_VALUE: {
				if (!index(callee, module.method_calls.size(), "method call") || !slot(base, ValueType::Callable)) {
					return false;
				}
				const MethodCall &method = module.method_calls[callee];
				const bool spread = method.method == "callv";
				if ((!spread && method.method != "call") || method.static_class >= 0 || method.is_async || method.defaults != 0 || p_call[3] != method.params.size() ||
						(spread && (method.params.size() != 1 || storage(method.params[0].type) != ValueType::Array))) {
					return fail("CALL_VALUE of what is not call() or callv() of a function value");
				}
				for (uint32_t i = 0; i < p_call[3]; i++) {
					if (!argument_fits(p_call, i, method.params[i].type)) {
						return false;
					}
				}
				return arguments(p_call) && result(dst, method.result.type);
			}
			case Op::TASK_ANY:
			case Op::TASK_ALL: {
				const ValueType result = p_op == Op::TASK_ANY ? ValueType::Int : ValueType::Void;
				if (callee >= module.types.size() || storage(callee) != result || base != NO_SLOT || p_call[3] % 2 != 0) {
					return fail("a task of other tasks that is not Task.any() or Task.all()");
				}
				for (uint32_t i = 0; i < p_call[3]; i++) {
					if (storage(p_call[4 + 2 * i]) != (i % 2 ? ValueType::Int : ValueType::Task)) {
						return fail("Task.any() or Task.all() of what is not a task and whether it is owned");
					}
				}
				return arguments(p_call) && slot(dst, ValueType::Task);
			}
			case Op::RANGE: {
				const bool counts = callee < module.types.size() && storage(callee) == ValueType::Int;
				const bool lists = callee < module.types.size() && module.types[callee].kind == TypeKind::Array && storage(module.types[callee].element) == ValueType::Int;
				if ((!counts && !lists) || base != NO_SLOT || p_call[3] != 3) {
					return fail("RANGE of other than three ints into an int or an Array[int]");
				}
				for (uint32_t i = 0; i < 3; i++) {
					if (storage(p_call[4 + 2 * i]) != ValueType::Int) {
						return fail("RANGE of other than three ints into an int or an Array[int]");
					}
				}
				return arguments(p_call) && slot(dst, storage(callee));
			}
			case Op::MAKE_LAMBDA: {
				if (!index(callee, module.functions.size(), "function")) {
					return false;
				}
				const Function &lambda = module.functions[callee];
				if (!lambda.is_lambda || base != NO_SLOT || p_call[3] != lambda.captures) {
					return fail("MAKE_LAMBDA of what is not a lambda, with a base or with other captures");
				}
				const size_t first = lambda.params.size() - lambda.captures;
				for (size_t i = 0; i < p_call[3]; i++) {
					if (!argument_fits(p_call, i, lambda.params[first + i].type)) {
						return false;
					}
				}
				return arguments(p_call) && slot(dst, ValueType::Callable);
			}
			case Op::CLEAR:
				return (callee == 0 && dst == NO_SLOT && base == NO_SLOT && arguments(p_call)) || fail("CLEAR with a target, a dst or a base");
			default:
				return fail("not a call");
		}
	}

	/* Instructions */

	// Whether the instruction needs the instance: members, or self as its base.
	bool uses_instance(Op p_op, const uint32_t *o) const {
		if (op_verify_schema(p_op)[0] == '@') {
			return true;
		}
		switch (p_op) {
			case Op::GET_PROPERTY:
			case Op::GET_SIGNAL:
			case Op::METHOD_VALUE:
			case Op::ENGINE_METHOD_VALUE:
				return o[2] == NO_SLOT;
			case Op::SET_PROPERTY:
				return o[1] == NO_SLOT;
			case Op::CALL_AWAIT_ENGINE:
			case Op::CALL_ENGINE:
				return o[2] == NO_SLOT && (o[0] >= module.engine_calls.size() || module.engine_calls[o[0]].static_class.empty());
			case Op::CALL:
			case Op::MAKE_LAMBDA:
				return o[0] < module.functions.size() && !module.functions[o[0]].is_static;
			default:
				return false;
		}
	}

	bool check_schema(Op p_op, const uint32_t *o) {
		const char *s = op_verify_schema(p_op);
		if (*s == '@') {
			s++;
		}
		for (size_t i = 0; s[i]; i++) {
			bool ok = true;
			switch (s[i]) {
				case 'i':
					ok = slot(o[i], ValueType::Int);
					break;
				case 'f':
					ok = slot(o[i], ValueType::Float);
					break;
				case 'b':
					ok = slot(o[i], ValueType::Bool);
					break;
				case 's':
					ok = slot(o[i], ValueType::String);
					break;
				case 'S':
					ok = slot_or_none(o[i], ValueType::String);
					break;
				case 'o':
					ok = slot(o[i], ValueType::Object);
					break;
				case 'O':
					ok = slot_or_none(o[i], ValueType::Object);
					break;
				case 'a':
					ok = slot(o[i], ValueType::Task);
					break;
				case 'v':
					ok = slot(o[i], ValueType::Variant);
					break;
				case 'c':
					ok = slot(o[i], ValueType::Callable);
					break;
				case 'g':
					ok = slot(o[i], ValueType::Signal);
					break;
				case 'n':
					ok = member(o[i], ValueType::String);
					break;
				case 't':
					ok = type_operand(o[i]);
					break;
				case 'l':
					ok = slot_like(o[i], o[0]);
					break;
				case 'm':
					ok = member_like(o[i], o[0]);
					break;
				case 'k':
					ok = constant_like(o[i], o[0]);
					break;
				case 'C':
					ok = index(o[i], module.classes.size(), "class");
					break;
				case 'M':
					ok = index(o[i], module.members.size(), "member");
					break;
				case 'F':
					ok = index(o[i], module.functions.size(), "function");
					break;
				case 'P':
					ok = index(o[i], module.properties.size(), "property");
					break;
				case 'E':
					ok = index(o[i], module.fields.size(), "field");
					break;
				case 'R':
					ok = index(o[i], module.operators.size(), "operator");
					break;
				case 'j':
					ok = target(o[i]);
					break;
			}
			if (!ok) {
				return false;
			}
		}
		return true;
	}

	bool instruction(Op p_op, const uint32_t *o) {
		// A static function runs without an instance (spec §4.8).
		if (function->is_static && uses_instance(p_op, o)) {
			return fail("a static function uses the instance");
		}
		if (!check_schema(p_op, o)) {
			return false;
		}
		if (operand_count(p_op) < 0) {
			return call(p_op, o);
		}
		switch (p_op) {
			case Op::CONST_INT:
				return constant(o[1], ValueType::Int);
			case Op::CONST_FLOAT:
				return constant(o[1], ValueType::Float);
			case Op::CONST_BOOL:
				return o[1] <= 1 || fail("a bool other than 0 and 1");
			case Op::CONST_STRING:
				return constant(o[1], ValueType::String);
			case Op::CONST_VALUE: {
				const ValueType value_type = storage(o[0]);
				return (value_type != ValueType::Bool && value_type != ValueType::Int && value_type != ValueType::Float && value_type != ValueType::String) || fail("a constant of a type that has an instruction of its own");
			}
			case Op::MOVE_8:
				return same_8_byte(frame, o[0], frame, o[1], "an 8-byte move of another type");
			case Op::LOAD_MEMBER_8:
				return same_8_byte(frame, o[0], instance, o[1], "an 8-byte load of a member of another type");
			case Op::STORE_MEMBER_8:
				return same_8_byte(instance, o[0], frame, o[1], "an 8-byte store into a member of another type");
			case Op::LOAD_STATIC:
				return static_variable(o[2], o[3], o[0]);
			case Op::STORE_STATIC:
				return static_variable(o[1], o[2], o[0]);
			case Op::INIT_STATIC:
				return (int32_t(function - module.functions.data()) == module.static_init_function || fail("INIT_STATIC outside the static initializer")) && static_variable(NO_SLOT, o[1], o[0]);
			case Op::EVALUATE: {
				const OperatorCall &op = module.operators[o[0]];
				if (op.op >= OPERATOR_COUNT || !is_native(op.left) || !is_native(op.result) || (op.right != ValueType::Void && !is_native(op.right))) {
					return fail("an operator outside the engine's table");
				}
				return slot(o[1], op.result) && slot(o[2], op.left) && (op.right == ValueType::Void ? (o[3] == NO_SLOT || fail("a unary operator has a right operand")) : slot(o[3], op.right));
			}
			case Op::GET_FIELD:
			case Op::SET_FIELD: {
				const FieldAccess &field = module.fields[o[0]];
				if (!is_native(field.base) || !is_native(field.type)) {
					return fail("a field outside the built-in types");
				}
				return p_op == Op::GET_FIELD ? slot(o[1], field.type) && slot(o[2], field.base) : slot(o[1], field.base) && slot(o[2], field.type);
			}
			case Op::GET_PROPERTY:
			case Op::SET_PROPERTY:
			case Op::GET_MEMBER_OF:
			case Op::SET_MEMBER_OF: {
				const uint32_t property = module.properties[o[0]].type;
				if (is_void(property)) {
					return fail("a property without a type");
				}
				// A property may be self's, NO_SLOT; a member is another object's.
				const auto object = [&](uint32_t p_slot) { return p_op == Op::GET_PROPERTY || p_op == Op::SET_PROPERTY ? slot_or_none(p_slot, ValueType::Object) : slot(p_slot, ValueType::Object); };
				return p_op == Op::GET_PROPERTY || p_op == Op::GET_MEMBER_OF ? slot_like(o[1], property) && object(o[2]) : object(o[1]) && slot_like(o[2], property);
			}
			case Op::CHECK_ENUM:
				return module.types[o[0]].kind == TypeKind::Enum || fail("CHECK_ENUM of no enum");
			case Op::NEW_OBJECT:
				return module.classes[o[0]].interface_name.empty() || fail("new() of an interface");
			case Op::NEW_WITHOUT_INIT:
				return (!module.classes[o[0]].script.empty() || fail("_init() of an engine class")) && (module.classes[o[0]].interface_name.empty() || fail("new() of an interface")) &&
						((function->code.size() >= at + 8 && Op(o[2]) == Op::CALL_METHOD && o[3] < module.method_calls.size() && module.method_calls[o[3]].method == "_init" && o[5] == o[1]) || fail("NEW_WITHOUT_INIT not followed by its _init()"));
			case Op::GET_SCRIPT:
				return !module.classes[o[0]].script.empty() || fail("the script of an engine class");
			case Op::NEW_CONTAINER:
				return (storage(o[0]) == ValueType::Array || storage(o[0]) == ValueType::Dictionary || fail("a container of a type that is not one")) && slot(o[1], storage(o[0]));
			case Op::GET_STACK:
				return ((module.types[o[0]].kind == TypeKind::Array && storage(module.types[o[0]].element) == ValueType::Dictionary) || fail("the call stack into other than an Array[Dictionary]")) && slot(o[1], ValueType::Array);
			case Op::GET_INDEX:
				if (o[0] >= module.types.size() || (!is_indexable(storage(o[0])) && storage(o[0]) != ValueType::String)) {
					return fail("an index into a value that has none");
				}
				return element_operand(o[0], o[1], o[2]) && slot(o[3], storage(o[0])) && index_operand(o[0], o[4]);
			case Op::ITERATE:
				if (o[0] >= module.types.size() || storage(o[0]) == ValueType::Dictionary || (!is_indexable(storage(o[0])) && storage(o[0]) != ValueType::String)) {
					return fail("iterating over a value that has no elements");
				}
				return element_operand(o[0], o[1], o[2]) && slot(o[3], storage(o[0]));
			case Op::SET_INDEX: {
				if (o[0] >= module.types.size() || !is_indexable(storage(o[0]))) {
					return fail("an index into a value that has none");
				}
				if (!slot(o[1], storage(o[0])) || !index_operand(o[0], o[2])) {
					return false;
				}
				const TypeDesc &base = module.types[o[0]];
				if (base.kind == TypeKind::Array || base.kind == TypeKind::Dictionary) {
					return kept_operand(o[3], base.element);
				}
				const ValueType element = packed_element(storage(o[0]));
				return slot(o[3], element == ValueType::Void ? ValueType::Variant : element);
			}
			case Op::IS_NONE:
				return is_option(o[0]) || storage(o[0]) == ValueType::Object || storage(o[0]) == ValueType::Task || storage(o[0]) == ValueType::Variant || fail("None is only tested on objects, tasks and options");
			case Op::SOME:
				return (is_option(o[0]) || fail("Some of a type that is not T?")) && slot_like(o[2], module.types[o[0]].element);
			case Op::LESS:
				return is_boxed(o[0]) || fail("LESS of a type that the engine orders");
			case Op::CHECK_RANGE:
				return o[0] < uint32_t(IntRange::COUNT) || fail("no integer range");
			case Op::ITER_STATE:
				return (o[0] <= 1 || fail("no such iteration method")) && slot(o[2], ValueType::Array);
			case Op::GET_AUTOLOAD:
			case Op::GET_SIGNAL:
			case Op::METHOD_VALUE:
			case Op::ENGINE_METHOD_VALUE:
			case Op::STATIC_VALUE: {
				const uint32_t name = o[p_op == Op::STATIC_VALUE ? 2 : 0];
				return index(name, module.constants.size(), "constant") && (storage(module.constants[name].type) == ValueType::StringName || fail("a name that is not a StringName"));
			}
			case Op::STRUCT_METHOD_VALUE: {
				if (!index(o[0], module.method_calls.size(), "method call")) {
					return false;
				}
				const MethodCall &method = module.method_calls[o[0]];
				return ((method.static_class >= 0 && !method.params.empty()) || fail("a struct method without its class or self")) && slot_like(o[2], method.params[0].type);
			}
			case Op::TO_VARIANT:
			case Op::FROM_VARIANT:
				return is_option(o[0]) || is_struct(o[0]) || is_element(storage(o[0])) || fail("no value type");
			case Op::IS_TYPE:
			case Op::CONSTRUCT_FROM:
				return (o[0] < module.types.size() && !is_option(o[0]) && !is_struct(o[0]) && is_native(storage(o[0]))) || fail("no built-in type");
			case Op::SAME_OBJECT:
				return o[0] <= 3 || fail("SAME_OBJECT of sides beyond two");
			case Op::BOX:
				return (is_boxed(o[0]) || fail("a box of a type that is not boxed")) && (o[3] <= 1 || fail("BOX takes 0 or 1"));
			case Op::ENCODE:
				return (is_boxed(o[0]) && (storage(o[0]) == ValueType::Array || storage(o[0]) == ValueType::Dictionary) || fail("encoding what is not a container of boxes")) && slot(o[1], storage(o[0]));
			case Op::UNWRAP: {
				if (storage(o[0]) == ValueType::Variant) {
					return fail("no type to unwrap to");
				}
				if ((storage(o[0]) == ValueType::Object || storage(o[0]) == ValueType::Task) && !is_option(o[0])) {
					return slot(o[2], storage(o[0]));
				}
				return any(frame, o[2], [&](uint32_t p_source) { return is_option(p_source) && same_layout(module.types[p_source].element, o[0]); }) || fail("unwrapping what is not T?");
			}
			case Op::CHECK_MUST:
			case Op::MARK_ASSIGNED:
				return (o[0] < module.members.size() && module.members[o[0]].tracks_assignment()) || fail("a member that does not track whether it is assigned");
			case Op::UNWRAP_OK: {
				// Ok's value is the field after the variant's index.
				const TypeDesc &type = module.types[o[0]];
				return (is_result(type) || fail("UNWRAP_OK of what is not a Result")) && slot_like(o[1], type.fields[1].type);
			}
			case Op::TRY_FROM: {
				// Err holds a struct whose one field is the message.
				const TypeDesc &type = module.types[o[0]];
				const bool error = is_result(type) && type.fields[2].type < o[0] && module.types[type.fields[2].type].kind == TypeKind::Struct;
				const std::vector<FieldDesc> *fields = error ? &module.types[type.fields[2].type].fields : nullptr;
				return (fields && fields->size() == 1 && storage((*fields)[0].type) == ValueType::String) || fail("TRY_FROM to what is not a Result[T, ConversionError]");
			}
			case Op::TASK_OF:
			case Op::TASK_DELAY:
				return !module.types[o[0]].task_result.empty() || fail("a task made of what is not a Task");
			case Op::TASK_SETTLED: {
				const TypeDesc &settled = module.types[o[0]];
				return (settled.kind == TypeKind::PayloadEnum && settled.name == "Settled" && o[3] <= 1) || fail("TASK_SETTLED of what is not a Settled[T]");
			}
			case Op::AWAIT_TASK:
			case Op::AWAIT_SIGNAL:
				return (function->is_async || fail("await in a synchronous function")) && (o[0] == NO_SLOT || frame.count(o[0]) || fail("unknown await result slot")) && (p_op == Op::AWAIT_SIGNAL || o[2] <= 1 || fail("invalid await ownership"));
			case Op::RETURN:
				return (!is_void(function->result.type) || fail("a void function returns a value")) && slot_like(o[0], function->result.type);
			default:
				return true;
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
		for (const NamedConstant &each : module.named_constants) {
			if (!index(each.constant, module.constants.size(), "constant")) {
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
		Slots statics;
		if (!layout(module.statics, module.statics_size, statics)) {
			return false;
		}
		if (module.static_init_function >= 0 && module.static_init_function < int32_t(module.functions.size())) {
			const Function &initializer = module.functions[module.static_init_function];
			if (!initializer.is_static || !is_void(initializer.result.type) || !initializer.params.empty()) {
				return fail("the static initializer takes or returns a value, or is not static");
			}
		} else if (module.static_init_function != -1) {
			return fail("the static initializer is not a function of the module");
		}
		for (const Member &variable : module.statics) {
			for (const int32_t index : { variable.setter, variable.getter }) {
				if (index < -1 || index >= int32_t(module.functions.size()) || (index >= 0 && !module.functions[index].is_static)) {
					return fail("an accessor of the static variable " + variable.name + " is not a static function of the module");
				}
			}
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
				if (!has_values(param.type)) {
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
			if (call.defaults + (call.is_vararg ? 1 : 0) > call.params.size() || (call.is_vararg && storage(call.params.back().type) != ValueType::Array)) {
				return fail("more default values than parameters in a method call, or a rest parameter that is no Array");
			}
			if (call.static_class >= 0 && (uint32_t(call.static_class) >= module.classes.size() || module.classes[call.static_class].script.empty() || !module.classes[call.static_class].interface_name.empty())) {
				return fail("a static method call of no class of this language");
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
			if ((each.given == NO_SLOT) != each.defaults.empty()) {
				return fail("default values without the slot of the argument count, or that slot without them");
			}
			if (each.given != NO_SLOT && !slot(each.given, ValueType::Int)) {
				return false;
			}
			if (each.rpc.mode > 2 || each.rpc.transfer_mode > 2) {
				return fail("an RPC mode the engine does not have");
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
			if (each.is_vararg && (each.captures + each.defaults.size() >= each.params.size() || storage(each.params[each.fixed_params()].type) != ValueType::Array)) {
				return fail("a rest parameter that is no Array");
			}
			for (const Preset &preset : each.presets) {
				const auto slot = std::find_if(each.slots.begin() + each.params.size(), each.slots.end(), [&](const Slot &p_slot) { return p_slot.offset == preset.offset; });
				const ValueType type = slot == each.slots.end() ? ValueType::Void : storage(slot->type);
				if ((type != ValueType::Int && type != ValueType::Float && type != ValueType::Bool) || (type == ValueType::Bool && preset.bits > 1) || is_option(slot->type)) {
					return fail("a preset value of what is not an int, a float or a bool slot");
				}
			}
			if (each.is_abstract && !each.code.empty()) {
				return fail("an abstract function with code");
			}
			if (each.is_inherited ? (!each.code.empty() || module.base_script.empty() || each.base_signature.empty()) : (!each.is_abstract && !code(each))) {
				return false;
			}
			if (each.is_abstract && (!module.is_abstract || each.is_static)) {
				return fail("an abstract function of a class that is not abstract, or a static one");
			}
			if (each.is_lambda ? each.captures > each.params.size() || !each.defaults.empty() || each.is_pub || each.is_override || each.is_abstract || each.is_inherited : each.captures != 0) {
				return fail("a lambda with more captures than parameters, or declared as a method");
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
	if (!Verifier(p_module, r_error).run()) {
		return false;
	}
	for (const Module &inner : p_module.inner_classes) {
		if (inner.name.empty()) {
			r_error = "an inner class without a name";
			return false;
		}
		if (!verify(inner, r_error)) {
			r_error = inner.name + ": " + r_error;
			return false;
		}
	}
	return true;
}

} // namespace petrichor::bytecode
