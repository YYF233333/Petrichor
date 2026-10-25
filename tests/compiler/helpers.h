#pragma once

#include "compiler/compiler.h"
#include "fake_engine.h"

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

	std::vector<compiler::Scene> scenes_with_script(const std::string &p_script_path) const override {
		std::vector<compiler::Scene> result;
		for (const compiler::Scene &scene : scenes) {
			for (const compiler::SceneNode &node : scene.nodes) {
				if (node.script == p_script_path) {
					result.push_back(scene);
					break;
				}
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

// Whether some error message contains `p_text`.
inline bool has_error(const std::string &p_source, const std::string &p_text) {
	for (const std::string &error : errors_of(p_source)) {
		if (error.find(p_text) != std::string::npos) {
			return true;
		}
	}
	return false;
}

// A function body in a class that extends Node (or `p_base`).
inline std::string in_function(const std::string &p_body, const std::string &p_signature = "func f() -> void:", const std::string &p_base = "Node") {
	std::string source = "extends " + p_base + "\n\n" + p_signature + "\n";
	size_t start = 0;
	while (start <= p_body.size()) {
		const size_t end = p_body.find('\n', start);
		source += "\t" + p_body.substr(start, end == std::string::npos ? std::string::npos : end - start) + "\n";
		if (end == std::string::npos) {
			break;
		}
		start = end + 1;
	}
	return source;
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
