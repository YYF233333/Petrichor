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
	var warnings := "\n".join(root.call("_get_configuration_warnings"))
	_check(warnings.contains(mark) and not warnings.contains(scene_path), "the node whose script uses $Label is marked, without the scene's own path")
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

	# Modification times count whole seconds.
	var written := FileAccess.get_modified_time(derived)
	while int(Time.get_unix_time_from_system()) <= written:
		await get_tree().process_frame
	_write(derived, inherit % [base, '{"damage": 7}'])
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
# analysis.
func _check_named_members() -> void:
	var script_path := "res://editor_check_named.pet"
	var scene_path := "res://editor_check_named.tscn"
	var library := "res://editor_check_named_library.tres"
	var track := 'tracks/0/type = "value"\ntracks/0/path = NodePath(".:%s")\ntracks/0/keys = {"times": PackedFloat32Array(0), "transitions": PackedFloat32Array(1), "update": 0, "values": [1.0]}\n'
	var stored_apart := '[gd_resource type="AnimationLibrary" format=3]\n\n[sub_resource type="Animation" id="1"]\n' + track + '\n[resource]\n_data = {&"blink": SubResource("1")}\n'
	_write(library, stored_apart % "speed")
	_write(script_path, "extends Node2D\n\nvar flash_amount := 0.0\nvar health := 3\nvar speed := 1.0\n\nfunc flash():\n\tpass\n")
	_write(scene_path, '[gd_scene format=3]\n\n[ext_resource type="Script" path="%s" id="1"]\n[ext_resource type="AnimationLibrary" path="%s" id="2"]\n\n[sub_resource type="Animation" id="3"]\n%stracks/1/type = "method"\ntracks/1/path = NodePath(".")\ntracks/1/keys = {"times": PackedFloat32Array(0), "transitions": PackedFloat32Array(1), "values": [{"args": [], "method": &"flash"}]}\n\n[sub_resource type="AnimationLibrary" id="4"]\n_data = {&"hit": SubResource("3")}\n\n[sub_resource type="SceneReplicationConfig" id="5"]\nproperties/0/path = NodePath(".:health")\nproperties/0/spawn = true\nproperties/0/replication_mode = 1\n\n[node name="Root" type="Node2D"]\nscript = ExtResource("1")\n\n[node name="AnimationPlayer" type="AnimationPlayer" parent="."]\nlibraries = {&"": SubResource("4"), &"extra": ExtResource("2")}\n\n[node name="MultiplayerSynchronizer" type="MultiplayerSynchronizer" parent="."]\nreplication_config = SubResource("5")\n' % [script_path, library, track % "flash_amount"])
	var script: Script = ResourceLoader.load(script_path, "", ResourceLoader.CACHE_MODE_IGNORE)
	_check(_compiles(script, script_path), "a script compiles whatever its scenes name")
	var warnings := _scene_marks(scene_path)
	for named in ['the animation "hit" of AnimationPlayer animates "flash_amount" of the root', 'the animation "hit" of AnimationPlayer calls flash() on the root', 'the animation "extra/blink" of AnimationPlayer animates "speed" of the root', 'MultiplayerSynchronizer synchronizes "health" of the root']:
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
