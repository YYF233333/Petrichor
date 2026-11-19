@tool
extends EditorPlugin

# Editor-side checks, run with:
#   godot --headless -e --path . -- --editor-checks
# In the editor, scripts that are not tools get placeholder instances, which
# show and store the exported properties.

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
	await _check_global_bases()
	_check_placeholder_fallback()
	_check_constant_defaults()
	_check_other_defaults()
	_check_struct_exports()
	await _check_stored_exports()
	_check_scene_exports()
	await _check_named_members()
	_check_inheritance_recovery()
	await _check_instanced_scene_change()
	await _check_scene_marks()
	_check_lookup()
	_check_completion()
	_check_highlighting()
	await _check_members_overview()
	await _check_documentation()
	await _check_script_dialog()
	_check_autoload_by_uid()
	await _check_rename()
	await _check_rename_open_scenes()
	await _check_rename_transaction()
	await _check_embedded_scene()
	await _check_quick_fixes()
	await _check_unsaved_texts()
	await _check_stale_reference()
	await _check_shown_again()
	await _check_node_moves()
	_check_unsaved_export()
	_leave_held_script()

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
	script.source_code = "extends Node2D\n@export pub var message: String = \"hello\"\n"
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
	script.source_code += "@export pub var count: int = 3\n"
	ResourceSaver.save(script, saved)
	_check(script.get_script_property_list().any(func(p): return p.name == "count"), "a saved script takes the class compiled from what was saved")
	for file in [saved, saved + ".uid", bytecode]:
		DirAccess.remove_absolute(file)


# The global class list gives a class the one it names as its base, as for
# GDScript, so that the inspector offers a subclass for an exported resource
# of its base's type; through an inner class, the engine class it extends.
func _check_global_bases() -> void:
	var bases := {
		"res://editor_check_weapon.pet": "class_name EditorCheckWeapon extends Resource\n",
		"res://editor_check_holder.pet": "class_name EditorCheckHolder extends Node\n\npub class Inner extends Node2D:\n\tpass\n",
	}
	var subclasses := {
		"res://editor_check_sword.pet": "class_name EditorCheckSword extends EditorCheckWeapon\n",
		"res://editor_check_inner_child.pet": "class_name EditorCheckInnerChild extends EditorCheckHolder.Inner\n",
	}
	for files in [bases, subclasses]:
		for path in files:
			_write(path, files[path])
			EditorInterface.get_resource_filesystem().update_file(path)
		await get_tree().process_frame
	var listed := {}
	for entry in ProjectSettings.get_global_class_list():
		listed[String(entry["class"])] = String(entry["base"])
	_check(listed.get("EditorCheckSword") == "EditorCheckWeapon", "a global class's base is the class it names")
	_check(listed.get("EditorCheckInnerChild") == "Node2D", "through an inner class, a global class's base is the engine class that one extends")
	for files in [subclasses, bases]:
		for path in files:
			DirAccess.remove_absolute(path)
			DirAccess.remove_absolute(path + ".uid")
			EditorInterface.get_resource_filesystem().update_file(path)


# The editor learns a script's global class from its header (spec §2.1), also
# while the script cannot be compiled, here because of @tool, which the first version does not have.
func _check_header() -> void:
	var script: Script = ClassDB.instantiate("PetScript")
	script.source_code = "# A comment.\n\"\"\"\nA comment in a string, \"class_name Wrong\".\nclass_name Wrong\n\"\"\"\n@tool\n@icon(\"res://icon#1.svg\") class_name HeaderCheck\nextends Node2D implements Combat.Damageable\nvar x := 1\n"
	script.reload()
	_check(script.get_global_name() == &"HeaderCheck" and script.get_instance_base_type() == &"Node2D", "the header is read past class annotations, their arguments and comments")
	_check_errors(["@tool is not in the first version: scripts of this language do not run in the editor."], "the script reports only its class annotation")


# While a script has errors, a scene that uses it keeps its exported values,
# whether the script broke after the scene was opened or before; saving the
# scene then loses none (as for GDScript).
func _check_placeholder_fallback() -> void:
	var script_path := "res://editor_check_fallback.pet"
	var scene_path := "res://editor_check_fallback.tscn"
	var valid := "extends Node\n@export pub var speed: int = 1\n"
	var broken := valid + "@export pub var extra: int = 2\n\nfunc f() -> int:\n\treturn missing\n"
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

	_write(script_path, "extends Node\n@export pub var speed: int = \n")
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
	_write(script_path, "extends Node\n@export pub var offset := Vector2(400.0, 0.0)\n@export pub var tint := Color(\"000000b0\")\n@export pub var up: Vector3 = Vector3.UP\n@export pub var placed := Transform2D(0.5, Vector2(-1, 2))\n@export pub var items: Array[float] = [1, 2.5]\n@export pub var checks: Array[LanguageCheck] = []\n")
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
	_write(script_path, "extends Node\n@export pub var count: int\n@export pub var checks: Array[LanguageCheck]\n@export pub var computed := Vector2(1.0, 2.0) * 2.0\n@export pub var target: Node\n")
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


# Spec §7.1: $X of an instanced scene has the class of that scene's root, and
# a node from a base scene the class it has there. When only the instanced or
# the base scene changes, the next analysis in the same editor sees the
# change, though the scene that uses it did not change.
func _check_instanced_scene_change() -> void:
	var inner := "res://editor_check_inner.tscn"
	var base := "res://editor_check_base.tscn"
	var outer := "res://editor_check_outer.pet"
	var derived := "res://editor_check_derived.pet"
	_write(inner, '[gd_scene format=3]\n\n[node name="Inner" type="Node3D"]\n')
	_write(base, '[gd_scene format=3]\n\n[node name="Base" type="Node"]\n\n[node name="X" type="Node3D" parent="."]\n')
	for path in [outer, derived]:
		_write(path, "extends Node\n\nfunc f() -> Vector3:\n\treturn $X.position\n")
	_write(outer.get_basename() + ".tscn", '[gd_scene format=3]\n\n[ext_resource type="Script" path="%s" id="1"]\n[ext_resource type="PackedScene" path="%s" id="2"]\n\n[node name="Outer" type="Node"]\nscript = ExtResource("1")\n\n[node name="X" parent="." instance=ExtResource("2")]\n' % [outer, inner])
	_write(derived.get_basename() + ".tscn", '[gd_scene format=3]\n\n[ext_resource type="Script" path="%s" id="1"]\n[ext_resource type="PackedScene" path="%s" id="2"]\n\n[node name="Derived" instance=ExtResource("2")]\nscript = ExtResource("1")\n' % [derived, base])
	var scripts := {}
	for path in [outer, derived]:
		scripts[path] = ResourceLoader.load(path, "", ResourceLoader.CACHE_MODE_IGNORE)
	_check(_compiles(scripts[outer], outer), "$X of an instanced scene has the class of its root")
	_check(_compiles(scripts[derived], derived), "$X of a base scene's node has the class it has there")

	var written := maxi(FileAccess.get_modified_time(inner), FileAccess.get_modified_time(base))
	await _write_after(inner, '[gd_scene format=3]\n\n[node name="Inner" type="Node2D"]\n', written)
	await _write_after(base, '[gd_scene format=3]\n\n[node name="Base" type="Node"]\n\n[node name="X" type="Node2D" parent="."]\n', written)
	_scanned()
	_check(not _compiles(scripts[outer], outer), "a change to the instanced scene alone reaches the next analysis ($X.position is now a Vector2)")
	_check(not _compiles(scripts[derived], derived), "a change to the base scene alone reaches the next analysis")
	for path in [outer, derived]:
		for file in [path, path + ".uid", path.get_basename() + ".tscn", "res://.godot/petrichor/%s.petc" % path.get_file().get_basename()]:
			DirAccess.remove_absolute(file)
	for file in [inner, base]:
		DirAccess.remove_absolute(file)


# Whether saving the script compiles it, which writes its bytecode.
func _compiles(script: Script, path: String) -> bool:
	var bytecode := "res://.godot/petrichor/%s.petc" % path.get_file().get_basename()
	DirAccess.remove_absolute(bytecode)
	ResourceSaver.save(script, path)
	return FileAccess.file_exists(bytecode)


# The editor learns of files changed outside it at its next scan, as when its
# window takes the focus (editor/editor_node.cpp:1048-1058; ed1daf0).
func _scanned() -> void:
	EditorInterface.get_resource_filesystem().filesystem_changed.emit()


func _write(path: String, text: String) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	file.store_string(text)
	file.close()


# Writes the file so that its modification time is later than `after`. The
# times count whole seconds, and Linux stamps a file from a clock that is up to
# a timer tick behind the one read here.
func _write_after(path: String, text: String, after: int) -> void:
	while int(Time.get_unix_time_from_system()) <= after:
		await get_tree().process_frame
	_write(path, text)
	while FileAccess.get_modified_time(path) <= after:
		await get_tree().process_frame
		_write(path, text)


