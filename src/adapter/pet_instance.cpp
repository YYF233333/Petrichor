#include "adapter/pet_instance.h"

#include "adapter/pet_language.h"

#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace petrichor {

/* Reflection structures */

static void fill_property(GDExtensionPropertyInfo &r_info, const Dictionary &p_dict) {
	r_info.type = GDExtensionVariantType(int(p_dict.get("type", 0)));
	r_info.name = memnew(StringName(p_dict.get("name", String())));
	r_info.class_name = memnew(StringName(p_dict.get("class_name", String())));
	r_info.hint = uint32_t(int(p_dict.get("hint", PROPERTY_HINT_NONE)));
	r_info.hint_string = memnew(String(p_dict.get("hint_string", String())));
	r_info.usage = uint32_t(int(p_dict.get("usage", PROPERTY_USAGE_DEFAULT)));
}

static void clear_property(const GDExtensionPropertyInfo &p_info) {
	memdelete(reinterpret_cast<StringName *>(p_info.name));
	memdelete(reinterpret_cast<StringName *>(p_info.class_name));
	memdelete(reinterpret_cast<String *>(p_info.hint_string));
}

GDExtensionPropertyInfo *make_property_list(const TypedArray<Dictionary> &p_list) {
	if (p_list.is_empty()) {
		return nullptr;
	}
	GDExtensionPropertyInfo *list = memnew_arr(GDExtensionPropertyInfo, p_list.size());
	for (int64_t i = 0; i < p_list.size(); i++) {
		fill_property(list[i], p_list[i]);
	}
	return list;
}

void free_property_list(const GDExtensionPropertyInfo *p_list, uint32_t p_count) {
	if (!p_list) {
		return;
	}
	for (uint32_t i = 0; i < p_count; i++) {
		clear_property(p_list[i]);
	}
	memdelete_arr(const_cast<GDExtensionPropertyInfo *>(p_list));
}

GDExtensionMethodInfo *make_method_list(const TypedArray<Dictionary> &p_list) {
	if (p_list.is_empty()) {
		return nullptr;
	}
	GDExtensionMethodInfo *list = memnew_arr(GDExtensionMethodInfo, p_list.size());
	for (int64_t i = 0; i < p_list.size(); i++) {
		const Dictionary dict = p_list[i];
		GDExtensionMethodInfo &info = list[i];
		info.name = memnew(StringName(dict.get("name", String())));
		fill_property(info.return_value, dict.get("return", Dictionary()));
		info.flags = uint32_t(int(dict.get("flags", METHOD_FLAGS_DEFAULT)));
		info.id = int32_t(int(dict.get("id", 0)));

		const Array args = dict.get("args", Array());
		info.argument_count = uint32_t(args.size());
		info.arguments = args.is_empty() ? nullptr : memnew_arr(GDExtensionPropertyInfo, args.size());
		for (int64_t j = 0; j < args.size(); j++) {
			fill_property(info.arguments[j], args[j]);
		}

		const Array defaults = dict.get("default_args", Array());
		info.default_argument_count = uint32_t(defaults.size());
		info.default_arguments = defaults.is_empty() ? nullptr : memnew_arr(GDExtensionVariantPtr, defaults.size());
		for (int64_t j = 0; j < defaults.size(); j++) {
			info.default_arguments[j] = memnew(Variant(defaults[j]));
		}
	}
	return list;
}

void free_method_list(const GDExtensionMethodInfo *p_list, uint32_t p_count) {
	if (!p_list) {
		return;
	}
	for (uint32_t i = 0; i < p_count; i++) {
		const GDExtensionMethodInfo &info = p_list[i];
		memdelete(reinterpret_cast<StringName *>(info.name));
		clear_property(info.return_value);
		for (uint32_t j = 0; j < info.argument_count; j++) {
			clear_property(info.arguments[j]);
		}
		if (info.arguments) {
			memdelete_arr(info.arguments);
		}
		for (uint32_t j = 0; j < info.default_argument_count; j++) {
			memdelete(reinterpret_cast<Variant *>(info.default_arguments[j]));
		}
		if (info.default_arguments) {
			memdelete_arr(info.default_arguments);
		}
	}
	memdelete_arr(const_cast<GDExtensionMethodInfo *>(p_list));
}

/* Instance callbacks */

static PetInstance *self(GDExtensionScriptInstanceDataPtr p_instance) {
	return static_cast<PetInstance *>(p_instance);
}

static const StringName &as_name(GDExtensionConstStringNamePtr p_name) {
	return *reinterpret_cast<const StringName *>(p_name);
}

static GDExtensionBool instance_set(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name, GDExtensionConstVariantPtr p_value) {
	const Variant &value = *reinterpret_cast<const Variant *>(p_value);
	if (as_name(p_name) == PetScript::message_name()) {
		if (value.get_type() != Variant::STRING && value.get_type() != Variant::STRING_NAME) {
			return false;
		}
		self(p_instance)->message = value;
		return true;
	}
	return false;
}

static GDExtensionBool instance_get(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name, GDExtensionVariantPtr r_ret) {
	if (as_name(p_name) == PetScript::message_name()) {
		*reinterpret_cast<Variant *>(r_ret) = self(p_instance)->message;
		return true;
	}
	return false;
}

static const GDExtensionPropertyInfo *instance_get_property_list(GDExtensionScriptInstanceDataPtr p_instance, uint32_t *r_count) {
	const TypedArray<Dictionary> list = self(p_instance)->script->_get_script_property_list();
	*r_count = uint32_t(list.size());
	return make_property_list(list);
}

