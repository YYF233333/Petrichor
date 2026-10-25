extends Node

# Run under the local debugger with a breakpoint in breakpoint.pet (run.py):
#   godot --headless -d --breakpoints res://language/breakpoint.pet:8 --path . res://language/breakpoint.tscn


func _ready() -> void:
	var target: Node = load("res://language/breakpoint.pet").new()
	add_child(target)
	print("breakpoint: step returned ", target.step(20))
	# A program error breaks before the stack unwinds.
	Engine.print_error_messages = false
	var quotient: int = target.divide(1, 0)
	Engine.print_error_messages = true
	print("breakpoint: divide returned ", quotient)
	get_tree().quit()