# Spec §7.5, §13.2: an error in how a scene uses a script is marked on the node
# that has it, in the scene dock, and goes once the scene is fixed, before it
# is saved (architecture §2.4).
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
	var warnings := "\n".join(root.call("_get_configuration_warnings"))
	_check(warnings.contains(mark) and not warnings.contains(scene_path), "the node whose script uses $Label is marked, without the scene's own path")
	await get_tree().process_frame
	_check(_dock_tooltips(root).contains(mark), "the scene dock shows the mark")

	# Sooner than the editor's pause.
	var plugin: Node = get_tree().root.find_children("*", "PetEditorPlugin", true, false)[0]
	var marks: Timer = plugin.get_children().filter(func(c): return c is Timer)[0]
	marks.wait_time = 0.05
	var label := Label.new()
	label.name = "Label"
	var undo_redo := get_undo_redo()
	undo_redo.create_action("Add Label")
	undo_redo.add_do_method(root, "add_child", label)
	undo_redo.add_do_property(label, "owner", root)
	undo_redo.add_do_reference(label)
	undo_redo.add_undo_method(root, "remove_child", label)
	undo_redo.commit_action()
	await marks.timeout
	_check(root.call("_get_configuration_warnings").is_empty(), "the mark goes once the scene has the node, before it is saved")

	# Saved within the second the scene was written, which its modification
	# time does not tell apart.
	EditorInterface.save_scene()
	await get_tree().process_frame
	# Headless, the scene's thumbnail has no texture
	# (servers/rendering/dummy/storage/texture_storage.h:108-111; ed1daf0).
	_check_errors(['Parameter "t" is null.'], "saving the scene reports only its missing thumbnail")
	_check(root.call("_get_configuration_warnings").is_empty() and not _dock_tooltips(root).contains(mark), "the scene dock no longer shows it once the scene is saved")

	EditorInterface.close_scene()
	for file in [scene_path, script_path, script_path + ".uid", "res://.godot/petrichor/editor_marks.petc"]:
		DirAccess.remove_absolute(file)


# The errors marked on the root of a scene loaded anew (spec §13.2).
func _scene_marks(path: String) -> String:
	var root := (ResourceLoader.load(path, "", ResourceLoader.CACHE_MODE_IGNORE_DEEP) as PackedScene).instantiate(PackedScene.GEN_EDIT_STATE_INSTANCE)
	var marks := "\n".join(root.call("_get_configuration_warnings"))
	root.free()
	return marks


# The tooltips of the buttons on the node's item in the scene dock.
func _dock_tooltips(node: Node) -> String:
	var dock: Node = EditorInterface.get_base_control().find_children("*", "SceneTreeDock", true, false)[0]
	var tree: Tree = dock.get_tree_editor().find_children("*", "Tree", true, false)[0]
	var item := _find_item(tree.get_root(), node)
	_check(item != null, "the scene dock has an item for the node")
	var tooltips := PackedStringArray()
	for i in item.get_button_count(0) if item else 0:
		tooltips.append(item.get_button_tooltip_text(0, i))
	return "\n".join(tooltips)


# Spec §13.2: Ctrl+click on a name goes to its declaration, which the script
# editor asks the language for (editor/script/script_text_editor.cpp:1190-1291).
func _check_lookup() -> void:
	var hit_path := "res://editor_lookup_hit.pet"
	var base_gd := "res://editor_lookup_base.gd"
	var child_gd := "res://editor_lookup_child.gd"
	var script_path := "res://editor_lookup.pet"
	_write(hit_path, "extends RefCounted\n\npub var damage := 1\n")
	_write(base_gd, "extends RefCounted\n\nconst LIMIT := 5\nvar speed := 2.0\n")
	_write(child_gd, "extends \"res://editor_lookup_base.gd\"\n")
	_write(script_path, "extends Node\n\nconst Hit = preload(\"editor_lookup_hit.pet\")\nconst Child = preload(\"editor_lookup_child.gd\")\n\nfunc f(hit: Hit, child: Child) -> float:\n\treturn hit.damage + child.speed + Child.LIMIT\n")
	var script: Script = load(script_path)
	var script_editor := EditorInterface.get_script_editor()
	var click := func(symbol: String) -> CodeEdit:
		EditorInterface.edit_script(script)
		var code: CodeEdit = script_editor.get_current_editor().get_base_editor()
		code.symbol_lookup.emit(symbol, 6, code.get_line(6).find(symbol))
		return script_editor.get_current_editor().get_base_editor()
	var reached := click.call("damage")
	_check(script_editor.get_current_script().resource_path == hit_path and reached.get_caret_line() == 2, "Ctrl+click on a member goes to its declaration in its file")
	reached = click.call("speed")
	_check(script_editor.get_current_script().resource_path == base_gd and reached.get_caret_line() == 3, "Ctrl+click on an inherited GDScript member goes to the base script")
	reached = click.call("LIMIT")
	_check(script_editor.get_current_script().resource_path == base_gd and reached.get_caret_line() == 2, "Ctrl+click on an inherited GDScript constant goes to the base script")
	reached = click.call("hit")
	_check(script_editor.get_current_script() == script and reached.get_caret_line() == 5, "Ctrl+click on a parameter goes to its declaration")
	EditorInterface.edit_script(load(hit_path))
	var hit_code: CodeEdit = script_editor.get_current_editor().get_base_editor()
	hit_code.insert_text("# Moved down.\n", 0, 0)
	reached = click.call("damage")
	_check(reached == hit_code and reached.get_caret_line() == 3, "Ctrl+click goes to the declaration's line in its unsaved text")
	hit_code.undo()
	_check_errors([], "looking up reports nothing")
	for path in [hit_path, base_gd, child_gd, script_path]:
		for file in [path, path + ".uid", path.replace("res://", "res://.godot/petrichor/").replace(".pet", ".petc")]:
			DirAccess.remove_absolute(file)
	_scanned()


# Spec §13.2: completion offers what checks at the cursor, which the script
# editor asks the language for (editor/gui/code_editor.cpp:1028-1055).
func _check_completion() -> void:
	var hit_path := "res://editor_complete_hit.pet"
	var script_path := "res://editor_complete.pet"
	_write(hit_path, "extends RefCounted\n\npub var damage := 1\nvar hidden := 2\n\npub func strike(times: int) -> void:\n\tpass\n")
	_write(script_path, "extends Node\n\nconst Hit = preload(\"editor_complete_hit.pet\")\n\nfunc f(hit: Hit) -> void:\n\tprint(hit.damage)\n\tvar r := load(\"res://\")\n\tprint(Input.is_action_pressed(\"ui_\"))\n")
	EditorInterface.edit_script(load(script_path))
	var code: CodeEdit = EditorInterface.get_script_editor().get_current_editor().get_base_editor()
	var offered := _complete(code, 5, code.get_line(5).find("damage"))
	var shown: bool = offered.has("damage") and offered.damage.kind == CodeEdit.KIND_MEMBER and offered.has("strike(…)") and offered["strike(…)"].insert_text == "strike(" and not offered.has("hidden")
	_check(shown, "completion offers the pub members of a value's class" + ("" if shown else ": " + str(offered.keys())))
	# In a string, which the editor quotes.
	offered = _complete(code, 6, code.get_line(6).find("res://") + 6)
	var listed: bool = offered.has("\"res://main.tscn\"") and offered["\"res://main.tscn\""].kind == CodeEdit.KIND_FILE_PATH
	_check(listed, "completion offers the project's files for a path" + ("" if listed else ": " + str(offered.keys())))
	offered = _complete(code, 7, code.get_line(7).find("ui_") + 3)
	_check(offered.has("\"ui_accept\""), "completion offers the input actions for an action" + ("" if offered.has("\"ui_accept\"") else ": " + str(offered.keys())))
	code.cancel_code_completion()
	_check_errors([], "completing reports nothing")
	for path in [hit_path, script_path]:
		for file in [path, path + ".uid", path.replace("res://", "res://.godot/petrichor/").replace(".pet", ".petc")]:
			DirAccess.remove_absolute(file)
	_scanned()


# The script editor colors a script by the lexer's tokens, in the colors of the
# settings GDScript's highlighter reads. Columns count characters, not bytes,
# and a string in triple quotes goes on to the next line.
func _check_highlighting() -> void:
	var path := "res://editor_highlight.pet"
	_write(path, "extends Node\n\n@export pub var speed := 1.0\nvar title := \"雨后\" + str(1)\nvar poem := \"\"\"雨\n后\"\"\" + \"!\"\n")
	EditorInterface.edit_script(load(path))
	var code: CodeEdit = EditorInterface.get_script_editor().get_current_editor().get_base_editor()
	var shown := func(line: int, column: int, setting: String) -> bool:
		var colors: Dictionary = code.syntax_highlighter.get_line_syntax_highlighting(line)
		var found := Color()
		for from: int in colors:
			if from <= column:
				found = colors[from].color
		return found == EditorInterface.get_editor_settings().get_setting("text_editor/theme/highlighting/" + setting)
	_check(shown.call(2, 1, "gdscript/annotation_color") and shown.call(2, 8, "keyword_color"), "the script editor colors annotations and keywords")
	_check(shown.call(3, 18, "symbol_color") and shown.call(3, 20, "gdscript/global_function_color"), "the script editor colors by characters after Chinese text")
	_check(shown.call(5, 0, "string_color") and shown.call(5, 5, "symbol_color"), "the script editor colors a string in triple quotes over its lines")
	_check_errors([], "highlighting reports nothing")
	for file in [path, path + ".uid", path.replace("res://", "res://.godot/petrichor/").replace(".pet", ".petc")]:
		DirAccess.remove_absolute(file)


