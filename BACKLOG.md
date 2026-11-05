# Backlog

In no fixed order. An item is removed as soon as it is done (AGENTS.md, Workflow).

## Language

- Coroutine helpers `Task.of`, `delay`, `any`, `all`, `settled`; async lambdas and calls of async function values, `f.call(x)` giving a `Task[T]` (spec §9, §3.11).
- Signals and default methods of interfaces, and interfaces that extend others (spec §3.9).
- Of match (spec §5.6): array and dictionary patterns; a match over `T?`, with `None` and `var x` and the check that it is exhaustive; the patterns of structs, and those of enums, Result and Settled once they exist.
- Of structs (spec §3.6): methods; the "modifies a copy" warning for a field of a struct loop variable; a struct that holds itself through a container, which the type table cannot list, a type's parts coming before it.
- `T.try_from(v)` (spec §5.3), as for `Array[Hit].try_from(Array(hits))`.
- The error for a type that is not pub, such as a struct or an interface, in the signature of a pub member (spec §4.2).
- Static checks of must-assign exports in scenes, in resources and at export (spec §7.5), of a scene used as a root whose root has a script with `@owner` (§7.3), and of a node of a scene whose script is abstract (§4.1).
- `@parent(T)` (spec §7.3).
- Setters that wait for ready on their own (spec §6.5, after the first version), once the open question below is settled.
- A warning for removed nodes that are neither stored nor freed, and one with a `weak` quick fix for a lambda capturing self stored in a member of self (spec §6.7, §15); `weak[T]` (§3.12).
- Preload cycles (spec §7.6); a literal path of `load` typed as preload types it (§7.4).
- Of GDScript classes (spec §11.1): enums, inner classes and static variables; nodes in scenes typed by their GDScript class rather than its engine class.
- The declarations of other scripts are checked as while the editor loads a script, which loads no script of another language, so a member typed by a preloaded GDScript class without a class_name, as `pub var unit: Unit?`, is not known to other classes (spec §2.1).
- Members that are not pub of another instance of the same class, which the engine reaches by name while reflection lists only pub ones.
- The check of `@onready` members of value types read before ready.
- preload() inside a constant's container (spec §4.5).
- In the editor, GDScript does not see a pub constant whose value is a preload(), such as `Spawner.SCENE`, and reports a parse error: the editor compiles the scripts it loads with declarations only, which read no scenes, so the constant is left out of the image (spec §4.5, §11.3).
- `Array[T]?`; `pop_front()` and `pop_back()` of `Array[T?]`, which give `T??`.
- Enums with payloads, and the names of the engine's enums as values, as in `Tween.TransitionType.keys()` (spec §3.7); `T.try_from(x)` (spec §5.3).
- Rest parameters `...args: Array[T]` (spec §4.6).
- `for` over a String (spec §5.7); no file of the coverage survey's corpus does it.
- Static functions as values: one of a class around an inner class by its bare name, or one of another class as `Class.f`; now they can only be called (spec §3.11, §4.1).
- Inner classes of two files that extend each other's classes, as `A.X extends B.Y` with `B.Y extends A.Z`: the checker accepts them, but no image can be loaded before the other's, so the editor reports it and the game cannot load the base.
- A text form of this language's own types, such as `Hit(damage = 3, crit = false)`, for `str()`, `print()` and `%` together; now they read as their encoding (spec §5.1, §10.1).
- Members that must be assigned and are not exported, and passing them by name to `instantiate(...)` and `new(...)` (spec §6.2, §4.6).
- Node paths into instanced scenes, and out of a scene from its root: `..` above the root, and `%Name` of a scene that instances it, as Ring's paddle.gd uses `$%HealthIcon` (spec §7.1).
- Property names as literals, as in `tween_property(self, "speed", ...)` and `set_deferred("speed", v)`: checked, and visible to the engine (spec §10.4, §10.5); only method names are.
- The audit of the most frequently called engine APIs for null (spec §10.2); `binding_metadata.txt` has two entries.
- Overriding Object's `_get`, `_set`, `_get_property_list`, `_validate_property`, `_property_can_revert` and `_property_get_revert`, which the engine reaches through the script instance and GDScript's instance calls (modules/gdscript/gdscript.cpp:1590, 1677, 1714, 1739, 1857, 1879; ed1daf0); now a compile error, since the runtime does not call them.
- Non-constant default values, of parameters and of struct fields.
- A `Callable` of unknown signature converted to a function type and checked on every call (spec §3.11).
- In debug builds, reporting where a freed instance was freed and where the instances still alive at exit were made (spec §3.3, §6.7, §12.5).

## Runtime

- Members of classes of this language read and written without the engine; then a pub member whose type is a container that keeps boxes can leave reflection (spec §10.5), through which this language now reaches members of other instances.
- Temporary slots reused by scope, so that temporary references in a loop are released in time and a suspended coroutine keeps only the slots still in use; a temporary that held self, as in `f(self, await g())`, keeps a RefCounted alive while the coroutine waits (against spec §9.4).

## Editor

