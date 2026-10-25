extends Node

# Hot reload (spec §12.4), with run.py standing in for the editor: when this
# scene asks for a version, run.py puts that version of live.pet and its
# bytecode in place and sends reload_scripts over the debugger connection, as
# the editor does when a script is saved.

const LIVE := "res://language/hot_reload/live.pet"
# Headless frames are not paced, so the wait is measured in time.
const MAX_WAIT_MSEC := 30000

var failures := 0
var live: Script
var node: Node
var waiting_for := 0
var asked_at := 0


func _check(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		print("hot reload: FAILED ", what)


func _ready() -> void:
	live = load(LIVE)
	node = live.new()
	add_child(node)
	node.count = 5
	_check(node.value() == 1, "version 1 runs")
	_ask_for(2)


func _ask_for(version: int) -> void:
	waiting_for = version
	asked_at = Time.get_ticks_msec()
	print("hot reload: waiting for version ", version)


func _process(_delta: float) -> void:
	if waiting_for == 2 and node.value() != 1:
		_check(node.value() == 50, "after a body change, the instance runs the new code on its members")
		_ask_for(3)
	elif waiting_for == 3 and live.get_script_property_list().any(func(property): return property.name == "extra"):
		_check(node.value() == 50, "after a member change, the instance keeps its code")
		var fresh: Node = live.new()
		_check(fresh.value() == 3, "after a member change, a new instance runs the new code")
		fresh.free()
		_finish()
	elif Time.get_ticks_msec() - asked_at > MAX_WAIT_MSEC:
		_check(false, "version %d arrives within %d ms" % [waiting_for, MAX_WAIT_MSEC])
		_finish()


func _finish() -> void:
	print("hot reload: %d failure(s)" % failures)
	set_process(false)
	get_tree().quit()
