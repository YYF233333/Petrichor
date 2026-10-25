#pragma once

#include "compiler/engine_api.h"

namespace petrichor {

// The compiler's view of the engine in the editor, read from ClassDB, so that
// classes of other extensions and newer engines are included (architecture §2.1).
class ClassDBApi : public compiler::EngineApi {
public:
	bool class_exists(const std::string &p_class) const override;
	std::string parent_class(const std::string &p_class) const override;
	bool can_instantiate(const std::string &p_class) const override;
	std::optional<compiler::EngineMethod> find_method(const std::string &p_class, const std::string &p_method) const override;
	std::optional<compiler::EngineProperty> find_property(const std::string &p_class, const std::string &p_property) const override;

	static const ClassDBApi &get();
};

} // namespace petrichor
