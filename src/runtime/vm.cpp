#include "runtime/vm.h"

#include "bytecode/opcodes.h"
#include "runtime/debugger.h"
#include "runtime/method_callable.h"
#include "runtime/object_watch.h"
#include "runtime/task.h"
#include "runtime/text.h"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/scene_tree_timer.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/callable_custom.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <array>
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

#ifdef DEBUG_ENABLED
struct SiteNames {
	String path;
	std::vector<StringName> functions;
};
#endif

// What a task runs: a function of this language, or for a task of a signal
// (spec §9.5) no function, with no image and no data, only the wait.
struct CoroutineFrame : std::enable_shared_from_this<CoroutineFrame> {
	std::shared_ptr<const ClassImage> image;
	InstanceContext context;
	// Kept by the frame, which a callback on another thread may hold after the
	// task ended; finish() lets go of the frame.
	TaskRef task;
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
	// suspend() set what disconnect() lets go of.
	bool waits = false;
	// To `waiting`, one-shot as GDScript's (modules/gdscript/gdscript_vm.cpp:2643),
	// so it is gone once the signal came.
	bool connected = false;
	Signal waiting;
	Callable callback;
	Signal script_changed;
	Callable changed_callback;
	object_watch::Subscription watch;
	Variant held_state;
	TaskRef waited_task;
	uint64_t waiter = 0; // What waited_task calls once it ends.
	TaskRef child;
	// The slots that held self when the frame was suspended.
	std::vector<std::pair<uint32_t, ValueType>> let_go_of_self;

	CoroutineFrame(const std::shared_ptr<const ClassImage> &p_image, uint32_t p_function, const InstanceContext &p_context, const TaskRef &p_task) :
			image(p_image), context(p_context), task(p_task), function(p_function), data(static_cast<uint8_t *>(::operator new(p_image->frames[p_function].size))) {
		image->frames[function].construct(data);
		line = int(image->module.functions[function].line);
	}
	CoroutineFrame(const InstanceContext &p_context, const TaskRef &p_task) :
			context(p_context), task(p_task), function(0), data(nullptr) {}
	~CoroutineFrame() {
		disconnect();
		if (data) {
			image->frames[function].destroy(data);
		}
		::operator delete(data);
	}
	void disconnect();
	bool receive(const Variant &p_value);
	bool await_task(const TaskRef &p_task);
	void report_error(const String &p_message);
	void resume();
	void complete_wait(uint64_t p_generation, const Variant &p_value);
	bool suspend(const Signal &p_signal, uint32_t p_dst, uint64_t p_watch, bool p_script = false);
	// The awaited object with the ObjectID was freed.
	void lose(uint64_t p_object);
};

// What a task does that waits for other tasks, its members, instead of
// running a function: t.settled() (spec §9.2), Task.any() and Task.all()
// (§9.5). It cancels the members it owns once it ends, and lets go of the
// others.
struct TaskJoin : std::enable_shared_from_this<TaskJoin> {
	enum class Kind {
		Settled,
		Any,
		All,
	};
	Kind kind = Kind::Settled;
	TaskRef task; // Its own, which keeps it until it ends.
	InstanceContext context;
	std::shared_ptr<const ClassImage> image; // That holds `result_type`.
	const TypeImage *result_type = nullptr; // The T of its Task[T]: Settled[T], int or void.
	// Where it was made, where awaiting a cancelled member is reported.
	std::string function;
	int line = 0;
	std::vector<TaskRef> members;
	std::vector<bool> owned;
	std::vector<bool> seen;
	size_t finished = 0;
	std::vector<uint64_t> waiters; // Each member's on_end().
	std::atomic<uint64_t> generation{ 0 };
	// A member cancelled as its object was freed in this frame: tasks::frame()
	// decides once the frame's deletions are done (§9.7).
	std::atomic<bool> awaited_freed{ false };
	size_t freed_member = 0;

	static TaskRef make(Kind p_kind, const std::vector<TaskRef> &p_members, const std::vector<bool> &p_owned, const TypeImage &p_result, const InstanceContext &p_context, const std::shared_ptr<const ClassImage> &p_image, const bytecode::Function &p_function, int p_line);
	void start();
	void member_ended(size_t p_member);
	Variant cancelled(const TaskState &p_member) const;
	void report_error(const String &p_message) const;
	std::vector<TaskRef> stop();
};

