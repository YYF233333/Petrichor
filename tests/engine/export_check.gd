# Runs an exported pack in a project directory without sources:
#   godot --headless --path <dir> --script export_check.gd -- <pack>
extends SceneTree


func _init() -> void:
	var pack: String = OS.get_cmdline_user_args()[0]
	if not ProjectSettings.load_resource_pack(pack, true):
		printerr("export check: cannot mount ", pack)
		quit(1)
		return
	for source in ["res://greeter.pet", "res://broken_export_check.pet"]:
		if FileAccess.file_exists(source):
			printerr("export check: the source ", source, " was exported")
			quit(1)
			return
	var header := FileAccess.get_file_as_bytes("res://greeter.petc").slice(0, 4).get_string_from_ascii()
	print("export check: greeter.petc header = ", header)
	change_scene_to_file.call_deferred("res://main.tscn")
