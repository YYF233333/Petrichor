extends SceneTree

func _initialize() -> void:
	run.call_deferred()

func run() -> void:
	var c = CoroutineCheck.new()
	root.add_child(c)
	var _task = c.start()
	var bridge = CoroutineBridge.new()
	root.add_child(bridge)
	c.emit_wrong(bridge)
	bridge.free()
	var worker = CoroutineCheck.new()
	root.add_child(worker)
	_task = c.start_shared(worker.start())
	print("coroutines debugger: expected error: The awaited object %d was freed or lost its script." % worker.get_instance_id())
	worker.free()
	await process_frame
	await process_frame
	c.free()
	print("coroutines debugger: done")
	quit()
