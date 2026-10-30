extends Node

# Hot reload (spec §12.4), with run.py standing in for the editor: when this
# scene asks for a version, run.py puts that version of live.pet and its
# bytecode in place and sends reload_scripts over the debugger connection, as
# the editor does when a script is saved, then a message of its own. The game
# reloads at the end of the poll that reads reload_scripts
# (core/debugger/remote_debugger.cpp:663-719), so once the message has arrived
# the reload has been handled, whether it was applied or not.

const LIVE := "res://language/hot_reload/live.pet"
# Headless frames are not paced, so the wait is measured in time.
const MAX_WAIT_MSEC := 30000

var inherited := OS.get_cmdline_user_args().has("--inheritance")
var bonus: int = 1 if inherited else 0
var coroutine_results: Array = []

func collect(value) -> void:
	coroutine_results.append(await value)

var failures := 0
var live: Script
var node: Node # Created under version 1.
var fresh: Node # Created after version 3 was refused.
var waiting_for := 0
var asked_at := 0
var reloaded := false


func _check(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		print("hot reload: FAILED ", what)


func _ready() -> void:
	EngineDebugger.register_message_capture("hot_reload_test", _on_message)
	live = load("res://language/hot_reload/derived.pet" if inherited else LIVE)
	node = live.new()
	add_child(node)
	node.count = 5
	_check(node.value() == 1 + bonus, "version 1 runs")
	collect(node.delayed())
	_ask_for(2)


func _on_message(message: String, _data: Array) -> bool:
	reloaded = message == "reloaded"
	return true


func _ask_for(version: int) -> void:
	waiting_for = version
	asked_at = Time.get_ticks_msec()
	reloaded = false
	print("hot reload: waiting for version ", version)


func _process(_delta: float) -> void:
	if reloaded and waiting_for == 2:
		_check(node.value() == 50 + bonus, "after a body change, the instance runs the new code on its members")
		node.tick.emit()
		_check(coroutine_results == [101 + bonus], "suspended coroutine keeps its old image")
		collect(node.delayed())
		node.tick.emit()
		_check(coroutine_results == [101 + bonus, 250 + bonus], "new coroutine runs the new image")
		_ask_for(3)
	elif reloaded and waiting_for == 3:
		_check(node.value() == 50 + bonus, "after a member change, the instance keeps its code")
		_check(not live.get_script_property_list().any(func(property): return property.name == "extra"), "after a member change, the script keeps its members")
		fresh = live.new()
		add_child(fresh)
		fresh.count = 2
		_check(fresh.value() == 20 + bonus, "after a member change, a new instance runs the code from before it")
		_ask_for(4)
	elif reloaded and waiting_for == 4:
		_check(node.value() == 50 + bonus and fresh.value() == 20 + bonus, "after a refused change, a later body change is not applied either")
		node.free()
		fresh.free()
		_finish()
	elif Time.get_ticks_msec() - asked_at > MAX_WAIT_MSEC:
		_check(false, "version %d arrives within %d ms" % [waiting_for, MAX_WAIT_MSEC])
		_finish()


func _finish() -> void:
	EngineDebugger.unregister_message_capture("hot_reload_test")
	print("hot reload: %d failure(s)" % failures)
	set_process(false)
	get_tree().quit()
