#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace petrichor::bytecode {

// The compiled form of one source file: the contract between the compiler and
// the runtime. It only needs to be readable by the same build that wrote it.
constexpr uint32_t FORMAT_VERSION = 1;

// How a value is stored in a frame or an instance. Every slot is 8-byte
// aligned; Bool, Int and Float take 8 bytes, String is the engine's String.
enum class ValueType : uint8_t {
	Void,
	Bool,
	Int,
	Float,
	String,
};

uint32_t value_size(ValueType p_type);
const char *value_type_name(ValueType p_type);

struct Constant {
	ValueType type = ValueType::Void;
	bool b = false;
	int64_t i = 0;
	double f = 0.0;
	std::string s;
};

struct Slot {
	std::string name; // Empty for temporaries.
	ValueType type = ValueType::Void;
	uint32_t offset = 0;
};

struct Member {
	std::string name;
	ValueType type = ValueType::Void;
	uint32_t offset = 0;
	bool is_pub = false;
	bool is_export = false;
	Constant initial;
};

struct Function {
	std::string name;
	std::vector<Slot> params;
	ValueType return_type = ValueType::Void;
	bool is_pub = false;
	bool is_override = false; // Overrides an engine virtual method.
	uint32_t line = 0;
	uint32_t frame_size = 0;
	std::vector<Slot> slots; // All slots with their types, params first.
	std::vector<uint32_t> code;
};

// A method of the engine called on self.
struct EngineCall {
	std::string method;
	ValueType return_type = ValueType::Void;
};

// A global utility function of the engine, such as print.
struct UtilityCall {
	std::string function;
	ValueType return_type = ValueType::Void;
};

struct Module {
	uint64_t source_hash = 0;
	std::string global_name;
	std::string base_class;
	std::vector<Member> members;
	uint32_t instance_size = 0;
	std::vector<Function> functions;
	std::vector<Constant> constants;
	std::vector<EngineCall> engine_calls;
	std::vector<UtilityCall> utility_calls;

	int find_function(const std::string &p_name) const;
	int find_member(const std::string &p_name) const;
};

uint64_t hash_source(const std::string &p_utf8);

std::vector<uint8_t> serialize(const Module &p_module);
// Returns false and sets r_error when the data is not a module of this format.
bool deserialize(const uint8_t *p_data, size_t p_size, Module &r_module, std::string &r_error);

// A readable listing, for tests and debugging.
std::string disassemble(const Module &p_module, const Function &p_function);

} // namespace petrichor::bytecode
