#include "doctest.h"
#include "helpers.h"

using namespace petrichor::tests;

// Built-in value types and engine objects (spec §3.1, §3.3, §5, §10).

TEST_CASE("operators follow the engine's table (spec §5.1)") {
	CHECK(type_of("Vector2(1, 2) * 2.0") == "Vector2");
	CHECK(type_of("Vector2i(1, 2) * 2.0") == "Vector2");
	CHECK(type_of("2.0 * Vector3(1, 2, 3)") == "Vector3");
	CHECK(type_of("Vector3(1, 2, 3) / 30") == "Vector3");
	CHECK(type_of("-Vector3(1, 2, 3)") == "Vector3");
	CHECK(type_of("7 / 2") == "int");
	CHECK(contains(type_of("Vector2(1, 2) + Vector2i(1, 2)"), "Invalid operands Vector2 and Vector2i"));
}

TEST_CASE("== follows the engine's table too (spec §5.2)") {
	CHECK(type_of("Vector2(1, 2) == Vector2(1, 2)") == "bool");
	CHECK(contains(type_of("Vector2() == Vector2i()"), "Invalid operands"));
	const std::string arrays = "var ints: Array[int] = [1]\nvar floats: Array[float] = [1.0]\nvar more: Array[int] = [2]";
	CHECK(type_of("ints == more", arrays) == "bool");
	CHECK(contains(type_of("ints == floats", arrays), "Array[int] and Array[float] are equal only when both are empty"));
	CHECK(contains(type_of("ints != floats", arrays), "are equal only when both are empty"));
}

TEST_CASE("utility functions bound with Variant are typed by their arguments (spec §10.3)") {
	CHECK(type_of("clamp(0.5, 0.0, 1.0)") == "float");
	CHECK(type_of("abs(-3)") == "int");
	CHECK(type_of("wrap(5, 0, 3)") == "int");
	CHECK(type_of("lerp(Vector3(), Vector3(1, 1, 1), 0.5)") == "Vector3");
	CHECK(type_of("lerp(0.0, 1.0, 1)") == "float"); // An int weight converts (spec §3.2).
	CHECK(contains(type_of("lerp(0.0, 1.0, \"a\")"), "must be a number"));
	CHECK(contains(type_of("min(1.5, 1)"), "Write the int argument as a float"));
	CHECK(contains(type_of("lerp(Vector3(), 1.0, 0.5)"), "same type"));
	CHECK(contains(type_of("clamp(\"a\", \"b\", \"c\")"), "numbers or vectors"));
}

TEST_CASE("other utility functions take the engine's types") {
	CHECK(type_of("cos(1)") == "float");
	CHECK(type_of("deg_to_rad(90)") == "float");
	CHECK(type_of("randi()") == "int");
	CHECK(type_of("str(1, \"a\")") == "String");
	CHECK(has_error(in_function("var r := randf_range(\"a\", 1.0)"), "Cannot pass a value of type String as float"));
}

TEST_CASE("members, constants and constructors of built-in types") {
	CHECK(type_of("v.y", "var v := Vector3(1, 2, 3)") == "float");
	CHECK(type_of("t.origin", "var t := Transform3D()") == "Vector3");
	CHECK(type_of("Vector3.UP") == "Vector3");
	CHECK(type_of("Vector3(1, 2, 3).normalized()") == "Vector3");
	CHECK(type_of("Vector3().lerp(Vector3.ONE, 0.5)") == "Vector3");
	CHECK(contains(type_of("v.w", "var v := Vector3()"), "Vector3 has no member \"w\""));
	CHECK(contains(type_of("Vector3.NOPE"), "no constant"));
	CHECK(contains(type_of("Vector3(\"a\")"), "No constructor of Vector3 takes (String)"));
}

TEST_CASE("engine properties and objects") {
	CHECK(type_of("position") == "Vector3");
	CHECK(type_of("position.y") == "float");
	CHECK(type_of("RandomNumberGenerator.new()") == "RandomNumberGenerator");
	CHECK(type_of("rng.randf_range(0, 1)", "var rng := RandomNumberGenerator.new()") == "float");
	CHECK(type_of("rng.seed", "var rng := RandomNumberGenerator.new()") == "int");
	CHECK(contains(type_of("rng.nope", "var rng := RandomNumberGenerator.new()"), "has no property"));
	CHECK(contains(type_of("RandomNumberGenerator.new(1)"), "takes no arguments"));
	CHECK(contains(type_of("Vector3"), "is a type, not a value"));
}

