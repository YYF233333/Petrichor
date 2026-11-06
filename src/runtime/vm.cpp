#include "runtime/vm.h"

#include "bytecode/opcodes.h"
#include "runtime/debugger.h"
#include "runtime/method_callable.h"
#include "runtime/object_watch.h"
#include "runtime/task.h"
#include "runtime/text.h"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/callable_custom.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <thread>

using namespace godot;

namespace petrichor {

using bytecode::NO_SLOT;
using bytecode::Op;
using bytecode::ValueType;

InstanceStorage::InstanceStorage(const std::shared_ptr<const ClassImage> &p_image) :
		image(p_image), members(static_cast<uint8_t *>(::operator new(p_image->members_layout.size))) {
	image->members_layout.construct(members);
}

InstanceStorage::~InstanceStorage() {
	image->members_layout.destroy(members);
	::operator delete(members);
}

struct CoroutineFrame : std::enable_shared_from_this<CoroutineFrame> {
	std::shared_ptr<const ClassImage> image;
	InstanceContext context;
	CoroutineTask *task;
	uint32_t function;
	uint8_t *data;
	size_t pc = 0;
	int line = 0;
	uint32_t destination = NO_SLOT;
	// Also read by the callbacks of what the frame awaits, on the thread they run on.
	std::atomic<uint64_t> generation{ 0 };
	// The awaited object was freed: tasks::frame() fails the wait once the
	// frame's deletions are done, unless the waiter's own object went too (spec §9.7).
	std::atomic<bool> awaited_freed{ false };
	std::atomic<uint64_t> lost_object{ 0 };
	Signal waiting;
	Callable callback;
	Signal script_changed;
	Callable changed_callback;
	object_watch::Subscription watch;
	Variant held_state;
	Ref<CoroutineTask> waited_task;
	Ref<CoroutineTask> child;

	CoroutineFrame(const std::shared_ptr<const ClassImage> &p_image, uint32_t p_function, const InstanceContext &p_context, CoroutineTask *p_task) :
			image(p_image), context(p_context), task(p_task), function(p_function), data(static_cast<uint8_t *>(::operator new(p_image->frames[p_function].size))) {
		image->frames[function].construct(data);
		line = int(image->module.functions[function].line);
	}
	~CoroutineFrame() {
		disconnect();
		if (data) {
			image->frames[function].destroy(data);
		}
		::operator delete(data);
	}
	void disconnect();
	bool receive(const Variant &p_value);
	bool await_task(const Ref<CoroutineTask> &p_task);
	void report_error(const String &p_message);
	void resume();
	void complete_wait(uint64_t p_generation, const Variant &p_value);
	bool suspend(const Signal &p_signal, uint32_t p_dst, uint64_t p_watch, bool p_script = false);
};

namespace {
std::mutex tasks_mutex;
std::atomic<uint64_t> task_frame{ 0 };
std::map<uint64_t, Ref<CoroutineTask>> active_tasks;

std::vector<Ref<CoroutineTask>> task_snapshot() {
	std::lock_guard<std::mutex> lock(tasks_mutex);
	std::vector<Ref<CoroutineTask>> tasks;
	for (const auto &entry : active_tasks) {
		tasks.push_back(entry.second);
	}
	return tasks;
}

class ResumeCallable : public CallableCustom {
	std::function<void(const Variant &)> callback;
	static bool equal(const CallableCustom *a, const CallableCustom *b) { return a == b; }
	static bool less(const CallableCustom *a, const CallableCustom *b) { return std::less<const CallableCustom *>()(a, b); }

public:
	explicit ResumeCallable(std::function<void(const Variant &)> p_callback) : callback(std::move(p_callback)) {}
	uint32_t hash() const override { return uint32_t(reinterpret_cast<uintptr_t>(this)); }
	String get_as_text() const override { return "Petrichor await"; }
	CompareEqualFunc get_compare_equal_func() const override { return equal; }
	CompareLessFunc get_compare_less_func() const override { return less; }
	ObjectID get_object() const override { return ObjectID(); }
	bool is_valid() const override { return true; }
	int get_argument_count(bool &r_valid) const override {
		r_valid = true;
		return 0;
	}
	void call(const Variant **p_args, int p_count, Variant &r_result, GDExtensionCallError &r_error) const override {
		r_error.error = GDEXTENSION_CALL_OK;
		Variant value;
		if (p_count == 1) {
			value = *p_args[0];
		} else if (p_count > 1) {
			Array args;
			for (int i = 0; i < p_count; ++i) {
				args.push_back(*p_args[i]);
			}
			value = args;
		}
		callback(value);
	}
};

// `p_result_key` is the result as the caller sees it, which an override may
// give as another type (spec §4.6).
Ref<CoroutineTask> start_task(const std::shared_ptr<const ClassImage> &p_image, uint32_t p_function, const InstanceContext &p_context, uint8_t *p_frame, const std::string *p_result_key = nullptr) {
	const bytecode::Function &fn = p_image->module.functions[p_function];
	Ref<CoroutineTask> task;
	task.instantiate();
	task->owner_id = p_context.owner ? uint64_t(p_context.owner->get_instance_id()) : 0;
	task->owner_storage = p_context.storage.get();
	task->result_signature = p_result_key ? *p_result_key : p_image->types[fn.result.type].result_key;
	task->continuation = std::make_shared<CoroutineFrame>(p_image, p_function, p_context, task.ptr());
	for (const auto &param : fn.params) {
		copy_value(p_image->types[param.type], task->continuation->data + param.offset, p_frame + param.offset);
	}
	{
		std::lock_guard<std::mutex> lock(tasks_mutex);
		active_tasks.emplace(uint64_t(task->get_instance_id()), task);
	}
	if (p_context.storage && p_context.storage->coroutines_cancelled) {
		task->cancel_requested = true;
		task->owner_freed = true;
		task->owner_freed_frame = task_frame.load();
	}
	task->continuation->resume();
	return task;
}

// What a static function, which has no instance, reads as its instance's state.
const std::atomic<bool> NO_INSTANCE_FREED{ false };

// GDScript's default limit (debug/settings/gdscript/max_call_stack,
// modules/gdscript/gdscript.cpp:2870); deeper recursion is a program error
// (spec §8.1) instead of a crash.
constexpr size_t MAX_CALL_DEPTH = 1024;

// The frames of this language on this thread, innermost last, for error lines
// and the debugger. It never grows past its first reservation, so that a
// function's record stays put while it runs.
thread_local std::vector<StackFrame> call_stack;

// The memory of the frames of this language on this thread, taken and given
// back last in first out, from blocks that do not move.
class FrameStack {
	static constexpr size_t BLOCK_SIZE = 64 * 1024;
	struct Block {
		std::unique_ptr<uint8_t[]> data;
		size_t size = 0;
		size_t used = 0;
	};
	std::vector<Block> blocks;
	size_t current = 0;

	static size_t aligned(uint32_t p_size) { return (size_t(p_size) + 15) & ~size_t(15); }

public:
	uint8_t *push(uint32_t p_size) {
		const size_t size = aligned(p_size);
		while (current < blocks.size() && blocks[current].size - blocks[current].used < size) {
			current++;
		}
		if (current == blocks.size()) {
			const size_t block_size = std::max(BLOCK_SIZE, size);
			blocks.push_back(Block{ std::make_unique<uint8_t[]>(block_size), block_size, 0 });
		}
		Block &block = blocks[current];
		uint8_t *data = block.data.get() + block.used;
		block.used += size;
		return data;
	}

	void pop(uint32_t p_size) {
		blocks[current].used -= aligned(p_size);
		while (current > 0 && blocks[current].used == 0) {
			current--;
		}
	}
};

thread_local FrameStack frame_stack;

// A frame of a function, constructed and destroyed by its layout.
class Frame {
	FrameStack &stack;
	const SlotLayout &layout;

public:
	uint8_t *const data;

	Frame(FrameStack &p_stack, const SlotLayout &p_layout) :
			stack(p_stack), layout(p_layout), data(stack.push(p_layout.size)) {
		layout.construct(data);
	}
	~Frame() {
		if (!layout.destroyed.empty()) {
			layout.destroy(data);
		}
		stack.pop(layout.size);
	}
	Frame(const Frame &) = delete;
	Frame &operator=(const Frame &) = delete;
};

// Where an engine function reads a right operand that a unary operator does not have.
alignas(16) const uint8_t NOTHING[64] = {};

// The arguments of a call to the engine, from the instruction's (type, offset)
// pairs: each in its slot's own layout, or as a Variant where the function
// takes a Variant, for extra vararg arguments, or for every argument of a call
// by name, an object as a view of its slot. Kept on the C++ stack up to a few
// arguments.
class Arguments {
	static constexpr uint32_t INLINE = 8;
	struct Heap {
		std::vector<const void *> pointers;
		std::vector<Variant> variants;
		std::vector<ObjectView> views;
	};
	const void *inline_pointers[INLINE];
	alignas(Variant) uint8_t inline_variants[INLINE * sizeof(Variant)];
	uint32_t variant_count = 0;
	alignas(ObjectView) uint8_t inline_views[INLINE * sizeof(ObjectView)];
	uint32_t view_count = 0;
	Heap *heap = nullptr;

public:
	const void **pointers;

	Arguments(const std::vector<TypeImage> &p_types, uint8_t *p_frame, const uint32_t *p_pairs, uint32_t p_count, const std::vector<ValueType> *p_params) {
		if (p_count > INLINE) {
			heap = new Heap();
			heap->pointers.resize(p_count);
			heap->variants.reserve(p_count); // Pointers into them stay valid.
			heap->views.reserve(p_count);
		}
		pointers = heap ? heap->pointers.data() : inline_pointers;
		for (uint32_t i = 0; i < p_count; i++) {
			const TypeImage &type = p_types[p_pairs[2 * i]];
			uint8_t *slot = p_frame + p_pairs[2 * i + 1];
			if (p_params && i < p_params->size() && (*p_params)[i] != ValueType::Variant) {
				pointers[i] = slot;
			} else if (type.storage == ValueType::Variant) {
				pointers[i] = slot;
			} else if (type.storage == ValueType::Object) {
				// A view needs no destruction.
				ObjectView *view = heap ? &heap->views.emplace_back(slot) : new (inline_views + view_count++ * sizeof(ObjectView)) ObjectView(slot);
				pointers[i] = &view->get();
			} else if (heap) {
				heap->variants.push_back(to_variant(type, slot));
				pointers[i] = &heap->variants.back();
			} else {
				pointers[i] = new (inline_variants + variant_count++ * sizeof(Variant)) Variant(to_variant(type, slot));
			}
		}
	}
	~Arguments() {
		for (uint32_t i = 0; i < variant_count; i++) {
			reinterpret_cast<Variant *>(inline_variants)[i].~Variant();
		}
		delete heap;
	}
	Arguments(const Arguments &) = delete;
	Arguments &operator=(const Arguments &) = delete;

