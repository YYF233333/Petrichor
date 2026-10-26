extends Node

# Runs caller.pet's call to callee.pet (spec §8.1), as built, and after run.py
# has put another version of callee.pet in place without building caller.pet
# again: the call is then a program error, not a call with the wrong layout.


func _ready() -> void:
	var caller: Node = preload("res://language/stale/stale.tscn").instantiate()
	add_child(caller)
	print("stale: ", caller.run())
	caller.queue_free()
	get_tree().quit()
