@tool
extends EditorPlugin

# Renames hit.damage in user.pet and apples in draft.pet, which no tab has, as
# the redo of a rename does, so that the editor holds their texts unsaved
# (architecture §2.3), and saves tour.tscn as a binary scene too. Opens
# arena.tscn, and gunner.pet in a tab. Once the file res://check is there, prints what the loaded blaster.tres
# holds and saves the scene shown, as Ctrl+S does. Once res://refactor is
# there, makes the refactoring's edit it holds and prints the tab's text.
# Once res://probe is there, prints whether the loaded script it names has its
# file's text, and its properties. Once res://undo is there, makes a step in
# the scene shown and saves it; once res://history is there, prints whether the
# scene still has a step to undo.
# Quits once the file res://quit is there.


func _enter_tree() -> void:
	if "--lsp-hold" in OS.get_cmdline_user_args():
		_hold.call_deferred()


func _hold() -> void:
	while EditorInterface.get_resource_filesystem().is_scanning():
		await get_tree().process_frame
	var at := FileAccess.get_file_as_string("res://user.pet").find("damage")
	var plugin: Node = get_tree().root.find_children("*", "PetEditorPlugin", true, false)[0]
	plugin.call("_change_scripts", [["res://user.pet", at, at + 6, "damage", "amount"]], true)
	at = FileAccess.get_file_as_string("res://draft.pet").find("apples")
	plugin.call("_change_scripts", [["res://draft.pet", at, at + 6, "apples", "oranges"]], true)
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
		if FileAccess.file_exists("res://refactor"):
			var edit: Array = JSON.parse_string(FileAccess.get_file_as_string("res://refactor"))
			DirAccess.remove_absolute("res://refactor")
			plugin.call("_change_scripts", [edit], true)
			print("lsp refactor: ", EditorInterface.get_script_editor().get_current_editor().get_base_editor().text.c_escape())
		if FileAccess.file_exists("res://probe"):
			var path := FileAccess.get_file_as_string("res://probe")
			DirAccess.remove_absolute("res://probe")
			var script: Script = load(path)
			var names := script.get_script_property_list().map(func(p): return p.name)
			print("lsp probe: %s %s" % [script.source_code == FileAccess.get_file_as_string(path), ",".join(PackedStringArray(names))])
		if FileAccess.file_exists("res://undo"):
			DirAccess.remove_absolute("res://undo")
			var root := EditorInterface.get_edited_scene_root()
			get_undo_redo().create_action("Describe the arena")
			get_undo_redo().add_do_property(root, "editor_description", "checked")
			get_undo_redo().add_undo_property(root, "editor_description", root.editor_description)
			get_undo_redo().commit_action()
			EditorInterface.save_scene()
			print("lsp undo: saved ", _scene_has_undo())
		if FileAccess.file_exists("res://history"):
			DirAccess.remove_absolute("res://history")
			print("lsp undo: kept ", _scene_has_undo())
		await get_tree().create_timer(0.1).timeout
	get_tree().quit()


func _scene_has_undo() -> bool:
	var root := EditorInterface.get_edited_scene_root()
	return get_undo_redo().get_history_undo_redo(get_undo_redo().get_object_history_id(root)).has_undo()
