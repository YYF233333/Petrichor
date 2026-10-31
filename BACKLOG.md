# Backlog

In no fixed order. An item is removed as soon as it is done (AGENTS.md, Workflow).

## Language

- Coroutine helpers `Task.of`, `delay`, `any`, `all`, `settled`, and async lambdas (spec §9).
- Signals and default methods of interfaces, and interfaces that extend others (spec §3.9).
- Of match (spec §5.6): array and dictionary patterns; a match over `T?`, with `None` and `var x` and the check that it is exhaustive; the patterns of enums, structs, Result and Settled once they exist.
- Static checks of must-assign exports in scenes, in resources and at export (spec §7.5), and of a scene used as a root whose root has a script with `@owner` (§7.3).
- `@parent(T)` (spec §7.3).
- Setters that run before ready (spec §6.5), once the open question below is settled; until then the checks that a node is ready report such a setter's first access to its node.
- A warning for removed nodes that are neither stored nor freed (spec §15).
- Preload cycles (spec §7.6), and preload of resources other than scenes.
- Of GDScript classes (spec §11.1): `new()`, static methods, constants and enums, inner classes; nodes in scenes typed by their GDScript class rather than its engine class.
- Members that are not pub of another instance of the same class, which the engine reaches by name while reflection lists only pub ones.
- The check of `@onready` members of value types read before ready.
- `Dictionary[K, V]`; StringName and NodePath literals.
- Constants of the engine class a class extends by their bare name, such as `NOTIFICATION_PREDELETE`, as GDScript has them in scope; now they need the class, `Object.NOTIFICATION_PREDELETE`.
- `Array[T]?`; `pop_front()` and `pop_back()` of `Array[T?]`, which give `T??`.
- Class annotations such as `@tool` and `@icon`.
- Values of type Variant (spec §3.13), which untyped members, parameters and results of GDScript classes have (§11.1); reading the elements of an untyped Array.
- Non-constant default values; static members; `_init()` with parameters.
- Assigning to an element, `a[i] = v`; calling a function value other than through `bind()`; `==` between objects.
- In debug builds, reporting where a freed instance was freed and where the instances still alive at exit were made (spec §3.3, §6.7, §12.5).

## Runtime

- A type descriptor for every type (architecture §2.2), replacing the Variant slots of objects and of `T?` of value types; `T??`, structs and enums with payloads need it. Objects as an ObjectID and a pointer, which ptrcall takes as `Object*`, belong to the same design.
- Members of classes of this language read and written without the engine.
- Temporary slots reused by scope, so that temporary references in a loop are released in time and a suspended coroutine keeps only the slots still in use; a temporary that held self, as in `f(self, await g())`, keeps a RefCounted alive while the coroutine waits (against spec §9.4).

## Editor

- A resident, incremental analysis that reads unsaved edits and does not walk the whole project at every check; the LSP, renaming and quick fixes build on it. Its cache should cover dependencies: compiled bytecode is now checked against its own source only, so a game run from the command line without a build keeps old bytecode after a change to a scene or to a script it uses, and only the runtime checks (signatures, `$X` classes) catch the difference.
- The scene index: read scenes unsaved in the editor; mark errors in scenes in the scene editor (spec §13.1); check whether it handles the nodes of the base of an inherited scene, and autoloads that are scenes.
- The debugger lists all locals of a function regardless of scope, so same-named locals of blocks side by side collide; the bytecode would record the scope of each slot.

## Tests

- Run an exported game with the official export templates; build and test the release library.

## Performance

Engine calls are by name with Variant arguments now, so code that calls the engine a lot is likely slower than typed GDScript, which resolves the MethodBind at compile time and makes a validated call with Variant arguments (modules/gdscript/gdscript_compiler.cpp:647-652, 705-710; ed1daf0).

- First, the engine-boundary benchmarks of architecture §4 against GDScript on the official build: reading and writing properties, engine methods with value-type arguments, signals.
- Engine methods: the MethodBind resolved when the class image loads (the bytecode records the class and the method's hash), and ptrcall with pointers to the registers, which already hold native values (gdextension_interface.h:2499 of godot-cpp); no boxing. GDScript cannot, its registers being Variants; it is as safe as GDScript's call of the MethodBind, since scripts cannot override engine methods that are not virtual.
- Engine properties: through their setter and getter by ptrcall. GDScript reads and writes them by name, with validated access only to members of built-in types (modules/gdscript/gdscript_byte_codegen.cpp:848-883). A script's `_set` or `_get` can take engine property names, so the fast path needs a check, such as whether the object's script defines them, cached per script.
- Engine singletons and autoloads resolved once: GET_SINGLETON looks the name up at every run, GET_AUTOLOAD calls get_node_or_null; a singleton's pointer can be kept, an autoload's ObjectID with a liveness check.
- Engine callbacks such as `_process(delta)`: no entry check of value-type arguments, whose types the engine guarantees.
- Calls between classes of this language: the method found by name once per call site, since a run keeps one version of each class (spec §12.4).
- Calls within a class: since inheritance they pass arguments the general way of calls between classes and repeat checks that loading made; the calls benchmark went from 18 to 23 ms.
- Engine calls: whether the result is a `GDScriptFunctionState`, which only methods of GDScript classes return (spec §9.8), is asked of every object result, by a call into the engine.
- Coroutines: freeing an instance, and every frame, walk all active tasks; tasks listed by their owner would walk only the owner's.
- Containers: the storage behind Array and Dictionary may be replaced later, keeping their public semantics (spec §3.8); the arrays benchmark loses to GDScript mostly to boxing each element into a Variant and back.
- Benchmarks of real workloads, which become representative after type descriptors and ptrcall; optimizations of the IR and code generation after those.

## Open questions

- Whether a setter that runs before ready should wait for ready on its own (spec §6.5), or the behavior should be written in the setter: the owner suspects that the implicit suspension makes setters hard to understand. To be discussed again before §6.5 is implemented.
- Whether diagnostics and semantic queries also serve programs, not only the editor's interface: saying which declaration or scene a constraint comes from and listing the references that broke could let AI and tools fix errors while reading less code.
- Whether the first version (spec §14) narrows: the FPS and Ring scripts of the second paper migration use none of user-defined generics, Result and `?`, enums, weak and the built-in test runner, and suspending setters before ready (spec §6.5) is costly to implement.
- The long-term boundary of compatibility with GDScript and Godot: which agreements are promises and which merely hold today, which Godot versions are supported, and which semantics may still change during the alpha.
- How to test the gain in maintenance: use the language in a real project through several rounds of changing requirements, and see how many problems it points out ahead of renames, interface changes and refactorings across files; the cost of adoption includes migrating in step by step, mixing and leaving, and with cross-language inheritance restricted, the unit of migration may be an inheritance cluster or a subsystem.
