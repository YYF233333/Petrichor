#include "probe/task.h"

#include <godot_cpp/core/class_db.hpp>

#include <mutex>
#include <utility>
#include <vector>

using namespace godot;

namespace petrichor {

void PetTask::_bind_methods() {
	ADD_SIGNAL(MethodInfo("completed", PropertyInfo(Variant::NIL, "result", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NIL_IS_VARIANT)));
}

namespace task_queue {

namespace {

struct Pending {
	Ref<PetTask> task;
	Variant result;
	int64_t frames = 0;
};

std::mutex mutex;
std::vector<Pending> pending;

} // namespace

Signal deliver_later(const Variant &p_result, int64_t p_frames) {
	Ref<PetTask> task;
	task.instantiate();
	{
		std::lock_guard<std::mutex> lock(mutex);
		pending.push_back({ task, p_result, p_frames });
	}
	return Signal(task.ptr(), "completed");
}

void frame() {
	std::vector<Pending> due;
	{
		std::lock_guard<std::mutex> lock(mutex);
		for (size_t i = 0; i < pending.size();) {
			if (--pending[i].frames <= 0) {
				due.push_back(std::move(pending[i]));
				pending.erase(pending.begin() + i);
			} else {
				i++;
			}
		}
	}
	// Emitting resumes the waiters, which may queue new tasks.
	for (Pending &entry : due) {
		entry.task->emit_signal("completed", entry.result);
	}
}

void clear() {
	std::lock_guard<std::mutex> lock(mutex);
	pending.clear();
}

} // namespace task_queue

} // namespace petrichor
