#include "probe/probe_methods.h"

#include "runtime/task.h"

#include <godot_cpp/core/object.hpp>

using namespace godot;

namespace petrichor::probe_methods {

static StringName echo_later() {
	return "echo_later";
}

static Dictionary property(Variant::Type p_type, const StringName &p_name, uint32_t p_usage) {
	Dictionary dict;
	dict["name"] = p_name;
	dict["class_name"] = StringName();
	dict["type"] = p_type;
	dict["hint"] = PROPERTY_HINT_NONE;
	dict["hint_string"] = String();
	dict["usage"] = p_usage;
	return dict;
}

bool has(const StringName &p_method) {
	return p_method == echo_later();
}

Dictionary info(const StringName &p_method) {
	if (!has(p_method)) {
		return Dictionary();
	}
	Array args;
	args.push_back(property(Variant::NIL, "value", PROPERTY_USAGE_NIL_IS_VARIANT));
	args.push_back(property(Variant::INT, "frames", PROPERTY_USAGE_DEFAULT));
	Dictionary dict;
	dict["name"] = echo_later();
	dict["args"] = args;
	dict["default_args"] = Array();
	dict["flags"] = METHOD_FLAGS_DEFAULT;
	dict["id"] = 0;
	// Async methods report an untyped result to the engine (spec §9.8).
	dict["return"] = property(Variant::NIL, StringName(), PROPERTY_USAGE_NIL_IS_VARIANT);
	return dict;
}

void append_to(TypedArray<Dictionary> &r_list) {
	r_list.push_back(info(echo_later()));
}

bool call(const StringName &p_method, const Variant **p_args, int64_t p_argc, Variant &r_result, GDExtensionCallError &r_error) {
	if (!has(p_method)) {
		return false;
	}
	r_error.error = GDEXTENSION_CALL_OK;
	if (p_argc != 2) {
		r_error.error = p_argc > 2 ? GDEXTENSION_CALL_ERROR_TOO_MANY_ARGUMENTS : GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS;
		r_error.expected = 2;
		return true;
	}
	if (p_args[1]->get_type() != Variant::INT) {
		r_error.error = GDEXTENSION_CALL_ERROR_INVALID_ARGUMENT;
		r_error.argument = 1;
		r_error.expected = GDEXTENSION_VARIANT_TYPE_INT;
		return true;
	}
	r_result = task_queue::deliver_later(*p_args[0], int64_t(*p_args[1]));
	return true;
}

} // namespace petrichor::probe_methods