TEST_CASE("assignment writes members of built-in values back to their place") {
	CHECK(errors_of(in_function("position.y += 1.0\nrotation.y = 2\nvar v := Vector3()\nv.x = 1", "func f() -> void:", "Node3D")).empty());
	CHECK(errors_of(in_function("var rng := RandomNumberGenerator.new()\nrng.seed = 3")).empty());
	CHECK(has_error(in_function("Vector3(1, 2, 3).y = 2", "func f() -> void:", "Node3D"), "member of a temporary"));
	CHECK(has_error(in_function("var rng := RandomNumberGenerator.new()\nrng.seed = 1.5"), "Convert explicitly with int(x)."));
	CHECK(has_error(in_function("position = 1", "func f() -> void:", "Node3D"), "Cannot assign a value of type int as Vector3"));
}

TEST_CASE("a method that changes a value needs a place to write it back") {
	CHECK(errors_of(in_function("var a := PackedInt32Array()\na.push_back(1)")).empty());
	CHECK(has_error(in_function("PackedInt32Array().push_back(1)"), "a temporary value"));
	// Arrays are shared: a method changes them in place.
	CHECK(errors_of(in_function("Array().append(1)")).empty());
}

TEST_CASE("T? is a nullable object type (spec §3.3)") {
	CHECK(errors_of("extends Node\nvar target: Node3D?\n").empty());
	CHECK(errors_of(in_function("var p: Node?\np = get_parent()")).empty());
	CHECK(type_of("rng", "var rng: RandomNumberGenerator? = RandomNumberGenerator.new()") == "RandomNumberGenerator?");
	CHECK(has_error(in_function("var n: Node = get_parent()"), "Cannot assign a value of type Node? as Node"));
}

TEST_CASE("T? of a value type is an Option (spec §3.4)") {
	CHECK(errors_of(in_function("var a: int? = 5\nvar b: int?\nb = a\nvar s: String? = \"x\"\nvar f: float? = 1")).empty());
	CHECK(type_of("a.unwrap()", "var a: int? = 5") == "int");
	CHECK(type_of("s.expect(\"a name\")", "var s: String? = \"x\"") == "String");
	CHECK(type_of("v.unwrap().y", "var v: Vector3? = Vector3.UP") == "float");
	CHECK(contains(type_of("a + 1", "var a: int? = 5"), "may be None (int?); unwrap it first"));
	CHECK(contains(type_of("s.length()", "var s: String? = \"x\""), "may be None (String?); check it before calling"));
	CHECK(contains(type_of("v.y", "var v: Vector3? = Vector3.UP"), "may be None (Vector3?); check it before reading"));
	CHECK(contains(type_of("a.unwrap_or(0)", "var a: int? = 5"), "not supported yet"));
	CHECK(has_error(in_function("var a: int? = 5\nvar b: int = a"), "Cannot assign a value of type int? as int"));
	// Only a T is converted on the way in, not the T inside.
	CHECK(has_error(in_function("var a: int? = 5\nvar f: float? = a"), "Cannot assign a value of type int? as float?"));
	CHECK(has_error("extends Node\n\nfunc f() -> void?:\n\tpass\n", "void cannot be optional"));
}

