#include "runtime/class_image.h"

#include "bytecode/opcodes.h"
#include "runtime/method_callable.h"

#include <godot_cpp/classes/script.hpp>

#include <algorithm>
#include <mutex>
#include <unordered_map>

using namespace godot;

namespace petrichor {

using bytecode::ValueType;

static String from_utf8(const std::string &p_text) {
	return String::utf8(p_text.c_str(), int64_t(p_text.size()));
}

static GDExtensionVariantType engine_type(ValueType p_type) {
	return GDExtensionVariantType(variant_type(p_type));
}

// A signature as a number that is the same for the same signature in every
// image, so that a call compares the one it was compiled against with the
// callee's by comparing two numbers.
static uint32_t signature_id(const std::vector<bytecode::Slot> &p_params, size_t p_defaults, const bytecode::Slot &p_result) {
	static std::mutex mutex;
	static std::unordered_map<std::string, uint32_t> ids;
	std::lock_guard<std::mutex> lock(mutex);
	return ids.emplace(bytecode::signature_key(p_params, p_defaults, p_result), uint32_t(ids.size())).first->second;
}

std::shared_ptr<const ClassImage> ClassImage::load(bytecode::Module &&p_module, const String &p_path, String &r_error) {
	if (!check_value_sizes(r_error)) {
		return nullptr;
	}
	auto image = std::make_shared<ClassImage>();
	image->module = std::move(p_module);
	image->path = p_path;
	image->source = p_path;
	const bytecode::Module &module = image->module;
	image->global_name = from_utf8(module.global_name);
	image->base_class = from_utf8(module.base_class);

	for (size_t i = 0; i < module.functions.size(); i++) {
		const bytecode::Function &function = module.functions[i];
		const StringName name = from_utf8(function.name);
		image->function_names.push_back(name);
		image->function_lookup.insert(name, int(i));
		if (function.name == "_init" && function.params.empty()) {
			image->user_init = int(i);
		}
		if (function.name == "_notification" && function.params.size() == 1 && function.params[0].type == ValueType::Int) {
			image->notification_function = int(i);
		}
		if (function.name == "_to_string" && function.params.empty() && function.result.type == ValueType::String) {
			image->to_string_function = int(i);
		}
		image->function_signatures.push_back(signature_id(function.params, function.defaults.size(), function.result));
		image->frames.push_back(SlotLayout::of(function.frame_size, function.slots));
		std::vector<ValueCheck> checks;
		for (const bytecode::Slot &param : function.params) {
			checks.push_back(ValueCheck(param));
		}
		image->param_checks.push_back(std::move(checks));
		std::vector<Variant> defaults;
		for (const bytecode::Constant &value : function.defaults) {
			defaults.push_back(literal_value(value));
		}
		image->param_defaults.push_back(std::move(defaults));
	}
	std::vector<bytecode::Slot> member_slots;
	for (size_t i = 0; i < module.members.size(); i++) {
		const bytecode::Member &member = module.members[i];
		const StringName name = from_utf8(member.name);
		image->member_names.push_back(name);
		image->member_checks.push_back(ValueCheck(member));
		bytecode::Slot slot;
		slot.type = member.type;
		slot.offset = member.offset;
		member_slots.push_back(slot);
		image->member_lookup.insert(name, int(i));
	}
	image->members_layout = SlotLayout::of(std::max<uint32_t>(module.instance_size, 8), member_slots);

	for (const bytecode::Constant &constant : module.constants) {
		image->strings.push_back(constant.type == ValueType::String ? from_utf8(constant.s) : String());
		Variant value;
		if (!constant.builtin.empty()) {
			const StringName name = from_utf8(constant.builtin);
			gdextension_interface::variant_get_constant_value(engine_type(constant.type), name._native_ptr(), value._native_ptr());
		} else if (constant.type == ValueType::StringName) {
			value = StringName(from_utf8(constant.s));
		} else if (constant.type == ValueType::NodePath) {
			value = NodePath(from_utf8(constant.s));
		} else {
			value = default_variant(constant.type); // None for Object.
		}
		if (constant.type != ValueType::Object && value.get_type() != variant_type(constant.type)) {
			r_error = vformat("the engine has no %s constant %s", bytecode::value_type_name(constant.type), from_utf8(constant.builtin));
			return nullptr;
		}
		image->values.push_back(value);
	}
	for (const bytecode::EngineCall &call : module.engine_calls) {
		image->engine_methods.push_back(from_utf8(call.method));
	}
	for (const bytecode::PropertyAccess &property : module.properties) {
		image->properties.push_back(from_utf8(property.name));
	}
	for (const bytecode::UtilityCall &call : module.utility_calls) {
		const StringName name = from_utf8(call.function);
		const GDExtensionPtrUtilityFunction pointer = gdextension_interface::variant_get_ptr_utility_function(name._native_ptr(), call.hash);
		if (!pointer) {
			r_error = "the engine has no utility function " + from_utf8(call.function) + " with this signature";
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
		const StringName name = from_utf8(field.name);
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
		const StringName name = from_utf8(call.method);
		// The engine finds the method by its hash; the parameter types recorded
		// with it come from the compiler.
		const GDExtensionPtrBuiltInMethod pointer = gdextension_interface::variant_get_ptr_builtin_method(engine_type(call.base), name._native_ptr(), call.hash);
		if (!pointer) {
			r_error = vformat("%s has no method %s with this signature", bytecode::value_type_name(call.base), name);
			return nullptr;
		}
		image->builtin_methods.push_back(pointer);
	}
	for (const bytecode::ClassRef &ref : module.classes) {
		image->classes.push_back(from_utf8(ref.name));
		image->class_scripts.push_back(from_utf8(ref.script));
		image->class_interfaces.push_back(ref.interface_name.empty() ? String() : from_utf8(ref.script + "::" + ref.interface_name));
	}
	for (const bytecode::MethodCall &call : module.method_calls) {
		image->method_names.push_back(from_utf8(call.method));
		image->call_signatures.push_back(signature_id(call.params, call.defaults, call.result));
	}
	return image;
}

Variant literal_value(const bytecode::Constant &p_constant) {
	switch (p_constant.type) {
		case ValueType::Bool:
			return p_constant.b;
		case ValueType::Int:
			return p_constant.i;
		case ValueType::Float:
			return p_constant.f;
		case ValueType::String:
			return from_utf8(p_constant.s);
		case ValueType::StringName:
			return StringName(from_utf8(p_constant.s));
		case ValueType::NodePath:
			return NodePath(from_utf8(p_constant.s));
		default:
			return Variant(); // None.
	}
}

bool ClassImage::fits_class(const Object *p_object, uint32_t p_index) const {
	if (!p_object->is_class(classes[p_index])) {
		return false;
	}
	if (!class_interfaces[p_index].is_empty()) {
		return implements_interface(const_cast<Object *>(p_object), class_interfaces[p_index]);
	}
	if (!class_scripts[p_index].is_empty()) {
		// Classes of this language extend engine classes only, so far: the
		// object's script is the class's.
		const Ref<Script> script = p_object->get_script();
		return script.is_valid() && script->get_path() == class_scripts[p_index];
	}
	return true;
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

bool same_constant(const bytecode::Constant &p_a, const bytecode::Constant &p_b) {
	return p_a.type == p_b.type && p_a.b == p_b.b && p_a.i == p_b.i && p_a.f == p_b.f && p_a.s == p_b.s && p_a.builtin == p_b.builtin;
}

bool same_slot(const bytecode::Slot &p_a, const bytecode::Slot &p_b) {
	return p_a.name == p_b.name && p_a.type == p_b.type && p_a.class_name == p_b.class_name && p_a.script == p_b.script && p_a.nullable == p_b.nullable && p_a.option == p_b.option && p_a.element == p_b.element;
}

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
	if (x.constants != y.constants || x.engine_calls != y.engine_calls || x.properties != y.properties || x.utility_calls != y.utility_calls || x.operators != y.operators ||
			x.fields != y.fields || x.constructors != y.constructors || x.builtin_methods != y.builtin_methods || x.classes != y.classes || x.method_calls != y.method_calls) {
		return false;
	}
	return same_prefix(p_a.constants, p_b.constants, x.constants, same_constant) &&
			same_prefix(p_a.engine_calls, p_b.engine_calls, x.engine_calls, [](const auto &a, const auto &b) { return a.method == b.method && a.return_type == b.return_type; }) &&
			same_prefix(p_a.properties, p_b.properties, x.properties, [](const auto &a, const auto &b) { return a.name == b.name && a.type == b.type; }) &&
			same_prefix(p_a.utility_calls, p_b.utility_calls, x.utility_calls, [](const auto &a, const auto &b) {
				return a.function == b.function && a.hash == b.hash && a.is_vararg == b.is_vararg && a.params == b.params && a.return_type == b.return_type && a.result_type == b.result_type;
			}) &&
			same_prefix(p_a.operators, p_b.operators, x.operators, [](const auto &a, const auto &b) { return a.op == b.op && a.left == b.left && a.right == b.right && a.result == b.result; }) &&
			same_prefix(p_a.fields, p_b.fields, x.fields, [](const auto &a, const auto &b) { return a.base == b.base && a.name == b.name && a.type == b.type; }) &&
			same_prefix(p_a.constructors, p_b.constructors, x.constructors, [](const auto &a, const auto &b) { return a.type == b.type && a.index == b.index && a.params == b.params; }) &&
			same_prefix(p_a.builtin_methods, p_b.builtin_methods, x.builtin_methods, [](const auto &a, const auto &b) {
				return a.base == b.base && a.method == b.method && a.hash == b.hash && a.is_vararg == b.is_vararg && a.params == b.params && a.return_type == b.return_type && a.result_type == b.result_type;
			}) &&
			same_prefix(p_a.classes, p_b.classes, x.classes, [](const auto &a, const auto &b) { return a == b; }) &&
			same_prefix(p_a.method_calls, p_b.method_calls, x.method_calls, [](const auto &a, const auto &b) {
				return a.method == b.method && bytecode::signature_key(a.params, a.defaults, a.result) == bytecode::signature_key(b.params, b.defaults, b.result);
			});
}

} // namespace

bool ClassImage::same_declarations(const ClassImage &p_other) const {
	const bytecode::Module &a = module;
	const bytecode::Module &b = p_other.module;
	if (a.declarations_only || b.declarations_only) {
		return false;
	}
	if (a.global_name != b.global_name || a.base_class != b.base_class || a.instance_size != b.instance_size || a.members.size() != b.members.size() || a.functions.size() != b.functions.size()) {
		return false;
	}
	for (size_t i = 0; i < a.members.size(); i++) {
		const bytecode::Member &x = a.members[i];
		const bytecode::Member &y = b.members[i];
		if (x.name != y.name || x.type != y.type || x.offset != y.offset || x.class_name != y.class_name || x.script != y.script || x.nullable != y.nullable || x.option != y.option || x.element != y.element || x.is_onready != y.is_onready ||
				x.is_pub != y.is_pub || x.is_export != y.is_export || x.hint != y.hint || x.hint_string != y.hint_string || x.has_initial != y.has_initial || !same_constant(x.initial, y.initial)) {
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
			if (!same_slot(x.params[p], y.params[p])) {
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
	// Non-literal initializers belong to the declarations, not to a body.
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
		if (x.name != y.name || !same_slot(x.result, y.result) || x.is_pub != y.is_pub || x.is_override != y.is_override || x.is_exposed != y.is_exposed ||
				x.is_static != y.is_static || x.params.size() != y.params.size() || x.defaults.size() != y.defaults.size()) {
			return false;
		}
		for (size_t d = 0; d < x.defaults.size(); d++) {
			if (!same_constant(x.defaults[d], y.defaults[d])) {
				return false;
			}
		}
		for (size_t p = 0; p < x.params.size(); p++) {
			if (!same_slot(x.params[p], y.params[p])) {
				return false;
			}
		}
	}
	return true;
}

} // namespace petrichor
