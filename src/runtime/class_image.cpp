#include "runtime/class_image.h"

#include "bytecode/opcodes.h"
#include "runtime/method_callable.h"
#include "runtime/text.h"

#include <godot_cpp/classes/script.hpp>

#include <algorithm>
#include <mutex>
#include <unordered_map>

using namespace godot;

namespace petrichor {

using bytecode::ValueType;

static GDExtensionVariantType engine_type(ValueType p_type) {
	return GDExtensionVariantType(variant_type(p_type));
}

// A signature as a number that is the same for the same signature in every
// image, so that a call compares the one it was compiled against with the
// callee's by comparing two numbers.
static uint32_t signature_id(const std::vector<bytecode::TypeDesc> &p_types, const std::vector<bytecode::Slot> &p_params, size_t p_defaults, const bytecode::Slot &p_result, bool p_async) {
	static std::mutex mutex;
	static std::unordered_map<std::string, uint32_t> ids;
	std::lock_guard<std::mutex> lock(mutex);
	return ids.emplace(bytecode::signature_key(p_types, p_params, p_defaults, p_result, p_async), uint32_t(ids.size())).first->second;
}

static std::string signature_key(const bytecode::Module &p_module, const bytecode::Function &p_function) {
	return bytecode::signature_key(p_module.types, p_function.params, p_function.defaults.size(), p_function.result, p_function.is_async);
}

// Whether two slots of two modules have the same type.
static bool same_type(const bytecode::Module &p_a, uint32_t p_type_a, const bytecode::Module &p_b, uint32_t p_type_b) {
	return bytecode::type_key(p_a.types, p_type_a) == bytecode::type_key(p_b.types, p_type_b);
}

std::shared_ptr<const ClassImage> ClassImage::load(bytecode::Module &&p_module, const String &p_path, String &r_error, const std::shared_ptr<const ClassImage> &p_base) {
	if (!check_value_sizes(r_error)) {
		return nullptr;
	}
	auto image = std::make_shared<ClassImage>();
	image->module = std::move(p_module);
	image->base = p_base;
	if (image->module.base_script.empty() != !p_base || (p_base && (p_base->path != to_godot(image->module.base_script) || p_base->module.base_class != image->module.base_class || image->module.inherited_members != p_base->module.members.size()))) {
		r_error = "Base script does not match this class. Rebuild the project.";
		return nullptr;
	}
	for (uint32_t i = 0; i < image->module.inherited_members; ++i) {
		const auto &a = image->module.members[i];
		const auto &b = p_base->module.members[i];
		if (a.name != b.name || a.offset != b.offset || !same_type(image->module, a.type, p_base->module, b.type) || a.is_pub != b.is_pub || a.is_export != b.is_export || a.is_onready != b.is_onready || a.setter != b.setter || a.getter != b.getter) {
			r_error = "Inherited member layout changed. Rebuild the project.";
			return nullptr;
		}
	}
	for (size_t i = 0; p_base && i < p_base->module.functions.size(); i++) {
		const std::string &name = p_base->module.functions[i].name;
		if ((name.empty() || name[0] != '@') && (i >= image->module.functions.size() || image->module.functions[i].name != name)) {
			r_error = "Base method declaration changed. Rebuild the project.";
			return nullptr;
		}
	}
	image->path = p_path;
	image->source = p_path;
	const bytecode::Module &module = image->module;
	image->global_name = to_godot(module.global_name);
	image->base_class = to_godot(module.base_class);
	image->types = type_images(module.types);
	const std::vector<TypeImage> &types = image->types;

	for (size_t i = 0; i < module.functions.size(); i++) {
		const bytecode::Function &function = module.functions[i];
		const StringName name = to_godot(function.name);
		image->function_names.push_back(name);
		image->function_lookup.insert(name, int(i));
		const int parent = p_base ? p_base->find_function(name) : -1;
		if (!function.name.empty() && function.name[0] != '@') {
			const std::string expected = parent < 0 ? std::string() : signature_key(p_base->module, p_base->module.functions[parent]);
			if (parent >= 0 && (function.is_static != p_base->module.functions[parent].is_static || function.is_pub != p_base->module.functions[parent].is_pub)) {
				r_error = "Base method declaration changed. Rebuild the project.";
				return nullptr;
			}
			if (function.base_signature != expected || (function.is_inherited && (parent < 0 || signature_key(module, function) != expected))) {
				r_error = "Base method signature changed. Rebuild the project.";
				return nullptr;
			}
		}
		FunctionOrigin origin;
		if (function.is_inherited) {
			origin.image = p_base;
			origin.index = parent;
			resolve_function(origin.image, origin.index);
		}
		image->function_origins.push_back(origin);
		if (function.name == "_init" && function.params.empty()) {
			image->user_init = int(i);
		}
		if (function.name == "_notification" && function.params.size() == 1 && types[function.params[0].type].storage == ValueType::Int) {
			image->notification_function = int(i);
		}
		if (function.name == "_to_string" && function.params.empty() && types[function.result.type].storage == ValueType::String) {
			image->to_string_function = int(i);
		}
		image->function_signatures.push_back(signature_id(module.types, function.params, function.defaults.size(), function.result, function.is_async));
		image->frames.push_back(SlotLayout::of(function.frame_size, function.slots, types));
	}
	for (size_t i = 0; i < module.members.size(); i++) {
		const StringName name = to_godot(module.members[i].name);
		image->member_names.push_back(name);
		image->member_lookup.insert(name, int(i));
	}
	image->members_layout = SlotLayout::of(std::max<uint32_t>(module.instance_size, 8), module.members, types);

	for (const bytecode::Constant &constant : module.constants) {
		image->strings.push_back(types[constant.type].storage == ValueType::String ? to_godot(constant.s) : String());
		image->values.emplace_back();
		if (!constant_value(types, constant, image->values.back(), r_error)) {
			return nullptr;
		}
	}
	for (const bytecode::EngineCall &call : module.engine_calls) {
		image->engine_methods.push_back(to_godot(call.method));
	}
	for (const bytecode::PropertyAccess &property : module.properties) {
		image->properties.push_back(to_godot(property.name));
	}
	for (const bytecode::UtilityCall &call : module.utility_calls) {
		const StringName name = to_godot(call.function);
		const GDExtensionPtrUtilityFunction pointer = gdextension_interface::variant_get_ptr_utility_function(name._native_ptr(), call.hash);
		if (!pointer) {
			r_error = "the engine has no utility function " + to_godot(call.function) + " with this signature";
			return nullptr;
		}
		image->utilities.push_back(pointer);
	}
	for (const bytecode::OperatorCall &call : module.operators) {
		const GDExtensionVariantType right = call.right == ValueType::Void ? GDEXTENSION_VARIANT_TYPE_NIL : engine_type(call.right);
		const GDExtensionPtrOperatorEvaluator pointer = gdextension_interface::variant_get_ptr_operator_evaluator(GDExtensionVariantOperator(call.op), engine_type(call.left), right);
		if (!pointer) {
			r_error = vformat("the engine has no operator %d for %s and %s", call.op, bytecode::value_type_name(call.left), bytecode::value_type_name(call.right));
			return nullptr;
		}
		image->evaluators.push_back(pointer);
	}
	for (const bytecode::FieldAccess &field : module.fields) {
		const StringName name = to_godot(field.name);
		const GDExtensionPtrGetter getter = gdextension_interface::variant_get_ptr_getter(engine_type(field.base), name._native_ptr());
		const GDExtensionPtrSetter setter = gdextension_interface::variant_get_ptr_setter(engine_type(field.base), name._native_ptr());
		if (!getter || !setter) {
			r_error = vformat("%s has no member %s", bytecode::value_type_name(field.base), name);
			return nullptr;
		}
		image->getters.push_back(getter);
		image->setters.push_back(setter);
	}
	for (const bytecode::ConstructorCall &call : module.constructors) {
		const GDExtensionPtrConstructor pointer = gdextension_interface::variant_get_ptr_constructor(engine_type(call.type), int32_t(call.index));
		if (!pointer) {
			r_error = vformat("%s has no constructor %d", bytecode::value_type_name(call.type), call.index);
			return nullptr;
		}
		image->constructors.push_back(pointer);
	}
	for (const bytecode::BuiltinMethodCall &call : module.builtin_methods) {
		const StringName name = to_godot(call.method);
		// The engine finds the method by its hash; the parameter types recorded
		// with it come from the compiler.
		const GDExtensionPtrBuiltInMethod pointer = gdextension_interface::variant_get_ptr_builtin_method(engine_type(call.base), name._native_ptr(), call.hash);
		if (!pointer) {
			r_error = vformat("%s has no method %s with this signature", bytecode::value_type_name(call.base), name);
			return nullptr;
		}
		image->builtin_methods.push_back(pointer);
		const bool dictionary = call.base == ValueType::Dictionary;
		const bool inserts = dictionary && (call.method == "set" || call.method == "get_or_add");
		const bool hands_out = dictionary && (call.method == "keys" || call.method == "find_key");
		image->key_roles.push_back(inserts ? KeyRole::Inserts : (hands_out ? KeyRole::HandsOut : KeyRole::None));
	}
	for (const bytecode::ClassRef &ref : module.classes) {
		image->classes.push_back(to_godot(ref.name));
		image->class_scripts.push_back(to_godot(ref.script));
		image->class_interfaces.push_back(ref.interface_name.empty() ? String() : to_godot(ref.script + "::" + ref.interface_name));
	}
	for (const bytecode::MethodCall &call : module.method_calls) {
		image->method_names.push_back(to_godot(call.method));
		image->call_signatures.push_back(signature_id(module.types, call.params, call.defaults, call.result, call.is_async));
	}
	return image;
}

// Built with the constructor that CONSTRUCT calls, from the arguments in their
// slots' layout.
static bool construct_constant(const std::vector<TypeImage> &p_types, const bytecode::Constant &p_constant, Variant &r_value, String &r_error) {
	const ValueType type = p_types[p_constant.type].storage;
	const GDExtensionPtrConstructor constructor = gdextension_interface::variant_get_ptr_constructor(engine_type(type), p_constant.constructor);
	if (!constructor) {
		r_error = vformat("%s has no constructor %d", bytecode::value_type_name(type), p_constant.constructor);
		return false;
	}
	std::vector<bytecode::Slot> slots;
	uint32_t size = 0;
	for (const bytecode::Constant &arg : p_constant.args) {
		bytecode::Slot slot;
		slot.type = arg.type;
		slot.offset = size;
		size += bytecode::slot_size(p_types[arg.type].storage);
		slots.push_back(slot);
	}
	const SlotLayout layout = SlotLayout::of(size, slots, p_types);
	std::vector<uint64_t> storage(size / 8 + 1); // Slots are 8-byte aligned.
	uint8_t *base = reinterpret_cast<uint8_t *>(storage.data());
	layout.construct(base);
	std::vector<const void *> pointers;
	bool ok = true;
	for (size_t i = 0; ok && i < slots.size(); i++) {
		Variant value;
		ok = constant_value(p_types, p_constant.args[i], value, r_error) && from_variant(p_types[slots[i].type].storage, value, base + slots[i].offset);
		pointers.push_back(base + slots[i].offset);
	}
	if (ok) {
		// The engine constructs into memory that holds no value.
		alignas(16) uint8_t result[64];
		constructor(result, pointers.data());
		r_value = to_variant(type, result);
		destroy_value(type, result);
	}
	layout.destroy(base);
	return ok;
}

bool constant_value(const std::vector<TypeImage> &p_types, const bytecode::Constant &p_constant, Variant &r_value, String &r_error) {
	const ValueType type = p_types[p_constant.type].storage;
	if (!p_constant.builtin.empty()) {
		const StringName name = to_godot(p_constant.builtin);
		Variant value; // The engine writes over it.
		gdextension_interface::variant_get_constant_value(engine_type(type), name._native_ptr(), value._native_ptr());
		if (value.get_type() != variant_type(type)) {
			r_error = vformat("the engine has no %s constant %s", bytecode::value_type_name(type), name);
			return false;
		}
		r_value = value;
		return true;
	}
	if (p_constant.constructor >= 0) {
		return construct_constant(p_types, p_constant, r_value, r_error);
	}
	if (type == ValueType::Array) {
		const ValueCheck &check = p_types[p_constant.type].check;
		Array array = typed_array(check.element, check.class_name, check.script);
		for (const bytecode::Constant &element : p_constant.args) {
			Variant value;
			if (!constant_value(p_types, element, value, r_error)) {
				return false;
			}
			array.push_back(value);
		}
		r_value = array;
		return true;
	}
	if (type == ValueType::Dictionary) {
		r_value = typed_dictionary(p_types[p_constant.type].check);
		return true;
	}
	switch (type) {
		case ValueType::Bool:
			r_value = p_constant.b;
			break;
		case ValueType::Int:
			r_value = p_constant.i;
			break;
		case ValueType::Float:
			r_value = p_constant.f;
			break;
		case ValueType::String:
			r_value = to_godot(p_constant.s);
			break;
		case ValueType::StringName:
			r_value = StringName(to_godot(p_constant.s));
			break;
		case ValueType::NodePath:
			r_value = NodePath(to_godot(p_constant.s));
			break;
		default:
			r_value = default_variant(type); // None for Object and Variant.
			break;
	}
	return true;
}

bool ClassImage::fits_class(const Object *p_object, uint32_t p_index) const {
	if (!p_object->is_class(classes[p_index])) {
		return false;
	}
	if (!class_interfaces[p_index].is_empty()) {
		return implements_interface(const_cast<Object *>(p_object), class_interfaces[p_index]);
	}
	return has_script(p_object->get_script(), class_scripts[p_index]);
}

void ClassImage::resolve_function(std::shared_ptr<const ClassImage> &r_image, int &r_index) {
	const FunctionOrigin origin = r_image->function_origins[r_index];
	if (origin.image) {
		r_image = origin.image;
		r_index = origin.index;
	}
}

bool ClassImage::has_signature(const StringName &p_name, uint32_t p_signature) const {
	for (const ClassImage *image = this; image; image = image->base.get()) {
		const int index = image->find_function(p_name);
		if (index >= 0 && image->function_signatures[index] == p_signature) {
			return true;
		}
	}
	return false;
}

int ClassImage::find_function(const StringName &p_name) const {
	const int *index = function_lookup.getptr(p_name);
	return index ? *index : -1;
}

int ClassImage::find_member(const StringName &p_name) const {
	const int *index = member_lookup.getptr(p_name);
	return index ? *index : -1;
}

/* Hot reload */

namespace {

// Two modules compared with each other, so that their types are compared by
// what they are rather than by index.
struct ModulePair {
	const bytecode::Module &a;
	const bytecode::Module &b;

