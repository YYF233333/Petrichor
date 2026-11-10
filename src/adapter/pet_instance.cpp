#include "adapter/pet_instance.h"

#include "adapter/pet_language.h"
#include "runtime/task.h"
#include "runtime/text.h"
#include "runtime/vm.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/node.hpp>

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

static std::vector<std::shared_ptr<const ClassImage>> image_chain(const std::shared_ptr<const ClassImage> &p_image) {
	std::vector<std::shared_ptr<const ClassImage>> chain;
	for (auto image = p_image; image; image = image->base) {
		chain.push_back(image);
	}
	return chain;
}

static bool run_initializers(const std::shared_ptr<const ClassImage> &p_image, InstanceContext p_context, bool p_ready) {
	const auto chain = image_chain(p_image);
	p_context.self_image = p_image;
	for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
		const int index = p_ready ? (*it)->module.ready_function : (*it)->module.init_function;
		if (index < 0) {
			continue;
		}
		GDExtensionCallError error;
		bool failed = false;
		vm::call(*it, index, p_context, nullptr, 0, error, &failed);
		if (failed || error.error != GDEXTENSION_CALL_OK) {
			return false;
		}
	}
	return true;
}

PetInstance::PetInstance(PetScript *p_script, const std::shared_ptr<const ClassImage> &p_image, Object *p_owner) :
		owner(p_owner->_owner), owner_object(p_owner), script(Ref<PetScript>(p_script)), image(p_image) {
	// Every member starts at its type's default value; then the initializers
	// run in declaration order, before anything else runs on the instance, as
	// in GDScript (spec §6.1; modules/gdscript/gdscript_compiler.cpp:2365-2435).
	// A program error in them ends the engine's call, as in the ready function,
	// so _init() does not run either (§8.1).
	const bool init_later = init_left_to_caller();
	storage = std::make_shared<InstanceStorage>(image);
	const bool initialized = run_initializers(image, context(), false);
	// Then the nearest _init(), as GDScript runs it after the member
	// initializers; new(...) calls it itself with its arguments.
	if (initialized && image->user_init >= 0 && !init_later) {
		const bytecode::Function &init = image->module.functions[image->user_init];
		if (!init.takes(0)) {
			// GDScript creates no instance then (modules/gdscript/gdscript.cpp:192-203, 406-424).
			vm::report_error(*image, "_init", int(init.line), vformat("%s was constructed without arguments, but its _init() requires them; construct it with new(...).", image->global_name == StringName() ? String(image->path) : String(image->global_name)));
		} else {
			GDExtensionCallError error;
			vm::call(image, image->user_init, context(), nullptr, 0, error);
		}
	}
	constructed(initialized);
}

// The members go when no call runs on them any more (spec §3.3).
PetInstance::~PetInstance() {
	storage->freed = true;
	tasks::cancel_owner(storage);
}

bool resolve_instance(GDExtensionObjectPtr p_object, ScriptedObject &r_instance) {
	static const bool editor = Engine::get_singleton()->is_editor_hint();
	PetInstance *instance = nullptr;
	if (editor) {
		// The engine's lookup would read a placeholder as an instance of this
		// language (core/extension/gdextension_interface.cpp:1589-1599), and the
		// editor has placeholders.
		Object *object = internal::get_object_instance_binding(p_object);
		const Ref<PetScript> script = object->get_script();
		instance = script.is_valid() ? script->find_instance(object) : nullptr;
	} else {
		instance = static_cast<PetInstance *>(gdextension_interface::object_get_script_instance(p_object, PetLanguage::get_singleton()->_owner));
	}
	if (!instance) {
		return false;
	}
	r_instance.image = instance->current_image();
	r_instance.context = instance->context();
	return true;
}

std::shared_ptr<const ClassImage> resolve_script(const String &p_path) {
	const Ref<PetScript> script = load_resource(p_path);
	return script.is_valid() ? script->get_image() : nullptr;
}

Variant resolve_inner_script(const Variant &p_script, const String &p_name) {
	const Ref<PetScript> script = p_script;
	return script.is_valid() ? Variant(script->get_inner_script(p_name)) : Variant();
}

String resolve_script_path(const Ref<Script> &p_script) {
	const Ref<PetScript> script = p_script;
	return script.is_valid() ? script->get_script_path() : p_script->get_path();
}

static PetInstance *self(GDExtensionScriptInstanceDataPtr p_instance) {
	return static_cast<PetInstance *>(p_instance);
}