TEST_CASE("Array[T] is the engine's typed array, with methods typed by T (spec §3.8)") {
	const char *ints = "var a: Array[int] = [1, 2]";
	CHECK(errors_of(in_function("var a: Array[int] = []\na.append(1)\nvar f: Array[float] = [1, 2.5]\nvar u: Array = []")).empty());
	CHECK(type_of("a[0]", ints) == "int");
	CHECK(type_of("a[-1] + a.front() + a.back() + a.size()", ints) == "int");
	CHECK(type_of("a.pop_front()", ints) == "int?");
	CHECK(type_of("a.pop_back().unwrap()", ints) == "int");
	CHECK(type_of("a.duplicate()", ints) == "Array[int]");
	CHECK(type_of("n[0]", "var n: Array[Node] = []") == "Node");
	CHECK(type_of("n.pop_back()", "var n: Array[Node] = []") == "Node?");
	CHECK(type_of("n[0]", "var n: Array[Node?] = []") == "Node?");
	// Node?? would tell an empty array from a None element (spec §3.8).
	CHECK(contains(type_of("n.pop_back()", "var n: Array[Node?] = []"), "gives Node??, which is not supported yet"));
	CHECK(contains(type_of("n.pop_front()", "var n: Array[Node?] = []"), "not supported yet"));
	// pop_at() gives T: an index out of bounds is a program error, as for a[i].
	CHECK(type_of("a.pop_at(0)", ints) == "int");
	CHECK(type_of("n.pop_at(0)", "var n: Array[Node?] = []") == "Node?");
	CHECK(has_error(in_function(std::string(ints) + "\na.append(\"x\")"), "Cannot pass a value of type String as int"));
	CHECK(has_error(in_function(std::string(ints) + "\nvar f: Array[float] = a"), "Cannot assign a value of type Array[int] as Array[float]"));
	CHECK(has_error(in_function("var a: Array[int] = [1, 2.5]"), "Cannot put a value of type float in Array[int]"));
	CHECK(has_error(in_function("var a = []"), "An empty array needs a declared type"));
	CHECK(contains(type_of("a[\"x\"]", ints), "An index must be int, found String"));
	CHECK(contains(type_of("u[0]", "var u: Array = []"), "untyped Array are Variant"));
	CHECK(contains(type_of("v[0]", "var v := Vector3()"), "Vector3 cannot be indexed"));
	CHECK(has_error(in_function(std::string(ints) + "\na[0] = 2"), "Assigning to an element, as in a[i] = v, is not supported yet."));
	// Spec §5.1: as the engine and GDScript type it, alike arrays add up typed.
	CHECK(type_of("a + a", ints) == "Array[int]");
	CHECK(type_of("a + f", std::string(ints) + "\nvar f: Array[float] = []") == "Array");
	CHECK(has_error(in_function("var a: Array[int?] = []"), "Arrays of int? are not supported yet"));
	// The entry checks would take any array for it.
	CHECK(has_error(in_function("var a: Array[int]? = None"), "Array[int]? is not supported yet"));
	CHECK(has_error("extends Node\n\nfunc f(a: Array[Node]?):\n\tpass\n", "Array[Node]? is not supported yet"));
	CHECK(has_error("extends Node\n\nfunc f() -> Array[int]?:\n\treturn None\n", "Array[int]? is not supported yet"));
	CHECK(has_error("extends Node\n@export var a: Array[int]?\n", "Array[int]? is not supported yet"));
	CHECK(errors_of("extends Node\n\nfunc f(a: Array[int], b: int?, c: Array?):\n\tpass\n").empty());
	CHECK(has_error(in_function("var d: Dictionary[String, int]"), "Dictionary[K, V] is not supported yet"));
}

TEST_CASE("a built-in method's default values are passed as the engine declares them") {
	CHECK(type_of("Vector3.ONE.limit_length()") == "Vector3"); // length: float = 1.0
	CHECK(type_of("\"a,b\".split(\",\")") == "PackedStringArray"); // allow_empty = true, maxsplit = 0
	CHECK(type_of("\"a\".split()") == "PackedStringArray"); // delimiter = ""
}

TEST_CASE("packed arrays are indexed by their element type") {
	CHECK(type_of("\"a,b\".split(\",\")[1]") == "String");
	CHECK(type_of("p[0]", "var p := PackedVector3Array()") == "Vector3");
}

// Spec §5.7.
TEST_CASE("for over an array") {
	CHECK(errors_of(in_function("var a: Array[int] = [1, 2]\nvar total := 0\nfor x in a:\n\ttotal += x")).empty());
	CHECK(errors_of(in_function("var a: Array[Node3D] = []\nfor n: Node in a:\n\tn.queue_free()")).empty());
	CHECK(has_error(in_function("var a: Array[Node] = []\nfor n: Node3D in a:\n\tpass"), "The elements are Node, which the loop variable of type Node3D cannot hold."));
	CHECK(errors_of(in_function("for s in \"a,b\".split(\",\"):\n\tprint(s)")).empty());
	CHECK(has_error(in_function("var u: Array = []\nfor x in u:\n\tpass"), "untyped Array are Variant"));
}

