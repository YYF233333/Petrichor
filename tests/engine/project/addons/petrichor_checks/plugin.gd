@tool
extends EditorPlugin

# Editor-side checks, run with:
#   godot --headless -e --path . -- --editor-checks
# In the editor, scripts that are not tools get placeholder instances, which
# show and store the exported properties. The last check is the undo probe of
# architecture §5 step 2.

var failures := 0


func _enter_tree() -> void:
	if "--editor-checks" in OS.get_cmdline_user_args():
		_run.call_deferred()


func _check(condition: bool, what: String) -> void:
	if condition:
		print("editor check: ok: ", what)
	else:
		failures += 1
		printerr("editor check: FAILED: ", what)


func _run() -> void:
	while EditorInterface.get_resource_filesystem().is_scanning():
		await get_tree().process_frame

	_check_scene()
	_check_new_script()
	_check_header()
	_check_placeholder_fallback()
	await _check_instanced_scene_change()
	await _check_undo_merge()

	print("editor check: %d failure(s)" % failures)
	get_tree().quit(1 if failures > 0 else 0)


func _check_scene() -> void:
	var scene: PackedScene = load("res://main.tscn")
	var root := scene.instantiate(PackedScene.GEN_EDIT_STATE_INSTANCE)
	_check(root.get_script() != null, "the scene's node keeps its .pet script")
	_check(root.get("message") == "from the scene", "the placeholder holds the value saved in the scene")
	var names := root.get_property_list().map(func(p): return p.name)
	_check("message" in names, "the inspector sees the property")

	root.set("message", "edited")
	var text := _pack_to_text(root)
	_check(text.contains("script = ExtResource("), "the saved scene references the script")
	_check(text.contains("message = \"edited\""), "an edited value is saved")

	root.set("message", "hello")
	text = _pack_to_text(root)
	_check(not text.contains("message ="), "a default value is not saved")
	root.free()


func _pack_to_text(root: Node) -> String:
	var packed := PackedScene.new()
	packed.pack(root)
	var path := "user://editor_check.tscn"
	ResourceSaver.save(packed, path)
	return FileAccess.get_file_as_string(path)


func _check_new_script() -> void:
	var script: Script = ClassDB.instantiate("PetScript")
	script.source_code = "extends Node2D\n@export var message: String = \"hello\"\n"
	script.reload()
	_check(script.get_instance_base_type() == &"Node2D", "a new script reads its base class")

	var path := "user://editor_check.pet"
	_check(ResourceSaver.save(script, path) == OK, "the saver writes .pet")
	_check(FileAccess.get_file_as_string(path) == script.source_code, "the saved file is the source")
	var loaded: Script = ResourceLoader.load(path, "", ResourceLoader.CACHE_MODE_IGNORE)
	_check(loaded != null and loaded.get_instance_base_type() == &"Node2D", "the loader reads .pet")

	var node := Node2D.new()
	node.set_script(loaded)
	_check(node.get_script() == loaded, "a script can be attached in the editor")
	_check(node.get("message") == "hello", "a new placeholder starts with the default value")
	node.free()

	# Under res://, saving also writes the bytecode that the game reads and
	# reloads (architecture §2.4).
	var saved := "res://editor_check_saved.pet"
	var bytecode := "res://.godot/petrichor/editor_check_saved.petc"
	DirAccess.remove_absolute(bytecode)
	ResourceSaver.save(script, saved)
	_check(FileAccess.get_file_as_bytes(bytecode).slice(0, 4).get_string_from_ascii() == "PETC", "saving under res:// writes the bytecode")
	script.source_code += "@export var count: int = 3\n"
	ResourceSaver.save(script, saved)
	_check(script.get_script_property_list().any(func(p): return p.name == "count"), "a saved script takes the class compiled from what was saved")
	for file in [saved, saved + ".uid", bytecode]:
		DirAccess.remove_absolute(file)


