extends Node

# The step-2 probes of the architecture (§5), verifying on a running engine what
# the specification derived from source. Run after an import:
#   godot --headless --path . res://probes/probes.tscn

const Coroutines := preload("res://probes/coroutines.gd")
const ErrorLog := preload("res://error_log.gd")
const GREETER_PATH := "res://probes/typed_greeter.pet"

var probe := PetrichorProbe.new()
var failures := 0
var errors := ErrorLog.new()


func _check(condition: bool, what: String) -> void:
	if condition:
		print("probe: ok: ", what)
	else:
		failures += 1
		printerr("probe: FAILED: ", what)


# The errors reported since the last call are exactly `expected`.
func _check_errors(expected: Array[String], what: String) -> void:
	var mismatch := errors.compare(expected, "probe")
	_check(mismatch.is_empty(), what if mismatch.is_empty() else "%s: %s" % [what, mismatch])


func _ready() -> void:
	OS.add_logger(errors)
	await _instance_binding()
	_gdscript_static_view()
	await _gdscript_awaits_this_language()
	await _this_language_awaits_gdscript()
	print("probe: %d failure(s)" % failures)
	get_tree().quit(1 if failures > 0 else 0)


# Spec §9.7: the runtime learns that objects are freed through its own binding.
func _instance_binding() -> void:
	var node := Node.new()
	var node_id := node.get_instance_id()
	probe.watch(node)
	_check(probe.wrapper_class(node) == "Node", "godot-cpp's wrapper still works on a watched object")
	node.free()
	_check(probe.take_freed() == PackedInt64Array([node_id]), "free() reaches the runtime's binding")

	var ref_id := _watch_new_ref_counted()
	_check(probe.take_freed() == PackedInt64Array([ref_id]), "releasing the last reference reaches it")

	# Queued in a process frame, the node is deleted at its end, before the next
	# process_frame (scene/main/scene_tree.cpp:713, 735). Queued in _ready(),
	# before the first frame, it is deleted by then only if a physics step, which
	# deletes too (:666), comes first, as it does not at 1000 frames per second.
	await get_tree().process_frame
	var queued := Node.new()
	add_child(queued)
	var queued_id := queued.get_instance_id()
	probe.watch(queued)
	probe.watch(queued)
	queued.queue_free()
	_check(probe.take_freed().is_empty(), "a node queued for deletion is not freed before the end of the frame")
	await get_tree().process_frame
	_check(probe.take_freed() == PackedInt64Array([queued_id, queued_id]), "it is freed at the end of the frame, and every listener hears it")

	var extension_id := _watch_new_ref_counted(true)
	_check(probe.take_freed() == PackedInt64Array([extension_id]), "objects of this extension can be watched as well")


# Locals and call temporaries are released when the function returns; GDScript
# may keep a temporary alive until then.
func _watch_new_ref_counted(from_extension := false) -> int:
	var object: RefCounted = PetrichorProbe.new() if from_extension else RefCounted.new()
	probe.watch(object)
	return object.get_instance_id()


