#pragma once

#include "compiler/project_api.h"

#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace petrichor::compiler {

// What a name refers to, the same in every file (architecture §2.1).
struct DeclId {
	enum class Kind {
		// A class of this language by its path; with a name, one of its
		// members, "Hit.damage" for a field of a struct and "Damageable.take"
		// for a member of an interface.
		Script,
		Foreign, // The same of a class of another language, such as GDScript.
		// An engine class or built-in type; with a name, one of its members.
		// Utility functions belong to "@GlobalScope", GDScript's own
		// constants and functions to "@GDScript".
		Engine,
		Language, // A member of a type of this language, such as unwrap of T?.
		Local, // A local or a parameter: the file's path and the offset of its declaration.
		Node, // A node: the scene's path and the node's path.
		Autoload, // By its global name.
		Unresolved, // A name nothing was found for.
	};

	Kind kind = Kind::Unresolved;
	std::string scope;
	std::string name;

	bool operator==(const DeclId &p_other) const {
		return kind == p_other.kind && scope == p_other.scope && name == p_other.name;
	}
	bool operator!=(const DeclId &p_other) const { return !(*this == p_other); }
	bool operator<(const DeclId &p_other) const {
		return std::tie(kind, scope, name) < std::tie(p_other.kind, p_other.scope, p_other.name);
	}
};

// What a declaration declares, as the editor's lookup tells them apart
// (core/object/script_language.h:365-377): a struct or an interface is a
// class, a field a property, and a value of an enum a constant.
enum class SymbolKind {
	Class,
	Constant,
	Property,
	Method,
	Signal,
	Enum,
	Local,
	LocalConstant,
};

// Where a name is resolved, recorded by the checker for the file it checks
// (architecture §2.1).
struct Reference {
	DeclId target;
	std::optional<SymbolKind> declares; // For a declaration: what it declares.
	// For a declaration that overrides or implements another: that one.
	std::optional<DeclId> overrides;
	std::string type; // For the declaration of a local: its type, which hover shows.
	// The name's bytes in the source, or none for a reference in a scene.
	uint32_t begin = 0;
	uint32_t end = 0;
	// A method named by a signal connection of a scene that attaches the
	// script (spec §7.7): the scene's path, and the connection.
	std::string scene;
	SceneConnection connection;
};

} // namespace petrichor::compiler
