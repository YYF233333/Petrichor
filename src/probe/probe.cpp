#include "probe/probe.h"

#include "runtime/object_watch.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object.hpp>

using namespace godot;

namespace petrichor {

void PetrichorProbe::_bind_methods() {
	ClassDB::bind_method(D_METHOD("watch", "object"), &PetrichorProbe::watch);
	ClassDB::bind_method(D_METHOD("take_freed"), &PetrichorProbe::take_freed);
	ClassDB::bind_method(D_METHOD("wrapper_class", "object"), &PetrichorProbe::wrapper_class);
	ClassDB::bind_method(D_METHOD("await_call", "target", "method", "args", "hold"), &PetrichorProbe::await_call);
	ClassDB::bind_method(D_METHOD("take_await", "ticket"), &PetrichorProbe::take_await);
}

// Callbacks find the probe by ID, since it may be freed before the objects it watches.
static PetrichorProbe *find_probe(uint64_t p_probe_id) {
	return Object::cast_to<PetrichorProbe>(ObjectDB::get_instance(p_probe_id));
}

void PetrichorProbe::on_freed(uint64_t p_object_id) {
	std::lock_guard<std::mutex> lock(mutex);
	freed.push_back(p_object_id);
}

void PetrichorProbe::watch(Object *p_object) {
	ERR_FAIL_NULL(p_object);
	const uint64_t probe_id = get_instance_id();
	object_watch::watch(p_object->_owner, [probe_id](uint64_t p_object_id) {
		if (PetrichorProbe *probe = find_probe(probe_id)) {
			probe->on_freed(p_object_id);
		}
	});
}

PackedInt64Array PetrichorProbe::take_freed() {
	std::lock_guard<std::mutex> lock(mutex);
	PackedInt64Array result;
	for (uint64_t id : freed) {
		result.push_back(int64_t(id));
	}
	freed.clear();
	return result;
}

String PetrichorProbe::wrapper_class(Object *p_object) const {
	ERR_FAIL_NULL_V(p_object, String());
	return p_object->get_class();
}

void PetrichorProbe::on_completed(const Variant &p_result, int64_t p_ticket) {
	std::lock_guard<std::mutex> lock(mutex);
	Await *entry = awaits.getptr(p_ticket);
	if (entry) {
		// The state is released in take_await, not while it emits.
		entry->result = p_result;
		entry->done = true;
	}
}

int64_t PetrichorProbe::await_call(Object *p_target, const StringName &p_method, const Array &p_args, bool p_hold) {
	ERR_FAIL_NULL_V(p_target, -1);
	const int64_t ticket = next_ticket++;
	Await entry;

	const Variant result = p_target->callv(p_method, p_args);
	Object *state = result.get_type() == Variant::OBJECT ? static_cast<Object *>(result) : nullptr;
	if (state && state->is_class("GDScriptFunctionState")) {
		state->connect("completed", callable_mp(this, &PetrichorProbe::on_completed).bind(ticket));
		if (p_hold) {
			entry.state = Ref<RefCounted>(Object::cast_to<RefCounted>(state));
		}
		// If the receiver is freed, GDScript drops the coroutine without completing it.
		const uint64_t probe_id = get_instance_id();
		object_watch::watch(p_target->_owner, [probe_id, ticket](uint64_t) {
			if (PetrichorProbe *probe = find_probe(probe_id)) {
				std::lock_guard<std::mutex> lock(probe->mutex);
				if (Await *pending = probe->awaits.getptr(ticket)) {
					pending->receiver_freed = true;
				}
			}
		});
	} else {
		entry.result = result;
		entry.done = true;
	}

	std::lock_guard<std::mutex> lock(mutex);
	awaits.insert(ticket, entry);
	return ticket;
}

Dictionary PetrichorProbe::take_await(int64_t p_ticket) {
	Await entry;
	{
		std::lock_guard<std::mutex> lock(mutex);
		Await *found = awaits.getptr(p_ticket);
		ERR_FAIL_NULL_V(found, Dictionary());
		entry = *found;
		if (entry.done || entry.receiver_freed) {
			awaits.erase(p_ticket);
		}
	}
	Dictionary result;
	result["done"] = entry.done;
	result["result"] = entry.result;
	result["receiver_freed"] = entry.receiver_freed;
	return result;
}

} // namespace petrichor
