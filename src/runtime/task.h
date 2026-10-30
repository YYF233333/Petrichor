#pragma once

#include "runtime/vm.h"

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>

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
	State state = State::Running;
	bool cancel_requested = false;
	bool owner_freed = false;
	uint64_t owner_id = 0;
	uint64_t owner_freed_frame = 0;
	godot::Variant result;
	std::string result_signature;
	std::shared_ptr<CoroutineFrame> continuation;

	void cancel();
	void detach();
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