# The script editor's method list shows the functions of a script and of its
# inner classes, each going to its line, as the language lists them
# (editor/script/script_editor_plugin.cpp:1762-1800; ed1daf0). Godot 4.8
# shows them in a tree, an inner class's under its name.
func _check_members_overview() -> void:
	var path := "res://editor_members.pet"
	_write(path, "extends Node\n\nfunc f() -> void:\n\tpass\n\nclass Inner:\n\tfunc g() -> void:\n\t\tpass\n")
	EditorInterface.edit_script(load(path))
	await get_tree().process_frame
	var listed := {}
	for list: ItemList in EditorInterface.get_script_editor().find_children("*", "ItemList", true, false):
		for i in list.item_count:
			listed[list.get_item_text(i)] = list.get_item_metadata(i)
	for tree: Tree in EditorInterface.get_script_editor().find_children("*", "Tree", true, false):
		var items: Array[TreeItem] = []
		if tree.is_visible_in_tree() and tree.get_root():
			items.append(tree.get_root())
		while not items.is_empty():
			var item: TreeItem = items.pop_back()
			var parent := item.get_parent()
			listed[(parent.get_text(0) + "." if parent and parent.get_parent() else "") + item.get_text(0)] = item.get_metadata(0)
			items.append_array(item.get_children())
	var shown: bool = listed.get("f") == 2 and listed.get("Inner.g") == 6
	_check(shown, "the method list shows a script's functions at their lines" + ("" if shown else ": " + str(listed)))
	for file in [path, path + ".uid", path.replace("res://", "res://.godot/petrichor/").replace(".pet", ".petc")]:
		DirAccess.remove_absolute(file)


# The options completion offers at a line and column, by what they show. A
# list of paths open still would keep the editor from asking again
# (scene/gui/code_edit.cpp:2417-2438).
func _complete(code: CodeEdit, line: int, column: int) -> Dictionary:
	code.cancel_code_completion()
	code.set_caret_line(line)
	code.set_caret_column(column)
	code.request_code_completion(true)
	var offered := {}
	for option in code.get_code_completion_options():
		offered[option.display_text] = option
	return offered


# Spec §13.2: hover and the help show a class's declarations, also those
# without pub, and their ## comments (§2.2), from the documentation it gives
# the editor, which the script editor takes on saving
# (editor/script/script_editor_plugin.cpp:3449-3456).
func _check_documentation() -> void:
	var path := "res://editor_doc.pet"
	_write(path, "## Waits for its target.\nextends Node\n\n## The node it [b]follows[/b].\npub var target: Node3D?\nvar hidden := 1\n\npub async func wait(seconds: float = 1.0) -> int:\n\treturn 1\n")
	var script: Script = load(path)
	var script_editor := EditorInterface.get_script_editor()
	script_editor.update_docs_from_script(script)
	script_editor.goto_help("class_name:\"editor_doc.pet\"")
	await get_tree().process_frame
	var pages := script_editor.find_children("*", "EditorHelp", true, false)
	var text: String = pages[-1].find_children("*", "RichTextLabel", true, false)[0].get_parsed_text().replace(char(0xA0), " ") if pages else ""
	var shown := ["Waits for its target.", "Node3D?", "The node it follows.", "hidden", "seconds: float = 1.0", "async"].all(func(part): return text.contains(part))
	_check(shown, "the help of a class shows its members as declared, with their comments" + ("" if shown else ": " + text))
	_check_errors([], "documenting reports nothing")
	for file in [path, path + ".uid"]:
		DirAccess.remove_absolute(file)


# A user attaches a new script to a node through the scene dock's dialog, and
# picks this language (editor/docks/scene_tree_dock.cpp:2680-2713, 4331-4363;
# editor/script/script_create_dialog.cpp:186-196, 368-401, 416-437; ed1daf0).
func _check_script_dialog() -> void:
	var scene_path := "res://editor_dialog.tscn"
	var script_path := "res://player.pet"
	_write(scene_path, '[gd_scene format=3]\n\n[node name="Dialog" type="Node"]\n\n[node name="Player" type="Node2D" parent="."]\n')
	EditorInterface.open_scene_from_path(scene_path)
	await get_tree().process_frame
	var player := EditorInterface.get_edited_scene_root().get_node("Player")
	EditorInterface.get_selection().clear()
	EditorInterface.get_selection().add_node(player)
	await get_tree().process_frame
	var dock: Node = EditorInterface.get_base_control().find_children("*", "SceneTreeDock", true, false)[0]
	var attach: Button = dock.find_children("*", "Button", true, false).filter(func(b): return b.tooltip_text == "Attach a new or existing script to the selected node.")[0]
	attach.pressed.emit()
	var dialog := get_script_create_dialog()
	var languages: OptionButton = dialog.find_children("*", "OptionButton", true, false)[0]
	var language := range(languages.item_count).filter(func(i): return languages.get_item_text(i) == "Petrichor")
	_check(attach.visible and dialog.visible and language.size() == 1, "Attach Script offers this language")
	languages.select(language[0])
	languages.item_selected.emit(language[0])
	await get_tree().process_frame
	var path: LineEdit = dialog.find_children("*", "LineEdit", true, false).filter(func(e): return e.text.begins_with("res://"))[0]
	_check(path.text == script_path and not dialog.get_ok_button().disabled, "the dialog proposes a .pet file named after the node, which it accepts")
	dialog.get_ok_button().pressed.emit()
	var script: Script = load(script_path)
	_check(FileAccess.get_file_as_string(script_path) == "extends Node2D\n" and script.get_class() == "PetScript" and player.get_script() == script, "the file has the language's template, loads as its script, and the node has it")
	EditorInterface.close_scene()
	for file in [scene_path, script_path, script_path + ".uid", "res://.godot/petrichor/player.petc"]:
		DirAccess.remove_absolute(file)


# Spec §11.1: an autoload's name has the class of its scene's root. The editor
# adds an autoload by its uid (editor/settings/editor_autoload_settings.cpp:787;
# ed1daf0).
func _check_autoload_by_uid() -> void:
	var path := "res://editor_autoload.pet"
	_write(path, "extends Node\n\nfunc f() -> String:\n\treturn EditorAutoload.greet()\n")
	ProjectSettings.set_setting("autoload/EditorAutoload", "*" + ResourceUID.path_to_uid("res://main.tscn"))
	_check(_compiles(ResourceLoader.load(path, "", ResourceLoader.CACHE_MODE_IGNORE), path), "an autoload added by its uid has the class of its scene's root")
	ProjectSettings.set_setting("autoload/EditorAutoload", null)
	for file in [path, path + ".uid", "res://.godot/petrichor/editor_autoload.petc"]:
		DirAccess.remove_absolute(file)


