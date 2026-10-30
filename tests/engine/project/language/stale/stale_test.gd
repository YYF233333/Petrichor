extends Node

# Runs caller.pet's call to callee.pet (spec §8.1), as built, and after run.py
# has put another version of callee.pet in place without building caller.pet
# again: the call is then a program error, not a call with the wrong layout.
# inherit_derived.pet, not built again after its base changed, is refused.


func _ready() -> void:
	var caller: Node = preload("res://language/stale/stale.tscn").instantiate()
	add_child(caller)
	print("stale: ", caller.run())
	caller.queue_free()
	var derived: Script = load("res://language/stale/inherit_derived.pet")
	if derived.can_instantiate():
		var node := Node.new()
		node.set_script(derived)
		print("stale inheritance: valid: ", node.run())
		node.free()
	else:
		print("stale inheritance: rejected")
	get_tree().quit()
