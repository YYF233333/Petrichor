extends Node

# Runs compiled code of the first language subset (architecture §5 step 3):
#   godot --headless --path . res://language/language.tscn

var failures := 0


func _check(condition: bool, what: String) -> void:
	if condition:
		print("language: ok: ", what)
	else:
		failures += 1
		printerr("language: FAILED: ", what)


func _ready() -> void:
	var c: LanguageCheck = LanguageCheck.new()
	add_child(c)
	_check(c.from_init == 10, "_init() runs after the member initializers")
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
	Engine.print_error_messages = false
	add_child(unready)
	Engine.print_error_messages = true
	_check(v.readied and not unready.readied, "a node that becomes ready without an export it must be assigned stops before _ready()")
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
	Engine.print_error_messages = false
	var refused = v.callv("takes_node", [no_node])
	Engine.print_error_messages = true
	_check(typeof(no_node) == TYPE_OBJECT and refused == null and v.takes_node(v) == 1, "a T parameter refuses a null object from the engine")
	Engine.print_error_messages = false
	var unwrapped_none = v.unwrapped(null)
	var wrong_type = v.callv("unwrapped", ["4"])
	Engine.print_error_messages = true
	_check(v.unwrapped(4) == 4 and unwrapped_none == 0 and wrong_type == null, "an int? parameter takes an int or null, and unwrap() of None is a program error")
	_check(v.optional_name(true) == "petrichor" and v.optional_name(false) == null, "a String? result is a String or null")
	var plain := Node.new()
	var freed := Node3D.new()
	freed.free()
	_check(v.as_node3d(v) == v and v.as_node3d(plain) == null and v.callv("as_node3d", [freed]) == null, "as gives None for another class and for a freed object")
	plain.free()

	var ints: Array[int] = [1, 2, 3]
	var floats: Array[float] = [1.0]
	Engine.print_error_messages = false
	var untyped_refused = v.callv("array_sum", [[1, 2]])
	var other_type_refused = v.callv("array_sum", [floats])
	Engine.print_error_messages = true
	_check(v.array_sum(ints) == 6 and untyped_refused == null and other_type_refused == null, "an Array[int] parameter takes only an array typed with int")
	var named: Array[Node] = [v, c]
	_check(v.names_of(named) == "%s%s" % [v.name, c.name], "for over an Array[Node]")
	named.append(null)
	Engine.print_error_messages = false
	var with_null = v.names_of(named)
	Engine.print_error_messages = true
	_check(with_null == "", "a null element of Array[Node] is a program error when it is read")
	_check(v.queue_demo() == "a1", "pop_front() of Array[String] gives a String?")
	Engine.print_error_messages = false
	var out_of_bounds = v.element(ints, 3)
	var no_part = v.part("a", 2)
	Engine.print_error_messages = true
	_check(v.element(ints, 0) == 1 and v.element(ints, -1) == 3 and out_of_bounds == 0, "an index out of bounds is a program error; a negative one counts from the end")
	_check(v.part("a,b,c", 1) == "b" and no_part == "", "PackedStringArray from split() with its default arguments")
	_check(v.players.is_typed() and v.players.get_typed_class_name() == &"Node3D", "a member of type Array[Node3D] starts as an empty typed array")
	Engine.print_error_messages = false
	var missing_resource = v.loaded("res://language/missing.tres")
	Engine.print_error_messages = true
	_check(v.loaded("res://language/nodes.tscn") is PackedScene and missing_resource == null, "load() with a path known at run time")
	_check(v.pressed() == false, "a method of the Input singleton")
	var listed_properties := {}
	for property in v.get_property_list():
		listed_properties[property.name] = property
	_check(listed_properties.has("Tuning") and listed_properties["Tuning"].usage == PROPERTY_USAGE_SUBGROUP, "@export_subgroup shows a heading in the inspector")
	_check(listed_properties["tuning"].hint == PROPERTY_HINT_RANGE and listed_properties["tuning"].hint_string == "0,10,0.5", "@export_range gives the range hint")
	_check(listed_properties["texture"].hint == PROPERTY_HINT_RESOURCE_TYPE and listed_properties["texture"].hint_string == "Texture2D", "an exported resource is picked from resources")
	var detached := ValuesCheck.new()
	Engine.print_error_messages = false
	var unassigned_size = detached.texture_size()
	Engine.print_error_messages = true
	detached.texture = texture
	_check(unassigned_size == Vector2() and detached.texture_size() == Vector2(2, 3) and v.texture_size() == Vector2(2, 3), "reading an export that was not assigned is a program error")
	Engine.print_error_messages = false
	var detached_root = detached.tree_root_name()
	Engine.print_error_messages = true
	detached.free()
	_check(v.tree_root_name() == &"root" and detached_root == &"", "get_tree() is T, and a program error outside the tree")
	_check(v.captured() == Input.MOUSE_MODE_CAPTURED, "an integer constant of an engine class")
	var gone_node := Node.new()
	gone_node.free()
	_check(v.name_or_dash(null) == "-" and v.name_or_dash(v) == str(v.name) and v.callv("name_or_dash", [gone_node]) == "-", "== None narrows, and a freed object is None in T?")
	var motion := InputEventMouseMotion.new()
	motion.relative = Vector2(3, 0)
	_check(v.relative_x(motion) == 3.0 and v.relative_x(InputEventKey.new()) == 0.0, "is narrows, also on the right of and")
	_check(v.doubled(4) == 8 and v.doubled(null) == -1, "an int? narrowed to int")
	var target := TargetCheck.new()
	var plain_node := Node.new()
	_check(v.hit(target, 2.5) and v.hit(target, 1) and not v.hit(plain_node, 1) and v.taken_by(target) == 3.5, "is with an interface of another script, and its method")
	var listed_targets: Array = v.targets(target)
	_check(listed_targets.get_typed_script() == TargetCheck and v.as_target(target) == target and v.as_target(plain_node) == null, "an array typed with a class of this language, and as to one")
	Engine.print_error_messages = false
	var refused_target = v.callv("taken_by", [plain_node])
	Engine.print_error_messages = true
	_check(refused_target == null, "a parameter of a class of this language refuses another object of its engine class")
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

	Engine.print_error_messages = false
	var zero := c.divide(1, 0)
	Engine.print_error_messages = true
	_check(zero == 0, "after a program error the caller gets the default value (spec §8.1)")

	var nodes: Node = load("res://language/nodes.tscn").instantiate()
	Engine.print_error_messages = false
	var early = nodes.early_text()
	Engine.print_error_messages = true
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
	Engine.print_error_messages = false
	var replaced = nodes.caption_node()
	impostor.free()
	var gone = nodes.caption_node()
	Engine.print_error_messages = true
	_check(replaced == null and gone == null, "$X whose node has changed class or is gone is a program error")

	var frames_before: int = c.frames
	await get_tree().process_frame
	await get_tree().process_frame
	_check(c.frames > frames_before, "_process runs every frame")

	print("language: %d failure(s)" % failures)
	get_tree().quit(1 if failures > 0 else 0)
