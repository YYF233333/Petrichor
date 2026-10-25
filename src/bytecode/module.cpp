#include "bytecode/module.h"

#include "bytecode/opcodes.h"

#include <cstring>
#include <sstream>

namespace petrichor::bytecode {

int operand_count(Op p_op) {
	static const int counts[] = {
#define PETRICHOR_OPCODE_COUNT(m_name, m_count) m_count,
		PETRICHOR_OPCODES(PETRICHOR_OPCODE_COUNT)
#undef PETRICHOR_OPCODE_COUNT
	};
	return counts[uint32_t(p_op)];
}

const char *op_name(Op p_op) {
	static const char *const names[] = {
#define PETRICHOR_OPCODE_NAME(m_name, m_count) #m_name,
		PETRICHOR_OPCODES(PETRICHOR_OPCODE_NAME)
#undef PETRICHOR_OPCODE_NAME
	};
	return uint32_t(p_op) < uint32_t(Op::COUNT) ? names[uint32_t(p_op)] : "?";
}

uint32_t value_size(ValueType p_type) {
	// The engine's String is a single pointer; 8 bytes also covers 32-bit targets.
	return p_type == ValueType::Void ? 0 : 8;
}

const char *value_type_name(ValueType p_type) {
	switch (p_type) {
		case ValueType::Void:
			return "void";
		case ValueType::Bool:
			return "bool";
		case ValueType::Int:
			return "int";
		case ValueType::Float:
			return "float";
		case ValueType::String:
			return "String";
	}
	return "?";
}

int Module::find_function(const std::string &p_name) const {
	for (size_t i = 0; i < functions.size(); i++) {
		if (functions[i].name == p_name) {
			return int(i);
		}
	}
	return -1;
}

int Module::find_member(const std::string &p_name) const {
	for (size_t i = 0; i < members.size(); i++) {
		if (members[i].name == p_name) {
			return int(i);
		}
	}
	return -1;
}

uint64_t hash_source(const std::string &p_utf8) {
	// FNV-1a.
	uint64_t hash = 14695981039346656037ull;
	for (unsigned char c : p_utf8) {
		hash ^= c;
		hash *= 1099511628211ull;
	}
	return hash;
}

/* Serialization */

namespace {

const uint8_t MAGIC[4] = { 'P', 'E', 'T', 'C' };

class Writer {
public:
	std::vector<uint8_t> data;

	void u8(uint8_t p_value) { data.push_back(p_value); }
	void u32(uint32_t p_value) {
		for (int i = 0; i < 4; i++) {
			data.push_back(uint8_t(p_value >> (8 * i)));
		}
	}
	void u64(uint64_t p_value) {
		for (int i = 0; i < 8; i++) {
			data.push_back(uint8_t(p_value >> (8 * i)));
		}
	}
	void f64(double p_value) {
		uint64_t bits;
		std::memcpy(&bits, &p_value, sizeof(bits));
		u64(bits);
	}
	void str(const std::string &p_value) {
		u32(uint32_t(p_value.size()));
		data.insert(data.end(), p_value.begin(), p_value.end());
	}
	void constant(const Constant &p_value) {
		u8(uint8_t(p_value.type));
		u8(p_value.b ? 1 : 0);
		u64(uint64_t(p_value.i));
		f64(p_value.f);
		str(p_value.s);
	}
	void slot(const Slot &p_slot) {
		str(p_slot.name);
		u8(uint8_t(p_slot.type));
		u32(p_slot.offset);
	}
};

class Reader {
	const uint8_t *data;
	size_t size;
	size_t at = 0;

public:
	bool failed = false;

	Reader(const uint8_t *p_data, size_t p_size) :
			data(p_data), size(p_size) {}

