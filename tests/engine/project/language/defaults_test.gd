extends Node

# Default values of parameters (spec §4.6), run by language_test.gd: a call
# from GDScript, within the class and from another class each leave them out
# in a way of their own.

var failures := 0

func check(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("defaults: FAILED: ", what)

func collect(value, results: Array) -> void:
	results.append(await value)

func run(_errors) -> int:
	var c := DefaultValues.new()
	add_child(c)
	var caller := DefaultCaller.new()
	var expected := [Color(0.66, 0, 0, 0.66), Color(1, 1, 1, 0.33), Transform2D(0.5, Vector2(-1, 2)), Vector3.UP, null, null, Color.RED, [Vector2(1, 2), Vector2.ZERO]]
	check(c.snapshot() == expected, "GDScript defaults")
	check(c.local_snapshot() == expected, "same class defaults")
	check(caller.snapshot(c) == expected, "cross class defaults")
	check(c.member == expected[2] and c.optional == Color.RED and c.absent == null, "member construction matches defaults")
	for i in 2:
		check(c.append() == 2 and c.local_append() == 2 and caller.append(c) == 2, "fresh typed arrays")
		var nested: Array = caller.arrays(c)
		check(nested == [[1, 2]], "fresh nested arrays")
		nested[0].append(3)
	var explicit: Array[int] = [7]
	check(c.append(explicit) == 2 and explicit == [7, 2], "explicit arrays remain shared")
	for method in c.get_script().get_script_method_list():
		if method.name == "arrays":
			method.default_args[0][0].append(99)
	check(c.arrays() == [[1, 2]], "method metadata cannot mutate defaults")
	var peer_arrays: Array = [c.peers(), c.local_peers(), caller.peers(c), c.member_peers]
	for method in c.get_script().get_script_method_list():
		if method.name == "peers":
			peer_arrays.append(method.default_args[0])
	for peers: Array in peer_arrays:
		check(peers.is_typed() and peers.get_typed_script() == DefaultValues, "self script type survives deferred construction")
		check(peers.is_empty(), "self typed defaults and metadata are independent")
		peers.append(c)
	check(c.peers().is_empty(), "mutations do not affect later self typed defaults")
	for peers: Array in peer_arrays:
		peers.clear()
	var results: Array = []
	collect(c.later(), results)
	collect(c.later(), results)
	collect(c.local_later(), results)
	collect(caller.later(c), results)
	c.tick.emit()
	check(results.size() == 4, "async defaults complete")
	for result in results:
		check(result == [[1, 2], null], "suspended calls keep independent defaults and None")
	c.free()
	return failures