namespace {
// The lock of the active tasks: all of them, for clear(), each owner's, for
// cancel_owner(), and those whose awaited object was freed, for frame().
std::mutex tasks_mutex;
std::atomic<uint64_t> task_frame{ 0 };
uint64_t last_serial = 0;
TaskList all_tasks;
std::vector<TaskRef> undecided;

void append_task(TaskList &r_list, TaskLinks TaskState::*p_links, TaskState *p_task) {
	(p_task->*p_links).previous = r_list.last;
	(r_list.last ? (r_list.last->*p_links).next : r_list.first) = p_task;
	r_list.last = p_task;
}

void remove_task(TaskList &r_list, TaskLinks TaskState::*p_links, TaskState *p_task) {
	const TaskLinks links = p_task->*p_links;
	(links.previous ? (links.previous->*p_links).next : r_list.first) = links.next;
	(links.next ? (links.next->*p_links).previous : r_list.last) = links.previous;
	p_task->*p_links = TaskLinks();
}

void list_task(const TaskRef &p_task) {
	std::lock_guard<std::mutex> lock(tasks_mutex);
	p_task->serial = ++last_serial;
	append_task(all_tasks, &TaskState::among_all, p_task.get());
	if (p_task->owner_storage) {
		append_task(p_task->owner_storage->tasks, &TaskState::among_owners, p_task.get());
	}
}

// While its frame or join still keeps the owner's storage.
void unlist_task(TaskState *p_task) {
	std::lock_guard<std::mutex> lock(tasks_mutex);
	remove_task(all_tasks, &TaskState::among_all, p_task);
	if (p_task->owner_storage) {
		remove_task(p_task->owner_storage->tasks, &TaskState::among_owners, p_task);
	}
}

std::vector<TaskRef> listed_tasks(const TaskList &p_list, TaskLinks TaskState::*p_links) {
	std::lock_guard<std::mutex> lock(tasks_mutex);
	std::vector<TaskRef> tasks;
	for (TaskState *task = p_list.first; task; task = (task->*p_links).next) {
		tasks.emplace_back(task);
	}
	return tasks;
}

// The task's awaited object was freed: tasks::frame() decides on it once the
// frame's deletions are done (spec §9.7).
void decide_later(const TaskRef &p_task) {
	std::lock_guard<std::mutex> lock(tasks_mutex);
	undecided.push_back(p_task);
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
TaskRef start_task(const std::shared_ptr<const ClassImage> &p_image, uint32_t p_function, const InstanceContext &p_context, uint8_t *p_frame, const std::string *p_result_key = nullptr) {
	const bytecode::Function &fn = p_image->module.functions[p_function];
	const TaskRef task = TaskRef::make();
	task->owner_id = p_context.owner ? instance_id(p_context.owner) : 0;
	task->owner_storage = p_context.storage.get();
	task->result_signature = p_result_key ? *p_result_key : p_image->types[fn.result.type].result_key;
	task->continuation = std::make_shared<CoroutineFrame>(p_image, p_function, p_context, task);
	for (const auto &param : fn.params) {
		copy_value(p_image->types[param.type], task->continuation->data + param.offset, p_frame + param.offset);
	}
	if (fn.given != bytecode::NO_SLOT) {
		std::memcpy(task->continuation->data + fn.given, p_frame + fn.given, sizeof(int64_t));
	}
	list_task(task);
	if (p_context.storage && p_context.storage->coroutines_cancelled) {
		task->cancel_requested = true;
		task->owner_freed = true;
		task->owner_freed_frame = task_frame.load();
	}
	task->continuation->resume();
	return task;
}

// A task that finishes with what the signal passes (spec §9.5), as an await
// of it would receive it, and is cancelled once the signal's object is freed
// (§9.2); tasks::frame() decides that, as for other awaits (§9.7). Null when
// the signal cannot be connected to.
TaskRef signal_task(const InstanceContext &p_context, const Signal &p_signal, const std::string &p_result_key) {
	const TaskRef task = TaskRef::make();
	task->owner_id = p_context.owner ? instance_id(p_context.owner) : 0;
	task->owner_storage = p_context.storage.get();
	task->result_signature = p_result_key;
	task->continuation = std::make_shared<CoroutineFrame>(p_context, task);
	list_task(task);
	if (p_context.storage && p_context.storage->coroutines_cancelled) {
		task->owner_freed = true;
		task->owner_freed_frame = task_frame.load();
		task->finish(TaskState::State::Cancelled);
		return task;
	}
	std::lock_guard<std::recursive_mutex> lock(task->mutex);
	if (!task->continuation->suspend(p_signal, NO_SLOT, uint64_t(p_signal.get_object_id()))) {
		task->finish(TaskState::State::Failed);
		return TaskRef();
	}
	return task;
}

// A member that must be assigned and is not as the node becomes ready, or as
// its scene does for one that ancestors assign, with the ways to assign it
// that the compiler checks (spec §6.2).
String unassigned_at_ready(const ClassImage &p_image, uint32_t p_member, bool p_by_ancestors) {
	const String name = p_image.member_names[p_member];
	if (p_by_ancestors) {
		return vformat("\"%s\" has not been assigned by the time the scene is ready. Assign it on every path of _init(), or in an ancestor's _ready() through $Child.%s = ... or an @onready reference, so that the compiler checks it.", name, name);
	}
	const char *scene = p_image.module.members[p_member].is_export ? "set it in the scene, " : "";
	return vformat("\"%s\" has not been assigned before ready. Assign it on every path of _init(), %sor pass it to new(%s = ...) or instantiate(%s = ...), so that the compiler checks it.", name, scene, name, name);
}

// The members of the class that ancestors must have assigned once the owner
// of the node is ready (spec §6.4); one an ancestor assigns after an await is
// too late.
void check_from_ancestors(const ClassImage &p_image, const InstanceStorage &p_storage) {
	if (p_storage.freed) {
		return;
	}
	for (uint32_t m = p_image.module.inherited_members; m < p_image.module.members.size(); m++) {
		const bytecode::Member &member = p_image.module.members[m];
		if (member.must_assign && !member.is_export && !p_image.assigned(p_storage.members, m)) {
			vm::report_error(p_image, "_ready", int(member.line), unassigned_at_ready(p_image, m, true));
		}
	}
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

#ifdef DEBUG_ENABLED
struct Freed {
	uint64_t object = 0;
	Site site;
	bool queued = false;
};
struct Made {
	uint64_t object = 0;
	Site site;
};
// What a thread records for program errors and for the listing at exit. Its
// latest frees are in a ring that allocates nothing to record one: an older
// free shows no place, as AddressSanitizer forgets frees that left its
// quarantine. What its new() made is pruned of what was freed whenever it
// doubles.
struct ThreadRecords {
	static constexpr size_t FREES = 4096;
	std::array<Freed, FREES> frees;
	size_t freed = 0;
	std::vector<Made> made;
	size_t prune_at = 1024;
	std::atomic<bool> owned{ true }; // By a thread; one that ended leaves them to the next.
	std::atomic<bool> held{ false };
};
// A hold on a thread's records. A reader waits for it; the thread itself,
// recording, skips the record rather than wait. Its release is a plain store,
// where a mutex's costs a second atomic operation.
class Holding {
	ThreadRecords *records;

public:
	Holding(ThreadRecords &p_records, bool p_wait) :
			records(&p_records) {
		while (records->held.exchange(true, std::memory_order_acquire)) {
			if (!p_wait) {
				records = nullptr;
				return;
			}
			std::this_thread::yield();
		}
	}
	~Holding() {
		if (records) {
			records->held.store(false, std::memory_order_release);
		}
	}
	Holding(const Holding &) = delete;
	Holding &operator=(const Holding &) = delete;
	explicit operator bool() const { return records; }
};
// Every thread's records, which outlive the thread, so that what happened on
// threads that ended stays known, and the names of the sites. They are never
// deleted, as a thread may end after the library's statics; the listing at
// exit lets go of the names.
std::mutex all_records_lock;
std::vector<ThreadRecords *> all_records;
std::vector<SiteNames *> all_site_names;

// The statement of this language running on this thread.
Site here() {
	const StackFrame &frame = call_stack.back();
	const ClassImage &image = *frame.image;
	const SiteNames *names = image.site_names.load(std::memory_order_acquire);
	if (!names) {
		std::lock_guard<std::mutex> guard(all_records_lock);
		names = image.site_names.load(std::memory_order_relaxed);
		if (!names) {
			names = all_site_names.emplace_back(new SiteNames{ image.path, image.function_names });
			image.site_names.store(names, std::memory_order_release);
		}
	}
	return { names, uint32_t(frame.function - image.module.functions.data()), frame.line };
}

struct OwnRecords {
	ThreadRecords *records = nullptr;
	~OwnRecords() {
		if (records) {
			records->owned.store(false, std::memory_order_release);
		}
	}
};
thread_local OwnRecords thread_records;

ThreadRecords &own_records() {
	if (thread_records.records) {
		return *thread_records.records;
	}
	std::lock_guard<std::mutex> guard(all_records_lock);
	for (ThreadRecords *records : all_records) {
		bool owned = false;
		if (records->owned.compare_exchange_strong(owned, true, std::memory_order_acquire)) {
			return *(thread_records.records = records);
		}
	}
	return *(thread_records.records = all_records.emplace_back(new ThreadRecords()));
}

// Keeps where this language's new() made an engine object (spec §6.7).
void record_made(uint64_t p_object) {
	const Site site = here();
	ThreadRecords &records = own_records();
	const Holding holding(records, false);
	if (!holding) {
		return;
	}
	std::vector<Made> &made = records.made;
	if (made.size() >= records.prune_at) {
		made.erase(std::remove_if(made.begin(), made.end(), [](const Made &p_made) { return !gdextension_interface::object_get_instance_from_id(p_made.object); }), made.end());
		records.prune_at = std::max<size_t>(1024, 2 * made.size());
	}
	made.push_back({ p_object, site });
}
#endif

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

	// With `p_defaults`, those of the last parameters follow the arguments.
	Arguments(const std::vector<TypeImage> &p_types, uint8_t *p_frame, const uint32_t *p_pairs, uint32_t p_count, const std::vector<ValueType> *p_params, const std::vector<const void *> *p_defaults = nullptr) {
		const uint32_t total = p_defaults ? uint32_t(p_params->size()) : p_count;
		if (total > INLINE) {
			heap = new Heap();
			heap->pointers.resize(total);
			heap->variants.reserve(p_count); // Pointers into them stay valid.
			heap->views.reserve(p_count);
		}
		pointers = heap ? heap->pointers.data() : inline_pointers;
		for (uint32_t i = p_count; i < total; i++) {
			pointers[i] = (*p_defaults)[i - (total - p_defaults->size())];
		}
		for (uint32_t i = 0; i < p_count; i++) {
			const TypeImage &type = p_types[p_pairs[2 * i]];
			uint8_t *slot = p_frame + p_pairs[2 * i + 1];
			if (p_params && i < p_params->size() && (*p_params)[i] != ValueType::Variant) {
				// The engine reads an object as its pointer (core/variant/method_ptrcall.h:251-260).
				pointers[i] = (*p_params)[i] == ValueType::Object ? slot + 8 : slot;
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
			destroy_value(ValueType::Variant, inline_variants + i * sizeof(Variant));
		}
		delete heap;
	}
	Arguments(const Arguments &) = delete;
	Arguments &operator=(const Arguments &) = delete;

	const Variant **as_variants() { return reinterpret_cast<const Variant **>(pointers); }
};

void report_error(const ClassImage &p_image, const bytecode::Function &p_function, int p_line, const String &p_message) {
	vm::report_error(p_image, p_function.name.c_str(), p_line, p_message);
}

// An argument of a call from the engine that fails its entry check (spec §8.1, §10.1).
void report_argument(const ClassImage &p_image, const bytecode::Function &p_function, size_t p_param, const String &p_error) {
	report_error(p_image, p_function, int(p_function.line), vformat("Argument \"%s\" of %s(): %s.", to_godot(p_function.params[p_param].name), to_godot(p_function.name), p_error));
}

// A rest parameter of type `p_type` (spec §4.6): the `p_count` arguments that
// `p_argument(i)` gives, each checked as a parameter of the element type is
// (§8.1). False with what entry_check() gives on a mismatch in `r_error`.
template <typename Argument>
bool fill_rest(const TypeImage &p_type, uint8_t *p_slot, size_t p_count, Argument p_argument, String &r_error) {
	Array rest = typed_array(p_type.check.element, p_type.check.class_name, p_type.check.script);
	for (size_t i = 0; i < p_count; i++) {
		const Variant value = p_argument(i);
		if (!p_type.item) {
			rest.push_back(value);
			continue;
		}
		const TypeImage &item = *p_type.item;
		ValueBuffer element(item);
		if (!entry_check(item, value, element.data, r_error)) {
			if (!r_error.is_empty()) {
				r_error = vformat("%s at index %d", r_error, int64_t(i));
			}
			return false;
		}
		rest.push_back(item.box ? make_box(item, element.data, false) : to_variant(item, element.data));
	}
	*reinterpret_cast<Array *>(p_slot) = rest;
	return true;
}

template <typename T>
int64_t packed_size(const uint8_t *p_slot) {
	return reinterpret_cast<const T *>(p_slot)->size();
}

// Of an Array, a packed array or a String, whose elements are its characters.
int64_t array_size(ValueType p_type, const uint8_t *p_slot) {
	switch (p_type) {
		case ValueType::Array:
			return reinterpret_cast<const Array *>(p_slot)->size();
		case ValueType::String:
			return reinterpret_cast<const String *>(p_slot)->length();
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
	if (!p_type || p_type->box) {
		return variant_at(p_slot);
	}
	return to_variant(*p_type, p_slot);
}

// A key as a diagnostic shows it, a box as its value.
String key_text(const Variant &p_key) {
	return type_of(p_key) == Variant::CALLABLE ? String(p_key) : UtilityFunctions::var_to_str(p_key);
}

// Whether calling the Callable would reach an object that was freed, or none,
// which is a program error of the caller (spec §3.3, §3.11): the engine checks
// that of a Callable that is not custom only in debug builds, and of a custom
// one tells no caller that sees only the result
// (core/variant/callable.cpp:44-70, ed1daf0).
bool target_freed(const Callable &p_callable) {
	return p_callable.is_custom() ? !p_callable.is_valid() : !p_callable.get_object();
}

// What is read out of a container is checked against its type where the
// engine does not keep it: a container below the first level (spec §3.8), and
// an enum of this language, which the engine holds as int (§3.7).
bool checked_when_read(const TypeImage &p_type) {
	if (p_type.kind == bytecode::TypeKind::Option) {
		return checked_when_read(*p_type.element);
	}
	return p_type.enum_checked || p_type.kind == bytecode::TypeKind::Array || p_type.kind == bytecode::TypeKind::Dictionary;
}

// "Expected Array[int], found Array.", as CHECK_OBJECT says it.
String read_error(const TypeImage &p_type, const Variant &p_value) {
	const String mismatch = entry_mismatch(p_type, p_value);
	return mismatch.substr(0, 1).to_upper() + mismatch.substr(1) + ".";
}

enum class ElementRead {
	Done,
	Past, // The index is past the end.
	Misfit, // An element of the Array does not fit the type.
};

// The element at a non-negative index of an Array or a packed array, into its
// slot as GET_INDEX reads it (spec §3.8).
ElementRead read_element(const TypeImage &p_base, const TypeImage &p_element, const uint8_t *p_container, int64_t p_index, uint8_t *p_dst) {
	if (p_base.storage != ValueType::Array) {
		if (p_index >= array_size(p_base.storage, p_container)) {
			return ElementRead::Past;
		}
		get_packed_element(p_base.storage, p_container, p_index, p_dst);
		return ElementRead::Done;
	}
	// Null past the end, without an error (core/extension/gdextension_interface.cpp:1289-1295).
	const Variant *value = static_cast<const Variant *>(gdextension_interface::array_operator_index_const(p_container, p_index));
	if (!value) {
		return ElementRead::Past;
	}
	// As from_variant() reads a scalar, without its calls.
	const bool fits = p_element.kind == bytecode::TypeKind::Value && is_scalar(p_element.storage) ? scalar_from_variant(p_element.storage, *value, p_dst) : from_variant(p_element, *value, p_dst, checked_when_read(p_element));
	return fits ? ElementRead::Done : ElementRead::Misfit;
}

// The program error of an element that does not fit, read again.
String misfit_error(const TypeImage &p_element, const uint8_t *p_array, int64_t p_index) {
	return read_error(p_element, (*reinterpret_cast<const Array *>(p_array))[p_index]);
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
PETRICHOR_NOINLINE bool arguments_fit(const bytecode::BuiltinMethodCall &p_method, const bytecode::ContainerMethod &p_container, const void *const *p_args, const uint8_t *p_base) {
	String &r_error = helper_error();
	const Variant container = to_variant(p_method.base, p_base);
	for (uint32_t i = 0; i < 2 && i < p_method.params.size(); i++) {
		const ElementRole role = p_container.args[i];
		if (role == ElementRole::None || (role == ElementRole::Entries && !p_container.fills) || (i == 1 && p_method.method == "get")) {
			continue;
		}
		const Variant value = p_method.params[i] == ValueType::Variant ? *static_cast<const Variant *>(p_args[i]) : to_variant(p_method.params[i], static_cast<const uint8_t *>(p_args[i]));
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
#ifdef DEBUG_ENABLED
	if (p_image.class_scripts[p_class].is_empty()) {
		record_made(*reinterpret_cast<const uint64_t *>(p_slot)); // Its ObjectID.
	}
#endif
	return true;
}

const char *const TOO_DEEP = "The value nests more than 100 levels deep, or holds itself.";

// Whether a walk went deeper than walks go since this was last asked (spec
// §3.6). The count, mostly 0, spares reading the thread's flag.
bool too_deep() {
	return unasked_walks.load(std::memory_order_relaxed) != 0 && walked_too_deep();
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
	if (too_deep()) {
		helper_error() = TOO_DEEP;
		return false;
	}
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

	// With where this language freed the object, in debug builds (spec §3.3).
	PETRICHOR_NOINLINE bool fail_freed(const bytecode::Function &p_fn, int p_line, const char *p_message, uint64_t p_object) {
		return fail(p_fn, p_line, p_message + vm::freed_site(p_object));
	}

	PETRICHOR_NOINLINE bool fail_self_freed(const bytecode::Function &p_fn, int p_line) {
		return fail_freed(p_fn, p_line, "self was freed, or lost its script, while this method ran.", context.storage->owner_id);
	}

	PETRICHOR_NOINLINE bool fail_helper(const bytecode::Function &p_fn, int p_line) {
		report_error(image, p_fn, p_line, helper_error());
		return false;
	}

	PETRICHOR_NOINLINE bool fail_too_deep(const bytecode::Function &p_fn, int p_line) {
		return fail_cstr(p_fn, p_line, TOO_DEEP);
	}

	// Reports an instruction's walk that went deeper than walks go (spec §3.6).
	// The helpers of instructions that may walk ask, not the dispatch loop: a
	// check there slowed loops that never walk.
	bool within_depth(const bytecode::Function &p_fn, int p_line) {
		return !too_deep() || fail_too_deep(p_fn, p_line);
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

	PETRICHOR_NOINLINE bool fail_unassigned(const bytecode::Function &p_fn, int p_line, const ClassImage &p_owner, uint32_t p_member) {
		// Spec §6.3; §6.2: no one assigned a member that must be.
		return fail(p_fn, p_line, vformat(p_owner.module.members[p_member].is_onready ? "\"%s\" is read before the node is ready." : "\"%s\" has not been assigned.", p_owner.member_names[p_member]));
	}

	// The class's own members that must be assigned, as the node becomes ready
	// (spec §6.4). Of a node whose owner is not ready yet, those not exported
	// come from ancestors, and are checked when the owner is, after its _ready
	// (scene/main/node.cpp:336-337).
	PETRICHOR_NOINLINE bool check_provided(const bytecode::Function &p_fn, uint8_t *p_members) {
		Node *node = Object::cast_to<Node>(context.owner);
		Node *scene = node ? node->get_owner() : nullptr;
		const bool later = scene && !scene->is_node_ready();
		bool deferred = false;
		for (uint32_t m = image.module.inherited_members; m < image.module.members.size(); m++) {
			const bytecode::Member &member = image.module.members[m];
			if (!member.must_assign || image.assigned(p_members, m)) {
				continue;
			}
			if (later && !member.is_export) {
				deferred = true;
				continue;
			}
			return fail(p_fn, int(member.line), unassigned_at_ready(image, m, false));
		}
		if (deferred) {
			std::function<void(const Variant &)> check = [image = image.shared_from_this(), storage = context.storage](const Variant &) { check_from_ancestors(*image, *storage); };
			scene->connect("ready", Callable(memnew(ResumeCallable(std::move(check)))), Object::CONNECT_ONE_SHOT);
		}
		return true;
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

	// unwrap() or expect(message) of Err(e), which it shows, as Rust's does.
	PETRICHOR_NOINLINE bool fail_unwrap_err(const bytecode::Function &p_fn, int p_line, const uint8_t *p_frame, const TypeImage &p_type, const uint8_t *p_value, uint32_t p_message) {
		const String found = value_text(p_type, p_value);
		return fail(p_fn, p_line, p_message == NO_SLOT ? vformat("unwrap() found %s.", found) : vformat("%s: %s", *reinterpret_cast<const String *>(p_frame + p_message), found));
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
				fail_self_freed(p_fn, p_line);
				return nullptr;
			}
			return &self();
		}
		if (!is_live_object(p_frame + p_offset)) {
			if (is_none_object(p_frame + p_offset)) {
				fail_cstr(p_fn, p_line, "The value is None.");
			} else {
				fail_freed(p_fn, p_line, "The object was freed.", *reinterpret_cast<const uint64_t *>(p_frame + p_offset));
			}
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
	// Unless `p_typed`, the call's types are not the function's at all: what is
	// laid out alike is checked too, unless its layout says all its type does:
	// an engine value type, but no function type, which is never null.
	// A lambda's captures follow the arguments. As an engine entry, an argument
	// that fails the entry check is reported as the engine's call reports it.
	PETRICHOR_NOINLINE bool call_function(const ClassImage &p_callee, uint32_t p_index, const InstanceContext &p_context, const uint32_t *p_call, uint8_t *frame, const bytecode::Slot &p_result, const bytecode::Function &p_caller, int p_line, const uint8_t *p_captures = nullptr, bool p_entry = false, bool p_typed = true) {
		const auto copies = [&](const TypeImage &p_from, const TypeImage &p_to) {
			return p_from.layout == p_to.layout && (p_typed || (p_to.kind == bytecode::TypeKind::Value && p_to.check.nullable));
		};
		const uint32_t dst = p_call[2], argc = p_call[4];
		const bytecode::Function &method = p_callee.module.functions[p_index];
		if (!method.is_static && !method.is_lambda && !p_context.storage) {
			return fail_cstr(p_caller, p_line, "An instance method needs a receiver. Build the project again.");
		}
		if (method.is_abstract) {
			return fail(p_caller, p_line, vformat("%s() is abstract, and this object's class does not implement it. Build the project again.", p_callee.function_names[p_index]));
		}
		if (!method.takes(argc)) {
			return fail_cstr(p_caller, p_line, "Override argument count changed. Rebuild the project.");
		}
		const size_t given = method.params.size() - method.captures;
		const size_t fixed = method.fixed_params();
		Frame callee_frame(frames, p_callee.frames[p_index]);
		if (method.given != NO_SLOT) {
			*reinterpret_cast<int64_t *>(callee_frame.data + method.given) = int64_t(argc);
		}
		const uint8_t *capture = p_captures;
		for (size_t i = 0; i < method.params.size(); i++) {
			uint8_t *param = callee_frame.data + method.params[i].offset;
			const TypeImage &param_type = p_callee.types[method.params[i].type];
			if (i >= given) {
				copy_value(param_type, param, capture);
				capture += param_type.size;
				continue;
			}
			String error;
			if (i == fixed) {
				const auto argument = [&](size_t p_rest) {
					const uint32_t *pair = p_call + 5 + 2 * (fixed + p_rest);
					return to_variant(image.types[pair[0]], frame + pair[1]);
				};
				if (fill_rest(param_type, param, argc > fixed ? argc - fixed : 0, argument, error)) {
					continue;
				}
			} else if (i >= argc) {
				continue; // The callee computes its default.
			} else if (copies(image.types[p_call[5 + 2 * i]], param_type)) {
				copy_value(param_type, param, frame + p_call[6 + 2 * i]);
				continue;
			} else if (entry_check(param_type, to_variant(image.types[p_call[5 + 2 * i]], frame + p_call[6 + 2 * i]), param, error)) {
				continue;
			}
			if (error.is_empty()) {
				return false; // A default's program error, which was reported.
			}
			if (p_entry) {
				report_argument(p_callee, method, i, error);
				return false;
			}
			return fail(p_caller, p_line, vformat("Argument \"%s\" of %s(): %s.", to_godot(method.params[i].name), p_callee.function_names[p_index], error));
		}
		const TypeImage &result_type = image.types[p_result.type];
		if (method.is_async) {
			TaskRef task = start_task(p_callee.shared_from_this(), p_index, p_context, callee_frame.data, &result_type.result_key);
			if (dst != NO_SLOT) {
				task_at(frame + dst) = std::move(task);
			}
			return true;
		}
		const bool here = &p_callee == &image && &p_context == &context;
		const auto run_callee = [&](uint8_t *p_returned) {
			return here ? run(p_index, callee_frame.data, p_returned) : Execution(p_callee, p_context, debugging).run(p_index, callee_frame.data, p_returned);
		};
		const TypeImage &returns = p_callee.types[method.result.type];
		if (dst == NO_SLOT || copies(returns, result_type)) {
			return run_callee(dst == NO_SLOT ? nullptr : frame + dst);
		}
		ValueBuffer returned(returns);
		if (!run_callee(returned.data)) {
			return false;
		}
		String error;
		if (entry_check(result_type, outward_variant(returns, returned.data), frame + dst, error)) {
			return true;
		}
		return !error.is_empty() && fail(p_caller, p_line, vformat("Result of %s(): %s.", p_callee.function_names[p_index], error));
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
		return within_depth(p_fn, p_line);
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
		return within_depth(p_fn, p_line);
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
		return within_depth(p_fn, p_line);
	}

	// The slot of the member that GET_MEMBER_OF or SET_MEMBER_OF names, of
	// the object in `p_object`, found by name in its current image (spec
	// §10.5); null after a program error.
	PETRICHOR_NOINLINE uint8_t *member_slot(const uint32_t *p_op, uint32_t p_object, uint8_t *p_frame, const std::atomic<bool> &p_freed, const bytecode::Function &p_fn, int p_line, ScriptedObject &r_object, int &r_member) {
		ObjectView view(nullptr);
		Variant *receiver = resolve_object(p_frame, p_object, p_freed, view, p_fn, p_line);
		if (!receiver) {
			return nullptr;
		}
		if (!scripted_object(object_pointer(*receiver), r_object)) {
			fail_cstr(p_fn, p_line, "The receiver is not a Petrichor instance.");
			return nullptr;
		}
		// Within a run every script is compiled against the others (spec §12.4).
		const StringName &name = image.properties[p_op[1]];
		r_member = r_object.image->find_member(name);
		const bytecode::Member *member = r_member >= 0 ? &r_object.image->module.members[r_member] : nullptr;
		if (!member || r_object.image->types[member->type].layout != image.types[image.module.properties[p_op[1]].type].layout) {
			fail(p_fn, p_line, vformat("\"%s\" is not the member this script was compiled against. Build the project again.", name));
			return nullptr;
		}
		return r_object.context.storage->members + member->offset;
	}

	PETRICHOR_NOINLINE bool op_get_member_of(const uint32_t *p_op, uint8_t *p_frame, const std::atomic<bool> &p_freed, const bytecode::Function &p_fn, int p_line) {
		ScriptedObject object;
		int member = -1;
		const uint8_t *slot = member_slot(p_op, p_op[3], p_frame, p_freed, p_fn, p_line, object, member);
		if (!slot) {
			return false;
		}
		const ClassImage &owner = *object.image;
		if (owner.module.members[member].tracks_assignment() && !owner.assigned(object.context.storage->members, uint32_t(member))) {
			return fail_unassigned(p_fn, p_line, owner, uint32_t(member));
		}
		copy_value(image.types[image.module.properties[p_op[1]].type], p_frame + p_op[2], slot);
		return true;
	}

	PETRICHOR_NOINLINE bool op_set_member_of(const uint32_t *p_op, uint8_t *p_frame, const std::atomic<bool> &p_freed, const bytecode::Function &p_fn, int p_line) {
		ScriptedObject object;
		int member = -1;
		uint8_t *slot = member_slot(p_op, p_op[2], p_frame, p_freed, p_fn, p_line, object, member);
		if (!slot) {
			return false;
		}
		copy_value(image.types[image.module.properties[p_op[1]].type], slot, p_frame + p_op[3]);
		const ClassImage &owner = *object.image;
		if (owner.module.members[member].tracks_assignment()) {
			owner.assigned(object.context.storage->members, uint32_t(member)) = 1;
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
			return fail_self_freed(p_fn, p_line);
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
			return fail_self_freed(p_fn, p_line);
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
		ObjectView view(nullptr);
		Variant *object = resolve_object(p_frame, p_op[3], p_freed, view, p_fn, p_line);
		if (!object) {
			return false;
		}
		const StringName name = image.values[p_op[1]];
		Object *owner = static_cast<Object *>(*object);
		*reinterpret_cast<Callable *>(p_frame + p_op[2]) = Op(p_op[0]) == Op::METHOD_VALUE ? Callable(memnew(MethodCallable(owner, name))) : Callable(owner, name);
		return true;
	}

	PETRICHOR_NOINLINE bool op_struct_method_value(const uint32_t *p_op, uint8_t *p_frame, const bytecode::Function &p_fn, int p_line) {
		const uint32_t target = p_op[1];
		const StringName &name = image.method_names[target];
		std::shared_ptr<const ClassImage> owner = script_image(image.class_scripts[image.module.method_calls[target].static_class]);
		const int index = owner ? owner->find_function(name) : -1;
		// Its struct is copied in the layout this script was compiled against.
		if (index < 0 || !owner->has_signature(name, image.call_signatures[target])) {
			return fail(p_fn, p_line, vformat("%s() is not the method this script was compiled against. Build the project again.", name));
		}
		*reinterpret_cast<Callable *>(p_frame + p_op[2]) = Callable(memnew(StructMethodCallable(owner, index, p_frame + p_op[3])));
		return true;
	}

	PETRICHOR_NOINLINE void op_static_value(const uint32_t *p_op, uint8_t *p_frame) {
		const String &script = image.class_scripts[p_op[1]];
		const bool of_engine = script.is_empty();
		*reinterpret_cast<Callable *>(p_frame + p_op[2]) = Callable(memnew(StaticCallable(of_engine ? String(image.classes[p_op[1]]) : script, image.values[p_op[3]], of_engine)));
	}

	PETRICHOR_NOINLINE bool op_make_lambda(const uint32_t *p_op, uint8_t *p_frame, const std::atomic<bool> &p_freed, const bytecode::Function &p_fn, int p_line) {
		const uint32_t lambda = p_op[1];
		const bool on_self = !image.module.functions[lambda].is_static;
		if (on_self && p_freed.load(std::memory_order_relaxed)) {
			return fail_self_freed(p_fn, p_line);
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
		// The engine's Signal(object, name), which takes the object's pointer
		// (core/variant/variant_construct.h:375-377), not the wrapper godot-cpp
		// finds under a lock of the object's.
		static const GDExtensionPtrConstructor construct = gdextension_interface::variant_get_ptr_constructor(GDEXTENSION_VARIANT_TYPE_SIGNAL, 2);
		const GDExtensionObjectPtr pointer = object_pointer(*object);
		const void *args[] = { &pointer, image.native_values[p_op[1]] };
		uint8_t *dst = p_frame + p_op[2];
		destroy_value(ValueType::Signal, dst);
		construct(dst, args);
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

	PETRICHOR_NOINLINE void op_is_script(const uint32_t *p_op, uint8_t *p_frame) {
		const uint8_t *src = p_frame + p_op[2];
		const Object *object = is_live_object(src) ? ObjectView(src).get().get_validated_object() : nullptr;
		*reinterpret_cast<uint64_t *>(p_frame + p_op[1]) = object && extends_script(object->get_script(), ObjectView(p_frame + p_op[3]).get()) ? 1 : 0;
	}

	PETRICHOR_NOINLINE void op_new_container(const uint32_t *p_op, uint8_t *p_frame) {
		const TypeImage &type = image.types[p_op[1]];
		if (type.storage == ValueType::Dictionary) {
			*reinterpret_cast<Dictionary *>(p_frame + p_op[2]) = typed_dictionary(type.check);
		} else {
			*reinterpret_cast<Array *>(p_frame + p_op[2]) = typed_array(type.check.element, type.check.class_name, type.check.script);
		}
	}

	// As the engine takes the state back (core/variant/variant_setget.cpp:1389-1394, 1623-1628; ed1daf0).
	PETRICHOR_NOINLINE bool op_iter_state(const uint32_t *p_op, uint8_t *p_frame, const bytecode::Function &p_fn, int p_line) {
		const Array &iter = *reinterpret_cast<const Array *>(p_frame + p_op[3]);
		if (iter.size() != 1) {
			return fail(p_fn, p_line, vformat("%s() left %d elements in the Array it got, which must hold the iteration state as its one element.", p_op[1] == 0 ? "_iter_init" : "_iter_next", iter.size()));
		}
		variant_at(p_frame + p_op[2]) = iter[0];
		return true;
	}

	// As GDScript's ord() checks it (modules/gdscript/gdscript_utility_functions.cpp:128-134; ed1daf0).
	PETRICHOR_NOINLINE bool op_ord(const uint32_t *p_op, uint8_t *p_frame, const bytecode::Function &p_fn, int p_line) {
		const String &text = *reinterpret_cast<const String *>(p_frame + p_op[2]);
		if (text.length() != 1) {
			return fail(p_fn, p_line, vformat("ord() takes one character, found a String of length %d.", text.length()));
		}
		*reinterpret_cast<int64_t *>(p_frame + p_op[1]) = text.unicode_at(0);
		return true;
	}

	// The frames of this language innermost first, as GDScript's print_stack()
	// and get_stack() give its own (modules/gdscript/gdscript_utility_functions.cpp:364-387; ed1daf0).
	PETRICHOR_NOINLINE void op_stack(const uint32_t *p_op, uint8_t *p_frame) {
		const bool printed = Op(p_op[0]) == Op::PRINT_STACK;
		Array frames;
		if (!printed) {
			const TypeImage &type = image.types[p_op[1]];
			frames = typed_array(type.check.element, type.check.class_name, type.check.script);
		}
		for (size_t level = 0; level < stack.size(); level++) {
			const StackFrame &frame = stack[stack.size() - 1 - level];
			const String source = frame.image->source;
			const String function = to_godot(frame.function->name);
			if (printed) {
				UtilityFunctions::print(vformat("Frame %d - %s:%d in function '%s'", int64_t(level), source, frame.line, function));
				continue;
			}
			Dictionary info;
			info["source"] = source;
			info["function"] = function;
			info["line"] = frame.line;
			frames.push_back(info);
		}
		if (!printed) {
			*reinterpret_cast<Array *>(p_frame + p_op[2]) = frames;
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
			return within_depth(p_fn, p_line);
		}
		// A negative index counts from the end, as in GDScript.
		const uint8_t *container = p_frame + p_op[4];
		const int64_t raw = *reinterpret_cast<const int64_t *>(p_frame + p_op[5]);
		const int64_t at = raw < 0 ? raw + array_size(base.storage, container) : raw;
		switch (at < 0 ? ElementRead::Past : read_element(base, element, container, at, p_frame + p_op[3])) {
			case ElementRead::Done:
				return within_depth(p_fn, p_line);
			case ElementRead::Past:
				return fail(p_fn, p_line, vformat("Index %d is out of bounds (size %d).", raw, array_size(base.storage, container)));
			case ElementRead::Misfit:
				break;
		}
		return fail(p_fn, p_line, misfit_error(element, container, at));
	}

	// The next element, or past the end the jump out of the loop.
	PETRICHOR_NOINLINE bool op_iterate(const uint32_t *p_op, uint8_t *p_frame, size_t &r_pc, const bytecode::Function &p_fn, int p_line) {
		const TypeImage &element = image.types[p_op[2]];
		const int64_t counter = ++*reinterpret_cast<int64_t *>(p_frame + p_op[5]);
		switch (read_element(image.types[p_op[1]], element, p_frame + p_op[4], counter, p_frame + p_op[3])) {
			case ElementRead::Done:
				r_pc += 7;
				return true;
			case ElementRead::Past:
				r_pc = p_op[6];
				return true;
			case ElementRead::Misfit:
				break;
		}
		return fail(p_fn, p_line, misfit_error(element, p_frame + p_op[4], counter));
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
			return within_depth(p_fn, p_line);
		}
		// A negative index counts from the end, as in GDScript.
		const int64_t raw = *reinterpret_cast<const int64_t *>(index);
		const int64_t at = raw < 0 ? raw + array_size(base.storage, container) : raw;
		if (base.kind == bytecode::TypeKind::Array && is_scalar(base.item->storage)) {
			// The typed Array's element is a Variant of the scalar's type, which
			// owns nothing (core/variant/variant.h:268-272): the value replaces
			// its data, without the copy and the check of Array::set() (core/variant/array.cpp:517-523).
			// The element is the Array's own, its shared data copied first, and
			// null past the end (core/extension/gdextension_interface.cpp:1281-1287, core/variant/array.cpp:104-110).
			uint8_t *element = at < 0 ? nullptr : static_cast<uint8_t *>(gdextension_interface::array_operator_index(container, at));
			if (element) {
				std::memcpy(element + 8, src, 8);
				return true;
			}
		} else if (at >= 0 && at < array_size(base.storage, container)) {
			if (base.storage == ValueType::Array) {
				alignas(Variant) uint8_t value[sizeof(Variant)];
				new (value) Variant(kept(base.item, src));
				reinterpret_cast<Array *>(container)->set(at, variant_at(value));
				destroy_value(ValueType::Variant, value);
			} else {
				set_packed_element(base.storage, container, at, src);
			}
			return within_depth(p_fn, p_line);
		}
		return fail(p_fn, p_line, vformat("Index %d is out of bounds (size %d).", raw, array_size(base.storage, container)));
	}

	// EQUAL and LESS.
	PETRICHOR_NOINLINE bool op_compare(const uint32_t *p_op, uint8_t *p_frame, const bytecode::Function &p_fn, int p_line) {
		const bool less = Op(p_op[0]) == Op::LESS;
		*reinterpret_cast<uint64_t *>(p_frame + p_op[2]) = (less ? less_values : equal_values)(image.types[p_op[1]], p_frame + p_op[3], p_frame + p_op[4]) ? 1 : 0;
		return within_depth(p_fn, p_line);
	}

	PETRICHOR_NOINLINE bool op_to_variant(const uint32_t *p_op, uint8_t *p_frame, const bytecode::Function &p_fn, int p_line) {
		variant_at(p_frame + p_op[2]) = to_variant(image.types[p_op[1]], p_frame + p_op[3]);
		return within_depth(p_fn, p_line);
	}

	// A key's box hashes the value.
	PETRICHOR_NOINLINE bool op_box(const uint32_t *p_op, uint8_t *p_frame, const bytecode::Function &p_fn, int p_line) {
		variant_at(p_frame + p_op[2]) = make_box(image.types[p_op[1]], p_frame + p_op[3], p_op[4] != 0);
		return within_depth(p_fn, p_line);
	}

	PETRICHOR_NOINLINE bool op_encode(const uint32_t *p_op, uint8_t *p_frame, const bytecode::Function &p_fn, int p_line) {
		const TypeImage &type = image.types[p_op[1]];
		const Variant encoded = encode(type, p_frame + p_op[3]);
		if (type.storage == ValueType::Array) {
			*reinterpret_cast<Array *>(p_frame + p_op[2]) = encoded;
		} else {
			*reinterpret_cast<Dictionary *>(p_frame + p_op[2]) = encoded;
		}
		return within_depth(p_fn, p_line);
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
			if (!scripted_object(object_pointer(*receiver), callee)) {
				return fail_cstr(p_fn, p_line, "The receiver is not a Petrichor instance.");
			}
			if (callee.context.storage == context.storage && context.self_image) {
				callee.image = context.self_image;
			}
			callee.context.self_image = callee.image;
		}
		// Within a run every script is compiled against the others (spec §12.4);
		// only bytecode that was not built together can differ, and then the
		// arguments would not fit. Whether a method is visible is the checker's:
		// its file and subclasses call those that are not pub too (spec §4.2).
		int index = callee.image ? callee.image->find_function(name) : -1;
		const bytecode::Function *found = index >= 0 ? &callee.image->module.functions[index] : nullptr;
		if (!found || !callee.image->has_signature(name, image.call_signatures[target]) || (static_class >= 0 && !found->is_static)) {
			return fail(p_fn, p_line, vformat("%s() is not the method this script was compiled against. Build the project again.", name));
		}
		ClassImage::resolve_function(callee.image, index);
		return call_function(*callee.image, uint32_t(index), callee.context, p_op, p_frame, image.module.method_calls[target].result, p_fn, p_line) && within_depth(p_fn, p_line);
	}

	// A lambda or a method value of this language, run as a method of another
	// object is (spec §3.11); none for another Callable, or one whose function
	// does not take as many arguments, which the engine passes on. The
	// arguments and the result are checked unless the call's signature is the
	// function's, or one it overrides: a function value came in as any Callable
	// from the engine, and an untyped Callable's call() passes anything. As an
	// engine entry, which that call() is (§8.1), a program error in it gives the
	// default result and this call goes on.
	std::optional<bool> call_directly(const uint32_t *p_op, uint8_t *p_frame, const bytecode::Slot &p_result, bool p_entry, const bytecode::Function &p_fn, int p_line) {
		// A Callable of this library by its exact type: neither type has
		// subclasses, and comparing types costs less than the casts of
		// get_custom() (thirdparty/godot-cpp/src/variant/callable_custom.cpp:122-125).
		const auto *custom = static_cast<const CallableCustomBase *>(gdextension_interface::callable_custom_get_userdata(reinterpret_cast<const Callable *>(p_frame + p_op[3])->_native_ptr(), gdextension_interface::token));
		const auto *lambda = custom && typeid(*custom) == typeid(LambdaCallable) ? static_cast<const LambdaCallable *>(custom) : nullptr;
		const auto *method = custom && typeid(*custom) == typeid(MethodCallable) ? static_cast<const MethodCallable *>(custom) : nullptr;
		// For a method of self, resolving would end with the image and the
		// context this call holds already.
		const bool on_self = method && context.storage && context.self_image && !context.storage->freed.load(std::memory_order_relaxed) && uint64_t(method->get_object()) == instance_id(context.owner);
		// A lambda is used as it is: the caller's register keeps it until the
		// call returns. A method value's object is resolved into `resolved`.
		ScriptedObject resolved;
		const ClassImage *found = nullptr;
		const InstanceContext *callee_context = nullptr;
		int index = -1;
		const uint8_t *captures = nullptr;
		bool typed = false;
		if (lambda && lambda->is_valid()) {
			found = lambda->get_image().get();
			callee_context = &lambda->get_context();
			index = lambda->get_function();
			captures = lambda->get_captures();
			typed = !p_entry && found->function_signatures[index] == image.call_signatures[p_op[1]];
		} else if (on_self || (method && method->resolve(resolved, index))) {
			// As op_call_method() finds it.
			if (!on_self && resolved.context.storage == context.storage && context.self_image) {
				resolved.image = context.self_image;
			}
			if (!on_self && !resolved.context.self_image) {
				resolved.context.self_image = resolved.image;
			}
			found = on_self ? context.self_image.get() : resolved.image.get();
			callee_context = on_self ? &context : &resolved.context;
			index = found->find_function(method->get_method());
			if (index < 0) {
				return std::nullopt;
			}
			typed = !p_entry && found->has_signature(method->get_method(), image.call_signatures[p_op[1]]);
		} else {
			return std::nullopt;
		}
		const ClassImage::FunctionOrigin &origin = found->function_origins[index];
		const ClassImage &target = origin.image ? *origin.image : *found;
		index = origin.image ? origin.index : index;
		const bytecode::Function &function = target.module.functions[index];
		if (function.is_async || !function.takes(p_op[4])) {
			return std::nullopt;
		}
		const bool ok = call_function(target, uint32_t(index), *callee_context, p_op, p_frame, p_result, p_fn, p_line, captures, p_entry, typed);
		if (ok || !p_entry) {
			return ok;
		}
		if (p_op[2] != NO_SLOT) {
			from_variant(image.types[p_result.type], default_variant(target.types[function.result.type]), p_frame + p_op[2], false);
		}
		return true;
	}

	PETRICHOR_NOINLINE bool op_call_value(const uint32_t *p_op, uint8_t *p_frame, const bytecode::Function &p_fn, int p_line) {
		const bytecode::MethodCall &call = image.module.method_calls[p_op[1]];
		if (call.method == "call") {
			if (const std::optional<bool> done = call_directly(p_op, p_frame, call.result, false, p_fn, p_line)) {
				return *done && within_depth(p_fn, p_line);
			}
		}
		// Through the engine, which passes the call on, also through bind(); a
		// program error in the callee aborts this call too (spec §8.1).
		const uint32_t dst = p_op[2];
		const uint32_t argc = p_op[4];
		if (target_freed(*reinterpret_cast<const Callable *>(p_frame + p_op[3]))) {
			return fail_freed(p_fn, p_line, "The function value's object was freed.", reinterpret_cast<const Callable *>(p_frame + p_op[3])->get_object_id());
		}
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
		value = { true, false, false, !image.types[call.result.type].check.task_result.empty() };
		Variant result;
		GDExtensionCallError error;
		callable.callp(image.call_method, pointers, count, result, error);
		const ValueCall made = value;
		value = outer;
		if (made.entered && made.failed) {
			return false; // The callee reported it.
		}
		if (error.error == GDEXTENSION_CALL_ERROR_INSTANCE_IS_NULL) {
			return fail_freed(p_fn, p_line, "The function value's object was freed.", Callable(callable).get_object_id());
		}
		if (error.error == GDEXTENSION_CALL_ERROR_TOO_MANY_ARGUMENTS || error.error == GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS) {
			return fail(p_fn, p_line, vformat("%s() passes %d argument(s), but the function takes %d.", call.method.c_str(), count, error.expected));
		}
		if (error.error != GDEXTENSION_CALL_OK) {
			return fail_cstr(p_fn, p_line, "The function value cannot be called.");
		}
		String mismatch;
		if (dst != NO_SLOT && !entry_check(image.types[call.result.type], result, p_frame + dst, mismatch)) {
			return !mismatch.is_empty() && fail(p_fn, p_line, vformat("A function value returned an unexpected %s.", Variant::get_type_name(result.get_type())));
		}
		return within_depth(p_fn, p_line);
	}

	// Through the method's MethodBind, with pointers to the values, as GDScript
	// calls a method of a class it knows (modules/gdscript/gdscript_vm.cpp:2273-2313).
	PETRICHOR_NOINLINE bool op_call_native(const uint32_t *p_op, uint8_t *p_frame, const std::atomic<bool> &p_freed, const bytecode::Function &p_fn, int p_line) {
		const ClassImage::NativeCall &native = image.native_calls[p_op[1]];
		const bytecode::EngineCall &call = image.module.engine_calls[p_op[1]];
		const uint32_t dst = p_op[2];
		void *object = nullptr; // None for a static method.
		if (call.static_class.empty()) {
			const uint8_t *slot = p_op[3] == NO_SLOT ? data_of(self()) : p_frame + p_op[3];
			if (p_op[3] == NO_SLOT ? p_freed.load(std::memory_order_relaxed) : !is_live_object(slot)) {
				ObjectView view(nullptr);
				return resolve_object(p_frame, p_op[3], p_freed, view, p_fn, p_line); // Reports why.
			}
			object = *reinterpret_cast<const GDExtensionObjectPtr *>(slot + 8);
#ifdef DEBUG_ENABLED
			if (native.queues_free) {
				vm::record_free(*reinterpret_cast<const uint64_t *>(slot), true);
			}
#endif
		}
		const uint32_t argc = p_op[4];
		const void *direct[8];
		std::optional<Arguments> args;
		const void **pointers = direct;
		if (native.direct && call.params.size() <= 8) {
			for (uint32_t i = 0; i < argc; i++) {
				// The engine reads an object as its pointer (core/variant/method_ptrcall.h:251-260).
				direct[i] = p_frame + p_op[6 + 2 * i] + (call.params[i] == ValueType::Object ? 8 : 0);
			}
			std::copy(native.defaults.end() - (call.params.size() - argc), native.defaults.end(), direct + argc);
		} else {
			pointers = args.emplace(image.types, p_frame, p_op + 5, argc, &call.params, &native.defaults).pointers;
		}
		const auto ptrcall = [&](void *r_result) { gdextension_interface::object_method_bind_ptrcall(native.bind, object, pointers, r_result); };
		if (call.return_type == ValueType::Void) {
			ptrcall(nullptr);
		} else if (call.return_type == ValueType::Object) {
			void *returned = nullptr;
			ptrcall(&returned);
			alignas(16) uint8_t unread[16] = {};
			uint8_t *slot = dst == NO_SLOT ? unread : p_frame + dst;
			take_returned_object(slot, returned, call.returns_reference);
			if (slot == unread) {
				destroy_value(ValueType::Object, unread);
			}
		} else if (dst != NO_SLOT && call.return_type != ValueType::Array) {
			ptrcall(p_frame + dst);
		} else {
			// Some methods fill the Array they are given, which a slot may share
			// (core/variant/method_ptrcall.h:116-123).
			const TypeImage &type = image.types[call.result_type];
			ValueBuffer result(type);
			ptrcall(result.data);
			if (dst != NO_SLOT) {
				copy_value(type, p_frame + dst, result.data);
			}
		}
		return within_depth(p_fn, p_line);
	}

	PETRICHOR_NOINLINE bool op_call_engine(const uint32_t *p_op, uint8_t *p_frame, const std::atomic<bool> &p_freed, CoroutineFrame *p_async, size_t &r_pc, bool &r_done, const bytecode::Function &p_fn, int p_line) {
		const uint32_t target = p_op[1];
		const uint32_t dst = p_op[2];
		const uint32_t argc = p_op[4];
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
			return within_depth(p_fn, p_line);
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
#ifdef DEBUG_ENABLED
		const ClassImage::NativeCall &native = image.native_calls[target];
		if (native.frees || native.queues_free) {
			vm::record_free(object_id_of(*object), native.queues_free);
		}
#endif
		Object *state = result.get_type() == Variant::OBJECT ? result.get_validated_object() : nullptr;
		if (state && state->is_class("GDScriptFunctionState")) {
			if (Op(p_op[0]) == Op::CALL_AWAIT_ENGINE) {
				if (p_async->task->cancel_requested) {
					r_done = true;
					return within_depth(p_fn, p_line);
				}
				p_async->held_state = result;
				p_async->pc = r_pc + 5 + 2 * argc;
				if (!p_async->suspend(Signal(state, "completed"), dst, object_id_of(*object), true)) {
					return fail_cstr(p_fn, p_line, "Cannot await GDScript completion.");
				}
				r_done = true;
				return within_depth(p_fn, p_line);
			}
			if (dst != NO_SLOT) {
				return fail_cstr(p_fn, p_line, "The method is a GDScript coroutine and needs await.");
			}
		} else if (dst != NO_SLOT && !from_variant(image.types[image.module.engine_calls[target].result_type], result, p_frame + dst, false)) {
			return fail(p_fn, p_line, vformat("%s() returned an unexpected %s.", image.engine_methods[target], Variant::get_type_name(result.get_type())));
		}
		r_pc += 5 + 2 * argc;
		return within_depth(p_fn, p_line);
	}

	PETRICHOR_NOINLINE bool op_call_await_builtin(const uint32_t *p_op, uint8_t *p_frame, CoroutineFrame *p_async, size_t &r_pc, bool &r_done, const bytecode::Function &p_fn, int p_line) {
		const uint32_t dst = p_op[2];
		const uint32_t argc = p_op[4];
		const Callable &callable = *reinterpret_cast<Callable *>(p_frame + p_op[3]);
		Arguments args(image.types, p_frame, p_op + 5, argc, nullptr);
		Variant base = callable;
		Variant result;
		GDExtensionCallError error;
		const bool freed = target_freed(callable);
		if (!freed) {
			base.callp(to_godot(image.module.builtin_methods[p_op[1]].method), args.as_variants(), int(argc), result, error);
		}
		if (freed || error.error != GDEXTENSION_CALL_OK) {
			return fail_cstr(p_fn, p_line, "Calling the awaited Callable failed.");
		}
		Object *state = result.get_type() == Variant::OBJECT ? result.get_validated_object() : nullptr;
		p_async->destination = dst;
		if (state && state->is_class("GDScriptFunctionState")) {
			if (p_async->task->cancel_requested) {
				r_done = true;
				return within_depth(p_fn, p_line);
			}
			p_async->held_state = result;
			p_async->pc = r_pc + 5 + 2 * argc;
			if (!p_async->suspend(Signal(state, "completed"), dst, uint64_t(callable.get_object_id()), true)) {
				return fail_cstr(p_fn, p_line, "Cannot await GDScript completion.");
			}
			r_done = true;
			return within_depth(p_fn, p_line);
		}
		if (result.get_type() == Variant::SIGNAL) {
			if (p_async->task->cancel_requested) {
				r_done = true;
				return within_depth(p_fn, p_line);
			}
			const Signal signal = result;
			const TaskRef task = task_of(signal.get_object());
			p_async->pc = r_pc + 5 + 2 * argc;
			if (task && signal.get_name() == StringName("completed")) {
				if (!p_async->await_task(task)) {
					return false;
				}
			} else if (!p_async->suspend(signal, dst, uint64_t(signal.get_object_id()))) {
				return fail_cstr(p_fn, p_line, "Cannot await this signal.");
			}
			if (p_async->task->state == TaskState::State::Suspended) {
				r_done = true;
				return within_depth(p_fn, p_line);
			}
		} else if (!p_async->receive(result)) {
			return false;
		}
		r_pc += 5 + 2 * argc;
		return within_depth(p_fn, p_line);
	}

	// A plain built-in method (ClassImage::BuiltinCall) with its arguments in
	// their slots, a Variant made only of a scalar; others as op_call_builtin
	// calls them.
	PETRICHOR_NOINLINE bool op_call_plain(const uint32_t *p_op, uint8_t *p_frame, const bytecode::Function &p_fn, int p_line) {
		constexpr uint32_t PLAIN_ARGS = 4;
		const bytecode::BuiltinMethodCall &method = image.module.builtin_methods[p_op[1]];
		const uint32_t dst = p_op[2];
		const uint32_t argc = p_op[4];
		if (argc > PLAIN_ARGS || (dst == NO_SLOT && method.return_type != ValueType::Void)) {
			return op_call_builtin(p_op, p_frame, p_fn, p_line);
		}
		const void *args[PLAIN_ARGS];
		alignas(Variant) uint8_t variants[PLAIN_ARGS][sizeof(Variant)];
		for (uint32_t i = 0; i < argc; i++) {
			const ValueType type = image.types[p_op[5 + 2 * i]].storage;
			uint8_t *slot = p_frame + p_op[6 + 2 * i];
			if (method.params[i] != ValueType::Variant) {
				// The engine reads an object as its pointer (core/variant/method_ptrcall.h:251-260).
				args[i] = method.params[i] == ValueType::Object ? slot + 8 : slot;
			} else if (type == ValueType::Variant) {
				args[i] = slot;
			} else if (is_scalar(type)) {
				make_scalar_variant(type, slot, variants[i]);
				args[i] = variants[i];
			} else {
				return op_call_builtin(p_op, p_frame, p_fn, p_line);
			}
		}
		// A static method has no base, which the engine does not read
		// (core/variant/variant_call.cpp:470-472).
		uint8_t *base = p_op[3] == NO_SLOT ? nullptr : p_frame + p_op[3];
		if (!method.is_const && base && is_read_only(method.base, base)) {
			return fail(p_fn, p_line, read_only_error(method.base));
		}
		// A method without a result does not write it (core/variant/variant_call.cpp:184-191, 508-510).
		image.builtin_calls[p_op[1]].method(base, args, dst == NO_SLOT ? nullptr : p_frame + dst, int(argc));
		return within_depth(p_fn, p_line);
	}

	PETRICHOR_NOINLINE bool op_call_builtin(const uint32_t *p_op, uint8_t *p_frame, const bytecode::Function &p_fn, int p_line) {
		const bytecode::Module &module = image.module;
		const bool utility = Op(p_op[0]) == Op::CALL_UTILITY;
		const uint32_t target = p_op[1];
		const uint32_t dst = p_op[2];
		const uint32_t argc = p_op[4];
		if (!utility && image.builtin_calls[target].calls_callable) {
			bytecode::Slot result;
			result.type = module.builtin_methods[target].result_type;
			if (const std::optional<bool> done = call_directly(p_op, p_frame, result, true, p_fn, p_line)) {
				return *done && within_depth(p_fn, p_line);
			}
		}
		if (!utility && image.builtin_calls[target].runs_callable && target_freed(*reinterpret_cast<const Callable *>(p_frame + p_op[3]))) {
			return fail_freed(p_fn, p_line, "The function value's object was freed.", reinterpret_cast<const Callable *>(p_frame + p_op[3])->get_object_id());
		}
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
			const bytecode::ContainerMethod *container = image.builtin_calls[target].container;
			if (!method.is_const && is_read_only(method.base, base)) {
				return fail(p_fn, p_line, read_only_error(method.base));
			}
			// Through an untyped view, values must fit the types the container has (spec §3.8).
			if (method.checks_elements && container && !arguments_fit(method, *container, args.pointers, base)) {
				return fail_helper(p_fn, p_line);
			}
			if (container && container->needs_element && reinterpret_cast<const Array *>(base)->is_empty()) {
				// The engine returns null, after an error of its own for front() and back().
				return fail(p_fn, p_line, vformat("%s() of an empty array.", method.method.c_str()));
			}
			if (container && container->replaced != ClassImage::Replaced::None && keeps_boxes(to_variant(method.base, base))) {
				replaced = container->replaced;
			}
			if (container && container->key_role != ClassImage::KeyRole::None && keys_hold_containers(*reinterpret_cast<const Dictionary *>(base))) {
				keys = container->key_role;
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
		alignas(16) uint8_t scratch[64];
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
		} else if (const CallableCustom *custom = image.builtin_calls[target].calls_callable ? reinterpret_cast<const Callable *>(base)->get_custom() : nullptr) {
			// A Callable of this library: what the engine's Callable.call() runs,
			// without its dispatch, which drops the error too (core/variant/callable.cpp:44-58).
			if (custom->is_valid()) {
				GDExtensionCallError error;
				custom->call(args.as_variants(), int(argc), variant_at(scratch), error);
			}
		} else if (const std::optional<StringName> &vararg = image.builtin_calls[target].vararg) {
			// A Variant that holds the Callable or Signal without a reference:
			// these methods only read it (core/variant/variant_call.cpp:1170-1211).
			alignas(Variant) uint8_t view[sizeof(Variant)] = {};
			*reinterpret_cast<int32_t *>(view) = int32_t(module.builtin_methods[target].base);
			std::memcpy(view + 8, base, 16);
			alignas(Variant) uint8_t returned[sizeof(Variant)];
			GDExtensionCallError error;
			gdextension_interface::variant_call(view, vararg->_native_ptr(), args.pointers, argc, via_variant ? scratch : returned, &error);
			if (!via_variant) {
				from_variant(result_type, variant_at(returned), static_cast<uint8_t *>(result), false);
				destroy_value(ValueType::Variant, returned);
			}
		} else {
			image.builtin_calls[target].method(base, args.pointers, result, int(argc));
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
		return within_depth(p_fn, p_line);
	}

	PETRICHOR_NOINLINE bool op_construct(const uint32_t *p_op, uint8_t *p_frame, const bytecode::Function &p_fn, int p_line) {
		const uint32_t target = p_op[1];
		const uint32_t dst = p_op[2];
		const uint32_t argc = p_op[4];
		const bytecode::ConstructorCall &constructor = image.module.constructors[target];
		if (argc <= 8 && argc <= constructor.params.size()) {
			const void *ptrs[8];
			bool native = true;
			for (uint32_t i = 0; i < argc; i++) {
				// Arguments gives an object as its pointer, as Signal(object, name) takes it.
				if (constructor.params[i] == ValueType::Variant || constructor.params[i] == ValueType::Object) {
					native = false;
					break;
				}
				ptrs[i] = p_frame + p_op[6 + 2 * i];
			}
			if (native) {
				destroy_value(constructor.type, p_frame + dst);
				image.constructors[target](p_frame + dst, ptrs);
				return true;
			}
		}
		Arguments args(image.types, p_frame, p_op + 5, argc, &constructor.params);
		// The engine constructs into memory that holds no value.
		destroy_value(constructor.type, p_frame + dst);
		image.constructors[target](p_frame + dst, args.pointers);
		return within_depth(p_fn, p_line);
	}

	PETRICHOR_NOINLINE bool op_task_cancel(const uint32_t *p_op, uint8_t *p_frame, const bytecode::Function &p_fn, int p_line) {
		const TaskRef task = task_at(p_frame + p_op[1]);
		if (!task) {
			return fail_cstr(p_fn, p_line, "Expected a Task handle.");
		}
		task->cancel();
		return true;
	}

	// TASK_OF and TASK_DELAY (spec §9.5).
	PETRICHOR_NOINLINE bool op_signal_task(const uint32_t *p_op, uint8_t *p_frame, const bytecode::Function &p_fn, int p_line) {
		Signal signal;
		if (Op(p_op[0]) == Op::TASK_DELAY) {
			SceneTree *tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop());
			if (!tree) {
				return fail_cstr(p_fn, p_line, "Task.delay() needs the SceneTree as the main loop.");
			}
			signal = Signal(tree->create_timer(*reinterpret_cast<const double *>(p_frame + p_op[3])).ptr(), "timeout");
		} else {
			signal = *reinterpret_cast<const Signal *>(p_frame + p_op[3]);
		}
		TaskRef task = signal_task(context, signal, image.types[p_op[1]].check.task_result);
		if (!task) {
			return fail_cstr(p_fn, p_line, "Task.of() cannot connect to the signal.");
		}
		task_at(p_frame + p_op[2]) = std::move(task);
		return true;
	}

	PETRICHOR_NOINLINE bool op_task_settled(const uint32_t *p_op, uint8_t *p_frame, const bytecode::Function &p_fn, int p_line) {
		const TaskRef &member = task_at(p_frame + p_op[3]);
		if (!member) {
			return fail_cstr(p_fn, p_line, "Expected a Task handle.");
		}
		task_at(p_frame + p_op[2]) = TaskJoin::make(TaskJoin::Kind::Settled, { member }, { p_op[4] != 0 }, image.types[p_op[1]], context, image.shared_from_this(), p_fn, p_line);
		return true;
	}

	// TASK_ANY and TASK_ALL (spec §9.5).
	PETRICHOR_NOINLINE bool op_task_join(const uint32_t *p_op, uint8_t *p_frame, const bytecode::Function &p_fn, int p_line) {
		const TaskJoin::Kind kind = Op(p_op[0]) == Op::TASK_ANY ? TaskJoin::Kind::Any : TaskJoin::Kind::All;
		std::vector<TaskRef> members;
		std::vector<bool> owned;
		if (p_op[4] == 1) {
			const Array &tasks = *reinterpret_cast<const Array *>(p_frame + p_op[6]);
			if (kind == TaskJoin::Kind::Any && tasks.is_empty()) {
				return fail_cstr(p_fn, p_line, "Task.any() of an empty Array has no task to wait for.");
			}
			const TypeImage &element = *image.types[p_op[5]].item;
			for (int64_t i = 0; i < tasks.size(); i++) {
				// Another language can put any Callable in it.
				const uint8_t *member = opened(element, tasks[i]);
				if (!member) {
					return fail(p_fn, p_line, read_error(element, tasks[i]));
				}
				members.push_back(task_at(member));
			}
			owned.assign(members.size(), false);
		}
		for (uint32_t i = 0; i + 1 < p_op[4]; i += 2) {
			const TaskRef &member = task_at(p_frame + p_op[6 + 2 * i]);
			if (!member) {
				return fail_cstr(p_fn, p_line, "Expected a Task handle.");
			}
			members.push_back(member);
			owned.push_back(*reinterpret_cast<const int64_t *>(p_frame + p_op[8 + 2 * i]) != 0);
		}
		task_at(p_frame + p_op[2]) = TaskJoin::make(kind, members, owned, image.types[p_op[1]], context, image.shared_from_this(), p_fn, p_line);
		return true;
	}

	PETRICHOR_NOINLINE bool op_range(const uint32_t *p_op, uint8_t *p_frame, const bytecode::Function &p_fn, int p_line) {
		const int64_t from = *reinterpret_cast<const int64_t *>(p_frame + p_op[6]);
		const int64_t to = *reinterpret_cast<const int64_t *>(p_frame + p_op[8]);
		const int64_t step = *reinterpret_cast<const int64_t *>(p_frame + p_op[10]);
		if (step == 0) {
			return fail_cstr(p_fn, p_line, "The step of range() is 0.");
		}
		// Unsigned, so that no span overflows.
		uint64_t count = 0;
		if (step > 0 ? from < to : from > to) {
			const uint64_t span = step > 0 ? uint64_t(to) - uint64_t(from) : uint64_t(from) - uint64_t(to);
			const uint64_t by = step > 0 ? uint64_t(step) : 0 - uint64_t(step);
			count = span / by + (span % by != 0 ? 1 : 0);
		}
		const TypeImage &type = image.types[p_op[1]];
		if (type.storage == ValueType::Int) {
			// More than INT64_MAX is a loop that no program sees finish.
			*reinterpret_cast<int64_t *>(p_frame + p_op[2]) = int64_t(std::min<uint64_t>(count, INT64_MAX));
			return true;
		}
		// GDScript's limit (modules/gdscript/gdscript_utility_functions.cpp:150, 173, 212; ed1daf0).
		if (count > uint64_t(INT32_MAX)) {
			return fail(p_fn, p_line, vformat("range() would make an Array of %s ints, more than 2147483647.", String::num_uint64(count)));
		}
		PackedInt64Array values;
		values.resize(int64_t(count));
		int64_t *write = values.ptrw();
		for (uint64_t i = 0; i < count; i++) {
			write[i] = int64_t(uint64_t(from) + i * uint64_t(step));
		}
		Array &result = *reinterpret_cast<Array *>(p_frame + p_op[2]);
		result = typed_array(type.check.element, type.check.class_name, type.check.script);
		result.assign(Array(values));
		return true;
	}

	PETRICHOR_NOINLINE bool op_await_task(const uint32_t *p_op, uint8_t *p_frame, CoroutineFrame *p_async, size_t &r_pc, bool &r_done, const bytecode::Function &p_fn, int p_line) {
		const TaskRef task = task_at(p_frame + p_op[2]);
		if (!task) {
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
		if (p_async->task->state == TaskState::State::Suspended) {
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
		const Signal &signal = *reinterpret_cast<const Signal *>(p_frame + p_op[2]);
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
		return fail_self_freed(fn, record.line); \
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
				case Op::BREAKPOINT:
					if (debugging) {
						debugger::breakpoint();
					}
					pc += 1;
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
					if (const uint8_t *native = image.native_values[code[pc + 3]]) {
						copy_value(types[code[pc + 1]].storage, frame + code[pc + 2], native);
					} else {
						from_variant(types[code[pc + 1]], image.values[code[pc + 3]], frame + code[pc + 2], false);
					}
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
				case Op::GET_MEMBER_OF:
					if (!op_get_member_of(code + pc, frame, freed, fn, record.line)) {
						return false;
					}
					pc += 4;
					break;
				case Op::SET_MEMBER_OF:
					if (!op_set_member_of(code + pc, frame, freed, fn, record.line)) {
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
				case Op::ENGINE_METHOD_VALUE:
					if (!op_method_value(code + pc, frame, freed, fn, record.line)) {
						return false;
					}
					pc += 4;
					break;
				case Op::STATIC_VALUE:
					op_static_value(code + pc, frame);
					pc += 4;
					break;
				case Op::STRUCT_METHOD_VALUE:
					if (!op_struct_method_value(code + pc, frame, fn, record.line)) {
						return false;
					}
					pc += 4;
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
				case Op::CHECK_MUST:
					SELF_ALIVE();
					if (!image.assigned(members, code[pc + 1])) {
						return fail_unassigned(fn, record.line, image, code[pc + 1]);
					}
					pc += 2;
					break;
				case Op::MARK_ASSIGNED:
					SELF_ALIVE();
					image.assigned(members, code[pc + 1]) = 1;
					pc += 2;
					break;
				case Op::CHECK_PROVIDED:
					SELF_ALIVE();
					if (!check_provided(fn, members)) {
						return false;
					}
					pc += 1;
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
						return fail_freed(fn, record.line, "The object given to the engine was freed.", *reinterpret_cast<const uint64_t *>(frame + code[pc + 1]));
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
				case Op::ITER_STATE:
					if (!op_iter_state(code + pc, frame, fn, record.line)) {
						return false;
					}
					pc += 4;
					break;
				case Op::ORD:
					if (!op_ord(code + pc, frame, fn, record.line)) {
						return false;
					}
					pc += 3;
					break;
				case Op::PRINT_STACK:
				case Op::GET_STACK:
					op_stack(code + pc, frame);
					pc += Op(code[pc]) == Op::GET_STACK ? 3 : 1;
					break;
				case Op::IS_INSTANCE:
					op_is_instance(code + pc, frame);
					pc += 4;
					break;
				case Op::IS_SCRIPT:
					op_is_script(code + pc, frame);
					pc += 4;
					break;
				case Op::IS_NONE: {
					const uint8_t *src = frame + code[pc + 3];
					const TypeImage &type = types[code[pc + 1]];
					if (type.kind == bytecode::TypeKind::Option) {
						U64(code[pc + 2]) = U64(code[pc + 3]) ? 0 : 1;
					} else if (type.storage == ValueType::Object) {
						U64(code[pc + 2]) = is_live_object(src) ? 0 : 1; // A freed object reads as None in T? (spec §3.3).
					} else if (type.storage == ValueType::Task) {
						U64(code[pc + 2]) = task_at(src) ? 0 : 1;
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
				case Op::LESS:
					if (!op_compare(code + pc, frame, fn, record.line)) {
						return false;
					}
					pc += 5;
					break;
				case Op::SAME_OBJECT:
					U64(code[pc + 2]) = same_object(frame + code[pc + 3], code[pc + 1] & 1, frame + code[pc + 4], code[pc + 1] & 2) ? 1 : 0;
					pc += 5;
					break;
				case Op::TO_VARIANT:
					if (!op_to_variant(code + pc, frame, fn, record.line)) {
						return false;
					}
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
					if (!op_box(code + pc, frame, fn, record.line)) {
						return false;
					}
					pc += 5;
					break;
				case Op::ENCODE:
					if (!op_encode(code + pc, frame, fn, record.line)) {
						return false;
					}
					pc += 4;
					break;
				case Op::UNWRAP: {
					const TypeImage &type = types[code[pc + 1]];
					const uint8_t *src = frame + code[pc + 3];
					// An object's T? holds the object as T does, and a freed one reads
					// as None (spec §3.3), a task's T? as T does too; T? of another
					// type holds a tag first.
					const bool option = type.kind == bytecode::TypeKind::Option;
					const bool none = option ? U64(code[pc + 3]) == 0 : (type.storage == ValueType::Task ? !task_at(src) : !is_live_object(src));
					if (none) {
						return fail_unwrap(fn, record.line, frame, code[pc + 4]);
					}
					copy_value(option ? *type.element : type, frame + code[pc + 2], option ? src + 8 : src);
					pc += 5;
					break;
				}
				case Op::TRY_FROM:
					if (!try_from(types[code[pc + 1]], *reinterpret_cast<const Variant *>(frame + code[pc + 3]), frame + code[pc + 2])) {
						// A default's program error is reported already, a walk too deep not.
						return walked_too_deep() && fail_too_deep(fn, record.line);
					}
					pc += 4;
					break;
				case Op::UNWRAP_OK: {
					// The index of the variant, 0 for Ok, then Ok's value (module.h).
					const TypeImage &type = types[code[pc + 1]];
					const uint8_t *src = frame + code[pc + 3];
					if (U64(code[pc + 3]) != 0) {
						return fail_unwrap_err(fn, record.line, frame, type, src, code[pc + 4]);
					}
					copy_value(*type.fields[1].type, frame + code[pc + 2], src + type.fields[1].offset);
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
				case Op::ITERATE_INT:
					pc = ++I64(code[pc + 1]) < I64(code[pc + 2]) ? pc + 4 : code[pc + 3];
					break;
				case Op::ITERATE:
					if (!op_iterate(code + pc, frame, pc, fn, record.line)) {
						return false;
					}
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
				case Op::CLEAR:
					for (uint32_t i = 0; i < code[pc + 4]; i++) {
						clear_value(image.types[code[pc + 5 + 2 * i]], frame + code[pc + 6 + 2 * i]);
					}
					pc += 5 + 2 * code[pc + 4];
					break;
				case Op::TASK_ANY:
				case Op::TASK_ALL:
					if (!op_task_join(code + pc, frame, fn, record.line)) {
						return false;
					}
					pc += 5 + 2 * code[pc + 4];
					break;
				case Op::RANGE:
					if (!op_range(code + pc, frame, fn, record.line)) {
						return false;
					}
					pc += 11;
					break;
				case Op::CALL_AWAIT_ENGINE:
				case Op::CALL_ENGINE: {
					if (image.native_calls[code[pc + 1]].bind) {
						if (!op_call_native(code + pc, frame, freed, fn, record.line)) {
							return false;
						}
						pc += 5 + 2 * code[pc + 4];
						break;
					}
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
				case Op::CALL_BUILTIN: {
					const bool plain = Op(code[pc]) == Op::CALL_BUILTIN && image.builtin_calls[code[pc + 1]].plain;
					if (!(plain ? op_call_plain(code + pc, frame, fn, record.line) : op_call_builtin(code + pc, frame, fn, record.line))) {
						return false;
					}
					pc += 5 + 2 * code[pc + 4];
					break;
				}
				case Op::CONSTRUCT:
					if (!op_construct(code + pc, frame, fn, record.line)) {
						return false;
					}
					pc += 5 + 2 * code[pc + 4];
					break;
				case Op::TASK_CANCEL:
					if (!op_task_cancel(code + pc, frame, fn, record.line)) {
						return false;
					}
					pc += 2;
					break;
				case Op::TASK_OF:
				case Op::TASK_DELAY:
					if (!op_signal_task(code + pc, frame, fn, record.line)) {
						return false;
					}
					pc += 4;
					break;
				case Op::TASK_SETTLED:
					if (!op_task_settled(code + pc, frame, fn, record.line)) {
						return false;
					}
					pc += 5;
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

InstanceStorage::InstanceStorage(const std::shared_ptr<const ClassImage> &p_image, uint64_t p_owner_id) :
		image(p_image), members(static_cast<uint8_t *>(::operator new(p_image->members_layout.size))), owner_id(p_owner_id) {
	image->members_layout.construct(members);
#ifdef DEBUG_ENABLED
	if (!call_stack.empty()) {
		made = here();
	}
#endif
}

InstanceStorage::~InstanceStorage() {
	image->members_layout.destroy(members);
	::operator delete(members);
}

void CoroutineTask::_bind_methods() {
	ClassDB::bind_method(D_METHOD("cancel"), &CoroutineTask::cancel);
	ADD_SIGNAL(MethodInfo("completed", PropertyInfo(Variant::NIL, "result", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NIL_IS_VARIANT)));
	ADD_SIGNAL(MethodInfo("cancelled", PropertyInfo(Variant::INT, "reason"))); // Spec §9.8.
}

void CoroutineTask::_notification(int p_what) {
	// Before the engine frees it, while object() can still find it gone by
	// its count of references.
	if (p_what == NOTIFICATION_PREDELETE && task) {
		std::lock_guard<std::mutex> lock(task->links_mutex);
		if (task->engine_object == this) {
			task->engine_object = nullptr;
		}
	}
}

void CoroutineTask::cancel() {
	if (task) {
		task->cancel();
	}
}

TaskRef task_of(const Variant &p_value) {
	const CoroutineTask *object = type_of(p_value) == Variant::OBJECT ? Object::cast_to<CoroutineTask>(p_value.get_validated_object()) : nullptr;
	return object ? object->task : TaskRef();
}

TaskState::TaskState() = default;
TaskState::~TaskState() = default;

Ref<CoroutineTask> TaskState::object() {
	Ref<CoroutineTask> found;
	std::lock_guard<std::mutex> lock(links_mutex);
	if (engine_object) {
		found.reference_ptr(engine_object); // None once its count is 0.
	}
	if (found.is_null()) {
		found.instantiate();
		found->task = TaskRef(this);
		engine_object = found.ptr();
		if (!ended()) {
			held = found;
		}
	}
	return found;
}

uint64_t TaskState::on_end(std::function<void()> p_ended) {
	std::lock_guard<std::mutex> lock(links_mutex);
	if (ended()) {
		return 0;
	}
	waiters.emplace_back(++next_waiter, std::move(p_ended));
	return next_waiter;
}

void TaskState::forget(uint64_t p_waiter) {
	std::lock_guard<std::mutex> lock(links_mutex);
	waiters.erase(std::remove_if(waiters.begin(), waiters.end(), [&](const auto &p_entry) { return p_entry.first == p_waiter; }), waiters.end());
}

void TaskState::finish(State p_state, const Variant &p_result) {
	const TaskRef keep(this);
	TaskRef child;
	std::vector<TaskRef> owned;
	{
		std::lock_guard<std::recursive_mutex> lock(mutex);
		if (ended()) {
			return;
		}
		result = p_state == State::Finished ? p_result : Variant();
		state = p_state;
		unlist_task(this);
		if (continuation) {
			const auto frame = std::move(continuation);
			frame->disconnect();
			child = std::move(frame->child);
			if (frame->data) {
				uint8_t *data = frame->data;
				frame->data = nullptr;
				frame->image->frames[frame->function].destroy(data);
				::operator delete(data);
			}
		}
		if (const std::shared_ptr<TaskJoin> stopped = std::move(join)) {
			owned = stopped->stop();
		}
	}
	// Cancelled as its owner is freed, it leaves the tasks it owns running, as
	// GDScript does; those on the same owner are cancelled with it (spec §9.3).
	if (p_state == State::Cancelled && owner_freed) {
		child = TaskRef();
		owned.clear();
	}
	if (p_state == State::Cancelled && child) {
		child->cancel();
	}
	for (const TaskRef &member : owned) {
		member->cancel();
	}
	// The state says ended before these are taken, so no waiter comes after.
	std::vector<std::pair<uint64_t, std::function<void()>>> ended;
	Ref<CoroutineTask> seen;
	{
		std::lock_guard<std::mutex> lock(links_mutex);
		ended.swap(waiters);
		seen = held;
		held.unref();
	}
	static const StringName cancelled_name("cancelled");
	static const StringName completed_name("completed");
	if (seen.is_valid() && p_state == State::Cancelled) {
		seen->emit_signal(cancelled_name, cancel_reason());
	}
	for (const auto &waiter : ended) {
		waiter.second();
	}
	if (seen.is_valid()) {
		seen->emit_signal(completed_name, result);
	}
}

void TaskState::cancel() {
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
void TaskState::cancel_owner() {
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
	lost_object = 0;
	if (!waits) {
		return;
	}
	waits = false;
	if (waited_task) {
		// A wait for a task holds nothing of the engine.
		waited_task->forget(waiter);
		waiter = 0;
		waited_task = TaskRef();
		return;
	}
	if (connected && waiting.get_object() && waiting.is_connected(callback)) {
		waiting.disconnect(callback);
	}
	connected = false;
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

bool CoroutineFrame::await_task(const TaskRef &p_task) {
	if (p_task->state == TaskState::State::Finished) {
		child = TaskRef();
		return receive(p_task->result);
	}
	if (p_task->state == TaskState::State::Failed) {
		return false;
	}
	if (p_task->owner_freed) {
		const uint64_t id = p_task->owner_id;
		if (p_task->owner_freed_frame != task_frame.load()) {
			report_error(vformat("The awaited object %d was freed or lost its script.", int64_t(id)) + vm::freed_site(id));
			return false;
		}
		child = TaskRef();
		lose(id);
		task->state = TaskState::State::Suspended;
		return true;
	}
	if (p_task->state == TaskState::State::Cancelled) {
		report_error("The awaited task was cancelled.");
		return false;
	}
	// The callback waits for this task's lock, which the caller holds, so it
	// finds the wait set up.
	const uint64_t ticket = ++generation;
	const std::weak_ptr<CoroutineFrame> weak = shared_from_this();
	const uint64_t id = p_task->on_end([weak, ticket] {
		if (const auto frame = weak.lock()) {
			frame->complete_wait(ticket, Variant());
		}
	});
	if (!id) {
		return await_task(p_task); // Ended meanwhile on another thread.
	}
	waits = true;
	waiter = id;
	waited_task = p_task;
	task->state = TaskState::State::Suspended;
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
		String mismatch;
		if (entry_check(image->types[slot.type], p_value, data + destination, mismatch)) {
			return true;
		}
		if (!mismatch.is_empty()) {
			report_error("Await result: " + mismatch);
		}
		return false;
	}
	return false;
}

bool CoroutineFrame::suspend(const Signal &p_signal, uint32_t p_dst, uint64_t p_watch, bool p_script) {
	waits = true;
	destination = p_dst;
	waiting = p_signal;
	task->state = TaskState::State::Suspended;
	const uint64_t ticket = ++generation;
	const std::weak_ptr<CoroutineFrame> weak = shared_from_this();
	const std::function<void(const Variant &)> resume = [weak, ticket](const Variant &value) {
		if (const auto frame = weak.lock()) {
			frame->complete_wait(ticket, value);
		}
	};
	callback = Callable(memnew(ResumeCallable(resume)));
	if (!gdextension_interface::object_get_instance_from_id(uint64_t(waiting.get_object_id()))) {
		lose(p_watch);
		return true;
	}
	if (waiting.connect(callback, Object::CONNECT_ONE_SHOT) != OK) {
		return false;
	}
	connected = true;
	if (p_watch) {
		const GDExtensionObjectPtr watched = gdextension_interface::object_get_instance_from_id(p_watch);
		if (!watched) {
			lose(p_watch);
		} else {
			watch = object_watch::watch(watched, [weak, ticket](uint64_t id) {
				if (const auto frame = weak.lock(); frame && frame->generation == ticket) {
					frame->lose(id);
				}
			});
			if (p_script) {
				script_changed = Signal(ObjectDB::get_instance(ObjectID(p_watch)), "script_changed");
				const std::function<void(const Variant &)> changed = [weak, ticket, p_watch](const Variant &) {
					if (const auto frame = weak.lock(); frame && frame->generation == ticket) {
						frame->lose(p_watch);
					}
				};
				changed_callback = Callable(memnew(ResumeCallable(changed)));
				script_changed.connect(changed_callback);
			}
		}
	}
	return true;
}

void CoroutineFrame::lose(uint64_t p_object) {
	awaited_freed = true;
	lost_object = p_object;
	decide_later(task);
}

void CoroutineFrame::complete_wait(uint64_t p_generation, const Variant &p_value) {
	// The frame keeps its task.
	const auto keep_frame = shared_from_this();
	bool failed = false;
	{
		std::lock_guard<std::recursive_mutex> lock(task->mutex);
		if (p_generation != generation || task->ended() || awaited_freed) {
			return;
		}
		connected = false; // The engine has dropped the one-shot connection.
		const TaskRef completed = waited_task;
		disconnect();
		child = TaskRef();
		if (image && completed) {
			failed = !await_task(completed);
			if (!failed && awaited_freed) {
				return;
			}
		} else if (image) {
			failed = !receive(p_value);
		}
	}
	if (!image) {
		task->finish(TaskState::State::Finished, p_value); // A task of a signal.
		return;
	}
	if (failed) {
		task->finish(TaskState::State::Failed);
		return;
	}
	resume();
}

void CoroutineFrame::resume() {
	// The frame keeps its task.
	const auto keep_frame = shared_from_this();
	TaskState::State ends = TaskState::State::Running;
	Variant value;
	{
		std::lock_guard<std::recursive_mutex> lock(task->mutex);
		if (task->ended()) {
			return; // Cancelled on another thread meanwhile.
		}
		if (task->cancel_requested && pc != 0) {
			ends = TaskState::State::Cancelled;
		} else {
			task->state = TaskState::State::Running;
			const TypeImage &result_type = image->types[image->module.functions[function].result.type];
			ValueBuffer result(result_type);
			if (!let_go_of_self.empty()) {
				SlotLayout::hold_again(data, Variant(context.owner), let_go_of_self);
				let_go_of_self.clear();
			}
			const bool ok = Execution(*image, context, debugger::is_active()).run(function, data, result.data, this);
			if (task->cancel_requested) {
				ends = TaskState::State::Cancelled;
			} else if (!ok) {
				ends = TaskState::State::Failed;
			} else if (task->state == TaskState::State::Running) {
				ends = TaskState::State::Finished;
				value = outward_variant(result_type, result.data);
			} else if (context.storage && context.owner) {
				// A suspended coroutine does not keep self alive (spec §9.4). It is
				// cancelled if self goes meanwhile (§9.7), so self is there again
				// when it resumes.
				image->frames[function].let_go(data, instance_id(context.owner), let_go_of_self);
			}
		}
	}
	if (ends != TaskState::State::Running) {
		task->finish(ends, value);
	}
}

TaskRef TaskJoin::make(Kind p_kind, const std::vector<TaskRef> &p_members, const std::vector<bool> &p_owned, const TypeImage &p_result, const InstanceContext &p_context, const std::shared_ptr<const ClassImage> &p_image, const bytecode::Function &p_function, int p_line) {
	const TaskRef task = TaskRef::make();
	task->owner_id = p_context.owner ? instance_id(p_context.owner) : 0;
	task->owner_storage = p_context.storage.get();
	task->result_signature = p_result.result_key;
	const auto join = std::make_shared<TaskJoin>();
	join->kind = p_kind;
	join->task = task;
	join->context = p_context;
	join->image = p_image;
	join->result_type = &p_result;
	join->function = p_function.name;
	join->line = p_line;
	join->members = p_members;
	join->owned = p_owned;
	join->seen.assign(p_members.size(), false);
	task->join = join;
	list_task(task);
	if (p_context.storage && p_context.storage->coroutines_cancelled) {
		task->owner_freed = true;
		task->owner_freed_frame = task_frame.load();
		task->finish(TaskState::State::Cancelled);
		return task;
	}
	join->start();
	return task;
}

void TaskJoin::start() {
	{
		std::lock_guard<std::recursive_mutex> lock(task->mutex);
		task->state = TaskState::State::Suspended;
		const uint64_t ticket = ++generation;
		const std::weak_ptr<TaskJoin> weak = shared_from_this();
		for (size_t i = 0; i < members.size(); i++) {
			waiters.push_back(members[i]->on_end([weak, ticket, i] {
				if (const auto join = weak.lock(); join && join->generation == ticket) {
					join->member_ended(i);
				}
			}));
		}
	}
	// Those that ended already; Task.all() of none has none to wait for.
	for (size_t i = 0; i < members.size(); i++) {
		if (members[i]->ended()) {
			member_ended(i);
		}
	}
	if (members.empty()) {
		task->finish(TaskState::State::Finished);
	}
}

void TaskJoin::member_ended(size_t p_member) {
	const auto keep = shared_from_this();
	const TaskRef own = task;
	TaskState::State ends = TaskState::State::Finished;
	Variant value;
	{
		std::lock_guard<std::recursive_mutex> lock(own->mutex);
		const TaskRef &member = members[p_member];
		if (own->ended() || seen[p_member] || !member->ended()) {
			return;
		}
		seen[p_member] = true;
		if (member->state == TaskState::State::Failed) {
			ends = TaskState::State::Failed;
		} else if (member->state == TaskState::State::Finished) {
			if (kind == Kind::Settled) {
				value = encode_variant(*result_type, 0, result_type->enum_declared[0].second ? std::vector<Variant>{ member->result } : std::vector<Variant>());
			} else if (kind == Kind::Any) {
				value = int64_t(p_member);
			} else if (++finished < members.size()) {
				return;
			}
		} else if (member->owner_freed && member->owner_freed_frame == task_frame.load()) {
			awaited_freed = true;
			freed_member = p_member;
			decide_later(own);
			return;
		} else if (kind == Kind::Settled) {
			value = cancelled(*member.get());
		} else {
			// As awaiting it would be (spec §9.2).
			report_error(member->owner_freed ? vformat("The awaited object %d was freed or lost its script.", int64_t(member->owner_id)) + vm::freed_site(member->owner_id) : String("The awaited task was cancelled."));
			ends = TaskState::State::Failed;
		}
	}
	own->finish(ends, value);
}

void TaskJoin::report_error(const String &p_message) const {
	vm::report_error(*image, function.c_str(), line, p_message);
}

// Cancelled(reason), as Settled[T] holds it (spec §9.2).
Variant TaskJoin::cancelled(const TaskState &p_member) const {
	return encode_variant(*result_type, 1, { p_member.cancel_reason() });
}

// Lets go of the members, and gives those it owns that run on, to cancel.
std::vector<TaskRef> TaskJoin::stop() {
	++generation;
	std::vector<TaskRef> running;
	for (size_t i = 0; i < members.size(); i++) {
		if (i < waiters.size()) {
			members[i]->forget(waiters[i]);
		}
		if (owned[i] && !members[i]->ended()) {
			running.push_back(members[i]);
		}
	}
	return running;
}

namespace tasks {
void cancel_owner(const std::shared_ptr<InstanceStorage> &p_storage) {
	p_storage->coroutines_cancelled = true;
	for (const auto &task : listed_tasks(p_storage->tasks, &TaskState::among_owners)) {
		task->cancel_owner();
	}
}
void frame() {
	++task_frame;
	std::vector<TaskRef> tasks;
	{
		std::lock_guard<std::mutex> lock(tasks_mutex);
		tasks.swap(undecided);
	}
	// In the order they started, as their errors are reported.
	std::sort(tasks.begin(), tasks.end(), [](const TaskRef &p_a, const TaskRef &p_b) { return p_a->serial < p_b->serial; });
	for (const auto &task : tasks) {
		// A task locked on another thread may be running there, and waiting for
		// it could deadlock; a later frame looks again.
		std::unique_lock<std::recursive_mutex> lock(task->mutex, std::try_to_lock);
		if (!lock.owns_lock()) {
			decide_later(task);
			continue;
		}
		if (const auto join = task->join; join && join->awaited_freed && !task->ended()) {
			const bool owner_gone = join->context.storage && join->context.storage->coroutines_cancelled;
			const TaskState &member = *join->members[join->freed_member].get();
			const bool settled = join->kind == TaskJoin::Kind::Settled;
			const Variant value = owner_gone || !settled ? Variant() : join->cancelled(member);
			if (!owner_gone && !settled) {
				join->report_error(vformat("The awaited object %d was freed or lost its script.", int64_t(member.owner_id)) + vm::freed_site(member.owner_id));
			}
			lock.unlock();
			if (owner_gone) {
				task->cancel_owner();
			} else {
				task->finish(settled ? TaskState::State::Finished : TaskState::State::Failed, value);
			}
			continue;
		}
		const auto frame = task->continuation;
		if (!frame || !frame->awaited_freed || task->ended()) {
			continue;
		}
		if (frame->context.storage && frame->context.storage->coroutines_cancelled) {
			lock.unlock();
			task->cancel_owner();
		} else if (!frame->image) {
			task->source_freed = true; // A task of a signal (spec §9.2).
			lock.unlock();
			task->cancel();
		} else {
			frame->report_error(vformat("The awaited object %d was freed or lost its script.", int64_t(frame->lost_object)) + vm::freed_site(frame->lost_object));
			lock.unlock();
			task->finish(TaskState::State::Failed);
		}
	}
}
void clear() {
	for (const auto &task : listed_tasks(all_tasks, &TaskState::among_all)) {
		task->cancel();
	}
	std::vector<TaskRef> decided;
	std::lock_guard<std::mutex> lock(tasks_mutex);
	decided.swap(undecided); // Let go of after the lock, which their freeing may take.
}
} // namespace tasks

namespace vm {

Variant call(std::shared_ptr<const ClassImage> p_image, int p_function, InstanceContext p_context, const Variant **p_args, int64_t p_argc, GDExtensionCallError &r_error, bool *r_failed, const uint8_t *p_captures, bool p_task, const uint8_t *p_bound_self) {
	// Old code stays alive until this call returns, even if a hot reload
	// replaces the image meanwhile (spec §12.4).
	std::shared_ptr<const ClassImage> image = std::move(p_image);
	InstanceContext context = std::move(p_context);
	if (!context.self_image) {
		context.self_image = image;
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
	// The last parameters may be left out when they have default values, and a
	// rest parameter takes any number more.
	const size_t bound = p_bound_self ? 1 : 0;
	const size_t given = fn.params.size() - fn.captures - bound;
	const size_t fixed = fn.fixed_params() - bound;
	const size_t required = fixed - fn.defaults.size();
	if (!fn.takes(size_t(p_argc) + bound)) {
		const bool many = size_t(p_argc) > fixed;
		r_error.error = many ? GDEXTENSION_CALL_ERROR_TOO_MANY_ARGUMENTS : GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS;
		r_error.expected = int32_t(many ? fixed : required);
		return Variant();
	}

	Frame frame(frame_stack, image->frames[p_function]);
	if (fn.given != bytecode::NO_SLOT) {
		*reinterpret_cast<int64_t *>(frame.data + fn.given) = p_argc + int64_t(bound);
	}
	const TypeImage &result_type = image->types[fn.result.type];
	const uint8_t *capture = p_captures;
	for (size_t i = 0; i < fn.params.size(); i++) {
		const bytecode::Slot &param = fn.params[i];
		const TypeImage &type = image->types[param.type];
		const size_t arg = i - bound;
		if (i < bound) {
			copy_value(type, frame.data + param.offset, p_bound_self);
			continue;
		}
		if (arg >= given) {
			copy_value(type, frame.data + param.offset, capture);
			capture += type.size;
			continue;
		}
		// The function computes the default of an argument left out.
		String error;
		if (arg == fixed ? fill_rest(type, frame.data + param.offset, size_t(p_argc) > fixed ? size_t(p_argc) - fixed : 0, [&](size_t p_rest) { return *p_args[fixed + p_rest]; }, error) : arg >= size_t(p_argc) || entry_check(type, *p_args[arg], frame.data + param.offset, error)) {
			continue;
		}
		// A program error at the entry, and the engine gets the default value
		// (spec §8.1, §10.1); the engine's backtrace of the caller goes with it.
		if (!error.is_empty()) {
			report_argument(*image, fn, i, error);
		}
		if (r_failed) {
			*r_failed = true;
		}
		return default_variant(result_type);
	}

	if (fn.is_async) {
		const TaskRef task = start_task(image, uint32_t(p_function), context, frame.data);
		if (p_task) {
			return task->object();
		}
		if (r_failed) {
			*r_failed = task->state == TaskState::State::Failed;
		}
		return task->ended() ? task->result : Variant(Signal(task->object().ptr(), "completed"));
	}
	ValueBuffer result(result_type);
	const bool ok = Execution(*image, context, debugger::is_active()).run(uint32_t(p_function), frame.data, result.data);
	if (r_failed) {
		*r_failed = !ok;
	}
	if (!ok) {
		return default_variant(result_type);
	}
	return outward_variant(result_type, result.data);
}

const std::vector<StackFrame> &stack() {
	return call_stack;
}

void record_free(uint64_t p_object, bool p_queued) {
#ifdef DEBUG_ENABLED
	if (call_stack.empty()) {
		return;
	}
	// Before the hold, as a reader may hold the list while it waits for it.
	const Site site = here();
	ThreadRecords &records = own_records();
	if (const Holding holding{ records, false }) {
		records.frees[records.freed++ % ThreadRecords::FREES] = { p_object, site, p_queued };
	}
#endif
}

String freed_site(uint64_t p_object) {
#ifdef DEBUG_ENABLED
	std::lock_guard<std::mutex> guard(all_records_lock);
	for (ThreadRecords *records : all_records) {
		const Holding holding(*records, true);
		for (size_t i = 1; i <= std::min(records->freed, ThreadRecords::FREES); i++) {
			const Freed &freed = records->frees[(records->freed - i) % ThreadRecords::FREES];
			if (freed.object == p_object) {
				const Site &site = freed.site;
				return vformat(freed.queued ? " queue_free() was called on it at %s:%d in function '%s'." : " It was freed at %s:%d in function '%s'.", site.names->path, site.line, site.names->functions[site.function]);
			}
		}
	}
#endif
	return String();
}

void report_leaks(std::vector<std::pair<String, Site>> p_instances) {
#ifdef DEBUG_ENABLED
	std::map<String, int64_t> places;
	int64_t total = 0;
	const auto count = [&](const String &p_class, const Site &p_site) {
		places[vformat("%s, made at %s:%d in %s()", p_class, p_site.names->path, p_site.line, p_site.names->functions[p_site.function])]++;
		total++;
	};
	{
		std::lock_guard<std::mutex> guard(all_records_lock);
		for (const auto &[made, site] : p_instances) {
			count(made, site);
		}
		for (ThreadRecords *records : all_records) {
			const Holding holding(*records, true);
			for (const Made &made : records->made) {
				StringName name;
				const GDExtensionObjectPtr object = gdextension_interface::object_get_instance_from_id(made.object);
				if (object && gdextension_interface::object_get_class_name(object, gdextension_interface::library, name._native_ptr())) {
					count(name, made.site);
				}
			}
			records->made = {};
		}
		// The names go while the engine can still free them; nothing records
		// after this.
		for (SiteNames *names : all_site_names) {
			delete names;
		}
		all_site_names.clear();
	}
	if (places.empty()) {
		return;
	}
	std::vector<std::pair<int64_t, String>> counted;
	for (const auto &[place, made] : places) {
		counted.emplace_back(-made, place);
	}
	std::sort(counted.begin(), counted.end());
	const auto plural = [](int64_t p_count, const char *p_word) { return vformat(p_count == 1 ? "%d %s" : "%d %ss", p_count, p_word); };
	String message = plural(total, "object") + " made by this language leaked at exit, at " + plural(int64_t(counted.size()), "place") + ":";
	const int64_t width = String::num_int64(-counted[0].first).length();
	for (size_t i = 0; i < std::min<size_t>(counted.size(), 5); i++) {
		message += "\n  " + String::num_int64(-counted[i].first).lpad(width) + " " + String::chr(0xD7) + " " + counted[i].second;
	}
	if (counted.size() > 5) {
		message += "\n  and " + plural(int64_t(counted.size() - 5), "more place");
	}
	// With no place, which the engine's printing at exit then leaves out
	// (core/error/error_macros.cpp:51-61, 110-114, ed1daf0).
	_err_print_error(nullptr, nullptr, 0, message, false, true);
#endif
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
	String mismatch;
	if (!entry_check(owner->types[variable.type], p_value, owner->statics->at(variable.offset), mismatch) && !mismatch.is_empty()) {
		report_error(*owner, variable.name.c_str(), int(variable.line), vformat("\"%s\" was not written: %s.", p_name, mismatch));
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
	// A walk too deep in the chain this error ends needs no report of its
	// own, as when a value that failed to decode is described.
	walked_too_deep();
	// The debugger breaks while the stack is still intact (spec §12.5).
	debugger::error(p_message);
	const CharString description = p_message.utf8();
	const CharString file = String(p_image.source).utf8(); // An inner class's is its file's.
	godot::gdextension_interface::print_script_error(description.get_data(), p_function, file.get_data(), p_line, true);
}

bool check_parent(const ClassImage &p_image, Object *p_node) {
	const int32_t index = p_image.module.parent_class;
	const Node *node = Object::cast_to<Node>(p_node);
	Node *parent = node ? node->get_parent() : nullptr;
	if (index < 0 || (parent && p_image.fits_class(parent, uint32_t(index)))) {
		return true;
	}
	report_error(p_image, "@parent", int(p_image.module.parent_line), vformat("The parent: %s (@parent).", entry_mismatch(ValueType::Object, Variant(parent), class_check(p_image, uint32_t(index)))));
	return false;
}

} // namespace vm

} // namespace petrichor
