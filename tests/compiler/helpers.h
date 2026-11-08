#pragma once

#include "bytecode/opcodes.h"
#include "compiler/compiler.h"
#include "doctest.h"

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace petrichor::tests {

const compiler::EngineApi &fake_engine();

// Compiles a source; when it compiles, also checks its references and cuts it
// line by line (architecture §3).
compiler::CompileResult compile(const std::string &p_source);

// Scenes described by hand, for the scene model (spec §7).
class FakeProject : public compiler::ProjectApi {
public:
	std::vector<compiler::Scene> scenes;
	std::map<std::string, std::string> sources; // By res:// path.
	std::map<std::string, std::string> global_classes; // class_name to path.
	std::map<std::string, std::string> autoloads; // Global name to path.
	std::map<std::string, compiler::ForeignClass> foreign; // Scripts of other languages, by path.
	std::map<std::string, compiler::ResourceInfo> resources; // Resources other than scenes, by path.
	std::map<std::string, std::string> uids; // uid:// to res://.
	std::map<std::string, std::vector<std::string>> loads; // What dependencies() lists, by path.
	std::vector<std::string> files; // What resource_files() lists.
	std::vector<std::string> actions; // Input actions.

	// Adds a script of this language, with its class_name if it has one.
	void add_script(const std::string &p_path, const std::string &p_source, const std::string &p_class_name = std::string());

	// Whether the script is `p_base` or extends it.
	bool extends_script(const std::string &p_script, const std::string &p_base) const;

	std::vector<compiler::Scene> scenes_with(bool (*p_uses)(const FakeProject &, const compiler::SceneNode &, const std::string &), const std::string &p_script_path) const;

	std::vector<compiler::Scene> scenes_with_script(const std::string &p_script_path) const override;
	std::vector<compiler::Scene> scenes_instancing_script(const std::string &p_script_path) const override;
	std::optional<compiler::Scene> scene(const std::string &p_path) const override;
	std::optional<compiler::ResourceInfo> resource(const std::string &p_path) const override;
	std::string uid_path(const std::string &p_uid) const override;
	std::vector<std::string> dependencies(const std::string &p_path) const override {
		const auto found = loads.find(p_path);
		return found == loads.end() ? std::vector<std::string>() : found->second;
	}
	std::string global_class_path(const std::string &p_name) const override;
	std::string autoload_path(const std::string &p_name) const override;
	std::map<std::string, std::string> global_class_paths() const override { return global_classes; }
	std::vector<std::string> autoload_names() const override;
	std::vector<std::string> resource_files() const override { return files; }
	std::vector<std::string> input_actions() const override { return actions; }
	std::optional<std::string> script_source(const std::string &p_path) const override;
	std::optional<compiler::ForeignClass> foreign_class(const std::string &p_path, bool p_members) const override;
};

// Compiles res://test.pet, used by the scenes of `p_project`.
compiler::CompileResult compile_in(const std::string &p_source, const FakeProject &p_project, bool p_declarations_only = false);

std::string messages(const compiler::CompileResult &p_result);

// Each error as "line:column: message".
std::vector<std::string> errors_of(const std::string &p_source);

std::string joined(const std::vector<std::string> &p_lines);

bool contains(const std::string &p_text, const std::string &p_part);

// Whether some error message contains `p_text`.
bool has_error(const std::string &p_source, const std::string &p_text);

// Each line of `p_text` indented by a tab, as the block of a function.
std::string indented(const std::string &p_text);

// A function body in a class that extends Node (or `p_base`).
std::string in_function(const std::string &p_body, const std::string &p_signature = "func f() -> void:", const std::string &p_base = "Node");

// The index of the function of a module with this name, or -1.
int function_index(const bytecode::Module &p_module, const std::string &p_name);

// The function of a module with this name, which it must have.
template <typename T>
auto &function_named(T &p_module, const std::string &p_name) {
	const int index = function_index(p_module, p_name);
	REQUIRE_MESSAGE(index >= 0, p_name);
	return p_module.functions[index];
}

// The position of the first instruction of a function with this opcode.
size_t find_instruction(const bytecode::Function &p_function, bytecode::Op p_op);

// How a slot of a type of the module holds its value.
bytecode::ValueType storage(const bytecode::Module &p_module, uint32_t p_type);

// The index of the value type in the module's type table, added when missing.
uint32_t value_type_index(bytecode::Module &r_module, bytecode::ValueType p_type);

// The member of a module with this name, which it must have.
template <typename T>
auto &member_named(T &p_module, const std::string &p_name) {
	size_t index = 0;
	while (index < p_module.members.size() && p_module.members[index].name != p_name) {
		index++;
	}
	REQUIRE_MESSAGE(index < p_module.members.size(), p_name);
	return p_module.members[index];
}

// The type the checker gives an expression in a function of a Node3D class,
// after `p_setup`: read from the error of assigning it to a bool. Another
// error comes back as "error: " and its message.
std::string type_of(const std::string &p_expr, const std::string &p_setup = "");

} // namespace petrichor::tests
