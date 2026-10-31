extends Node

# The main scene run.py gives Ring (architecture §5 step 6). It stands in for
# Root.cs, which starts breakout from C# (breakout/Root.cs:77-86): it instances
# the game and sends BreakoutManager the init message of Ring's main.md:16-22.

const FRAMES := 300
# The bricks and enemies that the starting level (goal_2.tscn) does not have,
# some of them in no level of Ring's; placed as a level places them, so that
# their scripts run too.
const PLACED := {
	"Level/Bricks": ["bricks/basic_brick/basic_brick", "bricks/charge_brick/charge_brick", "bricks/heal_brick/heal_brick", "bricks/indestructible_brick/indestructible_brick"],
	"Level/Enemies": ["enemies/oppressive_enemy/oppressive_enemy"],
}

var failures := 0


func _check(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		print("ring check: FAILED ", what)


func _ready() -> void:
	var breakout: Node = load("res://breakout/scenes/breakout/breakout.tscn").instantiate()
	add_child(breakout)
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

	_check(await _check_utility(manager), "Utility behavior checks complete")

	for parent_path in PLACED:
		var x := 598.0
		for scene in PLACED[parent_path]:
			var placed: Node2D = load("res://breakout/scenes/%s.tscn" % scene).instantiate()
			placed.position = Vector2(x, 300 if parent_path == "Level/Bricks" else 161)
			breakout.get_node(parent_path).add_child(placed)
			x += 181

	# Seen early too: an enemy may be defeated before the frames are over.
	for i in FRAMES:
		await get_tree().process_frame
		if i == 10:
			_see_scripts()
	_see_scripts()

	_report_scripts()
	print("ring check: %d failure(s)" % failures)
	get_tree().quit()


# These public calls are also made by Ring's remaining GDScript scripts.
func _check_utility(manager: Node) -> bool:
	var utility := get_node("/root/Utility")
	var target := Node2D.new()
	add_child(target)
	var flicker: Tween = utility.flicker_red(target, 1)
	await flicker.step_finished
	_check(target.modulate.is_equal_approx(Color(0.66, 0, 0, 0.66)), "Utility's default red reaches the target")
	await flicker.finished
	_check(target.modulate.is_equal_approx(Color.WHITE), "Utility restores the target after flickering")

	utility.screen_shake(0.5)
	_check(manager.breakout.shake and is_equal_approx(manager.breakout.shake_force, 0.5), "Utility starts shaking the GDScript breakout")
	await get_tree().create_timer(0.15).timeout
	_check(not manager.breakout.shake, "Utility resumes after the timer and stops shaking")

	utility.wait_with_end_hint(target, 0.8, 0.6)
	await get_tree().create_timer(0.05).timeout
	_check(target.modulate.is_equal_approx(Color.WHITE), "the end hint waits before flickering")
	await get_tree().create_timer(0.3).timeout
	_check(target.modulate.a < 1.0, "the delayed end hint starts flickering")
	await get_tree().create_timer(0.5).timeout
	var stopped := target.modulate
	await get_tree().create_timer(0.15).timeout
	_check(target.modulate.is_equal_approx(stopped), "the detached end hint kills its tween")

	target.modulate = Color.WHITE
	utility.wait_with_end_hint(target, 0.2, 0.6)
	await get_tree().create_timer(0.1).timeout
	_check(target.modulate.a < 1.0, "a short duration starts the end hint immediately")
	await get_tree().create_timer(0.2).timeout
	stopped = target.modulate
	await get_tree().create_timer(0.15).timeout
	_check(target.modulate.is_equal_approx(stopped), "the immediate end hint also stops")
	target.free()

	utility.wait_with_end_hint(null, 0.05, 0.01)
	await get_tree().create_timer(0.1).timeout
	return true


# The scripts of the nodes in the tree, and the scripts they extend, by path.
var seen_scripts := {}


func _see_scripts() -> void:
	for node in get_tree().root.find_children("*", "", true, false):
		var script: Script = node.get_script()
		while script and not seen_scripts.has(script.resource_path):
			seen_scripts[script.resource_path] = script.get_class()
			script = script.get_base_script()


# run.py checks the line of each migrated script.
func _report_scripts() -> void:
	for path in seen_scripts:
		print("ring check: %s runs %s" % [path.get_file().get_basename(), seen_scripts[path]])
	# No node has these: combat.pet declares the interface and has no GDScript
	# original, and heal_brick.tscn has charge_brick.gd, so that no scene of
	# Ring's uses heal_brick.gd.
	for path in ["res://breakout/scenes/components/combat.pet", "res://breakout/scenes/bricks/heal_brick/heal_brick.pet"]:
		if ResourceLoader.exists(path):
			print("ring check: %s runs %s" % [path.get_file().get_basename(), load(path).get_class()])
