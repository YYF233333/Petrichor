# Backlog

In no fixed order. An item is removed as soon as it is done (AGENTS.md, Workflow).

## Language

- Signals and default methods of interfaces, and interfaces that extend others (spec §3.9).
- Array and dictionary patterns of match (spec §5.6).
- A struct or an enum with payloads that holds itself through a container (spec §3.6). The type table cannot list it, since a type's parts come before it.
- `Class.new` as a value (spec §3.11), as gdUnit4 passes `GdUnitTuple.new` to `callv`. It must construct as `new(...)` does. PetScript's `new` refuses a non-pub `_init` that the caller sees, so the two share the construction.
- Static checks of:
  - must-assign exports in scenes, in resources and at export (spec §7.5);
  - a statically known scene's `instantiate(...)` missing an export the scene does not assign (§7.4). The scene index does not read property values yet;
  - a child reading a member that ancestors assign in its own `_ready` (§6.2);
  - a must-assign member that is not pub, which its file or a subclass must export or assign (§4.2). It needs every subclass;
  - a scene used as a root whose root has a script with `@owner` (§7.3);
  - a node of a scene whose script is abstract (§4.1).
- `@parent(T)` (spec §7.3).
- `weak[T]` (spec §3.12).
- Preload cycles (spec §7.6); a literal path of `load` typed as preload types it (§7.4).
- Of GDScript classes (spec §11.1):
  - enums, inner classes and static variables;
  - nodes in scenes typed by their GDScript class rather than its engine class.
- preload() inside a constant's container (spec §4.5).
- `Array[T]?`, and nested `Option`:
  - `T??` and `Some(...)`, with `flatten()` (spec §3.4);
  - `pop_front()` and `pop_back()` of `Array[T?]` giving `T??`;
  - `map()` of `T?` whose function returns `U?`;
  - `ok()` and `err()` of `Result[T?, E]` and `Result[T, E?]`.
- The names of the engine's enums as values, as in `Tween.TransitionType.keys()` (spec §3.7).
- `Ok(v)`, `Err(e)`, `None` and `?` in a lambda without a return type passed to `and_then()` or `or_else()`. Nothing gives their `U` or `F` yet (spec §3.4, §3.5, §4.6).
- `for` over objects that define `_iter_init`, `_iter_next` and `_iter_get`, as GDScript allows; the checker reports it as not supported yet.
- The `breakpoint` statement.
- Rest parameters `...args: Array[T]` (spec §4.6).
- `for` over a String (spec §5.7); no file of the coverage survey's corpus does it.
- An inner class named as an engine enum, such as `Key`. A type reads the name as the engine's enum, an expression as the inner class (spec §4.1). So `func make() -> Key: return Key.new()` is an error.
- Node paths (spec §7.1):
  - into instanced scenes;
  - out of a scene from its root: `..` above the root, and `%Name` of a scene that instances it. Ring's paddle.gd uses one, `$%HealthIcon`.
- Static types (spec §7.1–§7.3, §7.6):
  - `get_node` and `get_node_or_null` typed from literal paths, exempting base scenes used only for inheritance;
  - `/root/X` offering the autoload name;
  - `Resource.duplicate()` keeping the static type.
- Type-specific fix hints on `not x` (spec §5.4).
- `pub` required on the members that animation tracks and synchronized properties name (spec §10.5). The compiler does not read them yet (spec §7.7).
- Literal member names checked (spec §10.4): existence and value types, rename and find references. Only method names on self resolve now. `Callable(self, "m")` and `Signal(self, "s")` are refused as if no constructor took them.
- The audit of the most frequently called engine APIs for null (spec §10.2); `binding_metadata.txt` has two entries.
- Overriding Object's `_get`, `_set`, `_get_property_list`, `_validate_property`, `_property_can_revert` and `_property_get_revert`. The engine reaches them through the script instance, and GDScript's instance calls them (`modules/gdscript/gdscript.cpp:1590, 1677, 1714, 1739, 1857, 1879`, `ed1daf0`). Now they are a compile error, since the runtime does not call them.
- Non-constant default values, of parameters and of struct fields.
  - Each comes after step 9, so it is evaluated outside the function body. The IR of the body keeps one entry.
