extends Node

# The main scene run.py gives Ring (architecture §5 step 6). It stands in for
# Root.cs, which starts breakout from C# (breakout/Root.cs:77-86): it instances
# the game and sends BreakoutManager the init message of Ring's main.md:16-22.

const FRAMES := 300

var failures := 0


func _check(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		print("ring check: FAILED ", what)


func _ready() -> void:
	add_child(load("res://breakout/scenes/breakout/breakout.tscn").instantiate())
	var manager := get_node("/root/BreakoutManager")
	# C# hands a null string over as an empty one, and a null array as an
	# empty one (modules/mono/glue/GodotSharp/GodotSharp/Core/NativeInterop/Marshaling.cs:242-245, 579-584).
	manager.receive_init_message({
		"player_consumables": {
			"strange calculator": {"rest_times": 5, "transformed": false},
			"old model": {"rest_times": 3, "transformed": false},
		},
		"selected_skill": "blood bullet",
		"selected_policy": PackedStringArray(),
		"current_level": "",
		"current_level_manager": "",
		"player_max_health": 16,
		"player_init_ammo": 6,
	})
	# The manager resets from the message in a deferred call.
	await get_tree().process_frame
	await get_tree().process_frame
	_check(manager.max_health == 16 and manager.current_health == 16, "the player's health comes from the message (%d of %d)" % [manager.current_health, manager.max_health])
	var values := get_node("/root/ValueManager")
	_check(values.player_init_ammo == 6, "the initial ammo comes from the message (%d)" % values.player_init_ammo)
	_check(manager.skill != null, "the selected skill is set")
	_check(manager.consumables.consumable_nodes.size() == 2, "the consumables come from the message (%d)" % manager.consumables.consumable_nodes.size())

	for i in FRAMES:
		await get_tree().process_frame

	_report_scripts()
	print("ring check: %d failure(s)" % failures)
	get_tree().quit()


# run.py checks the line of each migrated script.
func _report_scripts() -> void:
	var reported := {}
	for node in get_tree().root.find_children("*", "", true, false):
		var script: Script = node.get_script()
		if script and not reported.has(script.resource_path):
			reported[script.resource_path] = true
			print("ring check: %s runs %s" % [script.resource_path.get_file().get_basename(), script.get_class()])
