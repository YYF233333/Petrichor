#pragma once

#include "compiler/types.h"

#include <optional>
#include <string>
#include <vector>

namespace petrichor::compiler {

struct EngineParam {
	std::string name;
	Type type;
};

struct EngineMethod {
	std::string name;
	std::vector<EngineParam> params;
	int default_count = 0; // Trailing parameters with default values.
	Type return_type = Type::of(TypeKind::Void);
	bool is_vararg = false; // Extra arguments are Variant.
	bool is_static = false;
	bool is_virtual = false;
};

struct EngineProperty {
	std::string name;
	Type type;
	bool writable = true;
};

// What the compiler knows about the engine's classes (architecture §2.1). The
// editor implements it over ClassDB; tests implement it with a few
// hand-written classes. Built-in types and utility functions come from
// builtin_api.h instead. Implementations must be safe to use from several
// threads.
class EngineApi {
public:
	virtual ~EngineApi() = default;

	virtual bool class_exists(const std::string &p_class) const = 0;
	// Empty for Object.
	virtual std::string parent_class(const std::string &p_class) const = 0;
	virtual bool can_instantiate(const std::string &p_class) const = 0;
	// These search the class and its ancestors.
	virtual std::optional<EngineMethod> find_method(const std::string &p_class, const std::string &p_method) const = 0;
	virtual std::optional<EngineProperty> find_property(const std::string &p_class, const std::string &p_property) const = 0;
	// A signal, as a method whose parameters are the signal's arguments.
	virtual std::optional<EngineMethod> find_signal(const std::string &p_class, const std::string &p_signal) const = 0;

	bool inherits(const std::string &p_class, const std::string &p_ancestor) const {
		for (std::string cls = p_class; !cls.empty(); cls = parent_class(cls)) {
			if (cls == p_ancestor) {
				return true;
			}
		}
		return false;
	}
};

} // namespace petrichor::compiler