	const Variant **as_variants() { return reinterpret_cast<const Variant **>(pointers); }
};

// A static method of an engine class, by name (core/core_bind.cpp:1722-1737).
void call_static(const StringName &p_class, const StringName &p_method, const Variant **p_args, uint32_t p_count, Variant &r_result, GDExtensionCallError &r_error) {
	const Variant names[2] = { p_class, p_method };
	std::vector<const Variant *> args = { &names[0], &names[1] };
	args.insert(args.end(), p_args, p_args + p_count);
	Variant(ClassDBSingleton::get_singleton()).callp("class_call_static", args.data(), int(args.size()), r_result, r_error);
}

void report_error(const ClassImage &p_image, const bytecode::Function &p_function, int p_line, const String &p_message) {
	vm::report_error(p_image, p_function.name.c_str(), p_line, p_message);
}

template <typename T>
int64_t packed_size(const uint8_t *p_slot) {
	return reinterpret_cast<const T *>(p_slot)->size();
}

int64_t packed_array_size(ValueType p_type, const uint8_t *p_slot) {
	switch (p_type) {
		case ValueType::PackedByteArray:
			return packed_size<PackedByteArray>(p_slot);
		case ValueType::PackedInt32Array:
			return packed_size<PackedInt32Array>(p_slot);
		case ValueType::PackedInt64Array:
			return packed_size<PackedInt64Array>(p_slot);
		case ValueType::PackedFloat32Array:
			return packed_size<PackedFloat32Array>(p_slot);
		case ValueType::PackedFloat64Array:
			return packed_size<PackedFloat64Array>(p_slot);
		case ValueType::PackedStringArray:
			return packed_size<PackedStringArray>(p_slot);
		case ValueType::PackedVector2Array:
			return packed_size<PackedVector2Array>(p_slot);
		case ValueType::PackedVector3Array:
			return packed_size<PackedVector3Array>(p_slot);
		case ValueType::PackedColorArray:
			return packed_size<PackedColorArray>(p_slot);
		case ValueType::PackedVector4Array:
			return packed_size<PackedVector4Array>(p_slot);
		default:
			return 0;
	}
}

// The engine only reports an error when a read-only container is changed, or
// changes a copy (core/variant/array.cpp:104-108, 518); here it is a program
// error (spec §3.8).
bool is_read_only(ValueType p_type, const uint8_t *p_slot) {
	if (p_type == ValueType::Array) {
		return reinterpret_cast<const Array *>(p_slot)->is_read_only();
	}
	return p_type == ValueType::Dictionary && reinterpret_cast<const Dictionary *>(p_slot)->is_read_only();
}

String read_only_error(ValueType p_type) {
	return vformat("The %s is read-only, as Dictionary keys and the containers of constants are; change a duplicate() of it instead.", bytecode::value_type_name(p_type));
}

// A key or an element as GET_INDEX and SET_INDEX take it: a Variant in a
// container without types or as a box, otherwise in its slot's layout.
Variant kept(const TypeImage *p_type, const uint8_t *p_slot) {
	return !p_type || p_type->box ? variant_at(p_slot) : to_variant(*p_type, p_slot);
}

// A key as a diagnostic shows it, a box as its value.
String key_text(const Variant &p_key) {
	return type_of(p_key) == Variant::CALLABLE ? String(p_key) : UtilityFunctions::var_to_str(p_key);
}

// A container read out of another is checked against its type, which the
// engine keeps one level deep (spec §3.8).
bool checked_when_read(const TypeImage &p_type) {
	return p_type.kind == bytecode::TypeKind::Array || p_type.kind == bytecode::TypeKind::Dictionary;
}

// "Expected Array[int], found Array.", as CHECK_OBJECT says it.
String read_error(const TypeImage &p_type, const Variant &p_value) {
	const String mismatch = entry_mismatch(p_type, p_value);
	return mismatch.substr(0, 1).to_upper() + mismatch.substr(1) + ".";
}

// The instructions below make values of their own here, out of run(), whose
// frame every call of this language nests, up to MAX_CALL_DEPTH of them on
// the thread's stack. Each is false after it sets helper_error() to the error of
// its program error.
#ifdef _MSC_VER
#define PETRICHOR_NOINLINE __declspec(noinline)
#else
#define PETRICHOR_NOINLINE __attribute__((noinline))
#endif

// Made on first use and never destroyed: no godot-cpp value may be made before
// the engine loads the library, or destroyed after it unloads it.
PETRICHOR_NOINLINE String &helper_error() {
	thread_local String *error = nullptr;
	if (!error) {
		error = new String();
	}
	return *error;
}

// The arguments of a call through an untyped view that do not fit the types
// the container has (spec §3.8).
PETRICHOR_NOINLINE bool arguments_fit(const std::vector<std::pair<uint32_t, ElementRole>> &p_roles, const std::vector<ValueType> &p_params, const void *const *p_args, ValueType p_base, const uint8_t *p_container) {
	String &r_error = helper_error();
	const Variant container = to_variant(p_base, p_container);
	for (const auto &[index, role] : p_roles) {
		const Variant value = p_params[index] == ValueType::Variant ? *static_cast<const Variant *>(p_args[index]) : to_variant(p_params[index], static_cast<const uint8_t *>(p_args[index]));
		r_error = untyped_misfit(container, role, value);
		if (!r_error.is_empty()) {
			return false;
		}
	}
	return true;
}

// An element, or a key and with `p_value` its value, written or looked up
// through an untyped view (spec §3.8).
PETRICHOR_NOINLINE bool entry_fits(const TypeImage &p_base, const uint8_t *p_container, const uint8_t *p_key, const uint8_t *p_value) {
	String &r_error = helper_error();
	if (p_base.storage == ValueType::Array) {
		r_error = untyped_misfit(*reinterpret_cast<const Array *>(p_container), ElementRole::Element, kept(p_base.item, p_value));
		return r_error.is_empty();
	}
	const Dictionary &dictionary = *reinterpret_cast<const Dictionary *>(p_container);
	r_error = untyped_misfit(dictionary, ElementRole::Key, kept(p_base.key, p_key));
	if (r_error.is_empty() && p_value) {
		r_error = untyped_misfit(dictionary, ElementRole::Value, kept(p_base.item, p_value));
	}
	return r_error.is_empty();
}

// The value of a Variant in a slot of its type (spec §5.4).
PETRICHOR_NOINLINE bool read_variant(const TypeImage &p_type, const uint8_t *p_variant, uint8_t *p_slot) {
	const Variant &value = variant_at(p_variant);
	if (from_variant(p_type, value, p_slot, true)) {
		return true;
	}
	helper_error() = read_error(p_type, value);
	return false;
}

// `T(v)` as GDScript converts (core/variant/variant_construct.cpp:264-289;
// ed1daf0, spec §3.13).
PETRICHOR_NOINLINE bool construct_from(const TypeImage &p_type, const uint8_t *p_variant, uint8_t *p_slot) {
	const Variant &value = variant_at(p_variant);
	const GDExtensionConstVariantPtr arg = value._native_ptr();
	Variant made;
	GDExtensionCallError error;
	gdextension_interface::variant_construct(GDExtensionVariantType(variant_type(p_type.storage)), made._native_ptr(), &arg, 1, &error);
	if (error.error == GDEXTENSION_CALL_OK && from_variant(p_type, made, p_slot, true)) {
		return true;
	}
	helper_error() = vformat("%s() cannot convert %s.", type_description(p_type), describe_value(value));
	return false;
}

// An object of the class: the engine class, with the script when the class is
// one of this language, as PetScript.new() makes it; the script's instance
// runs the initializers and, unless `p_without_init`, _init() (spec §6.1).
// `r_initialized` is false when a program error ended the initializers.
PETRICHOR_NOINLINE bool new_object(const ClassImage &p_image, uint32_t p_class, bool p_without_init, uint8_t *p_slot, bool &r_initialized) {
	r_initialized = true;
	const Variant object = ClassDBSingleton::get_singleton()->instantiate(p_image.classes[p_class]);
	if (object.get_type() != Variant::OBJECT || !object.booleanize()) {
		helper_error() = vformat("Cannot construct %s.", p_image.classes[p_class]);
		return false;
	}
	if (!p_image.class_scripts[p_class].is_empty()) {
		Object *made = object.get_validated_object();
		ScriptedObject instance;
		if (!attach_script(made, load_resource(p_image.class_scripts[p_class]), p_without_init, instance, r_initialized)) {
			if (!Object::cast_to<RefCounted>(made)) {
				memdelete(made);
			}
			helper_error() = vformat("Cannot construct %s: it has errors.", p_image.class_scripts[p_class]);
			return false;
		}
	}
	set_object(p_slot, object);
	return true;
}

// `==` by the engine's operator table (spec §5.2), or with `p_pattern` the
// match of a pattern, of its type first, String and StringName alike (§5.6).
PETRICHOR_NOINLINE bool compare_variants(const uint8_t *p_a, const uint8_t *p_b, bool p_pattern, uint64_t &r_equal) {
	const Variant &a = variant_at(p_a);
	const Variant &b = variant_at(p_b);
	const auto stringy = [](Variant::Type p_type) { return p_type == Variant::STRING || p_type == Variant::STRING_NAME; };
	r_equal = 0;
	if (p_pattern && type_of(a) != type_of(b) && !(stringy(type_of(a)) && stringy(type_of(b)))) {
		return true;
	}
	Variant result;
	bool valid = false;
	Variant::evaluate(Variant::OP_EQUAL, a, b, result, valid);
	if (!valid) {
		helper_error() = vformat("Cannot compare %s with %s.", describe_value(a), describe_value(b));
		return false;
	}
	r_equal = result.booleanize() ? 1 : 0;
	return true;
}

// What an object must be to fit the class table's entry `p_index`.
ValueCheck class_check(const ClassImage &p_image, uint32_t p_index) {
	ValueCheck check;
	check.class_name = p_image.classes[p_index];
	check.script = p_image.class_scripts[p_index];
	check.interface = p_image.class_interfaces[p_index];
	check.nullable = false;
	return check;
}

/* Static variables (spec §4.8) */

// The classes whose static initialization runs on this thread, innermost last.
thread_local std::vector<const ClassImage *> initializing;
// Guards the states of static variables, which another thread waits on while
// one thread initializes them.
std::mutex statics_mutex;
std::condition_variable statics_changed;

String class_label(const ClassImage &p_image) {
	return p_image.global_name.is_empty() ? p_image.path.get_file().replace("::", ".") : String(p_image.global_name);
}

// What the class's initialization is doing: the initializer of a static
// variable, or _static_init().
String initializing_part(const ClassImage &p_image) {
	const std::vector<bytecode::Member> &variables = p_image.module.statics;
	for (size_t i = p_image.statics->next; i < variables.size(); i++) {
		if (!variables[i].has_initial) {
			return class_label(p_image) + "." + to_godot(variables[i].name);
		}
	}
	return class_label(p_image) + "._static_init()";
}

void set_state(Statics &r_statics, Statics::State p_state) {
	{
		std::lock_guard<std::mutex> lock(statics_mutex);
		r_statics.state.store(p_state, std::memory_order_release);
	}
	statics_changed.notify_all();
}

// The class's own static initialization, after its base's; false when it, or
// its base's, ended in a program error.
bool initialize_class(const std::shared_ptr<const ClassImage> &p_image) {
	Statics &statics = *p_image->statics;
	{
		std::unique_lock<std::mutex> lock(statics_mutex);
		// Another thread finishes what it started; on this one, whoever asks
		// learns where it stands from the variable it reads.
		statics_changed.wait(lock, [&] { return statics.state.load(std::memory_order_acquire) != Statics::State::Initializing || statics.thread == std::this_thread::get_id(); });
		if (statics.state.load(std::memory_order_acquire) != Statics::State::Uninitialized) {
			return statics.state.load(std::memory_order_acquire) != Statics::State::Failed;
		}
		statics.state.store(Statics::State::Initializing, std::memory_order_release);
		statics.thread = std::this_thread::get_id();
		statics.next = 0;
	}
	initializing.push_back(p_image.get());
	bool ok = !p_image->base || initialize_class(p_image->base);
	if (ok && p_image->module.static_init_function >= 0) {
		GDExtensionCallError error;
		bool failed = false;
		vm::call(p_image, p_image->module.static_init_function, InstanceContext(), nullptr, 0, error, &failed);
		ok = !failed;
	}
	initializing.pop_back();
	set_state(statics, ok ? Statics::State::Initialized : Statics::State::Failed);
	return ok;
}

// Whether the static variable may be used now, which first initializes its
// class if that has not begun; otherwise why not.
bool static_ready(const ClassImage &p_owner, uint32_t p_variable, String &r_error) {
	Statics &statics = *p_owner.statics;
	const String name = class_label(p_owner) + "." + to_godot(p_owner.module.statics[p_variable].name);
	while (true) {
		switch (statics.state.load(std::memory_order_acquire)) {
			case Statics::State::Initialized:
				return true;
			case Statics::State::Failed:
				r_error = vformat("%s cannot be used: the static initialization of %s failed.", name, class_label(p_owner));
				return false;
			case Statics::State::Uninitialized:
				if (!initialize_class(p_owner.shared_from_this())) {
					continue;
				}
				break;
			case Statics::State::Initializing: {
				if (statics.thread != std::this_thread::get_id()) {
					initialize_class(p_owner.shared_from_this()); // Waits for the other thread.
					continue;
				}
				const auto at = std::find(initializing.begin(), initializing.end(), &p_owner);
				if (at == initializing.end() || at + 1 == initializing.end()) {
					// Its own initialization, also in a function it calls.
					if (p_owner.module.statics[p_variable].has_initial || p_variable < statics.next) {
						return true;
					}
					r_error = vformat("%s is used before its initializer runs: static variables are initialized in declaration order.", name);
					return false;
				}
				String chain;
				for (auto it = at; it != initializing.end(); ++it) {
					chain += initializing_part(**it) + String::utf8(" → ");
				}
				r_error = vformat("Static initialization cycle: %s%s.", chain, name);
				return false;
			}
		}
	}
}

class Execution {
	const ClassImage &image;
	const InstanceContext &context;
	// Whether the engine's debugger is on, which does not change while the
	// engine runs (core/debugger/engine_debugger.h:106); asked once per call
	// from the engine, since asking is a call into the engine.
	const bool debugging;
	std::vector<StackFrame> &stack;
	FrameStack &frames;
	// self as a Variant, made when an instruction first needs it.
	alignas(Variant) uint8_t self_storage[sizeof(Variant)] = {};
	bool has_self = false;

	Variant &self() {
		if (!has_self) {
			new (self_storage) Variant(context.owner);
			has_self = true;
		}
		return variant_at(self_storage);
	}

	PETRICHOR_NOINLINE bool fail(const bytecode::Function &p_fn, int p_line, const String &p_message) {
		report_error(image, p_fn, p_line, p_message);
		return false;
	}

	PETRICHOR_NOINLINE bool fail_cstr(const bytecode::Function &p_fn, int p_line, const char *p_message) {
		report_error(image, p_fn, p_line, String(p_message));
		return false;
	}

	PETRICHOR_NOINLINE bool fail_helper(const bytecode::Function &p_fn, int p_line) {
		report_error(image, p_fn, p_line, helper_error());
		return false;
	}

	PETRICHOR_NOINLINE bool fail_stack_overflow() {
		const StackFrame &caller = stack.back();
		report_error(*caller.image, *caller.function, caller.line, vformat("Stack overflow: more than %d calls deep. Check for infinite recursion.", int64_t(MAX_CALL_DEPTH)));
		return false;
	}

	PETRICHOR_NOINLINE bool fail_float_to_int(const bytecode::Function &p_fn, int p_line, double p_value) {
		return fail(p_fn, p_line, vformat("Cannot convert %s to int: out of range.", p_value));
	}

	PETRICHOR_NOINLINE bool fail_shift(const bytecode::Function &p_fn, int p_line, int64_t p_a, int64_t p_b, bool p_left) {
		if (p_a < 0 || p_b < 0) {
			return fail(p_fn, p_line, vformat("Cannot compute %d %s %d: a shift takes operands that are not negative.", p_a, p_left ? "<<" : ">>", p_b));
		}
		return fail(p_fn, p_line, vformat("Cannot compute %d %s %d: a shift by 64 or more bits is undefined.", p_a, p_left ? "<<" : ">>", p_b));
	}

	PETRICHOR_NOINLINE bool fail_pow_int(const bytecode::Function &p_fn, int p_line, int64_t p_exp) {
		return fail(p_fn, p_line, vformat("Cannot compute 0 ** %d: division by zero.", p_exp));
	}

	PETRICHOR_NOINLINE bool fail_check_enum(const bytecode::Function &p_fn, int p_line, uint32_t p_type, int64_t p_value) {
		return fail(p_fn, p_line, vformat("%d is none of the values of %s.", p_value, type_description(image.types[p_type])));
	}

