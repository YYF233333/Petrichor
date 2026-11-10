#include "runtime/method_callable.h"

#include "runtime/text.h"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/templates/hashfuncs.hpp>

using namespace godot;

namespace petrichor {

namespace {

InstanceResolver resolver = nullptr;
ScriptResolver script_resolver = nullptr;
thread_local ValueCall current_value_call;
thread_local std::vector<std::shared_ptr<const ClassImage>> loading_files;
thread_local bool without_init = false;
thread_local bool last_initialized = true;

// Calls the function, as the callee of a call of a function value when one is pending.
Variant call_value(std::shared_ptr<const ClassImage> p_image, int p_function, InstanceContext p_context, const Variant **p_args, int p_argc, GDExtensionCallError &r_error, const uint8_t *p_captures, const uint8_t *p_bound_self = nullptr) {
	ValueCall &value = current_value_call;
	const bool callee = value.pending;
	value.pending = false;
	value.entered = value.entered || callee;
	bool failed = false;
	Variant result = vm::call(std::move(p_image), p_function, std::move(p_context), p_args, p_argc, r_error, &failed, p_captures, callee && value.task, p_bound_self);
	if (callee) {
		value.failed = failed;
	}
	return result;
}

// Whether the object exists, without the wrapper that ObjectDB::get_instance()
// finds or makes under a lock of the object's
// (thirdparty/godot-cpp/include/godot_cpp/core/object.hpp:110-116; core/object/object.cpp:2167-2169).
bool alive(ObjectID p_object) {
	return gdextension_interface::object_get_instance_from_id(uint64_t(p_object)) != nullptr;
}

// The object's script instance and the function's index in its image.
bool resolve(ObjectID p_owner, const StringName &p_method, ScriptedObject &r_instance, int &r_function) {
	const GDExtensionObjectPtr object = gdextension_interface::object_get_instance_from_id(uint64_t(p_owner));
	if (!object || !scripted_object(object, r_instance)) {
		return false;
	}
	r_function = r_instance.image->find_function(p_method);
	return r_function >= 0;
}

} // namespace

ValueCall &value_call() {
	return current_value_call;
}

void set_instance_resolver(InstanceResolver p_resolver) {
	resolver = p_resolver;
}

bool scripted_object(GDExtensionObjectPtr p_object, ScriptedObject &r_instance) {
	return resolver && resolver(p_object, r_instance);
}

bool attach_script(Object *p_object, const Variant &p_script, bool p_without_init, ScriptedObject &r_instance, bool &r_initialized) {
	// The engine makes the instance within set_script(), before anything else
	// can make one on this thread; instances it makes meanwhile finish first.
	without_init = p_without_init;
	last_initialized = true;
	p_object->set_script(p_script);
	without_init = false;
	r_initialized = last_initialized;
	return scripted_object(p_object->_owner, r_instance);
}

bool init_left_to_caller() {
	const bool left = without_init;
	without_init = false;
	return left;
}

void constructed(bool p_initialized) {
	last_initialized = p_initialized;
}

void set_script_resolver(ScriptResolver p_resolver) {
	script_resolver = p_resolver;
}

std::shared_ptr<const ClassImage> script_image(const String &p_path) {
	for (auto it = loading_files.rbegin(); it != loading_files.rend(); ++it) {
		if (std::shared_ptr<const ClassImage> found = (*it)->find_class(p_path)) {
			return found;
		}
	}
	return script_resolver ? script_resolver(p_path) : nullptr;
}

void begin_loading(const std::shared_ptr<const ClassImage> &p_image) {
	loading_files.push_back(p_image);
}

void end_loading() {
	loading_files.pop_back();
}

bool implements_interface(Object *p_object, const String &p_interface) {
	ScriptedObject instance;
	if (!scripted_object(p_object->_owner, instance)) {
		return false;
	}
	const std::string id = to_utf8(p_interface);
	for (const std::string &implemented : instance.image->module.interfaces) {
		if (implemented == id) {
			return true;
		}
	}
	return false;
}

MethodCallable::MethodCallable(Object *p_owner, const StringName &p_method) :
		owner(instance_id(p_owner)), method(p_method) {}

bool MethodCallable::compare_equal(const CallableCustom *p_a, const CallableCustom *p_b) {
	const MethodCallable *a = static_cast<const MethodCallable *>(p_a);
	const MethodCallable *b = static_cast<const MethodCallable *>(p_b);
	return a->owner == b->owner && a->method == b->method;
}

bool MethodCallable::compare_less(const CallableCustom *p_a, const CallableCustom *p_b) {
	const MethodCallable *a = static_cast<const MethodCallable *>(p_a);
	const MethodCallable *b = static_cast<const MethodCallable *>(p_b);
	if (a->owner != b->owner) {
		return uint64_t(a->owner) < uint64_t(b->owner);
	}
	return a->method < b->method;
}

uint32_t MethodCallable::hash() const {
	return hash_murmur3_one_64(uint64_t(owner), method.hash());
}

String MethodCallable::get_as_text() const {
	return String(method);
}

bool MethodCallable::resolve(ScriptedObject &r_instance, int &r_function) const {
	return petrichor::resolve(owner, method, r_instance, r_function);
}

bool MethodCallable::is_valid() const {
	return alive(owner);
}

int MethodCallable::get_argument_count(bool &r_is_valid) const {
	ScriptedObject instance;
	int function = -1;
	r_is_valid = resolve(instance, function);
	return r_is_valid ? int(instance.image->module.functions[function].fixed_params()) : 0;
}

void MethodCallable::call(const Variant **p_arguments, int p_argcount, Variant &r_return_value, GDExtensionCallError &r_call_error) const {
	ScriptedObject instance;
	int function = -1;
	if (!resolve(instance, function)) {
		r_call_error.error = alive(owner) ? GDEXTENSION_CALL_ERROR_INVALID_METHOD : GDEXTENSION_CALL_ERROR_INSTANCE_IS_NULL;
		return;
	}
	r_return_value = call_value(std::move(instance.image), function, std::move(instance.context), p_arguments, p_argcount, r_call_error, nullptr);
}

void call_static(const StringName &p_class, const StringName &p_method, const Variant **p_args, uint32_t p_count, Variant &r_result, GDExtensionCallError &r_error) {
	const Variant names[2] = { p_class, p_method };
	std::vector<const Variant *> args = { &names[0], &names[1] };
	args.insert(args.end(), p_args, p_args + p_count);
	Variant(ClassDBSingleton::get_singleton()).callp("class_call_static", args.data(), int(args.size()), r_result, r_error);
}

Variant held_script(const ClassImage &p_image) {
	GDExtensionObjectPtr script = gdextension_interface::object_get_instance_from_id(uint64_t(p_image.script));
	Variant held;
	if (script) {
		static const GDExtensionVariantFromTypeConstructorFunc from_object = gdextension_interface::get_variant_from_type_constructor(GDEXTENSION_VARIANT_TYPE_OBJECT);
		from_object(held._native_ptr(), &script);
	}
	return held;
}

StaticCallable::StaticCallable(const String &p_owner, const StringName &p_method, bool p_of_engine) :
		owner(p_owner), method(p_method), of_engine(p_of_engine) {
	const std::shared_ptr<const ClassImage> image = of_engine ? nullptr : script_image(owner);
	if (image) {
		script = held_script(*image);
	}
}

ObjectID StaticCallable::get_object() const {
	return script;
}

bool StaticCallable::compare_equal(const CallableCustom *p_a, const CallableCustom *p_b) {
	const StaticCallable *a = static_cast<const StaticCallable *>(p_a);
	const StaticCallable *b = static_cast<const StaticCallable *>(p_b);
	return a->of_engine == b->of_engine && a->owner == b->owner && a->method == b->method;
}

bool StaticCallable::compare_less(const CallableCustom *p_a, const CallableCustom *p_b) {
	const StaticCallable *a = static_cast<const StaticCallable *>(p_a);
	const StaticCallable *b = static_cast<const StaticCallable *>(p_b);
	if (a->of_engine != b->of_engine) {
		return a->of_engine < b->of_engine;
	}
	if (a->owner != b->owner) {
		return a->owner < b->owner;
	}
	return a->method < b->method;
}

uint32_t StaticCallable::hash() const {
	return hash_murmur3_one_32(method.hash(), owner.hash());
}

String StaticCallable::get_as_text() const {
	return owner + String("::") + String(method);
}

int StaticCallable::get_argument_count(bool &r_is_valid) const {
	std::shared_ptr<const ClassImage> image = of_engine ? nullptr : script_image(owner);
	const int function = image ? image->find_function(method) : -1;
	r_is_valid = function >= 0;
	return r_is_valid ? int(image->module.functions[function].fixed_params()) : 0;
}

void StaticCallable::call(const Variant **p_arguments, int p_argcount, Variant &r_return_value, GDExtensionCallError &r_call_error) const {
	if (of_engine) {
		call_static(owner, method, p_arguments, uint32_t(p_argcount), r_return_value, r_call_error);
		return;
	}
	std::shared_ptr<const ClassImage> image = script_image(owner);
	const int function = image ? image->find_function(method) : -1;
	if (function < 0 || !image->module.functions[function].is_static) {
		r_call_error.error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
		return;
	}
	r_return_value = call_value(std::move(image), function, InstanceContext(), p_arguments, p_argcount, r_call_error, nullptr);
}

StructMethodCallable::StructMethodCallable(const std::shared_ptr<const ClassImage> &p_image, int p_function, const uint8_t *p_self) :
		image(p_image), function(p_function), self(p_image->types[p_image->module.functions[p_function].params[0].type]), script(held_script(*p_image)) {
	copy_value(image->types[image->module.functions[function].params[0].type], self.data, p_self);
}

ObjectID StructMethodCallable::get_object() const {
	return script;
}

uint32_t StructMethodCallable::hash() const {
	return hash_murmur3_one_64(uint64_t(this));
}

String StructMethodCallable::get_as_text() const {
	return String(image->function_names[function]);
}

int StructMethodCallable::get_argument_count(bool &r_is_valid) const {
	r_is_valid = true;
	return int(image->module.functions[function].fixed_params()) - 1;
}

void StructMethodCallable::call(const Variant **p_arguments, int p_argcount, Variant &r_return_value, GDExtensionCallError &r_call_error) const {
	r_return_value = call_value(image, function, InstanceContext(), p_arguments, p_argcount, r_call_error, nullptr, self.data);
}

LambdaCallable::LambdaCallable(const std::shared_ptr<const ClassImage> &p_image, int p_function, const InstanceContext &p_context, const uint8_t *p_frame, const uint32_t *p_captures) :
		image(p_image), function(p_function), context(p_context) {
	if (context.owner) {
		owner = ObjectID(instance_id(context.owner));
		kept = Variant(context.owner);
	} else {
		kept = held_script(*image);
		owner = kept.operator ObjectID();
	}
	// As a call would complete it, so that a direct call can use it as it is.
	if (!context.self_image) {
		context.self_image = image;
	}
	const bytecode::Function &fn = image->module.functions[function];
	const size_t first = fn.params.size() - fn.captures;
	size_t size = 0;
	for (size_t i = first; i < fn.params.size(); i++) {
		size += image->types[fn.params[i].type].size;
	}
	large_captures.reset(size > sizeof(inline_captures) ? new uint64_t[size / 8 + 1] : nullptr);
	captures = large_captures ? reinterpret_cast<uint8_t *>(large_captures.get()) : inline_captures;
	uint8_t *capture = captures;
	for (size_t i = 0; i < fn.captures; i++) {
		const TypeImage &type = image->types[fn.params[first + i].type];
		construct_value(type, capture);
		copy_value(type, capture, p_frame + p_captures[2 * i + 1]);
		capture += type.size;
	}
}

LambdaCallable::~LambdaCallable() {
	const bytecode::Function &fn = image->module.functions[function];
	uint8_t *capture = captures;
	for (size_t i = fn.params.size() - fn.captures; i < fn.params.size(); i++) {
		const TypeImage &type = image->types[fn.params[i].type];
		destroy_value(type, capture);
		capture += type.size;
	}
}

uint32_t LambdaCallable::hash() const {
	return hash_murmur3_one_64(uint64_t(this));
}

String LambdaCallable::get_as_text() const {
	const StringName &name = image->function_names[function];
	return String(name).begins_with("<") ? String("(anonymous lambda)") : String(name) + "(lambda)";
}

bool LambdaCallable::is_valid() const {
	return !context.storage || (!context.storage->freed && alive(owner));
}

int LambdaCallable::get_argument_count(bool &r_is_valid) const {
	r_is_valid = true;
	return int(image->module.functions[function].fixed_params());
}

void LambdaCallable::call(const Variant **p_arguments, int p_argcount, Variant &r_return_value, GDExtensionCallError &r_call_error) const {
	if (!is_valid()) {
		r_call_error.error = GDEXTENSION_CALL_ERROR_INSTANCE_IS_NULL;
		return;
	}
	r_return_value = call_value(image, function, context, p_arguments, p_argcount, r_call_error, captures);
}

} // namespace petrichor
