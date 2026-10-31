#pragma once

#include "bytecode/opcodes.h"
#include "compiler/compiler.h"
#include "compiler/script_classes.h"
#include "doctest.h"
#include "fake_engine.h"

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace petrichor::tests {

inline const FakeEngine &fake_engine() {
	static const FakeEngine engine;
	return engine;
}

inline compiler::CompileResult compile(const std::string &p_source) {
	return compiler::compile(p_source, fake_engine());
}

// Scenes described by hand, for the scene model (spec §7).
class FakeProject : public compiler::ProjectApi {
public:
	std::vector<compiler::Scene> scenes;
	std::map<std::string, std::string> sources; // By res:// path.
	std::map<std::string, std::string> global_classes; // class_name to path.
	std::map<std::string, std::string> autoloads; // Global name to path.
	std::map<std::string, compiler::ForeignClass> foreign; // Scripts of other languages, by path.

	// Adds a script of this language, with its class_name if it has one.
	void add_script(const std::string &p_path, const std::string &p_source, const std::string &p_class_name = std::string()) {
		sources[p_path] = p_source;
		if (!p_class_name.empty()) {
			global_classes[p_class_name] = p_path;
		}
	}

	// Whether the script is `p_base` or extends it.
	bool extends_script(const std::string &p_script, const std::string &p_base) const {
		compiler::ScriptClasses classes(fake_engine(), this);
		std::set<std::string> seen;
		for (std::string path = p_script; !path.empty() && seen.insert(path).second;) {
			if (path == p_base) {
				return true;
			}
			const auto *header = classes.header(path);
			path = header ? classes.base_path(header->decl, path) : std::string();
		}
		return false;
	}

	std::vector<compiler::Scene> scenes_with(bool (*p_uses)(const FakeProject &, const compiler::SceneNode &, const std::string &), const std::string &p_script_path) const {
		std::vector<compiler::Scene> result;
		for (const compiler::Scene &scene : scenes) {
			if (std::any_of(scene.nodes.begin(), scene.nodes.end(), [&](const compiler::SceneNode &p_node) { return p_uses(*this, p_node, p_script_path); })) {
				result.push_back(scene);
			}
		}
		return result;
	}

	std::vector<compiler::Scene> scenes_with_script(const std::string &p_script_path) const override {
		return scenes_with([](const FakeProject &p_project, const compiler::SceneNode &p_node, const std::string &p_script) {
			return p_project.extends_script(p_node.script, p_script);
		},
				p_script_path);
	}

	std::vector<compiler::Scene> scenes_instancing_script(const std::string &p_script_path) const override {
		return scenes_with([](const FakeProject &p_project, const compiler::SceneNode &p_node, const std::string &p_script) {
			return p_node.is_instance && p_node.script.empty() && p_project.extends_script(p_node.instance_script, p_script);
		},
				p_script_path);
	}

	std::optional<compiler::Scene> scene(const std::string &p_path) const override {
		for (const compiler::Scene &scene : scenes) {
			if (scene.path == p_path) {
				return scene;
			}
		}
		return std::nullopt;
	}

	std::string global_class_path(const std::string &p_name) const override {
		const auto found = global_classes.find(p_name);
		return found == global_classes.end() ? std::string() : found->second;
	}

	std::string autoload_path(const std::string &p_name) const override {
		const auto found = autoloads.find(p_name);
		return found == autoloads.end() ? std::string() : found->second;
	}

	std::optional<std::string> script_source(const std::string &p_path) const override {
		const auto found = sources.find(p_path);
		return found == sources.end() ? std::nullopt : std::optional<std::string>(found->second);
	}

	// As the editor answers: without loading the script, only what the global
	// class list says.
	std::optional<compiler::ForeignClass> foreign_class(const std::string &p_path, bool p_members) const override {
		const auto found = foreign.find(p_path);
		if (found == foreign.end()) {
			return std::nullopt;
		}
		compiler::ForeignClass result = found->second;
		result.members_known = p_members;
		if (!p_members) {
			result.properties.clear();
			result.methods.clear();
			result.signals.clear();
			const bool named = std::any_of(global_classes.begin(), global_classes.end(), [&](const auto &p_entry) { return p_entry.second == p_path; });
			if (!named) {
				result.native.clear();
			}
		}
		return result;
	}
};

// Compiles res://test.pet, used by the scenes of `p_project`.
inline compiler::CompileResult compile_in(const std::string &p_source, const FakeProject &p_project, bool p_declarations_only = false) {
	compiler::CompileOptions options;
	options.path = "res://test.pet";
	options.project = &p_project;
	options.declarations_only = p_declarations_only;
	return compiler::compile(p_source, fake_engine(), options);
}

inline std::string messages(const compiler::CompileResult &p_result) {
	std::string text;
	for (const compiler::Diagnostic &d : p_result.errors) {
		text += std::to_string(d.line) + ":" + std::to_string(d.column) + ": " + d.message + "\n";
	}
	return text;
}

// Each error as "line:column: message".
inline std::vector<std::string> errors_of(const std::string &p_source) {
	std::vector<std::string> result;
	for (const compiler::Diagnostic &d : compile(p_source).errors) {
		result.push_back(std::to_string(d.line) + ":" + std::to_string(d.column) + ": " + d.message);
	}
	return result;
}

inline std::string joined(const std::vector<std::string> &p_lines) {
	std::string result;
	for (const std::string &line : p_lines) {
		result += line + "\n";
	}
	return result;
}

inline bool contains(const std::string &p_text, const std::string &p_part) {
	return p_text.find(p_part) != std::string::npos;
}

// Whether some error message contains `p_text`.
inline bool has_error(const std::string &p_source, const std::string &p_text) {
	for (const std::string &error : errors_of(p_source)) {
		if (contains(error, p_text)) {
			return true;
		}
	}
	return false;
}

// Each line of `p_text` indented by a tab, as the block of a function.
inline std::string indented(const std::string &p_text) {
	std::string result;
	size_t start = 0;
	while (start <= p_text.size()) {
		const size_t end = p_text.find('\n', start);
		result += "\t" + p_text.substr(start, end == std::string::npos ? std::string::npos : end - start) + "\n";
		if (end == std::string::npos) {
			break;
		}
		start = end + 1;
	}
	return result;
}

// A function body in a class that extends Node (or `p_base`).
inline std::string in_function(const std::string &p_body, const std::string &p_signature = "func f() -> void:", const std::string &p_base = "Node") {
	return "extends " + p_base + "\n\n" + p_signature + "\n" + indented(p_body);
}

// The index of the function of a module with this name, or -1.
inline int function_index(const bytecode::Module &p_module, const std::string &p_name) {
	for (size_t i = 0; i < p_module.functions.size(); i++) {
		if (p_module.functions[i].name == p_name) {
			return int(i);
		}
	}
	return -1;
}

// The function of a module with this name, which it must have.
template <typename T>
auto &function_named(T &p_module, const std::string &p_name) {
	const int index = function_index(p_module, p_name);
	REQUIRE_MESSAGE(index >= 0, p_name);
	return p_module.functions[index];
}

// The position of the first instruction of a function with this opcode.
inline size_t find_instruction(const bytecode::Function &p_function, bytecode::Op p_op) {
	size_t pc = 0;
	while (pc < p_function.code.size()) {
		if (bytecode::Op(p_function.code[pc]) == p_op) {
			return pc;
		}
		pc += bytecode::instruction_length(p_function.code.data() + pc);
	}
	FAIL("no such instruction");
	return 0;
}

// How a slot of a type of the module holds its value.
inline bytecode::ValueType storage(const bytecode::Module &p_module, uint32_t p_type) {
	return bytecode::storage_of(p_module.types, p_type);
}

// The index of the value type in the module's type table, added when missing.
inline uint32_t value_type_index(bytecode::Module &r_module, bytecode::ValueType p_type) {
	bytecode::TypeDesc type;
	type.value = p_type;
	for (uint32_t i = 0; i < r_module.types.size(); i++) {
		if (r_module.types[i] == type) {
			return i;
		}
	}
	r_module.types.push_back(type);
	return uint32_t(r_module.types.size() - 1);
}

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
inline std::string type_of(const std::string &p_expr, const std::string &p_setup = "") {
	const std::string body = (p_setup.empty() ? "" : p_setup + "\n") + "var probe: bool = " + p_expr;
	const std::vector<std::string> errors = errors_of(in_function(body, "func f() -> void:", "Node3D"));
	if (errors.empty()) {
		return "bool";
	}
	const std::string prefix = "Cannot assign a value of type ";
	for (const std::string &error : errors) {
		const size_t start = error.find(prefix);
		const size_t end = error.find(" as bool");
		if (start != std::string::npos && end != std::string::npos) {
			return error.substr(start + prefix.size(), end - start - prefix.size());
		}
	}
	return "error: " + errors[0];
}

} // namespace petrichor::tests