	PETRICHOR_NOINLINE bool fail_unassigned(const bytecode::Function &p_fn, int p_line, uint32_t p_member) {
		// Spec §6.3; §7.5: the scene or the resource did not assign the export.
		return fail(p_fn, p_line, vformat(image.module.members[p_member].is_onready ? "\"%s\" is read before the node is ready." : "\"%s\" has not been assigned.", image.member_names[p_member]));
	}

	PETRICHOR_NOINLINE bool fail_range(const bytecode::Function &p_fn, int p_line, bytecode::IntRange p_range, int64_t p_value) {
		return fail(p_fn, p_line, vformat("%d is out of range for the engine's %s.", p_value, bytecode::int_range_name(p_range)));
	}

	PETRICHOR_NOINLINE bool fail_assert(const bytecode::Function &p_fn, int p_line, const uint8_t *p_frame, uint32_t p_slot) {
		// As GDScript words it (modules/gdscript/gdscript_vm.cpp:3876-3902; ed1daf0).
		if (p_slot == NO_SLOT || reinterpret_cast<const String *>(p_frame + p_slot)->is_empty()) {
			return fail_cstr(p_fn, p_line, "Assertion failed.");
		}
		return fail(p_fn, p_line, "Assertion failed: " + *reinterpret_cast<const String *>(p_frame + p_slot));
	}

	PETRICHOR_NOINLINE bool fail_unwrap(const bytecode::Function &p_fn, int p_line, const uint8_t *p_frame, uint32_t p_slot) {
		return p_slot == NO_SLOT ? fail_cstr(p_fn, p_line, "unwrap() found None.") : fail(p_fn, p_line, *reinterpret_cast<const String *>(p_frame + p_slot));
	}

	PETRICHOR_NOINLINE bool fail_invalid_op(const bytecode::Function &p_fn, int p_line, uint32_t p_op) {
		return fail(p_fn, p_line, vformat("Invalid instruction %d.", p_op));
	}

	PETRICHOR_NOINLINE void step_debugger(int p_line) {
		debugger::line(image.source, p_line);
	}

	// The object an instruction works on: self, or a live object in a slot (spec §3.3).
	Variant *resolve_object(uint8_t *p_frame, uint32_t p_offset, const std::atomic<bool> &p_freed, ObjectView &r_view, const bytecode::Function &p_fn, int p_line) {
		if (p_offset == NO_SLOT) {
			if (p_freed.load(std::memory_order_relaxed)) {
				fail_cstr(p_fn, p_line, "self was freed, or lost its script, while this method ran.");
				return nullptr;
			}
			return &self();
		}
		if (!is_live_object(p_frame + p_offset)) {
			fail_cstr(p_fn, p_line, is_none_object(p_frame + p_offset) ? "The value is None." : "The object was freed.");
			return nullptr;
		}
		new (&r_view) ObjectView(p_frame + p_offset);
		return &r_view.get();
	}

	// A static variable of the class at `p_class`, or of this one (spec §4.8);
	// null with the reason after a program error.
	uint8_t *static_variable(uint32_t p_class, uint32_t p_variable, uint32_t p_type, String &r_error) {
		if (p_class == NO_SLOT) {
			Statics &statics = *image.statics;
			if (statics.state.load(std::memory_order_acquire) != Statics::State::Initialized && !static_ready(image, p_variable, r_error)) {
				return nullptr;
			}
			return statics.at(image.module.statics[p_variable].offset);
		}
		const String &path = image.class_scripts[p_class];
		std::shared_ptr<const ClassImage> owner = image.base;
		while (owner && owner->path != path) {
			owner = owner->base;
		}
		if (!owner) {
			owner = script_image(path);
		}
		if (!owner) {
			r_error = vformat("%s cannot be loaded.", path);
			return nullptr;
		}
		const std::vector<bytecode::Member> &variables = owner->module.statics;
		if (p_variable >= variables.size() || owner->types[variables[p_variable].type].layout != image.types[p_type].layout) {
			r_error = vformat("A static variable of %s is not the one this script was compiled against. Build the project again.", path);
			return nullptr;
		}
		Statics &statics = *owner->statics;
		if (statics.state.load(std::memory_order_acquire) != Statics::State::Initialized && !static_ready(*owner, p_variable, r_error)) {
			return nullptr;
		}
		return statics.at(variables[p_variable].offset);
	}

	// Calls a function of a class of this language for the call instruction at
	// `p_call`: a program error in it ends the caller too (spec §8.1). An
	// override or an inherited function may take and give other types than the
	// call was compiled against (§4.6), which the arguments and result convert to.
	PETRICHOR_NOINLINE bool call_function(const ClassImage &p_callee, uint32_t p_index, const InstanceContext &p_context, const uint32_t *p_call, uint8_t *frame, const bytecode::Slot &p_result, const bytecode::Function &p_caller, int p_line) {
		const uint32_t dst = p_call[2], argc = p_call[4];
		const bytecode::Function &method = p_callee.module.functions[p_index];
		if (!method.is_static && !p_context.storage) {
			return fail_cstr(p_caller, p_line, "An instance method needs a receiver. Build the project again.");
		}
		if (method.is_abstract) {
			return fail(p_caller, p_line, vformat("%s() is abstract, and this object's class does not implement it. Build the project again.", p_callee.function_names[p_index]));
		}
		const size_t required = method.params.size() - method.defaults.size();
		if (argc < required || argc > method.params.size()) {
			return fail_cstr(p_caller, p_line, "Override argument count changed. Rebuild the project.");
		}
		Frame callee_frame(frames, p_callee.frames[p_index]);
		for (size_t i = 0; i < method.params.size(); i++) {
			uint8_t *param = callee_frame.data + method.params[i].offset;
			const TypeImage &param_type = p_callee.types[method.params[i].type];
			const TypeImage *source = i < argc ? &image.types[p_call[5 + 2 * i]] : nullptr;
			if (source && source->layout == param_type.layout) {
				copy_value(param_type, param, frame + p_call[6 + 2 * i]);
				continue;
			}
			Variant value;
			String error;
			if (source) {
				value = to_variant(*source, frame + p_call[6 + 2 * i]);
			} else if (!constant_value(p_callee, method.defaults[i - required], value, error)) {
				return fail(p_caller, p_line, error);
			}
			if (!from_variant(param_type, value, param, true)) {
				return fail(p_caller, p_line, vformat("Argument of %s(): %s.", p_callee.function_names[p_index], entry_mismatch(param_type, value)));
			}
		}
		const TypeImage &result_type = image.types[p_result.type];
		if (method.is_async) {
			const Ref<CoroutineTask> task = start_task(p_callee.shared_from_this(), p_index, p_context, callee_frame.data, &result_type.result_key);
			if (dst != NO_SLOT) {
				set_object(frame + dst, task);
			}
			return true;
		}
		const bool here = &p_callee == &image && &p_context == &context;
		const auto run_callee = [&](uint8_t *p_returned) {
			return here ? run(p_index, callee_frame.data, p_returned) : Execution(p_callee, p_context, debugging).run(p_index, callee_frame.data, p_returned);
		};
		const TypeImage &returns = p_callee.types[method.result.type];
		if (dst == NO_SLOT || returns.layout == result_type.layout) {
			return run_callee(dst == NO_SLOT ? nullptr : frame + dst);
		}
		ValueBuffer returned(returns);
		return run_callee(returned.data) && from_variant(result_type, outward_variant(returns, returned.data), frame + dst, true);
	}

	PETRICHOR_NOINLINE bool op_load_static(const uint32_t *p_op, uint8_t *p_frame, const bytecode::Function &p_fn, int p_line) {
		String error;
		const uint8_t *variable = static_variable(p_op[3], p_op[4], p_op[1], error);
		if (!variable) {
			return fail(p_fn, p_line, error);
		}
		copy_value(image.types[p_op[1]], p_frame + p_op[2], variable);
		return true;
	}

	PETRICHOR_NOINLINE bool op_store_static(const uint32_t *p_op, uint8_t *p_frame, const bytecode::Function &p_fn, int p_line) {
		String error;
		uint8_t *variable = static_variable(p_op[2], p_op[3], p_op[1], error);
		if (!variable) {
			return fail(p_fn, p_line, error);
		}
		copy_value(image.types[p_op[1]], variable, p_frame + p_op[4]);
		return true;
	}

	PETRICHOR_NOINLINE void op_init_static(const uint32_t *p_op, uint8_t *p_frame) {
		Statics &statics = *image.statics;
		const bytecode::Member &variable = image.module.statics[p_op[2]];
		copy_value(image.types[p_op[1]], statics.at(variable.offset), p_frame + p_op[3]);
		if (!variable.has_initial) {
			statics.next = p_op[2] + 1;
		}
	}

	PETRICHOR_NOINLINE static void op_add_string(const uint32_t *p_op, uint8_t *p_frame) {
		*reinterpret_cast<String *>(p_frame + p_op[1]) = *reinterpret_cast<const String *>(p_frame + p_op[2]) + *reinterpret_cast<const String *>(p_frame + p_op[3]);
	}

	PETRICHOR_NOINLINE bool op_format(const uint32_t *p_op, uint8_t *p_frame, const bytecode::Function &p_fn, int p_line) {
		// The engine's general evaluation, which reports the error that
		// its direct one drops (core/variant/variant_op.h:720-837; ed1daf0).
		Variant result;
		bool valid = false;
		Variant::evaluate(Variant::OP_MODULE, *reinterpret_cast<const String *>(p_frame + p_op[2]), variant_at(p_frame + p_op[3]), result, valid);
		if (!valid) {
			return fail(p_fn, p_line, vformat("String formatting error: %s.", result));
		}
		*reinterpret_cast<String *>(p_frame + p_op[1]) = result;
		return true;
	}

	PETRICHOR_NOINLINE bool op_get_property(const uint32_t *p_op, uint8_t *p_frame, const std::atomic<bool> &p_freed, const bytecode::Function &p_fn, int p_line) {
		const uint32_t index = p_op[1];
		ObjectView view(nullptr);
		Variant *object = resolve_object(p_frame, p_op[3], p_freed, view, p_fn, p_line);
		if (!object) {
			return false;
		}
		bool valid = false;
		const Variant value = object->get_named(image.properties[index], valid);
		if (!valid) {
			return fail(p_fn, p_line, vformat("Cannot read the property %s.", image.properties[index]));
		}
		if (!from_variant(image.types[image.module.properties[index].type], value, p_frame + p_op[2], false)) {
			return fail(p_fn, p_line, vformat("The property %s holds an unexpected %s.", image.properties[index], Variant::get_type_name(value.get_type())));
		}
		return true;
	}

	PETRICHOR_NOINLINE bool op_has_member(const uint32_t *p_op, uint8_t *p_frame, const std::atomic<bool> &p_freed, const bytecode::Function &p_fn, int p_line) {
		ObjectView view(nullptr);
		Variant *object = resolve_object(p_frame, p_op[2], p_freed, view, p_fn, p_line);
		if (!object) {
			return false;
		}
		// As the engine's `in` (core/variant/variant_op.h:1217-1250; ed1daf0).
		bool valid = false;
		object->get_named(StringName(*reinterpret_cast<const String *>(p_frame + p_op[3])), valid);
		*reinterpret_cast<uint64_t *>(p_frame + p_op[1]) = valid ? 1 : 0;
		return true;
	}

	PETRICHOR_NOINLINE bool op_set_property(const uint32_t *p_op, uint8_t *p_frame, const std::atomic<bool> &p_freed, const bytecode::Function &p_fn, int p_line) {
		const uint32_t index = p_op[1];
		ObjectView view(nullptr);
		Variant *object = resolve_object(p_frame, p_op[2], p_freed, view, p_fn, p_line);
		if (!object) {
			return false;
		}
		bool valid = false;
		object->set_named(image.properties[index], to_variant(image.types[image.module.properties[index].type], p_frame + p_op[3]), valid);
		if (!valid) {
			return fail(p_fn, p_line, vformat("Cannot write the property %s.", image.properties[index]));
		}
		return true;
	}

	PETRICHOR_NOINLINE bool op_get_script(const uint32_t *p_op, uint8_t *p_frame, const bytecode::Function &p_fn, int p_line) {
		const Variant script = load_resource(image.class_scripts[p_op[1]]);
		if (script.get_type() != Variant::OBJECT || !script.booleanize()) {
			return fail(p_fn, p_line, vformat("Cannot load the script of %s.", image.class_scripts[p_op[1]]));
		}
		set_object(p_frame + p_op[2], script);
		return true;
	}

	PETRICHOR_NOINLINE bool op_get_owner(const uint32_t *p_op, uint8_t *p_frame, const std::atomic<bool> &p_freed, const bytecode::Function &p_fn, int p_line) {
		// Checked at every read, since code and the engine change the owner (spec §7.3).
		if (p_freed.load(std::memory_order_relaxed)) {
			return fail_cstr(p_fn, p_line, "self was freed, or lost its script, while this method ran.");
		}
		const Node *node = Object::cast_to<Node>(context.owner);
		Node *found = node ? node->get_owner() : nullptr;
		const uint32_t index = p_op[1];
		if (!found || !image.fits_class(found, index)) {
			return fail(p_fn, p_line, vformat("owner: %s (@owner).", entry_mismatch(ValueType::Object, Variant(found), class_check(image, index))));
		}
		set_object(p_frame + p_op[2], Variant(found));
		return true;
	}

	PETRICHOR_NOINLINE bool op_get_self(const uint32_t *p_op, uint8_t *p_frame, const std::atomic<bool> &p_freed, const bytecode::Function &p_fn, int p_line) {
		if (p_freed.load(std::memory_order_relaxed)) {
			return fail_cstr(p_fn, p_line, "self was freed, or lost its script, while this method ran.");
		}
		set_object(p_frame + p_op[1], self());
		return true;
	}

