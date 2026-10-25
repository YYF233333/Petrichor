#pragma once

#include "compiler/compiler.h"
#include "fake_engine.h"

#include <string>
#include <vector>

namespace petrichor::tests {

inline compiler::CompileResult compile(const std::string &p_source) {
	static const FakeEngine engine;
	return compiler::compile(p_source, engine);
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

// A function body in a class that extends Node.
inline std::string in_function(const std::string &p_body, const std::string &p_signature = "func f() -> void:") {
	std::string source = "extends Node\n\n" + p_signature + "\n";
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

} // namespace petrichor::tests