- A `Callable` of unknown signature converted to a function type and checked on every call (spec §3.11).
- In debug builds, reporting two places (spec §3.3, §6.7, §12.5):
  - where a freed instance was freed;
  - where the instances still alive at exit were made.
- `Vector3.AXIS_X`: the checker rejects it.

## Runtime

- Double-precision builds of the engine (`precision=double`), whose constants spec §4.5 provides for. The bytecode lays out built-in values at their sizes in standard builds. So initialization and loading refuse such builds (`check_value_sizes()`).
- Temporary slots reused by scope. This releases temporary references in a loop in time. A suspended coroutine then keeps only the slots still in use.
- A temporary that holds self keeps a RefCounted alive while the coroutine waits, as in `f(self, await g())` (against spec §9.4). Self is needed after the await there, so reusing slots does not help. With the liveness of registers, such a register can drop self before the await and read it again after.
- Ordering of arrays of engine types as spec §5.2 says. It differs from the engine's operators only with NaN, so it waits for the open question on NaN.
- Range checks of ints passed through an engine method's value, as `f.call(x)` (spec §10.6). The value is the engine's Callable, which converts without them.

## Editor

- References from scenes for the keys of exported properties (architecture §2.1). They come once the scene index reads the values scenes give properties. Now only the methods of signal connections are references.
- The warnings of spec §6.7, with a `weak` quick fix where they break a cycle:
  - a lambda capturing self stored in a member of self, a signal connection or a container;
  - two RefCounted classes referring to each other through strong fields;
  - a node from `new()` or `instantiate()` that some path neither adds to the tree, returns, stores nor frees;
  - the result of `remove_child` neither stored nor freed.
- The hint of spec §6.5 on an exported property whose setter syncs a node. Its default value never passes the setter, so `_ready` should sync it once.
- The warnings of spec §7.2 and §7.6:
  - a literal `get_node_or_null` path that no scene has;
  - a scene that nothing references lacking a node;
  - writes to a resource known to be shared.
- The error explaining why a GDScript file cannot extend a class of this language (spec §11.2).
- A quick fix adding the constructor for the implicit conversions of GDScript that this language drops (spec §3.14).
- When lints are added, consider a hint on ordering arrays whose elements can be NaN. GDScript's results differ there (spec §5.2).
- Compiled bytecode is checked against its own source only. A game run from the command line without a build keeps old bytecode. That happens after a change to a scene or to a script it uses. Only the runtime checks (signatures, `$X` classes) then catch the difference. The analysis knows what each script read, but keeps it only in the editor's memory (architecture §2.1).
- The scene index: read scenes unsaved in the editor, which are marked as last saved (architecture §2.4). Check whether it handles autoloads that are scenes.
- Completion of the names in strings that wait on their checks (spec §10.4, §13.2). They are property names, as in `set_deferred("speed", v)`, members of other objects, as in `other.connect("hit", f)`, and the paths of `get_node("...")`.
- Completing a bare name takes about 75 ms in the test project, GDScript about 21 ms. Our check takes 19 ms of it. The editor spends most of the rest on about 2550 options. GDScript's list lacks about 470 editor classes.
- `_can_make_function`, so that the signal connection dialog generates the callback (editor/scene/connections_dialog.cpp:505-512; ed1daf0). It writes `pub func`, as connected methods need (spec §10.5).
- A `Foreign` `DeclId` names the script accessed, not the one that declares the member. Finding references (step 8) needs the declaring one.
- Declarations that are not pub show in the editor's help beyond their own file. The engine's documentation has no visibility (`core/doc_data.h:698-720`, `ed1daf0`). Keeping them to their own file needs support from the engine, to be sought later.
  - Structs, interfaces and inner classes have documentation classes of their own, which the whole project searches (`editor/doc/editor_help_search.cpp:113`).
  - Enums and members are listed on their class's page.