	PETRICHOR_NOINLINE bool op_get_singleton(const uint32_t *p_op, uint8_t *p_frame, const bytecode::Function &p_fn, int p_line) {
		Object *singleton = Engine::get_singleton()->get_singleton(image.classes[p_op[1]]);
		if (!singleton) {
			return fail(p_fn, p_line, vformat("The engine has no singleton %s.", image.classes[p_op[1]]));
		}
		set_object(p_frame + p_op[2], Variant(singleton));
		return true;
	}

	PETRICHOR_NOINLINE bool op_get_autoload(const uint32_t *p_op, uint8_t *p_frame, const bytecode::Function &p_fn, int p_line) {
		// An autoload is a child of the root by its name (spec §7.3).
		const StringName name = image.values[p_op[1]];
		SceneTree *tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop());
		Window *root = tree ? tree->get_root() : nullptr;
		Node *node = root ? root->get_node_or_null(NodePath(String(name))) : nullptr;
		if (!node) {
			return fail(p_fn, p_line, vformat("The autoload %s is not in the scene tree.", name));
		}
		set_object(p_frame + p_op[2], Variant(node));
		return true;
	}

	PETRICHOR_NOINLINE bool op_method_value(const uint32_t *p_op, uint8_t *p_frame, const std::atomic<bool> &p_freed, const bytecode::Function &p_fn, int p_line) {
		if (p_freed.load(std::memory_order_relaxed)) {
			return fail_cstr(p_fn, p_line, "self was freed, or lost its script, while this method ran.");
		}
		*reinterpret_cast<Callable *>(p_frame + p_op[2]) = Callable(memnew(MethodCallable(context.owner, image.function_names[p_op[1]])));
		return true;
	}

	PETRICHOR_NOINLINE bool op_make_lambda(const uint32_t *p_op, uint8_t *p_frame, const std::atomic<bool> &p_freed, const bytecode::Function &p_fn, int p_line) {
		const uint32_t lambda = p_op[1];
		const bool on_self = !image.module.functions[lambda].is_static;
		if (on_self && p_freed.load(std::memory_order_relaxed)) {
			return fail_cstr(p_fn, p_line, "self was freed, or lost its script, while this method ran.");
		}
		*reinterpret_cast<Callable *>(p_frame + p_op[2]) = Callable(memnew(LambdaCallable(image.shared_from_this(), lambda, on_self ? context : InstanceContext(), p_frame, p_op + 5)));
		return true;
	}

	PETRICHOR_NOINLINE bool op_get_signal(const uint32_t *p_op, uint8_t *p_frame, const std::atomic<bool> &p_freed, const bytecode::Function &p_fn, int p_line) {
		ObjectView view(nullptr);
		Variant *object = resolve_object(p_frame, p_op[3], p_freed, view, p_fn, p_line);
		if (!object) {
			return false;
		}
		*reinterpret_cast<Signal *>(p_frame + p_op[2]) = Signal(static_cast<Object *>(*object), StringName(image.values[p_op[1]]));
		return true;
	}

	PETRICHOR_NOINLINE bool op_check_object(const uint32_t *p_op, uint8_t *p_frame, const bytecode::Function &p_fn, int p_line) {
		ObjectView view(p_frame + p_op[2]);
		const Variant &value = view.get();
		const Object *object = value.get_validated_object();
		if (is_none(value) || (object && !image.fits_class(object, p_op[1]))) {
			const String mismatch = entry_mismatch(ValueType::Object, value, class_check(image, p_op[1]));
			return fail(p_fn, p_line, mismatch.substr(0, 1).to_upper() + mismatch.substr(1) + ".");
		}
		return true;
	}

	PETRICHOR_NOINLINE void op_cast_object(const uint32_t *p_op, uint8_t *p_frame) {
		// A freed object is None for every type (spec §5.3).
		const uint8_t *src = p_frame + p_op[3];
		ObjectView view(src);
		const Object *object = is_live_object(src) ? view.get().get_validated_object() : nullptr;
		if (object && image.fits_class(object, p_op[1])) {
			copy_value(ValueType::Object, p_frame + p_op[2], src);
		} else {
			destroy_value(ValueType::Object, p_frame + p_op[2]);
		}
	}

	PETRICHOR_NOINLINE void op_is_instance(const uint32_t *p_op, uint8_t *p_frame) {
		// A freed object is no class's instance (spec §3.3).
		const uint8_t *src = p_frame + p_op[3];
		ObjectView view(src);
		const Object *object = is_live_object(src) ? view.get().get_validated_object() : nullptr;
		*reinterpret_cast<uint64_t *>(p_frame + p_op[2]) = object && image.fits_class(object, p_op[1]) ? 1 : 0;
	}

	PETRICHOR_NOINLINE void op_new_container(const uint32_t *p_op, uint8_t *p_frame) {
		const TypeImage &type = image.types[p_op[1]];
		if (type.storage == ValueType::Dictionary) {
			*reinterpret_cast<Dictionary *>(p_frame + p_op[2]) = typed_dictionary(type.check);
		} else {
			*reinterpret_cast<Array *>(p_frame + p_op[2]) = typed_array(type.check.element, type.check.class_name, type.check.script);
		}
	}

	PETRICHOR_NOINLINE bool op_get_index(const uint32_t *p_op, uint8_t *p_frame, const bytecode::Function &p_fn, int p_line) {
		const TypeImage &base = image.types[p_op[1]];
		const TypeImage &element = image.types[p_op[2]];
		if (base.storage == ValueType::Dictionary) {
			const Dictionary &dictionary = *reinterpret_cast<const Dictionary *>(p_frame + p_op[4]);
			// Through an untyped view, a key looked up must fit (spec §3.8).
			if (base.kind == bytecode::TypeKind::Value && !entry_fits(base, p_frame + p_op[4], p_frame + p_op[5], nullptr)) {
				return fail_helper(p_fn, p_line);
			}
			const Variant key = kept(base.key, p_frame + p_op[5]);
			if (!dictionary.has(key)) {
				return fail(p_fn, p_line, vformat("The Dictionary has no key %s.", key_text(key)));
			}
			const Variant &value = dictionary[key];
			if (!from_variant(element, value, p_frame + p_op[3], checked_when_read(element))) {
				return fail(p_fn, p_line, read_error(element, value));
			}
			return true;
		}
		// A negative index counts from the end, as in GDScript.
		Variant container = to_variant(base.storage, p_frame + p_op[4]);
		const int64_t index = *reinterpret_cast<const int64_t *>(p_frame + p_op[5]);
		bool valid = false;
		bool out_of_bounds = false;
		const Variant value = container.get_indexed(index, valid, out_of_bounds);
		if (!valid || out_of_bounds) {
			return fail(p_fn, p_line, vformat("Index %d is out of bounds (size %d).", index, int64_t(container.call("size"))));
		}
		if (!from_variant(element, value, p_frame + p_op[3], checked_when_read(element))) {
			return fail(p_fn, p_line, read_error(element, value));
		}
		return true;
	}

	PETRICHOR_NOINLINE bool op_set_index(const uint32_t *p_op, uint8_t *p_frame, const bytecode::Function &p_fn, int p_line) {
		const TypeImage &base = image.types[p_op[1]];
		uint8_t *container = p_frame + p_op[2];
		const uint8_t *index = p_frame + p_op[3];
		const uint8_t *src = p_frame + p_op[4];
		if (is_read_only(base.storage, container)) {
			return fail(p_fn, p_line, read_only_error(base.storage));
		}
		// Through an untyped view, values must fit the types the container has (spec §3.8).
		if (base.kind == bytecode::TypeKind::Value && (base.storage == ValueType::Dictionary || base.storage == ValueType::Array) && !entry_fits(base, container, index, src)) {
			return fail_helper(p_fn, p_line);
		}
		if (base.storage == ValueType::Dictionary) {
			Dictionary &dictionary = *reinterpret_cast<Dictionary *>(container);
			Variant key = kept(base.key, index);
			if (keys_hold_containers(dictionary) && !dictionary.has(key)) {
				key = frozen_copy(key);
			}
			dictionary.set(key, kept(base.item, src));
			return true;
		}
		// A negative index counts from the end, as in GDScript.
		const int64_t raw = *reinterpret_cast<const int64_t *>(index);
		const int64_t size = base.storage == ValueType::Array ? reinterpret_cast<const Array *>(container)->size() : packed_array_size(base.storage, container);
		const int64_t at = raw < 0 ? raw + size : raw;
		if (at < 0 || at >= size) {
			return fail(p_fn, p_line, vformat("Index %d is out of bounds (size %d).", raw, size));
		}
		if (base.storage == ValueType::Array) {
			reinterpret_cast<Array *>(container)->set(at, kept(base.item, src));
		} else {
			// The element in its slot's layout, as the engine's setter takes it.
			gdextension_interface::variant_get_ptr_indexed_setter(GDExtensionVariantType(base.storage))(container, at, src);
		}
		return true;
	}

	PETRICHOR_NOINLINE void op_to_variant(const uint32_t *p_op, uint8_t *p_frame) {
		variant_at(p_frame + p_op[2]) = to_variant(image.types[p_op[1]], p_frame + p_op[3]);
	}

	PETRICHOR_NOINLINE void op_box(const uint32_t *p_op, uint8_t *p_frame) {
		variant_at(p_frame + p_op[2]) = make_box(image.types[p_op[1]], p_frame + p_op[3], p_op[4] != 0);
	}

	PETRICHOR_NOINLINE void op_encode(const uint32_t *p_op, uint8_t *p_frame) {
		const TypeImage &type = image.types[p_op[1]];
		const Variant encoded = encode(type, p_frame + p_op[3]);
		if (type.storage == ValueType::Array) {
			*reinterpret_cast<Array *>(p_frame + p_op[2]) = encoded;
		} else {
			*reinterpret_cast<Dictionary *>(p_frame + p_op[2]) = encoded;
		}
	}

	PETRICHOR_NOINLINE bool op_call_method(const uint32_t *p_op, uint8_t *p_frame, const std::atomic<bool> &p_freed, const bytecode::Function &p_fn, int p_line) {
		const uint32_t target = p_op[1];
		const StringName &name = image.method_names[target];
		const int32_t static_class = image.module.method_calls[target].static_class;
		ScriptedObject callee;
		if (Op(p_op[0]) == Op::CALL_SUPER) {
			callee = { image.base, context };
		} else if (static_class >= 0) {
			// The class's current image, without an instance (spec §4.8).
			callee.image = script_image(image.class_scripts[static_class]);
		} else {
			ObjectView view(nullptr);
			Variant *receiver = resolve_object(p_frame, p_op[3], p_freed, view, p_fn, p_line);
			if (!receiver) {
				return false;
			}
			if (!scripted_object(static_cast<Object *>(*receiver), callee)) {
				return fail_cstr(p_fn, p_line, "The receiver is not a Petrichor instance.");
			}
			if (callee.context.storage == context.storage && context.self_image) {
				callee.image = context.self_image;
			}
			callee.context.self_image = callee.image;
		}
		// Within a run every script is compiled against the others (spec §12.4);
		// only bytecode that was not built together can differ, and then the
		// arguments would not fit. Whether a static method or _init() is visible
		// is the checker's: subclasses call those that are not pub too (spec §4.2).
		int index = callee.image ? callee.image->find_function(name) : -1;
		const bytecode::Function *found = index >= 0 ? &callee.image->module.functions[index] : nullptr;
		if (!found || !callee.image->has_signature(name, image.call_signatures[target]) || (static_class >= 0 && !found->is_static) || (Op(p_op[0]) == Op::CALL_METHOD && static_class < 0 && !found->is_pub && !found->is_override && index != callee.image->user_init)) {
			return fail(p_fn, p_line, vformat("%s() is not the method this script was compiled against. Build the project again.", name));
		}
		ClassImage::resolve_function(callee.image, index);
		return call_function(*callee.image, uint32_t(index), callee.context, p_op, p_frame, image.module.method_calls[target].result, p_fn, p_line);
	}

	PETRICHOR_NOINLINE bool op_call_value(const uint32_t *p_op, uint8_t *p_frame, const bytecode::Function &p_fn, int p_line) {
		// Through the engine, which passes the call on, also through bind(); a
		// program error in the callee aborts this call too (spec §8.1).
		const bytecode::MethodCall &call = image.module.method_calls[p_op[1]];
		const uint32_t dst = p_op[2];
		const uint32_t argc = p_op[4];
		Variant callable = *reinterpret_cast<const Callable *>(p_frame + p_op[3]);
		Arguments args(image.types, p_frame, p_op + 5, argc, nullptr);
		std::vector<Variant> spread;
		std::vector<const Variant *> spread_pointers;
		const Variant **pointers = args.as_variants();
		int count = int(argc);
		if (call.method == "callv") {
			const Array &values = *reinterpret_cast<const Array *>(p_frame + p_op[6]);
			for (int64_t i = 0; i < values.size(); i++) {
				spread.push_back(values[i]);
			}
			for (const Variant &value : spread) {
				spread_pointers.push_back(&value);
			}
			pointers = spread_pointers.data();
			count = int(spread.size());
		}
		ValueCall &value = value_call();
		const ValueCall outer = value;
		value = { true, false, false };
		Variant result;
		GDExtensionCallError error;
		callable.callp(image.call_method, pointers, count, result, error);
		const ValueCall made = value;
		value = outer;
		if (made.entered && made.failed) {
			return false; // The callee reported it.
		}
		if (error.error == GDEXTENSION_CALL_ERROR_INSTANCE_IS_NULL) {
			return fail_cstr(p_fn, p_line, "The function value's object was freed.");
		}
		if (error.error == GDEXTENSION_CALL_ERROR_TOO_MANY_ARGUMENTS || error.error == GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS) {
			return fail(p_fn, p_line, vformat("callv() passes %d argument(s), but the function takes %d.", count, error.expected));
		}
		if (error.error != GDEXTENSION_CALL_OK) {
			return fail_cstr(p_fn, p_line, "The function value cannot be called.");
		}
		if (dst != NO_SLOT && !from_variant(image.types[call.result.type], result, p_frame + dst, true)) {
			return fail(p_fn, p_line, vformat("A function value returned an unexpected %s.", Variant::get_type_name(result.get_type())));
		}
		return true;
	}

