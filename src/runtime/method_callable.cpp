#include "runtime/method_callable.h"

#include <godot_cpp/core/object.hpp>
#include <godot_cpp/templates/hashfuncs.hpp>

using namespace godot;

namespace petrichor {

namespace {

InstanceResolver resolver = nullptr;

// The object's script instance and the function's index in its image.
bool resolve(ObjectID p_owner, const StringName &p_method, ScriptedObject &r_instance, int &r_function) {
	Object *object = ObjectDB::get_instance(p_owner);
	if (!object || !resolver || !resolver(object, r_instance)) {
		return false;
	}
	r_function = r_instance.image->find_function(p_method);
	return r_function >= 0;
}

} // namespace

void set_instance_resolver(InstanceResolver p_resolver) {
	resolver = p_resolver;
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
	r_return_value = vm::call(instance.image, function, instance.context, p_arguments, p_argcount, r_call_error);
}

} // namespace petrichor