- The debugger lists all locals of a function regardless of scope. So same-named locals of blocks side by side collide. The bytecode would record the scope of each slot.

## Tests

- Engine tests on a .NET build for classes written in C# (spec §11.1). The suite runs without .NET now.
- Run an exported game with the official export templates; build and test the release library.
- Cross-rule checks kept from the former conformance checklist:
  - `player.last_hit.damage = 3` running `last_hit`'s setter (spec §3.6, §4.4);
  - for §3.11 and §9.1:
    - function-type parameter contravariance;
    - rejecting `async` for an ordinary function parameter;
    - connecting an `async` method to a signal;
  - awaiting a `Signal` variable with two parameters yielding `Array` in `Variant` (§9.6);
  - a scene-saved value triggering a pre-ready setter error, and `if is_node_ready():` guarding it (§6.5);
  - `RefCounted` `PREDELETE` self field/method access, plus `self == x` and `await` errors (§6.6);
  - GDScript assigning a cancelled `Transform3D` coroutine result (§9.8);
  - a lambda created before a body-only hot reload running its old body (§12.4).

## Performance

Engine calls now go by name with Variant arguments. So code that calls the engine a lot is likely slower than typed GDScript. Typed GDScript resolves the MethodBind at compile time and makes a validated call with Variant arguments (`modules/gdscript/gdscript_compiler.cpp:647-652, 705-710`, `ed1daf0`).

- Engine methods without boxing. GDScript cannot do this, its registers being Variants.
  - The MethodBind is resolved when the class image loads. The bytecode records the class and the method's hash.
  - ptrcall takes pointers to the registers, which already hold native values (`gdextension_interface.h:2499` of godot-cpp).
  - This is as safe as GDScript's call of the MethodBind. Scripts cannot override engine methods that are not virtual.
- Engine properties: through their setter and getter by ptrcall. GDScript reads and writes them by name, with validated access only to members of built-in types (`modules/gdscript/gdscript_byte_codegen.cpp:848-883`). A script's `_set` or `_get` can take engine property names. So the fast path needs a check, cached per script, such as whether the object's script defines them.
- Engine singletons and autoloads resolved once. Now GET_SINGLETON looks the name up at every run, and GET_AUTOLOAD calls get_node_or_null. A singleton's pointer can be kept, and an autoload's ObjectID with a liveness check.
- Engine callbacks such as `_process(delta)`: no entry check of value-type arguments, whose types the engine guarantees.
- Calls between classes of this language: the method found by name once per call site. A run keeps one version of each class, which allows this (spec §12.4). Now these are loaded through ResourceLoader at every use:
  - a static method's script;
  - the script `Class.new()` sets;
  - the class of a static variable that another class or an inner class uses (spec §4.8).
- Calls within a class: since inheritance, they pass arguments the general way of calls between classes. They also repeat checks that loading made. The calls benchmark went from 18 to 22 ms (architecture §4).
- Function values: `f.call(x)` goes through the engine's `Callable.call` with Variant arguments. A struct is encoded and decoded on the way. A lambda or method value of this language could be called directly, its arguments in their slots' layout.
- Engine calls: every object result is asked whether it is a `GDScriptFunctionState`, by a call into the engine. Only methods of GDScript classes return one (spec §9.8).
- Coroutines: freeing an instance, and every frame, walk all active tasks. Tasks listed by their owner would walk only the owner's.
- Containers. The arrays benchmark loses to GDScript mostly to boxing each element into a Variant and back.
  - The check that a container is not read-only before each change is a call into the engine (spec §3.8). It takes about 7 ns of an append.
  - Escape analysis can give containers that do not escape a private representation (architecture §2.2). The engine container stays the reference semantics.
  - A private representation supports some operations. A container that uses others keeps the engine's. Each private operation is tested against the engine's.
