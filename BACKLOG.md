# Backlog

In no fixed order. An item is removed as soon as it is done (AGENTS.md, Workflow).

## Language

- Static checks of must-assign exports in scenes, in resources and at export (spec §7.5).
- A warning for removed nodes that are neither stored nor freed (spec §15).
- Preload cycles (spec §7.6), and preload of resources other than scenes.
- Inheritance between classes of this language; the static view of GDScript classes (spec §11.1).
- Members that are not pub of another instance of the same class, which the engine reaches by name while reflection lists only pub ones.
- The check of `@onready` members of value types read before ready.
- `Dictionary[K, V]`; StringName and NodePath literals; `self` as a value.
- `Array[T]?`; `pop_front()` and `pop_back()` of `Array[T?]`, which give `T??`.
- Class annotations such as `@tool` and `@icon`.
- Reading the elements of an untyped Array.
- Default values other than literals; static members; `_init()` with parameters.
- Assigning to an element, `a[i] = v`; calling a function value other than through `bind()`; `==` between objects.
- In debug builds, reporting where a freed instance was freed and where the instances still alive at exit were made (spec §3.3, §6.7, §12.5).
- What else Ring needs to run ([second paper migration](docs/surveys/paper-migration-2.en.md)): structs, property accessors and setters before ready (spec §6.5), coroutines, property requirements in interfaces, `@owner`, match, `const`. Ring running could be the second milestone, with real code deciding what comes first, as for the FPS kit.

## Runtime

- A type descriptor for every type (architecture §2.2), replacing the Variant slots of objects and of `T?` of value types; `T??`, structs and enums with payloads need it.
- Engine methods called through ptrcall.
- Members of classes of this language read and written without the engine.
- Temporary slots reused by scope, so that temporary references in a loop are released in time and a suspended coroutine keeps only the slots still in use.
- With await: unsubscribing from the notifications of freed objects.

## Editor

- A resident, incremental analysis that reads unsaved edits and does not walk the whole project at every check; the LSP, renaming and quick fixes build on it. Its cache should cover dependencies: compiled bytecode is now checked against its own source only, so a game run from the command line without a build keeps old bytecode after a change to a scene or to a script it uses, and only the runtime checks (signatures, `$X` classes) catch the difference.
- The scene index: read scenes unsaved in the editor; mark errors in scenes in the scene editor (spec §13.1); check whether it handles the nodes of the base of an inherited scene, and autoloads that are scenes.
- The debugger lists all locals of a function regardless of scope, so same-named locals of blocks side by side collide; the bytecode would record the scope of each slot.

## Tests

- Run an exported game with the official export templates; build and test the release library.

## Performance

- Benchmarks of real workloads, which become representative after type descriptors and ptrcall; optimizations of the IR and code generation after those.

## Open questions

- Whether diagnostics and semantic queries also serve programs, not only the editor's interface: saying which declaration or scene a constraint comes from and listing the references that broke could let AI and tools fix errors while reading less code.
- Whether the first version (spec §14) narrows: the FPS and Ring scripts of the second paper migration use none of user-defined generics, Result and `?`, enums, weak and the built-in test runner, and suspending setters before ready (spec §6.5) is costly to implement.
- The long-term boundary of compatibility with GDScript and Godot: which agreements are promises and which merely hold today, which Godot versions are supported, and which semantics may still change during the alpha.
- How to test the gain in maintenance: use the language in a real project through several rounds of changing requirements, and see how many problems it points out ahead of renames, interface changes and refactorings across files; the cost of adoption includes migrating in step by step, mixing and leaving, and with cross-language inheritance restricted, the unit of migration may be an inheritance cluster or a subsystem.