static const StringName &as_name(GDExtensionConstStringNamePtr p_name) {
	return *reinterpret_cast<const StringName *>(p_name);
}

static const bytecode::Member *visible_member(const ClassImage &p_image, const StringName &p_name) {
	const int index = p_image.find_member(p_name);
	if (index < 0 || !PetScript::is_visible(p_image, p_image.module.members[index])) {
		return nullptr;
	}
	return &p_image.module.members[index];
}

// Calls a method of Object on each class of the chain that declares it, the
// most derived first, until one handles the call. Object's documentation says
// so of _get, _set, _get_property_list, _property_can_revert and
// _property_get_revert, which therefore call no super (doc/classes/Object.xml:77,
// 192, 284, 293, 348; modules/gdscript/gdscript.cpp:1590, 1677, 1739, 1857, 1879;
// ed1daf0). A program error ends the engine's call (spec §8.1). Tweens and
// animations write by name every frame, so a chain without one costs no copy.
template <typename Handled>
static bool call_each(PetInstance *p_instance, const std::shared_ptr<const ClassImage> &p_image, ClassImage::Virtual p_virtual, const Variant **p_args, int p_argc, const Handled &p_handled) {
	for (const ClassImage *owner = p_image.get(); owner; owner = owner->base.get()) {
		const int index = owner->virtual_function(p_virtual);
		if (index < 0 || owner->module.functions[index].is_inherited) {
			continue;
		}
		InstanceContext context = p_instance->context();
		context.self_image = p_image;
		GDExtensionCallError error;
		bool failed = false;
		const Variant result = vm::call(owner->shared_from_this(), index, std::move(context), p_args, p_argc, error, &failed);
		if (failed || error.error != GDEXTENSION_CALL_OK) {
			return false;
		}
		if (p_handled(*owner, owner->module.functions[index], result)) {
			return true;
		}
	}
	return false;
}

// The nearest _validate_property(), an ordinary override, changes a property
// in the dictionary it is given (doc/classes/Object.xml:369;
// modules/gdscript/gdscript.cpp:1710-1729; ed1daf0).
static bool validate_property(PetInstance *p_instance, const std::shared_ptr<const ClassImage> &p_image, Dictionary &r_property) {
	const int index = p_image->virtual_function(ClassImage::Virtual::ValidateProperty);
	if (index < 0) {
		return false;
	}
	const Variant property = r_property;
	const Variant *args[] = { &property };
	GDExtensionCallError error;
	bool failed = false;
	vm::call(p_image, index, p_instance->context(), args, 1, error, &failed);
	return !failed && error.error == GDEXTENSION_CALL_OK;
}

static void instance_property_state(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionScriptInstancePropertyStateAdd p_add, void *p_userdata) {
	PetInstance *instance = self(p_instance);
	const auto image = instance->current_image();
	for (size_t i = 0; i < image->module.members.size(); ++i) {
		const auto &member = image->module.members[i];
		if (!member.is_export) {
			continue;
		}
		if (member.must_assign && !image->assigned(instance->storage->members, i)) {
			continue;
		}
		const Variant value = outward_variant(image->types[member.type], instance->storage->members + member.offset);
		p_add(image->member_names[i]._native_ptr(), value._native_ptr(), p_userdata);
	}
}

