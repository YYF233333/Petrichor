extends Node

# Inheritance between classes of this language, run by language_test.gd.

var failures := 0

func check(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("inheritance: FAILED: ", what)

func collect(value, results: Array) -> void:
	results.append(await value)

func run(_errors) -> int:
	var scene: PackedScene = load("res://language/inherit_leaf.tscn")
	var node = scene.instantiate()
	check(node.trace == "BCLIJ", "base-to-leaf members then nearest user constructor")
	add_child(node)
	check(node.trace == "BCLIJERSYZ", "inherited connection and onready before explicit ready chain")
	check(node.target == node.get_node("Target"), "inherited non-null export restored")
	check(node.read() == 14 and node.callback().call() == 14, "virtual self calls and inherited method values")
	node.bump()
	check(node.read() == 15, "subclass accesses protected inherited member")
	check(not node.freed_forward(), "super forwarding does not access a freed object")
	var caller := InheritCaller.new()
	check(node.dispatch() == 5 and caller.call_map(node) == 5, "virtual default dispatch and optional native-slot conversion")
	check(node.map(4) == 7 and caller.convert(node, node.target) == node.target, "extra default and parameter/return variance")
	check(node is InheritBase and node is InheritChild and node is InheritLeaf, "engine script ancestry")
	check(caller.cast_node(node) == node and caller.test_node(node), "Pet casts and type tests")
	var typed: Array[InheritBase] = [node]
	check(typed[0] == node, "typed array accepts a derived script")
	var results: Array = []
	collect(caller.wait(node), results)
	var handle = caller.keep(node, node.target)
	var objects: Array = []
	collect(caller.consume(handle), objects)
	node.tick.emit()
	check(node.trace.ends_with("T"), "derived-only callback accepts an inherited self signal")
	check(results == [5], "inherited suspended method resumes with virtual dispatch")
	check(objects == [node.target], "covariant async result crosses a Task entry")
	node.trace = ""
	node.notification(777)
	check(node.trace == "NM", "notification base-to-derived")
	node.trace = ""
	node.notification(777, true)
	check(node.trace == "MN", "notification derived-to-base")
	await get_tree().process_frame
	await get_tree().process_frame
	check(node.frames > 0, "inherited engine virtual enables processing")
	node.free()
	return failures
