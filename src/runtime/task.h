#pragma once

#include "runtime/vm.h"

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace petrichor {
struct CoroutineFrame;
struct TaskJoin;
class CoroutineTask;
class TaskRef;

// A task of this language (spec §9), which Task values refer to. The engine
// sees it as a CoroutineTask, made only once the task crosses into the engine,
// so that calling a coroutine makes no engine object (architecture §2.2).
struct TaskState {
	enum class State { Running,
		Suspended,
		Finished,
		Cancelled,
		Failed };
	std::atomic<uint32_t> references{ 0 };
	// A coroutine resumes on the thread that emits what it awaits, and may be
	// cancelled on another (spec §12.3): changes of state, and the suspended
	// frame, are under this lock, which a running coroutine holds; a task ends
	// under it, and tells others it ended, cancelling its child, after it.
	std::recursive_mutex mutex;
	std::atomic<State> state{ State::Running };
	std::atomic<bool> cancel_requested{ false };
	std::atomic<bool> owner_freed{ false };
	std::atomic<bool> source_freed{ false }; // A task of a signal, whose object was freed (spec §9.2).
	uint64_t owner_id = 0;
	// Set before the task is listed and never changed, so that freeing an
	// object finds its tasks without waiting for the locks of others.
	const InstanceStorage *owner_storage = nullptr;
	std::atomic<uint64_t> owner_freed_frame{ 0 };
	godot::Variant result; // Written before the state says Finished.
	std::string result_signature;
	std::shared_ptr<CoroutineFrame> continuation;
	std::shared_ptr<TaskJoin> join; // Instead of a continuation, for a task that waits for others.
	uint64_t serial = 0; // Lists the active tasks in the order they started.

	TaskState();
	~TaskState();
	void cancel();
	void cancel_owner();
	bool ended() const { return state == State::Finished || state == State::Cancelled || state == State::Failed; }
	// Why it was cancelled, as CancelReason numbers it (spec §9.2).
	int64_t cancel_reason() const { return owner_freed ? 1 : (source_freed ? 2 : 0); }
	void finish(State p_state, const godot::Variant &p_result = godot::Variant());
	// Calls `p_ended` once, on the thread that ends the task, before the engine
	// hears of it; 0, with no call, when the task has ended already.
	uint64_t on_end(std::function<void()> p_ended);
	void forget(uint64_t p_waiter);
	// The CoroutineTask the engine sees, one at a time; the task keeps it until
	// it ends, for those connected to its signals (spec §9.8).
	godot::Ref<CoroutineTask> object();

private:
	friend class CoroutineTask;
	// Taken last, with nothing else taken under it.
	std::mutex links_mutex;
	uint64_t next_waiter = 0;
	std::vector<std::pair<uint64_t, std::function<void()>>> waiters;
	CoroutineTask *engine_object = nullptr; // Cleared as the engine frees it.
	godot::Ref<CoroutineTask> held;
};

// A counted reference to a task. A Task slot holds one, all zero bytes for None.
class TaskRef {
	TaskState *state = nullptr;

public:
	TaskRef() = default;
	explicit TaskRef(TaskState *p_state) :
			state(p_state) {
		if (state) {
			state->references.fetch_add(1, std::memory_order_relaxed);
		}
	}
	TaskRef(const TaskRef &p_other) :
			TaskRef(p_other.state) {}
	TaskRef(TaskRef &&p_other) noexcept :
			state(std::exchange(p_other.state, nullptr)) {}
	TaskRef &operator=(TaskRef p_other) noexcept {
		std::swap(state, p_other.state);
		return *this;
	}
	~TaskRef() {
		if (state && state->references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
			delete state;
		}
	}
	static TaskRef make() { return TaskRef(new TaskState()); }
	TaskState *get() const { return state; }
	TaskState *operator->() const { return state; }
	explicit operator bool() const { return state != nullptr; }
};
static_assert(sizeof(TaskRef) == 8, "a Task slot is 8 bytes");

inline TaskRef &task_at(uint8_t *p_slot) {
	return *reinterpret_cast<TaskRef *>(p_slot);
}
inline const TaskRef &task_at(const uint8_t *p_slot) {
	return *reinterpret_cast<const TaskRef *>(p_slot);
}

// A task as the engine and GDScript see it (spec §9.8).
class CoroutineTask : public godot::RefCounted {
	GDCLASS(CoroutineTask, godot::RefCounted);

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	TaskRef task; // None in one made by CoroutineTask.new(), which is no task.

	void cancel();
};

// The task of a CoroutineTask in the Variant, or None.
TaskRef task_of(const godot::Variant &p_value);

namespace tasks {
void cancel_owner(const std::shared_ptr<InstanceStorage> &p_storage);
void frame();
void clear();
} //namespace tasks
} // namespace petrichor
