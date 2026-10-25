#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/signal.hpp>

namespace petrichor {

// A stand-in for the runtime's tasks until the virtual machine exists: a result
// delivered after some frames. GDScript sees it as the signal `completed`, which
// carries the result, or null when the task is cancelled (spec §9.8).
class PetTask : public godot::RefCounted {
	GDCLASS(PetTask, godot::RefCounted);

protected:
	static void _bind_methods();
};

namespace task_queue {

godot::Signal deliver_later(const godot::Variant &p_result, int64_t p_frames);

// Called from the script language's per-frame callback.
void frame();
void clear();

} // namespace task_queue

} // namespace petrichor
