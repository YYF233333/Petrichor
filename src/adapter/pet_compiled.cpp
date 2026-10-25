#include "adapter/pet_compiled.h"

using namespace godot;

namespace petrichor {

static const uint8_t MAGIC[4] = { 'P', 'E', 'T', 'C' };
static const uint32_t FORMAT_VERSION = 0;
static const int64_t HEADER_SIZE = 8;

PackedByteArray encode_compiled(const String &p_source) {
	PackedByteArray body = p_source.to_utf8_buffer();
	PackedByteArray bytes;
	bytes.resize(HEADER_SIZE);
	for (int i = 0; i < 4; i++) {
		bytes.set(i, MAGIC[i]);
	}
	bytes.encode_u32(4, FORMAT_VERSION);
	bytes.append_array(body);
	return bytes;
}

bool decode_compiled(const PackedByteArray &p_bytes, String &r_source) {
	if (p_bytes.size() < HEADER_SIZE) {
		return false;
	}
	for (int i = 0; i < 4; i++) {
		if (p_bytes[i] != MAGIC[i]) {
			return false;
		}
	}
	if (p_bytes.decode_u32(4) != FORMAT_VERSION) {
		return false;
	}
	r_source = p_bytes.slice(HEADER_SIZE).get_string_from_utf8();
	return true;
}

} // namespace petrichor