# Spec §13.3 in the editor (architecture §2.3). A rename that cannot be made
# changes nothing. A local renamed in one script is one step of that script's
# undo. A member's rename reads the open script's text and the open scene's
# unsaved change, which it saves before rewriting the scene. The scripts stay
# unsaved: the open one changes in its tab, whose history stays, and a closed
# one is held, which the editor's save writes byte for byte, and a loaded one
# reloads then. Ctrl+Z in the script, with the rename's edit last there,
# undoes it everywhere, and Ctrl+Shift+Z redoes it. A closed script opened
# shows its text held, unsaved. An undo or redo that would be refused is not
# made, and no history moves.
func _check_rename() -> void:
	var dir := "res://editor_rename"
	var unit := dir + "/unit.pet"
	var user := dir + "/user.pet"
	var scene := dir + "/unit.tscn"
	DirAccess.make_dir_absolute(dir)
	_write(unit, "extends Node\n\n@export pub var stride := 1\nvar health := 3\n\nfunc twice() -> int:\n\tvar count := stride\n\treturn count * 2\n")
	var user_text := "﻿extends Node\r\n\r\nconst Unit = preload(\"unit.pet\")\r\n\r\nfunc f(unit: Unit) -> int:\r\n\treturn unit.stride\r\n"
	FileAccess.open(user, FileAccess.WRITE).store_buffer(user_text.to_utf8_buffer())
	_write(scene, '[gd_scene format=3]\n\n[ext_resource type="Script" path="%s" id="1"]\n\n[node name="Unit" type="Node"]\nscript = ExtResource("1")\nstride = 5\n' % unit)
	EditorInterface.open_scene_from_path(scene)
	var root := EditorInterface.get_edited_scene_root()
	var undo_redo := get_undo_redo()
	undo_redo.create_action("Set stride")
	undo_redo.add_do_property(root, "stride", 7)
	undo_redo.add_undo_property(root, "stride", 5)
	undo_redo.commit_action()
	EditorInterface.edit_script(load(unit))
	var code: CodeEdit = EditorInterface.get_script_editor().get_current_editor().get_base_editor()
	var text := code.text
	var scene_text := FileAccess.get_file_as_string(scene)
	var global := undo_redo.get_history_undo_redo(EditorUndoRedoManager.GLOBAL_HISTORY)
	var actions := global.get_history_count()

	_rename(code, 2, 17, "health")
	var refused := _references_tree().get_root().get_child(0).get_text(0) if _references_tree().get_root().get_child_count() > 0 else ""
	_check(refused.begins_with(unit) and code.text == text and FileAccess.get_file_as_string(scene) == scene_text and EditorInterface.get_unsaved_scenes().has(scene), "a rename that cannot be made shows why and changes nothing: " + refused)

	_rename(code, 6, 7, "total")
	_check(code.text == text.replace("count", "total") and global.get_history_count() == actions, "a local is renamed in its script, outside the editor's history")
	code.undo()
	_check(code.text == text, "one undo in the script restores it")

	var loaded_user := load(user)
	code.insert_text("# Kept.\n", code.get_line_count() - 1, 0)
	var kept := code.text
	_rename(code, 2, 17, "pace")
	var apply: Button = _references_tree().get_parent().find_children("*", "Button", true, false).filter(func(b): return b.text == "Apply")[0]
	apply.pressed.emit()
	_check(code.text == kept.replace("stride", "pace") and FileAccess.get_file_as_string(scene).contains("\npace = 7\n"), "a member is renamed in the open script and in the open scene, with its unsaved change")
	_check(FileAccess.get_file_as_string(unit) == kept and FileAccess.get_file_as_bytes(user) == user_text.to_utf8_buffer() and not loaded_user.source_code.contains("pace"), "the scripts stay unsaved, and a loaded one keeps its text")
	await _press(code, "ui_undo")
	_check(code.text == kept and FileAccess.get_file_as_string(scene).contains("\nstride = 7\n"), "Ctrl+Z in the script, with the rename's edit last there, restores the script and the scene")
	await _press(code, "ui_redo")
	_check(code.text == kept.replace("stride", "pace") and FileAccess.get_file_as_string(scene).contains("\npace = 7\n"), "Ctrl+Shift+Z there renames them again")
	EditorInterface.save_scene()
	_check(FileAccess.get_file_as_bytes(user) == user_text.replace("stride", "pace").to_utf8_buffer() and loaded_user.source_code.contains("pace"), "saving the scene writes the closed script with its byte order mark and line breaks, and the loaded one reloads")

	await _press(code, "ui_undo")
	EditorInterface.edit_script(load(user))
	var user_code: CodeEdit = EditorInterface.get_script_editor().get_current_editor().get_base_editor()
	var listed := []
	for names: ItemList in EditorInterface.get_script_editor().find_children("*", "ItemList", true, false):
		listed.append_array(range(names.item_count).map(func(i): return names.get_item_text(i)))
	_check(user_code.text == user_text.replace("\r", "").trim_prefix("﻿") and listed.has("user.pet(*)"), "a closed script opened shows the text the undo left it, unsaved: %s" % [listed])
	var unrenamed := FileAccess.get_file_as_string(scene)
	_write(scene, unrenamed + "pace = 1\n")
	_scanned()
	var at := global.get_current_action()
	await _press(user_code, "ui_redo")
	_check(global.get_current_action() == at and code.text == kept and user_code.text.contains("unit.stride"), "a redo that would be refused is not made, and the history stays")
	_write(scene, unrenamed)
	_scanned()
	await _press(user_code, "ui_redo")
	_check(code.text == kept.replace("stride", "pace") and user_code.text.contains("unit.pace"), "with the scene fixed, it is made")
	while code.has_undo():
		code.undo()
	_check(code.text == text, "the open script's own history is kept through it")
	_check_errors([], "renaming reports nothing")

	EditorInterface.close_scene()
	for file in [unit, user]:
		EditorInterface.get_script_editor().close_file(file)
	for file in DirAccess.get_files_at(dir):
		DirAccess.remove_absolute(dir.path_join(file))
	DirAccess.remove_absolute(dir)


# Architecture §6.4: a scene that a scene file embeds is read as a scene of its
# own, so a rename finds the value it stores, and refuses, naming where the
# file holds that scene.
func _check_embedded_scene() -> void:
	var dir := "res://editor_embedded"
	var bomb := dir + "/bomb.pet"
	var spawner := dir + "/spawner.pet"
	var level := dir + "/level.tscn"
	DirAccess.make_dir_absolute(dir)
	_write(bomb, "extends Node2D\n\n@export pub var timer := 3.0\n")
	_write(spawner, "extends Node\n\n@export pub var bomb: PackedScene[Node2D]\n")
	var inner := Node2D.new()
	inner.name = "Bomb"
	inner.set_script(load(bomb))
	inner.set("timer", 1.0)
	var outer := Node.new()
	outer.name = "Level"
	outer.set_script(load(spawner))
	var packed := PackedScene.new()
	packed.pack(inner)
	outer.set("bomb", packed)
	var saved := PackedScene.new()
	saved.pack(outer)
	ResourceSaver.save(saved, level)
	inner.free()
	outer.free()
	EditorInterface.get_resource_filesystem().update_file(level)
	await get_tree().process_frame
	EditorInterface.edit_script(load(bomb))
	var code: CodeEdit = EditorInterface.get_script_editor().get_current_editor().get_base_editor()
	_rename(code, 2, 17, "fuse")
	var refused := _references_tree().get_root().get_child(0).get_text(0) if _references_tree().get_root().get_child_count() > 0 else ""
	_check(refused.begins_with(level + ": It is stored in the scene embedded in \"bomb\" of the root,") and code.text.contains("timer"), "a rename refuses a value that an embedded scene stores, naming the file, the node and the property: " + refused)
	_check_errors([], "an embedded scene reports nothing")
	EditorInterface.get_script_editor().close_file(bomb)
	for file in DirAccess.get_files_at(dir):
		DirAccess.remove_absolute(dir.path_join(file))
	DirAccess.remove_absolute(dir)
	_scanned()


# Architecture §2.3: before a rename rewrites a scene, an open scene that
# instances it is saved, and after, reloaded, so that it shows the scene as
# rewritten. The tab shown before shows again, a new scene without a path too.
# The rewritten scene is cached, and the saved scene has a node selected.
func _check_rename_open_scenes() -> void:
	var dir := "res://editor_rename_open"
	var unit := dir + "/unit.pet"
	var scene := dir + "/unit.tscn"
	var holder := dir + "/holder.tscn"
	DirAccess.make_dir_absolute(dir)
	_write(unit, "extends Node\n\n@export pub var stride := 1\n")
	_write(scene, '[gd_scene format=3]\n\n[ext_resource type="Script" path="%s" id="1"]\n\n[node name="Unit" type="Node"]\nscript = ExtResource("1")\nstride = 5\n' % unit)
	_write(holder, '[gd_scene format=3]\n\n[ext_resource type="PackedScene" path="%s" id="1"]\n\n[node name="Holder" type="Node"]\n\n[node name="Unit" parent="." instance=ExtResource("1")]\n' % scene)
	for file in [scene, holder]:
		EditorInterface.get_resource_filesystem().update_file(file)
	await get_tree().process_frame
	var _held: PackedScene = load(scene)
	EditorInterface.open_scene_from_path(holder)
	var holder_root := EditorInterface.get_edited_scene_root()
	var holder_id := holder_root.get_instance_id()
	var undo_redo := get_undo_redo()
	undo_redo.create_action("Describe holder")
	undo_redo.add_do_property(holder_root, "editor_description", "Kept")
	undo_redo.add_undo_property(holder_root, "editor_description", "")
	undo_redo.commit_action()
	EditorInterface.get_selection().add_node(holder_root.get_node("Unit"))
	await get_tree().process_frame
	var tabs: Node = EditorInterface.get_base_control().find_children("*", "EditorSceneTabs", true, false)[0]
	tabs.find_children("*", "TabBar", true, false)[0].find_children("*", "Button", false, false)[0].pressed.emit()
	await get_tree().process_frame
	EditorInterface.get_base_control().find_children("*", "Button", true, false).filter(func(b): return b.text == "2D Scene" and b.is_visible_in_tree())[0].pressed.emit()
	var unnamed := EditorInterface.get_edited_scene_root()
	await get_tree().process_frame
	EditorInterface.edit_script(load(unit))
	var code: CodeEdit = EditorInterface.get_script_editor().get_current_editor().get_base_editor()
	_rename(code, 2, 17, "pace")
	var apply: Button = _references_tree().get_parent().find_children("*", "Button", true, false).filter(func(b): return b.text == "Apply")[0]
	apply.pressed.emit()
	await get_tree().process_frame
	await get_tree().process_frame
	var holders := EditorInterface.get_open_scene_roots().filter(func(r): return r.scene_file_path == holder)
	_check(FileAccess.get_file_as_string(scene).contains("\npace = 5\n") and not EditorInterface.get_unsaved_scenes().has(holder) and FileAccess.get_file_as_string(holder).contains("Kept") and holders.size() == 1 and holders[0].get_instance_id() != holder_id, "an open scene instancing a rewritten one is saved first and reloaded")
	_check(unnamed.scene_file_path.is_empty() and EditorInterface.get_edited_scene_root() == unnamed, "the tab shown before, a scene without a path, shows again")
	EditorInterface.close_scene()
	EditorInterface.open_scene_from_path(holder)
	EditorInterface.close_scene()
	code.undo()
	EditorInterface.get_script_editor().close_file(unit)
	for file in DirAccess.get_files_at(dir):
		DirAccess.remove_absolute(dir.path_join(file))
	DirAccess.remove_absolute(dir)
	_scanned()