# The editor learns a script's global class from its header (spec §2.1), also
# while the script cannot be compiled, here because class annotations are not
# supported yet.
func _check_header() -> void:
	var script: Script = ClassDB.instantiate("PetScript")
	script.source_code = "# A comment.\n@tool\n@icon(\"res://icon#1.svg\") class_name HeaderCheck\nextends Node2D implements Combat.Damageable\nvar x := 1\n"
	Engine.print_error_messages = false
	script.reload()
	Engine.print_error_messages = true
	_check(script.get_global_name() == &"HeaderCheck" and script.get_instance_base_type() == &"Node2D", "the header is read past class annotations, their arguments and comments")


# While a script has errors, a scene that uses it keeps its exported values,
# whether the script broke after the scene was opened or before; saving the
# scene then loses none (as for GDScript).
func _check_placeholder_fallback() -> void:
	var script_path := "res://editor_check_fallback.pet"
	var scene_path := "res://editor_check_fallback.tscn"
	var valid := "extends Node\n@export var speed: int = 1\n"
	var broken := valid + "\nfunc f() -> int:\n\treturn missing\n"
	_write(script_path, valid)
	_write(scene_path, '[gd_scene format=3]\n\n[ext_resource type="Script" path="%s" id="1"]\n\n[node name="Root" type="Node"]\nscript = ExtResource("1")\nspeed = 7\n' % script_path)

	var root := (ResourceLoader.load(scene_path, "", ResourceLoader.CACHE_MODE_IGNORE_DEEP) as PackedScene).instantiate(PackedScene.GEN_EDIT_STATE_INSTANCE)
	var script: Script = root.get_script()
	script.source_code = broken
	Engine.print_error_messages = false
	script.reload()
	Engine.print_error_messages = true
	_check(root.get("speed") == 7 and _pack_to_text(root).contains("speed = 7"), "a placeholder keeps its values once its script has errors")
	root.free()

	_write(script_path, "extends Node\n@export var speed: int = \n")
	Engine.print_error_messages = false
	root = (ResourceLoader.load(scene_path, "", ResourceLoader.CACHE_MODE_IGNORE_DEEP) as PackedScene).instantiate(PackedScene.GEN_EDIT_STATE_INSTANCE)
	Engine.print_error_messages = true
	_check(root.get("speed") == 7 and _pack_to_text(root).contains("speed = 7"), "a scene whose script has errors keeps its values")
	root.free()
	for file in [script_path, script_path + ".uid", scene_path]:
		DirAccess.remove_absolute(file)


# Spec §7.1: $X of an instanced scene has the class of that scene's root. When
# only the instanced scene changes, the next analysis in the same editor sees
# the change, though the scene that instances it did not change.
func _check_instanced_scene_change() -> void:
	var inner := "res://editor_check_inner.tscn"
	var outer := "res://editor_check_outer.tscn"
	var script_path := "res://editor_check_outer.pet"
	var bytecode := "res://.godot/petrichor/editor_check_outer.petc"
	_write(inner, '[gd_scene format=3]\n\n[node name="Inner" type="Node3D"]\n')
	_write(script_path, "extends Node\n\nfunc f() -> Vector3:\n\treturn $X.position\n")
	_write(outer, '[gd_scene format=3]\n\n[ext_resource type="Script" path="%s" id="1"]\n[ext_resource type="PackedScene" path="%s" id="2"]\n\n[node name="Outer" type="Node"]\nscript = ExtResource("1")\n\n[node name="X" parent="." instance=ExtResource("2")]\n' % [script_path, inner])
	var script: Script = ResourceLoader.load(script_path, "", ResourceLoader.CACHE_MODE_IGNORE)
	DirAccess.remove_absolute(bytecode)
	ResourceSaver.save(script, script_path)
	_check(FileAccess.file_exists(bytecode), "$X of an instanced scene has the class of its root")

	# Modification times count whole seconds.
	var written := FileAccess.get_modified_time(inner)
	while int(Time.get_unix_time_from_system()) <= written:
		await get_tree().process_frame
	_write(inner, '[gd_scene format=3]\n\n[node name="Inner" type="Node2D"]\n')
	DirAccess.remove_absolute(bytecode)
	ResourceSaver.save(script, script_path)
	_check(not FileAccess.file_exists(bytecode), "a change to the instanced scene alone reaches the next analysis ($X.position is now a Vector2)")
	for file in [inner, outer, script_path, script_path + ".uid", bytecode]:
		DirAccess.remove_absolute(file)


