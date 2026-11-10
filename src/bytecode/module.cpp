#include "bytecode/module.h"

#include "bytecode/opcodes.h"
#include "format_id.gen.h"
#include "value_types.gen.h"

#include <algorithm>
#include <cstring>
#include <sstream>

namespace petrichor::bytecode {

namespace {

struct OpInfo {
	const char *name;
	const char *emit;
	const char *verify;
};

const OpInfo OP_INFOS[] = {
#define PETRICHOR_OPCODE_INFO(m_name, m_emit, m_verify) { #m_name, m_emit, m_verify },
	PETRICHOR_OPCODES(PETRICHOR_OPCODE_INFO)
#undef PETRICHOR_OPCODE_INFO
};

} // namespace

int operand_count(Op p_op) {
	if (uint32_t(p_op) >= uint32_t(Op::COUNT)) {
		return 0;
	}
	const char *emit = OP_INFOS[uint32_t(p_op)].emit;
	return emit[0] == '*' ? -1 : int(std::strlen(emit));
}

const char *int_range_name(IntRange p_range) {
	static const char *const names[] = { "int8", "int16", "int32", "uint8", "uint16", "uint32", "char32" };
	static_assert(sizeof(names) / sizeof(names[0]) == size_t(IntRange::COUNT), "one name per range");
	return p_range < IntRange::COUNT ? names[uint32_t(p_range)] : "?";
}

const char *op_name(Op p_op) {
	return uint32_t(p_op) < uint32_t(Op::COUNT) ? OP_INFOS[uint32_t(p_op)].name : "?";
}

const char *op_emit_schema(Op p_op) {
	return uint32_t(p_op) < uint32_t(Op::COUNT) ? OP_INFOS[uint32_t(p_op)].emit : "";
}

const char *op_verify_schema(Op p_op) {
	return uint32_t(p_op) < uint32_t(Op::COUNT) ? OP_INFOS[uint32_t(p_op)].verify : "";
}

static_assert(sizeof(TYPES) / sizeof(TYPES[0]) == size_t(ValueType::Count), "one entry per value type");

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

ValueType packed_element(ValueType p_type) {
	return p_type < ValueType::Count ? TYPES[uint32_t(p_type)].packed_element : ValueType::Void;
}

const ContainerMethod *find_container_method(ValueType p_base, std::string_view p_name) {
	using R = ElementRole;
	static const ContainerMethod methods[] = {
		{ ValueType::Array, "append", { R::Element, R::None } },
		{ ValueType::Array, "push_back", { R::Element, R::None } },
		{ ValueType::Array, "push_front", { R::Element, R::None } },
		{ ValueType::Array, "insert", { R::None, R::Element } },
		{ ValueType::Array, "set", { R::None, R::Element } },
		{ ValueType::Array, "fill", { R::Element, R::None } },
		{ ValueType::Array, "erase", { R::Element, R::None } },
		{ ValueType::Array, "has", { R::Element, R::None } },
		{ ValueType::Array, "count", { R::Element, R::None } },
		{ ValueType::Array, "find", { R::Element, R::None } },
		{ ValueType::Array, "rfind", { R::Element, R::None } },
		{ ValueType::Array, "bsearch", { R::Element, R::None }, R::None, KeyRole::None, Replaced::Bsearch, false, true, false },
		{ ValueType::Array, "bsearch_custom", { R::Element, R::None } },
		{ ValueType::Array, "append_array", { R::Elements, R::None }, R::None, KeyRole::None, Replaced::None, true },
		{ ValueType::Array, "assign", { R::Elements, R::None }, R::None, KeyRole::None, Replaced::None, true },
		{ ValueType::Array, "get", {}, R::Element },
		{ ValueType::Array, "front", {}, R::Element, KeyRole::None, Replaced::None, false, false, true },
		{ ValueType::Array, "back", {}, R::Element, KeyRole::None, Replaced::None, false, false, true },
		{ ValueType::Array, "pick_random", {}, R::Element },
		{ ValueType::Array, "pop_at", {}, R::Element },
		{ ValueType::Array, "pop_front", {}, R::OptionalElement },
		{ ValueType::Array, "pop_back", {}, R::OptionalElement },
		{ ValueType::Array, "min", {}, R::Element, KeyRole::None, Replaced::Min, false, true, true },
		{ ValueType::Array, "max", {}, R::Element, KeyRole::None, Replaced::Max, false, true, true },
		{ ValueType::Array, "sort", {}, R::None, KeyRole::None, Replaced::Sort, false, true, false },
		{ ValueType::Array, "duplicate", {}, R::Entries, KeyRole::None, Replaced::Duplicate },
		{ ValueType::Array, "duplicate_deep", {}, R::Entries, KeyRole::None, Replaced::DuplicateDeep },
		{ ValueType::Array, "slice", {}, R::Entries, KeyRole::None, Replaced::Slice },
		{ ValueType::Array, "filter", {}, R::Entries },
		{ ValueType::Array, "hash", {}, R::None, KeyRole::None, Replaced::Hash },
		{ ValueType::Dictionary, "has", { R::Key, R::None } },
		{ ValueType::Dictionary, "erase", { R::Key, R::None } },
		{ ValueType::Dictionary, "get", { R::Key, R::Value }, R::Value },
		{ ValueType::Dictionary, "set", { R::Key, R::Value }, R::None, KeyRole::Inserts },
		{ ValueType::Dictionary, "get_or_add", { R::Key, R::Value }, R::Value, KeyRole::Inserts },
		{ ValueType::Dictionary, "find_key", { R::Value, R::None }, R::OptionalKey, KeyRole::HandsOut },
		{ ValueType::Dictionary, "has_all", { R::Keys, R::None }, R::None, KeyRole::None, Replaced::HasAll },
		{ ValueType::Dictionary, "merge", { R::Entries, R::None }, R::None, KeyRole::None, Replaced::None, true },
		{ ValueType::Dictionary, "merged", { R::Entries, R::None }, R::Entries, KeyRole::None, Replaced::None, true },
		{ ValueType::Dictionary, "assign", { R::Entries, R::None }, R::None, KeyRole::None, Replaced::None, true },
		{ ValueType::Dictionary, "keys", {}, R::Keys, KeyRole::HandsOut },
		{ ValueType::Dictionary, "values", {}, R::Elements },
		{ ValueType::Dictionary, "duplicate", {}, R::Entries, KeyRole::None, Replaced::Duplicate },
		{ ValueType::Dictionary, "duplicate_deep", {}, R::Entries, KeyRole::None, Replaced::DuplicateDeep },
		{ ValueType::Dictionary, "sort", {}, R::None, KeyRole::None, Replaced::Sort, false, true, false },
		{ ValueType::Dictionary, "hash", {}, R::None, KeyRole::None, Replaced::Hash },
		{ ValueType::Dictionary, "is_same_typed", { R::Entries, R::None } },
		{ ValueType::Dictionary, "is_same_typed_key", { R::Entries, R::None } },
		{ ValueType::Dictionary, "is_same_typed_value", { R::Entries, R::None } },
		{ ValueType::Dictionary, "recursive_equal", { R::Entries, R::None } },
	};
	for (const ContainerMethod &m : methods) {
		if (m.base == p_base && p_name == m.name) {
			return &m;
		}
	}
	return nullptr;
}

ValueType storage_of(const std::vector<TypeDesc> &p_types, uint32_t p_type) {
	if (p_type >= p_types.size()) {
		return ValueType::Void;
	}
	switch (p_types[p_type].kind) {
		case TypeKind::Value:
			return p_types[p_type].value;
		case TypeKind::Object:
			return p_types[p_type].task_result.empty() ? ValueType::Object : ValueType::Task;
		case TypeKind::Array:
			return ValueType::Array;
		case TypeKind::Dictionary:
			return ValueType::Dictionary;
		case TypeKind::Enum:
			return ValueType::Int;
		default:
			return ValueType::Void;
	}
}

// The fields of a type and the T of T? come before it, which the verifier
// checks; until then, sizes and layouts do not follow one that does not.
// is_boxed() follows containers, whose cycles it checks first.
bool has_fields(const std::vector<TypeDesc> &p_types, uint32_t p_type) {
	return p_type < p_types.size() && (p_types[p_type].kind == TypeKind::Struct || p_types[p_type].kind == TypeKind::PayloadEnum);
}

uint32_t type_size(const std::vector<TypeDesc> &p_types, uint32_t p_type) {
	if (p_type < p_types.size() && p_types[p_type].kind == TypeKind::Option && p_types[p_type].element < p_type) {
		return 8 + type_size(p_types, p_types[p_type].element);
	}
	if (has_fields(p_types, p_type)) {
		uint32_t size = 0;
		for (const FieldDesc &field : p_types[p_type].fields) {
			size += field.type < p_type ? type_size(p_types, field.type) : 0;
		}
		return size;
	}
	return slot_size(storage_of(p_types, p_type));
}

std::vector<uint32_t> field_offsets(const std::vector<TypeDesc> &p_types, uint32_t p_type) {
	std::vector<uint32_t> offsets;
	uint32_t offset = 0;
	for (const FieldDesc &field : p_types[p_type].fields) {
		offsets.push_back(offset);
		offset += field.type < p_type ? type_size(p_types, field.type) : 0;
	}
	return offsets;
}

std::string layout_key(const std::vector<TypeDesc> &p_types, uint32_t p_type) {
	if (p_type < p_types.size() && p_types[p_type].kind == TypeKind::Option && p_types[p_type].element < p_type) {
		return "?(" + layout_key(p_types, p_types[p_type].element) + ")";
	}
	if (has_fields(p_types, p_type)) {
		std::string key = "s(";
		for (const FieldDesc &field : p_types[p_type].fields) {
			key += (field.type < p_type ? layout_key(p_types, field.type) : "!") + ",";
		}
		return key + ")";
	}
	return std::to_string(int(storage_of(p_types, p_type)));
}

bool is_boxed(const std::vector<TypeDesc> &p_types, uint32_t p_type) {
	if (p_type >= p_types.size()) {
		return false;
	}
	const TypeDesc &type = p_types[p_type];
	switch (type.kind) {
		case TypeKind::Option:
		case TypeKind::Struct:
		case TypeKind::PayloadEnum:
			return true;
		case TypeKind::Array:
			return is_boxed(p_types, type.element);
		case TypeKind::Dictionary:
			return is_boxed(p_types, type.key) || is_boxed(p_types, type.element);
		default:
			return false;
	}
}

bool holds_references(const std::vector<TypeDesc> &p_types, uint32_t p_type) {
	if (p_type < p_types.size() && p_types[p_type].kind == TypeKind::Option && p_types[p_type].element < p_type) {
		return holds_references(p_types, p_types[p_type].element);
	}
	if (has_fields(p_types, p_type)) {
		return std::any_of(p_types[p_type].fields.begin(), p_types[p_type].fields.end(), [&](const FieldDesc &p_field) { return p_field.type < p_type && holds_references(p_types, p_field.type); });
	}
	switch (storage_of(p_types, p_type)) {
		case ValueType::Object:
		case ValueType::Variant:
		case ValueType::Array:
		case ValueType::Dictionary:
		case ValueType::Callable:
		case ValueType::Task:
			return true;
		default:
			return false;
	}
}

// A type met again inside itself is keyed by how many types out it is.
static std::string type_key(const std::vector<TypeDesc> &p_types, uint32_t p_type, std::vector<uint32_t> &r_path) {
	if (p_type >= p_types.size()) {
		return "!";
	}
	const auto outer = std::find(r_path.begin(), r_path.end(), p_type);
	if (outer != r_path.end()) {
		return "^" + std::to_string(r_path.end() - outer - 1);
	}
	r_path.push_back(p_type);
	const TypeDesc &type = p_types[p_type];
	const auto inner = [&](uint32_t p_inner) { return type_key(p_types, p_inner, r_path); };
	const auto text = [](const std::string &p_text) { return std::to_string(p_text.size()) + ":" + p_text; };
	std::string key;
	switch (type.kind) {
		case TypeKind::Value:
			key = "v" + std::to_string(int(type.value));
			break;
		case TypeKind::Object:
			key = std::string(type.nullable ? "o?" : "o") + text(type.object.name) + text(type.object.script) + text(type.object.interface_name) + text(type.task_result);
			break;
		case TypeKind::Option:
			key = "?(" + inner(type.element) + ")";
			break;
		case TypeKind::Array:
			key = "a(" + inner(type.element) + ")";
			break;
		case TypeKind::Dictionary:
			key = "d(" + inner(type.key) + "," + inner(type.element) + ")";
			break;
		case TypeKind::Struct:
		case TypeKind::PayloadEnum: {
			// With its fields, so that a struct changed in one module is not taken
			// for the old one by another; an enum with its variants too.
			key = (type.kind == TypeKind::Struct ? "s" : "p") + text(type.name) + "(";
			for (const EnumValueDesc &value : type.values) {
				key += text(value.name) + std::to_string(value.value) + ",";
			}
			for (const FieldDesc &field : type.fields) {
				key += text(field.name) + inner(field.type) + ",";
			}
			key += ")";
			break;
		}
		case TypeKind::Enum: {
			// With its values, as a struct with its fields; an enum of the engine,
			// named without "::", is open, and later versions add values (spec §3.7).
			key = "e" + text(type.name) + "(";
			for (const EnumValueDesc &value : type.values) {
				if (type.name.find("::") != std::string::npos) {
					key += text(value.name) + std::to_string(value.value) + ",";
				}
			}
			key += ")";
			break;
		}
		default:
			key = "!";
	}
	r_path.pop_back();
	return key;
}

std::string type_key(const std::vector<TypeDesc> &p_types, uint32_t p_type) {
	std::vector<uint32_t> path;
	return type_key(p_types, p_type, path);
}

std::string signature_key(const std::vector<TypeDesc> &p_types, const std::vector<Slot> &p_params, size_t p_defaults, const Slot &p_result, bool p_async, bool p_vararg) {
	std::string key = std::to_string(p_defaults) + (p_async ? "a" : "s") + (p_vararg ? "v" : "");
	for (const Slot &param : p_params) {
		key += '|' + type_key(p_types, param.type);
	}
	return key + "->" + type_key(p_types, p_result.type);
}

std::string task_result_key(const std::vector<TypeDesc> &p_types, uint32_t p_type) {
	Slot result;
	result.type = p_type;
	return signature_key(p_types, {}, 0, result);
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
	static constexpr bool reading = false;
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
	void str(const std::string &p_value) {
		u32(uint32_t(p_value.size()));
		data.insert(data.end(), p_value.begin(), p_value.end());
	}
};

class Reader {
	const uint8_t *data;
	size_t size;
	size_t at = 0;

public:
	static constexpr bool reading = true;
	int depth = 0;
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
	uint8_t get_u8() {
		return take(1) ? data[at++] : 0;
	}
	uint32_t get_u32() {
		if (!take(4)) {
			return 0;
		}
		uint32_t value = 0;
		for (int i = 0; i < 4; i++) {
			value |= uint32_t(data[at++]) << (8 * i);
		}
		return value;
	}
	uint64_t get_u64() {
		if (!take(8)) {
			return 0;
		}
		uint64_t value = 0;
		for (int i = 0; i < 8; i++) {
			value |= uint64_t(data[at++]) << (8 * i);
		}
		return value;
	}
	void u8(uint8_t &r_value) { r_value = get_u8(); }
	void u32(uint32_t &r_value) { r_value = get_u32(); }
	void u64(uint64_t &r_value) { r_value = get_u64(); }
	void str(std::string &r_value) {
		const uint32_t length = get_u32();
		if (!take(length)) {
			r_value.clear();
			return;
		}
		r_value.assign(reinterpret_cast<const char *>(data + at), length);
		at += length;
	}
	// Counts are bounded by the remaining data, so corrupt input cannot
	// request huge allocations.
	uint32_t count() {
		const uint32_t value = get_u32();
		if (value > size - at) {
			failed = true;
			return 0;
		}
		return value;
	}
};

template <typename Ar, typename... Ts>
void io(Ar &r_ar, Ts &...r_fields);

template <typename Ar, typename T>
void archive(Ar &r_ar, T &r_value) {
	using V = std::remove_const_t<T>;
	if constexpr (std::is_same_v<V, uint8_t>) {
		r_ar.u8(r_value);
	} else if constexpr (std::is_same_v<V, bool>) {
		uint8_t raw = r_value ? 1 : 0;
		r_ar.u8(raw);
		if constexpr (Ar::reading) {
			r_value = raw != 0;
		}
	} else if constexpr (std::is_same_v<V, uint32_t>) {
		r_ar.u32(r_value);
	} else if constexpr (std::is_same_v<V, int32_t>) {
		uint32_t raw = uint32_t(r_value);
		r_ar.u32(raw);
		if constexpr (Ar::reading) {
			r_value = int32_t(raw);
		}
	} else if constexpr (std::is_same_v<V, uint64_t>) {
		r_ar.u64(r_value);
	} else if constexpr (std::is_same_v<V, int64_t>) {
		uint64_t raw = uint64_t(r_value);
		r_ar.u64(raw);
		if constexpr (Ar::reading) {
			r_value = int64_t(raw);
		}
	} else if constexpr (std::is_same_v<V, double>) {
		uint64_t bits = 0;
		if constexpr (!Ar::reading) {
			std::memcpy(&bits, &r_value, sizeof(bits));
		}
		r_ar.u64(bits);
		if constexpr (Ar::reading) {
			std::memcpy(&r_value, &bits, sizeof(r_value));
		}
	} else if constexpr (std::is_same_v<V, std::string>) {
		r_ar.str(r_value);
	} else if constexpr (std::is_same_v<V, ValueType> || std::is_same_v<V, TypeKind>) {
		uint8_t raw = uint8_t(r_value);
		r_ar.u8(raw);
		if constexpr (Ar::reading) {
			if (raw >= uint8_t(V::Count)) {
				r_ar.failed = true;
				r_value = V(0);
			} else {
				r_value = V(raw);
			}
		}
	} else if constexpr (std::is_same_v<V, Slot>) {
		io(r_ar, r_value.name, r_value.type, r_value.offset);
	} else if constexpr (std::is_same_v<V, ClassRef>) {
		io(r_ar, r_value.name, r_value.script, r_value.interface_name);
	} else if constexpr (std::is_same_v<V, FieldDesc>) {
		io(r_ar, r_value.name, r_value.type);
	} else if constexpr (std::is_same_v<V, EnumValueDesc>) {
		io(r_ar, r_value.name, r_value.value);
	} else if constexpr (std::is_same_v<V, TypeDesc>) {
		io(r_ar, r_value.kind, r_value.value, r_value.element, r_value.key, r_value.object, r_value.nullable, r_value.task_result, r_value.name, r_value.fields, r_value.values);
	} else if constexpr (std::is_same_v<V, Constant>) {
		io(r_ar, r_value.type, r_value.b, r_value.i, r_value.f, r_value.s, r_value.builtin, r_value.constructor, r_value.op, r_value.utility, r_value.field, r_value.load, r_value.args);
	} else if constexpr (std::is_same_v<V, Member>) {
		io(r_ar, r_value.name, r_value.type, r_value.offset, r_value.line, r_value.is_pub, r_value.is_export, r_value.is_onready, r_value.must_assign, r_value.setter, r_value.getter, r_value.hint, r_value.hint_string, r_value.usage, r_value.has_initial, r_value.initial);
	} else if constexpr (std::is_same_v<V, SignalInfo>) {
		io(r_ar, r_value.name, r_value.is_pub, r_value.params);
	} else if constexpr (std::is_same_v<V, ExportGroup>) {
		io(r_ar, r_value.name, r_value.prefix, r_value.level, r_value.before_member);
	} else if constexpr (std::is_same_v<V, Rpc>) {
		io(r_ar, r_value.mode, r_value.transfer_mode, r_value.call_local, r_value.channel);
	} else if constexpr (std::is_same_v<V, Preset>) {
		io(r_ar, r_value.offset, r_value.bits);
	} else if constexpr (std::is_same_v<V, Function>) {
		io(r_ar, r_value.name, r_value.is_inherited, r_value.base_signature, r_value.params, r_value.defaults, r_value.given, r_value.is_vararg, r_value.is_async, r_value.is_static, r_value.result, r_value.is_pub, r_value.is_override, r_value.rpc, r_value.is_abstract, r_value.is_lambda, r_value.captures, r_value.line, r_value.frame_size, r_value.slots, r_value.presets, r_value.code);
	} else if constexpr (std::is_same_v<V, TablePrefix>) {
		io(r_ar, r_value.types, r_value.constants, r_value.engine_calls, r_value.properties, r_value.utility_calls, r_value.operators, r_value.fields, r_value.constructors, r_value.builtin_methods, r_value.classes, r_value.method_calls);
	} else if constexpr (std::is_same_v<V, NamedConstant>) {
		io(r_ar, r_value.name, r_value.constant, r_value.is_pub);
	} else if constexpr (std::is_same_v<V, EngineCall>) {
		io(r_ar, r_value.method, r_value.result_type, r_value.static_class, r_value.owner, r_value.hash, r_value.params, r_value.defaults, r_value.return_type, r_value.returns_reference);
	} else if constexpr (std::is_same_v<V, PropertyAccess>) {
		io(r_ar, r_value.name, r_value.type);
	} else if constexpr (std::is_same_v<V, UtilityCall>) {
		io(r_ar, r_value.function, r_value.hash, r_value.is_vararg, r_value.params, r_value.return_type, r_value.result_type);
	} else if constexpr (std::is_same_v<V, OperatorCall>) {
		io(r_ar, r_value.op, r_value.left, r_value.right, r_value.result);
	} else if constexpr (std::is_same_v<V, FieldAccess>) {
		io(r_ar, r_value.base, r_value.name, r_value.type);
	} else if constexpr (std::is_same_v<V, ConstructorCall>) {
		io(r_ar, r_value.type, r_value.index, r_value.params);
	} else if constexpr (std::is_same_v<V, BuiltinMethodCall>) {
		io(r_ar, r_value.base, r_value.method, r_value.hash, r_value.is_vararg, r_value.is_const, r_value.is_static, r_value.checks_elements, r_value.params, r_value.return_type, r_value.result_type);
	} else if constexpr (std::is_same_v<V, MethodCall>) {
		io(r_ar, r_value.method, r_value.params, r_value.defaults, r_value.is_vararg, r_value.is_async, r_value.result, r_value.static_class);
	} else if constexpr (std::is_same_v<V, Module>) {
		if constexpr (Ar::reading) {
			if (r_ar.depth > 64) {
				r_ar.failed = true;
				return;
			}
			r_ar.depth++;
		}
		io(r_ar, r_value.source_hash, r_value.declarations_only, r_value.is_abstract, r_value.global_name, r_value.base_class, r_value.base_script, r_value.inherited_members,
				r_value.types, r_value.members, r_value.signals, r_value.groups, r_value.instance_size, r_value.functions, r_value.init_function, r_value.ready_function,
				r_value.init_tables, r_value.statics, r_value.statics_size, r_value.static_init_function, r_value.static_unload, r_value.named_constants, r_value.constants,
				r_value.engine_calls, r_value.properties, r_value.utility_calls, r_value.operators, r_value.fields, r_value.constructors, r_value.builtin_methods,
				r_value.classes, r_value.method_calls, r_value.interfaces, r_value.name, r_value.is_pub, r_value.inner_classes);
		if constexpr (Ar::reading) {
			const int32_t functions = int32_t(r_value.functions.size());
			if (r_value.init_function < -1 || r_value.init_function >= functions || r_value.ready_function < -1 || r_value.ready_function >= functions || r_value.static_init_function < -1 || r_value.static_init_function >= functions) {
				r_ar.failed = true;
			}
			r_ar.depth--;
		}
	} else {
		if constexpr (Ar::reading) {
			r_value.resize(r_ar.count());
		} else {
			r_ar.u32(uint32_t(r_value.size()));
		}
		for (auto &item : r_value) {
			archive(r_ar, item);
		}
	}
}

template <typename Ar, typename... Ts>
void io(Ar &r_ar, Ts &...r_fields) {
	(archive(r_ar, r_fields), ...);
}

} // namespace

