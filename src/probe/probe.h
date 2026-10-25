#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <mutex>
#include <vector>

namespace petrichor {

// Lets the engine tests drive the step-2 probes (architecture §5) from GDScript.
// Not part of release builds.
class PetrichorProbe : public godot::RefCounted {
	GDCLASS(PetrichorProbe, godot::RefCounted);

	struct Await {
		godot::Ref<godot::RefCounted> state;
		godot::Variant result;
		bool done = false;
		bool receiver_freed = false;
	};

	std::mutex mutex;
	std::vector<uint64_t> freed;
	godot::HashMap<int64_t, Await> awaits;
	int64_t next_ticket = 0;

	void on_freed(uint64_t p_object_id);
	void on_completed(const godot::Variant &p_result, int64_t p_ticket);

protected:
	static void _bind_methods();

public:
	// The runtime's own instance binding (spec §9.7).
	void watch(godot::Object *p_object);
	godot::PackedInt64Array take_freed();
	godot::String wrapper_class(godot::Object *p_object) const;

	// Awaiting a GDScript coroutine from this language (spec §9.8). Returns a
	// ticket; the outcome is in take_await(ticket). With p_hold false, the first
	// GDScriptFunctionState is not kept alive.
	int64_t await_call(godot::Object *p_target, const godot::StringName &p_method, const godot::Array &p_args, bool p_hold);
	godot::Dictionary take_await(int64_t p_ticket);
};

} // namespace petrichor
