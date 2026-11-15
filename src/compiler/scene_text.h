#pragma once

#include "compiler/references.h"

#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace petrichor::compiler {

// A change to a file that names the text it replaces, which whoever applies it
// checks first (architecture §2.1).
struct CheckedEdit {
	uint32_t begin = 0;
	uint32_t end = 0;
	std::string old_text;
	std::string new_text;
};

// A place in the text of a scene or a resource file that stores a name
// (architecture §6.4).
struct StoredName {
	enum class Kind {
		Key, // A key of a node or a resource, or a String down its value.
		Connection, // The signal or the method of a connection.
		Path, // The first subname of a track's or a synchronized property's NodePath.
		Method, // The method of a key of a method track.
		Class, // The script_class of a resource file's header.
		Stale, // A key of a node or a resource that must not be stored, the new name.
	};
	Kind kind = Kind::Key;
	bool node = false;
	std::string section; // A node's path, or a resource's id.
	std::string key;
	std::vector<int> items; // For a Key, down its value; for a Method, the key's index.
	SceneConnection connection;

	bool operator<(const StoredName &p_other) const;
	std::string shown() const; // As refusals name it.
};

// Where the names that a reference of a scene's check stands for are stored,
// with the files: the scene that stores the key or the connection, or the
// files of the animations and synchronization configurations.
std::vector<std::pair<std::string, StoredName>> stored_names(const Reference &p_reference);

// Where the text of a scene or a resource file, in the engine's text format,
// stores `p_names`, each `p_old` (architecture §6.4): a connection's signal if
// `p_signal`, else its method. Each becomes `p_new` as the engine writes it
// there, and so does a node's key where its header lists it in node_paths.
// `error` says what keeps it from it at `line`, from 1, or 0 for the whole
// file:
// - a name found not once, or that is not `p_old`;
// - a Stale key that is stored, or a struct field renamed beside one named
//   `p_new`, whose value would then take effect.
struct LocatedNames {
	std::vector<CheckedEdit> edits; // By offset.
	std::string error;
	int line = 0;
};
LocatedNames locate_names(const std::string &p_text, const std::set<StoredName> &p_names, bool p_signal, const std::string &p_old, const std::string &p_new);

} // namespace petrichor::compiler
