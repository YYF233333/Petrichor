#pragma once

#include "compiler/analysis.h"

#include <optional>
#include <string>

namespace petrichor::compiler {

// What a name refers to, as the script editor's lookup shows it (spec §13.2):
// where going to its definition leads, and what hover shows.
struct Lookup {
	// As the engine's lookup results (core/object/script_language.h:365-377).
	enum class Kind {
		Location, // A declaration of this language, which has no documentation yet.
		Class,
		Constant,
		Property,
		Method,
		Signal,
		Enum,
		Local,
		LocalConstant,
	};

	Kind kind = Kind::Location;
	// Of a declaration of the engine: the class and the member that its
	// documentation files it under. Of one of GDScript: the member, and no
	// class, as only the engine knows what GDScript names the class.
	std::string class_name;
	std::string member;
	std::string type; // Of a local.
	// The file that declares it and the line, from 1; none for the engine's.
	std::string path;
	int line = 0;
};

// What the name at byte `p_offset` of the script at `p_path` refers to, the
// script compiled from `p_source`; nullopt where nothing is, or what the
// editor cannot show, such as a node of a scene.
std::optional<Lookup> lookup(Analysis &r_analysis, const EngineApi &p_engine, const ProjectApi &p_project, const std::string &p_path, const std::string &p_source, uint32_t p_offset);

} // namespace petrichor::compiler
