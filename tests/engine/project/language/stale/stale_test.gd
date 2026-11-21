extends Node

# Runs caller.pet's call to callee.pet (spec §8.1), as built, and after run.py
# has put another version of callee.pet in place without building caller.pet
# again: the call and the reads of members whose types changed are then
# program errors, not accesses with the wrong type.
# inherit_derived.pet and layout_derived.pet, not built again after their
# bases changed, are refused.


func _ready() -> void:
	var caller: Node = preload("res://language/stale/stale.tscn").instantiate()
	add_child(caller)
	print("stale: ", caller.run())
	print("stale member: ", caller.read_member(), " static: ", caller.read_static())
	caller.queue_free()
	_derived("inheritance", "res://language/stale/inherit_derived.pet")
	_derived("layout", "res://language/stale/layout_derived.pet")
	get_tree().quit()


func _derived(label: String, path: String) -> void:
	var derived: Script = load(path)
	if derived.can_instantiate():
		var node := Node.new()
		node.set_script(derived)
		print("stale %s: valid: " % label, node.run())
		node.free()
	else:
		print("stale %s: rejected" % label)
