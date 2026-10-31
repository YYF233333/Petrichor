#include "runtime/values.h"

#include "runtime/method_callable.h"
#include "runtime/task.h"

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/templates/hashfuncs.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/callable_custom.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <array>
#include <cmath>
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

} // namespace

/* Boxes */

struct BoxType : std::enable_shared_from_this<BoxType> {
	std::vector<TypeImage> images; // The types inside the type, then the type.
	bool holds_containers = false; // Its values can hold an Array or a Dictionary.
	const TypeImage &type() const { return images.back(); }
};

namespace {

bool same_values(const TypeImage &p_type, const uint8_t *p_a, const uint8_t *p_b);
uint32_t hash_of(const TypeImage &p_type, const uint8_t *p_slot);
String value_text(const TypeImage &p_type, const uint8_t *p_slot);

class Box : public CallableCustom {
	std::unique_ptr<uint64_t[]> storage;
	// The registry lets go of its types when the library is unloaded, while
	// a container the engine has yet to free may still hold boxes.
	std::shared_ptr<const BoxType> holder;

	static bool equal(const CallableCustom *p_a, const CallableCustom *p_b) {
		const Box *a = static_cast<const Box *>(p_a);
		const Box *b = static_cast<const Box *>(p_b);
		return a->type == b->type && same_values(a->type->type(), a->value(), b->value());
	}
	static bool less(const CallableCustom *p_a, const CallableCustom *p_b) { return p_a < p_b; }

public:
	const BoxType *const type;
	const bool is_key;
	uint32_t hash_value = 0;

	Box(const BoxType *p_type, const uint8_t *p_value, bool p_key) :
			storage(new uint64_t[p_type->type().size / 8 + 1]), holder(p_type->shared_from_this()), type(p_type), is_key(p_key) {
		construct_value(type->type(), value());
		copy_value(type->type(), value(), p_value);
		// The engine asks once, when it makes the Callable; an element's box
		// is not hashed (architecture §2.2).
		hash_value = p_key ? hash_of(type->type(), value()) : uint32_t(uintptr_t(this) >> 4);
	}
	~Box() override { destroy_value(type->type(), value()); }
	uint8_t *value() const { return reinterpret_cast<uint8_t *>(storage.get()); }

