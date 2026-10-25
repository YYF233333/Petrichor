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
	return uint32_t(p_op) < uint32_t(Op::COUNT) ? counts[uint32_t(p_op)] : 0;
}

const char *op_name(Op p_op) {
	static const char *const names[] = {
#define PETRICHOR_OPCODE_NAME(m_name, m_count) #m_name,
		PETRICHOR_OPCODES(PETRICHOR_OPCODE_NAME)
#undef PETRICHOR_OPCODE_NAME
	};
	return uint32_t(p_op) < uint32_t(Op::COUNT) ? names[uint32_t(p_op)] : "?";
}

namespace {

struct TypeInfo {
	const char *name;
	uint32_t size;
};

// In ValueType order. Sizes are the engine's float_64 builtin_class_sizes,
// except Bool (8, like Int) and Object (held in a Variant).
const TypeInfo TYPES[] = {
	{ "void", 0 },
	{ "bool", 8 },
	{ "int", 8 },
	{ "float", 8 },
	{ "String", 8 },
	{ "Vector2", 8 },
	{ "Vector2i", 8 },
	{ "Rect2", 16 },
	{ "Rect2i", 16 },
	{ "Vector3", 12 },
	{ "Vector3i", 12 },
	{ "Transform2D", 24 },
	{ "Vector4", 16 },
	{ "Vector4i", 16 },
	{ "Plane", 16 },
	{ "Quaternion", 16 },
	{ "AABB", 24 },
	{ "Basis", 36 },
	{ "Transform3D", 48 },
	{ "Projection", 64 },
	{ "Color", 16 },
	{ "StringName", 8 },
	{ "NodePath", 8 },
	{ "RID", 8 },
	{ "Object", 24 },
	{ "Callable", 16 },
	{ "Signal", 16 },
	{ "Dictionary", 8 },
	{ "Array", 8 },
	{ "PackedByteArray", 16 },
	{ "PackedInt32Array", 16 },
	{ "PackedInt64Array", 16 },
	{ "PackedFloat32Array", 16 },
	{ "PackedFloat64Array", 16 },
	{ "PackedStringArray", 16 },
	{ "PackedVector2Array", 16 },
	{ "PackedVector3Array", 16 },
	{ "PackedColorArray", 16 },
	{ "PackedVector4Array", 16 },
	{ "Variant", 24 },
};
static_assert(sizeof(TYPES) / sizeof(TYPES[0]) == size_t(ValueType::Count), "one entry per value type");

} // namespace

uint32_t value_size(ValueType p_type) {
	return p_type < ValueType::Count ? TYPES[uint32_t(p_type)].size : 0;
}

uint32_t slot_size(ValueType p_type) {
	return (value_size(p_type) + 7) & ~7u;
}

const char *value_type_name(ValueType p_type) {
	return p_type < ValueType::Count ? TYPES[uint32_t(p_type)].name : "?";
}