# Architecture §2.3: a refactoring writes the scene files it changes all or
# none. One that cannot be written leaves the others and the scripts as they
# were. Saved safely, as the editor saves by default, the write would fail
# only after a second of retries.
func _check_rename_transaction() -> void:
	var dir := "res://editor_transaction"
	var unit := dir + "/unit.pet"
	var scenes := [dir + "/a.tscn", dir + "/b.tscn"]
	DirAccess.make_dir_absolute(dir)
	_write(unit, "extends Node\n\n@export pub var stride := 1\n")
	for scene in scenes:
		_write(scene, '[gd_scene format=3]\n\n[ext_resource type="Script" path="%s" id="1"]\n\n[node name="Unit" type="Node"]\nscript = ExtResource("1")\nstride = 5\n' % unit)
		EditorInterface.get_resource_filesystem().update_file(scene)
	await get_tree().process_frame
	var scene_text := FileAccess.get_file_as_string(scenes[0])
	FileAccess.set_read_only_attribute(scenes[1], true)
	OS.set_use_file_access_save_and_swap(false)
	EditorInterface.edit_script(load(unit))
	var code: CodeEdit = EditorInterface.get_script_editor().get_current_editor().get_base_editor()
	var text := code.text
	_rename(code, 2, 17, "pace")
	var apply: Button = _references_tree().get_parent().find_children("*", "Button", true, false).filter(func(b): return b.text == "Apply")[0]
	apply.pressed.emit()
	_check(code.text == text and FileAccess.get_file_as_string(scenes[0]) == scene_text, "a rename that cannot write one scene file changes no file and no script")
	_check_errors(["Cannot write %s." % scenes[1]], "it reports the file")
	OS.set_use_file_access_save_and_swap(true)
	FileAccess.set_read_only_attribute(scenes[1], false)
	EditorInterface.get_script_editor().close_file(unit)
	for file in DirAccess.get_files_at(dir):
		DirAccess.remove_absolute(dir.path_join(file))
	DirAccess.remove_absolute(dir)
	_scanned()


# Spec §13.2 in the editor (architecture §2.4). Alt+Enter lists the fixes of
# the caret's line. A fix in the script is one step of its undo, and so is
# fixing all of a kind, which leaves the other kinds. A fix that changes
# another script changes its text unsaved, found again where it is now, and
# keeps the tab. Opened, that script shows it, and Ctrl+Z there undoes it.
func _check_quick_fixes() -> void:
	var dir := "res://editor_fixes"
	var target := dir + "/target.pet"
	var user := dir + "/user.pet"
	DirAccess.make_dir_absolute(dir)
	var target_text := "extends Node\n\nvar speed := 1.0\n"
	_write(target, target_text)
	_write(user, "extends Node\n\nconst Target = preload(\"target.pet\")\n\nfunc f(t: Target, hp: int, tag: String) -> bool:\n\tprint(t != null)\n\tif hp:\n\t\treturn not tag\n\treturn t.speed > 0.0\n")
	EditorInterface.edit_script(load(user))
	var code: CodeEdit = EditorInterface.get_script_editor().get_current_editor().get_base_editor()
	var text := code.text
	var fixed := text.replace("if hp:", "if hp != 0:")

	var list := _quick_fixes(code, 6)
	_choose(list, 0)
	_check(code.text == fixed, "the first fix listed is that of the error at the caret")
	code.undo()
	_check(code.text == text, "one undo in the script restores it")

	list = _quick_fixes(code, 6)
	_choose(list, list.item_count - 1)
	_check(code.text == fixed.replace("not tag", "tag == \"\""), "fixing all of a kind applies its fixes in the script and no others")
	code.undo()
	_check(code.text == text, "one undo restores it")

	# The analysis keeps the check of user.pet while the declarations of
	# target.pet stay the same, though they move.
	var moved := target_text.replace("\n\n", "\n\n# Moved down.\n")
	_write(target, moved)
	list = _quick_fixes(code, 8)
	_check(list.item_count == 1, "a kind with one fix in the script has no item to fix all")
	_choose(list, 0)
	_check(FileAccess.get_file_as_string(target) == moved and code.text == text and EditorInterface.get_script_editor().get_current_editor().get_base_editor() == code, "a fix in another script leaves its file and the tab")
	EditorInterface.edit_script(load(target))
	var target_code: CodeEdit = EditorInterface.get_script_editor().get_current_editor().get_base_editor()
	_check(target_code.text == moved.replace("var", "pub var"), "opened, that script shows the fix where its text is now")
	await _press(target_code, "ui_undo")
	_check(target_code.text == moved, "Ctrl+Z in that script undoes it")
	_check_errors([], "quick fixes report nothing")

	for file in [user, target]:
		EditorInterface.get_script_editor().close_file(file)
	for file in DirAccess.get_files_at(dir):
		DirAccess.remove_absolute(dir.path_join(file))
	DirAccess.remove_absolute(dir)


# Architecture §2.1: every check in the editor reads the unsaved texts of the
# open scripts. The script editor shows a scene's error in a script at its
# line there, with its fix (architecture §2.4). A call to a function added
# to another open script has no error and no fix.
func _check_unsaved_texts() -> void:
	var dir := "res://editor_unsaved"
	var hud := dir + "/hud.pet"
	var target := dir + "/target.pet"
	var user := dir + "/user.pet"
	DirAccess.make_dir_absolute(dir)
	_write(hud, "extends Node\n\nfunc f() -> void:\n\tprint($Score)\n")
	_write(dir + "/menu.tscn", '[gd_scene format=3]\n\n[ext_resource type="Script" path="%s" id="1"]\n\n[node name="Menu" type="Node"]\nscript = ExtResource("1")\n\n[node name="Score" type="Label" parent="."]\n' % hud)
	_write(dir + "/main.tscn", '[gd_scene format=3]\n\n[ext_resource type="Script" path="%s" id="1"]\n\n[node name="Main" type="Node"]\n\n[node name="HUD" type="Node" parent="."]\nscript = ExtResource("1")\n' % hud)
	_write(target, "extends Node\n")
	_write(user, "extends Node\n\nconst Target = preload(\"target.pet\")\n\nfunc f(t: Target) -> void:\n\tt.heal()\n")
	for scene in ["menu.tscn", "main.tscn"]:
		EditorInterface.get_resource_filesystem().update_file(dir.path_join(scene))
	await get_tree().process_frame
	var script_editor := EditorInterface.get_script_editor()
	EditorInterface.edit_script(load(hud))
	var code: CodeEdit = script_editor.get_current_editor().get_base_editor()
	code.insert_text("# Moved down.\n", 0, 0)
	var listed := _listed_errors(code)
	_check(RegEx.create_from_string("Line 5:\\s*res://editor_unsaved/main.tscn: HUD has no node").search(listed) != null, "a scene's error in a script is at its line in the unsaved text: " + listed)
	code.undo()
	var fixes := _quick_fixes(code, 3)
	_check(fixes.item_count == 1 and fixes.get_item_text(0) == 'Write get_node_or_null("Score")', "a scene's error in a script lists its fix there")
	fixes.hide()
	EditorInterface.edit_script(load(target))
	var other: CodeEdit = script_editor.get_current_editor().get_base_editor()
	other.insert_text("\npub func heal() -> void:\n\tpass\n", 1, 0)
	EditorInterface.edit_script(load(user))
	code = script_editor.get_current_editor().get_base_editor()
	listed = _listed_errors(code)
	_check(not listed.contains("heal") and _quick_fixes(code, 5).item_count == 0, "a call to a function added to another open script, unsaved, has no error and no fix: " + listed)
	other.undo()
	_check_errors([], "reading unsaved texts reports nothing")
	for file in [hud, target, user]:
		script_editor.close_file(file)
	for file in DirAccess.get_files_at(dir):
		DirAccess.remove_absolute(dir.path_join(file))
	DirAccess.remove_absolute(dir)
	_scanned()


# The errors and warnings the script editor lists once it checks the script.
func _listed_errors(code: CodeEdit) -> String:
	code.get_parent().emit_signal("validate_script")
	return _shown_errors()


# The errors and warnings the script editor lists for the current tab.
func _shown_errors() -> String:
	var listed := ""
	for label: RichTextLabel in EditorInterface.get_script_editor().get_current_editor().find_children("*", "RichTextLabel", true, false):
		listed += label.get_parsed_text() + "\n"
	return listed


# Spec §13.2: an entry of the References panel in a file changed since the
# search is found again, and choosing it goes to where it is now
# (architecture §2.4). Selecting it is what a click does
# (scene/gui/tree.cpp:3006-3074; ed1daf0).
func _check_stale_reference() -> void:
	var dir := "res://editor_stale"
	var target := dir + "/target.pet"
	var user := dir + "/user.pet"
	DirAccess.make_dir_absolute(dir)
	_write(target, "extends Node\n\npub func boost() -> void:\n\tpass\n")
	_write(user, "extends Node\n\nconst Target = preload(\"target.pet\")\n\nfunc f(t: Target) -> void:\n\tt.boost()\n")
	var script_editor := EditorInterface.get_script_editor()
	EditorInterface.set_main_screen_editor("Script")
	EditorInterface.edit_script(load(user))
	var code: CodeEdit = script_editor.get_current_editor().get_base_editor()
	code.set_caret_line(5)
	code.set_caret_column(4)
	code.grab_focus()
	var key := InputEventKey.new()
	key.keycode = KEY_F12
	key.shift_pressed = true
	key.alt_pressed = true
	key.pressed = true
	get_viewport().push_input(key)
	code.insert_text("\n\n", 5, 0)
	var call: TreeItem = null
	for file in _references_tree().get_root().get_children():
		if file.get_text(0) == user:
			call = file.get_child(0)
	_check(call != null, "the References panel lists the call")
	if call:
		_references_tree().set_selected(call, 0)
		_check(is_instance_valid(call), "the panel is not filled again while the Tree selects an item, which it walks after")
		await get_tree().process_frame
		_check(script_editor.get_current_editor().get_base_editor() == code and code.get_caret_line() == 7, "choosing a reference in a file changed since goes to where it is now")
	code.undo()
	_check_errors([], "finding a reference again reports nothing")
	for file in [target, user]:
		script_editor.close_file(file)
	for file in DirAccess.get_files_at(dir):
		DirAccess.remove_absolute(dir.path_join(file))
	DirAccess.remove_absolute(dir)
	_scanned()


