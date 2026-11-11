#include "adapter/pet_placeholder.h"

#include "adapter/pet_instance.h"
#include "adapter/pet_language.h"
#include "runtime/text.h"

#ifdef TOOLS_ENABLED
#include "adapter/editor/script_compiler.h"

#include <godot_cpp/classes/node.hpp>
#endif

#include <godot_cpp/templates/hash_set.hpp>

#include <string>
#include <utility>
#include <vector>

using namespace godot;

namespace petrichor {

PetPlaceholder::PetPlaceholder(PetScript *p_script, Object *p_owner) :
		owner(p_owner), script(p_script) {
	update(false);
}

bool PetPlaceholder::default_value(const StringName &p_name, Variant &r_value) const {
	if (!script->_has_property_default_value(p_name)) {
		return false;
	}
	r_value = script->_get_property_default_value(p_name);
	return true;
}

void PetPlaceholder::update(bool p_notify) {
	const TypedArray<Dictionary> list = script->_get_script_property_list();
	HashMap<StringName, Variant> initial;
	if (const std::shared_ptr<const ClassImage> current = script->get_image()) {
		for (size_t i = 0; i < current->module.members.size(); i++) {
			const bytecode::Member &member = current->module.members[i];
			if (PetScript::is_visible(*current, member) && member.has_initial) {
				initial[current->member_names[i]] = initial_value(*current, i);
			}
		}
	}
	HashSet<StringName> names;
	for (int64_t i = 0; i < list.size(); i++) {
		const Dictionary property = list[i];
		if (uint32_t(int(property.get("usage", 0))) & (PROPERTY_USAGE_GROUP | PROPERTY_USAGE_SUBGROUP | PROPERTY_USAGE_CATEGORY)) {
			continue;
		}
		const StringName name = property.get("name", String());
		names.insert(name);
		const Variant::Type type = Variant::Type(int(property.get("type", Variant::NIL)));
		if ((!values.has(name) || (type != Variant::NIL && values[name].get_type() != type)) && initial.has(name)) {
			values[name] = initial[name];
		}
	}
	properties = list;
	std::vector<StringName> dropped;
	for (const KeyValue<StringName, Variant> &entry : values) {
		Variant default_;
		if (!names.has(entry.key) || (default_value(entry.key, default_) && default_ == entry.value)) {
			dropped.push_back(entry.key);
		}
	}
	for (const StringName &name : dropped) {
		values.erase(name);
	}
	if (p_notify) {
		owner->notify_property_list_changed();
	}
	constants.clear();
	const Dictionary script_constants = script->_get_constants();
	const Array keys = script_constants.keys();
	for (int64_t i = 0; i < keys.size(); i++) {
		constants[keys[i]] = script_constants[keys[i]];
	}
}

static PetPlaceholder *self(GDExtensionScriptInstanceDataPtr p_instance) {
	return static_cast<PetPlaceholder *>(p_instance);
}

static const StringName &as_name(GDExtensionConstStringNamePtr p_name) {
	return *reinterpret_cast<const StringName *>(p_name);
}

static bool holds(Variant::Operator p_operator, const Variant &p_a, const Variant &p_b) {
	Variant result;
	bool valid = false;
	Variant::evaluate(p_operator, p_a, p_b, result, valid);
	return valid && bool(result);
}

static GDExtensionBool placeholder_set(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name, GDExtensionConstVariantPtr p_value) {
	PetPlaceholder *placeholder = self(p_instance);
	if (placeholder->script->_is_placeholder_fallback_enabled()) {
		return false;
	}
	const StringName &name = as_name(p_name);
	const Variant &value = *reinterpret_cast<const Variant *>(p_value);
	Variant default_;
	const bool has_default = placeholder->default_value(name, default_);
	if (placeholder->values.has(name)) {
		// The evaluation takes nil as equal to, for example, an empty resource.
		if (has_default && holds(Variant::OP_EQUAL, default_, value)) {
			placeholder->values.erase(name);
		} else {
			placeholder->values[name] = value;
		}
		return true;
	}
	// Also one of another type, as an int stored for what is now a struct,
	// which the resource check reports (architecture §6.4).
	if (has_default && !holds(Variant::OP_EQUAL, default_, value)) {
		placeholder->values[name] = value;
	}
	return has_default;
}

static GDExtensionBool placeholder_get(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name, GDExtensionVariantPtr r_ret) {
	const PetPlaceholder *placeholder = self(p_instance);
	const StringName &name = as_name(p_name);
	Variant &result = *reinterpret_cast<Variant *>(r_ret);
	if (const Variant *value = placeholder->values.getptr(name)) {
		result = *value;
		return true;
	}
	if (const Variant *constant = placeholder->constants.getptr(name)) {
		result = *constant;
		return true;
	}
	return !placeholder->script->_is_placeholder_fallback_enabled() && placeholder->default_value(name, result);
}

static const GDExtensionPropertyInfo *placeholder_get_property_list(GDExtensionScriptInstanceDataPtr p_instance, uint32_t *r_count) {
	*r_count = uint32_t(self(p_instance)->properties.size());
	return make_property_list(self(p_instance)->properties);
}

static void placeholder_free_property_list(GDExtensionScriptInstanceDataPtr p_instance, const GDExtensionPropertyInfo *p_list, uint32_t p_count) {
	free_property_list(p_list, p_count);
}

static GDExtensionVariantType placeholder_get_property_type(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name, GDExtensionBool *r_is_valid) {
	const PetPlaceholder *placeholder = self(p_instance);
	const Variant *value = placeholder->values.getptr(as_name(p_name));
	if (!value) {
		value = placeholder->constants.getptr(as_name(p_name));
	}
	*r_is_valid = value != nullptr;
	return value ? GDExtensionVariantType(value->get_type()) : GDEXTENSION_VARIANT_TYPE_NIL;
}

static const GDExtensionMethodInfo *placeholder_get_method_list(GDExtensionScriptInstanceDataPtr p_instance, uint32_t *r_count) {
	const PetPlaceholder *placeholder = self(p_instance);
	const TypedArray<Dictionary> list = placeholder->script->_is_placeholder_fallback_enabled() ? TypedArray<Dictionary>() : placeholder->script->_get_script_method_list();
	*r_count = uint32_t(list.size());
	return make_method_list(list);
}

static void placeholder_free_method_list(GDExtensionScriptInstanceDataPtr p_instance, const GDExtensionMethodInfo *p_list, uint32_t p_count) {
	free_method_list(p_list, p_count);
}

static GDExtensionBool placeholder_has_method(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name) {
	const PetPlaceholder *placeholder = self(p_instance);
	if (placeholder->script->_is_placeholder_fallback_enabled()) {
		return false;
	}
	for (Ref<PetScript> script = placeholder->script; script.is_valid(); script = script->_get_base_script()) {
		if (script->_has_method(as_name(p_name))) {
			return true;
		}
	}
	return false;
}

#ifdef TOOLS_ENABLED
// The errors marked on the node where scenes place it (spec §13.2): as the
// root of an instanced scene, in that scene, and in the scene of each owner
// up the chain, whose root owns the roots of the scenes it instances, which
// own their nodes (scene/resources/packed_scene.cpp:567-573; ed1daf0); by the
// script's check and by those scenes' (architecture §6.4).
static PackedStringArray node_errors(const PetPlaceholder &p_placeholder) {
	PackedStringArray errors;
	Node *node = Object::cast_to<Node>(p_placeholder.owner);
	const String path = p_placeholder.script->get_path();
	if (!node || !path.begins_with("res://")) {
		return errors;
	}
	std::vector<std::pair<std::string, std::string>> places;
	for (const Node *root = node; root; root = root->get_owner()) {
		if (!root->get_scene_file_path().is_empty()) {
			places.emplace_back(to_utf8(root->get_scene_file_path()), to_utf8(String(root->get_path_to(node))));
		}
	}
	std::vector<compiler::Diagnostic> found = scene_errors(path, p_placeholder.script->_get_source_code());
	for (const auto &[scene, at] : places) {
		const std::vector<compiler::Diagnostic> checked = resource_errors(to_godot(scene));
		found.insert(found.end(), checked.begin(), checked.end());
	}
	for (const compiler::Diagnostic &error : found) {
		for (const auto &[scene, at] : places) {
			if (error.scene == scene && error.node == at) {
				// The message names the scene for the script editor; the dock shows it already.
				const std::string prefix = scene + ": ";
				const bool named = error.message.compare(0, prefix.size(), prefix) == 0;
				errors.push_back(to_godot(named ? error.message.substr(prefix.size()) : error.message));
			}
		}
	}
	return errors;
}
#endif

static void placeholder_call(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_method, const GDExtensionConstVariantPtr *p_args, GDExtensionInt p_argument_count, GDExtensionVariantPtr r_return, GDExtensionCallError *r_error) {
#ifdef TOOLS_ENABLED
	if (as_name(p_method) == StringName("_get_configuration_warnings") && p_argument_count == 0) {
		*reinterpret_cast<Variant *>(r_return) = node_errors(*self(p_instance));
		r_error->error = GDEXTENSION_CALL_OK;
		return;
	}
#endif
	r_error->error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
}

// While the script has errors, a value the scene sets is kept with a property
// of its own, which the inspector does not show; the write still fails.
static GDExtensionBool placeholder_set_fallback(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name, GDExtensionConstVariantPtr p_value) {
	PetPlaceholder *placeholder = self(p_instance);
	if (!placeholder->script->_is_placeholder_fallback_enabled()) {
		return false;
	}
	const StringName &name = as_name(p_name);
	const Variant &value = *reinterpret_cast<const Variant *>(p_value);
	placeholder->values[name] = value;
	for (int64_t i = 0; i < placeholder->properties.size(); i++) {
		if (StringName(Dictionary(placeholder->properties[i]).get("name", String())) == name) {
			return false;
		}
	}
	const Object *object = value.get_validated_object();
	Dictionary property;
	property["name"] = name;
	property["type"] = value.get_type();
	property["hint"] = object && object->is_class("Node") ? PROPERTY_HINT_NODE_TYPE : PROPERTY_HINT_NONE;
	property["hint_string"] = String();
	property["usage"] = PROPERTY_USAGE_NO_EDITOR | PROPERTY_USAGE_SCRIPT_VARIABLE;
	placeholder->properties.push_back(property);
	return false;
}

static GDExtensionBool placeholder_get_fallback(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name, GDExtensionVariantPtr r_ret) {
	const PetPlaceholder *placeholder = self(p_instance);
	if (!placeholder->script->_is_placeholder_fallback_enabled()) {
		return false;
	}
	const Variant *value = placeholder->values.getptr(as_name(p_name));
	if (!value) {
		value = placeholder->constants.getptr(as_name(p_name));
	}
	if (value) {
		*reinterpret_cast<Variant *>(r_ret) = *value;
	}
	return value != nullptr;
}

static void placeholder_refcount_incremented(GDExtensionScriptInstanceDataPtr p_instance) {
}

// Without it a RefCounted object with the placeholder, such as a resource,
// would never be freed (core/object/script_language_extension.h:887-892).
static GDExtensionBool placeholder_refcount_decremented(GDExtensionScriptInstanceDataPtr p_instance) {
	return true;
}

static GDExtensionObjectPtr placeholder_get_owner(GDExtensionScriptInstanceDataPtr p_instance) {
	return self(p_instance)->owner->_owner;
}

static GDExtensionObjectPtr placeholder_get_script(GDExtensionScriptInstanceDataPtr p_instance) {
	return self(p_instance)->script->_owner;
}

static GDExtensionBool placeholder_is_placeholder(GDExtensionScriptInstanceDataPtr p_instance) {
	return true;
}

static GDExtensionScriptLanguagePtr placeholder_get_language(GDExtensionScriptInstanceDataPtr p_instance) {
	return PetLanguage::get_singleton()->_owner;
}

static void placeholder_free(GDExtensionScriptInstanceDataPtr p_instance) {
	PetPlaceholder *placeholder = self(p_instance);
	placeholder->script->placeholder_freed(placeholder);
	memdelete(placeholder);
}

static GDExtensionScriptInstanceInfo3 build_info() {
	GDExtensionScriptInstanceInfo3 info = {};
	info.set_func = placeholder_set;
	info.get_func = placeholder_get;
	info.get_property_list_func = placeholder_get_property_list;
	info.free_property_list_func = placeholder_free_property_list;
	info.get_property_type_func = placeholder_get_property_type;
	info.get_method_list_func = placeholder_get_method_list;
	info.free_method_list_func = placeholder_free_method_list;
	info.has_method_func = placeholder_has_method;
	info.call_func = placeholder_call;
	info.set_fallback_func = placeholder_set_fallback;
	info.get_fallback_func = placeholder_get_fallback;
	info.refcount_incremented_func = placeholder_refcount_incremented;
	info.refcount_decremented_func = placeholder_refcount_decremented;
	info.get_owner_func = placeholder_get_owner;
	info.get_script_func = placeholder_get_script;
	info.is_placeholder_func = placeholder_is_placeholder;
	info.get_language_func = placeholder_get_language;
	info.free_func = placeholder_free;
	return info;
}

const GDExtensionScriptInstanceInfo3 PetPlaceholder::info = build_info();

} // namespace petrichor