- References from scenes for the keys of exported properties (architecture §2.1), once the scene index reads the values scenes give properties; now only the methods of signal connections are.
- The formatter and lints (spec §13.2, architecture §2.1).
- Compiled bytecode is checked against its own source only, so a game run from the command line without a build keeps old bytecode after a change to a scene or to a script it uses, and only the runtime checks (signatures, `$X` classes) catch the difference; the analysis knows what each script read (architecture §2.1) but keeps it only in the editor's memory.
- The scene index: read scenes unsaved in the editor, which are marked as last saved (architecture §2.4); check whether it handles autoloads that are scenes.
- Completion and go to definition in the editor (`_complete_code`, `_lookup_code`; spec §13.2), on the recovered syntax tree (architecture §2.1): the compiler now skips the checker when parsing fails. `_can_make_function`, so that the signal connection dialog generates the callback (editor/scene/connections_dialog.cpp:505-512; ed1daf0).
- The debugger lists all locals of a function regardless of scope, so same-named locals of blocks side by side collide; the bytecode would record the scope of each slot.

## Tests

- Run an exported game with the official export templates; build and test the release library.
## Performance

Engine calls are by name with Variant arguments now, so code that calls the engine a lot is likely slower than typed GDScript, which resolves the MethodBind at compile time and makes a validated call with Variant arguments (modules/gdscript/gdscript_compiler.cpp:647-652, 705-710; ed1daf0).

- Engine methods: the MethodBind resolved when the class image loads (the bytecode records the class and the method's hash), and ptrcall with pointers to the registers, which already hold native values (gdextension_interface.h:2499 of godot-cpp); no boxing. GDScript cannot, its registers being Variants; it is as safe as GDScript's call of the MethodBind, since scripts cannot override engine methods that are not virtual.
- Engine properties: through their setter and getter by ptrcall. GDScript reads and writes them by name, with validated access only to members of built-in types (modules/gdscript/gdscript_byte_codegen.cpp:848-883). A script's `_set` or `_get` can take engine property names, so the fast path needs a check, such as whether the object's script defines them, cached per script.
- Engine singletons and autoloads resolved once: GET_SINGLETON looks the name up at every run, GET_AUTOLOAD calls get_node_or_null; a singleton's pointer can be kept, an autoload's ObjectID with a liveness check.
- Engine callbacks such as `_process(delta)`: no entry check of value-type arguments, whose types the engine guarantees.
- Calls between classes of this language: the method found by name once per call site, since a run keeps one version of each class (spec §12.4); a static method's script, the script `Class.new()` sets and the class of a static variable that another class or an inner class uses (spec §4.8) are now loaded through ResourceLoader at every use.
- Calls within a class: since inheritance they pass arguments the general way of calls between classes and repeat checks that loading made; the calls benchmark went from 18 to 22 ms (architecture §4).
- Function values: `f.call(x)` goes through the engine's `Callable.call` with Variant arguments, a struct encoded and decoded on the way; a lambda or method value of this language could be called directly, its arguments in their slots' layout.
- Engine calls: whether the result is a `GDScriptFunctionState`, which only methods of GDScript classes return (spec §9.8), is asked of every object result, by a call into the engine.
- Coroutines: freeing an instance, and every frame, walk all active tasks; tasks listed by their owner would walk only the owner's.
- Containers: the storage behind Array and Dictionary may be replaced later, keeping their public semantics (spec §3.8); the arrays benchmark loses to GDScript mostly to boxing each element into a Variant and back, and the check that a container is not read-only before each change (spec §3.8) is a call into the engine, about 7 ns of an append. Escape analysis can give containers that do not escape a private representation, the engine container being the reference semantics (architecture §2.2): a private representation supports some operations, a container that uses others keeps the engine's, and each private operation is tested against the engine's.
- Constants and counting loops: every evaluation of a literal loads it into a new temporary with an instruction, and a counting `for` runs six instructions per iteration, where GDScript addresses constants as operands (modules/gdscript/gdscript_byte_codegen.h:366-367) and iterates with one instruction (modules/gdscript/gdscript_vm.cpp:3471-3492; ed1daf0); float_loop has little margin over GDScript and vectors only a modest one (architecture §4).
- Benchmarks of real workloads after ptrcall; optimizations of the IR and code generation after those.

## Open questions

- Whether a setter that runs before ready should wait for ready on its own (spec §6.5), or the behavior should be written in the setter: the owner suspects that the implicit suspension makes setters hard to understand. To be discussed again before §6.5 is implemented.
- Whether an autoload's name stands for its script's class in a type, as GDScript's does (`modules/gdscript/gdscript_analyzer.cpp:806-826`): godot-open-rts writes `var p: Constants.Match.Player` 5 times, `Constants` being an autoload (`docs/surveys/corpus-counts/inner_classes.py`).
- Whether diagnostics and semantic queries also serve programs, not only the editor's interface: saying which declaration or scene a constraint comes from and listing the references that broke could let AI and tools fix errors while reading less code.
- Whether the first version (spec §14) leaves out `weak[T]` (spec §3.12) and the built-in test runner (§13.2), which the FPS and Ring scripts of the second paper migration do not use.
- The long-term boundary of compatibility with GDScript and Godot: which agreements are promises and which merely hold today, which Godot versions are supported, and which semantics may still change during the alpha.
- How to test the gain in maintenance: use the language in a real project through several rounds of changing requirements, and see how many problems it points out ahead of renames, interface changes and refactorings across files; the cost of adoption includes migrating in step by step, mixing and leaving, and with cross-language inheritance restricted, the unit of migration may be an inheritance cluster or a subsystem.
