#include "runtime/values.h"

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/godot.hpp>

#include <array>
#include <cstring>
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
	GDExtensionPtrDestructor destroy = nullptr; // Null for plain data.
	GDExtensionVariantFromTypeConstructorFunc to_variant = nullptr;
	GDExtensionTypeFromVariantConstructorFunc from_variant = nullptr;
};

const TypeOps &ops(ValueType p_type) {
	// Built on first use: the engine's functions are only available once the
	// library is initialized.
	static const std::array<TypeOps, size_t(ValueType::Count)> table = [] {
		std::array<TypeOps, size_t(ValueType::Count)> result{};
		for (uint32_t i = uint32_t(ValueType::Bool); i < uint32_t(ValueType::Variant); i++) {
			if (ValueType(i) == ValueType::Object) {
				continue;
			}
			const GDExtensionVariantType type = GDExtensionVariantType(i);
			// Constructor 0 takes no arguments and 1 copies, for every built-in type.
			result[i].construct = gdextension_interface::variant_get_ptr_constructor(type, 0);
			result[i].copy = gdextension_interface::variant_get_ptr_constructor(type, 1);
			result[i].destroy = gdextension_interface::variant_get_ptr_destructor(type);
			result[i].to_variant = gdextension_interface::get_variant_from_type_constructor(type);
			result[i].from_variant = gdextension_interface::get_variant_to_type_constructor(type);
		}
		return result;
	}();
	return table[size_t(p_type)];
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

void construct_value(ValueType p_type, uint8_t *p_slot) {
	const uint32_t size = bytecode::value_size(p_type);
	std::memset(p_slot, 0, bytecode::slot_size(p_type));
	if (is_variant_slot(p_type)) {
		new (p_slot) Variant();
	} else if (size > 0 && p_type != ValueType::Bool && p_type != ValueType::Int && p_type != ValueType::Float) {
		ops(p_type).construct(p_slot, nullptr);
	}
}

void destroy_value(ValueType p_type, uint8_t *p_slot) {
	if (is_variant_slot(p_type)) {
		variant_at(p_slot).~Variant();
	} else if (p_type < ValueType::Count && ops(p_type).destroy) {
		ops(p_type).destroy(p_slot);
	}
}

void construct_slots(uint8_t *p_base, uint32_t p_size, const std::vector<bytecode::Slot> &p_slots) {
	std::memset(p_base, 0, p_size);
	for (const bytecode::Slot &slot : p_slots) {
		construct_value(slot.type, p_base + slot.offset);
	}
}

void destroy_slots(uint8_t *p_base, const std::vector<bytecode::Slot> &p_slots) {
	for (const bytecode::Slot &slot : p_slots) {
		destroy_value(slot.type, p_base + slot.offset);
	}
}

void copy_value(ValueType p_type, uint8_t *p_dst, const uint8_t *p_src) {
	if (p_dst == p_src || p_type == ValueType::Void) {
		return;
	}
	if (is_variant_slot(p_type)) {
		variant_at(p_dst) = variant_at(p_src);
		return;
	}
	const TypeOps &type = ops(p_type);
	if (!type.destroy) {
		std::memcpy(p_dst, p_src, bytecode::value_size(p_type));
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

bool from_variant(ValueType p_type, const Variant &p_value, uint8_t *p_slot, const StringName &p_class, bool p_nullable) {
	const Variant::Type have = p_value.get_type();
	switch (p_type) {
		case ValueType::Void:
			return true;
		case ValueType::Variant:
			variant_at(p_slot) = p_value;
			return true;
		case ValueType::Object:
			if (is_none(p_value)) {
				if (!p_nullable) {
					return false;
				}
				variant_at(p_slot) = Variant(); // None in one form.
				return true;
			}
			if (have != Variant::OBJECT) {
				return false;
			}
			if (p_value.booleanize() && p_class != StringName()) {
				const Object *object = p_value;
				if (!object || !object->is_class(p_class)) {
					return false;
				}
			}
			variant_at(p_slot) = p_value;
			return true;
		case ValueType::Bool:
		case ValueType::Int:
			if (have != variant_type(p_type)) {
				return false;
			}
			// The 8-byte slot stays 0 or 1 above the byte the engine writes.
			*reinterpret_cast<uint64_t *>(p_slot) = p_type == ValueType::Bool ? (bool(p_value) ? 1 : 0) : uint64_t(int64_t(p_value));
			return true;
		case ValueType::Float:
			if (!fits(p_type, have)) {
				return false;
			}
			*reinterpret_cast<double *>(p_slot) = double(p_value);
			return true;
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

bool is_none(const Variant &p_value) {
	// The engine passes a null Object* as an OBJECT Variant without an object
	// (core/variant/variant.cpp:2507-2511), not as NIL.
	return p_value.get_type() == Variant::NIL || (p_value.get_type() == Variant::OBJECT && p_value.operator ObjectID().is_null());
}

bool is_live_object(const uint8_t *p_slot) {
	const Variant &value = variant_at(p_slot);
	return value.get_type() == Variant::OBJECT && value.booleanize();
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
