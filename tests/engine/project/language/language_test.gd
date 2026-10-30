extends Node

# Runs compiled code of the first language subset (architecture §5 step 3),
# then the checks of coroutines, default values and inheritance:
#   godot --headless --path . res://language/language.tscn

const ErrorLog := preload("res://error_log.gd")

var failures := 0
var errors := ErrorLog.new()


func _check(condition: bool, what: String) -> void:
	if condition:
		print("language: ok: ", what)
	else:
		failures += 1
		printerr("language: FAILED: ", what)


# The errors reported since the last call are exactly `expected`, which names
# the program error each call raised (spec §8.1).
func _check_errors(expected: Array[String], what: String) -> void:
	var mismatch := errors.compare(expected, "language")
	_check(mismatch.is_empty(), what if mismatch.is_empty() else "%s: %s" % [what, mismatch])


func _ready() -> void:
	OS.add_logger(errors)
	var c: LanguageCheck = LanguageCheck.new()
	add_child(c)
	_check(c.from_init == 10, "_init() runs after the member initializers")
	_check(c.property_can_revert("offset") and c.property_get_revert("offset") == Vector2(400, 0), "the inspector reverts a member to its constant initializer")
	c.set("items", c.property_get_revert("items"))
	c.get("items").append(3)
	_check(c.get("items") == [1, 2, 3] and c.property_get_revert("items") == [1, 2], "an array a member reverts to is its own")
	_check(c.property_can_revert("level") and c.property_get_revert("level") == 0, "a member without an initializer reverts to its type's default")
	_check(c.add(2, 3) == 5, "int arithmetic")
	_check(c.divide(7, 2) == 3, "int division truncates (spec §5.1)")
	_check(c.scale(3) == 6.0, "an int argument is accepted for a float parameter")
	_check(c.sum_odd_to(6) == 9, "while, if and continue")
	_check(c.count_to(10) == 4 and c.count_to(4) == 3 and c.count_to(-1) == 0, "for over an int, with continue and break")
	_check(c.with_default(1) == 11 and c.with_default(1, 2) == 3 and c.call_with_default() == 11, "default values of parameters, from GDScript and from this language")
	_check(c.sign_of(-3) == -1 and c.sign_of(4) == 1 and c.neither(false, false) and not c.neither(true, false), "conditional expressions, ! and ;")
	var received := []
	c.counted.connect(func(total): received.append(total))
	c.emit_count(5)
	_check(c.has_signal("counted") and received == [5], "a signal of the class, connected from GDScript and emitted")
	_check(c.describe(-1) == "negative" and c.describe(0) == "zero" and c.describe(4) == "positive", "elif chains and early returns")
	_check(c.fib(15) == 610, "recursive calls")
	_check(c.truncate(2.7) == 2 and c.truncate(-2.7) == -2, "int() truncates toward zero")
	_check(c.wrap() < 0, "int arithmetic wraps around")
	_check(c.both(true, false) and not c.both(true, true), "and, or and not")
	var chosen: Array[int] = []
	for args in [[true, false], [false, true], [false, false]]:
		c.choose(args[0], args[1])
		chosen.append(c.chosen)
	_check(chosen == [1, 2, 3], "a void function that ends with if/elif/else returns from each branch")
	_check(c.children() == 0, "engine methods called on self")
	c.add_child(Node.new())
	_check(c.children() == 1, "engine calls see the current state")
	c.speed = 0.5
	_check(c.scale(3) == 1.5, "a member set from GDScript")

	var v: ValuesCheck = ValuesCheck.new()
	var texture := ImageTexture.create_from_image(Image.create(2, 3, false, Image.FORMAT_RGBA8))
	v.texture = texture
	add_child(v)
	var unready: ValuesCheck = ValuesCheck.new()
	add_child(unready)
	_check_errors(["values_check.pet:139: \"texture\" has not been assigned."], "the program error names the export that was not assigned")
	_check(v.readied and not unready.readied, "a node that becomes ready without an export it must be assigned stops before _ready()")
	_check(v.ready_notifications == 1, "_notification() receives the engine's notifications")
	_check(str(v) == "ValuesCheck 1", "str() gives what _to_string() returns")
	var made := ResourceCheck.new()
	_check(made.resource_name == "made" and made.get_reference_count() == 1, "a RefCounted of this language uses self in _init() and is held once")
	var half_made := InitCheck.new()
	_check_errors(["init_check.pet:7: Division by zero."], "the program error of the member initializer")
	_check(not half_made.init_ran, "a program error in a member initializer ends the construction before _init()")
	var ordered := OrderCheck.new()
	_check(ordered.seen == 0 and ordered.later == 9 and ordered.written == 1 and ordered.overwritten == 3, "member initializers run in declaration order, literals among them")
	unready.queue_free()
	_check(v.scaled(2) == Vector3(2, 4, 6), "Vector3 * float through the engine's operator")
	_check(is_equal_approx(v.length_of(3, 4), 5.0), "a constructor and a method of a built-in type")
	_check(v.up_plus(Vector3(1, 0, 0)) == Vector3(1, 1, 0), "a constant of a built-in type")
	_check(v.negated() == Vector3(-1, -2, -3), "a unary operator of a built-in type")
	v.position = Vector3(0, 1, 0)
	v.lift(0.5)
	_check(v.position == Vector3(0, 1.5, 0), "position.y += writes the property back")
	_check(v.field_write() == Vector3(1, 5, 2), "members of a local value")
	_check(v.interpolate(0.5) == Vector3(0.5, 1, 1.5), "lerp typed by its arguments (spec §10.3)")
	_check(v.clamp_int(15) == 10 and v.clamp_int(-1) == 0, "clamp of int stays int")
	_check(v.cosine(0) == 1.0, "a typed utility function")
	var rng := RandomNumberGenerator.new()
	rng.seed = 42
	_check(v.seeded(42) == rng.randf_range(0.0, 1.0), "an object member made with new(), its property and its method")
	_check(v.same(Vector3.ONE, Vector3.ONE) and not v.same(Vector3.ONE, Vector3.ZERO), "== of a built-in type")
	_check(v.node_name() == v.name, "a StringName property")
	v.name = &"petrichor_leak_probe"
	_check(v.read_name(5) == 5, "a slot overwritten in a loop")
	_check(v.maybe(null) == 1 and v.maybe(v) == 1, "a T? parameter accepts null and an object")
	# The engine returns a null object as an OBJECT Variant, not as null.
	var no_node = get_node_or_null("missing")
	# A failed entry check is a program error, and the caller gets the default
	# value (spec §8.1, §10.1).
	var refused = v.callv("takes_node", [no_node])
	_check_errors(["values_check.pet:67: Argument \"n\" of takes_node(): expected Node3D, found None."], "the program error names the parameter, its type and what it got")
	_check(typeof(no_node) == TYPE_OBJECT and refused == 0 and v.takes_node(v) == 1, "a T parameter refuses a null object from the engine")
	var unwrapped_none = v.unwrapped(null)
	_check_errors(["values_check.pet:72: unwrap() found None."], "the program error of unwrap()")
	var wrong_type = v.callv("unwrapped", ["4"])
	_check_errors(["values_check.pet:71: Argument \"x\" of unwrapped(): expected int?, found String."], "the program error of a String for an int? parameter")
	_check(v.unwrapped(4) == 4 and unwrapped_none == 0 and wrong_type == 0, "an int? parameter takes an int or null, and unwrap() of None is a program error")
	_check(v.optional_name(true) == "petrichor" and v.optional_name(false) == null, "a String? result is a String or null")
	var plain := Node.new()
	var freed := Node3D.new()
	freed.free()
	_check(v.as_node3d(v) == v and v.as_node3d(plain) == null and v.callv("as_node3d", [freed]) == null, "as gives None for another class and for a freed object")
	plain.free()

	var ints: Array[int] = [1, 2, 3]
	var floats: Array[float] = [1.0]
	var untyped_refused = v.callv("array_sum", [[1, 2]])
	var other_type_refused = v.callv("array_sum", [floats])
	_check_errors(["values_check.pet:87: Argument \"values\" of array_sum(): expected Array[int], found Array.", "values_check.pet:87: Argument \"values\" of array_sum(): expected Array[int], found Array[float]."], "the program errors give the array types")
	_check(v.array_sum(ints) == 6 and untyped_refused == 0 and other_type_refused == 0, "an Array[int] parameter takes only an array typed with int")
	var named: Array[Node] = [v, c]
	_check(v.names_of(named) == "%s%s" % [v.name, c.name], "for over an Array[Node]")
	named.append(null)
	var with_null = v.names_of(named)
	_check_errors(["values_check.pet:96: Expected Node, found None."], "the program error of the null element")
	_check(with_null == "", "a null element of Array[Node] is a program error when it is read")
	_check(v.queue_demo() == "a1", "pop_front() of Array[String] gives a String?")
	var out_of_bounds = v.element(ints, 3)
	var no_part = v.part("a", 2)
	_check_errors(["values_check.pet:107: Index 3 is out of bounds (size 3).", "values_check.pet:113: Index 2 is out of bounds (size 1)."], "the program errors give the index and the size")
	_check(v.element(ints, 0) == 1 and v.element(ints, -1) == 3 and out_of_bounds == 0, "an index out of bounds is a program error; a negative one counts from the end")
	var holder := Node.new()
	holder.add_child(Node.new())
	var area := Area2D.new()
	var layered := Node2D.new()
	var past_int32 = v.child_at(holder, 2147483648)
	v.set_layer(area, -1)
	v.set_z(layered, -2147483649)
	_check_errors(["values_check.pet:246: 2147483648 is out of range for the engine's int32.", "values_check.pet:252: -1 is out of range for the engine's uint32.", "values_check.pet:249: -2147483649 is out of range for the engine's int32."], "the program errors give the value and the engine's integer type")
	_check(past_int32 == null and v.child_at(holder, 0) == holder.get_child(0), "an int beyond int32 passed to an int32 parameter is a program error")
	_check(area.collision_layer == 1 and layered.z_index == 0, "a value out of range written to a uint32 or an int32 property is a program error and leaves it unchanged")
	v.set_layer(area, 4294967295)
	v.set_z(layered, 7)
	_check(area.collision_layer == 4294967295 and layered.z_index == 7, "values within range reach the engine")
	for node in [holder, area, layered]:
		node.free()
	var to_pop: Array[int] = [1, 2, 3]
	var empty_ints: Array[int] = []
	var last: int = v.popped(to_pop, -1)
	var past_end = v.popped(to_pop, 2)
	var from_empty = v.popped(empty_ints, 0)
	_check_errors(["values_check.pet:110: Index 2 is out of bounds (size 2).", "values_check.pet:110: Index 0 is out of bounds (size 0)."], "the program errors of pop_at()")
	_check(last == 3 and past_end == 0 and from_empty == 0 and to_pop == [1, 2], "pop_at() gives T, and an index out of bounds is a program error, also on an empty array")
	_check(v.part("a,b,c", 1) == "b" and no_part == "", "PackedStringArray from split() with its default arguments")
	_check(v.players.is_typed() and v.players.get_typed_class_name() == &"Node3D", "a member of type Array[Node3D] starts as an empty typed array")
	var missing_resource = v.loaded("res://language/missing.tres")
	_check_errors(["Cannot open file 'res://language/missing.tres'.", "Failed loading resource: res://language/missing.tres.", "Error loading resource: 'res://language/missing.tres'."], "a resource that does not exist is the engine's to report")
	_check(v.loaded("res://language/nodes.tscn") is PackedScene and missing_resource == null, "load() with a path known at run time")
	_check(v.pressed() == false, "a method of the Input singleton")
	var listed_properties := {}
	for property in v.get_property_list():
		listed_properties[property.name] = property
	_check(listed_properties.has("Tuning") and listed_properties["Tuning"].usage == PROPERTY_USAGE_SUBGROUP, "@export_subgroup shows a heading in the inspector")
	_check(listed_properties["tuning"].hint == PROPERTY_HINT_RANGE and listed_properties["tuning"].hint_string == "0,10,0.5", "@export_range gives the range hint")
	_check(listed_properties["texture"].hint == PROPERTY_HINT_RESOURCE_TYPE and listed_properties["texture"].hint_string == "Texture2D", "an exported resource is picked from resources")
	var detached := ValuesCheck.new()
	var unassigned_size = detached.texture_size()
	_check_errors(["values_check.pet:142: \"texture\" has not been assigned."], "the program error names the export that is read")
	detached.texture = texture
	_check(unassigned_size == Vector2() and detached.texture_size() == Vector2(2, 3) and v.texture_size() == Vector2(2, 3), "reading an export that was not assigned is a program error")
	var detached_root = detached.tree_root_name()
	# The engine reports the missing tree too, as for GDScript.
	_check_errors(["Parameter \"data.tree\" is null.", "values_check.pet:169: Expected SceneTree, found None."], "the program error of get_tree() outside the tree")
	detached.free()
	_check(v.tree_root_name() == &"root" and detached_root == &"", "get_tree() is T, and a program error outside the tree")
	_check(v.captured() == Input.MOUSE_MODE_CAPTURED, "an integer constant of an engine class")
	var deep = v.recurse(0)
	_check_errors(["values_check.pet:146: Stack overflow: more than 1024 calls deep. Check for infinite recursion."], "the program error of infinite recursion")
	_check(deep == 0, "infinite recursion is a program error, not a crash")
	var gone_node := Node.new()
	gone_node.free()
	_check(v.name_or_dash(null) == "-" and v.name_or_dash(v) == str(v.name) and v.callv("name_or_dash", [gone_node]) == "-", "== None narrows, and a freed object is None in T?")
	var motion := InputEventMouseMotion.new()
	motion.relative = Vector2(3, 0)
	_check(v.relative_x(motion) == 3.0 and v.relative_x(InputEventKey.new()) == 0.0, "is narrows, also on the right of and")
	_check(v.doubled(4) == 8 and v.doubled(null) == -1, "an int? narrowed to int")
	_check(v.incremented(4) == 5 and v.incremented(null) == null, "a compound assignment to a narrowed int?")
	var target := TargetCheck.new()
	var plain_node := Node.new()
	_check(v.hit(target, 2.5) and v.hit(target, 1) and not v.hit(plain_node, 1) and v.taken_by(target) == 3.5, "is with an interface of another script, and its method")
	var listed_targets: Array = v.targets(target)
	_check(listed_targets.get_typed_script() == TargetCheck and v.as_target(target) == target and v.as_target(plain_node) == null, "an array typed with a class of this language, and as to one")
	var refused_target = v.callv("taken_by", [plain_node])
	_check_errors(["values_check.pet:202: Argument \"target\" of taken_by(): expected TargetCheck, found Node."], "the program error names the class of this language by its class_name")
	_check(refused_target == 0.0, "a parameter of a class of this language refuses another object of its engine class")
	var with_target: Array[Object] = [target]
	var with_plain: Array[Object] = [plain_node]
	var refused_interface = v.callv("damage_through", [plain_node, 1.0])
	var refused_array = v.callv("count_damageable", [with_plain])
	_check_errors(["values_check.pet:235: Argument \"target\" of damage_through(): expected CombatCheck.Damageable, found Node.", "values_check.pet:239: Argument \"targets\" of count_damageable(): expected Array[CombatCheck.Damageable], found Node at index 0."], "the program errors name the interface, and the element that does not implement it")
	# Reported at the member, since a write by name, as the remote inspector's,
	# may come from no script (spec §7.5).
	v.set("damageable", plain_node)
	_check_errors(["values_check.pet:233: \"damageable\" was not written: expected CombatCheck.Damageable?, found Node."], "the program error of a write names the member, its type and what came")
	_check(refused_interface == false and v.damage_through(target, 0.0), "a parameter of an interface takes only an object that implements it")
	_check(refused_array == 0 and v.count_damageable(with_target) == 1, "an array of an interface takes only objects that implement it")
	_check(v.damageable == null, "a member of an interface refuses an object that does not implement it")
	v.damageable = target
	_check(v.damageable == target, "a member of an interface takes an object that implements it")
	var other_values := ValuesCheck.new()
	v.through_class(target)
	v.through_interface(target)
	v.through_instance(other_values)
	_check_errors(["target_check.pet:13: Division by zero.", "target_check.pet:13: Division by zero.", "values_check.pet:230: Division by zero."], "each program error is reported where it happened, in the callee")
	_check(v.after_failure == 0, "a program error in a method of another class, of an interface or of another instance ends the caller too")
	# Spec §3.3: self freed or detached while its method runs.
	var lost_script := FreeCheck.new()
	var firing := FreeCheck.new()
	firing.fired.connect(func(): firing.set_script(null))
	var called := FreeCheck.new()
	var read_after_detach = lost_script.detach_and_read()
	var read_after_signal = firing.fire_and_read()
	var read_through_call = v.through_detached(called)
	_check_errors(["free_check.pet:22: self was freed, or lost its script, while this method ran.", "free_check.pet:34: self was freed, or lost its script, while this method ran.", "free_check.pet:22: self was freed, or lost its script, while this method ran."], "the program errors of reading a member of a detached self")
	_check(read_after_detach == 0 and lost_script.get_script() == null, "reading a member after self lost its script is a program error")
	_check(read_after_signal == 0 and firing.get_script() == null, "reading a member after a signal handler took the emitter's script is a program error")
	_check(read_through_call == 0 and v.after_failure == 0, "the same through a call from another class, which it ends too")
	var returning := FreeCheck.new()
	_check(returning.detach_and_return(21) == 42 and returning.get_script() == null, "a method that removes its script and then uses no member or self ends normally")
	for node in [lost_script, firing, called, returning]:
		node.free()
	plain_node.name = &"Plain"
	_check(v.direct_results(target, plain_node) == "[2, 4] [3] 2 true Plain", "a direct call passes a default value and returns typed arrays, options and objects")
	other_values.free()
	target.free()
	plain_node.free()
	var timer: Timer = v.start_timer()
	timer.timeout.emit()
	_check(v.timeouts == 1 and v.last_timer == timer, "a private method connected through a method value, with a bound argument")
	v.stop_listening(timer)
	timer.timeout.emit()
	_check(v.timeouts == 1 and not timer.timeout.has_connections(), "disconnect() finds the connection by an equal method value")
	var listed: Array[float] = v.literal()
	_check(listed == [1.0, 2.5] and listed.get_typed_builtin() == TYPE_FLOAT, "an array literal takes its element type from where it goes")
	v.transform = Transform3D(Basis(), Vector3(7, 0, 0))
	_check(v.origin_x() == 7.0, "a member of a member of a property")

	var zero := c.divide(1, 0)
	_check_errors(["language_check.pet:20: Division by zero."], "the program error of division by zero")
	_check(zero == 0, "after a program error the caller gets the default value (spec §8.1)")

	var nodes: Node = load("res://language/nodes.tscn").instantiate()
	var early = nodes.early_text()
	_check_errors(["nodes_check.pet:16: \"caption_label\" is read before the node is ready."], "the program error names the @onready member")
	add_child(nodes)
	_check(early == "" and nodes.ready_text() == "from the scene" and nodes.early_text() == "from the scene", "@onready members are set when the node is ready, and reading one before is a program error")
	var caption: Label = nodes.get_node("Caption")
	_check(nodes.caption() == "from the scene" and nodes.caption_node() == caption, "$X reads the node the scene has (spec §7.1)")
	nodes.set_caption("set")
	_check(caption.text == "set", "a property written through $X")
	caption.free()
	var impostor := Node.new()
	impostor.name = "Caption"
	nodes.add_child(impostor)
	var replaced = nodes.caption_node()
	impostor.free()
	var gone = nodes.caption_node()
	# The engine reports the missing node too, as for GDScript.
	_check_errors(["nodes_check.pet:25: Expected Label, found Node.", "Node not found: \"Caption\" (relative to \"/root/LanguageTest/NodesCheck\").", "nodes_check.pet:25: Expected Label, found None."], "the program errors give the class $X expects and what it found")
	_check(replaced == null and gone == null, "$X whose node has changed class or is gone is a program error")

	# Spec §3.3: a T? to a freed object reads as None where it leaves the language too.
	var refs := FreedRefCheck.new()
	var held := Node.new()
	refs.hold(held)
	held.free()
	var orphan := Node.new()
	refs.written(orphan)
	_check(typeof(refs.returned()) == TYPE_NIL and typeof(refs.held) == TYPE_NIL and typeof(refs.get("held")) == TYPE_NIL, "a T? to a freed object reaches GDScript as None, returned or read as a member")
	_check(refs.printed() == "<null>", "the engine gets None for it as an argument")
	_check(typeof(refs.listed()[0]) == TYPE_NIL, "and in an array")
	_check(orphan.owner == null, "and written to a property")
	refs.adopted(orphan)
	refs.owned(orphan)
	refs.appended()
	_check_errors(["freed_ref_check.pet:29: The object given to the engine was freed.", "freed_ref_check.pet:34: The object given to the engine was freed.", "freed_ref_check.pet:40: The object given to the engine was freed."], "a freed object in a T is a program error as an engine argument, an engine property and an element of Array[Node]")
	_check(orphan.get_child_count() == 0 and orphan.owner == null, "and the engine does not get it")
	_check(not refs.valid(), "is_instance_valid() of a freed object in a T is false")
	orphan.free()
	refs.free()

	# Spec §11.1: GDScript classes as reflection shows them.
	var view := GdViewCheck.new()
	var base := GdViewBase.new()
	var child := GdViewChild.new()
	_check(view.doubled(base) == 42 and view.doubled(child) == 42, "a method of a GDScript class is called, on an instance of a GDScript subclass too")
	_check(view.faster(base) == 3.0 and base.speed == 3.0, "a member of a GDScript class is read and written")
	var scores := []
	base.scored.connect(func(points: int): scores.append(points))
	view.score(base)
	_check(scores == [7], "a signal of a GDScript class is emitted with its argument")
	base.buddy = child
	_check(view.buddy_is_child(base), "a member typed by a GDScript class holds an instance of a subclass, which is tests")
	_check(view.as_child(child) == child and view.as_child(base) == null and view.as_base(child) == child, "as names GDScript classes, and an instance of a subclass is one of its base")
	view.free()
	base.free()
	child.free()

	var frames_before: int = c.frames
	var freeing := FreeCheck.new()
	freeing.recorder = self
	add_child(freeing)
	await get_tree().process_frame
	await get_tree().process_frame
	_check_errors(["free_check.pet:17: self was freed, or lost its script, while this method ran."], "the program error of reading a member after self was freed")
	_check(c.frames > frames_before, "_process runs every frame")
	_check(not is_instance_valid(freeing) and not has_meta(&"after_free"), "reading a member after self was freed in _process is a program error")

	for test: GDScript in [preload("res://language/coroutines_test.gd"), preload("res://language/defaults_test.gd"), preload("res://language/inheritance_test.gd")]:
		var part: Node = test.new()
		add_child(part)
		failures += await part.run(errors)
		part.queue_free()

	_check_errors([], "no other errors")
	OS.remove_logger(errors)
	print("language: %d failure(s)" % failures)
	get_tree().quit(1 if failures > 0 else 0)
