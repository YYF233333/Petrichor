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
	c.chosen = 0
	c.choose_if(false)
	var skipped_if := c.chosen
	c.choose_if(true)
	var taken_if := c.chosen
	c.choose_in_loop(false)
	var skipped_loop := c.chosen
	c.choose_in_loop(true)
	_check(skipped_if == 0 and taken_if == 4 and skipped_loop == 4 and c.chosen == 5, "a void function that ends with an if without else, or with a loop, returns either way")
	var literals := c.literals()
	_check(typeof(literals[0]) == TYPE_STRING_NAME and literals[0] == &"idle" and typeof(literals[1]) == TYPE_NODE_PATH and literals[1] == ^"c/d", "&\"…\" is a StringName and ^\"…\" a NodePath")
	_check(typeof(literals[2]) == TYPE_STRING and literals[2] == "a" and typeof(literals[3]) == TYPE_STRING_NAME and literals[3] == &"b", "a string literal is built as the string type it goes to")
	_check(literals.slice(4, 7) == [true, true, 2], "a StringName literal compares with a String, names a method, and converts to NodePath")
	_check(literals[7] == "é😀\\n", "escapes, a line continued in a string, and a raw string")
	_check(c.state_of(&"idle") == "idle" and c.state_of(&"run") == "moving" and c.state_of(&"walk") == "moving" and c.state_of(&"jump") == "other", "match over a StringName")
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
	_check(v.itself() == v, "self is the instance")
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
	var seeded := RandomNumberGenerator.new()
	seeded.seed = 5
	v.raise_z(layered, 2147483647)
	v.set_seed(seeded, -1)
	_check_errors(["values_check.pet:287: 2147483654 is out of range for the engine's int32.", "values_check.pet:290: -1 is out of range for the engine's uint64."], "a compound assignment and a uint64 property are checked too")
	v.raise_z(layered, 1)
	_check(layered.z_index == 8 and seeded.seed == 5, "a value out of range is not written; one within range is")
	var preloaded := v.preloaded()
	_check(preloaded[0] is CompressedTexture2D and preloaded[1] is Gradient and preloaded[2] == "made", "preload() of an imported file, a resource and a resource with a script of this language")
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
	_check(listed_properties["tuning"].hint == PROPERTY_HINT_RANGE and listed_properties["tuning"].hint_string == "0.0,10.0,0.5", "@export_range gives the range hint")
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
	_check(v.asserted(true, true) == 0, "a passing assert does not evaluate its message (spec §8.3)")
	v.asserted(false, true)
	v.asserted(false, false)
	_check_errors(["values_check.pet:306: Assertion failed: not ok", "values_check.pet:307: Assertion failed."], "a failed assert is a program error in debug builds")
	_check(v.messages == 1, "a failed assert evaluates its message")
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
	var placed := TargetCheck.new()
	_check(v.shift(placed, Vector2(2, 3)) == 1.5 and placed.position == Vector2(2, 3), "interface properties: a member of this language and an engine property")
	placed.free()
	var accessors := AccessorCheck.new()
	_check(accessors.speed == 1.0 and accessors.writes == 0 and accessors.doubled == 6 and accessors.named == -1, "initializers write the field, and reads go through the getter")
	accessors.speed = -2.0
	accessors.set("named", 5)
	_check(accessors.speed == 0.0 and accessors.writes == 1 and accessors.named == 104, "writes from the engine go through the setter")
	accessors.inside()
	_check(accessors.speed == 0.0 and accessors.writes == 2 and accessors.doubled == 14 and accessors.named == 100, "within the class, accessors run, and in their own the name is the field")
	accessors.free()
	_check(c.classify("middle", 0) == "center" and c.classify("bottom", 0) == "b:bottom" and c.classify("x", 0) == "zero" and c.classify("x", 2) == "small" and c.classify("x", 101) == "big" and c.classify("x", 50) == "other", "match: literals, commas, var with when, and _")
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
	_check(nodes.title() == "unique", "%Name reads the node the scene gives that unique name (spec §7.1)")
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
	_check(view.through_classes() == [2, 10, 15, 5, 3, 1, true, 16.0, true, "res://language/preload_unit.gd", 2, 2, 16, true], "new() with the arguments of _init, static methods and constants through a class_name and a preload, of GDScript and of this language")
	view.free()
	base.free()
	child.free()

	# Spec §3.6: structs, which the engine sees as Dictionaries (§10.1).
	var s := StructCheck.new()
	add_child(s)
	_check(s.changed() == "7 3 (5.0, 2.0) 1 member hit 7", "a field changes where its struct lives, and an assignment copies")
	_check(s.equal(3, 3) and not s.equal(3, 4) and s.equal_sources(s), "structs compare field by field, objects by identity")
	_check(s.defaults() == "0 true 0.0" and s.loop_fresh() == 13, "a struct starts at its fields' defaults, and one made in a loop has every field written anew")
	_check(s.make(3) == {"damage": 3, "point": Vector2(1, 2), "crit": false, "source": null, "tag": "hit"}, "a struct reaches the engine as a Dictionary keyed by field name")
	_check(s.last_hit == {"damage": 7, "point": Vector2(5, 2), "crit": false, "source": null, "tag": "member"}, "a struct member is read through reflection as a copy")
	var hit := {"damage": 7.0, "point": Vector2(), "crit": true, "source": s, "tag": "gd"}
	_check(s.damage_of(hit) == 7, "a Dictionary decodes into a struct at the entry, an int field also from a float that is an integer")
	var fractional = s.damage_of({"damage": 7.5, "point": Vector2(), "crit": true, "source": null, "tag": "gd"})
	var missing = s.damage_of({"damage": 7})
	_check_errors(["struct_check.pet:58: Argument \"hit\" of damage_of(): StructCheck.Hit.damage: expected int, found float.", "struct_check.pet:58: Argument \"hit\" of damage_of(): StructCheck.Hit.point is missing."], "the program errors name the field that does not decode")
	_check(fractional == 0 and missing == 0, "a Dictionary without a field or with a field of another type is refused")
	_check(s.maybe_damage() == -1, "a struct T? starts at None")
	s.maybe = hit
	_check(s.maybe_damage() == 8 and s.maybe["damage"] == 8 and s.maybe["source"] == s, "a struct T? is written back after a narrowed local changed")
	var heard := []
	s.hit_taken.connect(func(h): heard.append(h["damage"]))
	s.emit_hit(5)
	_check(s.received == 5 and heard == [5], "a signal passes a struct as a Dictionary, which a handler of this language decodes")
	var other := StructCheck.new()
	s.remote(other)
	_check(other.last_hit["damage"] == 42 and other.last_hit["tag"] == "hit", "a field of another instance's member is written back through the engine")
	s.free()
	other.free()

	# Spec §3.8: Dictionary[K, V], elements assigned in place, keys that cannot change.
	var containers := ContainerCheck.new()
	_check(containers.counted() == "16 11 true 2 3 b", "a Dictionary[K, V] is indexed, assigned, iterated over its keys, and get(k) gives None for a missing key")
	_check(containers.contained() == "[true, true, true, true, false, true, true, false, true, true, false, true]", "in is has() or contains(), x converted as they take it; on an object it looks for a member by name (spec §5.1)")
	_check(containers.formatted(7, 2) == "7 2 { \"damage\": 3, \"tags\": [] } <2> [002.5]" and containers.formatted(7, null) == "7 <null> { \"damage\": 3, \"tags\": [] } <<null>> [002.5]", "% formats a value of any type, this language's own as they enter the engine (spec §5.1)")
	containers.misformatted()
	_check_errors(["container_check.pet:167: String formatting error: not enough arguments for format string."], "a format error is a program error")
	var won: Dictionary = containers.scored("ann", 3)
	containers.scored("ann", 2)
	_check(won.is_typed() and won.get_typed_key_builtin() == TYPE_STRING and won["ann"] == 5 and containers.scores.is_typed(), "a Dictionary[K, V] is the engine's typed dictionary, shared")
	containers.missing_key()
	_check_errors(["container_check.pet:26: The Dictionary has no key \"nope\"."], "the program error of a missing key")
	_check(containers.elements() == "[10, 2, 8] (4.0, 0.0) [(0.0, 2.0), (3.0, 3.0)] [7]", "elements are assigned in place, and a packed array or a member of an element is written back")
	containers.out_of_bounds()
	_check_errors(["container_check.pet:45: Index 1 is out of bounds (size 1)."], "the program error of assigning out of bounds")
	_check(containers.keys_frozen() == "true false 2 true", "a key is a read-only copy, and the original stays free to change")
	containers.change_key()
	_check_errors(["container_check.pet:58: The Array is read-only, as Dictionary keys and the containers of constants are; change a duplicate() of it instead."], "changing a key is a program error")
	var untyped: Dictionary = containers.untyped()
	_check(untyped == {&"name": 1, &"plain": 2, "v": Vector2(1, 2), "node": containers} and typeof(untyped.keys()[0]) == TYPE_STRING_NAME, "an untyped Dictionary takes any value, and a key written as a name is a StringName")
	var typed_scores: Dictionary[String, int] = {"a": 1}
	_check(containers.typed_entry(typed_scores) == 1, "a typed Dictionary passes the entry check")
	containers.callv("typed_entry", [{"a": 1}])
	_check_errors(["container_check.pet:66: Argument \"d\" of typed_entry(): expected Dictionary[String, int], found Dictionary."], "an untyped Dictionary is refused where Dictionary[K, V] is expected")
	_check(containers.boxed() == "15 true 0 1 5 false 2 true true 3", "containers of structs and T? keep them boxed, matched and hashed by value, and a deep copy copies inside them")
	_check(str(containers.hits_out()) == "[ContainerCheck.Hit(damage = 1, tags = [])]", "a box prints as its value")
	_check(containers.received({"states": {"a": {"damage": 4.0, "tags": ["t"]}}, "name": "m"}) == "4 t m 1", "a struct decodes its container fields into new containers")
	_check(containers.sent() == {"a": {"damage": 1, "tags": []}}, "Dictionary(d) encodes the structs in it")
	_check(containers.struct_keys() == "true false true", "a struct key is a frozen copy")
	containers.change_struct_key()
	_check_errors(["container_check.pet:116: The Array is read-only, as Dictionary keys and the containers of constants are; change a duplicate() of it instead."], "changing an Array in a struct key is a program error")
	_check(containers.nested() == "[[1, 2], [3, 4], [5]] true", "a container of containers keeps their types, and resize() fills in typed ones")
	_check(containers.grid_row(0) == 1, "an inner container of its type is read")
	containers.grid.append([2, 3])
	containers.grid_row(1)
	_check_errors(["container_check.pet:126: Expected Array[int], found Array."], "an inner container of another type is a program error when read")
	_check(containers.ordered() == "[1, 2, 2, 3] a 2 1 3 true true 1", "structs order by their fields, T? with None first, and containers of them sort, search and find their least and greatest")
	var no_ints: Array[int] = []
	var some_ints: Array[int] = [3, 1]
	_check(containers.least(some_ints) == 1 and containers.first(some_ints) == 3, "min() and front() give an element")
	containers.least(no_ints)
	containers.first(no_ints)
	_check_errors(["container_check.pet:145: min() of an empty array.", "container_check.pet:148: front() of an empty array."], "min() and front() of an empty array are program errors")
	containers.free()

	var frames_before: int = c.frames
	c.defer()
	var freeing := FreeCheck.new()
	var freer := FreeCheck.new()
	_check(freer.free_other(freeing) == 0, "a call whose callee read a member after freeing self gives the default value")
	_check_errors(["free_check.pet:17: self was freed, or lost its script, while this method ran."], "the program error of reading a member after self was freed")
	_check(not is_instance_valid(freeing), "a method of this language that frees self frees it")
	freer.free()
	await get_tree().process_frame
	await get_tree().process_frame
	_check(c.frames > frames_before, "_process runs every frame")
	_check(c.deferred == 2, "Callable.call_deferred() with an argument runs at the end of the frame")

	var owned: TargetCheck = preload("res://language/owner.tscn").instantiate()
	add_child(owned)
	var component: OwnerCheck = owned.get_node("Component")
	component.hurt(2.0)
	_check(component.taken() == 2.0 and owned.taken == 2.0, "@owner reads and writes through the owner, typed by the requirement")
	owned.remove_child(component)
	component.taken()
	var other_owner := Node2D.new()
	other_owner.add_child(component)
	component.owner = other_owner
	component.taken()
	_check_errors(["owner_check.pet:7: owner: expected CombatCheck.Placed, found None (@owner).", "owner_check.pet:7: owner: expected CombatCheck.Placed, found Node2D (@owner)."], "@owner checks every read, after the engine or code changed the owner")
	other_owner.free()
	owned.free()

	var k := ConstantCheck.new()
	_check(k.day() == 86400000, "a constant the compiler computes (spec §4.5)")
	_check(k.extents() == Vector3(0.5, 0.5, 0.5) and k.half_x() == 0.5 and k.tint() == Color(1, 1, 1, 0.5), "constants of engine types, computed when the class loads")
	_check(k.angle() == deg_to_rad(30) and k.slope() == tan(deg_to_rad(30)), "math functions that return float, computed by the engine")
	_check(k.right() and k.red() == Color.RED, "a bool and a Color from a string, computed by the engine")
	_check(k.names() == ["a", "b"] and k.hp() == 100 and k.read_only(), "containers in constants are read-only")
	k.change()
	_check_errors(["constant_check.pet:68: The Array is read-only, as Dictionary keys and the containers of constants are; change a duplicate() of it instead."], "changing a constant's container through a variable is a program error")
	_check(k.limit() == 3 and ConstantCheck.LIMIT == 3 and ConstantCheck.FALLBACK == null, "a constant of T? is Some of its value or None, which GDScript reads as null")
	_check(k.bits(182, 3) == 361 and k.shift(1, 62) == 1 << 62, "bitwise operators and their assignments at run time (spec §5.1)")
	k.shift(1, 64)
	k.shift(-1, 1)
	_check_errors(["constant_check.pet:91: Cannot compute 1 << 64: a shift by 64 or more bits is undefined.", "constant_check.pet:91: Cannot compute -1 << 1: a shift takes operands that are not negative."], "a shift of a negative int or by 64 or more bits is a program error")
	_check(k.power(3, 39) == 4052555153018976267 and k.power(2, 64) == 0 and k.power(2, -1) == 0 and k.power(-1, -3) == -1 and ConstantCheck.POWERS == [64, -4, 4052555153018976267, 0, -1], "** of ints is exact and wraps as * does, in code and in constants (spec §5.1)")
	_check(ConstantCheck.ROOT == pow(2.0, 0.5) and k.root(2.0) == pow(2.0, 0.5), "** of floats is the engine's pow")
	k.power(0, -1)
	_check_errors(["constant_check.pet:103: Cannot compute 0 ** -1: division by zero."], "0 to a negative power is a program error")
	_check(ConstantCheck.DAY_MS == 86400000 and ConstantCheck.NAMES == ["a", "b"] and k.MASK == 9, "GDScript reads a pub constant through the class and through an instance (spec §11.3)")
	var constant_map: Dictionary = (k.get_script() as Script).get_script_constant_map()
	_check(constant_map.get("ANGLE") == deg_to_rad(30) and not constant_map.has("SECRET") and k.secret() == 7, "the engine sees the pub constants only (spec §10.5)")
	_check(k.global_constants() == [KEY_ESCAPE, MOUSE_BUTTON_RIGHT, PROPERTY_HINT_FILE_PATH, INT64_MIN], "the engine's global constants have the engine's values")
	_check(k.mode_of(3) == 3 and k.takes_mode(3) == 4 and k.mode == 3 and ConstantCheck.Mode == {"CALM": 0, "ANGRY": 3} and ConstantCheck.LARGE == 1, "an enum's values are ints, and GDScript reads its name as a Dictionary (spec §3.7)")
	k.mode_of(1)
	k.takes_mode(2)
	_check_errors(["constant_check.pet:118: 1 is none of the values of ConstantCheck.Mode.", "constant_check.pet:121: Argument \"m\" of takes_mode(): expected ConstantCheck.Mode, found 2, which is none of its values."], "an int that is none of an enum's values is a program error, also at the entry")
	var mode_property := {}
	for property in k.get_property_list():
		if property.name == "mode":
			mode_property = property
	_check(mode_property.get("hint") == PROPERTY_HINT_ENUM and mode_property.get("hint_string") == "Calm:0,Angry:3", "an exported enum is a drop-down, as GDScript's")
	_check(k.button_of(2) == MOUSE_BUTTON_RIGHT and k.button_of(1000) == 1000 and k.process_name(Node.PROCESS_MODE_ALWAYS) == "always" and k.process_name(1000) == "other", "the engine's enums are open: their values are not checked (spec §3.7)")
	var angry: Array[int] = [3]
	var stray: Array[int] = [99]
	_check(k.mode_name(angry) == "angry", "an exhaustive match over an enum of this language takes every path (spec §5.6)")
	k.mode_name(stray)
	_check_errors(["constant_check.pet:140: 99 is none of the values of ConstantCheck.Mode."], "a value from the engine that is none of the enum's is a program error at the match")

	var statics := StaticChild.new()
	_check(statics.made(), "new() of classes of this language runs their initializers and _init() (spec §4.6)")
	_check(statics.own_static() == 8 and statics.through_base() == 7, "static methods of classes of this language, through the class's name (spec §4.8)")
	_check(statics.engine_static() and statics.builtin_static(), "static methods of engine classes and built-in types")
	_check(statics.on_objects(statics) == ["child", "check", true, true], "a static method called on an object is found from its class up, and one of an engine class or a built-in type is that type's (spec §4.8)")
	statics.free()

	# Lambdas (spec §3.11).
	var lambdas := LambdaCheck.new()
	add_child(lambdas)
	_check(lambdas.by_value() == [1, 1], "a lambda captures values as they are where it is made, a struct as a copy (spec §3.11)")
	_check(lambdas.shared() == [5, 5], "a lambda shares the containers it captures, and self")
	_check(lambdas.connected() == 40, "a lambda connected to a signal takes what the signal passes")
	var parent := Node.new()
	parent.name = "P"
	_check(lambdas.nested(parent) == "in P", "a lambda in a lambda captures through it, as narrowed where it is made")
	parent.free()
	_check(lambdas.static_lambda() == 3, "a lambda in a static function")
	_check(lambdas.calls() == [12, 45, 42] and lambdas.sorted() == [3, 2, 1], "call(), callv() and bind() of lambdas, and a lambda the engine calls")
	_check(lambdas.make_adder(3).call(4) == 7, "GDScript calls a lambda of this language")
	_check(str(lambdas.named()) == "on_tick(lambda)" and str(lambdas.make_adder(1)) == "(anonymous lambda)", "a lambda's name labels its Callable, as in GDScript")
	lambdas.after_error = -1
	lambdas.failing_call()
	lambdas.failing_bound()
	_check_errors(["lambda_check.pet:91: Index 5 is out of bounds (size 1).", "lambda_check.pet:96: Index 7 is out of bounds (size 1)."], "a program error in a lambda reports where it is")
	_check(lambdas.after_error == -1, "a program error in a lambda called with call() aborts the caller, also after bind() (spec §8.1)")
	var on_self: Callable = lambdas.bound_to_self()
	_check(on_self.is_valid() and on_self.get_object() == lambdas and on_self.call() == 5, "a lambda that uses self is bound to it")
	lambdas.free()
	_check(not on_self.is_valid(), "a lambda whose node was freed is not valid")
	var reader: Callable = LambdaHolder.new().reader()
	_check(reader.call() == 5, "a lambda keeps its RefCounted self alive")
	var static_script: Script = load("res://language/static_check.pet")
	for i in 3:
		_check(static_script.get("NODES") == load("res://language/nodes.tscn"), "a pub preloaded constant read by name is the loaded resource, each time")

	# Inner classes (spec §4.1).
	var inner := InnerCheck.new()
	add_child(inner)
	var shots := inner.shots()
	_check(shots == [6, 16, 24, true, false], "inner classes run, using the static functions and constants of the classes around them: %s" % [shots])
	var shot_script: Script = inner.shot_script()
	_check(shot_script.resource_path == "" and shot_script.get_global_name() == &"" and shot_script.get_instance_base_type() == &"RefCounted", "an inner class is a script of its own, without a path or a global name")
	var shot = shot_script.new()
	_check(shot.get_script() == shot_script and shot.boosted() == 12, "an object of an inner class has its script")
	var inner_file: Script = load("res://language/inner_check.pet")
	_check(inner_file.get_script_constant_map().get("Shot") == shot_script and not inner_file.get_script_constant_map().has("Trail") and inner_file.get("Shot") == shot_script and InnerCheck.Shot == shot_script, "a pub inner class is a constant of the class around it, which GDScript reads (spec §10.5)")
	var trail_node := Node.new()
	inner.attach(trail_node)
	_check(trail_node.get_script() != null and trail_node.get("label") == "trail" and trail_node.get_script().resource_path == "", "set_script() sets an inner class at run time")
	trail_node.free()
	_check(inner.copy_label() == "copy", "an inner class extends the class around it")
	inner.failing()
	_check_errors(["inner_check.pet:26: Index 5 is out of bounds (size 1)."], "a program error in an inner class reports the line of its file")
	inner.free()

	# Static variables (spec §4.8).
	var static_vars: Script = load("res://language/static_vars.pet")
	_check(static_vars.get("log") == ["StaticVars", "_static_init", "Late"], "static initialization runs when the class loads, _static_init() last, then the inner classes': %s" % [static_vars.get("log")])
	_check(static_vars.get("doubled") == 2 and static_vars.get("from_inner") == 6 and static_vars.get("names") == { "a": 2 } and static_vars.get("Late").get("order") == 2, "static initializers run in declaration order, and a class they read is initialized first")
	var static_object := StaticVars.new()
	_check(static_object.instance_count() == 8 and static_object.through_object(static_object) == 1 and static_object.get("count") == 1, "a static variable is read in its class and through objects")
	static_vars.set("label", "abc")
	static_vars.set("count", 4)
	_check(static_vars.get("label") == "ABC" and static_object.instance_count() == 11, "GDScript writes a pub static variable through the class, past its setter")
	_check(static_vars.get("hidden") == null, "a static variable that is not pub is not reached by name (spec §10.5)")
	StaticSub.new().set_count(9)
	_check(static_vars.get("count") == 9, "a subclass shares the static variables of its base")
	load("res://language/static_cycle_a.pet")
	_check_errors(["static_cycle_b.pet:2: Static initialization cycle: StaticCycleA.x → StaticCycleB.y → StaticCycleA.x.", "static_cycle_a.pet:3: StaticCycleB.y cannot be used: the static initialization of StaticCycleB failed."], "a cycle between static initializations is a program error that lists it")
	load("res://language/static_order.pet")
	_check_errors(["static_order.pet:8: static_order.pet.b is used before its initializer runs: static variables are initialized in declaration order."], "a static variable used before its initializer runs is a program error")

	# Annotations describe a class to the engine as GDScript's do (spec §2.3).
	var annotated := AnnotationCheck.new()
	var reference := AnnotationReference.new()
	var annotated_properties := {}
	for property in annotated.get_property_list():
		annotated_properties[property.name] = property
	var mismatched: Array[String] = []
	for property in reference.get_property_list():
		var own: Dictionary = annotated_properties.get(property.name, {})
		if property.usage & PROPERTY_USAGE_SCRIPT_VARIABLE and ["type", "hint", "hint_string", "usage"].any(func(key): return own.get(key) != property[key]):
			mismatched.append("%s: %s, not %s" % [property.name, own, property])
	_check(mismatched.is_empty(), "the @export family describes members as in GDScript%s" % ("" if mismatched.is_empty() else ": %s" % mismatched))
	var rpc_defaults := { "rpc_mode": MultiplayerAPI.RPC_MODE_DISABLED, "transfer_mode": MultiplayerPeer.TRANSFER_MODE_RELIABLE, "call_local": false, "channel": 0 }
	var own_rpc: Dictionary = annotated.get_script().get_rpc_config()
	var reference_rpc: Dictionary = reference.get_script().get_rpc_config()
	var same_rpc := own_rpc.size() == reference_rpc.size() and reference_rpc.size() == 3
	for method in reference_rpc:
		for key in rpc_defaults:
			same_rpc = same_rpc and own_rpc.get(method, {}).get(key, rpc_defaults[key]) == reference_rpc[method].get(key, rpc_defaults[key])
	_check(same_rpc, "@rpc configures methods as in GDScript: %s" % own_rpc)
	annotated.call("hit", 4)
	_check(annotated.hit_count() == 4, "the engine finds a method with @rpc by name")
	var icons := {}
	for entry in ProjectSettings.get_global_class_list():
		icons[entry["class"]] = entry["icon"]
	_check(icons.get(&"AnnotationCheck") == "res://language/icons/annotation.svg" and icons.get(&"AnnotationReference") == icons.get(&"AnnotationCheck"), "@icon is relative to the script's directory, as in GDScript")
	annotated.free()
	reference.free()

	# @abstract (spec §4.1).
	var strike := AbstractStrike.new()
	_check(strike.describe() == "strike 3", "a method of an abstract class calls the abstract method its subclass implements")
	var action_script: Script = load("res://language/abstract_action.pet")
	var cost_flags := 0
	for method in action_script.get_script_method_list():
		if method.name == "cost":
			cost_flags = method.flags
	_check(action_script.is_abstract() and not action_script.can_instantiate() and not strike.get_script().is_abstract() and cost_flags & METHOD_FLAG_VIRTUAL_REQUIRED, "the engine sees which classes and methods are abstract")
	var abstract_classes := {}
	for entry in ProjectSettings.get_global_class_list():
		abstract_classes[entry["class"]] = entry["is_abstract"]
	_check(abstract_classes.get(&"AbstractAction") == true and abstract_classes.get(&"AbstractStrike") == false, "the global class list marks abstract classes")
	Resource.new().set_script(action_script)
	_check_errors(["Cannot set object script. Script 'res://language/abstract_action.pet' should not be abstract."], "the engine gives no object an abstract script")

	for test: GDScript in [preload("res://language/coroutines_test.gd"), preload("res://language/defaults_test.gd"), preload("res://language/inheritance_test.gd")]:
		var part: Node = test.new()
		add_child(part)
		failures += await part.run(errors)
		part.queue_free()

	_check_errors([], "no other errors")
	OS.remove_logger(errors)
	print("language: %d failure(s)" % failures)
	get_tree().quit(1 if failures > 0 else 0)
