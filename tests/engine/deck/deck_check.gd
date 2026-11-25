extends Node

# An autoload that run.py adds to deck_builder_tutorial (architecture §5), whose
# main menu run.py starts. It plays one run through code, as the menus' and the
# map's signals would: a new run with the warrior, a battle of the map's first
# floor, cards played and turns ended until the battle ends or TURNS have
# passed, and the rewards of a battle won. Then it reports the scripts that ran.

const TURNS := 8
# The run is the same each time: the map is laid out with the global random
# numbers, and RNG deals the cards and picks the enemies' actions.
const SEED := 3
const SAVE := "user://savegame.tres"

var failures := 0
var counts := { "hands": 0, "enemy_turns": 0, "played": 0 }
var seen_scripts := {}
var seen_objects := {}


func _check(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		print("deck check: FAILED ", what)


# Waits up to `seconds` of game time for `condition`, which the runner's fixed
# frame rate makes independent of how fast the frames run.
func _wait_until(condition: Callable, seconds: float) -> bool:
	var waited := 0.0
	while not condition.call():
		if waited > seconds:
			return false
		await get_tree().process_frame
		waited += get_process_delta_time()
	return true


func _scene_named(scene_name: StringName) -> Callable:
	return func(): return get_tree().current_scene != null and get_tree().current_scene.name == scene_name


func _init() -> void:
	# The game continues the run it saved in the user's data, which every copy
	# of the project shares.
	if FileAccess.file_exists(SAVE):
		DirAccess.remove_absolute(SAVE)
	# The battle's end pauses the tree.
	process_mode = Node.PROCESS_MODE_ALWAYS


func _ready() -> void:
	var tree := get_tree()
	Events.player_hand_drawn.connect(func(): counts.hands += 1)
	Events.enemy_turn_ended.connect(func(): counts.enemy_turns += 1)
	Events.card_played.connect(func(_card): counts.played += 1)
	await tree.process_frame
	_see_tree()

	_check(tree.current_scene.name == &"MainMenu" and tree.current_scene.get_node("%Continue").disabled, "the main menu runs, with nothing to continue")
	tree.current_scene.get_node("VBoxContainer/NewRun").pressed.emit()
	_check(await _wait_until(_scene_named(&"CharacterSelector"), 2.0), "New Run opens the character selector")
	var selector := tree.current_scene
	_see_tree()
	selector.get_node("CharacterButtons/WizardButton").pressed.emit()
	var wizard: String = selector.get_node("%Title").text
	selector.get_node("CharacterButtons/WarriorButton").pressed.emit()
	_check(wizard == "Wizard" and selector.get_node("%Title").text == "Warrior", "the character buttons show the character picked (%s)" % wizard)
	seed(SEED)
	RNG.instance.seed = SEED
	selector.get_node("StartButton").pressed.emit()
	_check(await _wait_until(_scene_named(&"Run"), 2.0), "Start begins a run")
	var run := tree.current_scene
	var character = run.character
	_check(character.character_name == "Warrior" and character.deck.cards.size() == 10 and character.health == character.max_health, "the run has the warrior with the starting deck and full health")
	_check(FileAccess.file_exists(SAVE), "the new run is saved")
	_see_tree()

	var open: Array = run.get_node("Map").get_node("%Rooms").get_children().filter(func(map_room): return map_room.available)
	_check(not open.is_empty() and open.all(func(map_room): return map_room.room.row == 0 and map_room.room.battle_stats != null), "the map opens its first floor, of battles (%d rooms)" % open.size())
	if open.is_empty():
		_finish()
		return
	var click := InputEventMouseButton.new()
	click.button_index = MOUSE_BUTTON_LEFT
	click.pressed = true
	open[0].input_event.emit(get_viewport(), click, 0)
	var view: Node = run.get_node("CurrentView")
	_check(await _wait_until(func(): return view.get_child_count() > 0 and view.get_child(0).name == &"Battle", 3.0), "clicking an open room enters its battle")
	if view.get_child_count() == 0:
		_finish()
		return
	var outcome: String = await _check_battle(view.get_child(0), character)

	if outcome == "Victorious!":
		await _check_rewards(run, view, character)
	_finish()


# Plays the cards the mana allows each turn, on the first enemy, until the
# battle ends or TURNS have passed. Gives the battle's end panel's text.
func _check_battle(battle: Node, character) -> String:
	var hand := battle.get_node("BattleUI/Hand")
	var enemies := battle.get_node("EnemyHandler")
	var player := battle.get_node("Player")
	var panel := battle.get_node("BattleOverLayer/BattleOverPanel")
	var end_turn: Button = battle.get_node("BattleUI/EndTurnButton")
	var fighting := enemies.get_children().filter(func(enemy): return not enemy.is_queued_for_deletion())
	_check(not fighting.is_empty() and fighting.all(func(enemy): return enemy.stats.health == enemy.stats.max_health and enemy.current_action != null), "the battle's enemies come with full health and an action each (%d)" % fighting.size())
	var damaged := false
	var blocked := false
	var turns := 0
	while turns < TURNS and not panel.visible:
		var drawn: bool = await _wait_until(func(): return counts.hands > turns or panel.visible, 10.0)
		if panel.visible:
			break
		_check(drawn and hand.get_child_count() == character.cards_per_turn and character.mana == character.max_mana, "turn %d draws a hand of %d with full mana (%d, %d)" % [turns, character.cards_per_turn, hand.get_child_count(), character.mana])
		if not drawn:
			break
		_see_tree()
		for card_ui in hand.get_children():
			if not is_instance_valid(card_ui) or enemies.get_child_count() == 0 or not character.can_play_card(card_ui.card):
				continue
			var enemy: Node = enemies.get_child(0)
			var enemy_before: int = enemy.stats.health + enemy.stats.block
			var block_before: int = character.block
			var mana_before: int = character.mana
			var card = card_ui.card
			card_ui.targets.append(enemy if card.is_single_targeted() else player)
			card_ui.play()
			_see_tree()
			_check(character.mana == mana_before - card.cost, "playing %s spends its cost" % card.id)
			# Damage lands in a tween after the target shakes.
			await _wait_until(func(): return not is_instance_valid(enemy) or enemy.stats.health + enemy.stats.block < enemy_before or character.block > block_before, 0.5)
			damaged = damaged or not is_instance_valid(enemy) or enemy.stats.health + enemy.stats.block < enemy_before
			blocked = blocked or character.block > block_before
		if panel.visible or enemies.get_child_count() == 0:
			break
		end_turn.pressed.emit()
		turns += 1
		_check(await _wait_until(func(): return counts.enemy_turns >= turns or panel.visible, 10.0), "the enemies take turn %d" % turns)
	_check(damaged and blocked, "the cards played damage an enemy and block for the player")
	_check(counts.played > 0, "the cards played are reported (%d)" % counts.played)
	await _wait_until(func(): return panel.visible, 5.0)
	var outcome: String = panel.get_node("%Label").text if panel.visible else ""
	_check(outcome == "Victorious!" or outcome == "Game Over!" or turns == TURNS, "the battle ends or goes on for %d turns (%s after %d)" % [TURNS, outcome, turns])
	print("deck check: the battle ended %s after %d turn(s), with %d card(s) played" % [outcome if outcome else "not", turns, counts.played])
	_see_tree()
	return outcome


# The rewards of a battle won: the gold and one card of three, then the map.
func _check_rewards(run: Node, view: Node, character) -> void:
	var panel := view.get_child(0).get_node("BattleOverLayer/BattleOverPanel")
	panel.get_node("%ContinueButton").pressed.emit()
	_check(await _wait_until(func(): return view.get_child_count() > 0 and view.get_child(-1).name == &"BattleReward", 2.0), "Continue shows the rewards")
	var reward := view.get_child(-1)
	var rewards := reward.get_node("%Rewards")
	_check(await _wait_until(func(): return rewards.get_child_count() == 2 and rewards.get_children().all(func(button): return not button.is_queued_for_deletion()), 1.0), "the rewards are gold and a card")
	_see_tree()
	var run_stats = run.get_node("%GoldUI").run_stats
	var gold: int = run_stats.gold
	rewards.get_child(0).pressed.emit()
	_check(run_stats.gold > gold, "taking the gold adds it (%d to %d)" % [gold, run_stats.gold])
	rewards.get_child(1).pressed.emit()
	await get_tree().process_frame
	var offer := reward.get_node("CardRewards")
	var choices := offer.get_node("%Cards").get_children()
	_check(choices.size() == 3, "the card reward offers 3 cards (%d)" % choices.size())
	var deck_size: int = character.deck.cards.size()
	if not choices.is_empty():
		choices[0].tooltip_requested.emit(choices[0].card)
		offer.get_node("%TakeButton").pressed.emit()
	_check(character.deck.cards.size() == deck_size + 1, "taking a card adds it to the deck")
	_see_tree()
	reward.get_node("VBoxContainer/BackButton").pressed.emit()
	var map := run.get_node("Map")
	_check(await _wait_until(func(): return map.visible and map.get_node("%Rooms").get_children().any(func(map_room): return map_room.available and map_room.room.row == 1), 2.0), "back on the map, the next floor opens")


func _finish() -> void:
	_see_tree()
	if FileAccess.file_exists(SAVE):
		var saved: Resource = ResourceLoader.load(SAVE)
		_check(saved.char_stats.character_name == "Warrior" and not saved.current_deck.cards.is_empty(), "the run's save loads with its character and deck")
		_see(saved)
	_report_scripts()
	if FileAccess.file_exists(SAVE):
		DirAccess.remove_absolute(SAVE)
	print("deck check: %d failure(s)" % failures)
	get_tree().quit()


# The scripts of the nodes in the tree, of the resources and objects their
# members hold, and the scripts they extend, by path.
func _see_tree() -> void:
	for node in get_tree().root.find_children("*", "", true, false):
		_see(node)


func _see(value) -> void:
	if value is Array:
		for element in value:
			_see(element)
		return
	if not value is Object or not is_instance_valid(value) or seen_objects.has(value.get_instance_id()):
		return
	seen_objects[value.get_instance_id()] = true
	var script: Script = value.get_script()
	while script and not seen_scripts.has(script.resource_path):
		seen_scripts[script.resource_path] = script.get_class()
		script = script.get_base_script()
	for property in value.get_property_list():
		if property.usage & PROPERTY_USAGE_SCRIPT_VARIABLE:
			_see(value.get(property.name))


# run.py checks the line of each migrated script. The scripts the run did not
# reach are made here, which runs their initializers.
func _report_scripts() -> void:
	for path in seen_scripts:
		print("deck check: %s runs %s" % [path.get_file().get_basename(), seen_scripts[path]])
	for path in _scripts("res://"):
		if not seen_scripts.has(path):
			var made: Object = load(path).new()
			print("deck check: %s runs %s, made by the check" % [path.get_file().get_basename(), made.get_script().get_class()])
			if made is Node:
				made.free()


func _scripts(directory: String) -> Array[String]:
	var found: Array[String] = []
	for file in DirAccess.get_files_at(directory):
		if file.get_extension() == "pet":
			found.append(directory.path_join(file))
	for child in DirAccess.get_directories_at(directory):
		if not child in ["addons", "script_templates"] and not child.begins_with("."):
			found.append_array(_scripts(directory.path_join(child)))
	return found