// Spec §10.2, §10.3: the engine's RequiredResult and RequiredParam metadata,
// and the audit in binding_metadata.txt.
TEST_CASE("the binding metadata says which objects are never None") {
	CHECK(type_of("get_parent()") == "Node?");
	CHECK(type_of("get_tree()") == "SceneTree"); // Audited: null only when misused.
	CHECK(type_of("create_tween()") == "Tween"); // RequiredResult.
	CHECK(type_of("get_tree().root") == "Window"); // Its getter is a RequiredResult.
	CHECK(type_of("get_tree().get_root()") == "Window");
	CHECK(has_error(in_function("add_child(get_parent())"), "Cannot pass a value of type Node? as Node")); // RequiredParam.
	CHECK(errors_of(in_function("set_owner(get_parent())")).empty());
}

// Spec §10.3: scene/main/node.cpp:2031-2042.
TEST_CASE("an engine array filtered by a literal class name holds that class") {
	CHECK(type_of("find_children(\"*\", \"MeshInstance3D\")") == "Array[MeshInstance3D]");
	// player.gd of the FPS starter kit.
	CHECK(errors_of(in_function("for child in find_children(\"*\", \"MeshInstance3D\"):\n\tchild.layers = 2")).empty());
	CHECK(type_of("find_children(\"*\")") == "Array[Node]");
	CHECK(type_of("find_children(\"*\", \"\")") == "Array[Node]");
	CHECK(type_of("find_children(\"*\", kind)", "var kind := \"MeshInstance3D\"") == "Array[Node]");
	CHECK(contains(type_of("find_children(\"*\", \"MeshInstanc3D\")"), "find_children() filters by the class \"MeshInstanc3D\", which does not exist."));
	CHECK(contains(type_of("find_children(\"*\", \"Resource\")"), "find_children() returns objects of Node, and Resource is not one."));

	// The engine's array is Array[Node]; a new Array[MeshInstance3D] takes its elements.
	const auto result = compile(in_function("var meshes := find_children(\"*\", \"MeshInstance3D\")"));
	REQUIRE_MESSAGE(result.ok(), messages(result));
	bool assigned = false;
	for (const auto &call : result.module.builtin_methods) {
		assigned = assigned || (call.base == petrichor::bytecode::ValueType::Array && call.method == "assign");
	}
	CHECK(assigned);
	bool typed = false;
	for (const auto &type : result.module.types) {
		typed = typed || (type.kind == petrichor::bytecode::TypeKind::Array && result.module.types[type.element].object.name == "MeshInstance3D");
	}
	CHECK(typed);
}

TEST_CASE("a class of this language filters by its class_name") {
	FakeProject project;
	project.add_script("res://bar.pet", "extends Node3D\nclass_name Bar\n\npub func fill():\n\tpass\n", "Bar");
	project.global_classes["Legacy"] = "res://legacy.gd";
	const auto errors = [&](const std::string &p_body) {
		return messages(compile_in("extends Node\n\nfunc f():\n\t" + p_body + "\n", project));
	};
	CHECK(errors("for bar in find_children(\"*\", \"Bar\"):\n\t\tbar.fill()").empty());
	CHECK(errors("var nodes: Array[Node] = find_children(\"*\", \"Legacy\")").empty());
}

TEST_CASE("integer constants of engine classes") {
	CHECK(type_of("Input.MOUSE_MODE_CAPTURED") == "int");
	CHECK(type_of("Tween.EASE_OUT_IN") == "int");
	CHECK(errors_of(in_function("Input.mouse_mode = Input.MOUSE_MODE_CAPTURED")).empty());
	CHECK(contains(type_of("Tween.NOPE"), "Tween has no constant \"NOPE\""));
}

TEST_CASE("engine singletons are reached by their class name") {
	CHECK(type_of("Input.is_action_pressed(\"jump\")") == "bool");
	CHECK(type_of("Input") == "Input");
	CHECK(errors_of(in_function("var loader := ResourceLoader\nvar r = loader.load(\"res://a.tres\")")).empty());
}

// Spec §7.4: a path known only at run time gives what the engine declares.
TEST_CASE("load() is ResourceLoader.load()") {
	CHECK(type_of("load(\"res://\" + \"a.ogg\")") == "Resource?");
	CHECK(type_of("load(\"res://a.ogg\") as AudioStream") == "AudioStream?");
	CHECK(has_error(in_function("var r = load(1)"), "Cannot pass a value of type int as String"));
}

