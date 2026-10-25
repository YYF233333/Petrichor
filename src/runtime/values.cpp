#include "runtime/values.h"

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/godot.hpp>

#include <array>
#include <new>

using namespace godot;

namespace petrichor {

using bytecode::ValueType;

namespace {

bool is_variant_slot(ValueType p_type) {
	return p_type == ValueType::Object || p_type == ValueType::Variant;
}

// The engine's functions for one value type, on values in its own layout.
struct TypeOps {
	GDExtensionPtrConstructor construct = nullptr; // Without arguments.
	GDExtensionPtrConstructor copy = nullptr;
	// Null for types that own nothing (core/variant/variant_destruct.cpp:33-57).
	GDExtensionPtrDestructor destroy = nullptr;
	GDExtensionVariantFromTypeConstructorFunc to_variant = nullptr;
	GDExtensionTypeFromVariantConstructorFunc from_variant = nullptr;
	uint32_t size = 0;
	bool zero_default = true; // The default value is all zero bytes.
};

// Function pointers only, so a plain global (godot-cpp values cannot be).
std::array<TypeOps, size_t(ValueType::Count)> type_ops;

const TypeOps &ops(ValueType p_type) {
	return type_ops[size_t(p_type)];
}

// A class of this language is its script; with no script asked for, any will do.
bool has_script(const Variant &p_script, const String &p_path) {
	if (p_path.is_empty()) {
		return true;
	}
	const Ref<Script> script = p_script;
	return script.is_valid() && script->get_path() == p_path;
}

// A Variant converts String and StringName into each other, like GDScript (spec §10.7).
bool fits(ValueType p_type, Variant::Type p_have) {
	const Variant::Type want = variant_type(p_type);
	if (p_have == want) {
		return true;
	}
	switch (p_type) {
		case ValueType::Float:
			return p_have == Variant::INT; // Spec §3.2.
		case ValueType::String:
			return p_have == Variant::STRING_NAME;
		case ValueType::StringName:
			return p_have == Variant::STRING;
		default:
			return false;
	}
}

} // namespace

void init_values() {
	for (uint32_t i = 0; i < uint32_t(ValueType::Count); i++) {
		const ValueType type = ValueType(i);
		TypeOps &entry = type_ops[i];
		entry.size = bytecode::value_size(type);
		if (type == ValueType::Void || is_variant_slot(type)) {
			continue; // A NIL Variant is all zero bytes.
		}
		const GDExtensionVariantType engine_type = GDExtensionVariantType(i);
		// Constructor 0 takes no arguments and 1 copies, for every built-in type.
		entry.construct = gdextension_interface::variant_get_ptr_constructor(engine_type, 0);
		entry.copy = gdextension_interface::variant_get_ptr_constructor(engine_type, 1);
		entry.destroy = gdextension_interface::variant_get_ptr_destructor(engine_type);
		entry.to_variant = gdextension_interface::get_variant_from_type_constructor(engine_type);
		entry.from_variant = gdextension_interface::get_variant_to_type_constructor(engine_type);
		// Asked of the engine rather than listed: Basis and Color, say, are not zero.
		alignas(16) uint8_t probe[64] = {};
		entry.construct(probe, nullptr);
		for (uint32_t b = 0; b < entry.size; b++) {
			entry.zero_default = entry.zero_default && probe[b] == 0;
		}
		if (entry.destroy) {
			entry.destroy(probe);
		}
	}
}

void construct_value(ValueType p_type, uint8_t *p_slot) {
	std::memset(p_slot, 0, bytecode::slot_size(p_type));
	if (!ops(p_type).zero_default) {
		ops(p_type).construct(p_slot, nullptr);
	}
}

void destroy_value(ValueType p_type, uint8_t *p_slot) {
	if (is_variant_slot(p_type)) {
		if (type_of(variant_at(p_slot)) != Variant::NIL) {
			variant_at(p_slot).~Variant();
		}
	} else if (ops(p_type).destroy) {
		ops(p_type).destroy(p_slot);
	}
}

SlotLayout SlotLayout::of(uint32_t p_size, const std::vector<bytecode::Slot> &p_slots) {
	SlotLayout layout;
	layout.size = p_size;
	for (const bytecode::Slot &slot : p_slots) {
		if (!ops(slot.type).zero_default) {
			layout.constructed.emplace_back(slot.offset, slot.type);
		}
		if (is_variant_slot(slot.type) || ops(slot.type).destroy) {
			layout.destroyed.emplace_back(slot.offset, slot.type);
		}
	}
	return layout;
}

void SlotLayout::construct(uint8_t *p_base) const {
	std::memset(p_base, 0, size);
	for (const auto &[offset, type] : constructed) {
		ops(type).construct(p_base + offset, nullptr);
	}
}

void SlotLayout::destroy(uint8_t *p_base) const {
	for (const auto &[offset, type] : destroyed) {
		destroy_value(type, p_base + offset);
	}
}

void copy_value(ValueType p_type, uint8_t *p_dst, const uint8_t *p_src) {
	if (p_dst == p_src) {
		return;
	}
	if (is_variant_slot(p_type)) {
		variant_at(p_dst) = variant_at(p_src);
		return;
	}
	const TypeOps &type = ops(p_type);
	if (!type.destroy) {
		std::memcpy(p_dst, p_src, type.size);
		return;
	}
	type.destroy(p_dst);
	const GDExtensionConstTypePtr args[] = { p_src };
	type.copy(p_dst, args);
}

Variant to_variant(ValueType p_type, const uint8_t *p_slot) {
	if (is_variant_slot(p_type)) {
		return variant_at(p_slot);
	}
	Variant result;
	if (p_type != ValueType::Void) {
		// Writes into the still empty Variant.
		ops(p_type).to_variant(result._native_ptr(), const_cast<uint8_t *>(p_slot));
	}
	return result;
}

bool from_variant(ValueType p_type, const Variant &p_value, uint8_t *p_slot, const ValueCheck *p_check) {
	const Variant::Type have = type_of(p_value);
	switch (p_type) {
		case ValueType::Void:
			return true;
		case ValueType::Variant: {
			const ValueType option = p_check ? p_check->option : ValueType::Void;
			if (option != ValueType::Void && !is_none(p_value)) {
				// T? of a value type holds a T, converted as for a T slot.
				alignas(16) uint8_t storage[64];
				construct_value(option, storage);
				const bool ok = from_variant(option, p_value, storage);
				if (ok) {
					variant_at(p_slot) = to_variant(option, storage);
				}
				destroy_value(option, storage);
				return ok;
			}
			variant_at(p_slot) = option != ValueType::Void ? Variant() : p_value; // None in one form.
			return true;
		}
		case ValueType::Object:
			if (is_none(p_value)) {
				if (p_check && !p_check->nullable) {
					return false;
				}
				variant_at(p_slot) = Variant(); // None in one form.
				return true;
			}
			if (have != Variant::OBJECT) {
				return false;
			}
			if (p_check && !p_check->class_name.is_empty() && p_value.booleanize()) {
				const Object *object = p_value;
				if (!object || !object->is_class(p_check->class_name) || !has_script(object->get_script(), p_check->script)) {
					return false;
				}
			}
			variant_at(p_slot) = p_value;
			return true;
		case ValueType::Array:
			if (p_check && p_check->element != ValueType::Void && have == Variant::ARRAY) {
				// Array[T] is the engine's array typed with T, and does not vary
				// with T (spec §3.8).
				const Array array = p_value;
				if (!array.is_typed() || array.get_typed_builtin() != int64_t(variant_type(p_check->element)) ||
						(p_check->element == ValueType::Object && (array.get_typed_class_name() != p_check->class_name || !has_script(array.get_typed_script(), p_check->script)))) {
					return false;
				}
			}
			break;
		case ValueType::Bool:
			if (have != Variant::BOOL) {
				return false;
			}
			// The 8-byte slot stays 0 or 1 above the byte the engine keeps.
			*reinterpret_cast<uint64_t *>(p_slot) = *data_of(p_value) ? 1 : 0;
			return true;
		case ValueType::Int:
			if (have != Variant::INT) {
				return false;
			}
			std::memcpy(p_slot, data_of(p_value), 8);
			return true;
		case ValueType::Float:
			if (have == Variant::FLOAT) {
				std::memcpy(p_slot, data_of(p_value), 8);
				return true;
			}
			if (have == Variant::INT) {
				*reinterpret_cast<double *>(p_slot) = double(*reinterpret_cast<const int64_t *>(data_of(p_value)));
				return true;
			}
			return false;
		default:
			break;
	}
	if (!fits(p_type, have)) {
		return false;
	}
	// The engine constructs into memory that holds no value
	// (core/variant/variant_internal.h:1009-1012).
	const TypeOps &type = ops(p_type);
	if (type.destroy) {
		type.destroy(p_slot);
	}
	if (have != variant_type(p_type)) {
		// String to StringName or back: the engine's conversion.
		const Variant converted = p_type == ValueType::String ? Variant(String(p_value)) : Variant(StringName(p_value));
		type.from_variant(p_slot, const_cast<GDExtensionVariantPtr>(converted._native_ptr()));
		return true;
	}
	type.from_variant(p_slot, const_cast<GDExtensionVariantPtr>(p_value._native_ptr()));
	return true;
}

Variant default_variant(ValueType p_type) {
	if (is_variant_slot(p_type) || p_type == ValueType::Void) {
		return Variant();
	}
	alignas(16) uint8_t storage[64];
	construct_value(p_type, storage);
	Variant result = to_variant(p_type, storage);
	destroy_value(p_type, storage);
	return result;
}

Variant::Type variant_type(ValueType p_type) {
	if (p_type == ValueType::Variant || p_type >= ValueType::Count) {
		return Variant::NIL;
	}
	return Variant::Type(uint32_t(p_type));
}

bool is_live_object(const uint8_t *p_slot) {
	const Variant &value = variant_at(p_slot);
	return type_of(value) == Variant::OBJECT && object_id_of(value) != 0 && value.booleanize();
}

bool check_value_sizes(String &r_error) {
	// The engine's sizes for this build against the compiler's float_64 sizes.
	static const struct {
		ValueType type;
		size_t size;
	} sizes[] = {
		{ ValueType::String, sizeof(String) },
		{ ValueType::StringName, sizeof(StringName) },
		{ ValueType::Variant, sizeof(Variant) },
		{ ValueType::Vector3, sizeof(Vector3) },
		{ ValueType::Transform3D, sizeof(Transform3D) },
		{ ValueType::Projection, sizeof(Projection) },
		{ ValueType::Array, sizeof(Array) },
		{ ValueType::Callable, sizeof(Callable) },
		{ ValueType::PackedVector3Array, sizeof(PackedVector3Array) },
	};
	for (const auto &entry : sizes) {
		if (entry.size > bytecode::value_size(entry.type)) {
			r_error = vformat("%s takes %d bytes in this build, more than the %d the bytecode assumes.", bytecode::value_type_name(entry.type), int64_t(entry.size), bytecode::value_size(entry.type));
			return false;
		}
	}
	return true;
}

} // namespace petrichor
