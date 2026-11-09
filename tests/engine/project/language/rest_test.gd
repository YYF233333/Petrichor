extends Node

# Rest parameters (spec §4.6), run by language_test.gd: calls from GDScript,
# within the class, from another class against an override, and through the
# engine as Callables.

var failures := 0

func check(ok: bool, what: String) -> void:
	if not ok:
		failures += 1
		printerr("rest: FAILED: ", what)

func run(errors) -> int:
	var c := RestCheck.new()
	var child := RestChild.new()
	add_child(c)
	add_child(child)
	check(c.local_calls() == ["-", "-a", "-a,b,c", 0, 3], "calls within the class pass none, one or more arguments to the rest parameter")
	check(c.joined("-", "a", "b") == "-a,b" and c.count() == 0, "a call from GDScript passes the arguments after the others")
	check(c.calls_on(c) == ["/a,b,c", 5] and c.calls_on(child) == ["a/b,c", 5], "an override takes the arguments as its own parameters say")
	check(child.plain(5, 1, 2) == 7, "an override adds a rest parameter")
	check(c.through_values() == ["+a,b", 3, "=x,y"], "a method value, a lambda and callv() take the arguments as Callables")
	check(c.connected() == [[2, ["loud"]]], "a method with a rest parameter connected to a signal")
	check(c.bag() == [1, 2, 3] and c.tally() == 16, "new() and a method of a struct pass arguments to rest parameters")
	check(str(c) == "rest 0", "an engine virtual method with a rest parameter")
	var info := {}
	for method in c.get_method_list():
		if method.name == "joined":
			info = method
	check(info.args.size() == 1 and info.flags & METHOD_FLAG_VARARG and c.get_method_argument_count("joined") == 1, "the engine sees the rest parameter as GDScript's")
	check(c.joined("-", "a", 3) == "", "an argument that does not fit gives the default result")
	var expected: Array[String] = ["rest_check.pet:9: Argument \"parts\" of joined(): expected String, found int at index 1."]
	var mismatch: String = errors.compare(expected, "language")
	check(mismatch.is_empty(), "the program error names the rest parameter and the argument: " + mismatch)
	c.free()
	child.free()
	return failures
