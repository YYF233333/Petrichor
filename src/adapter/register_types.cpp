#include "adapter/pet_instance.h"
#include "adapter/pet_language.h"
#include "adapter/pet_resource_format.h"
#include "adapter/pet_script.h"
#include "runtime/task.h"
#include "runtime/values.h"

#ifdef PETRICHOR_PROBES
#include "probe/probe.h"
#include "probe/task.h"
#endif

#ifdef TOOLS_ENABLED
#include "adapter/editor/pet_editor_plugin.h"

#include <godot_cpp/classes/editor_plugin_registration.hpp>
#endif

#include <gdextension_interface.h>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;
using namespace petrichor;

static PetLanguage *language = nullptr;
static Ref<PetResourceLoader> loader;
static Ref<PetResourceSaver> saver;

static void initialize_petrichor(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		init_values();
		GDREGISTER_CLASS(CoroutineTask);
		GDREGISTER_INTERNAL_CLASS(PetLanguage);
		GDREGISTER_CLASS(PetScript);
		GDREGISTER_INTERNAL_CLASS(PetResourceLoader);
		GDREGISTER_INTERNAL_CLASS(PetResourceSaver);
#ifdef PETRICHOR_PROBES
		GDREGISTER_INTERNAL_CLASS(PetTask);
		GDREGISTER_CLASS(PetrichorProbe);
#endif

		language = memnew(PetLanguage);
		Engine::get_singleton()->register_script_language(language);
		set_instance_resolver(resolve_instance);

		loader.instantiate();
		ResourceLoader::get_singleton()->add_resource_format_loader(loader);
		saver.instantiate();
		ResourceSaver::get_singleton()->add_resource_format_saver(saver);
	}

#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		GDREGISTER_INTERNAL_CLASS(PetExportPlugin);
		GDREGISTER_INTERNAL_CLASS(PetEditorPlugin);
		EditorPlugins::add_by_type<PetEditorPlugin>();
	}
#endif
}

static void uninitialize_petrichor(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		ResourceSaver::get_singleton()->remove_resource_format_saver(saver);
		saver.unref();
		ResourceLoader::get_singleton()->remove_resource_format_loader(loader);
		loader.unref();

		tasks::clear();
		set_instance_resolver(nullptr);
		Engine::get_singleton()->unregister_script_language(language);
		memdelete(language);
		language = nullptr;
	}
}

extern "C" {
GDExtensionBool GDE_EXPORT petrichor_library_init(GDExtensionInterfaceGetProcAddress p_get_proc_address, GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization) {
	GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);
	init_obj.register_initializer(initialize_petrichor);
	init_obj.register_terminator(uninitialize_petrichor);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
	return init_obj.init();
}
}