# Architecture §2.4: a tab shown again checks its script again when the
# project changed while it was hidden, here by a scene edit on the 2D screen.
func _check_shown_again() -> void:
	var script_path := "res://editor_shown.pet"
	var scene_path := "res://editor_shown.tscn"
	_write(script_path, "extends Node\n\n@onready var label: Label = $Label\n")
	_write(scene_path, '[gd_scene format=3]\n\n[ext_resource type="Script" path="%s" id="1"]\n\n[node name="Shown" type="Node"]\nscript = ExtResource("1")\n' % script_path)
	EditorInterface.open_scene_from_path(scene_path)
	await get_tree().process_frame
	var root := EditorInterface.get_edited_scene_root()
	EditorInterface.set_main_screen_editor("Script")
	EditorInterface.edit_script(load(script_path))
	var code: CodeEdit = EditorInterface.get_script_editor().get_current_editor().get_base_editor()
	_check(_listed_errors(code).contains('has no node at "Label"'), "the tab lists the node that the scene lacks")
	EditorInterface.set_main_screen_editor("2D")
	var plugin: Node = get_tree().root.find_children("*", "PetEditorPlugin", true, false)[0]
	var marks: Timer = plugin.get_children().filter(func(c): return c is Timer)[0]
	marks.wait_time = 0.05
	var label := Label.new()
	label.name = "Label"
	var undo_redo := get_undo_redo()
	undo_redo.create_action("Add Label")
	undo_redo.add_do_method(root, "add_child", label)
	undo_redo.add_do_property(label, "owner", root)
	undo_redo.add_do_reference(label)
	undo_redo.add_undo_method(root, "remove_child", label)
	undo_redo.commit_action()
	await marks.timeout
	EditorInterface.set_main_screen_editor("Script")
	await get_tree().process_frame
	_check(not _shown_errors().contains('has no node at "Label"'), "shown again after the scene gained the node, the tab no longer lists it")
	undo_redo.get_history_undo_redo(undo_redo.get_object_history_id(root)).undo()
	EditorInterface.close_scene()
	EditorInterface.get_script_editor().close_file(script_path)
	for file in [script_path, script_path + ".uid", scene_path, "res://.godot/petrichor/editor_shown.petc"]:
		DirAccess.remove_absolute(file)
	_scanned()


# Spec §13.3: renaming or moving a node in the scene dock writes the paths to
# it anew in the scripts' texts, unsaved: an open one's tab and a closed one's
# held text. The change joins the action's step in the scene's history, and
# the Output panel lists it. It undoes there, as the undo probe of 768bec5 did.
func _check_node_moves() -> void:
	var dir := "res://editor_moves"
	var root_script := dir + "/root.pet"
	var gun_script := dir + "/gun.pet"
	var scene := dir + "/moves.tscn"
	DirAccess.make_dir_absolute(dir)
	_write(root_script, "extends Node2D\n\n@onready var sprite: Node2D = $Sprite\n@onready var gun: Node2D = $Gun\n")
	var gun_text := "extends Node2D\n\n@onready var hud: Node = $\"../HUD\"\n"
	_write(gun_script, gun_text)
	_write(scene, '[gd_scene format=3]\n\n[ext_resource type="Script" path="%s" id="1"]\n[ext_resource type="Script" path="%s" id="2"]\n\n[node name="Moves" type="Node2D"]\nscript = ExtResource("1")\n\n[node name="Sprite" type="Node2D" parent="."]\n\n[node name="Hand" type="Node2D" parent="."]\n\n[node name="Gun" type="Node2D" parent="."]\nscript = ExtResource("2")\n\n[node name="HUD" type="Node" parent="."]\n' % [root_script, gun_script])
	_scanned()
	EditorInterface.open_scene_from_path(scene)
	await get_tree().process_frame
	var root := EditorInterface.get_edited_scene_root()
	EditorInterface.edit_script(load(root_script))
	var script_editor := EditorInterface.get_script_editor()
	var code: CodeEdit = script_editor.get_current_editor().get_base_editor()
	var text := code.text
	var renamed := text.replace("$Sprite", "$Body")
	var undo := get_undo_redo()
	var history := undo.get_history_undo_redo(undo.get_object_history_id(root))
	var printed := Printed.new()
	OS.add_logger(printed)
	var dock: Node = EditorInterface.get_base_control().find_children("*", "SceneTreeDock", true, false)[0]
	var scene_tree_editor: Node = dock.get_tree_editor()
	var tree: Tree = scene_tree_editor.find_children("*", "Tree", true, false)[0]

	# As typing a new name in the dock's tree does.
	var steps := history.get_history_count()
	tree.set_selected(_find_item(tree.get_root(), root.get_node("Sprite")), 0)
	tree.edit_selected(true)
	var line_edit: LineEdit = tree.find_children("*", "LineEdit", true, false)[0]
	line_edit.text = "Body"
	line_edit.text_submitted.emit("Body")
	await get_tree().process_frame
	_check(code.text == renamed and history.get_history_count() == steps + 1, "renaming a node renames it in the open script, in the rename's step")
	_check(printed.lines.has("%s:3 $Sprite → $Body" % root_script), "the Output panel lists the change: %s" % [printed.lines])
	await _scene_step(history.undo)
	_check(root.has_node("Sprite") and code.text == text, "one undo in the scene's history restores the node and the script")
	await _scene_step(history.redo)
	_check(root.has_node("Body") and code.text == renamed, "one redo renames both again")

	# As dropping the node on another in the dock does.
	steps = history.get_history_count()
	EditorInterface.get_selection().clear()
	EditorInterface.get_selection().add_node(root.get_node("Gun"))
	scene_tree_editor.nodes_rearranged.emit([root.get_node("Gun").get_path()], root.get_node("Hand").get_path(), 0)
	await get_tree().process_frame
	_check(code.text == renamed.replace("$Gun", "$Hand/Gun") and history.get_history_count() == steps + 1, "moving a node writes the path to it anew, in the move's step")
	EditorInterface.edit_script(load(gun_script))
	var gun_code: CodeEdit = script_editor.get_current_editor().get_base_editor()
	_check(gun_code.text == gun_text.replace("../HUD", "../../HUD") and FileAccess.get_file_as_string(gun_script) == gun_text, "a closed script holds the path from the node moved, unsaved")
	await _scene_step(history.undo)
	_check(root.get_node("Gun").get_parent() == root and code.text == renamed and gun_code.text == gun_text, "one undo restores the node and both scripts")
	await _scene_step(history.undo)

	# As Reparent to New Node builds its step, which adds the parent; undoing
	# it removes the parent (editor/docks/scene_tree_dock.cpp:3223-3274; ed1daf0).
	var gun := root.get_node("Gun")
	var holder := Node2D.new()
	holder.name = "Holder"
	undo.create_action("Reparent to New Node", UndoRedo.MERGE_DISABLE, root)
	undo.add_do_method(root, "add_child", holder, true)
	undo.add_do_method(holder, "set_owner", root)
	undo.add_do_reference(holder)
	undo.add_do_method(root, "remove_child", gun)
	undo.add_do_method(holder, "add_child", gun, true)
	undo.add_do_method(gun, "set_owner", root)
	undo.add_undo_method(holder, "remove_child", gun)
	undo.add_undo_method(root, "add_child", gun, true)
	undo.add_undo_method(gun, "set_owner", root)
	undo.add_undo_method(root, "remove_child", holder)
	undo.commit_action()
	await get_tree().process_frame
	var held := text.replace("$Gun", "$Holder/Gun")
	_check(code.text == held and gun_code.text == gun_text.replace("../HUD", "../../HUD"), "reparenting to a new node writes the paths anew")
	await _scene_step(history.undo)
	_check(gun.get_parent() == root and code.text == text and gun_code.text == gun_text, "one undo removes the new parent and restores both scripts")
	await _scene_step(history.redo)
	_check(gun.get_parent() == holder and code.text == held, "one redo writes them anew again")
	await _scene_step(history.undo)
	OS.remove_logger(printed)
	_check_errors([], "moving nodes reports nothing")
	EditorInterface.close_scene()
	for file in [root_script, gun_script]:
		script_editor.close_file(file)
	for file in DirAccess.get_files_at(dir):
		DirAccess.remove_absolute(dir.path_join(file))
	DirAccess.remove_absolute(dir)
	_scanned()


