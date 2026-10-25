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
func _report(file: String, object: Object) -> void:
	print("fps check: %s runs %s" % [file, object.get_script().get_class()])


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
	var player: Node3D = scene.get_node("Player")
	_report("player", player)
	player.damage(10)
	var health: Label = huds[0].get_node("Health") if huds.size() == 1 else Label.new()
	_check(health.text == "90%", "the HUD shows the health the player reports (%s)" % health.text)

	# The weapons are resources that player.tscn lists in its typed export.
	var weapons: Array = player.weapons
	_check(weapons.size() == 2 and weapons.all(func(w): return w.model is PackedScene and w.crosshair is Texture2D and w.cooldown > 0.0), "the player's weapons load with their exports")
	if not weapons.is_empty():
		_report("weapon", weapons[0])

	# A tween puts the weapon's model in the container and renders it on layer
	# 2 only, and the weapon_toggle action changes to the next weapon.
	var container := player.get_node("Head/Camera/SubViewportContainer/SubViewport/CameraItem/Container")
	for i in weapons.size():
		if i > 0:
			Input.action_press("weapon_toggle")
			await get_tree().process_frame
			Input.action_release("weapon_toggle")
		var model_path: String = weapons[i].model.resource_path
		var shown: bool = await _wait_until(func(): return container.get_child_count() == 1 and container.get_child(0).scene_file_path == model_path, 3.0)
		_check(shown, "weapon %d shows its model" % i)
		if shown:
			var meshes := container.get_child(0).find_children("*", "MeshInstance3D")
			_check(not meshes.is_empty() and meshes.all(func(mesh): return mesh.layers == 2), "weapon %d's model renders on layer 2 only" % i)
		_check(player.crosshair.texture == weapons[i].crosshair, "weapon %d shows its crosshair" % i)

	# An enemy shoots what is Combat.Damageable in front of it: with the player
	# held there, the HUD's health drops.
	var enemies := _nodes_with_script(scene, "enemy")
	_check(enemies.size() == 4, "the scene has its four enemies")
	# combat.pet, which declares the interface, has no GDScript original.
	if ResourceLoader.exists("res://scripts/combat.pet"):
		print("fps check: combat runs %s" % load("res://scripts/combat.pet").get_class())
	if not enemies.is_empty():
		var enemy: Node3D = enemies[0]
		var start := player.global_position
		var before := health.text
		var held_in_front := func():
			player.global_position = enemy.global_position + Vector3(0, -0.5, 2)
			return health.text != before
		var hit: bool = await _wait_until(held_in_front, 5.0)
		player.global_position = start
		_check(hit, "an enemy's shot damages the player (%s)" % health.text)

		# By id, as below for impacts.
		var target: Node3D = enemies[1]
		var target_id := target.get_instance_id()
		target.damage(100)
		var destroyed: bool = await _wait_until(func(): return not is_instance_id_valid(target_id), 3.0)
		_check(destroyed, "an enemy damaged by its whole health is destroyed")

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
