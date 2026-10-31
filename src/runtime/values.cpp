#include "runtime/values.h"

#include "runtime/method_callable.h"
#include "runtime/task.h"

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <array>
#include <mutex>
#include <new>
#include <unordered_map>

using namespace godot;

namespace petrichor {

using bytecode::ValueType;

namespace {

// Slots that the engine's functions for each type do not handle.
bool is_variant_slot(ValueType p_type) {
	return p_type == ValueType::Object || p_type == ValueType::Variant;
}

/* Objects */

struct ObjectData {
	uint64_t id = 0;
	void *object = nullptr;
};

ObjectData &object_data(uint8_t *p_slot) {
	return *reinterpret_cast<ObjectData *>(p_slot);
}

const ObjectData &object_data(const uint8_t *p_slot) {
	return *reinterpret_cast<const ObjectData *>(p_slot);
}

bool holds_reference(const ObjectData &p_data) {
	return p_data.object && (p_data.id >> 63);
}

// Drops the reference the data holds, as a Variant with it would.
void release(const ObjectData &p_data) {
	if (holds_reference(p_data)) {
		ObjectView view(reinterpret_cast<const uint8_t *>(&p_data));
		gdextension_interface::variant_destroy(view.get()._native_ptr());
	}
}

// Holds the object of an OBJECT Variant, with a reference of its own.
void take_object(uint8_t *p_slot, const Variant &p_value) {
	const ObjectData old = object_data(p_slot);
	alignas(Variant) uint8_t copy[sizeof(Variant)];
	gdextension_interface::variant_new_copy(copy, p_value._native_ptr());
	std::memcpy(p_slot, copy + 8, sizeof(ObjectData));
	release(old);
}

void copy_object(uint8_t *p_dst, const uint8_t *p_src) {
	if (holds_reference(object_data(p_src))) {
		ObjectView view(p_src);
		take_object(p_dst, view.get());
		return;
	}
	const ObjectData old = object_data(p_dst);
	object_data(p_dst) = object_data(p_src);
	release(old);
}

// The engine's functions for one value type, on values in its own layout.
struct TypeOps {
	GDExtensionPtrConstructor construct = nullptr; // Without arguments.
	GDExtensionPtrConstructor copy = nullptr;
	// Null for types that own nothing (core/variant/variant_destruct.cpp:33-57).
	GDExtensionPtrDestructor destroy = nullptr;
	GDExtensionVariantFromTypeConstructorFunc to_variant = nullptr;
	GDExtensionTypeFromVariantConstructorFunc from_variant = nullptr;
	GDExtensionPtrOperatorEvaluator equal = nullptr; // With a value of the same type.
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
			return "Variant";
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
			continue; // A NIL Variant and None are all zero bytes.
		}
		const GDExtensionVariantType engine_type = GDExtensionVariantType(i);
		// Constructor 0 takes no arguments and 1 copies, for every built-in type.
		entry.construct = gdextension_interface::variant_get_ptr_constructor(engine_type, 0);
		entry.copy = gdextension_interface::variant_get_ptr_constructor(engine_type, 1);
		entry.destroy = gdextension_interface::variant_get_ptr_destructor(engine_type);
		entry.to_variant = gdextension_interface::get_variant_from_type_constructor(engine_type);
		entry.from_variant = gdextension_interface::get_variant_to_type_constructor(engine_type);
		entry.equal = gdextension_interface::variant_get_ptr_operator_evaluator(GDEXTENSION_VARIANT_OP_EQUAL, engine_type, engine_type);
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
	if (p_type == ValueType::Object) {
		release(object_data(p_slot));
		object_data(p_slot) = ObjectData();
	} else if (p_type == ValueType::Variant) {
		if (type_of(variant_at(p_slot)) != Variant::NIL) {
			variant_at(p_slot).~Variant();
		}
	} else if (ops(p_type).destroy) {
		ops(p_type).destroy(p_slot);
	}
}

ObjectView::ObjectView(const uint8_t *p_slot) {
	if (p_slot && object_data(p_slot).object) {
		*reinterpret_cast<int32_t *>(bytes) = Variant::OBJECT;
		std::memcpy(bytes + 8, p_slot, sizeof(ObjectData));
	}
}

void set_object(uint8_t *p_slot, const Variant &p_value) {
	if (is_none(p_value) || type_of(p_value) != Variant::OBJECT) {
		destroy_value(ValueType::Object, p_slot);
	} else {
		take_object(p_slot, p_value);
	}
}

bool is_none_object(const uint8_t *p_slot) {
	return !object_data(p_slot).object;
}

// A number for each layout, the same in every image.
static uint32_t layout_id(const std::string &p_key) {
	static std::mutex mutex;
	static std::unordered_map<std::string, uint32_t> ids;
	std::lock_guard<std::mutex> lock(mutex);
	return ids.emplace(p_key, uint32_t(ids.size())).first->second;
}

std::vector<TypeImage> type_images(const std::vector<bytecode::TypeDesc> &p_types) {
	std::vector<TypeImage> images;
	images.reserve(p_types.size());
	for (uint32_t i = 0; i < p_types.size(); i++) {
		const bytecode::TypeDesc &type = p_types[i];
		TypeImage image;
		image.kind = type.kind;
		image.storage = bytecode::storage_of(p_types, i);
		// The verifier has checked that a type inside another comes before it.
		image.element = type.kind == bytecode::TypeKind::Option ? &images[type.element] : nullptr;
		image.size = bytecode::type_size(p_types, i);
		image.layout = layout_id(bytecode::layout_key(p_types, i));
		image.result_key = bytecode::task_result_key(p_types, i);
		ValueCheck &check = image.check;
		// The class of an object, or of the objects in an array.
		const bytecode::TypeDesc &object = type.kind == bytecode::TypeKind::Array ? p_types[type.element] : type;
		if (object.kind == bytecode::TypeKind::Object) {
			check.class_name = to_godot(object.object.name);
			if (object.object.interface_name.empty()) {
				check.script = to_godot(object.object.script);
			} else {
				check.interface = to_godot(object.object.script + "::" + object.object.interface_name);
			}
		}
		check.task_result = type.task_result;
		check.nullable = type.kind != bytecode::TypeKind::Object || type.nullable;
		if (type.kind == bytecode::TypeKind::Array) {
			check.element = bytecode::storage_of(p_types, type.element);
		}
		if (type.kind == bytecode::TypeKind::Struct) {
			image.name = to_godot(type.name);
			const std::vector<uint32_t> offsets = bytecode::field_offsets(p_types, i);
			for (size_t f = 0; f < type.fields.size(); f++) {
				image.fields.push_back({ to_godot(type.fields[f].name), &images[type.fields[f].type], offsets[f] });
			}
		}
		images.push_back(std::move(image));
	}
	return images;
}

void SlotLayout::add(uint32_t p_offset, const TypeImage &p_type) {
	if (p_type.kind == bytecode::TypeKind::Option) {
		add(p_offset + 8, *p_type.element); // The tag is zero: None.
		return;
	}
	if (p_type.kind == bytecode::TypeKind::Struct) {
		for (const FieldImage &field : p_type.fields) {
			add(p_offset + field.offset, *field.type);
		}
		return;
	}
	if (!ops(p_type.storage).zero_default) {
		constructed.emplace_back(p_offset, p_type.storage);
	}
	if (is_variant_slot(p_type.storage) || ops(p_type.storage).destroy) {
		destroyed.emplace_back(p_offset, p_type.storage);
	}
}

/* Values by their type */

static bool is_option(const TypeImage &p_type) {
	return p_type.kind == bytecode::TypeKind::Option;
}

static bool is_struct(const TypeImage &p_type) {
	return p_type.kind == bytecode::TypeKind::Struct;
}

static uint64_t &tag(uint8_t *p_slot) {
	return *reinterpret_cast<uint64_t *>(p_slot);
}

static uint64_t tag(const uint8_t *p_slot) {
	return *reinterpret_cast<const uint64_t *>(p_slot);
}

void construct_value(const TypeImage &p_type, uint8_t *p_slot) {
	if (is_option(p_type)) {
		tag(p_slot) = 0;
		construct_value(*p_type.element, p_slot + 8);
	} else if (is_struct(p_type)) {
		for (const FieldImage &field : p_type.fields) {
			construct_value(*field.type, p_slot + field.offset);
		}
	} else {
		construct_value(p_type.storage, p_slot);
	}
}

void destroy_value(const TypeImage &p_type, uint8_t *p_slot) {
	if (is_option(p_type)) {
		destroy_value(*p_type.element, p_slot + 8);
	} else if (is_struct(p_type)) {
		for (const FieldImage &field : p_type.fields) {
			destroy_value(*field.type, p_slot + field.offset);
		}
	} else {
		destroy_value(p_type.storage, p_slot);
	}
}

void copy_value(const TypeImage &p_type, uint8_t *p_dst, const uint8_t *p_src) {
	if (is_option(p_type)) {
		tag(p_dst) = tag(p_src);
		copy_value(*p_type.element, p_dst + 8, p_src + 8);
	} else if (is_struct(p_type)) {
		for (const FieldImage &field : p_type.fields) {
			copy_value(*field.type, p_dst + field.offset, p_src + field.offset);
		}
	} else {
		copy_value(p_type.storage, p_dst, p_src);
	}
}

Variant to_variant(const TypeImage &p_type, const uint8_t *p_slot) {
	if (is_option(p_type)) {
		return tag(p_slot) ? to_variant(*p_type.element, p_slot + 8) : Variant(); // None is NIL (spec §10.1).
	}
	if (is_struct(p_type)) {
		Dictionary encoded;
		for (const FieldImage &field : p_type.fields) {
			encoded[field.name] = outward_variant(*field.type, p_slot + field.offset);
		}
		return encoded;
	}
	return to_variant(p_type.storage, p_slot);
}

// As `T.try_from` decodes a field (spec §5.3): an int also from a float that
// is exactly an integer within its range, which is how the engine parses
// every number in JSON (§10.8).
static bool decode_field(const TypeImage &p_type, const Variant &p_value, uint8_t *p_slot, bool p_check) {
	if (is_option(p_type) && !is_none(p_value)) {
		if (!decode_field(*p_type.element, p_value, p_slot + 8, p_check)) {
			return false;
		}
		tag(p_slot) = 1;
		return true;
	}
	if (p_type.kind == bytecode::TypeKind::Value && p_type.storage == ValueType::Int && type_of(p_value) == Variant::FLOAT) {
		const double value = p_value;
		if (!(value >= -9223372036854775808.0 && value < 9223372036854775808.0) || value != double(int64_t(value))) {
			return false;
		}
		*reinterpret_cast<int64_t *>(p_slot) = int64_t(value);
		return true;
	}
	return from_variant(p_type, p_value, p_slot, p_check);
}

// The first field of the encoded struct that does not decode, or -1.
static int64_t undecodable_field(const TypeImage &p_type, const Dictionary &p_encoded) {
	alignas(16) uint8_t probe[80];
	for (size_t i = 0; i < p_type.fields.size(); i++) {
		const FieldImage &field = p_type.fields[i];
		if (!p_encoded.has(field.name)) {
			return int64_t(i);
		}
		std::vector<uint64_t> large(field.type->size > sizeof(probe) ? field.type->size / 8 + 1 : 0);
		uint8_t *slot = large.empty() ? probe : reinterpret_cast<uint8_t *>(large.data());
		construct_value(*field.type, slot);
		const bool fits = decode_field(*field.type, p_encoded[field.name], slot, true);
		destroy_value(*field.type, slot);
		if (!fits) {
			return int64_t(i);
		}
	}
	return -1;
}

bool from_variant(const TypeImage &p_type, const Variant &p_value, uint8_t *p_slot, bool p_check) {
	if (is_option(p_type)) {
		if (is_none(p_value)) {
			tag(p_slot) = 0;
			return true;
		}
		if (!from_variant(*p_type.element, p_value, p_slot + 8, p_check)) {
			return false;
		}
		tag(p_slot) = 1;
		return true;
	}
	if (is_struct(p_type)) {
		if (type_of(p_value) != Variant::DICTIONARY) {
			return false;
		}
		const Dictionary encoded = p_value;
		for (const FieldImage &field : p_type.fields) {
			if (!encoded.has(field.name) || !decode_field(*field.type, encoded[field.name], p_slot + field.offset, p_check)) {
				return false;
			}
		}
		return true;
	}
	return from_variant(p_type.storage, p_value, p_slot, p_check ? &p_type.check : nullptr);
}

String type_description(const TypeImage &p_type) {
	if (is_struct(p_type)) {
		return class_description(StringName(), String(), p_type.name);
	}
	return is_option(p_type) ? type_description(*p_type.element) + "?" : type_description(p_type.storage, p_type.check);
}

String entry_mismatch(const TypeImage &p_type, const Variant &p_value) {
	if (is_struct(p_type) && type_of(p_value) == Variant::DICTIONARY) {
		// Which field: "Hit.damage: expected int, found String".
		const Dictionary encoded = p_value;
		const int64_t index = undecodable_field(p_type, encoded);
		if (index >= 0) {
			const FieldImage &field = p_type.fields[index];
			const String where = vformat("%s.%s", type_description(p_type), field.name);
			return encoded.has(field.name) ? vformat("%s: %s", where, entry_mismatch(*field.type, encoded[field.name])) : vformat("%s is missing", where);
		}
	}
	if (is_option(p_type) || is_struct(p_type)) {
		return vformat("expected %s, found %s", type_description(p_type), value_description(p_value));
	}
	return entry_mismatch(p_type.storage, p_value, p_type.check);
}

Variant default_variant(const TypeImage &p_type) {
	if (is_struct(p_type)) {
		std::vector<uint64_t> storage(p_type.size / 8 + 1);
		uint8_t *slot = reinterpret_cast<uint8_t *>(storage.data());
		construct_value(p_type, slot);
		const Variant result = to_variant(p_type, slot);
		destroy_value(p_type, slot);
		return result;
	}
	return is_option(p_type) ? Variant() : default_variant(p_type.storage);
}

Variant::Type variant_type(const TypeImage &p_type) {
	return is_struct(p_type) ? Variant::DICTIONARY : variant_type(p_type.storage);
}

static bool equal_values(ValueType p_type, const uint8_t *p_a, const uint8_t *p_b) {
	switch (p_type) {
		case ValueType::Bool:
		case ValueType::Int:
			return *reinterpret_cast<const uint64_t *>(p_a) == *reinterpret_cast<const uint64_t *>(p_b);
		case ValueType::Float:
			return *reinterpret_cast<const double *>(p_a) == *reinterpret_cast<const double *>(p_b);
		case ValueType::Object:
			return object_data(p_a).id == object_data(p_b).id;
		case ValueType::Variant: {
			Variant result;
			bool valid = false;
			Variant::evaluate(Variant::OP_EQUAL, variant_at(p_a), variant_at(p_b), result, valid);
			return valid && result.booleanize();
		}
		default: {
			alignas(8) uint64_t result = 0;
			ops(p_type).equal(p_a, p_b, &result);
			return result != 0;
		}
	}
}

bool equal_values(const TypeImage &p_type, const uint8_t *p_a, const uint8_t *p_b) {
	if (is_option(p_type)) {
		return tag(p_a) == tag(p_b) && (!tag(p_a) || equal_values(*p_type.element, p_a + 8, p_b + 8));
	}
	if (is_struct(p_type)) {
		for (const FieldImage &field : p_type.fields) {
			if (!equal_values(*field.type, p_a + field.offset, p_b + field.offset)) {
				return false;
			}
		}
		return true;
	}
	return equal_values(p_type.storage, p_a, p_b);
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
	if (p_type == ValueType::Object) {
		copy_object(p_dst, p_src);
		return;
	}
	if (p_type == ValueType::Variant) {
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
	if (p_type == ValueType::Object) {
		ObjectView view(p_slot);
		return view.get();
	}
	if (p_type == ValueType::Variant) {
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
		case ValueType::Variant:
			variant_at(p_slot) = p_value;
			return true;
		case ValueType::Object:
			if (is_none(p_value)) {
				if (p_check && !p_check->nullable) {
					return false;
				}
				destroy_value(ValueType::Object, p_slot);
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
			if (p_check && !p_check->task_result.empty()) {
				const CoroutineTask *task = Object::cast_to<CoroutineTask>(static_cast<Object *>(p_value));
				if (!task || task->result_signature != p_check->task_result) {
					return false;
				}
			}
			take_object(p_slot, p_value);
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
	const ObjectData &data = object_data(p_slot);
	// A reference keeps a RefCounted object alive; the engine finds any other
	// by its ObjectID only while it lives.
	return data.object && (holds_reference(data) || gdextension_interface::object_get_instance_from_id(data.id));
}

Variant outward_variant(const TypeImage &p_type, const uint8_t *p_data) {
	if (p_type.storage == ValueType::Object && p_type.check.nullable && !is_live_object(p_data)) {
		return Variant();
	}
	return to_variant(p_type, p_data);
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
