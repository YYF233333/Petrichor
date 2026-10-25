#pragma once

#include "compiler/engine_api.h"

#include <map>
#include <string>
#include <vector>

namespace petrichor::tests {

using compiler::EngineMethod;
using compiler::EngineParam;
using compiler::Type;
using compiler::TypeKind;

// A few engine classes with hand-written signatures.
class FakeEngine : public compiler::EngineApi {
	std::map<std::string, std::string> parents = {
		{ "Object", "" },
		{ "RefCounted", "Object" },
		{ "Node", "Object" },
		{ "Node2D", "Node" },
	};
	std::map<std::string, std::vector<EngineMethod>> methods;

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
		const Type node = Type{ TypeKind::Object, "Node" };
		methods["Node"] = {
			method("_ready", {}, t(TypeKind::Void), true),
			method("_process", { { "delta", t(TypeKind::Float) } }, t(TypeKind::Void), true),
			method("get_child_count", { { "include_internal", t(TypeKind::Bool) } }, t(TypeKind::Int), false, 1),
			method("set_process", { { "enable", t(TypeKind::Bool) } }, t(TypeKind::Void)),
			method("get_parent", {}, node),
			method("add_child", { { "node", node } }, t(TypeKind::Void)),
		};
	}

	bool class_exists(const std::string &p_class) const override { return parents.count(p_class) > 0; }

	std::string parent_class(const std::string &p_class) const override {
		const auto found = parents.find(p_class);
		return found == parents.end() ? std::string() : found->second;
	}

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

	std::optional<EngineMethod> find_utility(const std::string &p_function) const override {
		if (p_function == "print") {
			EngineMethod m = method("print", {}, t(TypeKind::Void));
			m.is_vararg = true;
			return m;
		}
		return std::nullopt;
	}
};

} // namespace petrichor::tests
