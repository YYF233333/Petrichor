@tool
extends EditorPlugin

# Editor-side checks, run with:
#   godot --headless -e --path . -- --editor-checks
# In the editor, scripts that are not tools get placeholder instances, which
# show and store the exported properties. The last check is the undo probe of
# architecture §5 step 2.

const ErrorLog := preload("res://error_log.gd")

var failures := 0
var errors := ErrorLog.new()


func _enter_tree() -> void:
	if "--editor-checks" in OS.get_cmdline_user_args():
		_run.call_deferred()


func _check(condition: bool, what: String) -> void:
	if condition:
		print("editor check: ok: ", what)
	else:
		failures += 1
		printerr("editor check: FAILED: ", what)


# The errors reported since the last call are exactly `expected`.
func _check_errors(expected: Array[String], what: String) -> void:
	var mismatch := errors.compare(expected, "editor check")
	_check(mismatch.is_empty(), what if mismatch.is_empty() else "%s: %s" % [what, mismatch])


func _run() -> void:
	while EditorInterface.get_resource_filesystem().is_scanning():
		await get_tree().process_frame
	OS.add_logger(errors)

	_check_scene()
	_check_new_script()
	_check_header()
	_check_placeholder_fallback()
	_check_constant_defaults()
	_check_other_defaults()
	_check_inheritance_recovery()
	await _check_instanced_scene_change()
	await _check_scene_marks()
	await _check_undo_merge()
	_check_lookup()
	await _check_documentation()

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
# while the script cannot be compiled, here because of @tool, which the first version does not have.
func _check_header() -> void:
	var script: Script = ClassDB.instantiate("PetScript")
	script.source_code = "# A comment.\n\"\"\"\nA comment in a string, \"class_name Wrong\".\nclass_name Wrong\n\"\"\"\n@tool\n@icon(\"res://icon#1.svg\") class_name HeaderCheck\nextends Node2D implements Combat.Damageable\nvar x := 1\n"
	script.reload()
	_check(script.get_global_name() == &"HeaderCheck" and script.get_instance_base_type() == &"Node2D", "the header is read past class annotations, their arguments and comments")
	_check_errors(["@tool is not in the first version: scripts of this language do not run in the editor (spec §2.3)."], "the script reports only its class annotation")


# While a script has errors, a scene that uses it keeps its exported values,
# whether the script broke after the scene was opened or before; saving the
# scene then loses none (as for GDScript).
func _check_placeholder_fallback() -> void:
	var script_path := "res://editor_check_fallback.pet"
	var scene_path := "res://editor_check_fallback.tscn"
	var valid := "extends Node\n@export var speed: int = 1\n"
	var broken := valid + "@export var extra: int = 2\n\nfunc f() -> int:\n\treturn missing\n"
	_write(script_path, valid)
	_write(scene_path, '[gd_scene format=3]\n\n[ext_resource type="Script" path="%s" id="1"]\n\n[node name="Root" type="Node"]\nscript = ExtResource("1")\nspeed = 7\nextra = 9\n' % script_path)

	var packed := ResourceLoader.load(scene_path, "", ResourceLoader.CACHE_MODE_IGNORE_DEEP) as PackedScene
	var root := packed.instantiate(PackedScene.GEN_EDIT_STATE_INSTANCE)
	var script: Script = root.get_script()
	script.source_code = broken
	script.reload()
	_check(root.get("speed") == 7 and _pack_to_text(root).contains("speed = 7"), "a placeholder keeps its values once its script has errors")
	_check_errors(["editor_check_fallback.pet:6: Identifier \"missing\" is not declared in the current scope."], "the script reports its error")
	root.free()
	root = packed.instantiate(PackedScene.GEN_EDIT_STATE_INSTANCE)
	_check(root.get("extra") == 9 and _pack_to_text(root).contains("extra = 9"), "a scene instanced after its script broke keeps a value the script it had did not take")
	root.free()

	_write(script_path, "extends Node\n@export var speed: int = \n")
	root = (ResourceLoader.load(scene_path, "", ResourceLoader.CACHE_MODE_IGNORE_DEEP) as PackedScene).instantiate(PackedScene.GEN_EDIT_STATE_INSTANCE)
	_check(root.get("speed") == 7 and _pack_to_text(root).contains("speed = 7"), "a scene whose script has errors keeps its values")
	_check_errors(["editor_check_fallback.pet:2: Expected an expression, found line break."], "the script loaded with the scene reports its error")
	root.free()
	for file in [script_path, script_path + ".uid", scene_path]:
		DirAccess.remove_absolute(file)