	bool same_type(uint32_t p_a, uint32_t p_b) const { return ::petrichor::same_type(a, p_a, b, p_b); }

	bool same_constant(const bytecode::Constant &p_a, const bytecode::Constant &p_b) const {
		return same_type(p_a.type, p_b.type) && p_a.b == p_b.b && p_a.i == p_b.i && p_a.f == p_b.f && p_a.s == p_b.s && p_a.builtin == p_b.builtin &&
				p_a.constructor == p_b.constructor && std::equal(p_a.args.begin(), p_a.args.end(), p_b.args.begin(), p_b.args.end(), [&](const auto &x, const auto &y) { return same_constant(x, y); });
	}

	bool same_slot(const bytecode::Slot &p_a, const bytecode::Slot &p_b) const {
		return p_a.name == p_b.name && same_type(p_a.type, p_b.type);
	}
};

template <typename T, typename Same>
bool same_prefix(const std::vector<T> &p_a, const std::vector<T> &p_b, uint32_t p_count, Same p_same) {
	for (uint32_t i = 0; i < p_count; i++) {
		if (!p_same(p_a[i], p_b[i])) {
			return false;
		}
	}
	return true;
}

// The same instructions, apart from the lines LINE records, which change when a
// line is added above the members.
bool same_code(const bytecode::Module &p_a, int32_t p_function_a, const bytecode::Module &p_b, int32_t p_function_b) {
	if ((p_function_a < 0) != (p_function_b < 0)) {
		return false;
	}
	if (p_function_a < 0) {
		return true;
	}
	const std::vector<uint32_t> &a = p_a.functions[p_function_a].code;
	const std::vector<uint32_t> &b = p_b.functions[p_function_b].code;
	if (a.size() != b.size()) {
		return false;
	}
	for (size_t pc = 0; pc < a.size();) {
		const size_t length = bytecode::instruction_length(a.data() + pc);
		if (a[pc] != b[pc] || (bytecode::Op(a[pc]) != bytecode::Op::LINE && !std::equal(a.begin() + pc, a.begin() + pc + length, b.begin() + pc))) {
			return false;
		}
		pc += length;
	}
	return true;
}

// The init and ready functions and the table entries they use.
bool same_initializers(const bytecode::Module &p_a, const bytecode::Module &p_b) {
	if (!same_code(p_a, p_a.init_function, p_b, p_b.init_function) || !same_code(p_a, p_a.ready_function, p_b, p_b.ready_function)) {
		return false;
	}
	const bytecode::TablePrefix &x = p_a.init_tables;
	const bytecode::TablePrefix &y = p_b.init_tables;
	if (x.types != y.types || x.constants != y.constants || x.engine_calls != y.engine_calls || x.properties != y.properties || x.utility_calls != y.utility_calls || x.operators != y.operators ||
			x.fields != y.fields || x.constructors != y.constructors || x.builtin_methods != y.builtin_methods || x.classes != y.classes || x.method_calls != y.method_calls) {
		return false;
	}
	// The code refers to the same types by the same indices.
	const ModulePair pair{ p_a, p_b };
	return same_prefix(p_a.types, p_b.types, x.types, [](const auto &a, const auto &b) { return a == b; }) &&
			same_prefix(p_a.constants, p_b.constants, x.constants, [&](const auto &a, const auto &b) { return pair.same_constant(a, b); }) &&
			same_prefix(p_a.engine_calls, p_b.engine_calls, x.engine_calls, [](const auto &a, const auto &b) { return a.method == b.method && a.result_type == b.result_type; }) &&
			same_prefix(p_a.properties, p_b.properties, x.properties, [](const auto &a, const auto &b) { return a.name == b.name && a.type == b.type; }) &&
			same_prefix(p_a.utility_calls, p_b.utility_calls, x.utility_calls, [](const auto &a, const auto &b) { return a.function == b.function && a.hash == b.hash && a.is_vararg == b.is_vararg && a.params == b.params && a.return_type == b.return_type && a.result_type == b.result_type; }) &&
			same_prefix(p_a.operators, p_b.operators, x.operators, [](const auto &a, const auto &b) { return a.op == b.op && a.left == b.left && a.right == b.right && a.result == b.result; }) &&
			same_prefix(p_a.fields, p_b.fields, x.fields, [](const auto &a, const auto &b) { return a.base == b.base && a.name == b.name && a.type == b.type; }) &&
			same_prefix(p_a.constructors, p_b.constructors, x.constructors, [](const auto &a, const auto &b) { return a.type == b.type && a.index == b.index && a.params == b.params; }) &&
			same_prefix(p_a.builtin_methods, p_b.builtin_methods, x.builtin_methods, [](const auto &a, const auto &b) { return a.base == b.base && a.method == b.method && a.hash == b.hash && a.is_vararg == b.is_vararg && a.is_const == b.is_const && a.params == b.params && a.return_type == b.return_type && a.result_type == b.result_type; }) &&
			same_prefix(p_a.classes, p_b.classes, x.classes, [](const auto &a, const auto &b) { return a == b; }) &&
			same_prefix(p_a.method_calls, p_b.method_calls, x.method_calls, [&](const auto &a, const auto &b) { return a.method == b.method && bytecode::signature_key(p_a.types, a.params, a.defaults, a.result, a.is_async) == bytecode::signature_key(p_b.types, b.params, b.defaults, b.result, b.is_async); });
}

} // namespace

bool ClassImage::same_declarations(const ClassImage &p_other) const {
	const bytecode::Module &a = module;
	const bytecode::Module &b = p_other.module;
	if (a.base_script != b.base_script || bool(base) != bool(p_other.base) || (base && !base->same_declarations(*p_other.base))) {
		return false;
	}
	if (a.declarations_only || b.declarations_only) {
		return false;
	}
	if (a.global_name != b.global_name || a.base_class != b.base_class || a.instance_size != b.instance_size || a.members.size() != b.members.size() || a.functions.size() != b.functions.size()) {
		return false;
	}
	const ModulePair pair{ a, b };
	for (size_t i = 0; i < a.members.size(); i++) {
		const bytecode::Member &x = a.members[i];
		const bytecode::Member &y = b.members[i];
		if (!pair.same_slot(x, y) || x.offset != y.offset || x.is_onready != y.is_onready || x.setter != y.setter || x.getter != y.getter ||
				x.is_pub != y.is_pub || x.is_export != y.is_export || x.hint != y.hint || x.hint_string != y.hint_string || x.has_initial != y.has_initial || !pair.same_constant(x.initial, y.initial)) {
			return false;
		}
	}
	if (a.groups.size() != b.groups.size() || a.signals.size() != b.signals.size()) {
		return false;
	}
	for (size_t i = 0; i < a.signals.size(); i++) {
		const bytecode::SignalInfo &x = a.signals[i];
		const bytecode::SignalInfo &y = b.signals[i];
		if (x.name != y.name || x.is_pub != y.is_pub || x.params.size() != y.params.size()) {
			return false;
		}
		for (size_t p = 0; p < x.params.size(); p++) {
			if (!pair.same_slot(x.params[p], y.params[p])) {
				return false;
			}
		}
	}
	for (size_t i = 0; i < a.groups.size(); i++) {
		const bytecode::ExportGroup &x = a.groups[i];
		const bytecode::ExportGroup &y = b.groups[i];
		if (x.name != y.name || x.prefix != y.prefix || x.level != y.level || x.before_member != y.before_member) {
			return false;
		}
	}
	if (a.interfaces != b.interfaces) {
		return false;
	}
	// Initializers belong to the declarations, not to a body.
	if (!same_initializers(a, b)) {
		return false;
	}
	// Signatures; the line, the frame and the code belong to the body.
	for (size_t i = 0; i < a.functions.size(); i++) {
		if (int32_t(i) == a.init_function || int32_t(i) == a.ready_function) {
			continue;
		}
		const bytecode::Function &x = a.functions[i];
		const bytecode::Function &y = b.functions[i];
		if (x.is_inherited != y.is_inherited || x.base_signature != y.base_signature || x.name != y.name || !pair.same_slot(x.result, y.result) || x.is_pub != y.is_pub || x.is_override != y.is_override || x.is_exposed != y.is_exposed ||
				x.is_async != y.is_async || x.is_static != y.is_static || x.params.size() != y.params.size() || x.defaults.size() != y.defaults.size()) {
			return false;
		}
		for (size_t d = 0; d < x.defaults.size(); d++) {
			if (!pair.same_constant(x.defaults[d], y.defaults[d])) {
				return false;
			}
		}
		for (size_t p = 0; p < x.params.size(); p++) {
			if (!pair.same_slot(x.params[p], y.params[p])) {
				return false;
			}
		}
	}
	return true;
}

} // namespace petrichor
