#include "adapter/pet_instance.h"

#include "adapter/pet_language.h"
#include "runtime/text.h"
#include "runtime/vm.h"

#ifdef PETRICHOR_PROBES
#include "probe/probe_methods.h"
#endif

#include <godot_cpp/classes/engine.hpp>

#include <algorithm>
#include <new>

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

/* Instance */

PetInstance::PetInstance(PetScript *p_script, const std::shared_ptr<const ClassImage> &p_image, Object *p_owner) :
		owner(p_owner->_owner), owner_object(p_owner), script(Ref<PetScript>(p_script)), image(p_image) {
	// Every member starts at its type's default value; then the initializers
	// run in declaration order, before anything else runs on the instance, as
	// in GDScript (spec §6.1; modules/gdscript/gdscript_compiler.cpp:2365-2435).
	// A program error in them ends the engine's call, as in the ready function,
	// so _init() does not run either (§8.1).
	storage = std::make_shared<InstanceStorage>(image);
	if (image->module.init_function >= 0) {
		GDExtensionCallError error;
		bool failed = false;
		vm::call(image, image->module.init_function, context(), nullptr, 0, error, &failed);
		if (failed) {
			return;
		}
	}
	// Then the class's own _init(), as GDScript runs it after the member
	// initializers.
	if (image->user_init >= 0) {
		GDExtensionCallError error;
		vm::call(image, image->user_init, context(), nullptr, 0, error);
	}
}

// The members go when no call runs on them any more (spec §3.3).
PetInstance::~PetInstance() {
	storage->freed = true;
}

bool resolve_instance(Object *p_object, ScriptedObject &r_instance) {
	PetInstance *instance = nullptr;
	if (Engine::get_singleton()->is_editor_hint()) {
		// The engine's lookup would read a placeholder as an instance of this
		// language (core/extension/gdextension_interface.cpp:1589-1599), and the
		// editor has placeholders.
		const Ref<PetScript> script = p_object->get_script();
		instance = script.is_valid() ? script->find_instance(p_object) : nullptr;
	} else {
		instance = static_cast<PetInstance *>(gdextension_interface::object_get_script_instance(p_object->_owner, PetLanguage::get_singleton()->_owner));
	}
	if (!instance) {
		return false;
	}
	r_instance.image = instance->current_image();
	r_instance.context = instance->context();
	return true;
}

static PetInstance *self(GDExtensionScriptInstanceDataPtr p_instance) {
	return static_cast<PetInstance *>(p_instance);
}

static const StringName &as_name(GDExtensionConstStringNamePtr p_name) {
	return *reinterpret_cast<const StringName *>(p_name);
}

static const bytecode::Member *visible_member(const ClassImage &p_image, const StringName &p_name) {
	const int index = p_image.find_member(p_name);
	if (index < 0 || !PetScript::is_visible(p_image.module.members[index])) {
		return nullptr;
	}
	return &p_image.module.members[index];
}

static GDExtensionBool instance_set(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name, GDExtensionConstVariantPtr p_value) {
	PetInstance *instance = self(p_instance);
	const std::shared_ptr<const ClassImage> image = instance->current_image();
	const bytecode::Member *member = visible_member(*image, as_name(p_name));
	if (!member) {
		return false;
	}
	// A value that fails the entry check is a program error, reported at the
	// member, which keeps its value (spec §7.5, §10.1); the write is handled.
	const size_t index = size_t(member - image->module.members.data());
	const Variant &value = *reinterpret_cast<const Variant *>(p_value);
	if (!from_variant(member->type, value, instance->storage->members + member->offset, &image->member_checks[index])) {
		vm::report_error(*image, member->name.c_str(), int(member->line), vformat("\"%s\" was not written: %s.", image->member_names[index], entry_mismatch(member->type, value, image->member_checks[index])));
	}
	return true;
}

static GDExtensionBool instance_get(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name, GDExtensionVariantPtr r_ret) {
	PetInstance *instance = self(p_instance);
	const std::shared_ptr<const ClassImage> image = instance->current_image();
	const bytecode::Member *member = visible_member(*image, as_name(p_name));
	if (!member) {
		// A signal reads as a Signal, as in GDScript (modules/gdscript/gdscript.cpp:1648-1651).
		for (const bytecode::SignalInfo &signal : image->module.signals) {
			if (as_name(p_name) == StringName(to_godot(signal.name))) {
				*reinterpret_cast<Variant *>(r_ret) = Signal(instance->owner_object, as_name(p_name));
				return true;
			}
		}
		return false;
	}
	*reinterpret_cast<Variant *>(r_ret) = to_variant(member->type, instance->storage->members + member->offset);
	return true;
}

static const GDExtensionPropertyInfo *instance_get_property_list(GDExtensionScriptInstanceDataPtr p_instance, uint32_t *r_count) {
	const TypedArray<Dictionary> list = visible_properties(*self(p_instance)->current_image());
	*r_count = uint32_t(list.size());
	return make_property_list(list);
}

static void instance_free_property_list(GDExtensionScriptInstanceDataPtr p_instance, const GDExtensionPropertyInfo *p_list, uint32_t p_count) {
	free_property_list(p_list, p_count);
}

// Only a constant initial value is a default the editor can revert to; other
// initializers run per instance (spec §6.1).
static GDExtensionBool instance_property_can_revert(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name) {
	const bytecode::Member *member = visible_member(*self(p_instance)->current_image(), as_name(p_name));
	return member && member->has_initial;
}