# A constant initializer, such as a constructor or an array of constants, is
# the default value the inspector shows and the scene leaves out, as in
# GDScript; an array is typed as the member.
func _check_constant_defaults() -> void:
	var script_path := "res://editor_check_defaults.pet"
	var scene_path := "res://editor_check_defaults.tscn"
	_write(script_path, "extends Node\n@export var offset := Vector2(400.0, 0.0)\n@export var tint := Color(\"000000b0\")\n@export var up: Vector3 = Vector3.UP\n@export var placed := Transform2D(0.5, Vector2(-1, 2))\n@export var items: Array[float] = [1, 2.5]\n@export var checks: Array[LanguageCheck] = []\n")
	_write(scene_path, '[gd_scene format=3]\n\n[ext_resource type="Script" path="%s" id="1"]\n\n[node name="Root" type="Node"]\nscript = ExtResource("1")\n' % script_path)
	var root := (ResourceLoader.load(scene_path, "", ResourceLoader.CACHE_MODE_IGNORE_DEEP) as PackedScene).instantiate(PackedScene.GEN_EDIT_STATE_INSTANCE)
	var expected := { "offset": Vector2(400, 0), "tint": Color("000000b0"), "up": Vector3.UP, "placed": Transform2D(0.5, Vector2(-1, 2)), "items": [1.0, 2.5], "checks": [] }
	_check(expected.keys().all(func(name): return root.get(name) == expected[name]), "a placeholder shows constant initializers")
	var checks = root.get("checks")
	_check(checks is Array and checks.get_typed_script() == load("res://language/language_check.pet") and root.get("items").get_typed_builtin() == TYPE_FLOAT, "an array default is typed as the member")
	var text := _pack_to_text(root)
	_check(expected.keys().all(func(name): return not text.contains(name + " =")), "values equal to constant defaults are not saved")
	root.set("offset", Vector2(1, 2))
	_check(_pack_to_text(root).contains("offset = Vector2(1, 2)"), "a value other than its constant default is saved")
	root.free()
	for file in [script_path, script_path + ".uid", scene_path]:
		DirAccess.remove_absolute(file)


# Every export has a default, as in GDScript (modules/gdscript/gdscript.cpp:557-563):
# without an initializer its type's (spec §4.3), and None where code computes
# the value or the scene must assign it, so that the inspector can set them and
# the scene stores only what was set.
func _check_other_defaults() -> void:
	var script_path := "res://editor_check_other_defaults.pet"
	var scene_path := "res://editor_check_other_defaults.tscn"
	_write(script_path, "extends Node\n@export var count: int\n@export var checks: Array[LanguageCheck]\n@export var computed := Vector2(1.0, 2.0) * 2.0\n@export var target: Node\n")
	_write(scene_path, '[gd_scene format=3]\n\n[ext_resource type="Script" path="%s" id="1"]\n\n[node name="Root" type="Node"]\nscript = ExtResource("1")\n\n[node name="Child" type="Node" parent="."]\n' % script_path)
	var root := (ResourceLoader.load(scene_path, "", ResourceLoader.CACHE_MODE_IGNORE_DEEP) as PackedScene).instantiate(PackedScene.GEN_EDIT_STATE_INSTANCE)
	var names := ["count", "checks", "computed", "target"]
	var checks = root.get("checks")
	_check(root.get("count") == 0 and checks is Array and checks.get_typed_script() == load("res://language/language_check.pet"), "a placeholder shows an export without an initializer at its type's default")
	var text := _pack_to_text(root)
	_check(names.all(func(name): return not text.contains(name + " =")), "a scene stores no export that was not set")
	root.set("count", 5)
	root.set("computed", Vector2(7, 7))
	root.set("target", root.get_node("Child"))
	text = _pack_to_text(root)
	_check(text.contains("count = 5") and text.contains("computed = Vector2(7, 7)") and text.contains("target = NodePath(\"Child\")"), "the exports that were set are stored")
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


