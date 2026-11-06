#include "runtime/class_image.h"

#include "bytecode/opcodes.h"
#include "runtime/method_callable.h"
#include "runtime/text.h"

#include <godot_cpp/classes/script.hpp>

#include <algorithm>
#include <cstring>
#include <functional>
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

namespace {

// The file of a class's path: everything before "::", which an inner class's
// path continues with its names (spec §4.1).
std::string script_file(const std::string &p_path) {
	return p_path.substr(0, p_path.find("::"));
}

} // namespace

std::shared_ptr<ClassImage> ClassImage::load_class(bytecode::Module &&p_module, const String &p_path, String &r_error, const std::shared_ptr<const ClassImage> &p_base) {
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
		if (!p_base->module.functions[i].is_lambda && (name.empty() || name[0] != '@') && (i >= image->module.functions.size() || image->module.functions[i].name != name)) {
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
		// A lambda is no method of the class (spec §3.11).
		if (function.is_lambda) {
			image->function_origins.emplace_back();
			image->function_signatures.push_back(signature_id(module.types, function.params, function.defaults.size(), function.result, function.is_async));
			image->frames.push_back(SlotLayout::of(function.frame_size, function.slots, types));
			continue;
		}
		image->function_lookup.insert(name, int(i));
		const int parent = p_base ? p_base->find_function(name) : -1;
		// The base's bodies may name the method to the engine, as in
		// call_deferred("m"); compiling this class does not check them.
		if (parent >= 0 && p_base->module.functions[parent].is_exposed) {
			image->module.functions[i].is_exposed = true;
		}
		if (!function.name.empty() && function.name[0] != '@') {
			const std::string expected = parent < 0 ? std::string() : signature_key(p_base->module, p_base->module.functions[parent]);
			// Each class's _init() has its own visibility (spec §6.1).
			if (parent >= 0 && function.name != "_init" && (function.is_static != p_base->module.functions[parent].is_static || function.is_pub != p_base->module.functions[parent].is_pub)) {
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
		if (function.name == "_init") {
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
	for (size_t i = 0; i < module.statics.size(); i++) {
		image->static_lookup.insert(to_godot(module.statics[i].name), int(i));
	}
	image->statics = std::make_shared<Statics>(SlotLayout::of(std::max<uint32_t>(module.statics_size, 8), module.statics, types));

	for (const bytecode::EngineCall &call : module.engine_calls) {
		image->engine_methods.push_back(to_godot(call.method));
		image->engine_static_classes.push_back(call.static_class.empty() ? StringName() : StringName(to_godot(call.static_class)));
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
		const bytecode::ContainerMethod *container = bytecode::find_container_method(call.base, call.method);
		image->key_roles.push_back(container ? container->key_role : KeyRole::None);
		image->replaced.push_back(container ? container->replaced : Replaced::None);
		image->needs_element.push_back(container && container->needs_element);
		std::vector<std::pair<uint32_t, ElementRole>> roles;
		if (call.checks_elements && container) {
			for (uint32_t i = 0; i < 2; i++) {
				const ElementRole role = container->args[i];
				if (role != ElementRole::None && (role != ElementRole::Entries || container->fills) && !(i == 1 && call.method == "get")) {
					roles.emplace_back(i, role);
				}
			}
		}
		image->element_args.push_back(std::move(roles));
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
	// Each constant once (spec §4.5), its containers read-only at every level,
	// as GDScript makes them (modules/gdscript/gdscript_analyzer.cpp:5318-5367).
	// A scalar the engine computed is written into the constant, which the
	// instructions for scalars read; a resource loads only when asked for.
	for (size_t i = 0; i < module.constants.size(); i++) {
		bytecode::Constant &constant = image->module.constants[i];
		const ValueType storage = types[constant.type].storage;
		image->values.emplace_back();
		if (!constant.load && !constant_value(*image, constant, image->values.back(), r_error)) {
			return nullptr;
		}
		const Variant &value = image->values.back();
		const bool computed = constant.constructor >= 0 || constant.op >= 0 || constant.utility >= 0 || !constant.field.empty();
		if (computed && (storage == ValueType::Bool || storage == ValueType::Int || storage == ValueType::Float || storage == ValueType::String)) {
			constant.b = value.booleanize();
			constant.i = storage == ValueType::Bool ? int64_t(constant.b) : int64_t(value);
			constant.f = double(value);
			constant.s = storage == ValueType::String ? to_utf8(String(value)) : std::string();
		}
		image->strings.push_back(storage == ValueType::String ? to_godot(constant.s) : String());
		freeze(value);
	}
	return image;
}

std::shared_ptr<const ClassImage> ClassImage::load(bytecode::Module &&p_module, const String &p_path, String &r_error, const std::shared_ptr<const ClassImage> &p_base, const BaseLoader &p_bases) {
	const std::shared_ptr<ClassImage> image = load_class(std::move(p_module), p_path, r_error, p_base);
	if (!image || image->module.inner_classes.empty()) {
		return image;
	}
	// The inner classes (spec §4.1), each after its base when that is a class
	// of this file; the base of any other comes from `p_bases`, or from the
	// current image of its script.
	const std::string file = to_utf8(p_path);
	std::unordered_map<std::string, std::shared_ptr<ClassImage>> loaded{ { file, image } };
	std::vector<std::string> loading;
	std::function<std::shared_ptr<ClassImage>(const std::string &)> at = [&](const std::string &p_inner) -> std::shared_ptr<ClassImage> {
		const auto found = loaded.find(p_inner);
		if (found != loaded.end()) {
			return found->second;
		}
		if (std::find(loading.begin(), loading.end(), p_inner) != loading.end()) {
			r_error = "Inheritance cycle in compiled scripts.";
			return nullptr;
		}
		const bytecode::Module *module = &image->module;
		for (size_t at_name = p_inner.find("::"); module && at_name != std::string::npos;) {
			const size_t next = p_inner.find("::", at_name + 2);
			const std::string name = p_inner.substr(at_name + 2, next == std::string::npos ? next : next - at_name - 2);
			const auto inner = std::find_if(module->inner_classes.begin(), module->inner_classes.end(), [&](const bytecode::Module &p_module) { return p_module.name == name; });
			module = inner == module->inner_classes.end() ? nullptr : &*inner;
			at_name = next;
		}
		if (!module || script_file(p_inner) != file) {
			r_error = "Base script " + to_godot(p_inner) + " is not in the compiled script. Rebuild the project.";
			return nullptr;
		}
		loading.push_back(p_inner);
		std::shared_ptr<const ClassImage> base;
		if (!module->base_script.empty()) {
			const String base_path = to_godot(module->base_script);
			base = script_file(module->base_script) == file ? at(module->base_script) : (p_bases ? p_bases(base_path) : script_image(base_path));
			if (!base) {
				if (r_error.is_empty()) {
					r_error = "Base script " + base_path + " could not be loaded. Rebuild the project.";
				}
				return nullptr;
			}
		}
		loading.pop_back();
		bytecode::Module copy = *module;
		copy.inner_classes.clear(); // Each inner class's own image has them.
		const std::shared_ptr<ClassImage> inner = load_class(std::move(copy), to_godot(p_inner), r_error, base);
		if (inner) {
			inner->source = image->source;
			loaded[p_inner] = inner;
		}
		return inner;
	};
	const std::function<bool(ClassImage &, const bytecode::Module &, const std::string &)> attach = [&](ClassImage &r_outer, const bytecode::Module &p_module, const std::string &p_outer) {
		for (const bytecode::Module &module : p_module.inner_classes) {
			const std::string path = p_outer + "::" + module.name;
			const std::shared_ptr<ClassImage> inner = at(path);
			if (!inner || !attach(*inner, module, path)) {
				return false;
			}
			r_outer.inner_classes.push_back(inner);
		}
		return true;
	};
	return attach(*image, image->module, file) ? image : nullptr;
}

std::shared_ptr<const ClassImage> ClassImage::find_class(const String &p_path) const {
	if (p_path == path) {
		return shared_from_this();
	}
	for (const std::shared_ptr<const ClassImage> &inner : inner_classes) {
		if (p_path == inner->path || p_path.begins_with(inner->path + String("::"))) {
			return inner->find_class(p_path);
		}
	}
	return nullptr;
}
// Built with the constructor that CONSTRUCT calls, from the arguments in their
// slots' layout.
static bool construct_constant(const ClassImage &p_image, const bytecode::Constant &p_constant, Variant &r_value, String &r_error) {
	const std::vector<TypeImage> &p_types = p_image.types;
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
		ok = constant_value(p_image, p_constant.args[i], value, r_error) && from_variant(p_types[slots[i].type].storage, value, base + slots[i].offset);
		pointers.push_back(base + slots[i].offset);
	}
	if (ok) {
		// The engine constructs into memory that holds no value.
		alignas(16) uint8_t result[MAX_VALUE_SIZE];
		constructor(result, pointers.data());
		r_value = to_variant(type, result);
		destroy_value(type, result);
	}
	layout.destroy(base);
	return ok;
}

// A math utility function of the module's table (spec §4.5), whose parameters
// are int, float or Variant.
static bool call_utility_constant(const ClassImage &p_image, const bytecode::Constant &p_constant, Variant &r_value, String &r_error) {
	const bytecode::UtilityCall &call = p_image.module.utility_calls[p_constant.utility];
	const StringName name = to_godot(call.function);
	const GDExtensionPtrUtilityFunction function = gdextension_interface::variant_get_ptr_utility_function(name._native_ptr(), call.hash);
	if (!function) {
		r_error = "the engine has no utility function " + name + " with this signature";
		return false;
	}
	const size_t count = p_constant.args.size();
	std::vector<Variant> variants(count);
	std::vector<uint64_t> words(count);
	std::vector<const void *> pointers(count);
	for (size_t i = 0; i < count; i++) {
		if (!constant_value(p_image, p_constant.args[i], variants[i], r_error)) {
			return false;
		}
		const ValueType param = i < call.params.size() ? call.params[i] : ValueType::Variant;
		if (param == ValueType::Variant) {
			pointers[i] = &variants[i];
		} else if (param == ValueType::Int || param == ValueType::Float) {
			from_variant(param, variants[i], reinterpret_cast<uint8_t *>(&words[i]));
			pointers[i] = &words[i];
		} else {
			r_error = "the utility function " + name + " takes a parameter constants do not pass";
			return false;
		}
	}
	if (call.return_type == ValueType::Variant) {
		function(&r_value, pointers.data(), int(count));
		return true;
	}
	uint64_t result = 0;
	function(&result, pointers.data(), int(count));
	switch (call.return_type) {
		case ValueType::Bool:
			r_value = (result & 0xFF) != 0;
			return true;
		case ValueType::Int:
			r_value = int64_t(result);
			return true;
		case ValueType::Float: {
			double number;
			std::memcpy(&number, &result, sizeof(number));
			r_value = number;
			return true;
		}
		default:
			r_error = "the utility function " + name + " returns a type constants do not hold";
			return false;
	}
}

bool pub_constant(const ClassImage &p_image, const StringName &p_name, Variant &r_value) {
	for (const bytecode::NamedConstant &constant : p_image.module.named_constants) {
		if (!constant.is_pub || to_godot(constant.name) != String(p_name)) {
			continue;
		}
		if (!p_image.module.constants[constant.constant].load) {
			r_value = p_image.values[constant.constant];
			return true;
		}
		String error;
		if (!constant_value(p_image, p_image.module.constants[constant.constant], r_value, error)) {
			ERR_PRINT(vformat("%s: the constant %s: %s", p_image.path, p_name, error));
		}
		return true;
	}
	return false;
}

bool constant_value(const ClassImage &p_image, const bytecode::Constant &p_constant, Variant &r_value, String &r_error) {
	const std::vector<TypeImage> &p_types = p_image.types;
	const ValueType type = p_types[p_constant.type].storage;
	if (p_constant.load) {
		r_value = load_resource(to_godot(p_constant.s));
		return true;
	}
	if (p_constant.op >= 0) {
		Variant operands[2];
		for (size_t i = 0; i < p_constant.args.size(); i++) {
			if (!constant_value(p_image, p_constant.args[i], operands[i], r_error)) {
				return false;
			}
		}
		bool valid = false;
		Variant::evaluate(Variant::Operator(p_constant.op), operands[0], operands[1], r_value, valid);
		if (!valid) {
			r_error = vformat("the engine has no operator %d for %s and %s", p_constant.op, Variant::get_type_name(operands[0].get_type()), Variant::get_type_name(operands[1].get_type()));
			return false;
		}
		return true;
	}
	if (p_constant.utility >= 0) {
		return call_utility_constant(p_image, p_constant, r_value, r_error);
	}
	if (!p_constant.field.empty()) {
		Variant base;
		if (!constant_value(p_image, p_constant.args[0], base, r_error)) {
			return false;
		}
		bool valid = false;
		r_value = base.get_named(to_godot(p_constant.field), valid);
		if (!valid) {
			r_error = vformat("%s has no member %s", Variant::get_type_name(base.get_type()), to_godot(p_constant.field));
		}
		return valid;
	}
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
		return construct_constant(p_image, p_constant, r_value, r_error);
	}
	if (type == ValueType::Array) {
		const ValueCheck &check = p_types[p_constant.type].check;
		Array array = typed_array(check.element, check.class_name, check.script);
		for (const bytecode::Constant &element : p_constant.args) {
			Variant value;
			if (!constant_value(p_image, element, value, r_error)) {
				return false;
			}
			array.push_back(value);
		}
		r_value = array;
		return true;
	}
	if (type == ValueType::Dictionary) {
		Dictionary dictionary = typed_dictionary(p_types[p_constant.type].check);
		for (size_t i = 0; i + 1 < p_constant.args.size(); i += 2) {
			Variant key;
			Variant value;
			if (!constant_value(p_image, p_constant.args[i], key, r_error) || !constant_value(p_image, p_constant.args[i + 1], value, r_error)) {
				return false;
			}
			dictionary[key] = value;
		}
		r_value = dictionary;
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

int ClassImage::find_static(const StringName &p_name) const {
	const int *index = static_lookup.getptr(p_name);
	return index ? *index : -1;
}

void ClassImage::adopt_statics(const ClassImage &p_previous) const {
	statics = p_previous.statics;
	for (const std::shared_ptr<const ClassImage> &inner : inner_classes) {
		if (const std::shared_ptr<const ClassImage> previous = p_previous.find_class(inner->path)) {
			inner->adopt_statics(*previous);
		}
	}
}

Statics::Statics(const SlotLayout &p_layout) :
		layout(p_layout), data(new uint64_t[p_layout.size / 8 + 1]()) {
	layout.construct(at(0));
}

Statics::~Statics() {
	layout.destroy(at(0));
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
				p_a.constructor == p_b.constructor && p_a.op == p_b.op && p_a.field == p_b.field && p_a.load == p_b.load && (p_a.utility < 0) == (p_b.utility < 0) &&
				(p_a.utility < 0 || (a.utility_calls[p_a.utility].function == b.utility_calls[p_b.utility].function && a.utility_calls[p_a.utility].params == b.utility_calls[p_b.utility].params)) &&
				std::equal(p_a.args.begin(), p_a.args.end(), p_b.args.begin(), p_b.args.end(), [&](const auto &x, const auto &y) { return same_constant(x, y); });
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
			same_prefix(p_a.engine_calls, p_b.engine_calls, x.engine_calls, [](const auto &a, const auto &b) { return a.method == b.method && a.result_type == b.result_type && a.static_class == b.static_class; }) &&
			same_prefix(p_a.properties, p_b.properties, x.properties, [](const auto &a, const auto &b) { return a.name == b.name && a.type == b.type; }) &&
			same_prefix(p_a.utility_calls, p_b.utility_calls, x.utility_calls, [](const auto &a, const auto &b) { return a.function == b.function && a.hash == b.hash && a.is_vararg == b.is_vararg && a.params == b.params && a.return_type == b.return_type && a.result_type == b.result_type; }) &&
			same_prefix(p_a.operators, p_b.operators, x.operators, [](const auto &a, const auto &b) { return a.op == b.op && a.left == b.left && a.right == b.right && a.result == b.result; }) &&
			same_prefix(p_a.fields, p_b.fields, x.fields, [](const auto &a, const auto &b) { return a.base == b.base && a.name == b.name && a.type == b.type; }) &&
			same_prefix(p_a.constructors, p_b.constructors, x.constructors, [](const auto &a, const auto &b) { return a.type == b.type && a.index == b.index && a.params == b.params; }) &&
			same_prefix(p_a.builtin_methods, p_b.builtin_methods, x.builtin_methods, [](const auto &a, const auto &b) { return a.base == b.base && a.method == b.method && a.hash == b.hash && a.is_vararg == b.is_vararg && a.is_const == b.is_const && a.is_static == b.is_static && a.params == b.params && a.return_type == b.return_type && a.result_type == b.result_type; }) &&
			same_prefix(p_a.classes, p_b.classes, x.classes, [](const auto &a, const auto &b) { return a == b; }) &&
			same_prefix(p_a.method_calls, p_b.method_calls, x.method_calls, [&](const auto &a, const auto &b) { return a.method == b.method && a.static_class == b.static_class && bytecode::signature_key(p_a.types, a.params, a.defaults, a.result, a.is_async) == bytecode::signature_key(p_b.types, b.params, b.defaults, b.result, b.is_async); });
}

} // namespace

bool ClassImage::same_declarations(const ClassImage &p_other) const {
	const bytecode::Module &a = module;
	const bytecode::Module &b = p_other.module;
	// Those the class declares, which come first; lambdas belong to the bodies
	// they are in (spec §3.11, §12.4).
	const auto declared = [](const bytecode::Module &p_module) {
		size_t count = 0;
		while (count < p_module.functions.size() && !p_module.functions[count].is_lambda && int32_t(count) != p_module.init_function && int32_t(count) != p_module.ready_function && int32_t(count) != p_module.static_init_function) {
			count++;
		}
		return count;
	};
	// A base of the same file is compared with the file's classes.
	if (a.base_script != b.base_script || bool(base) != bool(p_other.base) || (base && script_file(to_utf8(base->path)) != script_file(to_utf8(path)) && !base->same_declarations(*p_other.base))) {
		return false;
	}
	if (a.declarations_only || b.declarations_only) {
		return false;
	}
	if (a.global_name != b.global_name || a.base_class != b.base_class || a.is_abstract != b.is_abstract || a.instance_size != b.instance_size || a.members.size() != b.members.size() || declared(a) != declared(b)) {
		return false;
	}
	const ModulePair pair{ a, b };
	for (size_t i = 0; i < a.members.size(); i++) {
		const bytecode::Member &x = a.members[i];
		const bytecode::Member &y = b.members[i];
		if (!pair.same_slot(x, y) || x.offset != y.offset || x.is_onready != y.is_onready || x.setter != y.setter || x.getter != y.getter ||
				x.is_pub != y.is_pub || x.is_export != y.is_export || x.hint != y.hint || x.hint_string != y.hint_string || x.usage != y.usage || x.has_initial != y.has_initial || !pair.same_constant(x.initial, y.initial)) {
			return false;
		}
	}
	// Their initializers belong to a body, which does not run again (spec §4.8).
	if (a.statics.size() != b.statics.size() || a.statics_size != b.statics_size || a.static_unload != b.static_unload) {
		return false;
	}
	for (size_t i = 0; i < a.statics.size(); i++) {
		const bytecode::Member &x = a.statics[i];
		const bytecode::Member &y = b.statics[i];
		if (x.name != y.name || !pair.same_slot(x, y) || x.offset != y.offset || x.is_pub != y.is_pub || x.setter != y.setter || x.getter != y.getter || x.has_initial != y.has_initial) {
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
	if (a.interfaces != b.interfaces || a.named_constants.size() != b.named_constants.size()) {
		return false;
	}
	// Other classes may have been compiled with a constant's value (spec §4.5).
	for (size_t i = 0; i < a.named_constants.size(); i++) {
		const bytecode::NamedConstant &x = a.named_constants[i];
		const bytecode::NamedConstant &y = b.named_constants[i];
		if (x.name != y.name || x.is_pub != y.is_pub || !pair.same_constant(a.constants[x.constant], b.constants[y.constant])) {
			return false;
		}
	}
	// Initializers belong to the declarations, not to a body.
	if (!same_initializers(a, b)) {
		return false;
	}
	// Signatures; the line, the frame and the code belong to the body.
	for (size_t i = 0; i < declared(a); i++) {
		const bytecode::Function &x = a.functions[i];
		const bytecode::Function &y = b.functions[i];
		if (x.is_inherited != y.is_inherited || x.base_signature != y.base_signature || x.name != y.name || !pair.same_slot(x.result, y.result) || x.is_pub != y.is_pub || x.is_override != y.is_override || x.is_exposed != y.is_exposed ||
				x.is_async != y.is_async || x.is_static != y.is_static || !(x.rpc == y.rpc) || x.is_abstract != y.is_abstract || x.params.size() != y.params.size() || x.defaults.size() != y.defaults.size()) {
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
	if (inner_classes.size() != p_other.inner_classes.size()) {
		return false;
	}
	for (size_t i = 0; i < inner_classes.size(); i++) {
		const bytecode::Module &x = inner_classes[i]->module;
		const bytecode::Module &y = p_other.inner_classes[i]->module;
		if (x.name != y.name || x.is_pub != y.is_pub || !inner_classes[i]->same_declarations(*p_other.inner_classes[i])) {
			return false;
		}
	}
	return true;
}

} // namespace petrichor
