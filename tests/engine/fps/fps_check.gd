extends Node

# An autoload that run.py adds to the FPS starter kit (architecture §5 step 4):
# it lets the main scene run for a while, checks what the migrated scripts did,
# and quits. A check whose script is not migrated yet checks the original.

const FRAMES := 120

var failures := 0


func _check(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		print("fps check: FAILED ", what)


func _nodes_with_script(root: Node, file: String) -> Array[Node]:
	var found: Array[Node] = []
	for node in root.find_children("*", "", true, false):
		var script: Script = node.get_script()
		if script and script.resource_path.get_file().get_basename() == file:
			found.append(node)
	# run.py checks this line for each migrated script.
	if not found.is_empty():
		print("fps check: %s runs %s" % [file, found[0].get_script().get_class()])
	return found


func _ready() -> void:
	await get_tree().process_frame
	var scene := get_tree().current_scene
	_check(scene != null and scene.name == &"Main", "the main scene runs")

	var clouds := _nodes_with_script(scene, "cloud")
	_check(clouds.size() > 0, "the scene has clouds")
	var heights := clouds.map(func(cloud): return cloud.position.y)

	for i in FRAMES:
		await get_tree().process_frame

	var moved := 0
	for i in clouds.size():
		if clouds[i].position.y != heights[i]:
			moved += 1
	_check(moved == clouds.size(), "every cloud moves up and down (%d of %d)" % [moved, clouds.size()])

	print("fps check: %d failure(s)" % failures)
	get_tree().quit()
