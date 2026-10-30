extends Node

func _ready() -> void:
	var script: Script = load("res://language/stale/inherit_derived.pet")
	if script.can_instantiate():
		var node := Node.new()
		node.set_script(script)
		print("stale inheritance: valid: ", node.run())
		node.free()
	else:
		print("stale inheritance: rejected")
	get_tree().quit()
