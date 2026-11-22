@tool
extends EditorPlugin

# Renames apples in draft.pet, which no tab has, as the redo of a rename does,
# which writes it (architecture §2.3). Leaves an unsaved change in a tab of
# user.pet, and saves tour.tscn as a binary scene too. Opens arena.tscn, and
# gunner.pet in a tab. Once the file res://check is there, prints what the loaded blaster.tres
# holds and saves the scene shown, as Ctrl+S does. Once res://refactor is
# there, makes the refactoring's edit it holds and prints the tab's text.
# Once res://probe is there, prints whether the loaded script it names has its
# file's text, and its properties, and keeps it loaded. Once res://undo is there, makes a step in
# the scene shown and saves it; once res://history is there, prints whether the
# scene still has a step to undo. Once res://rename is there, renames at the
# place in the script it names, as F2 does, prints the tab's text and undoes
# the rename. Once res://ramp is there, widens ramp.tres through the undo
# history and saves the scene shown, which saves the resource too; the next
# time, prints whether the resource keeps its gradient. Once res://scene is
# there, prints whether the scene shown has a node Extra; once res://dirty is
# there, makes an unsaved change in the tab of the script it names, through the
# undo history in the resource it names, or else in the scene shown. Once
# res://call is there, calls the method
# of the Petrichor plugin it names with the arguments after, and prints the
# result.
# Quits once the file res://quit is there.


var probed: Script
var ramp: GradientTexture1D
var gradient: Gradient
var edited: Resource


func _enter_tree() -> void:
	if "--lsp-hold" in OS.get_cmdline_user_args():
		_hold.call_deferred()


func _hold() -> void:
	while EditorInterface.get_resource_filesystem().is_scanning():
		await get_tree().process_frame
	var plugin: Node = get_tree().root.find_children("*", "PetEditorPlugin", true, false)[0]
	var at := FileAccess.get_file_as_string("res://draft.pet").find("apples")
	plugin.call("_change_scripts", [["res://draft.pet", at, at + 6, "apples", "oranges"]], true)
	EditorInterface.edit_script(load("res://user.pet"))
	var user: CodeEdit = EditorInterface.get_script_editor().get_current_editor().get_base_editor()
	user.insert_text("# Unsaved in Godot, beside hit.damage.\n", user.get_line_count() - 1, 0)
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
			probed = load(path)
			var names := probed.get_script_property_list().map(func(p): return p.name)
			print("lsp probe: %s %s" % [probed.source_code == FileAccess.get_file_as_string(path), ",".join(PackedStringArray(names))])
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
		if FileAccess.file_exists("res://ramp"):
			DirAccess.remove_absolute("res://ramp")
			if ramp:
				print("lsp ramp: kept ", ramp.gradient == gradient)
			else:
				ramp = load("res://ramp.tres")
				gradient = ramp.gradient
				get_undo_redo().create_action("Widen the ramp")
				get_undo_redo().add_do_property(ramp, "width", 64)
				get_undo_redo().add_undo_property(ramp, "width", ramp.width)
				get_undo_redo().commit_action()
				EditorInterface.save_scene()
				print("lsp ramp: saved ", FileAccess.get_file_as_string("res://ramp.tres").contains("width = 64"))
		if FileAccess.file_exists("res://scene"):
			DirAccess.remove_absolute("res://scene")
			print("lsp scene: ", EditorInterface.get_edited_scene_root().has_node("Extra"))
		if FileAccess.file_exists("res://dirty"):
			var path := FileAccess.get_file_as_string("res://dirty")
			DirAccess.remove_absolute("res://dirty")
			if path.ends_with(".tres"):
				edited = load(path)
				get_undo_redo().create_action("Widen it")
				get_undo_redo().add_do_property(edited, "width", 128)
				get_undo_redo().add_undo_property(edited, "width", edited.get("width"))
				get_undo_redo().commit_action()
			elif path:
				EditorInterface.edit_script(load(path))
				EditorInterface.get_script_editor().get_current_editor().get_base_editor().insert_text("# Unsaved in Godot.\n", 0, 0)
			else:
				var root := EditorInterface.get_edited_scene_root()
				get_undo_redo().create_action("Mark the arena")
				get_undo_redo().add_do_property(root, "editor_description", "unsaved")
				get_undo_redo().add_undo_property(root, "editor_description", root.editor_description)
				get_undo_redo().commit_action()
			print("lsp dirty: ", EditorInterface.get_script_editor().get_unsaved_files(), EditorInterface.get_unsaved_scenes())
		if FileAccess.file_exists("res://call"):
			var called: Array = JSON.parse_string(FileAccess.get_file_as_string("res://call"))
			DirAccess.remove_absolute("res://call")
			print("lsp call: ", plugin.callv(called[0], called.slice(1).map(func(a): return int(a) if a is float else a)))
		if FileAccess.file_exists("res://rename"):
			var asked: Array = JSON.parse_string(FileAccess.get_file_as_string("res://rename"))
			DirAccess.remove_absolute("res://rename")
			EditorInterface.edit_script(load(asked[0]))
			EditorInterface.set_main_screen_editor("Script")
			await get_tree().process_frame
			var code: CodeEdit = EditorInterface.get_script_editor().get_current_editor().get_base_editor()
			var before := code.text
			code.set_caret_line(int(asked[1]))
			code.set_caret_column(int(asked[2]))
			code.grab_focus()
			var key := InputEventKey.new()
			key.keycode = KEY_F2
			key.pressed = true
			get_viewport().push_input(key)
			var name_edit: LineEdit = plugin.find_children("*", "PopupPanel", false, false)[0].find_children("*", "LineEdit", true, false)[0]
			name_edit.text = asked[3]
			name_edit.text_submitted.emit(asked[3])
			print("lsp rename: ", code.text.c_escape())
			if code.text != before:
				code.undo()
		await get_tree().create_timer(0.1).timeout
	get_tree().quit()


func _scene_has_undo() -> bool:
	var root := EditorInterface.get_edited_scene_root()
	return get_undo_redo().get_history_undo_redo(get_undo_redo().get_object_history_id(root)).has_undo()
