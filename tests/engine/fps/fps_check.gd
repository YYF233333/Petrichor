extends Node

# An autoload that run.py adds to the FPS starter kit (architecture §5 step 4):
# it lets the main scene run for a while, checks what the migrated scripts did,
# and quits.

const FRAMES := 120

var failures := 0


func _check(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		print("fps check: FAILED ", what)


func _ready() -> void:
	await get_tree().process_frame
	var scene := get_tree().current_scene
	_check(scene != null and scene.name == &"Main", "the main scene runs")
	for i in FRAMES:
		await get_tree().process_frame
	print("fps check: %d failure(s)" % failures)
	get_tree().quit()