static GDExtensionBool instance_set(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name, GDExtensionConstVariantPtr p_value) {
	PetInstance *instance = self(p_instance);
	const std::shared_ptr<const ClassImage> image = instance->current_image();
	const bytecode::Member *member = visible_member(*image, as_name(p_name));
	const Variant &value = *reinterpret_cast<const Variant *>(p_value);
	if (!member) {
		// A pub static variable, through an object as in GDScript
		// (modules/gdscript/gdscript.cpp:1562-1587; spec §4.8); then _set().
		if (vm::set_static(image, as_name(p_name), value)) {
			return true;
		}
		const Variant name = as_name(p_name);
		const Variant *args[] = { &name, &value };
		return call_each(instance, image, ClassImage::Virtual::Set, args, 2, [](const ClassImage &, const bytecode::Function &, const Variant &p_handled) { return bool(p_handled); });
	}
	// The scene, the inspector, Tween and other classes write through the
	// setter (spec §4.4), whose entry check takes the value.
	if (member->setter >= 0) {
		const Variant *args[] = { &value };
		GDExtensionCallError error;
		vm::call(image, member->setter, instance->context(), args, 1, error);
		return true;
	}
	// A value that fails the entry check is a program error, reported at the
	// member, which keeps its value (spec §7.5, §10.1); the write is handled.
	const size_t index = size_t(member - image->module.members.data());
	String mismatch;
	const bool written = entry_check(image->types[member->type], value, instance->storage->members + member->offset, mismatch);
	if (!mismatch.is_empty()) {
		vm::report_error(*image, member->name.c_str(), int(member->line), vformat("\"%s\" was not written: %s.", image->member_names[index], mismatch));
	} else if (written && member->tracks_assignment()) {
		image->assigned(instance->storage->members, index) = 1;
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
			if (as_name(p_name) == StringName(to_godot(bytecode::engine_signal_name(signal.name, signal.is_pub)))) {
				*reinterpret_cast<Variant *>(r_ret) = Signal(instance->owner_object, as_name(p_name));
				return true;
			}
		}
		// A pub constant reads through an instance too, as in GDScript
		// (modules/gdscript/gdscript.cpp:1624-1632; spec §4.5).
		// So do pub static variables (gdscript.cpp:1634-1646; spec §4.8).
		Variant &ret = *reinterpret_cast<Variant *>(r_ret);
		if (pub_constant(*image, as_name(p_name), ret) || vm::get_static(image, as_name(p_name), ret)) {
			return true;
		}
		const Variant name = as_name(p_name);
		const Variant *args[] = { &name };
		return call_each(instance, image, ClassImage::Virtual::Get, args, 1, [&](const ClassImage &, const bytecode::Function &, const Variant &p_value) {
			ret = p_value;
			return p_value.get_type() != Variant::NIL;
		});
	}
	if (member->getter >= 0) {
		GDExtensionCallError error;
		*reinterpret_cast<Variant *>(r_ret) = vm::call(image, member->getter, instance->context(), nullptr, 0, error);
		return true;
	}
	// Null until assigned (spec §6.2), with no program error: the debugger's
	// inspector reads every property (scene/debugger/scene_debugger_object.cpp:85).
	const size_t index = size_t(member - image->module.members.data());
	if (member->must_assign && !image->assigned(instance->storage->members, index)) {
		*reinterpret_cast<Variant *>(r_ret) = Variant();
		return true;
	}
	*reinterpret_cast<Variant *>(r_ret) = outward_variant(image->types[member->type], instance->storage->members + member->offset);
	return true;
}

// The members, then what each class's _get_property_list() adds; each passes
// _validate_property(), as in GDScript (modules/gdscript/gdscript.cpp:1731-1848; ed1daf0).
static const GDExtensionPropertyInfo *instance_get_property_list(GDExtensionScriptInstanceDataPtr p_instance, uint32_t *r_count) {
	PetInstance *instance = self(p_instance);
	const std::shared_ptr<const ClassImage> image = instance->current_image();
	TypedArray<Dictionary> list = visible_properties(*image);
	call_each(instance, image, ClassImage::Virtual::GetPropertyList, nullptr, 0, [&](const ClassImage &p_class, const bytecode::Function &p_function, const Variant &p_added) {
		const Array added = p_added;
		for (int64_t i = 0; i < added.size(); i++) {
			const Dictionary property = added[i];
			if (property.has("name") && property.has("type")) {
				list.push_back(property);
			} else {
				vm::report_error(p_class, "_get_property_list", int(p_function.line), "_get_property_list() gave a property without \"name\" or \"type\"; each needs both.");
			}
		}
		return false;
	});
	for (int64_t i = 0; i < list.size(); i++) {
		Dictionary property = list[i];
		validate_property(instance, image, property);
	}
	*r_count = uint32_t(list.size());
	return make_property_list(list);
}

static void instance_free_property_list(GDExtensionScriptInstanceDataPtr p_instance, const GDExtensionPropertyInfo *p_list, uint32_t p_count) {
	free_property_list(p_list, p_count);
}

// The classes' own _property_can_revert() and _property_get_revert() answer
// first. Otherwise only a constant initial value is a default the editor can
// revert to; other initializers run per instance (spec §6.1).
static GDExtensionBool instance_property_can_revert(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name) {
	PetInstance *instance = self(p_instance);
	const std::shared_ptr<const ClassImage> image = instance->current_image();
	const Variant name = as_name(p_name);
	const Variant *args[] = { &name };
	if (call_each(instance, image, ClassImage::Virtual::PropertyCanRevert, args, 1, [](const ClassImage &, const bytecode::Function &, const Variant &p_can) { return bool(p_can); })) {
		return true;
	}
	const bytecode::Member *member = visible_member(*image, as_name(p_name));
	return member && member->has_initial;
}

