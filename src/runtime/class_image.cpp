#include "runtime/class_image.h"

using namespace godot;

namespace petrichor {

using bytecode::ValueType;

static String from_utf8(const std::string &p_text) {
	return String::utf8(p_text.c_str(), int64_t(p_text.size()));
}

static GDExtensionVariantType engine_type(ValueType p_type) {
	return GDExtensionVariantType(variant_type(p_type));
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
		const StringName name = from_utf8(module.functions[i].name);
		image->function_names.push_back(name);
		image->function_lookup.insert(name, int(i));
	}
	for (size_t i = 0; i < module.members.size(); i++) {
		const bytecode::Member &member = module.members[i];
		const StringName name = from_utf8(member.name);
		image->member_names.push_back(name);
		image->member_classes.push_back(from_utf8(member.class_name));
		bytecode::Slot slot;
		slot.name = member.name;
		slot.type = member.type;
		slot.offset = member.offset;
		image->member_slots.push_back(slot);
		image->member_lookup.insert(name, int(i));
	}

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
			value = default_variant(constant.type);
		}
		if (value.get_type() != variant_type(constant.type)) {
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
		// The engine checks the signature through the hash.
		const GDExtensionPtrBuiltInMethod pointer = gdextension_interface::variant_get_ptr_builtin_method(engine_type(call.base), name._native_ptr(), call.hash);
		if (!pointer) {
			r_error = vformat("%s has no method %s with this signature", bytecode::value_type_name(call.base), name);
			return nullptr;
		}
		image->builtin_methods.push_back(pointer);
	}
	for (const std::string &name : module.classes) {
		image->classes.push_back(from_utf8(name));
	}
	return image;
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
	return p_a.name == p_b.name && p_a.type == p_b.type && p_a.class_name == p_b.class_name && p_a.nullable == p_b.nullable;
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

// The init function and the table entries it uses.
bool same_initializers(const bytecode::Module &p_a, const bytecode::Module &p_b) {
	if ((p_a.init_function < 0) != (p_b.init_function < 0)) {
		return false;
	}
	if (p_a.init_function < 0) {
		return true;
	}
	if (p_a.functions[p_a.init_function].code != p_b.functions[p_b.init_function].code) {
		return false;
	}
	const bytecode::TablePrefix &x = p_a.init_tables;
	const bytecode::TablePrefix &y = p_b.init_tables;
	if (x.constants != y.constants || x.engine_calls != y.engine_calls || x.properties != y.properties || x.utility_calls != y.utility_calls || x.operators != y.operators ||
			x.fields != y.fields || x.constructors != y.constructors || x.builtin_methods != y.builtin_methods || x.classes != y.classes) {
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
				return a.base == b.base && a.method == b.method && a.hash == b.hash && a.params == b.params && a.return_type == b.return_type && a.result_type == b.result_type;
			}) &&
			same_prefix(p_a.classes, p_b.classes, x.classes, [](const auto &a, const auto &b) { return a == b; });
}

} // namespace

bool ClassImage::same_declarations(const ClassImage &p_other) const {
	const bytecode::Module &a = module;
	const bytecode::Module &b = p_other.module;
	if (a.global_name != b.global_name || a.base_class != b.base_class || a.instance_size != b.instance_size || a.members.size() != b.members.size() || a.functions.size() != b.functions.size()) {
		return false;
	}
	for (size_t i = 0; i < a.members.size(); i++) {
		const bytecode::Member &x = a.members[i];
		const bytecode::Member &y = b.members[i];
		if (x.name != y.name || x.type != y.type || x.offset != y.offset || x.class_name != y.class_name || x.nullable != y.nullable ||
				x.is_pub != y.is_pub || x.is_export != y.is_export || x.has_initial != y.has_initial || !same_constant(x.initial, y.initial)) {
			return false;
		}
	}
	// Non-literal initializers belong to the declarations, not to a body.
	if (!same_initializers(a, b)) {
		return false;
	}
	// Signatures; the line, the frame and the code belong to the body.
	for (size_t i = 0; i < a.functions.size(); i++) {
		if (int32_t(i) == a.init_function) {
			continue;
		}
		const bytecode::Function &x = a.functions[i];
		const bytecode::Function &y = b.functions[i];
		if (x.name != y.name || x.return_type != y.return_type || x.return_class != y.return_class || x.is_pub != y.is_pub || x.is_override != y.is_override || x.params.size() != y.params.size()) {
			return false;
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