// Spec §3.11, §4.7.
TEST_CASE("method values, signals and connect") {
	const std::string handler = "extends Node\n\nfunc handle(p: AudioStreamPlayer):\n\tpass\n\nfunc on_child(n: Node):\n\tpass\n\nfunc f():\n\tvar p := AudioStreamPlayer.new()\n\t";
	CHECK(errors_of(handler + "p.finished.connect(handle.bind(p))\n").empty());
	CHECK(errors_of(handler + "child_entered_tree.connect(on_child)\n").empty());
	CHECK(errors_of(handler + "var c: Callable = handle\n\tvar s: Signal = p.finished\n").empty());
	CHECK(has_error(handler + "p.finished.connect(handle)\n", "Signal() passes 0 argument(s), but func(AudioStreamPlayer) takes 1."));
	CHECK(has_error(handler + "child_entered_tree.connect(handle)\n", "Signal(Node) passes Node as argument 1, which func(AudioStreamPlayer) does not take."));
	CHECK(has_error(handler + "p.finished.connect(handle.bind(p, p))\n", "bind() binds 2 argument(s), but func(AudioStreamPlayer) takes 1."));
	CHECK(has_error(handler + "p.finished.connect(handle.bind(1))\n", "Cannot pass a value of type int as AudioStreamPlayer"));
	CHECK(errors_of(handler + "p.finished.emit()\n").empty());
	CHECK(has_error(handler + "p.finished.emit(1)\n", "takes 0 argument(s), 1 given"));
	CHECK(type_of("p.finished", "var p := AudioStreamPlayer.new()") == "Signal()");
}

// Spec §4.7.
TEST_CASE("signals the class declares") {
	const std::string head = "extends Node\nsignal hit(amount: int, by: Node)\nsignal done\n\nfunc on_hit(a: int, n: Node):\n\tpass\n\nfunc f():\n\t";
	CHECK(errors_of(head + "hit.emit(3, self_node())\n\tdone.emit()\n\thit.connect(on_hit)\n\nfunc self_node() -> Node:\n\treturn Node.new()\n").empty());
	CHECK(has_error(head + "hit.emit(\"x\", Node.new())\n", "Cannot pass a value of type String as int"));
	CHECK(has_error(head + "hit.emit(3)\n", "takes 2 argument(s), 1 given"));
	CHECK(has_error(head + "done.connect(on_hit)\n", "Signal() passes 0 argument(s), but func(int, Node) takes 2."));
	CHECK(has_error("extends Node\nsignal bad(x)\n", "needs a type"));
	CHECK(has_error("extends Node\nvar hit := 1\nsignal hit\n", "already declared"));
}

TEST_CASE("as converts objects to T? (spec §5.3)") {
	const char *setup = "var n: Node = Node3D.new()";
	CHECK(type_of("n as Node3D", setup) == "Node3D?");
	CHECK(type_of("(n as Node3D).unwrap().position", setup) == "Vector3");
	CHECK(contains(type_of("(n as Node3D).position", setup), "may be None (Node3D?)"));
	// Already a Node: nothing can fail.
	CHECK(type_of("Node3D.new() as Node") == "Node");
	CHECK(type_of("get_parent() as Node") == "Node?");
	CHECK(contains(type_of("n as RandomNumberGenerator", setup), "Node and RandomNumberGenerator are unrelated classes; as would always give None."));
	CHECK(contains(type_of("n as int", setup), "as only converts objects; convert to int with int(x)"));
	CHECK(contains(type_of("5 as Node"), "as only converts objects, not int"));
}

// Spec §10.2: what the engine passes in is T; a parameter may accept more (§4.6).
TEST_CASE("an override may take T? where the engine passes T") {
	CHECK(errors_of("extends Node\n\nfunc _input(event):\n\tpass\n").empty());
	CHECK(errors_of("extends Node\n\nfunc _input(event: InputEvent):\n\tpass\n").empty());
	CHECK(errors_of("extends Node\n\nfunc _input(event: InputEvent?):\n\tpass\n").empty());
	CHECK(errors_of("extends Node\n\nfunc _input(event: Resource):\n\tpass\n").empty());
	CHECK(has_error("extends Node\n\nfunc _input(event: Node):\n\tpass\n", "declares this parameter as InputEvent"));
}

TEST_CASE("objects that must be assigned are not supported yet (spec §6.2)") {
	CHECK(has_error("extends Node\nvar n: Node3D\n", "not supported yet"));
	CHECK(has_error(in_function("var n: Node3D"), "needs an initial value"));
	CHECK(errors_of(in_function("var v: Vector3\nvar s: StringName")).empty());
}