	PETRICHOR_NOINLINE bool op_call_engine(const uint32_t *p_op, uint8_t *p_frame, const std::atomic<bool> &p_freed, CoroutineFrame *p_async, size_t &r_pc, bool &r_done, const bytecode::Function &p_fn, int p_line) {
		const uint32_t target = p_op[1];
		const uint32_t dst = p_op[2];
		const uint32_t argc = p_op[4];
		// By name for now; through MethodBind pointers once the compiler knows
		// method hashes (architecture §2.2).
		Arguments args(image.types, p_frame, p_op + 5, argc, nullptr);
		Variant result;
		GDExtensionCallError error;
		if (!image.engine_static_classes[target].is_empty()) {
			call_static(image.engine_static_classes[target], image.engine_methods[target], args.as_variants(), argc, result, error);
			if (error.error != GDEXTENSION_CALL_OK) {
				return fail(p_fn, p_line, vformat("Calling %s.%s() failed.", image.engine_static_classes[target], image.engine_methods[target]));
			}
			if (dst != NO_SLOT && !from_variant(image.types[image.module.engine_calls[target].result_type], result, p_frame + dst, false)) {
				return fail(p_fn, p_line, vformat("%s() returned an unexpected %s.", image.engine_methods[target], Variant::get_type_name(result.get_type())));
			}
			r_pc += 5 + 2 * argc;
			return true;
		}
		ObjectView view(nullptr);
		Variant *object = resolve_object(p_frame, p_op[3], p_freed, view, p_fn, p_line);
		if (!object) {
			return false;
		}
		object->callp(image.engine_methods[target], args.as_variants(), int(argc), result, error);
		if (error.error != GDEXTENSION_CALL_OK) {
			return fail(p_fn, p_line, vformat("Calling %s() failed.", image.engine_methods[target]));
		}
		Object *state = result.get_type() == Variant::OBJECT ? result.get_validated_object() : nullptr;
		if (state && state->is_class("GDScriptFunctionState")) {
			if (Op(p_op[0]) == Op::CALL_AWAIT_ENGINE) {
				if (p_async->task->cancel_requested) {
					r_done = true;
					return true;
				}
				p_async->held_state = result;
				p_async->pc = r_pc + 5 + 2 * argc;
				if (!p_async->suspend(Signal(state, "completed"), dst, object_id_of(*object), true)) {
					return fail_cstr(p_fn, p_line, "Cannot await GDScript completion.");
				}
				r_done = true;
				return true;
			}
			if (dst != NO_SLOT) {
				return fail_cstr(p_fn, p_line, "The method is a GDScript coroutine and needs await.");
			}
		} else if (dst != NO_SLOT && !from_variant(image.types[image.module.engine_calls[target].result_type], result, p_frame + dst, false)) {
			return fail(p_fn, p_line, vformat("%s() returned an unexpected %s.", image.engine_methods[target], Variant::get_type_name(result.get_type())));
		}
		r_pc += 5 + 2 * argc;
		return true;
	}

	PETRICHOR_NOINLINE bool op_call_await_builtin(const uint32_t *p_op, uint8_t *p_frame, CoroutineFrame *p_async, size_t &r_pc, bool &r_done, const bytecode::Function &p_fn, int p_line) {
		const uint32_t dst = p_op[2];
		const uint32_t argc = p_op[4];
		const Callable &callable = *reinterpret_cast<Callable *>(p_frame + p_op[3]);
		Arguments args(image.types, p_frame, p_op + 5, argc, nullptr);
		Variant base = callable;
		Variant result;
		GDExtensionCallError error;
		base.callp(to_godot(image.module.builtin_methods[p_op[1]].method), args.as_variants(), int(argc), result, error);
		if (error.error != GDEXTENSION_CALL_OK) {
			return fail_cstr(p_fn, p_line, "Calling the awaited Callable failed.");
		}
		Object *state = result.get_type() == Variant::OBJECT ? result.get_validated_object() : nullptr;
		p_async->destination = dst;
		if (state && state->is_class("GDScriptFunctionState")) {
			if (p_async->task->cancel_requested) {
				r_done = true;
				return true;
			}
			p_async->held_state = result;
			p_async->pc = r_pc + 5 + 2 * argc;
			if (!p_async->suspend(Signal(state, "completed"), dst, uint64_t(callable.get_object_id()), true)) {
				return fail_cstr(p_fn, p_line, "Cannot await GDScript completion.");
			}
			r_done = true;
			return true;
		}
		if (result.get_type() == Variant::SIGNAL) {
			if (p_async->task->cancel_requested) {
				r_done = true;
				return true;
			}
			const Signal signal = result;
			Ref<CoroutineTask> task = Object::cast_to<CoroutineTask>(signal.get_object());
			p_async->pc = r_pc + 5 + 2 * argc;
			if (task.is_valid() && signal.get_name() == StringName("completed")) {
				if (!p_async->await_task(task)) {
					return false;
				}
			} else if (!p_async->suspend(signal, dst, uint64_t(signal.get_object_id()))) {
				return fail_cstr(p_fn, p_line, "Cannot await this signal.");
			}
			if (p_async->task->state == CoroutineTask::State::Suspended) {
				r_done = true;
				return true;
			}
		} else if (!p_async->receive(result)) {
			return false;
		}
		r_pc += 5 + 2 * argc;
		return true;
	}

	PETRICHOR_NOINLINE bool op_call_builtin(const uint32_t *p_op, uint8_t *p_frame, const bytecode::Function &p_fn, int p_line) {
		const bytecode::Module &module = image.module;
		const bool utility = Op(p_op[0]) == Op::CALL_UTILITY;
		const uint32_t target = p_op[1];
		const uint32_t dst = p_op[2];
		const uint32_t argc = p_op[4];
		const std::vector<ValueType> &params = utility ? module.utility_calls[target].params : module.builtin_methods[target].params;
		const ValueType returns = utility ? module.utility_calls[target].return_type : module.builtin_methods[target].return_type;
		const TypeImage &result_type = image.types[utility ? module.utility_calls[target].result_type : module.builtin_methods[target].result_type];
		Arguments args(image.types, p_frame, p_op + 5, argc, &params);
		// A static method has no base, which the engine does not read
		// (core/variant/variant_call.cpp:470-472).
		uint8_t *base = p_op[3] == NO_SLOT ? nullptr : p_frame + p_op[3];
		ClassImage::KeyRole keys = ClassImage::KeyRole::None;
		ClassImage::Replaced replaced = ClassImage::Replaced::None;
		std::optional<Variant> inserted; // A Variant is made by a call into the engine.
		if (!utility && base) {
			const bytecode::BuiltinMethodCall &method = module.builtin_methods[target];
			if (!method.is_const && is_read_only(method.base, base)) {
				return fail(p_fn, p_line, read_only_error(method.base));
			}
			// Through an untyped view, values must fit the types the container has (spec §3.8).
			if (!image.element_args[target].empty() && !arguments_fit(image.element_args[target], params, args.pointers, method.base, base)) {
				return fail_helper(p_fn, p_line);
			}
			if (image.needs_element[target] && reinterpret_cast<const Array *>(base)->is_empty()) {
				// The engine returns null, after an error of its own for front() and back().
				return fail(p_fn, p_line, vformat("%s() of an empty array.", method.method.c_str()));
			}
			if (image.replaced[target] != ClassImage::Replaced::None && keeps_boxes(to_variant(method.base, base))) {
				replaced = image.replaced[target];
			}
			if (image.key_roles[target] != ClassImage::KeyRole::None && keys_hold_containers(*reinterpret_cast<const Dictionary *>(base))) {
				keys = image.key_roles[target];
			}
			// A new key goes in as a key (spec §3.8).
			if (keys == ClassImage::KeyRole::Inserts && !reinterpret_cast<const Dictionary *>(base)->has(*reinterpret_cast<const Variant *>(args.pointers[0]))) {
				inserted = frozen_copy(*reinterpret_cast<const Variant *>(args.pointers[0]));
				args.pointers[0] = &*inserted;
			}
		}
		// A native result goes straight into its slot; a Variant one is checked
		// on the way (spec §10.3). The scratch slot takes a Variant result, or
		// one that nothing reads.
		const bool via_variant = returns == ValueType::Variant || returns == ValueType::Void;
		const bool unread = !via_variant && dst == NO_SLOT;
		alignas(16) uint8_t scratch[MAX_VALUE_SIZE];
		construct_value(via_variant ? ValueType::Variant : (unread ? returns : ValueType::Void), scratch);
		void *result = via_variant || unread ? scratch : p_frame + dst;
		if (utility) {
			image.utilities[target](result, args.pointers, int(argc));
		} else if (replaced == ClassImage::Replaced::Hash) {
			*static_cast<int64_t *>(result) = content_hash(to_variant(module.builtin_methods[target].base, base));
		} else if (replaced == ClassImage::Replaced::Sort && module.builtin_methods[target].base == ValueType::Array) {
			reinterpret_cast<Array *>(base)->sort_custom(less_callable());
		} else if (replaced == ClassImage::Replaced::Sort) {
			// The keys in order, each with its value (spec §3.8).
			Dictionary &dictionary = *reinterpret_cast<Dictionary *>(base);
			Array keys_list = dictionary.keys();
			keys_list.sort_custom(less_callable());
			Array values;
			for (int64_t i = 0; i < keys_list.size(); i++) {
				values.push_back(dictionary[keys_list[i]]);
			}
			dictionary.clear();
			for (int64_t i = 0; i < keys_list.size(); i++) {
				dictionary[keys_list[i]] = values[i];
			}
		} else if (replaced == ClassImage::Replaced::Bsearch) {
			const bool before = *static_cast<const uint8_t *>(args.pointers[1]);
			*static_cast<int64_t *>(result) = reinterpret_cast<const Array *>(base)->bsearch_custom(*static_cast<const Variant *>(args.pointers[0]), less_callable(), before);
		} else if (replaced == ClassImage::Replaced::Min || replaced == ClassImage::Replaced::Max) {
			// The first of the least or the greatest, as the engine picks it
			// (core/variant/array.cpp:828-868); null for an empty array.
			const Array &array = *reinterpret_cast<const Array *>(base);
			Variant best = array.is_empty() ? Variant() : array[0];
			for (int64_t i = 1; i < array.size(); i++) {
				if (replaced == ClassImage::Replaced::Min ? variant_less(array[i], best) : variant_less(best, array[i])) {
					best = array[i];
				}
			}
			variant_at(scratch) = best;
		} else if (replaced == ClassImage::Replaced::HasAll) {
			const Dictionary &dictionary = *reinterpret_cast<const Dictionary *>(base);
			const Array &wanted = *static_cast<const Array *>(args.pointers[0]);
			bool all = true;
			for (int64_t i = 0; all && i < wanted.size(); i++) {
				// A box made for a key is hashed.
				const Variant key = wanted[i];
				all = dictionary.has(type_of(key) == Variant::CALLABLE ? frozen_copy(key) : key);
			}
			*static_cast<uint64_t *>(result) = all ? 1 : 0;
		} else {
			image.builtin_methods[target](base, args.pointers, result, int(argc));
		}
		// A deep copy copies the boxes too (spec §3.8).
		const bool deep = (replaced == ClassImage::Replaced::Duplicate && *static_cast<const uint8_t *>(args.pointers[0])) || replaced == ClassImage::Replaced::DuplicateDeep ||
				(replaced == ClassImage::Replaced::Slice && *static_cast<const uint8_t *>(args.pointers[3]));
		if (deep && !unread) {
			const int64_t mode = replaced == ClassImage::Replaced::DuplicateDeep ? *static_cast<const int64_t *>(args.pointers[0]) : -1;
			if (returns == ValueType::Array) {
				Array &copy = *static_cast<Array *>(result);
				for (int64_t i = 0; i < copy.size(); i++) {
					copy[i] = deep_copy(copy[i], mode);
				}
			} else {
				Dictionary &copy = *static_cast<Dictionary *>(result);
				const Array copied = copy.keys();
				for (int64_t i = 0; i < copied.size(); i++) {
					copy[copied[i]] = deep_copy(copy[copied[i]], mode);
				}
			}
		}
		if (keys == ClassImage::KeyRole::HandsOut) {
			// Keys this language obtains cannot be changed (spec §3.8).
			if (returns == ValueType::Array) {
				const Array handed = *reinterpret_cast<const Array *>(result);
				for (int64_t i = 0; i < handed.size(); i++) {
					freeze(handed[i]);
				}
			} else {
				freeze(variant_at(scratch));
			}
		}
		const bool fits = returns != ValueType::Variant || dst == NO_SLOT || from_variant(result_type, variant_at(scratch), p_frame + dst, checked_when_read(result_type));
		const Variant::Type got = via_variant ? type_of(variant_at(scratch)) : Variant::NIL;
		destroy_value(via_variant ? ValueType::Variant : (unread ? returns : ValueType::Void), scratch);
		if (!fits) {
			return fail(p_fn, p_line, vformat("A call returned an unexpected %s.", Variant::get_type_name(got)));
		}
		return true;
	}

	PETRICHOR_NOINLINE void op_construct(const uint32_t *p_op, uint8_t *p_frame) {
		const uint32_t target = p_op[1];
		const uint32_t dst = p_op[2];
		const uint32_t argc = p_op[4];
		const bytecode::ConstructorCall &constructor = image.module.constructors[target];
		if (argc <= 8 && argc <= constructor.params.size()) {
			const void *ptrs[8];
			bool native = true;
			for (uint32_t i = 0; i < argc; i++) {
				if (constructor.params[i] == ValueType::Variant) {
					native = false;
					break;
				}
				ptrs[i] = p_frame + p_op[6 + 2 * i];
			}
			if (native) {
				destroy_value(constructor.type, p_frame + dst);
				image.constructors[target](p_frame + dst, ptrs);
				return;
			}
		}
		Arguments args(image.types, p_frame, p_op + 5, argc, &constructor.params);
		// The engine constructs into memory that holds no value.
		destroy_value(constructor.type, p_frame + dst);
		image.constructors[target](p_frame + dst, args.pointers);
	}

