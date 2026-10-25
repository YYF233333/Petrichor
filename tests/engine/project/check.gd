extends Node

# Runs before the parent's _ready, after the scene has assigned its properties.
func _ready() -> void:
	var greeter := get_parent()
	print("check: message = ", greeter.message)
	print("check: ", greeter.greet())
	greeter.message = "set from GDScript"
	print("check: ", greeter.greet())
	print("check: has_method(\"greet\") = ", greeter.has_method("greet"))
	if DisplayServer.get_name() == "headless":
		get_tree().quit()