# Spec §10.1, §11.3: GDScript checks the members and `new` of this language's classes.
func _gdscript_static_view() -> void:
	var classes := ProjectSettings.get_global_class_list().filter(func(c): return c["class"] == &"TypedGreeter")
	_check(classes.size() == 1 and classes[0]["language"] == &"Petrichor", "class_name registers a global class")

	_check(_compiles("""
var g: TypedGreeter = TypedGreeter.new()
var s: String = g.greet()
var m: String = g.message
g.message = "x"
var r = await g.echo_later(1, 1)
g.free()"""), "a typed use of the class compiles")
	_check(not _compiles("var i: int = TypedGreeter.new().greet()"), "a method's return type is checked")
	_check_errors(["Parse Error: Cannot assign a value of type String to variable \"i\" with specified type int."], "GDScript reports the return type")
	_check(not _compiles("TypedGreeter.new().message = 1"), "a property's type is checked")
	_check_errors(["Parse Error: Cannot assign a value of type \"int\" as \"String\".", "Parse Error: Value of type \"int\" cannot be assigned to a variable of type \"String\"."], "GDScript reports the property's type")
	# GDScript treats an unknown method on any object type as an unsafe access
	# (a warning), not an error (modules/gdscript/gdscript_analyzer.cpp:3750-3757).
	_check(_compiles("TypedGreeter.new().nope()") == _compiles("Node.new().nope()"), "an unknown method is treated as on a native class")
	_check(not _compiles("TypedGreeter.new().echo_later(1)"), "a method's argument count is checked")
	_check_errors(["Parse Error: Too few arguments for \"echo_later()\" call. Expected at least 2 but received 1."], "GDScript reports the argument count")
	_check(not _compiles("TypedGreeter.new(1)"), "new() checks its arguments")
	_check_errors(["Parse Error: Too many arguments for \"new()\" call. Expected at most 0 but received 1."], "GDScript reports the arguments of new()")
	_check(not _compiles("var n: Node2D = TypedGreeter.new()"), "new() has the type of the class")
	_check_errors(["Parse Error: Cannot assign a value of type res://probes/typed_greeter.pet to variable \"n\" with specified type Node2D."], "GDScript reports the type of new()")
	# With redundant_await raised to an error in this project, awaiting a call
	# typed String fails, and awaiting echo_later (untyped result) does not.
	_check(not _compiles("var r = await TypedGreeter.new().greet()"), "awaiting a typed result is a redundant await")
	_check_errors(["Parse Error: \"await\" keyword is unnecessary because the expression isn't a coroutine nor a signal. (Warning treated as error.)"], "GDScript reports the redundant await")

	var greeter: Node = load(GREETER_PATH).new()
	_check(greeter.greet() == "Hello, hello", "new() constructs an instance at run time")
	greeter.free()


func _compiles(body: String) -> bool:
	var script := GDScript.new()
	script.source_code = "extends Node\n\nfunc f() -> void:\n\t" + body.strip_edges().replace("\n", "\n\t") + "\n"
	var error := script.reload()
	return error == OK


# Spec §9.8: GDScript awaits a coroutine of this language.
func _gdscript_awaits_this_language() -> void:
	var greeter: Node = load(GREETER_PATH).new()
	add_child(greeter)
	var result = await greeter.echo_later("value", 2)
	_check(result == "value", "GDScript gets the result of an async method of this language")
	result = await greeter.echo_later(null, 1)
	_check(result == null, "a cancelled task gives GDScript null instead of leaving it suspended")

	var coroutines := Coroutines.new()
	add_child(coroutines)
	var reached: bool = await coroutines.assign_cancelled(greeter)
	_check(not reached, "null assigned to a Transform3D variable stops that function instead of crashing")
	_check_errors(["coroutines.gd:11: Trying to assign value of type 'Nil' to a variable of type 'Transform3D'."], "GDScript reports the assignment")
	coroutines.free()
	greeter.free()


# Spec §9.8: this language awaits a GDScript coroutine.
func _this_language_awaits_gdscript() -> void:
	var coroutines := Coroutines.new()
	add_child(coroutines)
	var held := probe.await_call(coroutines, "twice", [7], true)
	var unheld := probe.await_call(coroutines, "twice", [8], false)
	for i in 4:
		await get_tree().process_frame
	var outcome := probe.take_await(held)
	_check(outcome.done and outcome.result == 7, "holding the first state gets the result after the coroutine suspends again")
	outcome = probe.take_await(unheld)
	_check(not outcome.done, "without holding the first state, the result never arrives")

	var doomed := probe.await_call(coroutines, "twice", [9], true)
	await get_tree().process_frame
	coroutines.free()
	for i in 3:
		await get_tree().process_frame
	outcome = probe.take_await(doomed)
	_check(not outcome.done and outcome.receiver_freed, "when the receiver is freed, the coroutine never completes and the runtime's binding reports it")