# Undoes or redoes a step of the scene's own history, and signals it as the
# editor's undo does (editor/editor_undo_redo_manager.cpp:327, 380; ed1daf0).
# The Edit menu picks the global history or the scene's by timestamps in whole
# seconds, so it could take a global step of the same second (:509-538).
func _scene_step(step: Callable) -> void:
	step.call()
	get_undo_redo().version_changed.emit()
	await get_tree().process_frame


class Printed extends Logger:
	var lines: Array[String] = []

	func _log_message(message: String, _error: bool) -> void:
		lines.append(message.strip_edges(false, true))


# An export added in the script editor, unsaved, reaches the node's
# placeholder, as the script editor gives the script its text after a check
# that passes (editor/script/script_text_editor.cpp:874-878; ed1daf0).
func _check_unsaved_export() -> void:
	var path := "res://editor_unsaved_export.pet"
	var text := "extends Node\n\n@export pub var speed := 1.0\n"
	_write(path, text)
	var script: Script = load(path)
	var node := Node.new()
	node.set_script(script)
	EditorInterface.edit_script(script)
	var code: CodeEdit = EditorInterface.get_script_editor().get_current_editor().get_base_editor()
	code.insert_text("@export pub var armor := 2\n", code.get_line_count() - 1, 0)
	_listed_errors(code)
	var names := node.get_property_list().map(func(p): return p.name)
	_check(names.has("armor") and node.get("armor") == 2 and FileAccess.get_file_as_string(path) == text, "an unsaved export reaches the placeholder: %s" % [names])
	node.free()
	EditorInterface.get_script_editor().close_file(path)
	for file in [path, path + ".uid"]:
		DirAccess.remove_absolute(file)


# Architecture §2.3: a script that a rename left unsaved stays so as the
# editor leaves, which asks every plugin to save (editor/editor_node.cpp:969-991;
# ed1daf0). run.py reads it once the editor has quit.
func _leave_held_script() -> void:
	var dir := "res://editor_quit"
	DirAccess.make_dir_absolute(dir)
	_write(dir + "/unit.pet", "extends Node\n\npub var held_at_quit := 1\n")
	_write(dir + "/user.pet", "extends Node\n\nconst Unit = preload(\"unit.pet\")\n\nfunc f(unit: Unit) -> int:\n\treturn unit.held_at_quit\n")
	EditorInterface.set_main_screen_editor("Script")
	EditorInterface.edit_script(load(dir + "/unit.pet"))
	var code: CodeEdit = EditorInterface.get_script_editor().get_current_editor().get_base_editor()
	# This file has the name too, as a candidate left unchecked.
	_rename(code, 2, 9, "saved_at_quit")
	var apply: Button = _references_tree().get_parent().find_children("*", "Button", true, false).filter(func(b): return b.text == "Apply")[0]
	apply.pressed.emit()
	_check(code.text.contains("saved_at_quit"), "a rename leaves a closed script unsaved before the editor quits")
	EditorInterface.get_script_editor().close_file(dir + "/unit.pet")


# Opens the list of quick fixes at the line of the open script, as Alt+Enter
# does.
func _quick_fixes(code: CodeEdit, line: int) -> PopupMenu:
	code.set_caret_line(line)
	code.set_caret_column(4)
	code.grab_focus()
	var key := InputEventKey.new()
	key.keycode = KEY_ENTER
	key.alt_pressed = true
	key.pressed = true
	get_viewport().push_input(key)
	return get_tree().root.find_children("QuickFixes", "PopupMenu", true, false)[0]


# Chooses the item of the list as a click does, which also closes it.
func _choose(list: PopupMenu, index: int) -> void:
	list.id_pressed.emit(list.get_item_id(index))
	list.hide()


# Renames the name at the line and column of the open script, as F2 and
# entering the new name do.
func _rename(code: CodeEdit, line: int, column: int, name: String) -> void:
	code.set_caret_line(line)
	code.set_caret_column(column)
	code.grab_focus()
	var key := InputEventKey.new()
	key.keycode = KEY_F2
	key.pressed = true
	get_viewport().push_input(key)
	var plugin: Node = get_tree().root.find_children("*", "PetEditorPlugin", true, false)[0]
	var edit: LineEdit = plugin.find_children("*", "PopupPanel", false, false)[0].find_children("*", "LineEdit", true, false)[0]
	edit.text = name
	edit.text_submitted.emit(name)


# Presses the first key of the input action with the focus in the code editor,
# and waits for the frame to end.
func _press(code: CodeEdit, action: String) -> void:
	code.grab_focus()
	var key: InputEventKey = InputMap.action_get_events(action)[0].duplicate()
	key.pressed = true
	get_viewport().push_input(key)
	await get_tree().process_frame


func _references_tree() -> Tree:
	return EditorInterface.get_base_control().find_children("*", "PetReferences", true, false)[0].find_children("*", "Tree", true, false)[0]


# The dock's item for a node. The item names its node by a path: from the
# tree's root in Godot 4.7, and from the scene's root since 4.8
# (editor/scene/scene_tree_editor.cpp:520, 2039-2044; c51ff06e8).
func _find_item(item: TreeItem, node: Node) -> TreeItem:
	if EditorInterface.get_edited_scene_root().get_node_or_null(item.get_metadata(0)) == node:
		return item
	for child in item.get_children():
		var found := _find_item(child, node)
		if found:
			return found
	return null


func _check_inheritance_recovery() -> void:
	var base_path := "res://editor_inherit_base.pet"
	var child_path := "res://editor_inherit_child.pet"
	var valid := "extends Node\n@export pub var speed: int = 1\npub func read() -> int:\n\treturn 1\n"
	_write(base_path, valid)
	_write(child_path, 'extends "editor_inherit_base.pet"\n@export pub var own: int = 2\npub func read() -> int:\n\treturn 7\n')
	var base: Script = load(base_path)
	var child: Script = load(child_path)
	_check(child.reload() == OK, "the derived script initially compiles")
	var node := Node.new()
	node.set_script(child)
	node.set("own", 9)
	base.source_code = valid.replace("-> int:", "-> String:").replace("return 1", 'return "bad"')
	_check(ResourceSaver.save(base, base_path) == OK, "saving a changed base recompiles its child")
	_check(not child.has_script_method("read"), "incompatible override invalidates the child")
	_check_errors(["editor_inherit_child.pet:3: The override widens its base return type."], "the child reports the incompatible override")
	base.source_code = valid
	_check(ResourceSaver.save(base, base_path) == OK, "the repaired base saves")
	_check(child.has_script_method("read") and node.get("speed") == 1 and node.get("own") == 9, "repairing only the base recovers the failed child and its placeholder")
	_check_errors([], "recovering the child introduces no errors")
	node.free()
	for path in [base_path, child_path]:
		for file in [path, path + ".uid", path.replace("res://", "res://.godot/petrichor/").replace(".pet", ".petc")]:
			DirAccess.remove_absolute(file)


# Spec §4.3: the inspector edits an exported struct or enum with payloads as
# the Dictionary that encodes it (§10.1), with the engine's untyped editor,
# and takes its encoded initial value as the default.
func _check_struct_exports() -> void:
	var script_path := "res://editor_check_structs.pet"
	var scene_path := "res://editor_check_structs.tscn"
	_write(script_path, "extends Node\n\npub struct Hit:\n\tvar damage: int = 3\n\tvar crit := false\n\npub enum Order { WAIT, MOVE(target: Vector3) }\n\n@export pub var hit: Hit\n@export pub var order := Order.MOVE(target = Vector3(1, 2, 3))\n@export pub var rolled := Hit(damage = randi())\n")
	_write(scene_path, '[gd_scene format=3]\n\n[ext_resource type="Script" path="%s" id="1"]\n\n[node name="Root" type="Node"]\nscript = ExtResource("1")\n' % script_path)
	var root := (ResourceLoader.load(scene_path, "", ResourceLoader.CACHE_MODE_IGNORE_DEEP) as PackedScene).instantiate(PackedScene.GEN_EDIT_STATE_INSTANCE)
	_check(root.get("hit") == {"damage": 3, "crit": false} and root.get("order") == {"$variant": "MOVE", "target": Vector3(1, 2, 3)} and root.get("rolled") == null, "a placeholder shows the encoded initial value, and none that code computes")
	var hit: Dictionary = root.get_property_list().filter(func(p): return p.name == "hit")[0]
	var editor := EditorInspector.instantiate_property_editor(root, hit.type, hit.name, hit.hint, hit.hint_string, hit.usage)
	editor.set_object_and_property(root, &"hit")
	editor.update_property()
	var shown: String = editor.find_children("*", "Button", true, false)[0].text
	_check(hit.hint_string == "0:editor_check_structs.pet.Hit" and shown == "Dictionary (size 2)", "the hint string names the type, and the inspector edits the value as an untyped Dictionary: %s, %s" % [hit.hint_string, shown])
	editor.free()
	_check(not _pack_to_text(root).contains(" = {"), "a scene stores no value left at its default")
	root.set("hit", {"damage": 4})
	_check(_pack_to_text(root).contains('hit = {\n"damage": 4\n}'), "an edited value is stored as the Dictionary")
	root.free()
	for file in [script_path, script_path + ".uid", scene_path]:
		DirAccess.remove_absolute(file)


