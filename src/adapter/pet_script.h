#pragma once

#include "runtime/class_image.h"

#include <godot_cpp/classes/script_extension.hpp>
#include <godot_cpp/classes/script_language.hpp>
#include <godot_cpp/templates/hash_set.hpp>

#include <memory>
#include <mutex>

namespace petrichor {

struct PetInstance;

// The class header: `class_name` and `extends`, read without compiling, for
// the editor's global class list.
struct PetHeader {
	godot::String class_name;
	godot::String base = "RefCounted";
};

PetHeader parse_header(const godot::String &p_source);

// A script of this language. Its compiled class (ClassImage) comes from the
// compiler in the editor, and from the bytecode cache or the exported package
// in the game (architecture §2.4).
class PetScript : public godot::ScriptExtension {
	GDCLASS(PetScript, godot::ScriptExtension);

	godot::String source;
	std::shared_ptr<const ClassImage> image; // Null while the script has errors.
	PetHeader header;

	mutable std::mutex mutex;
	mutable godot::HashSet<PetInstance *> instances;
	mutable godot::HashSet<void *> placeholders;

	void update_placeholder(void *p_placeholder) const;
	void set_image(const std::shared_ptr<const ClassImage> &p_image);

protected:
	static void _bind_methods();

public:
	godot::String get_script_path() const;
	std::shared_ptr<const ClassImage> get_image() const;

	// The source as loaded, with its compiled class.
	void set_loaded(const godot::String &p_source, const std::shared_ptr<const ClassImage> &p_image);
	void instance_freed(PetInstance *p_instance);
	// The instance of this script on the object; null for a placeholder.
	PetInstance *find_instance(godot::Object *p_object) const;

	// Members and functions visible to the engine and other languages (spec
	// §10.5): public or exported members; public functions, engine overrides
	// and methods connected in scenes.
	static bool is_visible(const bytecode::Member &p_member) { return p_member.is_pub || p_member.is_export; }
	static bool is_visible(const bytecode::Function &p_function) { return p_function.is_pub || p_function.is_override || p_function.is_exposed; }
	static int find_visible_function(const ClassImage &p_image, const godot::StringName &p_name);

	// GDScript constructs scripts of other languages through a bound `new`, and
	// checks it as the static `_init` (modules/gdscript/gdscript_analyzer.cpp:5958-5961).
	godot::Variant _new(const godot::Variant **p_args, GDExtensionInt p_argcount, GDExtensionCallError &r_error);

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

// Reflection dictionaries for the engine.
godot::Dictionary property_info(const bytecode::Member &p_member);
godot::TypedArray<godot::Dictionary> visible_properties(const ClassImage &p_image);
godot::TypedArray<godot::Dictionary> visible_methods(const ClassImage &p_image);
godot::Variant initial_value(const ClassImage &p_image, const bytecode::Member &p_member);

} // namespace petrichor