static GDExtensionBool instance_property_get_revert(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name, GDExtensionVariantPtr r_ret) {
	PetInstance *instance = self(p_instance);
	const std::shared_ptr<const ClassImage> image = instance->current_image();
	Variant &ret = *reinterpret_cast<Variant *>(r_ret);
	const Variant name = as_name(p_name);
	const Variant *args[] = { &name };
	if (call_each(instance, image, ClassImage::Virtual::PropertyGetRevert, args, 1, [&](const ClassImage &, const bytecode::Function &, const Variant &p_value) {
			ret = p_value;
			return p_value.get_type() != Variant::NIL;
		})) {
		return true;
	}
	const bytecode::Member *member = visible_member(*image, as_name(p_name));
	if (!member || !member->has_initial) {
		return false;
	}
	ret = initial_value(*image, size_t(member - image->module.members.data()));
	return true;
}

// For the engine's own properties, which Object::validate_property() passes to the
// script last (core/object/object.cpp:565, core/object/script_language_extension.h:757-778; ed1daf0).
static GDExtensionBool instance_validate_property(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionPropertyInfo *p_property) {
	PetInstance *instance = self(p_instance);
	const std::shared_ptr<const ClassImage> image = instance->current_image();
	if (image->virtual_function(ClassImage::Virtual::ValidateProperty) < 0) {
		return false;
	}
	StringName &name = *reinterpret_cast<StringName *>(p_property->name);
	StringName &class_name = *reinterpret_cast<StringName *>(p_property->class_name);
	String &hint_string = *reinterpret_cast<String *>(p_property->hint_string);
	Dictionary property;
	property["name"] = String(name);
	property["class_name"] = class_name;
	property["type"] = int64_t(p_property->type);
	property["hint"] = p_property->hint;
	property["hint_string"] = hint_string;
	property["usage"] = p_property->usage;
	if (!validate_property(instance, image, property)) {
		return false;
	}
	p_property->type = GDExtensionVariantType(int(property.get("type", 0)));
	name = property.get("name", String());
	class_name = property.get("class_name", StringName());
	p_property->hint = uint32_t(int(property.get("hint", 0)));
	hint_string = property.get("hint_string", String());
	p_property->usage = uint32_t(int(property.get("usage", 0)));
	return true;
}

static GDExtensionObjectPtr instance_get_owner(GDExtensionScriptInstanceDataPtr p_instance) {
	return self(p_instance)->owner;
}

static const GDExtensionMethodInfo *instance_get_method_list(GDExtensionScriptInstanceDataPtr p_instance, uint32_t *r_count) {
	const std::shared_ptr<const ClassImage> image = self(p_instance)->current_image();
	const TypedArray<Dictionary> list = visible_methods(*image);
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
	return member ? GDExtensionVariantType(variant_type(image->types[member->type])) : GDEXTENSION_VARIANT_TYPE_NIL;
}

static GDExtensionBool instance_has_method(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name) {
	return PetScript::find_visible_function(*self(p_instance)->current_image(), as_name(p_name)) >= 0;
}

static GDExtensionInt instance_get_method_argument_count(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name, GDExtensionBool *r_is_valid) {
	const std::shared_ptr<const ClassImage> image = self(p_instance)->current_image();
	const int index = PetScript::find_visible_function(*image, as_name(p_name));
	*r_is_valid = index >= 0;
	return index >= 0 ? GDExtensionInt(image->module.functions[index].fixed_params()) : 0;
}

static void instance_call(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_method, const GDExtensionConstVariantPtr *p_args, GDExtensionInt p_argument_count, GDExtensionVariantPtr r_return, GDExtensionCallError *r_error) {
	PetInstance *instance = self(p_instance);
	const StringName &method = as_name(p_method);
	const Variant **args = reinterpret_cast<const Variant **>(const_cast<GDExtensionConstVariantPtr *>(p_args));
	Variant &result = *reinterpret_cast<Variant *>(r_return);
	r_error->error = GDEXTENSION_CALL_OK;

	// One snapshot for the lookup and the call.
	std::shared_ptr<const ClassImage> image = instance->current_image();
	InstanceContext context = instance->context();
	// The @onready initializers run first, base class first, as in GDScript
	// (modules/gdscript/gdscript.cpp:1937-1953); the engine calls _ready()
	// whether or not the class defines it (core/object/make_virtuals.py:1-10).
	// They are part of the engine's call, which a program error in them ends
	// (spec §8.1).
	if (method == image->ready_method && !run_initializers(image, context, true)) {
		return;
	}
	const int index = PetScript::find_visible_function(*image, method);
	if (index < 0) {
		r_error->error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
		return;
	}
	result = vm::call(std::move(image), index, std::move(context), args, p_argument_count, *r_error);
}