static void instance_free_property_list(GDExtensionScriptInstanceDataPtr p_instance, const GDExtensionPropertyInfo *p_list, uint32_t p_count) {
	free_property_list(p_list, p_count);
}

static GDExtensionBool instance_property_can_revert(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name) {
	return self(p_instance)->script->_has_property_default_value(as_name(p_name));
}

static GDExtensionBool instance_property_get_revert(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name, GDExtensionVariantPtr r_ret) {
	const PetScript *script = self(p_instance)->script.ptr();
	if (!script->_has_property_default_value(as_name(p_name))) {
		return false;
	}
	*reinterpret_cast<Variant *>(r_ret) = script->_get_property_default_value(as_name(p_name));
	return true;
}

static GDExtensionObjectPtr instance_get_owner(GDExtensionScriptInstanceDataPtr p_instance) {
	return self(p_instance)->owner;
}

static const GDExtensionMethodInfo *instance_get_method_list(GDExtensionScriptInstanceDataPtr p_instance, uint32_t *r_count) {
	const TypedArray<Dictionary> list = self(p_instance)->script->_get_script_method_list();
	*r_count = uint32_t(list.size());
	return make_method_list(list);
}

static void instance_free_method_list(GDExtensionScriptInstanceDataPtr p_instance, const GDExtensionMethodInfo *p_list, uint32_t p_count) {
	free_method_list(p_list, p_count);
}

static GDExtensionVariantType instance_get_property_type(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name, GDExtensionBool *r_is_valid) {
	if (as_name(p_name) == PetScript::message_name()) {
		*r_is_valid = true;
		return GDEXTENSION_VARIANT_TYPE_STRING;
	}
	*r_is_valid = false;
	return GDEXTENSION_VARIANT_TYPE_NIL;
}

static GDExtensionBool instance_has_method(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name) {
	return self(p_instance)->script->_has_method(as_name(p_name));
}

static GDExtensionInt instance_get_method_argument_count(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name, GDExtensionBool *r_is_valid) {
	*r_is_valid = self(p_instance)->script->_has_method(as_name(p_name));
	return 0;
}

static void instance_call(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_method, const GDExtensionConstVariantPtr *p_args, GDExtensionInt p_argument_count, GDExtensionVariantPtr r_return, GDExtensionCallError *r_error) {
	PetInstance *instance = self(p_instance);
	const StringName &method = as_name(p_method);
	r_error->error = GDEXTENSION_CALL_OK;

	if (!instance->script->_has_method(method)) {
		r_error->error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
		return;
	}
	if (p_argument_count > 0) {
		r_error->error = GDEXTENSION_CALL_ERROR_TOO_MANY_ARGUMENTS;
		r_error->expected = 0;
		return;
	}

	if (method == PetScript::ready_name()) {
		UtilityFunctions::print(vformat("%s: _ready, message = %s", instance->script->get_script_path(), instance->message));
	} else if (method == PetScript::greet_name()) {
		*reinterpret_cast<Variant *>(r_return) = vformat("Hello from %s: %s", instance->script->get_script_path(), instance->message);
	}
}

static void instance_notification(GDExtensionScriptInstanceDataPtr p_instance, int32_t p_what, GDExtensionBool p_reversed) {
}

static void instance_to_string(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionBool *r_is_valid, GDExtensionStringPtr r_out) {
	*r_is_valid = false;
}

static void instance_refcount_incremented(GDExtensionScriptInstanceDataPtr p_instance) {
}

static GDExtensionBool instance_refcount_decremented(GDExtensionScriptInstanceDataPtr p_instance) {
	return true;
}

static GDExtensionObjectPtr instance_get_script(GDExtensionScriptInstanceDataPtr p_instance) {
	return self(p_instance)->script->_owner;
}

static GDExtensionBool instance_is_placeholder(GDExtensionScriptInstanceDataPtr p_instance) {
	return false;
}

static GDExtensionScriptLanguagePtr instance_get_language(GDExtensionScriptInstanceDataPtr p_instance) {
	return PetLanguage::get_singleton()->_owner;
}

static void instance_free(GDExtensionScriptInstanceDataPtr p_instance) {
	PetInstance *instance = self(p_instance);
	instance->script->instance_freed(instance);
	memdelete(instance);
}

static GDExtensionScriptInstanceInfo3 build_info() {
	GDExtensionScriptInstanceInfo3 info = {};
	info.set_func = instance_set;
	info.get_func = instance_get;
	info.get_property_list_func = instance_get_property_list;
	info.free_property_list_func = instance_free_property_list;
	info.property_can_revert_func = instance_property_can_revert;
	info.property_get_revert_func = instance_property_get_revert;
	info.get_owner_func = instance_get_owner;
	info.get_method_list_func = instance_get_method_list;
	info.free_method_list_func = instance_free_method_list;
	info.get_property_type_func = instance_get_property_type;
	info.has_method_func = instance_has_method;
	info.get_method_argument_count_func = instance_get_method_argument_count;
	info.call_func = instance_call;
	info.notification_func = instance_notification;
	info.to_string_func = instance_to_string;
	info.refcount_incremented_func = instance_refcount_incremented;
	info.refcount_decremented_func = instance_refcount_decremented;
	info.get_script_func = instance_get_script;
	info.is_placeholder_func = instance_is_placeholder;
	info.get_language_func = instance_get_language;
	info.free_func = instance_free;
	return info;
}

const GDExtensionScriptInstanceInfo3 PetInstance::info = build_info();

} // namespace petrichor