ValueType value_type_from_name(const std::string &p_name) {
	for (uint32_t i = 0; i < uint32_t(ValueType::Count); i++) {
		if (p_name == TYPES[i].name) {
			return ValueType(i);
		}
	}
	return ValueType::Count;
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
	void type(ValueType p_type) { u8(uint8_t(p_type)); }
	void types(const std::vector<ValueType> &p_types) {
		u32(uint32_t(p_types.size()));
		for (ValueType value : p_types) {
			type(value);
		}
	}
	void constant(const Constant &p_value) {
		type(p_value.type);
		u8(p_value.b ? 1 : 0);
		u64(uint64_t(p_value.i));
		f64(p_value.f);
		str(p_value.s);
		str(p_value.builtin);
	}
	void slot(const Slot &p_slot) {
		str(p_slot.name);
		type(p_slot.type);
		u32(p_slot.offset);
		str(p_slot.class_name);
		u8(p_slot.nullable ? 1 : 0);
		type(p_slot.option);
		type(p_slot.element);
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
		if (value >= uint8_t(ValueType::Count)) {
			failed = true;
			return ValueType::Void;
		}
		return ValueType(value);
	}
	std::vector<ValueType> types() {
		std::vector<ValueType> value(count());
		for (ValueType &item : value) {
			item = type();
		}
		return value;
	}
	Constant constant() {
		Constant value;
		value.type = type();
		value.b = u8() != 0;
		value.i = int64_t(u64());
		value.f = f64();
		value.s = str();
		value.builtin = str();
		return value;
	}
	Slot slot() {
		Slot value;
		value.name = str();
		value.type = type();
		value.offset = u32();
		value.class_name = str();
		value.nullable = u8() != 0;
		value.option = type();
		value.element = type();
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
	w.u8(p_module.declarations_only ? 1 : 0);
	w.str(p_module.global_name);
	w.str(p_module.base_class);

	w.u32(uint32_t(p_module.members.size()));
	for (const Member &member : p_module.members) {
		w.str(member.name);
		w.type(member.type);
		w.u32(member.offset);
		w.str(member.class_name);
		w.u8(member.nullable ? 1 : 0);
		w.type(member.option);
		w.type(member.element);
		w.u8(member.is_pub ? 1 : 0);
		w.u8(member.is_export ? 1 : 0);
		w.u8(member.is_onready ? 1 : 0);
		w.u32(member.hint);
		w.str(member.hint_string);
		w.u8(member.has_initial ? 1 : 0);
		w.constant(member.initial);
	}
	w.u32(uint32_t(p_module.signals.size()));
	for (const SignalInfo &signal : p_module.signals) {
		w.str(signal.name);
		w.u8(signal.is_pub ? 1 : 0);
		w.u32(uint32_t(signal.params.size()));
		for (const Slot &param : signal.params) {
			w.slot(param);
		}
	}
	w.u32(uint32_t(p_module.groups.size()));
	for (const ExportGroup &group : p_module.groups) {
		w.str(group.name);
		w.str(group.prefix);
		w.u8(group.level);
		w.u32(group.before_member);
	}
	w.u32(p_module.instance_size);

	w.u32(uint32_t(p_module.functions.size()));
	for (const Function &function : p_module.functions) {
		w.str(function.name);
		w.u32(uint32_t(function.params.size()));
		for (const Slot &param : function.params) {
			w.slot(param);
		}
		w.u32(uint32_t(function.defaults.size()));
		for (const Constant &value : function.defaults) {
			w.constant(value);
		}
		w.u8(function.is_static ? 1 : 0);
		w.type(function.return_type);
		w.str(function.return_class);
		w.u8(function.is_pub ? 1 : 0);
		w.u8(function.is_override ? 1 : 0);
		w.u8(function.is_exposed ? 1 : 0);
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

	w.u32(uint32_t(int32_t(p_module.init_function)));
	w.u32(uint32_t(int32_t(p_module.ready_function)));
	const TablePrefix &prefix = p_module.init_tables;
	for (uint32_t count : { prefix.constants, prefix.engine_calls, prefix.properties, prefix.utility_calls, prefix.operators, prefix.fields, prefix.constructors, prefix.builtin_methods, prefix.classes }) {
		w.u32(count);
	}

	w.u32(uint32_t(p_module.constants.size()));
	for (const Constant &constant : p_module.constants) {
		w.constant(constant);
	}
	w.u32(uint32_t(p_module.engine_calls.size()));
	for (const EngineCall &call : p_module.engine_calls) {
		w.str(call.method);
		w.type(call.return_type);
	}
	w.u32(uint32_t(p_module.properties.size()));
	for (const PropertyAccess &property : p_module.properties) {
		w.str(property.name);
		w.type(property.type);
	}
	w.u32(uint32_t(p_module.utility_calls.size()));
	for (const UtilityCall &call : p_module.utility_calls) {
		w.str(call.function);
		w.u32(call.hash);
		w.u8(call.is_vararg ? 1 : 0);
		w.types(call.params);
		w.type(call.return_type);
		w.type(call.result_type);
	}
	w.u32(uint32_t(p_module.operators.size()));
	for (const OperatorCall &call : p_module.operators) {
		w.u8(call.op);
		w.type(call.left);
		w.type(call.right);
		w.type(call.result);
	}
	w.u32(uint32_t(p_module.fields.size()));
	for (const FieldAccess &field : p_module.fields) {
		w.type(field.base);
		w.str(field.name);
		w.type(field.type);
	}
	w.u32(uint32_t(p_module.constructors.size()));
	for (const ConstructorCall &call : p_module.constructors) {
		w.type(call.type);
		w.u32(call.index);
		w.types(call.params);
	}
	w.u32(uint32_t(p_module.builtin_methods.size()));
	for (const BuiltinMethodCall &call : p_module.builtin_methods) {
		w.type(call.base);
		w.str(call.method);
		w.u32(call.hash);
		w.u8(call.is_vararg ? 1 : 0);
		w.types(call.params);
		w.type(call.return_type);
		w.type(call.result_type);
	}
	w.u32(uint32_t(p_module.classes.size()));
	for (const std::string &name : p_module.classes) {
		w.str(name);
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
	module.declarations_only = r.u8() != 0;
	module.global_name = r.str();
	module.base_class = r.str();

	module.members.resize(r.count());
	for (Member &member : module.members) {
		member.name = r.str();
		member.type = r.type();
		member.offset = r.u32();
		member.class_name = r.str();
		member.nullable = r.u8() != 0;
		member.option = r.type();
		member.element = r.type();
		member.is_pub = r.u8() != 0;
		member.is_export = r.u8() != 0;
		member.is_onready = r.u8() != 0;
		member.hint = r.u32();
		member.hint_string = r.str();
		member.has_initial = r.u8() != 0;
		member.initial = r.constant();
	}
	module.signals.resize(r.count());
	for (SignalInfo &signal : module.signals) {
		signal.name = r.str();
		signal.is_pub = r.u8() != 0;
		signal.params.resize(r.count());
		for (Slot &param : signal.params) {
			param = r.slot();
		}
	}
	module.groups.resize(r.count());
	for (ExportGroup &group : module.groups) {
		group.name = r.str();
		group.prefix = r.str();
		group.level = r.u8();
		group.before_member = r.u32();
	}
	module.instance_size = r.u32();

	module.functions.resize(r.count());
	for (Function &function : module.functions) {
		function.name = r.str();
		function.params.resize(r.count());
		for (Slot &param : function.params) {
			param = r.slot();
		}
		function.defaults.resize(r.count());
		for (Constant &value : function.defaults) {
			value = r.constant();
		}
		function.is_static = r.u8() != 0;
		function.return_type = r.type();
		function.return_class = r.str();
		function.is_pub = r.u8() != 0;
		function.is_override = r.u8() != 0;
		function.is_exposed = r.u8() != 0;
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

	module.init_function = int32_t(r.u32());
	module.ready_function = int32_t(r.u32());
	if (module.init_function < -1 || module.init_function >= int32_t(module.functions.size()) || module.ready_function < -1 || module.ready_function >= int32_t(module.functions.size())) {
		r.failed = true;
	}
	TablePrefix &prefix = module.init_tables;
	for (uint32_t *count : { &prefix.constants, &prefix.engine_calls, &prefix.properties, &prefix.utility_calls, &prefix.operators, &prefix.fields, &prefix.constructors, &prefix.builtin_methods, &prefix.classes }) {
		*count = r.u32();
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
	module.properties.resize(r.count());
	for (PropertyAccess &property : module.properties) {
		property.name = r.str();
		property.type = r.type();
	}
	module.utility_calls.resize(r.count());
	for (UtilityCall &call : module.utility_calls) {
		call.function = r.str();
		call.hash = r.u32();
		call.is_vararg = r.u8() != 0;
		call.params = r.types();
		call.return_type = r.type();
		call.result_type = r.type();
	}
	module.operators.resize(r.count());
	for (OperatorCall &call : module.operators) {
		call.op = r.u8();
		call.left = r.type();
		call.right = r.type();
		call.result = r.type();
	}
	module.fields.resize(r.count());
	for (FieldAccess &field : module.fields) {
		field.base = r.type();
		field.name = r.str();
		field.type = r.type();
	}
	module.constructors.resize(r.count());
	for (ConstructorCall &call : module.constructors) {
		call.type = r.type();
		call.index = r.u32();
		call.params = r.types();
	}
	module.builtin_methods.resize(r.count());
	for (BuiltinMethodCall &call : module.builtin_methods) {
		call.base = r.type();
		call.method = r.str();
		call.hash = r.u32();
		call.is_vararg = r.u8() != 0;
		call.params = r.types();
		call.return_type = r.type();
		call.result_type = r.type();
	}
	module.classes.resize(r.count());
	for (std::string &name : module.classes) {
		name = r.str();
	}

	if (r.failed) {
		r_error = "truncated or corrupt compiled script";
		return false;
	}
	std::string problem;
	if (!verify(module, problem)) {
		r_error = "invalid compiled script: " + problem;
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
			const uint32_t argc = code[pc + 4];
			out << ' ' << code[pc + 1] << ' ' << code[pc + 2] << ' ' << code[pc + 3] << ' ' << argc;
			for (uint32_t i = 0; i < argc; i++) {
				out << " (" << value_type_name(ValueType(code[pc + 5 + 2 * i])) << ' ' << code[pc + 6 + 2 * i] << ')';
			}
			pc += 5 + 2 * argc;
		}
		out << '\n';
	}
	return out.str();
}

} // namespace petrichor::bytecode