static GDExtensionBool instance_property_get_revert(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name, GDExtensionVariantPtr r_ret) {
	const std::shared_ptr<const ClassImage> image = self(p_instance)->current_image();
	const bytecode::Member *member = visible_member(*image, as_name(p_name));
	if (!member || !member->has_initial) {
		return false;
	}
	*reinterpret_cast<Variant *>(r_ret) = initial_value(*image, size_t(member - image->module.members.data()));
	return true;
}

static GDExtensionObjectPtr instance_get_owner(GDExtensionScriptInstanceDataPtr p_instance) {
	return self(p_instance)->owner;
}

static const GDExtensionMethodInfo *instance_get_method_list(GDExtensionScriptInstanceDataPtr p_instance, uint32_t *r_count) {
	const std::shared_ptr<const ClassImage> image = self(p_instance)->current_image();
	TypedArray<Dictionary> list = visible_methods(*image);
#ifdef PETRICHOR_PROBES
	probe_methods::append_to(image->path, list);
#endif
	*r_count = uint32_t(list.size());
	return make_method_list(list);
}

static void instance_free_method_list(GDExtensionScriptInstanceDataPtr p_instance, const GDExtensionMethodInfo *p_list, uint32_t p_count) {
	free_method_list(p_list, p_count);
}

static GDExtensionVariantType instance_get_property_type(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name, GDExtensionBool *r_is_valid) {
	const std::shared_ptr<const ClassImage> image = self(p_instance)->current_image();
	const bytecode::Member *member = visible_member(*image, as_name(p_name));
	*r_is_valid = member != nullptr;
	return member ? GDExtensionVariantType(variant_type(member->type)) : GDEXTENSION_VARIANT_TYPE_NIL;
}

static GDExtensionBool instance_has_method(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name) {
	const std::shared_ptr<const ClassImage> image = self(p_instance)->current_image();
	if (PetScript::find_visible_function(*image, as_name(p_name)) >= 0) {
		return true;
	}
#ifdef PETRICHOR_PROBES
	return probe_methods::has(image->path, as_name(p_name));
#else
	return false;
#endif
}

static GDExtensionInt instance_get_method_argument_count(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name, GDExtensionBool *r_is_valid) {
	const std::shared_ptr<const ClassImage> image = self(p_instance)->current_image();
	const int index = PetScript::find_visible_function(*image, as_name(p_name));
	*r_is_valid = index >= 0;
	return index >= 0 ? GDExtensionInt(image->module.functions[index].params.size()) : 0;
}

static void instance_call(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_method, const GDExtensionConstVariantPtr *p_args, GDExtensionInt p_argument_count, GDExtensionVariantPtr r_return, GDExtensionCallError *r_error) {
	PetInstance *instance = self(p_instance);
	const StringName &method = as_name(p_method);
	const Variant **args = reinterpret_cast<const Variant **>(const_cast<GDExtensionConstVariantPtr *>(p_args));
	Variant &result = *reinterpret_cast<Variant *>(r_return);
	r_error->error = GDEXTENSION_CALL_OK;

	// One snapshot for the lookup and the call.
	const std::shared_ptr<const ClassImage> image = instance->current_image();
	const InstanceContext context = instance->context();
	if (image->module.ready_function >= 0 && method == image->ready_method) {
		// The @onready initializers run first, as in GDScript
		// (modules/gdscript/gdscript.cpp:1948-1953); the engine calls _ready()
		// whether or not the class defines it (core/object/gdvirtual.gen.inc:39-45).
		// They are part of the engine's call, which a program error in them
		// ends (spec §8.1).
		GDExtensionCallError ready_error;
		bool failed = false;
		vm::call(image, image->module.ready_function, context, nullptr, 0, ready_error, &failed);
		if (failed) {
			return;
		}
	}
	const int index = PetScript::find_visible_function(*image, method);
	if (index < 0) {
#ifdef PETRICHOR_PROBES
		if (probe_methods::call(image->path, method, args, p_argument_count, result, *r_error)) {
			return;
		}
#endif
		r_error->error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
		return;
	}
	result = vm::call(image, index, context, args, p_argument_count, *r_error);
}

// The engine sends notifications to the script instance instead of calling
// _notification(); GDScript calls it on every script of the inheritance chain
// (modules/gdscript/gdscript.cpp:1968-2000), and a class of this language is one.
static void instance_notification(GDExtensionScriptInstanceDataPtr p_instance, int32_t p_what, GDExtensionBool p_reversed) {
	PetInstance *instance = self(p_instance);
	const std::shared_ptr<const ClassImage> image = instance->current_image();
	if (image->notification_function < 0) {
		return;
	}
	const Variant what = int64_t(p_what);
	const Variant *args[] = { &what };
	GDExtensionCallError error;
	vm::call(image, image->notification_function, instance->context(), args, 1, error);
}

// The engine's to_string() asks the instance (modules/gdscript/gdscript.cpp:2003-2022).
static void instance_to_string(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionBool *r_is_valid, GDExtensionStringPtr r_out) {
	PetInstance *instance = self(p_instance);
	const std::shared_ptr<const ClassImage> image = instance->current_image();
	*r_is_valid = false;
	if (image->to_string_function < 0) {
		return;
	}
	GDExtensionCallError error;
	bool failed = false;
	const Variant text = vm::call(image, image->to_string_function, instance->context(), nullptr, 0, error, &failed);
	if (!failed && error.error == GDEXTENSION_CALL_OK) {
		*reinterpret_cast<String *>(r_out) = text;
		*r_is_valid = true;
	}
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