# Spec §13.2: an error in how a scene uses a script is marked on the node that
# has it, in the scene dock, and goes once the scene is fixed and saved.
func _check_scene_marks() -> void:
	var scene_path := "res://editor_marks.tscn"
	var script_path := "res://editor_marks.pet"
	_write(script_path, "extends Node\n\nfunc show_label():\n\t$Label.text = \"shown\"\n")
	_write(scene_path, '[gd_scene format=3]\n\n[ext_resource type="Script" path="%s" id="1"]\n\n[node name="Marks" type="Node"]\nscript = ExtResource("1")\n' % script_path)
	EditorInterface.open_scene_from_path(scene_path)
	await get_tree().process_frame
	var root := EditorInterface.get_edited_scene_root()
	if root == null or root.name != &"Marks":
		_check(false, "the scene with a missing node opens")
		return
	var mark := 'has no node at "Label"'
	_check("\n".join(root.call("_get_configuration_warnings")).contains(mark), "the node whose script uses $Label is marked")
	await get_tree().process_frame
	_check(_dock_tooltips(root).contains(mark), "the scene dock shows the mark")

	# Saved within the second the scene was written, which its modification
	# time does not tell apart.
	var label := Label.new()
	label.name = "Label"
	root.add_child(label)
	label.owner = root
	EditorInterface.save_scene()
	await get_tree().process_frame
	# Headless, the scene's thumbnail has no texture
	# (servers/rendering/dummy/storage/texture_storage.h:108-111; ed1daf0).
	_check_errors(['Parameter "t" is null.'], "saving the scene reports only its missing thumbnail")
	_check(root.call("_get_configuration_warnings").is_empty(), "the mark goes once the scene has the node and is saved")
	_check(not _dock_tooltips(root).contains(mark), "the scene dock no longer shows it")

	EditorInterface.close_scene()
	for file in [scene_path, script_path, script_path + ".uid", "res://.godot/petrichor/editor_marks.petc"]:
		DirAccess.remove_absolute(file)


# The tooltips of the buttons on the node's item in the scene dock.
func _dock_tooltips(node: Node) -> String:
	var dock: Node = EditorInterface.get_base_control().find_children("*", "SceneTreeDock", true, false)[0]
	var tree: Tree = dock.get_tree_editor().find_children("*", "Tree", true, false)[0]
	var item := _find_item(tree.get_root(), node.get_path())
	var tooltips := PackedStringArray()
	for i in item.get_button_count(0) if item else 0:
		tooltips.append(item.get_button_tooltip_text(0, i))
	return "\n".join(tooltips)


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


