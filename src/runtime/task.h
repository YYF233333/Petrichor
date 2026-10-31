#pragma once

#include "runtime/vm.h"

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>

#include <atomic>
#include <mutex>

namespace petrichor {
struct CoroutineFrame;

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
	uint64_t owner_id = 0;
	std::atomic<uint64_t> owner_freed_frame{ 0 };
	godot::Variant result; // Written before the state says Finished.
	std::string result_signature;
	std::shared_ptr<CoroutineFrame> continuation;

	void cancel();
	void cancel_owner();
	bool ended() const { return state == State::Finished || state == State::Cancelled || state == State::Failed; }
	void finish(State p_state, const godot::Variant &p_result = godot::Variant());
};

namespace tasks {
void cancel_owner(const std::shared_ptr<InstanceStorage> &p_storage);
void frame();
void clear();
} //namespace tasks
} // namespace petrichor
