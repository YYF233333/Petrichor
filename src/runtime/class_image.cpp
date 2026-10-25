#include "runtime/class_image.h"

#include <cstring>
#include <new>

using namespace godot;

namespace petrichor {

using bytecode::ValueType;

const std::vector<std::pair<const char *, Utility>> &utility_functions() {
	static const std::vector<std::pair<const char *, Utility>> table = {
		{ "print", Utility::Print },
		{ "prints", Utility::Prints },
		{ "printerr", Utility::Printerr },
		{ "push_error", Utility::PushError },
		{ "push_warning", Utility::PushWarning },
	};
	return table;
}

static String from_utf8(const std::string &p_text) {
	return String::utf8(p_text.c_str(), int64_t(p_text.size()));
}

std::shared_ptr<const ClassImage> ClassImage::load(bytecode::Module &&p_module, const String &p_path, String &r_error) {
	auto image = std::make_shared<ClassImage>();
	image->module = std::move(p_module);
	image->path = p_path;
	image->source = p_path;
	const bytecode::Module &module = image->module;
	image->global_name = from_utf8(module.global_name);
	image->base_class = from_utf8(module.base_class);

	for (size_t i = 0; i < module.functions.size(); i++) {
		const StringName name = from_utf8(module.functions[i].name);
		image->function_names.push_back(name);
		image->function_lookup.insert(name, int(i));
	}
	for (size_t i = 0; i < module.members.size(); i++) {
		const StringName name = from_utf8(module.members[i].name);
		image->member_names.push_back(name);
		image->member_slots.push_back({ module.members[i].name, module.members[i].type, module.members[i].offset });
		image->member_lookup.insert(name, int(i));
	}
	for (const bytecode::Constant &constant : module.constants) {
		image->strings.push_back(constant.type == ValueType::String ? from_utf8(constant.s) : String());
	}
	for (const bytecode::EngineCall &call : module.engine_calls) {
		image->engine_methods.push_back(from_utf8(call.method));
	}
	for (const bytecode::UtilityCall &call : module.utility_calls) {
		bool found = false;
		for (const auto &entry : utility_functions()) {
			if (call.function == entry.first) {
				image->utilities.push_back(entry.second);
				found = true;
				break;
			}
		}
		if (!found) {
			r_error = "unknown utility function " + from_utf8(call.function);
			return nullptr;
		}
	}
	return image;
}

int ClassImage::find_function(const StringName &p_name) const {
	const int *index = function_lookup.getptr(p_name);
	return index ? *index : -1;
}

int ClassImage::find_member(const StringName &p_name) const {
	const int *index = member_lookup.getptr(p_name);
	return index ? *index : -1;
}

static bool same_constant(const bytecode::Constant &p_a, const bytecode::Constant &p_b) {
	return p_a.type == p_b.type && p_a.b == p_b.b && p_a.i == p_b.i && p_a.f == p_b.f && p_a.s == p_b.s;
}

static bool same_slot(const bytecode::Slot &p_a, const bytecode::Slot &p_b) {
	return p_a.name == p_b.name && p_a.type == p_b.type;
}

bool ClassImage::same_declarations(const ClassImage &p_other) const {
	const bytecode::Module &a = module;
	const bytecode::Module &b = p_other.module;
	if (a.global_name != b.global_name || a.base_class != b.base_class || a.instance_size != b.instance_size || a.members.size() != b.members.size() || a.functions.size() != b.functions.size()) {
		return false;
	}
	for (size_t i = 0; i < a.members.size(); i++) {
		const bytecode::Member &x = a.members[i];
		const bytecode::Member &y = b.members[i];
		if (x.name != y.name || x.type != y.type || x.offset != y.offset || x.is_pub != y.is_pub || x.is_export != y.is_export || !same_constant(x.initial, y.initial)) {
			return false;
		}
	}
	// Signatures; the line, the frame and the code belong to the body.
	for (size_t i = 0; i < a.functions.size(); i++) {
		const bytecode::Function &x = a.functions[i];
		const bytecode::Function &y = b.functions[i];
		if (x.name != y.name || x.return_type != y.return_type || x.is_pub != y.is_pub || x.is_override != y.is_override || x.params.size() != y.params.size()) {
			return false;
		}
		for (size_t p = 0; p < x.params.size(); p++) {
			if (!same_slot(x.params[p], y.params[p])) {
				return false;
			}
		}
	}
	return true;
}

/* Values */

void construct_slots(uint8_t *p_base, uint32_t p_size, const std::vector<bytecode::Slot> &p_slots) {
	std::memset(p_base, 0, p_size);
	for (const bytecode::Slot &slot : p_slots) {
		if (slot.type == ValueType::String) {
			new (p_base + slot.offset) String();
		}
	}
}

void destroy_slots(uint8_t *p_base, const std::vector<bytecode::Slot> &p_slots) {
	for (const bytecode::Slot &slot : p_slots) {
		if (slot.type == ValueType::String) {
			reinterpret_cast<String *>(p_base + slot.offset)->~String();
		}
	}
}

void copy_value(ValueType p_type, uint8_t *p_dst, const uint8_t *p_src) {
	if (p_type == ValueType::String) {
		*reinterpret_cast<String *>(p_dst) = *reinterpret_cast<const String *>(p_src);
	} else if (p_type != ValueType::Void) {
		std::memcpy(p_dst, p_src, 8);
	}
}

Variant to_variant(ValueType p_type, const uint8_t *p_slot) {
	switch (p_type) {
		case ValueType::Bool:
			return *reinterpret_cast<const uint64_t *>(p_slot) != 0;
		case ValueType::Int:
			return *reinterpret_cast<const int64_t *>(p_slot);
		case ValueType::Float:
			return *reinterpret_cast<const double *>(p_slot);
		case ValueType::String:
			return *reinterpret_cast<const String *>(p_slot);
		case ValueType::Void:
			break;
	}
	return Variant();
}

bool from_variant(ValueType p_type, const Variant &p_value, uint8_t *p_slot) {
	const Variant::Type type = p_value.get_type();
	switch (p_type) {
		case ValueType::Bool:
			if (type != Variant::BOOL) {
				return false;
			}
			*reinterpret_cast<uint64_t *>(p_slot) = bool(p_value) ? 1 : 0;
			return true;
		case ValueType::Int:
			if (type != Variant::INT) {
				return false;
			}
			*reinterpret_cast<int64_t *>(p_slot) = int64_t(p_value);
			return true;
		case ValueType::Float:
			// An int is accepted where a float is expected (spec §3.2).
			if (type != Variant::FLOAT && type != Variant::INT) {
				return false;
			}
			*reinterpret_cast<double *>(p_slot) = double(p_value);
			return true;
		case ValueType::String:
			if (type != Variant::STRING && type != Variant::STRING_NAME) {
				return false;
			}
			*reinterpret_cast<String *>(p_slot) = String(p_value);
			return true;
		case ValueType::Void:
			return true;
	}
	return false;
}

Variant default_variant(ValueType p_type) {
	switch (p_type) {
		case ValueType::Bool:
			return false;
		case ValueType::Int:
			return int64_t(0);
		case ValueType::Float:
			return 0.0;
		case ValueType::String:
			return String();
		case ValueType::Void:
			break;
	}
	return Variant();
}

Variant::Type variant_type(ValueType p_type) {
	switch (p_type) {
		case ValueType::Bool:
			return Variant::BOOL;
		case ValueType::Int:
			return Variant::INT;
		case ValueType::Float:
			return Variant::FLOAT;
		case ValueType::String:
			return Variant::STRING;
		case ValueType::Void:
			break;
	}
	return Variant::NIL;
}

} // namespace petrichor