# Spec §13.2: Ctrl+click on a name goes to its declaration, which the script
# editor asks the language for (editor/script/script_text_editor.cpp:1190-1291).
func _check_lookup() -> void:
	var hit_path := "res://editor_lookup_hit.pet"
	var script_path := "res://editor_lookup.pet"
	_write(hit_path, "extends RefCounted\n\npub var damage := 1\n")
	_write(script_path, "extends Node\n\nconst Hit = preload(\"editor_lookup_hit.pet\")\n\nfunc f(hit: Hit) -> int:\n\treturn hit.damage\n")
	var script: Script = load(script_path)
	var script_editor := EditorInterface.get_script_editor()
	var click := func(symbol: String) -> CodeEdit:
		EditorInterface.edit_script(script)
		var code: CodeEdit = script_editor.get_current_editor().get_base_editor()
		code.symbol_lookup.emit(symbol, 5, code.get_line(5).find(symbol))
		return script_editor.get_current_editor().get_base_editor()
	var reached := click.call("damage")
	_check(script_editor.get_current_script().resource_path == hit_path and reached.get_caret_line() == 2, "Ctrl+click on a member goes to its declaration in its file")
	reached = click.call("hit")
	_check(script_editor.get_current_script() == script and reached.get_caret_line() == 4, "Ctrl+click on a parameter goes to its declaration")
	_check_errors([], "looking up reports nothing")
	for path in [hit_path, script_path]:
		for file in [path, path + ".uid", path.replace("res://", "res://.godot/petrichor/").replace(".pet", ".petc")]:
			DirAccess.remove_absolute(file)


# Spec §13.2: hover and the help show a class's declarations, also those
# without pub, from the documentation it gives the editor, which the script
# editor takes on saving (editor/script/script_editor_plugin.cpp:3449-3456).
func _check_documentation() -> void:
	var path := "res://editor_doc.pet"
	_write(path, "extends Node\n\npub var target: Node3D?\nvar hidden := 1\n\npub async func wait(seconds: float = 1.0) -> int:\n\treturn 1\n")
	var script: Script = load(path)
	var script_editor := EditorInterface.get_script_editor()
	script_editor.update_docs_from_script(script)
	script_editor.goto_help("class_name:\"editor_doc.pet\"")
	await get_tree().process_frame
	var pages := script_editor.find_children("*", "EditorHelp", true, false)
	var text: String = pages[-1].find_children("*", "RichTextLabel", true, false)[0].get_parsed_text().replace(char(0xA0), " ") if pages else ""
	var shown := ["Node3D?", "hidden", "seconds: float = 1.0", "async"].all(func(part): return text.contains(part))
	_check(shown, "the help of a class shows its members as declared" + ("" if shown else ": " + text))
	_check_errors([], "documenting reports nothing")
	for file in [path, path + ".uid"]:
		DirAccess.remove_absolute(file)


func _find_item(item: TreeItem, path: NodePath) -> TreeItem:
	if item.get_metadata(0) == path:
		return item
	for child in item.get_children():
		var found := _find_item(child, path)
		if found:
			return found
	return null


func _check_inheritance_recovery() -> void:
	var base_path := "res://editor_inherit_base.pet"
	var child_path := "res://editor_inherit_child.pet"
	var valid := "extends Node\n@export var speed: int = 1\npub func read() -> int:\n\treturn 1\n"
	_write(base_path, valid)
	_write(child_path, 'extends "editor_inherit_base.pet"\n@export var own: int = 2\npub func read() -> int:\n\treturn 7\n')
	var base: Script = load(base_path)
	var child: Script = load(child_path)
	_check(child.reload() == OK, "the derived script initially compiles")
	var node := Node.new()
	node.set_script(child)
	node.set("own", 9)
	base.source_code = valid.replace("-> int:", "-> String:").replace("return 1", 'return "bad"')
	_check(ResourceSaver.save(base, base_path) == OK, "saving a changed base recompiles its child")
	_check(not child.has_script_method("read"), "incompatible override invalidates the child")
	_check_errors(["editor_inherit_child.pet:3: The override widens its base return type (spec §4.6)."], "the child reports the incompatible override")
	base.source_code = valid
	_check(ResourceSaver.save(base, base_path) == OK, "the repaired base saves")
	_check(child.has_script_method("read") and node.get("speed") == 1 and node.get("own") == 9, "repairing only the base recovers the failed child and its placeholder")
	_check_errors([], "recovering the child introduces no errors")
	node.free()
	for path in [base_path, child_path]:
		for file in [path, path + ".uid", path.replace("res://", "res://.godot/petrichor/").replace(".pet", ".petc")]:
			DirAccess.remove_absolute(file)