	PETRICHOR_NOINLINE bool op_task_cancel(const uint32_t *p_op, uint8_t *p_frame, const bytecode::Function &p_fn, int p_line) {
		ObjectView view(p_frame + p_op[1]);
		CoroutineTask *task = Object::cast_to<CoroutineTask>(static_cast<Object *>(view.get()));
		if (!task) {
			return fail_cstr(p_fn, p_line, "Expected a Task handle.");
		}
		task->cancel();
		return true;
	}

	PETRICHOR_NOINLINE bool op_await_task(const uint32_t *p_op, uint8_t *p_frame, CoroutineFrame *p_async, size_t &r_pc, bool &r_done, const bytecode::Function &p_fn, int p_line) {
		ObjectView view(p_frame + p_op[2]);
		Ref<CoroutineTask> task = view.get();
		if (task.is_null()) {
			return fail_cstr(p_fn, p_line, "Expected a Task handle.");
		}
		if (p_op[3]) {
			p_async->child = task;
		}
		if (p_async->task->cancel_requested) {
			r_done = true;
			return true;
		}
		p_async->destination = p_op[1];
		p_async->pc = r_pc + 4;
		if (!p_async->await_task(task)) {
			return false;
		}
		if (p_async->task->state == CoroutineTask::State::Suspended) {
			r_done = true;
			return true;
		}
		r_pc += 4;
		return true;
	}

	PETRICHOR_NOINLINE bool op_await_signal(const uint32_t *p_op, uint8_t *p_frame, CoroutineFrame *p_async, size_t p_pc, const bytecode::Function &p_fn, int p_line) {
		if (p_async->task->cancel_requested) {
			return true;
		}
		const Signal signal = *reinterpret_cast<Signal *>(p_frame + p_op[2]);
		p_async->pc = p_pc + 3;
		if (!p_async->suspend(signal, p_op[1], uint64_t(signal.get_object_id()))) {
			return fail_cstr(p_fn, p_line, "Cannot await this signal.");
		}
		return true;
	}

public:
	Execution(const ClassImage &p_image, const InstanceContext &p_context, bool p_debugging) :
			image(p_image), context(p_context), debugging(p_debugging), stack(call_stack), frames(frame_stack) {
		if (stack.capacity() < MAX_CALL_DEPTH + 1) {
			stack.reserve(MAX_CALL_DEPTH + 1);
		}
	}
	~Execution() {
		if (has_self) {
			variant_at(self_storage).~Variant();
		}
	}
	Execution(const Execution &) = delete;
	Execution &operator=(const Execution &) = delete;