// The engine sends notifications to the script instance instead of calling
// _notification(); GDScript calls it on every script of the inheritance chain
// (modules/gdscript/gdscript.cpp:1968-2000), and a class of this language is one.
static void instance_notification(GDExtensionScriptInstanceDataPtr p_instance, int32_t p_what, GDExtensionBool p_reversed) {
	PetInstance *instance = self(p_instance);
	if (p_what == Object::NOTIFICATION_PREDELETE) {
		tasks::cancel_owner(instance->storage);
	}
	const std::shared_ptr<const ClassImage> image = instance->current_image();
	auto chain = image_chain(image);
	// Sent as the node gets a parent (scene/main/node.cpp:1699; ed1daf0), before
	// _notification() (spec §6.4).
	if (p_what == Node::NOTIFICATION_PARENTED && !std::all_of(chain.rbegin(), chain.rend(), [&](const auto &p_class) { return vm::check_parent(*p_class, instance->owner_object); })) {
		return;
	}
	if (!p_reversed) {
		std::reverse(chain.begin(), chain.end());
	}
	InstanceContext context = instance->context();
	context.self_image = image;
	const Variant what = int64_t(p_what);
	const Variant *args[] = { &what };
	for (const auto &owner : chain) {
		const int index = owner->virtual_function(ClassImage::Virtual::Notification);
		if (index < 0 || owner->module.functions[index].is_inherited) {
			continue;
		}
		GDExtensionCallError error;
		bool failed = false;
		vm::call(owner, index, context, args, 1, error, &failed);
		if (failed) {
			return;
		}
	}
}

// The engine's to_string() asks the instance (modules/gdscript/gdscript.cpp:2003-2022).
static void instance_to_string(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionBool *r_is_valid, GDExtensionStringPtr r_out) {
	PetInstance *instance = self(p_instance);
	const std::shared_ptr<const ClassImage> image = instance->current_image();
	const int index = image->virtual_function(ClassImage::Virtual::ToString);
	*r_is_valid = false;
	if (index < 0) {
		return;
	}
	GDExtensionCallError error;
	bool failed = false;
	const Variant text = vm::call(image, index, instance->context(), nullptr, 0, error, &failed);
	if (!failed && error.error == GDEXTENSION_CALL_OK) {
		*reinterpret_cast<String *>(r_out) = text;
		*r_is_valid = true;
	}
}

static void instance_refcount_incremented(GDExtensionScriptInstanceDataPtr p_instance) {
}

// Without this callback the engine answers false, and a RefCounted owner is
// never freed (core/object/script_language_extension.h:888-891; ed1daf0).
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

// Editor builds ask for these after every other lookup by name has failed,
// and without them read an uninitialized bool that may say found
// (core/object/object.cpp:289-293, 364-368; core/object/script_language_extension.h:909-926; ed1daf0).
static GDExtensionBool instance_set_fallback(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name, GDExtensionConstVariantPtr p_value) {
	return false;
}

static GDExtensionBool instance_get_fallback(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name, GDExtensionVariantPtr r_ret) {
	return false;
}

static GDExtensionScriptInstanceInfo3 build_info() {
	GDExtensionScriptInstanceInfo3 info = {};
	info.set_func = instance_set;
	info.get_func = instance_get;
	info.set_fallback_func = instance_set_fallback;
	info.get_fallback_func = instance_get_fallback;
	info.get_property_list_func = instance_get_property_list;
	info.get_property_state_func = instance_property_state;
	info.free_property_list_func = instance_free_property_list;
	info.property_can_revert_func = instance_property_can_revert;
	info.property_get_revert_func = instance_property_get_revert;
	info.get_owner_func = instance_get_owner;
	info.get_method_list_func = instance_get_method_list;
	info.free_method_list_func = instance_free_method_list;
	info.get_property_type_func = instance_get_property_type;
	info.validate_property_func = instance_validate_property;
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
