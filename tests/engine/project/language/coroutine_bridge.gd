class_name CoroutineBridge
extends Node
signal tick(value: int)

func twice() -> int:
	var a: int = await tick
	var b: int = await tick
	return a + b

var task

func get_tick() -> Signal:
	return tick

func get_completion() -> Signal:
	return task.completed

func emit_wrong(s: Signal) -> void:
	s.emit("wrong")
