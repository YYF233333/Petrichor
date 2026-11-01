#include "runtime/method_callable.h"

#include "runtime/text.h"

#include <godot_cpp/core/object.hpp>
#include <godot_cpp/templates/hashfuncs.hpp>

using namespace godot;

namespace petrichor {

namespace {

InstanceResolver resolver = nullptr;
ScriptResolver script_resolver = nullptr;
thread_local ValueCall current_value_call;

// Calls the function, as the callee of a call of a function value when one is pending.
Variant call_value(const std::shared_ptr<const ClassImage> &p_image, int p_function, const InstanceContext &p_context, const Variant **p_args, int p_argc, GDExtensionCallError &r_error, const uint8_t *const *p_captures) {
	ValueCall &value = current_value_call;
	const bool callee = value.pending;
	value.pending = false;
	value.entered = value.entered || callee;
	bool failed = false;
	Variant result = vm::call(p_image, p_function, p_context, p_args, p_argc, r_error, &failed, p_captures);
	if (callee) {
		value.failed = failed;
	}
	return result;
}

// The object's script instance and the function's index in its image.
bool resolve(ObjectID p_owner, const StringName &p_method, ScriptedObject &r_instance, int &r_function) {
	Object *object = ObjectDB::get_instance(p_owner);
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

bool scripted_object(Object *p_object, ScriptedObject &r_instance) {
	return resolver && resolver(p_object, r_instance);
}

void set_script_resolver(ScriptResolver p_resolver) {
	script_resolver = p_resolver;
}

std::shared_ptr<const ClassImage> script_image(const String &p_path) {
	return script_resolver ? script_resolver(p_path) : nullptr;
}

bool implements_interface(Object *p_object, const String &p_interface) {
	ScriptedObject instance;
	if (!scripted_object(p_object, instance)) {
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
		owner(p_owner->get_instance_id()), method(p_method) {}

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

int MethodCallable::get_argument_count(bool &r_is_valid) const {
	ScriptedObject instance;
	int function = -1;
	r_is_valid = resolve(owner, method, instance, function);
	return r_is_valid ? int(instance.image->module.functions[function].params.size()) : 0;
}

void MethodCallable::call(const Variant **p_arguments, int p_argcount, Variant &r_return_value, GDExtensionCallError &r_call_error) const {
	ScriptedObject instance;
	int function = -1;
	if (!resolve(owner, method, instance, function)) {
		r_call_error.error = ObjectDB::get_instance(owner) ? GDEXTENSION_CALL_ERROR_INVALID_METHOD : GDEXTENSION_CALL_ERROR_INSTANCE_IS_NULL;
		return;
	}
	r_return_value = call_value(instance.image, function, instance.context, p_arguments, p_argcount, r_call_error, nullptr);
}

LambdaCallable::LambdaCallable(const std::shared_ptr<const ClassImage> &p_image, int p_function, const InstanceContext &p_context, const uint8_t *p_frame, const uint32_t *p_captures) :
		image(p_image), function(p_function), context(p_context) {
	if (context.owner) {
		owner = context.owner->get_instance_id();
		kept = Variant(context.owner);
	}
	const bytecode::Function &fn = image->module.functions[function];
	const size_t first = fn.params.size() - fn.captures;
	for (size_t i = 0; i < fn.captures; i++) {
		const TypeImage &type = image->types[fn.params[first + i].type];
		captures.push_back(std::make_unique<ValueBuffer>(type));
		copy_value(type, captures.back()->data, p_frame + p_captures[2 * i + 1]);
		capture_data.push_back(captures.back()->data);
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
	return !context.storage || (!context.storage->freed && ObjectDB::get_instance(owner));
}

int LambdaCallable::get_argument_count(bool &r_is_valid) const {
	r_is_valid = true;
	const bytecode::Function &fn = image->module.functions[function];
	return int(fn.params.size() - fn.captures);
}

void LambdaCallable::call(const Variant **p_arguments, int p_argcount, Variant &r_return_value, GDExtensionCallError &r_call_error) const {
	if (!is_valid()) {
		r_call_error.error = GDEXTENSION_CALL_ERROR_INSTANCE_IS_NULL;
		return;
	}
	r_return_value = call_value(image, function, context, p_arguments, p_argcount, r_call_error, capture_data.data());
}

} // namespace petrichor
