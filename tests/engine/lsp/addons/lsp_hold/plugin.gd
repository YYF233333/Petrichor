@tool
extends EditorPlugin

# Renames hit.damage in user.pet, which no tab has, as the redo of a rename
# does, so that the editor holds its text unsaved (architecture §2.3), and
# saves tour.tscn as a binary scene too. Opens arena.tscn, and gunner.pet in a
# tab. Once the file res://check is there, prints what the loaded blaster.tres
# holds and saves the scene shown, as Ctrl+S does. Quits once the file
# res://quit is there.


func _enter_tree() -> void:
	if "--lsp-hold" in OS.get_cmdline_user_args():
		_hold.call_deferred()


func _hold() -> void:
	while EditorInterface.get_resource_filesystem().is_scanning():
		await get_tree().process_frame
	var at := FileAccess.get_file_as_string("res://user.pet").find("damage")
	var plugin: Node = get_tree().root.find_children("*", "PetEditorPlugin", true, false)[0]
	plugin.call("_change_scripts", [["res://user.pet", at, at + 6, "damage", "amount"]], true)
	ResourceSaver.save(load("res://tour.tscn"), "res://tour.scn")
	EditorInterface.open_scene_from_path("res://arena.tscn")
	EditorInterface.edit_script(load("res://gunner.pet"))
	print("lsp hold: done")
	while not FileAccess.file_exists("res://quit"):
		if FileAccess.file_exists("res://check"):
			DirAccess.remove_absolute("res://check")
			var blaster: Resource = load("res://blaster.tres")
			print("lsp check: %s %s" % [blaster.get("shot_spread"), blaster.get("spread")])
			EditorInterface.save_scene()
			print("lsp check: saved")
		await get_tree().create_timer(0.1).timeout
	get_tree().quit()
