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
	if not found.is_empty():
		_report(file, found[0])
	return found


# run.py checks this line for each migrated script.
func _report(file: String, node: Node) -> void:
	print("fps check: %s runs %s" % [file, node.get_script().get_class()])


# Waits up to `seconds` of real time for `condition`; headless frames are not paced.
func _wait_until(condition: Callable, seconds: float) -> bool:
	var deadline := Time.get_ticks_msec() + int(seconds * 1000)
	while not condition.call():
		if Time.get_ticks_msec() > deadline:
			return false
		await get_tree().process_frame
	return true


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

	# The player's damage() emits health_updated, which main.tscn connects to the HUD.
	var huds := _nodes_with_script(scene, "hud")
	var player := scene.get_node("Player")
	player.damage(10)
	var health_text: String = huds[0].get_node("Health").text if huds.size() == 1 else ""
	_check(health_text == "%d%%" % player.health, "the HUD shows the health the player reports (%s)" % health_text)

	# The player and the enemies call Audio.play() when they land, shoot or are
	# hurt, which a headless run does not do.
	var audio := get_node("/root/Audio")
	_report("audio", audio)
	audio.play("sounds/land.ogg")
	var loaded: bool = await _wait_until(func(): return audio.get_children().any(func(p): return p.stream != null and p.stream.resource_path == "res://sounds/land.ogg"), 3.0)
	_check(loaded, "Audio.play() loads the sound into one of its players")

	# The player spawns impacts when shooting, which a headless run does not do.
	var impact: AnimatedSprite3D = load("res://objects/impact.tscn").instantiate()
	scene.add_child(impact)
	_report("impact", impact)
	impact.play("shot")
	# By id: a lambda that captured the freed impact would report an error.
	var impact_id := impact.get_instance_id()
	var freed: bool = await _wait_until(func(): return not is_instance_id_valid(impact_id), 3.0)
	_check(freed, "an impact frees itself when its animation finishes")

	print("fps check: %d failure(s)" % failures)
	get_tree().quit()