# Spec §4.3: the values that scenes store for an exported struct are checked
# by each scene's own check, which marks an error on the node; the script
# compiles. A scene that inherits another replaces the whole value.
func _check_stored_exports() -> void:
	var script_path := "res://editor_check_stored.pet"
	var base := "res://editor_check_stored_base.tscn"
	var derived := "res://editor_check_stored_derived.tscn"
	var inherit := '[gd_scene format=3]\n\n[ext_resource type="PackedScene" path="%s" id="1"]\n\n[node name="Base" instance=ExtResource("1")]\nhit = %s\n'
	_write(script_path, "extends Node\n\npub struct Hit:\n\tvar damage: int\n\tvar crit := false\n\n@export pub var hit := Hit(damage = 1)\n")
	_write(base, '[gd_scene format=3]\n\n[ext_resource type="Script" path="%s" id="1"]\n\n[node name="Base" type="Node"]\nscript = ExtResource("1")\nhit = {"crit": true, "damage": 5}\n' % script_path)
	_write(derived, inherit % [base, '{"crit": false}'])
	var script: Script = ResourceLoader.load(script_path, "", ResourceLoader.CACHE_MODE_IGNORE)
	_check(_compiles(script, script_path), "a script compiles whatever its scenes store")
	var warnings := _scene_marks(derived)
	_check(warnings.contains("the value stored for \"hit\" of . does not decode: editor_check_stored.pet.Hit.damage is missing."), "a scene that inherits another replaces the whole value, here without a field that has no default; the error is marked on the node of the scene that stores it: " + warnings)

	await _write_after(derived, inherit % [base, '{"damage": 7}'], FileAccess.get_modified_time(derived))
	# Saving tells the editor of a change once the frame ends
	# (editor/editor_node.cpp:7839-7841; ed1daf0).
	_compiles(script, script_path)
	await get_tree().process_frame
	warnings = _scene_marks(derived)
	_check(warnings.is_empty(), "a field left out that has a default decodes: " + warnings)
	for file in [script_path, script_path + ".uid", base, derived, "res://.godot/petrichor/editor_check_stored.petc"]:
		DirAccess.remove_absolute(file)


# Spec §7.5: an export that must be assigned gets its value from any scene the
# node is built from, here from the main scene, which changes the node inside
# the scene it instances. The scene dock marks a node without one, also deep
# in the scenes that the main scene instances.
func _check_scene_exports() -> void:
	var script_path := "res://editor_check_exports.pet"
	var inner := "res://editor_check_exports_inner.tscn"
	var level := "res://editor_check_exports_level.tscn"
	var game := "res://editor_check_exports_game.tscn"
	var instance := '[gd_scene format=3]\n\n[ext_resource type="PackedScene" path="%s" id="1"]\n\n[node name="%s" type="Node"]\n\n[node name="%s" parent="." instance=ExtResource("1")]\n'
	_write(script_path, "extends Node\n\n@export pub var target: Node\n")
	_write(inner, '[gd_scene format=3]\n\n[ext_resource type="Script" path="%s" id="1"]\n\n[node name="Inner" type="Node"]\nscript = ExtResource("1")\n' % script_path)
	_write(level, instance % [inner, "Level", "Far"])
	_write(game, instance % [level, "Game", "Level"] + '\n[node name="Far" parent="Level" node_paths=PackedStringArray("target")]\ntarget = NodePath("..")\n\n[node name="Other" parent="." instance=ExtResource("1")]\n')
	var main: String = ProjectSettings.get_setting("application/run/main_scene")
	ProjectSettings.set_setting("application/run/main_scene", game)
	var root := (ResourceLoader.load(game, "", ResourceLoader.CACHE_MODE_IGNORE_DEEP) as PackedScene).instantiate(PackedScene.GEN_EDIT_STATE_INSTANCE)
	var given := "\n".join(root.get_node("Level/Far").call("_get_configuration_warnings"))
	var lacking := "\n".join(root.get_node("Other/Far").call("_get_configuration_warnings"))
	root.free()
	ProjectSettings.set_setting("application/run/main_scene", main)
	_check(given.is_empty(), "a scene gives the value to a node of the scene it instances: " + given)
	_check(lacking == 'Other/Far has no value for "target", an export that must be assigned.', "the main scene's check marks a node deep in the scenes it instances: " + lacking)

	# Spec §7.4: a scene stored for an exported PackedScene[T] leaves to
	# instantiate() only what it asks for.
	var spawner_path := "res://editor_check_spawner.pet"
	var spawn := "res://editor_check_spawner.tscn"
	_write(spawner_path, "extends Node\n\n@export pub var wave: PackedScene[Node]\n")
	_write(spawn, '[gd_scene format=3]\n\n[ext_resource type="Script" path="%s" id="1"]\n[ext_resource type="PackedScene" path="%s" id="2"]\n\n[node name="Spawner" type="Node"]\nscript = ExtResource("1")\nwave = ExtResource("2")\n' % [spawner_path, inner])
	var marks := _scene_marks(spawn)
	_check(marks.contains('"wave" of . holds %s, which leaves "target" to instantiate()' % inner), "the scene stored for an export leaves its root's target to instantiate(): " + marks)
	for file in [script_path, script_path + ".uid", inner, level, game, spawner_path, spawner_path + ".uid", spawn]:
		DirAccess.remove_absolute(file)


# Spec §10.5: the members that a scene's animation tracks and synchronized
# properties name must be pub, as the engine finds them by name. The scene's
# check marks them on the node, and the script still compiles. A library
# stored apart names them too, and a change to it alone reaches the next
# analysis. An animation under a key `anims/` of 3.x names them too.
func _check_named_members() -> void:
	var script_path := "res://editor_check_named.pet"
	var scene_path := "res://editor_check_named.tscn"
	var library := "res://editor_check_named_library.tres"
	var track := 'tracks/0/type = "value"\ntracks/0/path = NodePath(".:%s")\ntracks/0/keys = {"times": PackedFloat32Array(0), "transitions": PackedFloat32Array(1), "update": 0, "values": [1.0]}\n'
	var stored_apart := '[gd_resource type="AnimationLibrary" format=3]\n\n[sub_resource type="Animation" id="1"]\n' + track + '\n[resource]\n_data = {&"blink": SubResource("1")}\n'
	_write(library, stored_apart % "speed")
	_write(script_path, "extends Node2D\n\nvar flash_amount := 0.0\nvar health := 3\nvar speed := 1.0\n\nfunc flash():\n\tpass\n")
	_write(scene_path, '[gd_scene format=3]\n\n[ext_resource type="Script" path="%s" id="1"]\n[ext_resource type="AnimationLibrary" path="%s" id="2"]\n\n[sub_resource type="Animation" id="3"]\n%stracks/1/type = "method"\ntracks/1/path = NodePath(".")\ntracks/1/keys = {"times": PackedFloat32Array(0), "transitions": PackedFloat32Array(1), "values": [{"args": [], "method": &"flash"}]}\n\n[sub_resource type="AnimationLibrary" id="4"]\n_data = {&"hit": SubResource("3")}\n\n[sub_resource type="SceneReplicationConfig" id="5"]\nproperties/0/path = NodePath(".:health")\nproperties/0/spawn = true\nproperties/0/replication_mode = 1\n\n[node name="Root" type="Node2D"]\nscript = ExtResource("1")\n\n[node name="AnimationPlayer" type="AnimationPlayer" parent="."]\nlibraries = {&"": SubResource("4"), &"extra": ExtResource("2")}\n\n[node name="MultiplayerSynchronizer" type="MultiplayerSynchronizer" parent="."]\nreplication_config = SubResource("5")\n\n[node name="Legacy" type="AnimationPlayer" parent="."]\nanims/old = SubResource("3")\n' % [script_path, library, track % "flash_amount"])
	var script: Script = ResourceLoader.load(script_path, "", ResourceLoader.CACHE_MODE_IGNORE)
	_check(_compiles(script, script_path), "a script compiles whatever its scenes name")
	var warnings := _scene_marks(scene_path)
	for named in ['the animation "hit" of AnimationPlayer animates "flash_amount" of the root', 'the animation "hit" of AnimationPlayer calls flash() on the root', 'the animation "extra/blink" of AnimationPlayer animates "speed" of the root', 'MultiplayerSynchronizer synchronizes "health" of the root', 'the animation "old" of Legacy animates "flash_amount" of the root']:
		_check(warnings.contains(named + ", which is not pub"), "the error is marked on the node: " + named)

	await _write_after(library, stored_apart % "flash_amount", FileAccess.get_modified_time(library))
	_write(script_path, "extends Node2D\n\npub var flash_amount := 0.0\npub var health := 3\nvar speed := 1.0\n\npub func flash():\n\tpass\n")
	script = ResourceLoader.load(script_path, "", ResourceLoader.CACHE_MODE_IGNORE)
	# Saving tells the editor of a change once the frame ends
	# (editor/editor_node.cpp:7839-7841; ed1daf0).
	_compiles(script, script_path)
	await get_tree().process_frame
	warnings = _scene_marks(scene_path)
	_check(warnings.is_empty(), "a change to the library alone reaches the next analysis: " + warnings)
	for file in [script_path, script_path + ".uid", scene_path, library, "res://.godot/petrichor/editor_check_named.petc"]:
		DirAccess.remove_absolute(file)