	// Runs a function on an initialized frame. Returns false after a program
	// error, which unwinds every frame of this language up to the engine.
	bool run(uint32_t p_function, uint8_t *frame, uint8_t *p_result, CoroutineFrame *p_async = nullptr) {
		const bytecode::Module &module = image.module;
		const bytecode::Function &fn = module.functions[p_function];
		const uint32_t *code = fn.code.data();
		const bytecode::Constant *constants = module.constants.data();
		const TypeImage *types = image.types.data();
		uint8_t *members = context.storage ? context.storage->members : nullptr;
		const std::atomic<bool> &freed = context.storage ? context.storage->freed : NO_INSTANCE_FREED;
		if (stack.size() == MAX_CALL_DEPTH) {
			return fail_stack_overflow();
		}
		stack.push_back({ &image, context.self_image.get(), &fn, frame, members, p_async ? p_async->line : int(fn.line) });
		StackFrame &record = stack.back();
		if (debugging) {
			debugger::enter_function();
		}
		struct Pop {
			std::vector<StackFrame> &stack;
			bool debugging;
			~Pop() {
				if (debugging) {
					debugger::exit_function();
				}
				stack.pop_back();
			}
		} pop{ stack, debugging };

#define I64(m_offset) (*reinterpret_cast<int64_t *>(frame + (m_offset)))
#define U64(m_offset) (*reinterpret_cast<uint64_t *>(frame + (m_offset)))
#define F64(m_offset) (*reinterpret_cast<double *>(frame + (m_offset)))
#define STR(m_offset) (*reinterpret_cast<String *>(frame + (m_offset)))
// Wrapping arithmetic on int, as in GDScript; through unsigned to stay defined.
#define WRAP(m_op) int64_t(uint64_t(I64(code[pc + 2])) m_op uint64_t(I64(code[pc + 3])))
// self and the members, unless the engine deleted the instance while this
// function ran (spec §3.3).
#define SELF_ALIVE() \
	if (freed.load(std::memory_order_relaxed)) { \
		return fail_cstr(fn, record.line, "self was freed, or lost its script, while this method ran."); \
	}

		size_t pc = p_async ? p_async->pc : 0;
		while (true) {
			switch (Op(code[pc])) {
				case Op::LINE:
					record.line = int(code[pc + 1]);
					if (p_async) {
						p_async->line = record.line;
					}
					if (debugging) {
						step_debugger(record.line);
					}
					pc += 2;
					break;
				case Op::CONST_INT:
					I64(code[pc + 1]) = constants[code[pc + 2]].i;
					pc += 3;
					break;
				case Op::CONST_FLOAT:
					F64(code[pc + 1]) = constants[code[pc + 2]].f;
					pc += 3;
					break;
				case Op::CONST_BOOL:
					U64(code[pc + 1]) = code[pc + 2];
					pc += 3;
					break;
				case Op::CONST_STRING:
					STR(code[pc + 1]) = image.strings[code[pc + 2]];
					pc += 3;
					break;
				case Op::CONST_VALUE:
					from_variant(types[code[pc + 1]], image.values[code[pc + 3]], frame + code[pc + 2], false);
					pc += 4;
					break;
				case Op::MOVE_8:
					U64(code[pc + 1]) = U64(code[pc + 2]);
					pc += 3;
					break;
				case Op::MOVE_STRING:
					STR(code[pc + 1]) = STR(code[pc + 2]);
					pc += 3;
					break;
				case Op::MOVE_VALUE:
					copy_value(types[code[pc + 1]], frame + code[pc + 2], frame + code[pc + 3]);
					pc += 4;
					break;
				case Op::LOAD_MEMBER_8:
					SELF_ALIVE();
					std::memcpy(frame + code[pc + 1], members + code[pc + 2], 8);
					pc += 3;
					break;
				case Op::LOAD_MEMBER_STRING:
					SELF_ALIVE();
					STR(code[pc + 1]) = *reinterpret_cast<String *>(members + code[pc + 2]);
					pc += 3;
					break;
				case Op::LOAD_MEMBER_VALUE:
					SELF_ALIVE();
					copy_value(types[code[pc + 1]], frame + code[pc + 2], members + code[pc + 3]);
					pc += 4;
					break;
				case Op::STORE_MEMBER_8:
					SELF_ALIVE();
					std::memcpy(members + code[pc + 1], frame + code[pc + 2], 8);
					pc += 3;
					break;
				case Op::STORE_MEMBER_STRING:
					SELF_ALIVE();
					*reinterpret_cast<String *>(members + code[pc + 1]) = STR(code[pc + 2]);
					pc += 3;
					break;
				case Op::STORE_MEMBER_VALUE:
					SELF_ALIVE();
					copy_value(types[code[pc + 1]], members + code[pc + 2], frame + code[pc + 3]);
					pc += 4;
					break;
				case Op::LOAD_STATIC:
					if (!op_load_static(code + pc, frame, fn, record.line)) {
						return false;
					}
					pc += 5;
					break;
				case Op::STORE_STATIC:
					if (!op_store_static(code + pc, frame, fn, record.line)) {
						return false;
					}
					pc += 5;
					break;
				case Op::INIT_STATIC:
					op_init_static(code + pc, frame);
					pc += 4;
					break;
				case Op::INT_TO_FLOAT:
					F64(code[pc + 1]) = double(I64(code[pc + 2]));
					pc += 3;
					break;
				case Op::FLOAT_TO_INT: {
					const double value = F64(code[pc + 2]);
					if (!(value >= -9223372036854775808.0 && value < 9223372036854775808.0)) {
						return fail_float_to_int(fn, record.line, value);
					}
					I64(code[pc + 1]) = int64_t(value);
					pc += 3;
					break;
				}
				case Op::ADD_INT:
					I64(code[pc + 1]) = WRAP(+);
					pc += 4;
					break;
				case Op::SUB_INT:
					I64(code[pc + 1]) = WRAP(-);
					pc += 4;
					break;
				case Op::MUL_INT:
					I64(code[pc + 1]) = WRAP(*);
					pc += 4;
					break;
				case Op::DIV_INT:
				case Op::MOD_INT: {
					const int64_t a = I64(code[pc + 2]);
					const int64_t b = I64(code[pc + 3]);
					if (b == 0) {
						return fail_cstr(fn, record.line, "Division by zero.");
					}
					const bool div = Op(code[pc]) == Op::DIV_INT;
					// The minimum int divided by -1 wraps instead of trapping.
					if (b == -1) {
						I64(code[pc + 1]) = div ? int64_t(0ull - uint64_t(a)) : 0;
					} else {
						I64(code[pc + 1]) = div ? a / b : a % b;
					}
					pc += 4;
					break;
				}
				case Op::NEG_INT:
					I64(code[pc + 1]) = int64_t(0ull - uint64_t(I64(code[pc + 2])));
					pc += 3;
					break;
				case Op::AND_INT:
					I64(code[pc + 1]) = I64(code[pc + 2]) & I64(code[pc + 3]);
					pc += 4;
					break;
				case Op::OR_INT:
					I64(code[pc + 1]) = I64(code[pc + 2]) | I64(code[pc + 3]);
					pc += 4;
					break;
				case Op::XOR_INT:
					I64(code[pc + 1]) = I64(code[pc + 2]) ^ I64(code[pc + 3]);
					pc += 4;
					break;
				case Op::NOT_INT:
					I64(code[pc + 1]) = ~I64(code[pc + 2]);
					pc += 3;
					break;
				case Op::SHL_INT:
				case Op::SHR_INT: {
					const int64_t a = I64(code[pc + 2]);
					const int64_t b = I64(code[pc + 3]);
					const bool left = Op(code[pc]) == Op::SHL_INT;
					if (a < 0 || uint64_t(b) >= 64) {
						return fail_shift(fn, record.line, a, b, left);
					}
					I64(code[pc + 1]) = left ? int64_t(uint64_t(a) << b) : a >> b;
					pc += 4;
					break;
				}
				case Op::POW_INT:
					if (!bytecode::int_power(I64(code[pc + 2]), I64(code[pc + 3]), I64(code[pc + 1]))) {
						return fail_pow_int(fn, record.line, I64(code[pc + 3]));
					}
					pc += 4;
					break;
				case Op::ADD_FLOAT:
					F64(code[pc + 1]) = F64(code[pc + 2]) + F64(code[pc + 3]);
					pc += 4;
					break;
				case Op::SUB_FLOAT:
					F64(code[pc + 1]) = F64(code[pc + 2]) - F64(code[pc + 3]);
					pc += 4;
					break;
				case Op::MUL_FLOAT:
					F64(code[pc + 1]) = F64(code[pc + 2]) * F64(code[pc + 3]);
					pc += 4;
					break;
				case Op::DIV_FLOAT:
					F64(code[pc + 1]) = F64(code[pc + 2]) / F64(code[pc + 3]);
					pc += 4;
					break;
				case Op::POW_FLOAT:
					// As the engine's Math::pow (core/math/math_funcs.h:185-187; ed1daf0).
					F64(code[pc + 1]) = std::pow(F64(code[pc + 2]), F64(code[pc + 3]));
					pc += 4;
					break;
				case Op::NEG_FLOAT:
					F64(code[pc + 1]) = -F64(code[pc + 2]);
					pc += 3;
					break;
				case Op::ADD_STRING:
					op_add_string(code + pc, frame);
					pc += 4;
					break;
				case Op::FORMAT:
					if (!op_format(code + pc, frame, fn, record.line)) {
						return false;
					}
					pc += 4;
					break;
#define COMPARE(m_op, m_access, m_cmp) \
	case Op::m_op: \
		U64(code[pc + 1]) = (m_access(code[pc + 2]) m_cmp m_access(code[pc + 3])) ? 1 : 0; \
		pc += 4; \
		break;
					COMPARE(EQ_INT, I64, ==)
					COMPARE(NE_INT, I64, !=)
					COMPARE(LT_INT, I64, <)
					COMPARE(LE_INT, I64, <=)
					COMPARE(GT_INT, I64, >)
					COMPARE(GE_INT, I64, >=)
					COMPARE(EQ_FLOAT, F64, ==)
					COMPARE(NE_FLOAT, F64, !=)
					COMPARE(LT_FLOAT, F64, <)
					COMPARE(LE_FLOAT, F64, <=)
					COMPARE(GT_FLOAT, F64, >)
					COMPARE(GE_FLOAT, F64, >=)
					COMPARE(EQ_BOOL, U64, ==)
					COMPARE(NE_BOOL, U64, !=)
					COMPARE(EQ_STRING, STR, ==)
					COMPARE(NE_STRING, STR, !=)
#undef COMPARE
				case Op::NOT:
					U64(code[pc + 1]) = U64(code[pc + 2]) ? 0 : 1;
					pc += 3;
					break;
				case Op::EVALUATE: {
					const uint32_t right = code[pc + 4];
					image.evaluators[code[pc + 1]](frame + code[pc + 3], right == NO_SLOT ? NOTHING : frame + right, frame + code[pc + 2]);
					pc += 5;
					break;
				}
				case Op::GET_FIELD:
					image.getters[code[pc + 1]](frame + code[pc + 3], frame + code[pc + 2]);
					pc += 4;
					break;
				case Op::SET_FIELD:
					image.setters[code[pc + 1]](frame + code[pc + 2], frame + code[pc + 3]);
					pc += 4;
					break;
				case Op::GET_PROPERTY:
					if (!op_get_property(code + pc, frame, freed, fn, record.line)) {
						return false;
					}
					pc += 4;
					break;
				case Op::CHECK_ENUM:
					if (!is_enum_value(types[code[pc + 1]], I64(code[pc + 2]))) {
						return fail_check_enum(fn, record.line, code[pc + 1], I64(code[pc + 2]));
					}
					pc += 3;
					break;
				case Op::HAS_MEMBER:
					if (!op_has_member(code + pc, frame, freed, fn, record.line)) {
						return false;
					}
					pc += 4;
					break;
				case Op::SET_PROPERTY:
					if (!op_set_property(code + pc, frame, freed, fn, record.line)) {
						return false;
					}
					pc += 4;
					break;
				case Op::NEW_OBJECT:
				case Op::NEW_WITHOUT_INIT: {
					bool initialized;
					const bool without_init = Op(code[pc]) == Op::NEW_WITHOUT_INIT;
					if (!new_object(image, code[pc + 1], without_init, frame + code[pc + 2], initialized)) {
						return fail_helper(fn, record.line);
					}
					pc += 3;
					if (!initialized && without_init) {
						// A program error ended the initializers: no _init() (spec §8.1).
						pc += bytecode::instruction_length(code + pc);
					}
					break;
				}
				case Op::GET_SCRIPT:
					if (!op_get_script(code + pc, frame, fn, record.line)) {
						return false;
					}
					pc += 3;
					break;
				case Op::GET_OWNER:
					if (!op_get_owner(code + pc, frame, freed, fn, record.line)) {
						return false;
					}
					pc += 3;
					break;
				case Op::GET_SELF:
					if (!op_get_self(code + pc, frame, freed, fn, record.line)) {
						return false;
					}
					pc += 2;
					break;
				case Op::GET_SINGLETON:
					if (!op_get_singleton(code + pc, frame, fn, record.line)) {
						return false;
					}
					pc += 3;
					break;
				case Op::GET_AUTOLOAD:
					if (!op_get_autoload(code + pc, frame, fn, record.line)) {
						return false;
					}
					pc += 3;
					break;
				case Op::METHOD_VALUE:
					if (!op_method_value(code + pc, frame, freed, fn, record.line)) {
						return false;
					}
					pc += 3;
					break;
				case Op::MAKE_LAMBDA:
					if (!op_make_lambda(code + pc, frame, freed, fn, record.line)) {
						return false;
					}
					pc += 5 + 2 * code[pc + 4];
					break;
				case Op::GET_SIGNAL:
					if (!op_get_signal(code + pc, frame, freed, fn, record.line)) {
						return false;
					}
					pc += 4;
					break;
				case Op::CHECK_ASSIGNED:
					if (is_none_object(frame + code[pc + 2])) {
						return fail_unassigned(fn, record.line, code[pc + 1]);
					}
					pc += 3;
					break;
				case Op::CHECK_RANGE: {
					const bytecode::IntRange range = bytecode::IntRange(code[pc + 1]);
					if (!bytecode::int_in_range(range, I64(code[pc + 2]))) {
						return fail_range(fn, record.line, range, I64(code[pc + 2]));
					}
					pc += 3;
					break;
				}
				case Op::CHECK_OBJECT:
					if (!op_check_object(code + pc, frame, fn, record.line)) {
						return false;
					}
					pc += 3;
					break;
				case Op::CHECK_LIVE:
					if (!is_live_object(frame + code[pc + 1])) {
						return fail_cstr(fn, record.line, "The object given to the engine was freed.");
					}
					pc += 2;
					break;
				case Op::DEBUG_BUILD:
#ifdef DEBUG_ENABLED
					U64(code[pc + 1]) = 1;
#else
					U64(code[pc + 1]) = 0;
#endif
					pc += 2;
					break;
				case Op::ASSERT_FAIL:
					return fail_assert(fn, record.line, frame, code[pc + 1]);
				case Op::CAST_OBJECT:
					op_cast_object(code + pc, frame);
					pc += 4;
					break;
				case Op::NEW_CONTAINER:
					op_new_container(code + pc, frame);
					pc += 3;
					break;
				case Op::GET_INDEX:
					if (!op_get_index(code + pc, frame, fn, record.line)) {
						return false;
					}
					pc += 6;
					break;
				case Op::SET_INDEX:
					if (!op_set_index(code + pc, frame, fn, record.line)) {
						return false;
					}
					pc += 5;
					break;
				case Op::IS_INSTANCE:
					op_is_instance(code + pc, frame);
					pc += 4;
					break;
				case Op::IS_NONE: {
					const uint8_t *src = frame + code[pc + 3];
					const TypeImage &type = types[code[pc + 1]];
					if (type.kind == bytecode::TypeKind::Option) {
						U64(code[pc + 2]) = U64(code[pc + 3]) ? 0 : 1;
					} else if (type.storage == ValueType::Object) {
						U64(code[pc + 2]) = is_live_object(src) ? 0 : 1; // A freed object reads as None in T? (spec §3.3).
					} else {
						const Variant &value = variant_at(src);
						U64(code[pc + 2]) = is_none(value) || (value.get_type() == Variant::OBJECT && !value.booleanize()) ? 1 : 0;
					}
					pc += 4;
					break;
				}
				case Op::SOME: {
					const TypeImage &option = types[code[pc + 1]];
					copy_value(*option.element, frame + code[pc + 2] + 8, frame + code[pc + 3]);
					U64(code[pc + 2]) = 1;
					pc += 4;
					break;
				}
				case Op::EQUAL:
					U64(code[pc + 2]) = equal_values(types[code[pc + 1]], frame + code[pc + 3], frame + code[pc + 4]) ? 1 : 0;
					pc += 5;
					break;
				case Op::SAME_OBJECT:
					U64(code[pc + 2]) = same_object(frame + code[pc + 3], code[pc + 1] & 1, frame + code[pc + 4], code[pc + 1] & 2) ? 1 : 0;
					pc += 5;
					break;
				case Op::LESS:
					U64(code[pc + 2]) = less_values(types[code[pc + 1]], frame + code[pc + 3], frame + code[pc + 4]) ? 1 : 0;
					pc += 5;
					break;
				case Op::TO_VARIANT:
					op_to_variant(code + pc, frame);
					pc += 4;
					break;
				case Op::FROM_VARIANT:
					if (!read_variant(types[code[pc + 1]], frame + code[pc + 3], frame + code[pc + 2])) {
						return fail_helper(fn, record.line);
					}
					pc += 4;
					break;
				case Op::VARIANT_OBJECT:
					set_object(frame + code[pc + 1], variant_at(frame + code[pc + 2]));
					pc += 3;
					break;
				case Op::IS_TYPE:
					U64(code[pc + 2]) = is_exactly(types[code[pc + 1]], variant_at(frame + code[pc + 3])) ? 1 : 0;
					pc += 4;
					break;
				case Op::CONSTRUCT_FROM:
					if (!construct_from(types[code[pc + 1]], frame + code[pc + 3], frame + code[pc + 2])) {
						return fail_helper(fn, record.line);
					}
					pc += 4;
					break;
				case Op::VARIANT_EQUAL:
				case Op::MATCHES:
					if (!compare_variants(frame + code[pc + 2], frame + code[pc + 3], Op(code[pc]) == Op::MATCHES, U64(code[pc + 1]))) {
						return fail_helper(fn, record.line);
					}
					pc += 4;
					break;
				case Op::BOX:
					op_box(code + pc, frame);
					pc += 5;
					break;
				case Op::ENCODE:
					op_encode(code + pc, frame);
					pc += 4;
					break;
				case Op::UNWRAP: {
					const TypeImage &type = types[code[pc + 1]];
					const uint8_t *src = frame + code[pc + 3];
					// An object's T? holds the object as T does, and a freed one reads
					// as None (spec §3.3); T? of another type holds a tag first.
					const bool object = type.kind == bytecode::TypeKind::Object;
					if (object ? !is_live_object(src) : U64(code[pc + 3]) == 0) {
						return fail_unwrap(fn, record.line, frame, code[pc + 4]);
					}
					copy_value(type, frame + code[pc + 2], object ? src : src + 8);
					pc += 5;
					break;
				}
				case Op::JUMP:
					pc = code[pc + 1];
					break;
				case Op::JUMP_IF_FALSE:
					pc = U64(code[pc + 1]) ? pc + 3 : code[pc + 2];
					break;
				case Op::JUMP_IF_TRUE:
					pc = U64(code[pc + 1]) ? code[pc + 2] : pc + 3;
					break;
				// Calls: target, dst, base, argc, then (type, offset) per argument.
				case Op::CALL: {
					const uint32_t target = code[pc + 1];
					const ClassImage *owner = module.functions[target].is_static || !context.self_image ? &image : context.self_image.get();
					const ClassImage::FunctionOrigin &origin = owner->function_origins[target];
					if (!call_function(origin.image ? *origin.image : *owner, origin.image ? uint32_t(origin.index) : target, context, code + pc, frame, module.functions[target].result, fn, record.line)) {
						return false;
					}
					pc += 5 + 2 * code[pc + 4];
					break;
				}
				case Op::CALL_SUPER:
				case Op::CALL_METHOD:
					if (!op_call_method(code + pc, frame, freed, fn, record.line)) {
						return false;
					}
					pc += 5 + 2 * code[pc + 4];
					break;
				case Op::CALL_VALUE:
					if (!op_call_value(code + pc, frame, fn, record.line)) {
						return false;
					}
					pc += 5 + 2 * code[pc + 4];
					break;
				case Op::CALL_AWAIT_ENGINE:
				case Op::CALL_ENGINE: {
					bool done = false;
					if (!op_call_engine(code + pc, frame, freed, p_async, pc, done, fn, record.line)) {
						return false;
					}
					if (done) {
						return true;
					}
					break;
				}
				case Op::CALL_AWAIT_BUILTIN: {
					bool done = false;
					if (!op_call_await_builtin(code + pc, frame, p_async, pc, done, fn, record.line)) {
						return false;
					}
					if (done) {
						return true;
					}
					break;
				}
				case Op::CALL_UTILITY:
				case Op::CALL_BUILTIN:
					if (!op_call_builtin(code + pc, frame, fn, record.line)) {
						return false;
					}
					pc += 5 + 2 * code[pc + 4];
					break;
				case Op::CONSTRUCT:
					op_construct(code + pc, frame);
					pc += 5 + 2 * code[pc + 4];
					break;
				case Op::TASK_CANCEL:
					if (!op_task_cancel(code + pc, frame, fn, record.line)) {
						return false;
					}
					pc += 2;
					break;
				case Op::AWAIT_TASK: {
					bool done = false;
					if (!op_await_task(code + pc, frame, p_async, pc, done, fn, record.line)) {
						return false;
					}
					if (done) {
						return true;
					}
					break;
				}
				case Op::AWAIT_SIGNAL:
					return op_await_signal(code + pc, frame, p_async, pc, fn, record.line);
				case Op::RETURN:
					if (p_result) {
						copy_value(types[fn.result.type], p_result, frame + code[pc + 1]);
					}
					return true;
				case Op::RETURN_VOID:
					return true;
				default:
					return fail_invalid_op(fn, record.line, code[pc]);
			}
		}
#undef I64
#undef U64
#undef F64
#undef STR
#undef WRAP
#undef SELF_ALIVE
	}
};

} // namespace

void CoroutineTask::_bind_methods() {
	ClassDB::bind_method(D_METHOD("cancel"), &CoroutineTask::cancel);
	ADD_SIGNAL(MethodInfo("completed", PropertyInfo(Variant::NIL, "result", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NIL_IS_VARIANT)));
}

void CoroutineTask::finish(State p_state, const Variant &p_result) {
	Ref<CoroutineTask> keep(this);
	Ref<CoroutineTask> child;
	{
		std::lock_guard<std::recursive_mutex> lock(mutex);
		if (ended()) {
			return;
		}
		result = p_state == State::Finished ? p_result : Variant();
		state = p_state;
		if (continuation) {
			const auto frame = std::move(continuation);
			frame->disconnect();
			child = frame->child;
			frame->child.unref();
			if (frame->data) {
				uint8_t *data = frame->data;
				frame->data = nullptr;
				frame->image->frames[frame->function].destroy(data);
				::operator delete(data);
			}
		}
	}
	if (p_state == State::Cancelled && child.is_valid()) {
		child->cancel();
	}
	{
		std::lock_guard<std::mutex> lock(tasks_mutex);
		active_tasks.erase(uint64_t(get_instance_id()));
	}
	emit_signal("completed", result);
}

void CoroutineTask::cancel() {
	{
		std::lock_guard<std::recursive_mutex> lock(mutex);
		if (ended()) {
			return;
		}
		cancel_requested = true;
		if (state != State::Suspended) {
			return; // A running coroutine ends itself once it stops.
		}
	}
	finish(State::Cancelled);
}
void CoroutineTask::cancel_owner() {
	{
		std::lock_guard<std::recursive_mutex> lock(mutex);
		if (ended()) {
			return;
		}
		owner_freed = true;
		owner_freed_frame = task_frame.load();
	}
	cancel();
}

void CoroutineFrame::disconnect() {
	++generation;
	if (!waiting.is_null() && waiting.get_object() && waiting.is_connected(callback)) {
		waiting.disconnect(callback);
	}
	if (!script_changed.is_null() && script_changed.get_object() && script_changed.is_connected(changed_callback)) {
		script_changed.disconnect(changed_callback);
	}
	object_watch::unwatch(watch);
	watch = {};
	waiting = Signal();
	callback = Callable();
	script_changed = Signal();
	changed_callback = Callable();
	held_state = Variant();
	waited_task.unref();
	lost_object = 0;
}

