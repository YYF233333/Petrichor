#pragma once

#include "runtime/vm.h"

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>

#include <atomic>
#include <mutex>

namespace petrichor {
struct CoroutineFrame;
struct TaskJoin;

class CoroutineTask : public godot::RefCounted {
	GDCLASS(CoroutineTask, godot::RefCounted);

protected:
	static void _bind_methods();

public:
	enum class State { Running,
		Suspended,
		Finished,
		Cancelled,
		Failed };
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

	void cancel();
	void cancel_owner();
	bool ended() const { return state == State::Finished || state == State::Cancelled || state == State::Failed; }
	// Why it was cancelled, as CancelReason numbers it (spec §9.2).
	int64_t cancel_reason() const { return owner_freed ? 1 : (source_freed ? 2 : 0); }
	void finish(State p_state, const godot::Variant &p_result = godot::Variant());
};

namespace tasks {
void cancel_owner(const std::shared_ptr<InstanceStorage> &p_storage);
void frame();
void clear();
} //namespace tasks
} // namespace petrichor
