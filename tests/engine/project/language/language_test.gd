extends Node

# Runs compiled code of the first language subset (architecture §5 step 3):
#   godot --headless --path . res://language/language.tscn

var failures := 0


func _check(condition: bool, what: String) -> void:
	if condition:
		print("language: ok: ", what)
	else:
		failures += 1
		printerr("language: FAILED: ", what)


func _ready() -> void:
	var c: LanguageCheck = LanguageCheck.new()
	add_child(c)
	_check(c.add(2, 3) == 5, "int arithmetic")
	_check(c.divide(7, 2) == 3, "int division truncates (spec §5.1)")
	_check(c.scale(3) == 6.0, "an int argument is accepted for a float parameter")
	_check(c.sum_odd_to(6) == 9, "while, if and continue")
	_check(c.describe(-1) == "negative" and c.describe(0) == "zero" and c.describe(4) == "positive", "elif chains and early returns")
	_check(c.fib(15) == 610, "recursive calls")
	_check(c.truncate(2.7) == 2 and c.truncate(-2.7) == -2, "int() truncates toward zero")
	_check(c.wrap() < 0, "int arithmetic wraps around")
	_check(c.both(true, false) and not c.both(true, true), "and, or and not")
	var chosen: Array[int] = []
	for args in [[true, false], [false, true], [false, false]]:
		c.choose(args[0], args[1])
		chosen.append(c.chosen)
	_check(chosen == [1, 2, 3], "a void function that ends with if/elif/else returns from each branch")
	_check(c.children() == 0, "engine methods called on self")
	c.add_child(Node.new())
	_check(c.children() == 1, "engine calls see the current state")
	c.speed = 0.5
	_check(c.scale(3) == 1.5, "a member set from GDScript")

	Engine.print_error_messages = false
	var zero := c.divide(1, 0)
	Engine.print_error_messages = true
	_check(zero == 0, "after a program error the caller gets the default value (spec §8.1)")

	var frames_before: int = c.frames
	await get_tree().process_frame
	await get_tree().process_frame
	_check(c.frames > frames_before, "_process runs every frame")

	print("language: %d failure(s)" % failures)
	get_tree().quit(1 if failures > 0 else 0)
