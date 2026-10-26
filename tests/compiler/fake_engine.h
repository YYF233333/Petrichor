#pragma once

#include "compiler/engine_api.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace petrichor::tests {

using compiler::EngineMethod;
using compiler::EngineParam;
using compiler::EngineProperty;
using compiler::Type;
using compiler::TypeKind;

// A few engine classes with hand-written signatures, as ClassDB reports them.
class FakeEngine : public compiler::EngineApi {
	std::map<std::string, std::string> parents = {
		{ "Object", "" },
		{ "RefCounted", "Object" },
		{ "Node", "Object" },
		{ "Node2D", "Node" },
		{ "Node3D", "Node" },
		{ "RandomNumberGenerator", "RefCounted" },
		{ "Timer", "Node" },
		{ "Resource", "RefCounted" },
		{ "InputEvent", "Resource" },
		{ "CanvasLayer", "Node" },
		{ "CanvasItem", "Node" },
		{ "Control", "CanvasItem" },
		{ "Label", "Control" },
		{ "TextureRect", "Control" },
		{ "Input", "Object" },
		{ "ResourceLoader", "Object" },
		{ "AudioStream", "Resource" },
		{ "AudioStreamPlayer", "Node" },
		{ "Texture2D", "Resource" },
		{ "PackedScene", "Resource" },
		{ "SceneTree", "Object" },
		{ "Viewport", "Node" },
		{ "Window", "Viewport" },
		{ "Tween", "RefCounted" },
		{ "InputEventMouseMotion", "InputEvent" },
		{ "MeshInstance3D", "Node3D" },
	};
	std::vector<std::string> singletons = { "Input", "ResourceLoader" };
	std::map<std::string, int64_t> constants = {
		{ "Input.MOUSE_MODE_VISIBLE", 0 },
		{ "Input.MOUSE_MODE_CAPTURED", 2 },
		{ "Tween.EASE_OUT_IN", 3 },
	};
	std::map<std::string, std::vector<EngineMethod>> methods;
	std::map<std::string, std::vector<EngineProperty>> properties;
	std::map<std::string, std::vector<EngineMethod>> signals;

	static Type t(TypeKind p_kind) { return Type::of(p_kind); }

	static EngineMethod method(const std::string &p_name, std::vector<EngineParam> p_params, Type p_return, bool p_virtual = false, int p_defaults = 0) {
		EngineMethod m;
		m.name = p_name;
		m.params = std::move(p_params);
		m.return_type = p_return;
		m.is_virtual = p_virtual;
		m.default_count = p_defaults;
		return m;
	}

public:
	FakeEngine() {
		// Objects the engine returns are nullable until audited (spec §10.2).
		const Type node = Type::object("Node", true);
		const Type vector3 = Type::builtin("Vector3");
		methods["Node"] = {
			method("_ready", {}, t(TypeKind::Void), true),
			method("_process", { { "delta", t(TypeKind::Float) } }, t(TypeKind::Void), true),
			method("_input", { { "event", Type::object("InputEvent", true) } }, t(TypeKind::Void), true),
			method("get_child_count", { { "include_internal", t(TypeKind::Bool) } }, t(TypeKind::Int), false, 1),
			method("get_child", { { "idx", t(TypeKind::Int) }, { "include_internal", t(TypeKind::Bool) } }, node, false, 1),
			method("set_process", { { "enable", t(TypeKind::Bool) } }, t(TypeKind::Void)),
			method("get_parent", {}, node),
			method("get_tree", {}, Type::object("SceneTree", true)),
			method("create_tween", {}, Type::object("Tween", true)),
			method("add_child", { { "node", node } }, t(TypeKind::Void)),
			method("set_owner", { { "owner", node } }, t(TypeKind::Void)),
			method("queue_free", {}, t(TypeKind::Void)),
			method("find_children", { { "pattern", t(TypeKind::String) }, { "type", t(TypeKind::String) }, { "recursive", t(TypeKind::Bool) }, { "owned", t(TypeKind::Bool) } }, Type::array(Type::object("Node")), false, 3),
		};
		properties["MeshInstance3D"] = {
			{ "layers", t(TypeKind::Int), true },
		};
		methods["Node3D"] = {
			method("rotate_y", { { "angle", t(TypeKind::Float) } }, t(TypeKind::Void)),
		};
		methods["Input"] = {
			method("is_action_pressed", { { "action", Type::builtin("StringName") }, { "exact_match", t(TypeKind::Bool) } }, t(TypeKind::Bool), false, 1),
		};
		methods["ResourceLoader"] = {
			method("load", { { "path", t(TypeKind::String) }, { "type_hint", t(TypeKind::String) }, { "cache_mode", t(TypeKind::Int) } }, Type::object("Resource", true), false, 2),
		};
		methods["SceneTree"] = {
			method("get_root", {}, Type::object("Window", true)),
			method("reload_current_scene", {}, t(TypeKind::Int)),
		};
		properties["SceneTree"] = {
			{ "root", Type::object("Window", true), false },
		};
		properties["InputEventMouseMotion"] = {
			{ "relative", Type::builtin("Vector2"), true },
		};
		properties["Input"] = {
			{ "mouse_mode", t(TypeKind::Int), true },
		};
		methods["PackedScene"] = {
			method("instantiate", { { "edit_state", t(TypeKind::Int) } }, node, false, 1),
		};
		methods["AudioStreamPlayer"] = {
			method("play", { { "from_position", t(TypeKind::Float) } }, t(TypeKind::Void), false, 1),
		};
		properties["AudioStreamPlayer"] = {
			{ "stream", Type::object("AudioStream", true), true },
			{ "volume_db", t(TypeKind::Float), true },
			{ "bus", Type::builtin("StringName"), true },
		};
		signals["AudioStreamPlayer"] = {
			method("finished", {}, t(TypeKind::Void)),
		};
		methods["RandomNumberGenerator"] = {
			method("randf_range", { { "from", t(TypeKind::Float) }, { "to", t(TypeKind::Float) } }, t(TypeKind::Float)),
			method("randi", {}, t(TypeKind::Int)),
		};
		properties["Node"] = {
			{ "name", Type::builtin("StringName"), true },
		};
		properties["Node3D"] = {
			{ "position", vector3, true },
			{ "rotation", vector3, true },
			{ "transform", Type::builtin("Transform3D"), true },
		};
		properties["RandomNumberGenerator"] = {
			{ "seed", t(TypeKind::Int), true },
		};
		properties["Label"] = {
			{ "text", t(TypeKind::String), true },
		};
		properties["CanvasItem"] = {
			{ "z_index", t(TypeKind::Int), true },
		};
		signals["Node"] = {
			method("ready", {}, t(TypeKind::Void)),
			method("child_entered_tree", { { "node", node } }, t(TypeKind::Void)),
		};
		signals["Timer"] = {
			method("timeout", {}, t(TypeKind::Void)),
		};
	}

