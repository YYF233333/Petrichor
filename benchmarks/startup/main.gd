extends Node

func _ready() -> void:
	var extension: String = ProjectSettings.get_setting("benchmark/extension")
	var count: int = ProjectSettings.get_setting("benchmark/scripts")
	var total := 0
	for i in count:
		var instance: RefCounted = load("res://scripts/unit_%d.%s" % [i, extension]).new()
		total += int(instance.value())
	print("startup: %d %d" % [count, total])
	get_tree().quit()