void CoroutineFrame::report_error(const String &p_message) {
	const auto &fn = image->module.functions[function];
	const bool already_running = !call_stack.empty() && call_stack.back().frame == data;
	if (!already_running) {
		if (call_stack.capacity() < MAX_CALL_DEPTH + 1) {
			call_stack.reserve(MAX_CALL_DEPTH + 1);
		}
		uint8_t *members = context.storage && !context.storage->freed.load() ? context.storage->members : nullptr;
		call_stack.push_back({ image.get(), context.self_image.get(), &fn, data, members, line });
	}
	vm::report_error(*image, fn.name.c_str(), line, p_message);
	if (!already_running) {
		call_stack.pop_back();
	}
}

bool CoroutineFrame::await_task(const Ref<CoroutineTask> &p_task) {
	if (p_task->state == CoroutineTask::State::Finished) {
		child.unref();
		return receive(p_task->result);
	}
	if (p_task->state == CoroutineTask::State::Failed) {
		return false;
	}
	if (p_task->owner_freed) {
		const uint64_t id = p_task->owner_id;
		if (p_task->owner_freed_frame != task_frame.load()) {
			report_error(vformat("The awaited object %d was freed or lost its script.", int64_t(id)));
			return false;
		}
		child.unref();
		awaited_freed = true;
		lost_object = id;
		task->state = CoroutineTask::State::Suspended;
		return true;
	}
	if (p_task->state == CoroutineTask::State::Cancelled) {
		report_error("The awaited task was cancelled.");
		return false;
	}
	waited_task = p_task;
	if (!suspend(Signal(p_task.ptr(), "completed"), destination, 0)) {
		report_error("Cannot await task completion.");
		return false;
	}
	return true;
}

bool CoroutineFrame::receive(const Variant &p_value) {
	if (destination == NO_SLOT) {
		return true;
	}
	for (const auto &slot : image->module.functions[function].slots) {
		if (slot.offset != destination) {
			continue;
		}
		const TypeImage &type = image->types[slot.type];
		if (from_variant(type, p_value, data + destination, true)) {
			return true;
		}
		report_error("Await result: " + entry_mismatch(type, p_value));
		return false;
	}
	return false;
}

bool CoroutineFrame::suspend(const Signal &p_signal, uint32_t p_dst, uint64_t p_watch, bool p_script) {
	destination = p_dst;
	waiting = p_signal;
	task->state = CoroutineTask::State::Suspended;
	const uint64_t ticket = ++generation;
	const std::weak_ptr<CoroutineFrame> weak = shared_from_this();
	const std::function<void(const Variant &)> resume = [weak, ticket](const Variant &value) {
		if (const auto frame = weak.lock()) {
			frame->complete_wait(ticket, value);
		}
	};
	callback = Callable(memnew(ResumeCallable(resume)));
	if (!waiting.get_object()) {
		awaited_freed = true;
		lost_object = p_watch;
		return true;
	}
	if (waiting.connect(callback) != OK) {
		return false;
	}
	if (p_watch) {
		Object *object = ObjectDB::get_instance(ObjectID(p_watch));
		if (!object) {
			awaited_freed = true;
			lost_object = p_watch;
		} else {
			watch = object_watch::watch(object->_owner, [weak, ticket](uint64_t id) {
				if (const auto frame = weak.lock(); frame && frame->generation == ticket) {
					frame->awaited_freed = true;
					frame->lost_object = id;
				}
			});
			if (p_script) {
				script_changed = Signal(object, "script_changed");
				const std::function<void(const Variant &)> changed = [weak, ticket, p_watch](const Variant &) {
					if (const auto frame = weak.lock(); frame && frame->generation == ticket) {
						frame->awaited_freed = true;
						frame->lost_object = p_watch;
					}
				};
				changed_callback = Callable(memnew(ResumeCallable(changed)));
				script_changed.connect(changed_callback);
			}
		}
	}
	return true;
}

void CoroutineFrame::complete_wait(uint64_t p_generation, const Variant &p_value) {
	const auto keep_frame = shared_from_this();
	Ref<CoroutineTask> keep(task);
	bool failed = false;
	{
		std::lock_guard<std::recursive_mutex> lock(task->mutex);
		if (p_generation != generation || task->ended() || awaited_freed) {
			return;
		}
		const Ref<CoroutineTask> completed = waited_task;
		disconnect();
		child.unref();
		if (completed.is_valid()) {
			failed = !await_task(completed);
			if (!failed && awaited_freed) {
				return;
			}
		} else {
			failed = !receive(p_value);
		}
	}
	if (failed) {
		task->finish(CoroutineTask::State::Failed);
		return;
	}
	resume();
}

void CoroutineFrame::resume() {
	const auto keep_frame = shared_from_this();
	Ref<CoroutineTask> keep_task(task);
	CoroutineTask::State ends = CoroutineTask::State::Running;
	Variant value;
	{
		std::lock_guard<std::recursive_mutex> lock(task->mutex);
		if (task->ended()) {
			return; // Cancelled on another thread meanwhile.
		}
		if (task->cancel_requested && pc != 0) {
			ends = CoroutineTask::State::Cancelled;
		} else {
			task->state = CoroutineTask::State::Running;
			const TypeImage &result_type = image->types[image->module.functions[function].result.type];
			ValueBuffer result(result_type);
			const bool ok = Execution(*image, context, debugger::is_active()).run(function, data, result.data, this);
			if (task->cancel_requested) {
				ends = CoroutineTask::State::Cancelled;
			} else if (!ok) {
				ends = CoroutineTask::State::Failed;
			} else if (task->state == CoroutineTask::State::Running) {
				ends = CoroutineTask::State::Finished;
				value = outward_variant(result_type, result.data);
			}
		}
	}
	if (ends != CoroutineTask::State::Running) {
		task->finish(ends, value);
	}
}

namespace tasks {
void cancel_owner(const std::shared_ptr<InstanceStorage> &p_storage) {
	p_storage->coroutines_cancelled = true;
	for (const auto &task : task_snapshot()) {
		// An ended task's object may be gone and its address reused; cancelling
		// an ended task does nothing.
		if (task->owner_storage == p_storage.get()) {
			task->cancel_owner();
		}
	}
}
void frame() {
	++task_frame;
	for (const auto &task : task_snapshot()) {
		// A task locked on another thread may be running there, and waiting for
		// it could deadlock; a later frame looks again.
		std::unique_lock<std::recursive_mutex> lock(task->mutex, std::try_to_lock);
		if (!lock.owns_lock()) {
			continue;
		}
		const auto frame = task->continuation;
		if (!frame || !frame->awaited_freed || task->ended()) {
			continue;
		}
		if (frame->context.storage && frame->context.storage->coroutines_cancelled) {
			lock.unlock();
			task->cancel_owner();
		} else {
			frame->report_error(vformat("The awaited object %d was freed or lost its script.", int64_t(frame->lost_object)));
			lock.unlock();
			task->finish(CoroutineTask::State::Failed);
		}
	}
}
void clear() {
	for (const auto &task : task_snapshot()) {
		task->cancel();
	}
}
} // namespace tasks

namespace vm {

Variant call(const std::shared_ptr<const ClassImage> &p_image, int p_function, const InstanceContext &p_context, const Variant **p_args, int64_t p_argc, GDExtensionCallError &r_error, bool *r_failed, const uint8_t *const *p_captures) {
	// Old code stays alive until this call returns, even if a hot reload
	// replaces the image meanwhile (spec §12.4).
	std::shared_ptr<const ClassImage> image = p_image;
	InstanceContext context = p_context;
	if (!context.self_image) {
		context.self_image = p_image;
	}
	ClassImage::resolve_function(image, p_function);
	const bytecode::Function &fn = image->module.functions[p_function];
	r_error.error = GDEXTENSION_CALL_OK;
	if (image->module.declarations_only) {
		ERR_PRINT(vformat("%s was only described for the editor; it runs once the project is built.", image->path));
		r_error.error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
		return Variant();
	}
	if (fn.is_abstract) {
		ERR_PRINT(vformat("%s: %s() is abstract, and this object's class does not implement it.", image->path, image->function_names[p_function]));
		r_error.error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
		return Variant();
	}
	// The last parameters may be left out when they have default values.
	const size_t given = fn.params.size() - fn.captures;
	const size_t required = given - fn.defaults.size();
	if (size_t(p_argc) < required || size_t(p_argc) > given) {
		r_error.error = size_t(p_argc) > given ? GDEXTENSION_CALL_ERROR_TOO_MANY_ARGUMENTS : GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS;
		r_error.expected = int32_t(size_t(p_argc) > given ? given : required);
		return Variant();
	}

	Frame frame(frame_stack, image->frames[p_function]);
	const TypeImage &result_type = image->types[fn.result.type];
	for (size_t i = 0; i < fn.params.size(); i++) {
		const bytecode::Slot &param = fn.params[i];
		const TypeImage &type = image->types[param.type];
		if (i >= given) {
			copy_value(type, frame.data + param.offset, p_captures[i - given]);
			continue;
		}
		// Like member initializers, defaults are built late, after script types load.
		Variant value = i < size_t(p_argc) ? *p_args[i] : Variant();
		String error;
		bool valid = i < size_t(p_argc) || constant_value(*image, fn.defaults[i - required], value, error);
		if (valid && !from_variant(type, value, frame.data + param.offset, true)) {
			error = entry_mismatch(type, value);
			valid = false;
		}
		if (!valid) {
			// A program error at the entry, and the engine gets the default value
			// (spec §8.1, §10.1); the engine's backtrace of the caller goes with it.
			report_error(*image, fn.name.c_str(), int(fn.line), vformat("Argument \"%s\" of %s(): %s.", to_godot(param.name), to_godot(fn.name), error));
			if (r_failed) {
				*r_failed = true;
			}
			return default_variant(result_type);
		}
	}

	if (fn.is_async) {
		const Ref<CoroutineTask> task = start_task(image, uint32_t(p_function), context, frame.data);
		if (r_failed) {
			*r_failed = task->state == CoroutineTask::State::Failed;
		}
		return task->ended() ? task->result : Variant(Signal(task.ptr(), "completed"));
	}
	ValueBuffer result(result_type);
	const bool ok = Execution(*image, context, debugger::is_active()).run(uint32_t(p_function), frame.data, result.data);
	if (r_failed) {
		*r_failed = !ok;
	}
	return ok ? outward_variant(result_type, result.data) : default_variant(result_type);
}

const std::vector<StackFrame> &stack() {
	return call_stack;
}

namespace {

// The pub static variable of that name of the class or a base: the image of
// the class that declares it, and its index there.
std::shared_ptr<const ClassImage> pub_static(const std::shared_ptr<const ClassImage> &p_image, const StringName &p_name, int &r_index) {
	for (std::shared_ptr<const ClassImage> owner = p_image; owner; owner = owner->base) {
		r_index = owner->find_static(p_name);
		if (r_index >= 0) {
			return owner->module.statics[r_index].is_pub ? owner : nullptr;
		}
	}
	return nullptr;
}

// Whether code may use the variable, reporting why not; the editor runs none.
bool static_usable(const ClassImage &p_owner, int p_index) {
	String error;
	if (Engine::get_singleton()->is_editor_hint() || p_owner.statics->state.load(std::memory_order_acquire) == Statics::State::Initialized || static_ready(p_owner, uint32_t(p_index), error)) {
		return true;
	}
	const bytecode::Member &variable = p_owner.module.statics[p_index];
	report_error(p_owner, variable.name.c_str(), int(variable.line), error);
	return false;
}

} // namespace

bool get_static(const std::shared_ptr<const ClassImage> &p_image, const StringName &p_name, Variant &r_value) {
	int index = -1;
	const std::shared_ptr<const ClassImage> owner = pub_static(p_image, p_name, index);
	if (!owner) {
		return false;
	}
	const bytecode::Member &variable = owner->module.statics[index];
	const TypeImage &type = owner->types[variable.type];
	if (!static_usable(*owner, index)) {
		r_value = default_variant(type);
	} else if (variable.getter >= 0 && !Engine::get_singleton()->is_editor_hint()) {
		GDExtensionCallError error;
		r_value = call(owner, variable.getter, InstanceContext(), nullptr, 0, error);
	} else {
		r_value = outward_variant(type, owner->statics->at(variable.offset));
	}
	return true;
}

bool set_static(const std::shared_ptr<const ClassImage> &p_image, const StringName &p_name, const Variant &p_value) {
	int index = -1;
	const std::shared_ptr<const ClassImage> owner = pub_static(p_image, p_name, index);
	if (!owner) {
		return false;
	}
	const bytecode::Member &variable = owner->module.statics[index];
	if (!static_usable(*owner, index)) {
		return true;
	}
	if (variable.setter >= 0 && !Engine::get_singleton()->is_editor_hint()) {
		const Variant *args[] = { &p_value };
		GDExtensionCallError error;
		call(owner, variable.setter, InstanceContext(), args, 1, error);
		return true;
	}
	const TypeImage &type = owner->types[variable.type];
	if (!from_variant(type, p_value, owner->statics->at(variable.offset), true)) {
		report_error(*owner, variable.name.c_str(), int(variable.line), vformat("\"%s\" was not written: %s.", p_name, entry_mismatch(type, p_value)));
	}
	return true;
}

bool initialize_statics(const std::shared_ptr<const ClassImage> &p_image) {
	bool ok = initialize_class(p_image);
	for (const std::shared_ptr<const ClassImage> &inner : p_image->inner_classes) {
		ok = initialize_statics(inner) && ok;
	}
	return ok;
}

void report_error(const ClassImage &p_image, const char *p_function, int p_line, const String &p_message) {
	// The debugger breaks while the stack is still intact (spec §12.5).
	debugger::error(p_message);
	const CharString description = p_message.utf8();
	const CharString file = String(p_image.source).utf8(); // An inner class's is its file's.
	godot::gdextension_interface::print_script_error(description.get_data(), p_function, file.get_data(), p_line, true);
}

} // namespace vm

} // namespace petrichor