	uint32_t hash() const override { return hash_value; }
	String get_as_text() const override { return value_text(type->type(), value()); }
	CompareEqualFunc get_compare_equal_func() const override { return equal; }
	CompareLessFunc get_compare_less_func() const override { return less; }
	bool is_valid() const override { return false; }
	ObjectID get_object() const override { return ObjectID(); }
	void call(const Variant **, int, Variant &, GDExtensionCallError &r_call_error) const override { r_call_error.error = GDEXTENSION_CALL_ERROR_INVALID_METHOD; }
};

const Box *as_box(const Variant &p_value) {
	if (type_of(p_value) != Variant::CALLABLE) {
		return nullptr;
	}
	return dynamic_cast<const Box *>(reinterpret_cast<const Callable *>(data_of(p_value))->get_custom());
}

// One type for each type key, kept until the library is unloaded, so that
// boxes outlive the images that made them and boxes of one type made by two
// images are of the same type.
std::recursive_mutex box_mutex;
std::unordered_map<std::string, std::shared_ptr<BoxType>> *box_types = nullptr;

// The type and the types inside it, in a table of their own.
uint32_t copy_type(const std::vector<bytecode::TypeDesc> &p_from, uint32_t p_type, std::vector<bytecode::TypeDesc> &r_to, std::unordered_map<uint32_t, uint32_t> &r_copied) {
	const auto found = r_copied.find(p_type);
	if (found != r_copied.end()) {
		return found->second;
	}
	bytecode::TypeDesc type = p_from[p_type];
	if (type.kind == bytecode::TypeKind::Dictionary) {
		type.key = copy_type(p_from, type.key, r_to, r_copied);
	}
	if (type.kind == bytecode::TypeKind::Option || type.kind == bytecode::TypeKind::Array || type.kind == bytecode::TypeKind::Dictionary) {
		type.element = copy_type(p_from, type.element, r_to, r_copied);
	}
	for (bytecode::FieldDesc &field : type.fields) {
		field.type = copy_type(p_from, field.type, r_to, r_copied);
	}
	r_to.push_back(type);
	r_copied.emplace(p_type, uint32_t(r_to.size() - 1));
	return uint32_t(r_to.size() - 1);
}

bool holds_containers(const TypeImage &p_type) {
	if (p_type.kind == bytecode::TypeKind::Option) {
		return holds_containers(*p_type.element);
	}
	if (p_type.kind == bytecode::TypeKind::Struct) {
		return std::any_of(p_type.fields.begin(), p_type.fields.end(), [](const FieldImage &p_field) { return holds_containers(*p_field.type); });
	}
	return p_type.storage == ValueType::Array || p_type.storage == ValueType::Dictionary;
}

const BoxType *box_type(const std::vector<bytecode::TypeDesc> &p_types, uint32_t p_type) {
	const std::string key = bytecode::type_key(p_types, p_type);
	std::lock_guard<std::recursive_mutex> lock(box_mutex);
	if (!box_types) {
		box_types = new std::unordered_map<std::string, std::shared_ptr<BoxType>>();
	}
	const auto found = box_types->find(key);
	if (found != box_types->end()) {
		return found->second.get();
	}
	// Registered before its images, the last of which is the type itself.
	BoxType *box = box_types->emplace(key, std::make_shared<BoxType>()).first->second.get();
	std::vector<bytecode::TypeDesc> closure;
	std::unordered_map<uint32_t, uint32_t> copied;
	copy_type(p_types, p_type, closure, copied);
	box->images = type_images(closure);
	box->holds_containers = holds_containers(box->type());
	return box;
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

// An element type of a container, or a Dictionary's key type.
String element_description(ValueType p_type, const StringName &p_class, const String &p_script, const String &p_interface) {
	return p_type == ValueType::Object ? class_description(p_class, p_script, p_interface) : String(bytecode::value_type_name(p_type));
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
			return "Array[" + element_description(p_check.element, p_check.class_name, p_check.script, p_check.interface) + "]";
		case ValueType::Dictionary:
			if (p_check.key == ValueType::Void) {
				return "Dictionary";
			}
			return "Dictionary[" + element_description(p_check.key, p_check.key_class_name, p_check.key_script, p_check.key_interface) + ", " +
					element_description(p_check.element, p_check.class_name, p_check.script, p_check.interface) + "]";
		default:
			return bytecode::value_type_name(p_type);
	}
}

// How the engine types a container's elements, or a Dictionary's keys.
String typed_description(int64_t p_builtin, const StringName &p_class, const Variant &p_script) {
	if (p_builtin != Variant::OBJECT) {
		return Variant::get_type_name(Variant::Type(p_builtin));
	}
	const String name = script_name(p_script);
	return name.is_empty() ? String(p_class) : name;
}

String value_description(const Variant &p_value) {
	if (is_none(p_value)) {
		return "None";
	}
	if (const Box *box = as_box(p_value)) {
		return type_description(box->type->type());
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
		return "Array[" + typed_description(array.get_typed_builtin(), array.get_typed_class_name(), array.get_typed_script()) + "]";
	}
	if (type_of(p_value) == Variant::DICTIONARY) {
		const Dictionary dictionary = p_value;
		if (!dictionary.is_typed()) {
			return "Dictionary";
		}
		return "Dictionary[" + typed_description(dictionary.get_typed_key_builtin(), dictionary.get_typed_key_class_name(), dictionary.get_typed_key_script()) + ", " +
				typed_description(dictionary.get_typed_value_builtin(), dictionary.get_typed_value_class_name(), dictionary.get_typed_value_script()) + "]";
	}
	return Variant::get_type_name(p_value.get_type());
}

// A container typed as the engine types it, with the element type, and with
// the class and script of object elements (spec §3.8); a container does not
// vary with its element type.
bool typed_as(int64_t p_builtin, const StringName &p_class, const Variant &p_script, ValueType p_type, const StringName &p_want_class, const String &p_want_script) {
	return p_builtin == int64_t(variant_type(p_type)) && (p_type != ValueType::Object || (p_class == p_want_class && has_script(p_script, p_want_script)));
}

// The engine types containers by class or script only: an element of an
// interface type is checked one by one. A freed or null element is the
// reader's to report (spec §3.8).
bool implements(const Variant &p_element, const String &p_interface) {
	return p_interface.is_empty() || !p_element.booleanize() || implements_interface(p_element, p_interface);
}

Variant load_script(const String &p_path) {
	return p_path.is_empty() ? Variant() : ResourceLoader::get_singleton()->call("load", p_path);
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
		const bool container = type.kind == bytecode::TypeKind::Array || type.kind == bytecode::TypeKind::Dictionary;
		// The class of an object, or of the objects in a container.
		const auto class_of = [&](const bytecode::TypeDesc &p_object, StringName &r_class, String &r_script, String &r_interface) {
			if (p_object.kind == bytecode::TypeKind::Object) {
				r_class = to_godot(p_object.object.name);
				if (p_object.object.interface_name.empty()) {
					r_script = to_godot(p_object.object.script);
				} else {
					r_interface = to_godot(p_object.object.script + "::" + p_object.object.interface_name);
				}
			}
		};
		class_of(container ? p_types[type.element] : type, check.class_name, check.script, check.interface);
		check.task_result = type.task_result;
		check.nullable = type.kind != bytecode::TypeKind::Object || type.nullable;
		// How the engine types a container's elements or keys: Callable for boxes.
		const auto kept_as = [&](uint32_t p_type) { return bytecode::is_boxed(p_types, p_type) ? ValueType::Callable : bytecode::storage_of(p_types, p_type); };
		if (container) {
			check.element = kept_as(type.element);
			image.item = &images[type.element];
		}
		if (type.kind == bytecode::TypeKind::Dictionary) {
			check.key = kept_as(type.key);
			class_of(p_types[type.key], check.key_class_name, check.key_script, check.key_interface);
			image.key = &images[type.key];
		}
		if (bytecode::is_boxed(p_types, i)) {
			image.box = box_type(p_types, i);
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
	return is_struct(p_type) ? encode(p_type, p_slot) : to_variant(p_type.storage, p_slot);
}

// A key or an element as spec §10.1 encodes it, out of its box.
static Variant encode_kept(const TypeImage &p_type, const Variant &p_value) {
	const uint8_t *value = opened(p_type, p_value);
	return value ? encode(p_type, value) : p_value;
}

Variant encode(const TypeImage &p_type, const uint8_t *p_slot) {
	if (is_option(p_type)) {
		return tag(p_slot) ? encode(*p_type.element, p_slot + 8) : Variant();
	}
	if (is_struct(p_type)) {
		Dictionary encoded;
		for (const FieldImage &field : p_type.fields) {
			encoded[field.name] = encode(*field.type, p_slot + field.offset);
		}
		return encoded;
	}
	if (p_type.box && p_type.storage == ValueType::Array) {
		const Array &array = *reinterpret_cast<const Array *>(p_slot);
		Array encoded;
		for (int64_t i = 0; i < array.size(); i++) {
			encoded.push_back(encode_kept(*p_type.item, array[i]));
		}
		return encoded;
	}
	if (p_type.box && p_type.storage == ValueType::Dictionary) {
		const Dictionary &dictionary = *reinterpret_cast<const Dictionary *>(p_slot);
		const Array keys = dictionary.keys();
		Dictionary encoded;
		for (int64_t i = 0; i < keys.size(); i++) {
			encoded[encode_kept(*p_type.key, keys[i])] = encode_kept(*p_type.item, dictionary[keys[i]]);
		}
		return encoded;
	}
	return outward_variant(p_type, p_slot);
}

static bool decode_container(const TypeImage &p_type, const Variant &p_value, uint8_t *p_slot);
static String found_description(ValueType p_type, const Variant &p_value, const ValueCheck &p_check);

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
	if (p_type.item && !from_variant(p_type, p_value, p_slot, true)) {
		return decode_container(p_type, p_value, p_slot);
	}
	return from_variant(p_type, p_value, p_slot, p_check);
}

// A key or an element decoded as a container keeps it.
static bool decode_kept(const TypeImage &p_type, const Variant &p_value, bool p_key, Variant &r_kept) {
	ValueBuffer value(p_type);
	if (!decode_field(p_type, p_value, value.data, true)) {
		return false;
	}
	r_kept = p_type.box ? make_box(p_type, value.data, p_key) : to_variant(p_type, value.data);
	return true;
}

// A new container of the type from an engine container typed otherwise, each
// key and element decoded, as JSON, say, gives untyped ones (spec §3.6).
static bool decode_container(const TypeImage &p_type, const Variant &p_value, uint8_t *p_slot) {
	if (p_type.storage == ValueType::Array && type_of(p_value) == Variant::ARRAY) {
		const Array source = p_value;
		Array decoded = typed_array(p_type.check.element, p_type.check.class_name, p_type.check.script);
		for (int64_t i = 0; i < source.size(); i++) {
			Variant element;
			if (!decode_kept(*p_type.item, source[i], false, element)) {
				return false;
			}
			decoded.push_back(element);
		}
		*reinterpret_cast<Array *>(p_slot) = decoded;
		return true;
	}
	if (p_type.storage == ValueType::Dictionary && type_of(p_value) == Variant::DICTIONARY) {
		const Dictionary source = p_value;
		const Array keys = source.keys();
		Dictionary decoded = typed_dictionary(p_type.check);
		for (int64_t i = 0; i < keys.size(); i++) {
			Variant key;
			Variant value;
			if (!decode_kept(*p_type.key, keys[i], true, key) || !decode_kept(*p_type.item, source[keys[i]], false, value)) {
				return false;
			}
			decoded[frozen_copy(key)] = value;
		}
		*reinterpret_cast<Dictionary *>(p_slot) = decoded;
		return true;
	}
	return false;
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

// Whether the type of the keys or elements of a container needs a look at
// each, the engine typing them as Callable or one level deep (spec §3.8).
static bool checked_one_by_one(const TypeImage &p_type) {
	return p_type.box || p_type.kind == bytecode::TypeKind::Array || p_type.kind == bytecode::TypeKind::Dictionary;
}

static bool kept_fits(const TypeImage &p_type, const Variant &p_value) {
	if (p_type.box) {
		return opened(p_type, p_value) != nullptr;
	}
	ValueBuffer value(p_type);
	return from_variant(p_type, p_value, value.data, true);
}

// The keys and elements of a container the engine has typed as the type says.
static int64_t misfit_element(const TypeImage &p_type, const Variant &p_value) {
	const bool keys = p_type.key && checked_one_by_one(*p_type.key);
	const bool items = checked_one_by_one(*p_type.item);
	if (type_of(p_value) == Variant::ARRAY && items) {
		const Array &array = *reinterpret_cast<const Array *>(data_of(p_value));
		for (int64_t i = 0; i < array.size(); i++) {
			if (!kept_fits(*p_type.item, array[i])) {
				return i;
			}
		}
	}
	if (type_of(p_value) == Variant::DICTIONARY && (keys || items)) {
		const Dictionary &dictionary = *reinterpret_cast<const Dictionary *>(data_of(p_value));
		const Array all = dictionary.keys();
		for (int64_t i = 0; i < all.size(); i++) {
			if ((keys && !kept_fits(*p_type.key, all[i])) || (items && !kept_fits(*p_type.item, dictionary[all[i]]))) {
				return i;
			}
		}
	}
	return -1;
}

bool from_variant(const TypeImage &p_type, const Variant &p_value, uint8_t *p_slot, bool p_check) {
	if (const uint8_t *value = opened(p_type, p_value)) {
		copy_value(p_type, p_slot, value);
		return true;
	}
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
	if (!from_variant(p_type.storage, p_value, p_slot, p_check ? &p_type.check : nullptr)) {
		return false;
	}
	return !p_check || !p_type.item || misfit_element(p_type, p_value) < 0;
}

String type_description(const TypeImage &p_type) {
	if (is_struct(p_type)) {
		return class_description(StringName(), String(), p_type.name);
	}
	if (p_type.kind == bytecode::TypeKind::Array) {
		return "Array[" + type_description(*p_type.item) + "]";
	}
	if (p_type.kind == bytecode::TypeKind::Dictionary) {
		return "Dictionary[" + type_description(*p_type.key) + ", " + type_description(*p_type.item) + "]";
	}
	return is_option(p_type) ? type_description(*p_type.element) + "?" : type_description(p_type.storage, p_type.check);
}

String entry_mismatch(const TypeImage &p_type, const Variant &p_value) {
	if (p_type.item) {
		// Typed as the engine types it, with an element that is not what the
		// type says: "found Array[float] at index 2".
		ValueBuffer value(p_type);
		const int64_t at = from_variant(p_type.storage, p_value, value.data, &p_type.check) ? misfit_element(p_type, p_value) : -1;
		String found = at < 0 ? found_description(p_type.storage, p_value, p_type.check) : String();
		if (at >= 0 && type_of(p_value) == Variant::ARRAY) {
			found = vformat("%s at index %d", value_description(Array(p_value)[at]), at);
		} else if (at >= 0) {
			const Dictionary dictionary = p_value;
			const Variant key = dictionary.keys()[at];
			found = kept_fits(*p_type.key, key) ? vformat("%s for the key %s", value_description(dictionary[key]), UtilityFunctions::var_to_str(key)) : vformat("the key %s", value_description(key));
		}
		return vformat("expected %s, found %s", type_description(p_type), found);
	}
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

/* What boxes do */

namespace {

// As containers match keys and elements: no int to float, NaN matches NaN
// (spec §5.2, core/variant/variant.cpp:3138-3139).
bool same_values(const TypeImage &p_type, const uint8_t *p_a, const uint8_t *p_b) {
	if (is_option(p_type)) {
		return tag(p_a) == tag(p_b) && (!tag(p_a) || same_values(*p_type.element, p_a + 8, p_b + 8));
	}
	if (is_struct(p_type)) {
		for (const FieldImage &field : p_type.fields) {
			if (!same_values(*field.type, p_a + field.offset, p_b + field.offset)) {
				return false;
			}
		}
		return true;
	}
	switch (p_type.storage) {
		case ValueType::Bool:
		case ValueType::Int:
			return *reinterpret_cast<const uint64_t *>(p_a) == *reinterpret_cast<const uint64_t *>(p_b);
		case ValueType::Float: {
			const double a = *reinterpret_cast<const double *>(p_a);
			const double b = *reinterpret_cast<const double *>(p_b);
			return a == b || (std::isnan(a) && std::isnan(b));
		}
		case ValueType::Object:
			return object_data(p_a).id == object_data(p_b).id;
		default:
			return to_variant(p_type.storage, p_a).hash_compare(to_variant(p_type.storage, p_b));
	}
}

// The engine stops at this depth too (core/typedefs.h:242).
constexpr int MAX_DEPTH = 100;

uint32_t hash_contents(const Variant &p_value, int p_depth);

// Every field, containers by their contents (spec §3.6).
uint32_t hash_of(const TypeImage &p_type, const uint8_t *p_slot) {
	if (is_option(p_type)) {
		return tag(p_slot) ? hash_murmur3_one_32(hash_of(*p_type.element, p_slot + 8), 1) : 0;
	}
	if (is_struct(p_type)) {
		uint32_t hash = HASH_MURMUR3_SEED;
		for (const FieldImage &field : p_type.fields) {
			hash = hash_murmur3_one_32(hash_of(*field.type, p_slot + field.offset), hash);
		}
		return hash_fmix32(hash);
	}
	if (p_type.storage == ValueType::Object) {
		return hash_one_uint64(object_data(p_slot).id);
	}
	return hash_contents(to_variant(p_type.storage, p_slot), 0);
}

// A Dictionary's entries in any order, as `==` compares them (spec §5.2).
uint32_t hash_contents(const Variant &p_value, int p_depth) {
	if (p_depth > MAX_DEPTH) {
		return 0;
	}
	if (const Box *box = as_box(p_value)) {
		return box->is_key ? box->hash_value : hash_of(box->type->type(), box->value());
	}
	if (type_of(p_value) == Variant::ARRAY) {
		const Array &array = *reinterpret_cast<const Array *>(data_of(p_value));
		uint32_t hash = hash_murmur3_one_32(Variant::ARRAY);
		for (int64_t i = 0; i < array.size(); i++) {
			hash = hash_murmur3_one_32(hash_contents(array[i], p_depth + 1), hash);
		}
		return hash_fmix32(hash);
	}
	if (type_of(p_value) == Variant::DICTIONARY) {
		const Dictionary &dictionary = *reinterpret_cast<const Dictionary *>(data_of(p_value));
		const Array keys = dictionary.keys();
		uint32_t sum = 0;
		for (int64_t i = 0; i < keys.size(); i++) {
			sum += hash_murmur3_one_32(hash_contents(dictionary[keys[i]], p_depth + 1), hash_contents(keys[i], p_depth + 1));
		}
		return hash_fmix32(hash_murmur3_one_32(sum, Variant::DICTIONARY));
	}
	return p_value.hash();
}

// "Hit(damage = 3, crit = false)".
String value_text(const TypeImage &p_type, const uint8_t *p_slot) {
	if (is_option(p_type)) {
		return tag(p_slot) ? value_text(*p_type.element, p_slot + 8) : String("None");
	}
	if (is_struct(p_type)) {
		String text = type_description(p_type) + "(";
		for (size_t i = 0; i < p_type.fields.size(); i++) {
			const FieldImage &field = p_type.fields[i];
			text += (i > 0 ? ", " : "") + field.name + " = " + value_text(*field.type, p_slot + field.offset);
		}
		return text + ")";
	}
	const Variant value = outward_variant(p_type, p_slot);
	if (is_none(value)) {
		return "None";
	}
	return type_of(value) == Variant::STRING || type_of(value) == Variant::STRING_NAME ? vformat("\"%s\"", value) : String(value);
}

// The containers in a value, copied as deep_copy() copies them; with a mode,
// resources in it too, as duplicate_deep() decides.
void deep_copy_value(const TypeImage &p_type, uint8_t *p_slot, int64_t p_mode) {
	if (is_option(p_type)) {
		if (tag(p_slot)) {
			deep_copy_value(*p_type.element, p_slot + 8, p_mode);
		}
	} else if (is_struct(p_type)) {
		for (const FieldImage &field : p_type.fields) {
			deep_copy_value(*field.type, p_slot + field.offset, p_mode);
		}
	} else if (p_type.storage == ValueType::Array) {
		Array &array = *reinterpret_cast<Array *>(p_slot);
		array = deep_copy(array, p_mode);
	} else if (p_type.storage == ValueType::Dictionary) {
		Dictionary &dictionary = *reinterpret_cast<Dictionary *>(p_slot);
		dictionary = deep_copy(dictionary, p_mode);
	} else if (p_type.storage == ValueType::Object && p_mode >= 0 && !is_none_object(p_slot)) {
		ObjectView view(p_slot);
		Array wrapped;
		wrapped.push_back(view.get());
		set_object(p_slot, wrapped.duplicate_deep(p_mode)[0]);
	}
}

// The containers in a key's value replaced by frozen copies (spec §3.8).
void frozen_value(const TypeImage &p_type, uint8_t *p_slot) {
	if (is_option(p_type)) {
		if (tag(p_slot)) {
			frozen_value(*p_type.element, p_slot + 8);
		}
	} else if (is_struct(p_type)) {
		for (const FieldImage &field : p_type.fields) {
			frozen_value(*field.type, p_slot + field.offset);
		}
	} else if (p_type.storage == ValueType::Array) {
		Array &array = *reinterpret_cast<Array *>(p_slot);
		array = frozen_copy(array);
	} else if (p_type.storage == ValueType::Dictionary) {
		Dictionary &dictionary = *reinterpret_cast<Dictionary *>(p_slot);
		dictionary = frozen_copy(dictionary);
	}
}

void freeze_value(const TypeImage &p_type, const uint8_t *p_slot) {
	if (is_option(p_type)) {
		if (tag(p_slot)) {
			freeze_value(*p_type.element, p_slot + 8);
		}
	} else if (is_struct(p_type)) {
		for (const FieldImage &field : p_type.fields) {
			freeze_value(*field.type, p_slot + field.offset);
		}
	} else if (p_type.storage == ValueType::Array || p_type.storage == ValueType::Dictionary) {
		freeze(to_variant(p_type.storage, p_slot));
	}
}

} // namespace

Variant make_box(const TypeImage &p_type, const uint8_t *p_value, bool p_key) {
	return Callable(memnew(Box(p_type.box, p_value, p_key)));
}

const uint8_t *opened(const TypeImage &p_type, const Variant &p_value) {
	const Box *box = p_type.box ? as_box(p_value) : nullptr;
	return box && box->type == p_type.box ? box->value() : nullptr;
}

bool keeps_boxes(const Variant &p_container) {
	if (type_of(p_container) == Variant::ARRAY) {
		return reinterpret_cast<const Array *>(data_of(p_container))->get_typed_builtin() == Variant::CALLABLE;
	}
	if (type_of(p_container) == Variant::DICTIONARY) {
		const Dictionary &dictionary = *reinterpret_cast<const Dictionary *>(data_of(p_container));
		return dictionary.get_typed_key_builtin() == Variant::CALLABLE || dictionary.get_typed_value_builtin() == Variant::CALLABLE;
	}
	return false;
}

void clear_box_types() {
	std::lock_guard<std::recursive_mutex> lock(box_mutex);
	delete box_types;
	box_types = nullptr;
}

uint32_t content_hash(const Variant &p_value) {
	return hash_contents(p_value, 0);
}

Variant deep_copy(const Variant &p_value, int64_t p_mode) {
	if (const Box *box = as_box(p_value)) {
		const TypeImage &type = box->type->type();
		if (!box->type->holds_containers && p_mode < 0) {
			return p_value;
		}
		ValueBuffer copy(type);
		copy_value(type, copy.data, box->value());
		deep_copy_value(type, copy.data, p_mode);
		return make_box(type, copy.data, box->is_key);
	}
	if (type_of(p_value) == Variant::ARRAY) {
		const Array source = p_value;
		if (!keeps_boxes(p_value)) {
			return p_mode < 0 ? source.duplicate(true) : source.duplicate_deep(p_mode);
		}
		Array copy = source.duplicate(false);
		for (int64_t i = 0; i < source.size(); i++) {
			copy[i] = deep_copy(source[i], p_mode);
		}
		return copy;
	}
	if (type_of(p_value) == Variant::DICTIONARY) {
		const Dictionary source = p_value;
		if (!keeps_boxes(p_value)) {
			return p_mode < 0 ? source.duplicate(true) : source.duplicate_deep(p_mode);
		}
		// Keys are read-only (spec §3.8), so the copy shares them.
		Dictionary copy = source.duplicate(false);
		const Array keys = source.keys();
		for (int64_t i = 0; i < keys.size(); i++) {
			copy[keys[i]] = deep_copy(source[keys[i]], p_mode);
		}
		return copy;
	}
	return p_value;
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
				const Array array = p_value;
				if (!typed_as(array.get_typed_builtin(), array.get_typed_class_name(), array.get_typed_script(), p_check->element, p_check->class_name, p_check->script)) {
					return false;
				}
				for (int64_t i = 0; !p_check->interface.is_empty() && i < array.size(); i++) {
					if (!implements(array[i], p_check->interface)) {
						return false;
					}
				}
			}
			break;
		case ValueType::Dictionary:
			if (p_check && p_check->key != ValueType::Void && have == Variant::DICTIONARY) {
				const Dictionary dictionary = p_value;
				if (!typed_as(dictionary.get_typed_key_builtin(), dictionary.get_typed_key_class_name(), dictionary.get_typed_key_script(), p_check->key, p_check->key_class_name, p_check->key_script) ||
						!typed_as(dictionary.get_typed_value_builtin(), dictionary.get_typed_value_class_name(), dictionary.get_typed_value_script(), p_check->element, p_check->class_name, p_check->script)) {
					return false;
				}
				if (!p_check->key_interface.is_empty() || !p_check->interface.is_empty()) {
					const Array keys = dictionary.keys();
					for (int64_t i = 0; i < keys.size(); i++) {
						if (!implements(keys[i], p_check->key_interface) || !implements(dictionary[keys[i]], p_check->interface)) {
							return false;
						}
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

static String found_description(ValueType p_type, const Variant &p_value, const ValueCheck &p_check) {
	if (p_type == ValueType::Array && !p_check.interface.is_empty() && type_of(p_value) == Variant::ARRAY) {
		// Typed as the engine types it, with an element that does not implement
		// the interface.
		const Array array = p_value;
		for (int64_t i = 0; i < array.size(); i++) {
			const Variant element = array[i];
			if (element.booleanize() && !implements_interface(element, p_check.interface)) {
				return vformat("%s at index %d", value_description(element), i);
			}
		}
	}
	return value_description(p_value);
}

String entry_mismatch(ValueType p_type, const Variant &p_value, const ValueCheck &p_check) {
	return vformat("expected %s, found %s", type_description(p_type, p_check), found_description(p_type, p_value, p_check));
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
		array.set_typed(uint32_t(variant_type(p_element)), p_class, load_script(p_script));
	}
	return array;
}

Dictionary typed_dictionary(const ValueCheck &p_check) {
	Dictionary dictionary;
	if (p_check.key != ValueType::Void) {
		dictionary.set_typed(uint32_t(variant_type(p_check.key)), p_check.key_class_name, load_script(p_check.key_script), uint32_t(variant_type(p_check.element)), p_check.class_name, load_script(p_check.script));
	}
	return dictionary;
}

// make_read_only() leaves the containers inside alone and cannot be undone
// (core/variant/array.cpp:938-942, core/variant/dictionary.cpp:599-603), so a
// key is copied and frozen level by level.
Variant frozen_copy(const Variant &p_key) {
	if (const Box *box = as_box(p_key)) {
		if (!box->type->holds_containers && box->is_key) {
			return p_key;
		}
		const TypeImage &type = box->type->type();
		ValueBuffer copy(type);
		copy_value(type, copy.data, box->value());
		frozen_value(type, copy.data);
		return make_box(type, copy.data, true);
	}
	if (type_of(p_key) == Variant::ARRAY) {
		const Array original = p_key;
		Array copy = original.duplicate(false);
		for (int64_t i = 0; i < copy.size(); i++) {
			copy[i] = frozen_copy(original[i]);
		}
		copy.make_read_only();
		return copy;
	}
	if (type_of(p_key) == Variant::DICTIONARY) {
		const Dictionary original = p_key;
		Dictionary copy = original.duplicate(false);
		copy.clear();
		const Array keys = original.keys();
		for (int64_t i = 0; i < keys.size(); i++) {
			copy[frozen_copy(keys[i])] = frozen_copy(original[keys[i]]);
		}
		copy.make_read_only();
		return copy;
	}
	return p_key;
}

void freeze(const Variant &p_key) {
	if (const Box *box = as_box(p_key)) {
		freeze_value(box->type->type(), box->value());
	} else if (type_of(p_key) == Variant::ARRAY) {
		Array array = p_key;
		for (int64_t i = 0; i < array.size(); i++) {
			freeze(array[i]);
		}
		array.make_read_only();
	} else if (type_of(p_key) == Variant::DICTIONARY) {
		Dictionary dictionary = p_key;
		const Array keys = dictionary.keys();
		for (int64_t i = 0; i < keys.size(); i++) {
			freeze(keys[i]);
			freeze(dictionary[keys[i]]);
		}
		dictionary.make_read_only();
	}
}

bool keys_hold_containers(const Dictionary &p_dictionary) {
	const int64_t key = p_dictionary.get_typed_key_builtin();
	return key == Variant::NIL || key == Variant::ARRAY || key == Variant::DICTIONARY || key == Variant::CALLABLE;
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
