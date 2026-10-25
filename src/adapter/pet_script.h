#pragma once

#include <godot_cpp/classes/script_extension.hpp>
#include <godot_cpp/classes/script_language.hpp>
#include <godot_cpp/templates/hash_set.hpp>

#include <mutex>

namespace petrichor {

struct PetInstance;

// Step 1 of the architecture (engine integration) has no compiler: a script
// only reads its base class from `extends`, and every script exposes the same
// hard-coded members, the property `message` and the method `greet()`.
class PetScript : public godot::ScriptExtension {
	GDCLASS(PetScript, godot::ScriptExtension);

	godot::String source;
	godot::StringName base_type;
	bool valid = false;

	mutable std::mutex mutex;
	mutable godot::HashSet<PetInstance *> instances;
	mutable godot::HashSet<void *> placeholders;

	void parse();
	void update_placeholder(void *p_placeholder) const;

protected:
	static void _bind_methods() {}

public:
	// Built on each call: static StringNames would be destroyed after the
	// engine has unloaded the extension.
	static godot::StringName message_name() { return "message"; }
	static godot::StringName greet_name() { return "greet"; }
	static godot::StringName ready_name() { return "_ready"; }
	static godot::String message_default() { return "hello"; }

	godot::String get_script_path() const;

	void set_source(const godot::String &p_source);
	void instance_freed(PetInstance *p_instance);

	bool _editor_can_reload_from_file() override;
	void _placeholder_erased(void *p_placeholder) override;
	bool _can_instantiate() const override;
	godot::Ref<godot::Script> _get_base_script() const override;
	godot::StringName _get_global_name() const override;
	bool _inherits_script(const godot::Ref<godot::Script> &p_script) const override;
	godot::StringName _get_instance_base_type() const override;
	void *_instance_create(godot::Object *p_for_object) const override;
	void *_placeholder_instance_create(godot::Object *p_for_object) const override;
	bool _has_source_code() const override;
	godot::String _get_source_code() const override;
	void _set_source_code(const godot::String &p_code) override;
	godot::Error _reload(bool p_keep_state) override;
	godot::StringName _get_doc_class_name() const override;
	godot::TypedArray<godot::Dictionary> _get_documentation() const override;
	godot::String _get_class_icon_path() const override;
	bool _has_method(const godot::StringName &p_method) const override;
	bool _has_static_method(const godot::StringName &p_method) const override;
	godot::Variant _get_script_method_argument_count(const godot::StringName &p_method) const override;
	godot::Dictionary _get_method_info(const godot::StringName &p_method) const override;
	bool _is_tool() const override;
	bool _is_valid() const override;
	bool _is_abstract() const override;
	godot::ScriptLanguage *_get_language() const override;
	bool _has_script_signal(const godot::StringName &p_signal) const override;
	godot::TypedArray<godot::Dictionary> _get_script_signal_list() const override;
	bool _has_property_default_value(const godot::StringName &p_property) const override;
	godot::Variant _get_property_default_value(const godot::StringName &p_property) const override;
	void _update_exports() override;
	godot::TypedArray<godot::Dictionary> _get_script_method_list() const override;
	godot::TypedArray<godot::Dictionary> _get_script_property_list() const override;
	int32_t _get_member_line(const godot::StringName &p_member) const override;
	godot::Dictionary _get_constants() const override;
	godot::TypedArray<godot::StringName> _get_members() const override;
	bool _is_placeholder_fallback_enabled() const override;
	godot::Variant _get_rpc_config() const override;
	bool _instance_has(godot::Object *p_object) const override;
};

} // namespace petrichor