	bool class_exists(const std::string &p_class) const override { return parents.count(p_class) > 0; }

	std::string parent_class(const std::string &p_class) const override {
		const auto found = parents.find(p_class);
		return found == parents.end() ? std::string() : found->second;
	}

	bool can_instantiate(const std::string &p_class) const override { return class_exists(p_class) && p_class != "Object"; }

	bool is_singleton(const std::string &p_class) const override {
		return std::find(singletons.begin(), singletons.end(), p_class) != singletons.end();
	}

	std::optional<EngineMethod> find_method(const std::string &p_class, const std::string &p_method) const override {
		for (std::string cls = p_class; !cls.empty(); cls = parent_class(cls)) {
			const auto found = methods.find(cls);
			if (found == methods.end()) {
				continue;
			}
			for (EngineMethod m : found->second) {
				if (m.name == p_method) {
					m.owner = cls;
					return m;
				}
			}
		}
		return std::nullopt;
	}

	std::optional<EngineMethod> find_signal(const std::string &p_class, const std::string &p_signal) const override {
		for (std::string cls = p_class; !cls.empty(); cls = parent_class(cls)) {
			const auto found = signals.find(cls);
			if (found == signals.end()) {
				continue;
			}
			for (const EngineMethod &signal : found->second) {
				if (signal.name == p_signal) {
					return signal;
				}
			}
		}
		return std::nullopt;
	}

	std::optional<EngineProperty> find_property(const std::string &p_class, const std::string &p_property) const override {
		for (std::string cls = p_class; !cls.empty(); cls = parent_class(cls)) {
			const auto found = properties.find(cls);
			if (found == properties.end()) {
				continue;
			}
			for (EngineProperty property : found->second) {
				if (property.name == p_property) {
					property.owner = cls;
					return property;
				}
			}
		}
		return std::nullopt;
	}

	std::optional<int64_t> find_constant(const std::string &p_class, const std::string &p_constant) const override {
		for (std::string cls = p_class; !cls.empty(); cls = parent_class(cls)) {
			const auto found = constants.find(cls + "." + p_constant);
			if (found != constants.end()) {
				return found->second;
			}
		}
		return std::nullopt;
	}
};

} // namespace petrichor::tests
