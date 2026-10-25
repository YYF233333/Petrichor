#pragma once

#include <godot_cpp/classes/resource_format_loader.hpp>
#include <godot_cpp/classes/resource_format_saver.hpp>

namespace petrichor {

// Loads `.pet` sources and `.petc` compiled scripts as PetScript.
class PetResourceLoader : public godot::ResourceFormatLoader {
	GDCLASS(PetResourceLoader, godot::ResourceFormatLoader);

protected:
	static void _bind_methods() {}

public:
	godot::PackedStringArray _get_recognized_extensions() const override;
	bool _handles_type(const godot::StringName &p_type) const override;
	godot::String _get_resource_type(const godot::String &p_path) const override;
	godot::Variant _load(const godot::String &p_path, const godot::String &p_original_path, bool p_use_sub_threads, int32_t p_cache_mode) const override;
};

// Saves PetScript as `.pet` source.
class PetResourceSaver : public godot::ResourceFormatSaver {
	GDCLASS(PetResourceSaver, godot::ResourceFormatSaver);

protected:
	static void _bind_methods() {}

public:
	godot::Error _save(const godot::Ref<godot::Resource> &p_resource, const godot::String &p_path, uint32_t p_flags) override;
	bool _recognize(const godot::Ref<godot::Resource> &p_resource) const override;
	godot::PackedStringArray _get_recognized_extensions(const godot::Ref<godot::Resource> &p_resource) const override;
};

} // namespace petrichor