std::vector<uint8_t> serialize(const Module &p_module) {
	Writer w;
	for (uint8_t byte : MAGIC) {
		w.u8(byte);
	}
	w.u64(FORMAT_ID);
	archive(w, p_module);
	return w.data;
}

bool deserialize(const uint8_t *p_data, size_t p_size, Module &r_module, std::string &r_error) {
	Reader r(p_data, p_size);
	for (uint8_t byte : MAGIC) {
		if (r.get_u8() != byte) {
			r_error = "not a compiled Petrichor script";
			return false;
		}
	}
	if (r.get_u64() != FORMAT_ID) {
		r_error = "compiled by a build of Petrichor with another bytecode format";
		return false;
	}
	Module module;
	archive(r, module);
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
	for (const Preset &preset : p_function.presets) {
		out << "preset " << preset.offset << ' ' << preset.bits << '\n';
	}
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
		} else {
			const uint32_t argc = code[pc + 4];
			out << ' ' << code[pc + 1] << ' ' << code[pc + 2] << ' ' << code[pc + 3] << ' ' << argc;
			for (uint32_t i = 0; i < argc; i++) {
				out << " (" << value_type_name(storage_of(p_module.types, code[pc + 5 + 2 * i])) << ' ' << code[pc + 6 + 2 * i] << ')';
			}
		}
		pc += instruction_length(code.data() + pc);
		out << '\n';
	}
	return out.str();
}

} // namespace petrichor::bytecode
