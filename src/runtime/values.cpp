#include "runtime/values.h"

#include "runtime/method_callable.h"

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
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

/* Diagnostics */

// A script by its class_name, or its path when it has none.
String script_name(const Ref<Script> &p_script) {
	return p_script.is_null() ? String() : (p_script->get_global_name().is_empty() ? p_script->get_path() : String(p_script->get_global_name()));
}

// An engine class, a class of this language or an interface ("script::Name")
// as the source names them.
String class_description(const StringName &p_class, const String &p_script, const String &p_interface) {
	if (!p_interface.is_empty()) {
		const int64_t split = p_interface.rfind("::");
		const Ref<Script> owner = ResourceLoader::get_singleton()->call("load", p_interface.substr(0, split));
		const String name = p_interface.substr(split + 2);
		return owner.is_valid() && !owner->get_global_name().is_empty() ? String(owner->get_global_name()) + "." + name : name;
	}
	if (!p_script.is_empty()) {
		return script_name(ResourceLoader::get_singleton()->call("load", p_script));
	}
	return p_class;
}

String type_description(ValueType p_type, const ValueCheck &p_check) {
	switch (p_type) {
		case ValueType::Object: {
			const String name = class_description(p_check.class_name, p_check.script, p_check.interface);
			return p_check.nullable ? name + String("?") : name;
		}
		case ValueType::Variant:
			return p_check.option == ValueType::Void ? String("Variant") : String(bytecode::value_type_name(p_check.option)) + "?";
		case ValueType::Array:
			if (p_check.element == ValueType::Void) {
				return "Array";
			}
			return "Array[" + (p_check.element == ValueType::Object ? class_description(p_check.class_name, p_check.script, p_check.interface) : String(bytecode::value_type_name(p_check.element))) + "]";
		default:
			return bytecode::value_type_name(p_type);
	}
}

String value_description(const Variant &p_value) {
	if (is_none(p_value)) {
		return "None";
	}
	if (type_of(p_value) == Variant::OBJECT) {
		const Object *object = p_value.get_validated_object();
		if (!object) {
			return "a freed object";
		}
		const String name = script_name(object->get_script());
		return name.is_empty() ? String(object->get_class()) : name;
	}
	if (type_of(p_value) == Variant::ARRAY) {
		const Array array = p_value;
		if (!array.is_typed()) {
			return "Array";
		}
		if (array.get_typed_builtin() != Variant::OBJECT) {
			return "Array[" + Variant::get_type_name(Variant::Type(array.get_typed_builtin())) + "]";
		}
		const String name = script_name(array.get_typed_script());
		return "Array[" + (name.is_empty() ? String(array.get_typed_class_name()) : name) + "]";
	}
	return Variant::get_type_name(p_value.get_type());
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
				Object *object = p_value;
				if (!object || !object->is_class(p_check->class_name) || !has_script(object->get_script(), p_check->script) ||
						(!p_check->interface.is_empty() && !implements_interface(object, p_check->interface))) {
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
				// The engine types arrays by class or script only: the elements
				// of an array of an interface are checked one by one.
				for (int64_t i = 0; !p_check->interface.is_empty() && i < array.size(); i++) {
					const Variant element = array[i];
					// A freed or null element is the reader's to report (spec §3.8).
					if (element.booleanize() && !implements_interface(element, p_check->interface)) {
						return false;
					}
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

String entry_mismatch(ValueType p_type, const Variant &p_value, const ValueCheck &p_check) {
	String found = value_description(p_value);
	if (p_type == ValueType::Array && !p_check.interface.is_empty() && type_of(p_value) == Variant::ARRAY) {
		// Typed as the engine types it, with an element that does not implement
		// the interface.
		const Array array = p_value;
		for (int64_t i = 0; i < array.size(); i++) {
			const Variant element = array[i];
			if (element.booleanize() && !implements_interface(element, p_check.interface)) {
				found = vformat("%s at index %d", value_description(element), i);
				break;
			}
		}
	}
	return vformat("expected %s, found %s", type_description(p_type, p_check), found);
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

Array typed_array(ValueType p_element, const StringName &p_class, const String &p_script) {
	Array array;
	if (p_element != ValueType::Void) {
		Variant script;
		if (!p_script.is_empty()) {
			script = ResourceLoader::get_singleton()->call("load", p_script);
		}
		array.set_typed(uint32_t(variant_type(p_element)), p_class, script);
	}
	return array;
}

Variant::Type variant_type(ValueType p_type) {
	if (p_type == ValueType::Variant || p_type >= ValueType::Count) {
		return Variant::NIL;
	}
	return Variant::Type(uint32_t(p_type));
}

bool has_script(const Variant &p_script, const String &p_path) {
	if (p_path.is_empty()) {
		return true;
	}
	for (Ref<Script> script = p_script; script.is_valid(); script = script->get_base_script()) {
		if (script->get_path() == p_path) {
			return true;
		}
	}
	return false;
}

bool is_live_object(const uint8_t *p_slot) {
	const Variant &value = variant_at(p_slot);
	return type_of(value) == Variant::OBJECT && object_id_of(value) != 0 && value.booleanize();
}

Variant outward_variant(const bytecode::Slot &p_slot, const uint8_t *p_data) {
	if (p_slot.type == ValueType::Object && p_slot.nullable && !is_live_object(p_data)) {
		return Variant();
	}
	return to_variant(p_slot.type, p_data);
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
