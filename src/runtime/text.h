#pragma once

#include <godot_cpp/variant/string.hpp>

#include <string>

namespace petrichor {

// Text crosses between the engine's String and the UTF-8 of the compiler and
// the bytecode here.

inline godot::String to_godot(const std::string &p_text) {
	return godot::String::utf8(p_text.c_str(), int64_t(p_text.size()));
}

inline std::string to_utf8(const godot::String &p_text) {
	const godot::CharString utf8 = p_text.utf8();
	return std::string(utf8.get_data(), size_t(utf8.length()));
}

} // namespace petrichor
