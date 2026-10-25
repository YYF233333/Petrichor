#pragma once

#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

namespace petrichor {

// Exported games contain only compiled scripts (spec §1.3). Until the compiler
// produces bytecode, a compiled file is a header followed by the source.
godot::PackedByteArray encode_compiled(const godot::String &p_source);
bool decode_compiled(const godot::PackedByteArray &p_bytes, godot::String &r_source);

} // namespace petrichor