- Constants and counting loops. float_loop has little margin over GDScript, and vectors only a modest one (architecture §4).
  - Every evaluation of a literal loads it into a new temporary with an instruction. GDScript addresses constants as operands (`modules/gdscript/gdscript_byte_codegen.h:366-367`).
  - A counting `for` runs six instructions per iteration. GDScript iterates with one (`modules/gdscript/gdscript_vm.cpp:3471-3492`, `ed1daf0`).
- Benchmarks of real workloads after ptrcall; optimizations of the IR and code generation after those. In the init function, instructions that can run other code are barriers to them.
- Liveness of the IR's registers (`src/compiler/ir.h`), one analysis on the IR before any optimization.
  - It serves the warning `modified_copy` (spec §3.6), which is off until then.
  - It also serves what a suspended coroutine keeps (the Runtime item on temporary slots).
  - The receiver of a method value in a temporary stays until its statement ends (spec §3.11).
  - Each instruction's reads and writes go in the opcode table beside its schemas. Writing a View writes part of its root.
  - The warning takes a write to a field of a struct loop variable, after which the variable is not live. The write may also go through a value type's subscript, as `h.pos[0] = 1.0`.
  - A read in an inner loop's next iteration keeps the variable live, as `while h.count > 0: h.count -= 1`. So does a lambda's capture.
  - `continue`, `break` and `return` end its paths.
  - The warning shows only for files without errors, which have IR. The checker's former cases are in `git show 3e63b1c:tests/compiler/test_warnings.cpp`.

## Open questions

- How `NaN` behaves in containers, which GDScript handles inconsistently (spec §5.2, engine facts). NaN rarely stands for a legal state in game logic. Candidates:
  - forbid `NaN` from entering containers;
  - warn wherever a `NaN` is detected.
  A survey of the corpus decides. Until then, spec §5.2 marks it a known limitation.
- Whether setters that run before ready wait for ready on their own after the first version (spec §6.5, §14.2). The alternative is writing the behavior in the setter. The owner suspects that the implicit suspension makes setters hard to understand.
- Exporting enums with payloads, which 0.1 needs (spec §3.7). Architecture §5 still puts it after the first version, designed in §6.2. How is settled with the toolchain (step 8).
- What the name of an enum with payloads gives as a value (spec §3.7). Spec §3.7 gives every enum name `keys()`, `values()`, `find_key()` and `get()`. `values()` cannot list a variant with fields. The checker now rejects the name as a value.
- Whether an autoload's name stands for its script's class in a type, as GDScript's does (`modules/gdscript/gdscript_analyzer.cpp:806-826`). godot-open-rts writes `var p: Constants.Match.Player` 5 times, `Constants` being an autoload (`docs/surveys/corpus-counts/inner_classes.py`).
- Whether diagnostics and semantic queries also serve programs, not only the editor's interface. They could say which declaration or scene a constraint comes from, and list the references that broke. That could let AI and tools fix errors while reading less code.
- Whether the first version (spec §14) leaves out `weak[T]` (spec §3.12) and the built-in test runner (§13.2). The FPS and Ring scripts of the second paper migration use neither.
- Whether a container that holds itself is a program error as a `Dictionary` key (spec §3.8). The engine stops hashing it at a depth of 100 with an error. One that holds itself twice takes about 2^100 steps, in GDScript too (`core/variant/array.cpp:195-208`, `ed1daf0`).
- The long-term boundary of compatibility with GDScript and Godot:
  - which agreements are promises and which merely hold today;
  - which Godot versions are supported;
  - which semantics may still change during the alpha.
- Tuples, to be discussed after the first version. `await Task.all([...])` gives nothing (spec §9.5). With tuples it could give the results of tasks of different types, which an array cannot type.
- How to test the gain in maintenance. Use the language in a real project through several rounds of changing requirements. See how many problems it points out ahead of renames, interface changes and refactorings across files. The cost of adoption includes migrating in step by step, mixing and leaving. With cross-language inheritance restricted, the unit of migration may be an inheritance cluster or a subsystem.