	bool take(size_t p_count) {
		if (failed || size - at < p_count) {
			failed = true;
			return false;
		}
		return true;
	}
	uint8_t u8() {
		return take(1) ? data[at++] : 0;
	}
	uint32_t u32() {
		if (!take(4)) {
			return 0;
		}
		uint32_t value = 0;
		for (int i = 0; i < 4; i++) {
			value |= uint32_t(data[at++]) << (8 * i);
		}
		return value;
	}
	uint64_t u64() {
		if (!take(8)) {
			return 0;
		}
		uint64_t value = 0;
		for (int i = 0; i < 8; i++) {
			value |= uint64_t(data[at++]) << (8 * i);
		}
		return value;
	}
	double f64() {
		const uint64_t bits = u64();
		double value;
		std::memcpy(&value, &bits, sizeof(value));
		return value;
	}
	std::string str() {
		const uint32_t length = u32();
		if (!take(length)) {
			return std::string();
		}
		std::string value(reinterpret_cast<const char *>(data + at), length);
		at += length;
		return value;
	}
	// Counts are bounded by the remaining data, so corrupt input cannot
	// request huge allocations.
	uint32_t count() {
		const uint32_t value = u32();
		if (value > size - at) {
			failed = true;
			return 0;
		}
		return value;
	}
	ValueType type() {
		const uint8_t value = u8();
		if (value > uint8_t(ValueType::String)) {
			failed = true;
		}
		return ValueType(value);
	}
	Constant constant() {
		Constant value;
		value.type = type();
		value.b = u8() != 0;
		value.i = int64_t(u64());
		value.f = f64();
		value.s = str();
		return value;
	}
	Slot slot() {
		Slot value;
		value.name = str();
		value.type = type();
		value.offset = u32();
		return value;
	}
};

} // namespace

std::vector<uint8_t> serialize(const Module &p_module) {
	Writer w;
	for (uint8_t byte : MAGIC) {
		w.u8(byte);
	}
	w.u32(FORMAT_VERSION);
	w.u64(p_module.source_hash);
	w.str(p_module.global_name);
	w.str(p_module.base_class);

	w.u32(uint32_t(p_module.members.size()));
	for (const Member &member : p_module.members) {
		w.str(member.name);
		w.u8(uint8_t(member.type));
		w.u32(member.offset);
		w.u8(member.is_pub ? 1 : 0);
		w.u8(member.is_export ? 1 : 0);
		w.constant(member.initial);
	}
	w.u32(p_module.instance_size);

	w.u32(uint32_t(p_module.functions.size()));
	for (const Function &function : p_module.functions) {
		w.str(function.name);
		w.u32(uint32_t(function.params.size()));
		for (const Slot &param : function.params) {
			w.slot(param);
		}
		w.u8(uint8_t(function.return_type));
		w.u8(function.is_pub ? 1 : 0);
		w.u8(function.is_override ? 1 : 0);
		w.u32(function.line);
		w.u32(function.frame_size);
		w.u32(uint32_t(function.slots.size()));
		for (const Slot &slot : function.slots) {
			w.slot(slot);
		}
		w.u32(uint32_t(function.code.size()));
		for (uint32_t word : function.code) {
			w.u32(word);
		}
	}

	w.u32(uint32_t(p_module.constants.size()));
	for (const Constant &constant : p_module.constants) {
		w.constant(constant);
	}
	w.u32(uint32_t(p_module.engine_calls.size()));
	for (const EngineCall &call : p_module.engine_calls) {
		w.str(call.method);
		w.u8(uint8_t(call.return_type));
	}
	w.u32(uint32_t(p_module.utility_calls.size()));
	for (const UtilityCall &call : p_module.utility_calls) {
		w.str(call.function);
		w.u8(uint8_t(call.return_type));
	}
	return w.data;
}

bool deserialize(const uint8_t *p_data, size_t p_size, Module &r_module, std::string &r_error) {
	Reader r(p_data, p_size);
	for (uint8_t byte : MAGIC) {
		if (r.u8() != byte) {
			r_error = "not a compiled Petrichor script";
			return false;
		}
	}
	const uint32_t version = r.u32();
	if (version != FORMAT_VERSION) {
		r_error = "compiled by another version of Petrichor";
		return false;
	}

	Module module;
	module.source_hash = r.u64();
	module.global_name = r.str();
	module.base_class = r.str();

	module.members.resize(r.count());
	for (Member &member : module.members) {
		member.name = r.str();
		member.type = r.type();
		member.offset = r.u32();
		member.is_pub = r.u8() != 0;
		member.is_export = r.u8() != 0;
		member.initial = r.constant();
	}
	module.instance_size = r.u32();

	module.functions.resize(r.count());
	for (Function &function : module.functions) {
		function.name = r.str();
		function.params.resize(r.count());
		for (Slot &param : function.params) {
			param = r.slot();
		}
		function.return_type = r.type();
		function.is_pub = r.u8() != 0;
		function.is_override = r.u8() != 0;
		function.line = r.u32();
		function.frame_size = r.u32();
		function.slots.resize(r.count());
		for (Slot &slot : function.slots) {
			slot = r.slot();
		}
		function.code.resize(r.count());
		for (uint32_t &word : function.code) {
			word = r.u32();
		}
	}

	module.constants.resize(r.count());
	for (Constant &constant : module.constants) {
		constant = r.constant();
	}
	module.engine_calls.resize(r.count());
	for (EngineCall &call : module.engine_calls) {
		call.method = r.str();
		call.return_type = r.type();
	}
	module.utility_calls.resize(r.count());
	for (UtilityCall &call : module.utility_calls) {
		call.function = r.str();
		call.return_type = r.type();
	}

	if (r.failed) {
		r_error = "truncated or corrupt compiled script";
		return false;
	}
	r_module = std::move(module);
	return true;
}

std::string disassemble(const Module &p_module, const Function &p_function) {
	std::ostringstream out;
	const std::vector<uint32_t> &code = p_function.code;
	size_t pc = 0;
	while (pc < code.size()) {
		const Op op = Op(code[pc]);
		out << pc << ": " << op_name(op);
		const int count = operand_count(op);
		if (count >= 0) {
			for (int i = 0; i < count; i++) {
				out << ' ' << code[pc + 1 + i];
			}
			pc += 1 + count;
		} else {
			const uint32_t argc = code[pc + 3];
			out << ' ' << code[pc + 1] << ' ' << code[pc + 2] << ' ' << argc;
			for (uint32_t i = 0; i < argc; i++) {
				out << " (" << value_type_name(ValueType(code[pc + 4 + 2 * i])) << ' ' << code[pc + 5 + 2 * i] << ')';
			}
			pc += 4 + 2 * argc;
		}
		out << '\n';
	}
	return out.str();
}

} // namespace petrichor::bytecode
