#include "runtime/values.h"

#include "runtime/method_callable.h"
#include "runtime/task.h"
#include "runtime/vm.h"

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
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <unordered_map>
#include <unordered_set>

using namespace godot;

namespace petrichor {

using bytecode::ValueType;

namespace {

// Slots that the engine's functions for each type do not handle.
bool is_variant_slot(ValueType p_type) {
	return p_type == ValueType::Object || p_type == ValueType::Variant || p_type == ValueType::Task;
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
	GDExtensionPtrIndexedGetter get_indexed = nullptr;
	GDExtensionPtrIndexedSetter set_indexed = nullptr;
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
	if (!p_type.fields.empty()) {
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
	return p_script.is_null() ? String() : (p_script->get_global_name().is_empty() ? script_path(p_script) : String(p_script->get_global_name()));
}

// An engine class, a class of this language or an interface ("script::Name")
// as the source names them.
String class_description(const StringName &p_class, const String &p_script, const String &p_interface) {
	if (!p_interface.is_empty()) {
		const int64_t split = p_interface.rfind("::");
		const Ref<Script> owner = load_resource(p_interface.substr(0, split));
		const String name = p_interface.substr(split + 2);
		return owner.is_valid() && !owner->get_global_name().is_empty() ? String(owner->get_global_name()) + "." + name : name;
	}
	if (!p_script.is_empty()) {
		return script_name(load_resource(p_script));
	}
	return p_class;
}

// An element type of a container, or a Dictionary's key type.
String element_description(ValueType p_type, const StringName &p_class, const String &p_script, const String &p_interface) {
	return p_type == ValueType::Object ? class_description(p_class, p_script, p_interface) : String(bytecode::value_type_name(p_type));
}

String type_description(ValueType p_type, const ValueCheck &p_check) {
	switch (p_type) {
		case ValueType::Object:
		case ValueType::Task: {
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
	if (type_of(p_value) == Variant::CALLABLE && Callable(p_value).is_null()) {
		return "a null Callable";
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

// The implicit conversions of spec §3.14: int to float, the integer vectors and
// Rect2i to their float counterparts, String to and from StringName and
// NodePath.
bool converts(Variant::Type p_have, Variant::Type p_want) {
	switch (p_want) {
		case Variant::FLOAT:
			return p_have == Variant::INT;
		case Variant::VECTOR2:
			return p_have == Variant::VECTOR2I;
		case Variant::VECTOR3:
			return p_have == Variant::VECTOR3I;
		case Variant::VECTOR4:
			return p_have == Variant::VECTOR4I;
		case Variant::RECT2:
			return p_have == Variant::RECT2I;
		case Variant::STRING:
			return p_have == Variant::STRING_NAME || p_have == Variant::NODE_PATH;
		case Variant::STRING_NAME:
		case Variant::NODE_PATH:
			return p_have == Variant::STRING;
		default:
			return false;
	}
}

} // namespace

void init_values() {
	String error;
	if (!check_value_sizes(error)) {
		return;
	}
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
		entry.get_indexed = gdextension_interface::variant_get_ptr_indexed_getter(engine_type);
		entry.set_indexed = gdextension_interface::variant_get_ptr_indexed_setter(engine_type);
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
		// The engine destroys nothing of these (core/variant/variant.h:268-272, 314-319).
		if (type_of(variant_at(p_slot)) > Variant::FLOAT) {
			variant_at(p_slot).~Variant();
		}
	} else if (p_type == ValueType::Task) {
		task_at(p_slot) = TaskRef();
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

void take_returned_object(uint8_t *p_slot, void *p_object, bool p_with_reference) {
	const ObjectData old = object_data(p_slot);
	if (p_with_reference || !p_object) {
		object_data(p_slot) = p_object ? ObjectData{ uint64_t(gdextension_interface::object_get_instance_id(p_object)), p_object } : ObjectData();
	} else {
		// As a Variant holds it (core/variant/variant.cpp:1094-1114).
		static const GDExtensionVariantFromTypeConstructorFunc from_object = gdextension_interface::get_variant_from_type_constructor(GDEXTENSION_VARIANT_TYPE_OBJECT);
		alignas(Variant) uint8_t value[sizeof(Variant)];
		from_object(value, &p_object);
		std::memcpy(p_slot, value + 8, sizeof(ObjectData));
	}
	release(old);
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
		const bool function = type.kind == bytecode::TypeKind::Value && type.value == ValueType::Callable && !type.name.empty();
		check.nullable = (type.kind != bytecode::TypeKind::Object && !function) || type.nullable;
		if (function) {
			image.name = to_godot(type.name);
		}
		// How the engine types a container's elements or keys: Callable for boxes,
		// and a task as its CoroutineTask.
		const auto kept_as = [&](uint32_t p_type) {
			const ValueType storage = bytecode::storage_of(p_types, p_type);
			return bytecode::is_boxed(p_types, p_type) ? ValueType::Callable : (storage == ValueType::Task ? ValueType::Object : storage);
		};
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
		if (bytecode::has_fields(p_types, i)) {
			image.name = to_godot(type.name);
			const std::vector<uint32_t> offsets = bytecode::field_offsets(p_types, i);
			for (size_t f = 0; f < type.fields.size(); f++) {
				image.fields.push_back({ to_godot(type.fields[f].name), &images[type.fields[f].type], offsets[f] });
			}
		}
		if (type.kind == bytecode::TypeKind::PayloadEnum) {
			int64_t first = 1;
			for (const bytecode::EnumValueDesc &value : type.values) {
				image.enum_values.push_back(first);
				image.enum_declared.push_back({ to_godot(value.name), value.value });
				first += value.value;
			}
		}
		if (type.kind == bytecode::TypeKind::Enum) {
			image.name = to_godot(type.name);
			// The engine gives values outside its enums, such as a Key with the
			// bits of modifiers (core/input/input_event.cpp:394-396; ed1daf0).
			image.enum_checked = type.name.find("::") != std::string::npos;
			for (const bytecode::EnumValueDesc &value : type.values) {
				image.enum_values.push_back(value.value);
				image.enum_declared.push_back({ to_godot(value.name), value.value });
			}
			std::sort(image.enum_values.begin(), image.enum_values.end());
		}
		images.push_back(std::move(image));
	}
	return images;
}

/* Values by their type */

static bool is_option(const TypeImage &p_type) {
	return p_type.kind == bytecode::TypeKind::Option;
}

// A struct or a PayloadEnum, whose fields are slots inside its slot.
static bool is_struct(const TypeImage &p_type) {
	return p_type.kind == bytecode::TypeKind::Struct || p_type.kind == bytecode::TypeKind::PayloadEnum;
}

static bool is_payload_enum(const TypeImage &p_type) {
	return p_type.kind == bytecode::TypeKind::PayloadEnum;
}

// Result[T, E] (spec §3.5) and Settled[T] (§9.2), which no script declares.
static bool is_result(const TypeImage &p_type) {
	return is_payload_enum(p_type) && p_type.name == "Result";
}

static bool is_settled(const TypeImage &p_type) {
	return is_payload_enum(p_type) && p_type.name == "Settled";
}

// The key of an encoded PayloadEnum that names its variant (spec §10.1).
static const char *const VARIANT_KEY = "$variant";

static uint64_t &tag(uint8_t *p_slot) {
	return *reinterpret_cast<uint64_t *>(p_slot);
}

static uint64_t tag(const uint8_t *p_slot) {
	return *reinterpret_cast<const uint64_t *>(p_slot);
}

// Calls `p_fn` on the fields that the value of a struct or a PayloadEnum
// holds, while it returns true: a struct's every field, a PayloadEnum's index
// of its variant and that variant's fields; the others hold their defaults.
template <typename Fn>
static bool all_held_fields(const TypeImage &p_type, const uint8_t *p_slot, Fn &&p_fn) {
	size_t first = 0;
	size_t end = p_type.fields.size();
	if (is_payload_enum(p_type)) {
		if (!p_fn(p_type.fields[0])) {
			return false;
		}
		const size_t variant = size_t(tag(p_slot));
		first = size_t(p_type.enum_values[variant]);
		end = first + size_t(p_type.enum_declared[variant].second);
	}
	for (size_t i = first; i < end; i++) {
		if (!p_fn(p_type.fields[i])) {
			return false;
		}
	}
	return true;
}

template <typename Fn>
static void for_each_slot_leaf(const TypeImage &p_type, uint32_t p_offset, Fn &&p_fn) {
	if (is_option(p_type)) {
		for_each_slot_leaf(*p_type.element, p_offset + 8, p_fn);
	} else if (is_struct(p_type)) {
		for (const FieldImage &field : p_type.fields) {
			for_each_slot_leaf(*field.type, p_offset + field.offset, p_fn);
		}
	} else {
		p_fn(p_type.storage, p_offset);
	}
}

template <typename Ptr, typename Fn>
static void for_each_active_leaf(const TypeImage &p_type, Ptr *p_slot, Fn &&p_fn) {
	if (is_option(p_type)) {
		if (tag(p_slot)) {
			for_each_active_leaf(*p_type.element, p_slot + 8, p_fn);
		}
	} else if (is_struct(p_type)) {
		all_held_fields(p_type, p_slot, [&](const FieldImage &p_field) {
			for_each_active_leaf(*p_field.type, p_slot + p_field.offset, p_fn);
			return true;
		});
	} else {
		p_fn(p_type.storage, p_slot);
	}
}

void SlotLayout::add(uint32_t p_offset, const TypeImage &p_type) {
	for_each_slot_leaf(p_type, p_offset, [&](ValueType p_storage, uint32_t p_at) {
		if (!ops(p_storage).zero_default) {
			constructed.emplace_back(p_at, p_storage);
		}
		if (is_variant_slot(p_storage) || ops(p_storage).destroy) {
			destroyed.emplace_back(p_at, p_storage);
		}
	});
}

void construct_value(const TypeImage &p_type, uint8_t *p_slot) {
	std::memset(p_slot, 0, p_type.size);
	for_each_slot_leaf(p_type, 0, [&](ValueType p_storage, uint32_t p_offset) {
		if (!ops(p_storage).zero_default) {
			ops(p_storage).construct(p_slot + p_offset, nullptr);
		}
	});
}

void destroy_value(const TypeImage &p_type, uint8_t *p_slot) {
	for_each_slot_leaf(p_type, 0, [&](ValueType p_storage, uint32_t p_offset) { destroy_value(p_storage, p_slot + p_offset); });
}

void clear_value(const TypeImage &p_type, uint8_t *p_slot) {
	// The value moves out first, as the engine's values can be moved by their
	// bytes. A destructor that stops at a breakpoint then shows the slot
	// already cleared, not half destroyed.
	alignas(16) uint8_t small[64];
	std::unique_ptr<uint8_t[]> large(p_type.size > sizeof(small) ? new uint8_t[p_type.size] : nullptr);
	uint8_t *old = large ? large.get() : small;
	std::memcpy(old, p_slot, p_type.size);
	construct_value(p_type, p_slot);
	destroy_value(p_type, old);
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

// Each value is returned as it is made, not moved: godot-cpp's move swaps
// the bytes in wide pieces, which stall on a Variant just written field by
// field (thirdparty/godot-cpp/src/variant/variant.cpp:90-92).
Variant to_variant(const TypeImage &p_type, const uint8_t *p_slot) {
	if (is_option(p_type) && !tag(p_slot)) {
		return Variant(); // None is NIL (spec §10.1).
	}
	if (is_option(p_type)) {
		return to_variant(*p_type.element, p_slot + 8);
	}
	if (is_struct(p_type)) {
		return encode(p_type, p_slot);
	}
	return to_variant(p_type.storage, p_slot);
}

Variant encode_variant(const TypeImage &p_type, size_t p_variant, const std::vector<Variant> &p_fields) {
	Dictionary encoded;
	encoded[VARIANT_KEY] = p_type.enum_declared[p_variant].first;
	size_t field = 1;
	for (size_t v = 0; v < p_variant; v++) {
		field += size_t(p_type.enum_declared[v].second);
	}
	for (const Variant &value : p_fields) {
		encoded[p_type.fields[field++].name] = value;
	}
	return encoded;
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
		// A PayloadEnum names its variant by the reserved key (spec §3.7).
		Dictionary encoded;
		all_held_fields(p_type, p_slot, [&](const FieldImage &p_field) {
			if (is_payload_enum(p_type) && &p_field == &p_type.fields[0]) {
				encoded[VARIANT_KEY] = p_type.enum_declared[tag(p_slot)].first;
			} else {
				encoded[p_field.name] = encode(*p_field.type, p_slot + p_field.offset);
			}
			return true;
		});
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
static String described(const TypeImage &p_type, const Variant &p_value, bool p_decoding);

// A float that is exactly an integer within its range, as the engine parses
// every number in JSON (spec §10.8).
static bool whole_float(const Variant &p_value, int64_t &r_int) {
	if (type_of(p_value) != Variant::FLOAT) {
		return false;
	}
	const double value = p_value;
	if (!(value >= -9223372036854775808.0 && value < 9223372036854775808.0) || value != double(int64_t(value))) {
		return false;
	}
	r_int = int64_t(value);
	return true;
}

// As `T.try_from` decodes a field (spec §5.3): an int or an enum without
// payloads also from a whole float.
static bool decode_field(const TypeImage &p_type, const Variant &p_value, uint8_t *p_slot, bool p_check) {
	if (is_option(p_type) && !is_none(p_value)) {
		if (!decode_field(*p_type.element, p_value, p_slot + 8, p_check)) {
			return false;
		}
		tag(p_slot) = 1;
		return true;
	}
	if (p_type.storage == ValueType::Int && type_of(p_value) == Variant::FLOAT) {
		int64_t whole = 0;
		return whole_float(p_value, whole) && from_variant(p_type, whole, p_slot, p_check);
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

// The variant that an encoded PayloadEnum names, or -1 (spec §10.1).
static int64_t encoded_variant(const TypeImage &p_type, const Dictionary &p_encoded) {
	const Variant name = p_encoded.get(VARIANT_KEY, Variant());
	if (type_of(name) == Variant::STRING || type_of(name) == Variant::STRING_NAME) {
		for (size_t i = 0; i < p_type.enum_declared.size(); i++) {
			if (p_type.enum_declared[i].first == String(name)) {
				return int64_t(i);
			}
		}
	}
	return -1;
}

// While a mismatch is described by decoding again, a field left out only has
// to have a default, so that no default runs twice. The defaults computed on
// this thread that ended in a program error tell such a decoding from a
// mismatch.
static thread_local bool describing = false;
static thread_local uint64_t defaults_failed = 0;

// A struct's field left out takes its default, which the function
// "@Struct.field" of the struct's class computes, as for a construction that
// leaves it out (spec §3.6). A variant's fields have none.
static bool decode_default(const TypeImage &p_type, const FieldImage &p_field, uint8_t *p_slot) {
	if (is_payload_enum(p_type)) {
		return false;
	}
	const int64_t at = p_type.name.rfind("::");
	const std::shared_ptr<const ClassImage> image = script_image(p_type.name.substr(0, at));
	const int index = image ? image->find_function("@" + p_type.name.substr(at + 2) + "." + p_field.name) : -1;
	if (index < 0 || describing) {
		return index >= 0;
	}
	GDExtensionCallError error;
	bool failed = false;
	const Variant value = vm::call(image, index, InstanceContext(), nullptr, 0, error, &failed);
	if (failed) {
		defaults_failed++;
		return false;
	}
	return error.error == GDEXTENSION_CALL_OK && decode_field(*p_field.type, value, p_slot, true);
}

// Decodes the fields that the encoded struct, or the variant whose index is
// already in the slot, holds; false at the first that does not decode, which
// goes in `r_failed`.
static bool decode_fields(const TypeImage &p_type, const Dictionary &p_encoded, uint8_t *p_slot, bool p_check, const FieldImage **r_failed = nullptr) {
	return all_held_fields(p_type, p_slot, [&](const FieldImage &p_field) {
		if (is_payload_enum(p_type) && &p_field == &p_type.fields[0]) {
			return true;
		}
		if (p_encoded.has(p_field.name) ? decode_field(*p_field.type, p_encoded[p_field.name], p_slot + p_field.offset, p_check) : decode_default(p_type, p_field, p_slot + p_field.offset)) {
			return true;
		}
		if (r_failed) {
			*r_failed = &p_field;
		}
		return false;
	});
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
		if (is_payload_enum(p_type)) {
			const int64_t variant = encoded_variant(p_type, encoded);
			if (variant < 0) {
				return false;
			}
			// The fields of the other variants back at their defaults.
			destroy_value(p_type, p_slot);
			construct_value(p_type, p_slot);
			tag(p_slot) = uint64_t(variant);
		}
		return decode_fields(p_type, encoded, p_slot, p_check);
	}
	if (!from_variant(p_type.storage, p_value, p_slot, p_check ? &p_type.check : nullptr)) {
		return false;
	}
	if (p_check && p_type.enum_checked) {
		return is_enum_value(p_type, *reinterpret_cast<const int64_t *>(p_slot));
	}
	return !p_check || !p_type.item || misfit_element(p_type, p_value) < 0;
}

bool is_enum_value(const TypeImage &p_type, int64_t p_value) {
	return std::binary_search(p_type.enum_values.begin(), p_type.enum_values.end(), p_value);
}

bool try_from(const TypeImage &p_result, const Variant &p_value, uint8_t *p_slot) {
	destroy_value(p_result, p_slot);
	construct_value(p_result, p_slot);
	const FieldImage &value = p_result.fields[1];
	const uint64_t failures = defaults_failed;
	if (decode_field(*value.type, p_value, p_slot + value.offset, true)) {
		return true;
	}
	if (defaults_failed != failures) {
		return false;
	}
	// The value field back at its default, as the other variant's fields are.
	destroy_value(*value.type, p_slot + value.offset);
	construct_value(*value.type, p_slot + value.offset);
	tag(p_slot) = 1;
	const FieldImage &error = p_result.fields[2];
	*reinterpret_cast<String *>(p_slot + error.offset + error.type->fields[0].offset) = described(*value.type, p_value, true);
	return true;
}

String type_description(const TypeImage &p_type) {
	if (is_result(p_type)) {
		return "Result[" + type_description(*p_type.fields[1].type) + ", " + type_description(*p_type.fields[2].type) + "]";
	}
	if (is_settled(p_type)) {
		return "Settled[" + (p_type.enum_declared[0].second ? type_description(*p_type.fields[1].type) : String("void")) + "]";
	}
	if (is_struct(p_type) || (p_type.kind == bytecode::TypeKind::Enum && p_type.name.contains("::"))) {
		return class_description(StringName(), String(), p_type.name);
	}
	// An enum, or a function type as written.
	if (p_type.kind == bytecode::TypeKind::Enum || (p_type.storage == ValueType::Callable && !p_type.name.is_empty())) {
		return p_type.name;
	}
	if (p_type.kind == bytecode::TypeKind::Array) {
		return "Array[" + type_description(*p_type.item) + "]";
	}
	if (p_type.kind == bytecode::TypeKind::Dictionary) {
		return "Dictionary[" + type_description(*p_type.key) + ", " + type_description(*p_type.item) + "]";
	}
	return is_option(p_type) ? type_description(*p_type.element) + "?" : type_description(p_type.storage, p_type.check);
}

// Why the value does not fit the type, after the path to where it lies in a
// value being decoded: "SaveData.enemies[2].pos: expected Vector2, found
// String". Decoding takes what decode_field() takes, an entry check a
// container only as the engine types it (spec §3.8).
static String mismatch(const TypeImage &p_type, const Variant &p_value, const String &p_path, bool p_decoding) {
	const String at = p_path.is_empty() ? String() : p_path + String(": ");
	const String where = p_path.is_empty() ? type_description(p_type) : p_path;
	if (p_decoding && is_option(p_type) && !is_none(p_value)) {
		return mismatch(*p_type.element, p_value, p_path, true);
	}
	int64_t whole = 0;
	if (p_decoding && p_type.storage == ValueType::Int && whole_float(p_value, whole)) {
		return mismatch(p_type, whole, p_path, true);
	}
	Variant kept;
	if (p_decoding && p_type.item && p_type.storage == ValueType::Array && type_of(p_value) == Variant::ARRAY) {
		const Array array = p_value;
		for (int64_t i = 0; i < array.size(); i++) {
			if (!decode_kept(*p_type.item, array[i], false, kept)) {
				return mismatch(*p_type.item, array[i], vformat("%s[%d]", where, i), true);
			}
		}
	}
	if (p_decoding && p_type.item && p_type.storage == ValueType::Dictionary && type_of(p_value) == Variant::DICTIONARY) {
		const Dictionary dictionary = p_value;
		const Array keys = dictionary.keys();
		for (int64_t i = 0; i < keys.size(); i++) {
			const String key = UtilityFunctions::var_to_str(keys[i]);
			if (!decode_kept(*p_type.key, keys[i], true, kept)) {
				return mismatch(*p_type.key, keys[i], vformat("%s (the key %s)", where, key), true);
			}
			if (!decode_kept(*p_type.item, dictionary[keys[i]], false, kept)) {
				return mismatch(*p_type.item, dictionary[keys[i]], vformat("%s[%s]", where, key), true);
			}
		}
	}
	if (p_type.item) {
		// Typed as the engine types it, with an element that is not what the
		// type says: "found Array[float] at index 2".
		ValueBuffer value(p_type);
		const int64_t misfit = from_variant(p_type.storage, p_value, value.data, &p_type.check) ? misfit_element(p_type, p_value) : -1;
		String found = misfit < 0 ? found_description(p_type.storage, p_value, p_type.check) : String();
		if (misfit >= 0 && type_of(p_value) == Variant::ARRAY) {
			found = vformat("%s at index %d", value_description(Array(p_value)[misfit]), misfit);
		} else if (misfit >= 0) {
			const Dictionary dictionary = p_value;
			const Variant key = dictionary.keys()[misfit];
			found = kept_fits(*p_type.key, key) ? vformat("%s for the key %s", value_description(dictionary[key]), UtilityFunctions::var_to_str(key)) : vformat("the key %s", value_description(key));
		}
		return vformat("%sexpected %s, found %s", at, type_description(p_type), found);
	}
	if (p_type.kind == bytecode::TypeKind::Enum && type_of(p_value) == Variant::INT) {
		return vformat("%sexpected %s, found %d, which is none of its values", at, type_description(p_type), int64_t(p_value));
	}
	if (is_struct(p_type) && type_of(p_value) == Variant::DICTIONARY) {
		// Which field: "Hit.damage: expected int, found String", or of a
		// variant: "Command.MOVE.target is missing".
		const Dictionary encoded = p_value;
		ValueBuffer slot(p_type);
		String field_path = where;
		if (is_payload_enum(p_type)) {
			const int64_t variant = encoded_variant(p_type, encoded);
			if (variant < 0) {
				return vformat("%sexpected %s, found a Dictionary whose \"%s\" names none of its variants", at, type_description(p_type), VARIANT_KEY);
			}
			tag(slot.data) = uint64_t(variant);
			field_path += "." + p_type.enum_declared[variant].first;
		}
		const FieldImage *field = nullptr;
		if (!decode_fields(p_type, encoded, slot.data, true, &field)) {
			field_path += "." + field->name;
			return encoded.has(field->name) ? mismatch(*field->type, encoded[field->name], field_path, true) : field_path + " is missing";
		}
	}
	if (is_option(p_type) || is_struct(p_type) || (p_type.storage == ValueType::Callable && !p_type.name.is_empty())) {
		return vformat("%sexpected %s, found %s", at, type_description(p_type), value_description(p_value));
	}
	return at + entry_mismatch(p_type.storage, p_value, p_type.check);
}

// A mismatch described without computing defaults again.
static String described(const TypeImage &p_type, const Variant &p_value, bool p_decoding) {
	const bool was = describing;
	describing = true;
	const String description = mismatch(p_type, p_value, String(), p_decoding);
	describing = was;
	return description;
}

String entry_mismatch(const TypeImage &p_type, const Variant &p_value) {
	return described(p_type, p_value, false);
}

bool entry_check(const TypeImage &p_type, const Variant &p_value, uint8_t *p_slot, String &r_mismatch) {
	const uint64_t failures = defaults_failed;
	if (from_variant(p_type, p_value, p_slot, true)) {
		return true;
	}
	r_mismatch = defaults_failed == failures ? entry_mismatch(p_type, p_value) : String();
	return false;
}

Variant default_variant(const TypeImage &p_type) {
	if (is_struct(p_type)) {
		ValueBuffer slot(p_type);
		return to_variant(p_type, slot.data);
	}
	return is_option(p_type) ? Variant() : default_variant(p_type.storage);
}

Variant::Type variant_type(const TypeImage &p_type) {
	return is_struct(p_type) ? Variant::DICTIONARY : variant_type(p_type.storage);
}

template <typename Fn>
static bool all_leaves_match(const TypeImage &p_type, const uint8_t *p_a, const uint8_t *p_b, Fn &&p_fn) {
	if (is_option(p_type)) {
		return tag(p_a) == tag(p_b) && (!tag(p_a) || all_leaves_match(*p_type.element, p_a + 8, p_b + 8, p_fn));
	}
	if (is_struct(p_type)) {
		// A PayloadEnum's index of its variant first.
		return all_held_fields(p_type, p_a, [&](const FieldImage &p_field) { return all_leaves_match(*p_field.type, p_a + p_field.offset, p_b + p_field.offset, p_fn); });
	}
	return p_fn(p_type.storage, p_a, p_b);
}

static bool equal_leaf(ValueType p_type, const uint8_t *p_a, const uint8_t *p_b, bool p_hash_compare) {
	switch (p_type) {
		case ValueType::Bool:
		case ValueType::Int:
			return *reinterpret_cast<const uint64_t *>(p_a) == *reinterpret_cast<const uint64_t *>(p_b);
		case ValueType::Float: {
			const double a = *reinterpret_cast<const double *>(p_a);
			const double b = *reinterpret_cast<const double *>(p_b);
			return a == b || (p_hash_compare && std::isnan(a) && std::isnan(b));
		}
		case ValueType::Object:
			return object_data(p_a).id == object_data(p_b).id;
		case ValueType::Task:
			return task_at(p_a).get() == task_at(p_b).get();
		default:
			if (p_hash_compare) {
				return to_variant(p_type, p_a).hash_compare(to_variant(p_type, p_b));
			}
			if (p_type == ValueType::Variant) {
				Variant result;
				bool valid = false;
				Variant::evaluate(Variant::OP_EQUAL, variant_at(p_a), variant_at(p_b), result, valid);
				return valid && result.booleanize();
			}
			alignas(8) uint64_t result = 0;
			ops(p_type).equal(p_a, p_b, &result);
			return result != 0;
	}
}

bool equal_values(const TypeImage &p_type, const uint8_t *p_a, const uint8_t *p_b) {
	return all_leaves_match(p_type, p_a, p_b, [](ValueType p_storage, const uint8_t *p_x, const uint8_t *p_y) { return equal_leaf(p_storage, p_x, p_y, false); });
}

/* What boxes do */

namespace {

// As containers match keys and elements: no int to float, NaN matches NaN
// (spec §5.2, core/variant/variant.cpp:3138-3139).
bool same_values(const TypeImage &p_type, const uint8_t *p_a, const uint8_t *p_b) {
	return all_leaves_match(p_type, p_a, p_b, [](ValueType p_storage, const uint8_t *p_x, const uint8_t *p_y) { return equal_leaf(p_storage, p_x, p_y, true); });
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
		all_held_fields(p_type, p_slot, [&](const FieldImage &p_field) {
			hash = hash_murmur3_one_32(hash_of(*p_field.type, p_slot + p_field.offset), hash);
			return true;
		});
		return hash_fmix32(hash);
	}
	if (p_type.storage == ValueType::Object) {
		return hash_one_uint64(object_data(p_slot).id);
	}
	if (p_type.storage == ValueType::Task) {
		return hash_one_uint64(uint64_t(uintptr_t(task_at(p_slot).get())));
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

} // namespace

String value_text(const TypeImage &p_type, const uint8_t *p_slot) {
	if (is_option(p_type)) {
		return tag(p_slot) ? value_text(*p_type.element, p_slot + 8) : String("None");
	}
	if (is_result(p_type)) {
		const FieldImage &field = p_type.fields[1 + tag(p_slot)];
		return vformat("%s(%s)", p_type.enum_declared[tag(p_slot)].first, value_text(*field.type, p_slot + field.offset));
	}
	if (is_struct(p_type)) {
		String text = type_description(p_type);
		String fields;
		all_held_fields(p_type, p_slot, [&](const FieldImage &p_field) {
			if (is_payload_enum(p_type) && &p_field == &p_type.fields[0]) {
				text += "." + p_type.enum_declared[tag(p_slot)].first;
			} else {
				fields += (fields.is_empty() ? "" : ", ") + p_field.name + " = " + value_text(*p_field.type, p_slot + p_field.offset);
			}
			return true;
		});
		return is_payload_enum(p_type) && fields.is_empty() ? text : text + "(" + fields + ")";
	}
	const Variant value = outward_variant(p_type, p_slot);
	if (is_none(value)) {
		return "None";
	}
	return type_of(value) == Variant::STRING || type_of(value) == Variant::STRING_NAME ? vformat("\"%s\"", value) : String(value);
}

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

// The copies of containers by their identity: an Array or a Dictionary is a
// pointer to its contents (core/variant/array.h:48, dictionary.h:47). Made
// at the first container, since most keys hold none.
using Copies = std::optional<std::unordered_map<uint64_t, Variant>>;

// Copies a value's containers level by level: for deep_copy() (`!p_freeze`,
// with `p_mode` for duplicate_deep()), or for frozen_copy() (`p_freeze`),
// since make_read_only() leaves inner containers alone and cannot be undone
// (core/variant/array.cpp:938-942, core/variant/dictionary.cpp:599-603).
// A container that holds itself gets a copy that holds itself. A frozen copy
// shares what the value shares; deep_copy() copies a container once for each
// place that holds it, as duplicate(true) does (core/variant/array.cpp:553-555).
static Variant copy_containers(const Variant &p_value, int64_t p_mode, bool p_freeze, Copies &r_copies) {
	if (const Box *box = as_box(p_value)) {
		const TypeImage &type = box->type->type();
		if (!box->type->holds_containers && (p_freeze ? box->is_key : p_mode < 0)) {
			return p_value;
		}
		ValueBuffer copy(type);
		copy_value(type, copy.data, box->value());
		for_each_active_leaf(type, copy.data, [&](ValueType p_storage, uint8_t *p_leaf) {
			if (p_storage == ValueType::Array) {
				Array &array = *reinterpret_cast<Array *>(p_leaf);
				array = copy_containers(array, p_mode, p_freeze, r_copies);
			} else if (p_storage == ValueType::Dictionary) {
				Dictionary &dictionary = *reinterpret_cast<Dictionary *>(p_leaf);
				dictionary = copy_containers(dictionary, p_mode, p_freeze, r_copies);
			} else if (p_storage == ValueType::Object && p_mode >= 0 && !is_none_object(p_leaf)) {
				ObjectView view(p_leaf);
				Array wrapped;
				wrapped.push_back(view.get());
				set_object(p_leaf, wrapped.duplicate_deep(p_mode)[0]);
			}
		});
		return make_box(type, copy.data, p_freeze || box->is_key);
	}
	const bool array = type_of(p_value) == Variant::ARRAY;
	if (!array && type_of(p_value) != Variant::DICTIONARY) {
		return p_value;
	}
	if (!p_freeze && !keeps_boxes(p_value)) {
		if (array) {
			return p_mode < 0 ? Array(p_value).duplicate(true) : Array(p_value).duplicate_deep(p_mode);
		}
		return p_mode < 0 ? Dictionary(p_value).duplicate(true) : Dictionary(p_value).duplicate_deep(p_mode);
	}
	auto &copies = r_copies ? *r_copies : r_copies.emplace();
	const uint64_t id = object_id_of(p_value);
	if (const auto found = copies.find(id); found != copies.end()) {
		return found->second;
	}
	Variant result;
	if (array) {
		const Array source = p_value;
		Array copy = source.duplicate(false);
		copies.emplace(id, copy);
		for (int64_t i = 0; i < source.size(); i++) {
			copy[i] = copy_containers(source[i], p_mode, p_freeze, r_copies);
		}
		if (p_freeze) {
			copy.make_read_only();
		}
		result = copy;
	} else {
		const Dictionary source = p_value;
		Dictionary copy = source.duplicate(false);
		copies.emplace(id, copy);
		const Array keys = source.keys();
		if (p_freeze) {
			copy.clear();
			for (int64_t i = 0; i < keys.size(); i++) {
				const Variant key = copy_containers(keys[i], -1, true, r_copies);
				copy[key] = copy_containers(source[keys[i]], -1, true, r_copies);
			}
			copy.make_read_only();
		} else {
			// Keys are read-only (spec §3.8), so the copy shares them.
			for (int64_t i = 0; i < keys.size(); i++) {
				copy[keys[i]] = copy_containers(source[keys[i]], p_mode, false, r_copies);
			}
		}
		result = copy;
	}
	if (!p_freeze) {
		copies.erase(id);
	}
	return result;
}

Variant deep_copy(const Variant &p_value, int64_t p_mode) {
	Copies copies;
	return copy_containers(p_value, p_mode, false, copies);
}

bool less_values(const TypeImage &p_type, const uint8_t *p_a, const uint8_t *p_b) {
	if (is_option(p_type)) {
		if (!tag(p_a) || !tag(p_b)) {
			return !tag(p_a) && tag(p_b);
		}
		return less_values(*p_type.element, p_a + 8, p_b + 8);
	}
	if (is_struct(p_type)) {
		// The first field in which one is less decides, as in Array's `<`
		// (core/variant/array.cpp:164-179); a PayloadEnum's index of its
		// variant first (spec §3.7).
		bool less = false;
		all_held_fields(p_type, p_a, [&](const FieldImage &p_field) {
			less = less_values(*p_field.type, p_a + p_field.offset, p_b + p_field.offset);
			return !less && !less_values(*p_field.type, p_b + p_field.offset, p_a + p_field.offset);
		});
		return less;
	}
	switch (p_type.storage) {
		case ValueType::Bool:
		case ValueType::Int:
			return *reinterpret_cast<const int64_t *>(p_a) < *reinterpret_cast<const int64_t *>(p_b);
		case ValueType::Float:
			return *reinterpret_cast<const double *>(p_a) < *reinterpret_cast<const double *>(p_b);
		default:
			return variant_less(to_variant(p_type.storage, p_a), to_variant(p_type.storage, p_b));
	}
}

bool variant_less(const Variant &p_a, const Variant &p_b) {
	const Box *a = as_box(p_a);
	const Box *b = as_box(p_b);
	if (a && b) {
		return a->type == b->type && less_values(a->type->type(), a->value(), b->value());
	}
	if (type_of(p_a) == Variant::ARRAY && type_of(p_b) == Variant::ARRAY && (keeps_boxes(p_a) || keeps_boxes(p_b))) {
		const Array &x = *reinterpret_cast<const Array *>(data_of(p_a));
		const Array &y = *reinterpret_cast<const Array *>(data_of(p_b));
		for (int64_t i = 0; i < x.size() && i < y.size(); i++) {
			if (variant_less(x[i], y[i])) {
				return true;
			}
			if (variant_less(y[i], x[i])) {
				return false;
			}
		}
		return x.size() < y.size();
	}
	Variant result;
	bool valid = false;
	Variant::evaluate(Variant::OP_LESS, p_a, p_b, result, valid);
	return valid && result.booleanize();
}

namespace {

class LessCallable : public CallableCustom {
	static bool equal(const CallableCustom *p_a, const CallableCustom *p_b) { return p_a == p_b; }
	static bool less(const CallableCustom *p_a, const CallableCustom *p_b) { return p_a < p_b; }

public:
	uint32_t hash() const override { return uint32_t(uintptr_t(this) >> 4); }
	String get_as_text() const override { return "<"; }
	CompareEqualFunc get_compare_equal_func() const override { return equal; }
	CompareLessFunc get_compare_less_func() const override { return less; }
	bool is_valid() const override { return true; }
	ObjectID get_object() const override { return ObjectID(); }
	int get_argument_count(bool &r_is_valid) const override {
		r_is_valid = true;
		return 2;
	}
	void call(const Variant **p_arguments, int p_argcount, Variant &r_return_value, GDExtensionCallError &r_call_error) const override {
		if (p_argcount != 2) {
			r_call_error.error = p_argcount < 2 ? GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS : GDEXTENSION_CALL_ERROR_TOO_MANY_ARGUMENTS;
			r_call_error.expected = 2;
			return;
		}
		r_return_value = variant_less(*p_arguments[0], *p_arguments[1]);
		r_call_error.error = GDEXTENSION_CALL_OK;
	}
};

} // namespace

Callable less_callable() {
	return Callable(memnew(LessCallable()));
}

void SlotLayout::construct(uint8_t *p_base) const {
	std::memset(p_base, 0, size);
	for (const auto &[offset, type] : constructed) {
		ops(type).construct(p_base + offset, nullptr);
	}
	for (const bytecode::Preset &preset : presets) {
		std::memcpy(p_base + preset.offset, &preset.bits, sizeof(preset.bits));
	}
}

void SlotLayout::destroy(uint8_t *p_base) const {
	for (const auto &[offset, type] : destroyed) {
		destroy_value(type, p_base + offset);
	}
}

void SlotLayout::let_go(uint8_t *p_base, uint64_t p_id, std::vector<std::pair<uint32_t, ValueType>> &r_slots) const {
	for (const auto &[offset, type] : destroyed) {
		uint8_t *slot = p_base + offset;
		if (type == ValueType::Object && holds_reference(object_data(slot)) && object_data(slot).id == p_id) {
			destroy_value(type, slot);
			r_slots.emplace_back(offset, type);
		} else if (type == ValueType::Variant && type_of(variant_at(slot)) == Variant::OBJECT && object_data(slot + 8).id == p_id) {
			// An OBJECT Variant holds the object's ObjectID and pointer after its type.
			variant_at(slot) = Variant();
			r_slots.emplace_back(offset, type);
		}
	}
}

void SlotLayout::hold_again(uint8_t *p_base, const Variant &p_object, const std::vector<std::pair<uint32_t, ValueType>> &p_slots) {
	for (const auto &[offset, type] : p_slots) {
		if (type == ValueType::Object) {
			set_object(p_base + offset, p_object);
		} else {
			variant_at(p_base + offset) = p_object;
		}
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
	if (p_type == ValueType::Task) {
		task_at(p_dst) = task_at(p_src);
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

void get_packed_element(ValueType p_type, const uint8_t *p_array, int64_t p_index, uint8_t *p_element) {
	ops(p_type).get_indexed(p_array, p_index, p_element);
}

void set_packed_element(ValueType p_type, uint8_t *p_array, int64_t p_index, const uint8_t *p_element) {
	ops(p_type).set_indexed(p_array, p_index, p_element);
}

// A value of a built-in type, by its one return as the named Variant.
static Variant builtin_variant(ValueType p_type, const uint8_t *p_slot) {
	Variant result;
	// Written into the still empty Variant, a scalar without the engine.
	if (is_scalar(p_type)) {
		make_scalar_variant(p_type, p_slot, reinterpret_cast<uint8_t *>(&result));
	} else if (p_type != ValueType::Void) {
		ops(p_type).to_variant(result._native_ptr(), const_cast<uint8_t *>(p_slot));
	}
	return result;
}

Variant to_variant(ValueType p_type, const uint8_t *p_slot) {
	if (p_type == ValueType::Object) {
		ObjectView view(p_slot);
		return view.get();
	}
	if (p_type == ValueType::Variant) {
		return variant_at(p_slot);
	}
	if (p_type == ValueType::Task) {
		const TaskRef &task = task_at(p_slot);
		return task ? Variant(task->object()) : Variant();
	}
	return builtin_variant(p_type, p_slot);
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
			take_object(p_slot, p_value);
			return true;
		case ValueType::Task: {
			if (is_none(p_value)) {
				if (p_check && !p_check->nullable) {
					return false;
				}
				destroy_value(ValueType::Task, p_slot);
				return true;
			}
			TaskRef task = task_of(p_value);
			if (!task || (p_check && task->result_signature != p_check->task_result)) {
				return false;
			}
			task_at(p_slot) = std::move(task);
			return true;
		}
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
		case ValueType::Int:
		case ValueType::Float:
			return scalar_from_variant(p_type, p_value, p_slot);
		case ValueType::Callable:
			if (p_check && !p_check->nullable && have == Variant::CALLABLE && Callable(p_value).is_null()) {
				return false;
			}
			break;
		default:
			break;
	}
	if (have != variant_type(p_type) && !converts(have, variant_type(p_type))) {
		return false;
	}
	// The engine constructs into memory that holds no value, converting as
	// the Variant's operator to the type does (core/variant/variant_internal.h:1009-1012).
	const TypeOps &type = ops(p_type);
	if (type.destroy) {
		type.destroy(p_slot);
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

namespace {
InnerScriptResolver inner_script = nullptr;
ScriptPathResolver path_of_script = nullptr;
} // namespace

void set_inner_script_resolvers(InnerScriptResolver p_inner, ScriptPathResolver p_path) {
	inner_script = p_inner;
	path_of_script = p_path;
}

String script_path(const Ref<Script> &p_script) {
	return p_script.is_null() ? String() : (path_of_script ? path_of_script(p_script) : p_script->get_path());
}

Variant load_resource(const String &p_path) {
	if (p_path.is_empty()) {
		return Variant();
	}
	const int64_t inner = p_path.find("::");
	if (inner < 0) {
		return ResourceLoader::get_singleton()->call("load", p_path);
	}
	Variant script = ResourceLoader::get_singleton()->call("load", p_path.substr(0, inner));
	for (const String &name : p_path.substr(inner + 2).split("::")) {
		script = inner_script && type_of(script) == Variant::OBJECT ? inner_script(script, name) : Variant();
	}
	return script;
}

Array typed_array(ValueType p_element, const StringName &p_class, const String &p_script) {
	Array array;
	if (p_element != ValueType::Void) {
		array.set_typed(uint32_t(variant_type(p_element)), p_class, load_resource(p_script));
	}
	return array;
}

Dictionary typed_dictionary(const ValueCheck &p_check) {
	Dictionary dictionary;
	if (p_check.key != ValueType::Void) {
		dictionary.set_typed(uint32_t(variant_type(p_check.key)), p_check.key_class_name, load_resource(p_check.key_script), uint32_t(variant_type(p_check.element)), p_check.class_name, load_resource(p_check.script));
	}
	return dictionary;
}

Variant frozen_copy(const Variant &p_key) {
	Copies copies;
	return copy_containers(p_key, -1, true, copies);
}

// `r_frozen` holds the containers met, by identity, as Copies does.
static void freeze(const Variant &p_key, std::optional<std::unordered_set<uint64_t>> &r_frozen) {
	if (const Box *box = as_box(p_key)) {
		for_each_active_leaf(box->type->type(), box->value(), [&](ValueType p_storage, const uint8_t *p_leaf) {
			if (p_storage == ValueType::Array || p_storage == ValueType::Dictionary) {
				freeze(to_variant(p_storage, p_leaf), r_frozen);
			}
		});
		return;
	}
	if (type_of(p_key) != Variant::ARRAY && type_of(p_key) != Variant::DICTIONARY) {
		return;
	}
	auto &frozen = r_frozen ? *r_frozen : r_frozen.emplace();
	if (!frozen.insert(object_id_of(p_key)).second) {
		return;
	}
	if (type_of(p_key) == Variant::ARRAY) {
		Array array = p_key;
		for (int64_t i = 0; i < array.size(); i++) {
			freeze(array[i], r_frozen);
		}
		array.make_read_only();
	} else {
		Dictionary dictionary = p_key;
		const Array keys = dictionary.keys();
		for (int64_t i = 0; i < keys.size(); i++) {
			freeze(keys[i], r_frozen);
			freeze(dictionary[keys[i]], r_frozen);
		}
		dictionary.make_read_only();
	}
}

void freeze(const Variant &p_key) {
	std::optional<std::unordered_set<uint64_t>> frozen;
	freeze(p_key, frozen);
}

bool keys_hold_containers(const Dictionary &p_dictionary) {
	const int64_t key = p_dictionary.get_typed_key_builtin();
	return key == Variant::NIL || key == Variant::ARRAY || key == Variant::DICTIONARY || key == Variant::CALLABLE;
}

Variant::Type variant_type(ValueType p_type) {
	if (p_type == ValueType::Task) {
		return Variant::OBJECT;
	}
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
		if (script_path(script) == p_path) {
			return true;
		}
	}
	return false;
}

bool extends_script(const Variant &p_script, const Variant &p_base) {
	const Object *base = p_base.get_validated_object();
	for (Ref<Script> script = p_script; script.is_valid(); script = script->get_base_script()) {
		if (script.ptr() == base) {
			return true;
		}
	}
	return false;
}

namespace {

bool exactly_typed(int64_t p_builtin, const StringName &p_class, const Variant &p_script, ValueType p_type, const StringName &p_want_class, const String &p_want_script) {
	return p_builtin == int64_t(variant_type(p_type)) && (p_type != ValueType::Object || (p_class == p_want_class && script_path(p_script) == p_want_script));
}

// As a value is assigned to an element of the type: converted implicitly
// (spec §3.14); None to an object type, which the engine keeps alike for T and
// T?; a live object of the class and script (§3.3).
bool fits_typed(int64_t p_builtin, const StringName &p_class, const Variant &p_script, const Variant &p_value) {
	const Variant::Type want = Variant::Type(p_builtin);
	const Variant::Type have = type_of(p_value);
	if (want == Variant::NIL) {
		return true;
	}
	if (is_none(p_value)) {
		return want == Variant::OBJECT;
	}
	if (want == Variant::OBJECT) {
		const Object *object = have == Variant::OBJECT ? p_value.get_validated_object() : nullptr;
		return object && (p_class.is_empty() || object->is_class(p_class)) && has_script(object->get_script(), script_path(p_script));
	}
	return have == want || converts(have, want);
}

} // namespace

String describe_value(const Variant &p_value) {
	return value_description(p_value);
}

bool is_exactly(const TypeImage &p_type, const Variant &p_value) {
	if (type_of(p_value) != variant_type(p_type.storage)) {
		return false;
	}
	const ValueCheck &check = p_type.check;
	if (p_type.storage == ValueType::Array && check.element != ValueType::Void) {
		const Array array = p_value;
		return exactly_typed(array.get_typed_builtin(), array.get_typed_class_name(), array.get_typed_script(), check.element, check.class_name, check.script);
	}
	if (p_type.storage == ValueType::Dictionary && check.key != ValueType::Void) {
		const Dictionary dictionary = p_value;
		return exactly_typed(dictionary.get_typed_key_builtin(), dictionary.get_typed_key_class_name(), dictionary.get_typed_key_script(), check.key, check.key_class_name, check.key_script) &&
				exactly_typed(dictionary.get_typed_value_builtin(), dictionary.get_typed_value_class_name(), dictionary.get_typed_value_script(), check.element, check.class_name, check.script);
	}
	return true;
}

String untyped_misfit(const Variant &p_container, ElementRole p_role, const Variant &p_value) {
	int64_t builtin = Variant::NIL;
	StringName class_name;
	Variant script;
	const bool key = p_role == ElementRole::Key || p_role == ElementRole::Keys;
	if (type_of(p_container) == Variant::ARRAY) {
		const Array array = p_container;
		builtin = array.get_typed_builtin();
		class_name = array.get_typed_class_name();
		script = array.get_typed_script();
	} else if (key) {
		const Dictionary dictionary = p_container;
		builtin = dictionary.get_typed_key_builtin();
		class_name = dictionary.get_typed_key_class_name();
		script = dictionary.get_typed_key_script();
	} else if (p_role != ElementRole::Entries) {
		const Dictionary dictionary = p_container;
		builtin = dictionary.get_typed_value_builtin();
		class_name = dictionary.get_typed_value_class_name();
		script = dictionary.get_typed_value_script();
	}
	const auto misfit = [&](const Variant &p_item, const String &p_where) {
		if (fits_typed(builtin, class_name, script, p_item)) {
			return String();
		}
		const String holds = key ? String("has keys of type") : String("holds");
		return vformat("%s %s %s, not %s%s.", value_description(p_container), holds, typed_description(builtin, class_name, script), value_description(p_item), p_where);
	};
	switch (p_role) {
		case ElementRole::Element:
		case ElementRole::Key:
		case ElementRole::Value:
			return misfit(p_value, String());
		case ElementRole::Elements:
		case ElementRole::Keys: {
			const Array items = p_value;
			for (int64_t i = 0; i < items.size(); i++) {
				const String error = misfit(items[i], vformat(", which the Array given has at index %d", i));
				if (!error.is_empty()) {
					return error;
				}
			}
			return String();
		}
		case ElementRole::Entries: {
			const Dictionary entries = p_value;
			const Array keys = entries.keys();
			for (int64_t i = 0; i < keys.size(); i++) {
				String error = untyped_misfit(p_container, ElementRole::Key, keys[i]);
				if (error.is_empty()) {
					error = untyped_misfit(p_container, ElementRole::Value, entries[keys[i]]);
				}
				if (!error.is_empty()) {
					return error;
				}
			}
			return String();
		}
	}
	return String();
}

bool is_live_object(const uint8_t *p_slot) {
	const ObjectData &data = object_data(p_slot);
	// A reference keeps a RefCounted object alive; the engine finds any other
	// by its ObjectID only while it lives.
	return data.object && (holds_reference(data) || gdextension_interface::object_get_instance_from_id(data.id));
}

bool same_object(const uint8_t *p_a, bool p_a_optional, const uint8_t *p_b, bool p_b_optional) {
	const auto identity = [](const uint8_t *p_slot, bool p_optional) -> uint64_t {
		const ObjectData &data = object_data(p_slot);
		return !data.object || (p_optional && !is_live_object(p_slot)) ? 0 : data.id;
	};
	return identity(p_a, p_a_optional) == identity(p_b, p_b_optional);
}

Variant outward_variant(const TypeImage &p_type, const uint8_t *p_data) {
	if (p_type.storage == ValueType::Object && p_type.check.nullable && !is_live_object(p_data)) {
		return Variant();
	}
	return to_variant(p_type, p_data);
}

bool check_value_sizes(String &r_error) {
	// The engine's sizes for this build against the bytecode's size table.
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
