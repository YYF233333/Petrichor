@tool
extends EditorPlugin

# In the editor, scripts that are not tools get placeholder instances, which
# show and store the exported properties. These checks cover that path:
#   godot --headless -e --path . -- --editor-checks

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
	script.source_code = "extends Node2D\n"
	script.reload()
	_check(script.get_instance_base_type() == &"Node2D", "a new script reads its base class")

	var path := "user://editor_check.pet"
	_check(ResourceSaver.save(script, path) == OK, "the saver writes .pet")
	_check(FileAccess.get_file_as_string(path) == "extends Node2D\n", "the saved file is the source")
	var loaded: Script = ResourceLoader.load(path, "", ResourceLoader.CACHE_MODE_IGNORE)
	_check(loaded != null and loaded.get_instance_base_type() == &"Node2D", "the loader reads .pet")

	var node := Node2D.new()
	node.set_script(loaded)
	_check(node.get_script() == loaded, "a script can be attached in the editor")
	_check(node.get("message") == "hello", "a new placeholder starts with the default value")
	node.free()
