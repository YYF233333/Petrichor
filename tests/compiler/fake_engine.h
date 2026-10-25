#pragma once

#include "compiler/engine_api.h"

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
			method("get_child_count", { { "include_internal", t(TypeKind::Bool) } }, t(TypeKind::Int), false, 1),
			method("set_process", { { "enable", t(TypeKind::Bool) } }, t(TypeKind::Void)),
			method("get_parent", {}, node),
			method("add_child", { { "node", node } }, t(TypeKind::Void)),
			method("queue_free", {}, t(TypeKind::Void)),
		};
		methods["Node3D"] = {
			method("rotate_y", { { "angle", t(TypeKind::Float) } }, t(TypeKind::Void)),
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

	std::optional<EngineMethod> find_method(const std::string &p_class, const std::string &p_method) const override {
		for (std::string cls = p_class; !cls.empty(); cls = parent_class(cls)) {
			const auto found = methods.find(cls);
			if (found == methods.end()) {
				continue;
			}
			for (const EngineMethod &m : found->second) {
				if (m.name == p_method) {
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
			for (const EngineProperty &property : found->second) {
				if (property.name == p_property) {
					return property;
				}
			}
		}
		return std::nullopt;
	}
};

} // namespace petrichor::tests
