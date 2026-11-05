@tool
extends EditorPlugin

func _enter_tree() -> void:
	if "--bench-prepare" in OS.get_cmdline_user_args():
		prepare.call_deferred()

func prepare() -> void:
	if EditorInterface.get_resource_filesystem().is_scanning():
		get_tree().process_frame.connect(prepare, CONNECT_ONE_SHOT)
		return
	print("startup: prepared")
	get_tree().quit()