func _write(path: String, text: String) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	file.store_string(text)
	file.close()


# Spec §13.3: after the editor commits a rename or a move, an action with the
# same name and MERGE_ALL, created in history_changed, joins that undo step.
func _check_undo_merge() -> void:
	EditorInterface.open_scene_from_path("res://probes/undo_scene.tscn")
	await get_tree().process_frame
	var root := EditorInterface.get_edited_scene_root()
	if root == null or root.name != &"UndoRoot":
		_check(false, "the undo probe scene opens")
		return
	var a := root.get_node("A")
	var b := root.get_node("B")

	var undo := get_undo_redo()
	var history := undo.get_history_undo_redo(undo.get_object_history_id(root))
	var marker := RefCounted.new()
	marker.set_meta("value", "before")
	var state := { "armed": false, "merged_into": "" }
	var merge := func() -> void:
		if not state.armed:
			return
		state.armed = false
		# The action name is translated in the editor, so it is read, not assumed.
		state.merged_into = history.get_current_action_name()
		undo.create_action(state.merged_into, UndoRedo.MERGE_ALL, root)
		undo.add_do_method(marker, "set_meta", "value", "after")
		undo.add_undo_method(marker, "set_meta", "value", "before")
		undo.commit_action()
	undo.history_changed.connect(merge)

	var dock: Node = EditorInterface.get_base_control().find_children("*", "SceneTreeDock", true, false)[0]
	var scene_tree_editor: Node = dock.get_tree_editor()
	var tree: Tree = scene_tree_editor.find_children("*", "Tree", true, false)[0]

	# Rename A through the dock's tree, as typing a new name does.
	var steps := history.get_history_count()
	tree.set_selected(_find_item(tree.get_root(), a.get_path()), 0)
	state.armed = true
	tree.edit_selected(true)
	var line_edit: LineEdit = tree.find_children("*", "LineEdit", true, false)[0]
	line_edit.text = "Renamed"
	line_edit.text_submitted.emit("Renamed")
	_check(a.name == &"Renamed" and marker.get_meta("value") == "after", "renaming in the scene dock runs the merged operation")
	_check(history.get_history_count() == steps + 1, "the rename and the merged operation are one undo step (%s)" % state.merged_into)
	history.undo()
	_check(a.name == &"A" and marker.get_meta("value") == "before", "one undo reverts the rename and the merged operation")
	history.redo()
	_check(a.name == &"Renamed" and marker.get_meta("value") == "after", "one redo restores both")

	# Move B under A, as dropping it in the dock does.
	marker.set_meta("value", "before")
	steps = history.get_history_count()
	EditorInterface.get_selection().clear()
	EditorInterface.get_selection().add_node(b)
	state.armed = true
	scene_tree_editor.nodes_rearranged.emit([b.get_path()], a.get_path(), 0)
	_check(b.get_parent() == a and marker.get_meta("value") == "after", "moving a node in the scene dock runs the merged operation")
	_check(history.get_history_count() == steps + 1, "the move and the merged operation are one undo step (%s)" % state.merged_into)
	history.undo()
	_check(b.get_parent() == root and marker.get_meta("value") == "before", "one undo reverts the move and the merged operation")
	history.redo()
	_check(b.get_parent() == a and marker.get_meta("value") == "after", "one redo restores both")

	undo.history_changed.disconnect(merge)


func _find_item(item: TreeItem, path: NodePath) -> TreeItem:
	if item.get_metadata(0) == path:
		return item
	for child in item.get_children():
		var found := _find_item(child, path)
		if found:
			return found
	return null