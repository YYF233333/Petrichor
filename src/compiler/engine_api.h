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

// What the compiler knows about the engine (architecture §2.1). The editor
// implements it over ClassDB; tests implement it with a few hand-written
// classes. Implementations must be safe to use from several threads.
class EngineApi {
public:
	virtual ~EngineApi() = default;

	virtual bool class_exists(const std::string &p_class) const = 0;
	// Empty for Object.
	virtual std::string parent_class(const std::string &p_class) const = 0;
	// Searches the class and its ancestors.
	virtual std::optional<EngineMethod> find_method(const std::string &p_class, const std::string &p_method) const = 0;
	virtual std::optional<EngineMethod> find_utility(const std::string &p_function) const = 0;
};

} // namespace petrichor::compiler
