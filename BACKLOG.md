# Backlog

In no fixed order. An item is removed as soon as it is done (AGENTS.md, Workflow).

## Language

- Of `_init` on Node and Resource scripts (spec §6.2):
  - A member that `_init` in another file assigns counts as unassigned, since the checker does not check that body. So `instantiate(...)` asks for one that the root's `_init` assigns. A child's `_ready` may not read one that a base's `_init` assigns.
  - A scene or `.tres` using a script whose `_init` has required parameters is a compile error. Now the engine's construction gives the program error.
- A must-assign member without pub that no code of its file or its subclasses writes (spec §4.2). It needs each file's list of the inherited members it writes, and the subclasses of a class.
- Assignments by ancestors (spec §6.2), which the checker assumes without looking for them.
  - It takes a must-assign member of a node with an owner as one they assign. Exported members and those `_init` assigns are not.
  - The spec requires `$Child.target = v` or an `@onready` reference in their `_ready`. Without one, the member must be `T?`.
- Checking at compile time what resources store in exported structs and enums with payloads (spec §4.3). Also their exported scenes and must-assign exports (§7.4, §7.5). They are `.tres` files and resources inside scenes, which the scene index does not read. Nor is a struct checked that a scene stores on a node of an instanced scene it changes.
- `T??` and `Variant?` wherever the engine boundary would encode them, a compile error (spec §10.1). The checker refuses `T??` only in pub members and parameters, signal parameters and coroutine results. These still lose the inner `None` silently:
  - `Variant?` in those places;
  - either in a struct, an enum with payloads or a `Result` that crosses, and in `Array(a)`;
  - the result of an async lambda, and the arguments of `bind()`.
- `find_key()` of `Dictionary[T?, V]` for an object `T`. The engine gives null for a missing key and for a None key.
- `is_instance_of(v, T)` with a native class held in a variable, which waits for classes as values (spec §2.6). gdUnit4 passes them.
- A scene used as a root through `load`, `preload` or an exported `PackedScene` (spec §7.2, §7.3, §7.5). Only the main scene is known as one. Finding the others reads every script and every scene, beyond a check's inputs now (architecture §6.4). Until then:
  - the node paths of a script take a scene that is also inherited for a base used only for inheritance. It may then lack a node without an error, which the run-time check catches;
  - a root with an `@owner` script is a compile error on the main scene only. Elsewhere reading `owner` gives the program error;
  - a must-assign export without a value is a compile error only in what the main scene is built from. Elsewhere the node's check at ready gives the program error.
- Members that animation tracks and synchronized properties name, unchecked for `pub` (spec §10.5):
  - through a path that leaves the scene storing it, as from an AnimationPlayer at its root;
  - after the first subname, on a resource of this language, as `health` in `Body:stats:health`.
- In debug builds, listing where the instances still alive at exit were made (spec §6.7, §12.5).

## Runtime

- Double-precision builds of the engine (`precision=double`), whose constants spec §4.5 provides for. The bytecode lays out built-in values at their sizes in standard builds. So initialization and loading refuse such builds (`check_value_sizes()`).
- Ordering of arrays of engine types as spec §5.2 says. It differs from the engine's operators only with NaN, so it waits for the open question on NaN.
- Range checks of ints passed through an engine method's value, as `f.call(x)` (spec §10.6). The value is the engine's Callable, which converts without them.

## Editor

- References from scenes for the keys of exported properties (architecture §2.1). Also for the members that animation tracks and synchronized properties name (spec §7.7). Now only the methods of signal connections are references.
- The warnings of spec §6.7:
  - a lambda capturing self stored in a member of self, a signal connection or a container;
  - two RefCounted classes referring to each other through strong fields;
  - a node from `new()` or `instantiate()` that some path neither adds to the tree, returns, stores nor frees;
  - the result of `remove_child` neither stored nor freed.
- The hint of spec §6.5 on an exported property whose setter syncs a node. Its default value never passes the setter, so `_ready` should sync it once.
- The warnings of spec §7.2, §7.5 and §7.6:
  - a literal `get_node_or_null` path that no scene has;
  - a scene that nothing references lacking a node or a value for a must-assign export. A lacking node is now an error, as for a scene used as a root (Language);
  - writes to a resource known to be shared.
- The error explaining why a GDScript file cannot extend a class of this language (spec §11.2).
- A quick fix adding the constructor for the implicit conversions of GDScript that this language drops (spec §3.14).
- When lints are added, consider a hint on ordering arrays whose elements can be NaN. GDScript's results differ there (spec §5.2).
- When lints are added, consider a warning on code that relies on the order of assignment (spec §3.6). An example is `arr[i] = next()` with `next()` assigning `i`. The owner considers such code harmful.
- When lints are added, consider a hint on assignments to must-assign members right after `new()`. Named arguments of `new(...)` would let the compiler check them (spec §6.2).
- When lints are added, consider a warning on names spelled as this language's keywords, such as `None` (spec §2.2). They are legal but not recommended.
- Compiled bytecode is checked against its own source only. A game run from the command line without a build keeps old bytecode. That happens after a change to a scene or to a script it uses. Only the runtime checks (signatures, `$X` classes) then catch the difference. The analysis knows what each script read, but keeps it only in the editor's memory (architecture §2.1).
- The typed inspector panel for exported structs and enums with payloads (architecture §6.2). Until then the engine's generic Dictionary editor edits them.
- The scene index: read scenes unsaved in the editor, which are marked as last saved (architecture §2.4). Check whether it handles autoloads that are scenes.
- Completion of the names in strings that wait on their checks (spec §10.4, §13.2). They are property names, as in `set_deferred("speed", v)`, members of other objects, as in `other.connect("hit", f)`, and the paths of `get_node("...")`.
- Completing a bare name takes about 75 ms in the test project, GDScript about 21 ms. Our check takes 19 ms of it. The editor spends most of the rest on about 2550 options. GDScript's list lacks about 470 editor classes.
- `_can_make_function`, so that the signal connection dialog generates the callback (editor/scene/connections_dialog.cpp:505-512; ed1daf0). It writes `pub func`, as connected methods need (spec §10.5).
- A `Foreign` `DeclId` names the script accessed, not the one that declares the member. Finding references (step 8) needs the declaring one.
- Declarations that are not pub show in the editor's help beyond their own file. The engine's documentation has no visibility (`core/doc_data.h:698-720`, `ed1daf0`). Keeping them to their own file needs support from the engine, to be sought later.
  - Structs, interfaces and inner classes have documentation classes of their own, which the whole project searches (`editor/doc/editor_help_search.cpp:113`).
  - Enums and members are listed on their class's page.
- The two-way migration tool (spec §1.2, §13.4), wanted in 0.2 or 0.3.
  - Reverse conversion writes types GDScript can express. A nested typed container becomes `Array[Array]`, and a narrower result such as `map`'s goes through `assign()`.
  - It drops this language's names from `@warning_ignore`, which GDScript rejects (`modules/gdscript/gdscript_parser.cpp:5126-5128`, `ed1daf0`).
  - The report lists by-name access with names that are not literals, and `get_name()` on signals. Their members may need `pub` (spec §10.5).
  - Migrating a class offers to migrate its GDScript users that GDScript could not keep (spec §11.3). They call its static methods or use its enums in types. `docs/surveys/corpus-counts/class_level_access.py` finds them by spelling.
  - It rewrites assignments to a character of a String, a compile error here (spec §5.1).
    - `s[i] = c` becomes `s = s.erase(i).insert(i, c)`, and `s[i] = ""` becomes `s = s.erase(i)`.
    - A negative literal index `-k` becomes `s.length() - k`, since `erase` refuses negative positions (`core/string/ustring.cpp:2881-2884`, `ed1daf0`).
    - The report lists other indices and bases with side effects.
- The debugger lists all locals of a function regardless of scope. So same-named locals of blocks side by side collide. The bytecode would record the scope of each slot.
- Before 0.9, diagnostics and semantic queries that serve programs, not only the editor's interface. They say which declaration or scene a constraint comes from, and list the references that broke. Agents then fix errors while reading less code. Accepted by an agent evaluation (architecture §5, 0.9).
- Before 0.9, function times in Godot's profiler. The `_profiling_*` methods of `PetLanguage` are empty, so the profiler shows no function of this language. Deeper integration comes after 1.0 (spec §14.2).

## Tests

- Before 0.1, engine tests on a .NET build for classes written in C# (spec §11.1). The suite runs without .NET now, and the README says C# code works alongside.
- Run an exported game with the official export templates; build and test the release library.
- Before 0.1, run the suites on Godot 4.8's release candidate and check its changes one by one. One known: 4.8 rejects an extension class that binds a method its ancestor has ([godot-cpp#2083](https://github.com/godotengine/godot-cpp/issues/2083)).
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

Engine methods with a MethodBind, also the accessors of engine properties, are called through its ptrcall (architecture §2.2). Vararg methods and other languages' methods and properties still go by name with Variant arguments.

- Engine singletons and autoloads resolved once. Now GET_SINGLETON looks the name up at every run, and GET_AUTOLOAD calls get_node_or_null. A singleton's pointer can be kept, and an autoload's ObjectID with a liveness check.
- Engine callbacks such as `_process(delta)`: no entry check of value-type arguments, whose types the engine guarantees.
- Calls between classes of this language: the method found by name once per call site. A run keeps one version of each class, which allows this (spec §12.4). Now these are loaded through ResourceLoader at every use:
  - a static method's script;
  - the script `Class.new()` sets;
  - the class of a static variable that another class or an inner class uses (spec §4.8).
- Calls within a class: since inheritance, they pass arguments the general way of calls between classes. They also repeat checks that loading made. The calls benchmark went from 18 to 22 ms (architecture §4).
- Function values that still go through the engine, with Variant arguments: struct method values and `async` functions. A struct is encoded and decoded on the way.
- Calls by name: every object result is asked whether it is a `GDScriptFunctionState`, by a call into the engine. Only methods of GDScript classes return one (spec §9.8).
- Default values built anew. Each call constructs every slot whose default is not zero, such as an Array's. CLEAR builds such a default again for each register it clears (spec §6.8). Two remedies:
  - registers whose scopes do not overlap share slots, as GDScript pools its temporaries (`modules/gdscript/gdscript_byte_codegen.cpp:122-131`, `ed1daf0`);
  - CLEAR copies one shared empty container instead of making one.
- Optimizations from what static analysis knows, keeping the meaning. Lowering does not know which classes are RefCounted, so CLEAR clears every object register (spec §6.8). An object whose static type is not RefCounted, such as a Node, holds no reference, and its clear can go.
- Containers. Reading, writing and iterating over an Array's elements call the engine once each (architecture §2.2).
  - Built-in methods with a Variant result, such as `pop_back()`, take the general way of a call. So do methods that handle elements or keys. It costs about 11 ns more than a plain call.
  - A Dictionary read calls the engine twice, for `has()` and the lookup, after the key is made a Variant.
  - The check that a container is not read-only before each change is a call into the engine (spec §3.8).
  - Escape analysis can give containers that do not escape a private representation (architecture §2.2). The engine container stays the reference semantics.
  - A private representation supports some operations. A container that uses others keeps the engine's. Each private operation is tested against the engine's.
- An interpreter whose speed does not depend on code layout. The compute benchmarks could then gate changes again (architecture §4).
  - Changing unrelated functions before `Execution::run` slowed int_loop by 24% on AMD Zen 5.
  - Placing `run` first in `.text` (`__declspec(code_seg(".text$a"))`) steadied float_loop, members and vectors, but not int_loop.
  - Candidates:
    - dispatch without one shared indirect jump, as computed goto or tail calls do (architecture §2.2);
    - placing the hot handlers.
- Instructions left in compute code, which already beats GDScript by 2 to 3.5 times.
  - Each evaluation of a String literal copies it with an instruction. Other literals are preset in the frame or read from the constant table.
  - Each statement starts with a LINE instruction, one of six per iteration of int_loop. GDScript debug builds also emit one per statement (`modules/gdscript/gdscript.cpp:2871-2875`, `ed1daf0`). Release builds emit none.
    - A table from code positions to lines would replace it.
    - An error would look its line up by position. The 52 handler calls that pass a line would drop it.
    - Every engine error captures the backtraces of all languages (`core/error/error_macros.cpp:126`, `ed1daf0`). So a frame would save its position before each instruction that can call the engine. Lua saves it only before such instructions. CPython 3.11 and later save it at every instruction.
    - Under the debugger, breakpoints, stepping and the editor's break button need a call at each line (`core/debugger/engine_debugger.h:128-134`, `ed1daf0`). The debugger is on or off for the whole process. When it is off, the runtime would drop LINE as it loads the code.
    - The compiler could record each instruction's statement in the table. Such a table would also fix a wrong line. From the second pass on, an error in a `while` condition is reported at the body's last statement. GDScript does the same (`modules/gdscript/gdscript_compiler.cpp:1903`, `ed1daf0`).
- Benchmarks of real workloads; optimizations of the IR and code generation after those. In the init function, instructions that can run other code are barriers to them.
- Liveness of the IR's registers (`src/compiler/ir.h`), one analysis on the IR before any optimization.
  - It serves the warning `modified_copy` (spec §3.6), which is off until then.
  - It also serves native code's register allocation (architecture §5).
  - Each instruction's reads and writes go in the opcode table beside its schemas. Writing a View writes part of its root.
  - The warning takes a write to a field of a struct loop variable, after which the variable is not live. The write may also go through a value type's subscript, as `h.pos[0] = 1.0`.
  - A read in an inner loop's next iteration keeps the variable live, as `while h.count > 0: h.count -= 1`. So does a lambda's capture.
  - `continue`, `break` and `return` end its paths.
  - The warning shows only for files without errors, which have IR. The checker's former cases are in `git show 3e63b1c:tests/compiler/test_warnings.cpp`.

## Upstream

- GDScript's per-iteration clearing of loop-body locals, left as commented-out TODOs (`modules/gdscript/gdscript_compiler.cpp:2106, 2140`, `ed1daf0`). It would match spec §6.8. The owner may contribute it.
- Store-forwarding stalls in moves of Variant, found while profiling arrays (d516c08). A wide load reads a Variant just written by narrower stores. It then waits for those stores to reach the cache. The owner may contribute both fixes once the current work is done.
  - godot-cpp: the move constructor and assignment are out of line and swap the bytes (`src/variant/variant.cpp:90-92, 551-554`, `8070599`).
    - MSVC builds have no LTO by default, so no caller can inline them (`tools/windows.py:214-217`).
    - Every moved Variant therefore goes through memory. In one profile of Array element writes, 38% of the time was in the move.
    - Inline in the header, they could copy the bytes and set the source to NIL.
  - Godot: `Array::set()` copies the value, validates the copy and moves it into place (`core/variant/array.cpp:517-523`, `ed1daf0`).
    - For an int, the copy writes 8 bytes of `_data`, and the move reads all 16 (`core/variant/variant.cpp:1148-1150`, `core/variant/variant.h:853-861`, `ed1daf0`).
    - GDScript spends 54% of a loop writing `Array[int]` elements stalled there. `push_back()` has the same shape (`core/variant/array.cpp:282-287`, `ed1daf0`).
    - A candidate fix: `reference()` copies the whole `_data` for types that need no deinit.
  - Each patch needs a benchmark that shows its gain. `docs/development.md` describes the profiling.
- Godot: a script's `_get` and `_set` take engine property names in GDScript's `.` access, not in calls of their methods. On 4.7.2, `label.text = s` goes to a `_set` that takes `text`, and `label.set_text(s)` does not. Object's docs say these methods are not called for built-in properties (`doc/classes/Object.xml:39, 303`, `29a3f56701`). The owner may report it as an issue, or contribute a PR that makes the docs and the behavior agree.
  - The order comes from the first public commit (`core/object/object.cpp:233-259, 316-338`, `0b806ee0fc9`). GDScript instances call `_set` and `_get` for every name that is not a member (`modules/gdscript/gdscript.cpp:1589-1600, 1676-1685`, `ed1daf0`).
  - [#99828](https://github.com/godotengine/godot/issues/99828) is about a script's own properties, which these methods do not see. It likely led to the note in the docs.
- Godot: in the official Windows builds, each read of a `thread_local` goes through GCC's emulated TLS. The owner may report it as an issue.
  - Each read calls winpthreads' `pthread_getspecific`, as disassembly of the official 4.7.2 binary shows. That call saves the last error, takes a spin lock and restores the error.
  - Node's thread guards make such reads in each guarded method of debug builds (`scene/main/node.h:694-716, 934-948`, `core/os/thread_safe.cpp:36-38`, `ed1daf0`).
  - In the engine_method benchmark, each `Node3D.translate()` makes about four such reads. They take about two thirds of each iteration, in GDScript as in this language.
  - Candidate fixes: native TLS in the official builds, or guards that read fewer thread-locals.
- godot-cpp: each object an extension constructs makes a StringName from its engine class's name. The owner may contribute a fix.
  - The default constructor of an engine class passes its name as a C string (`include/godot_cpp/classes/wrapped.hpp:436, 516`, `8070599`).
  - Wrapped's constructor takes a StringName, so each construction converts it (`wrapped.hpp:133`, `8070599`). The engine then looks the name up under a lock.
  - In coroutines.pet, when each call made a CoroutineTask, this took 1.8% of the loop. The whole construction took 14.5%.
  - A candidate fix: pass the class's cached `get_class_static()` instead.
- Godot: the open [#124220](https://github.com/godotengine/godot/pull/124220) changes how the script editor takes an extension's errors and warnings. The owner may comment before it merges; nothing here changes until then.
  - It groups every error that names a path into one diagnostic on the last line. The editor does not underline it (`core/object/script_language_extension.h:425-436`, `editor/script/script_text_editor.cpp:756-758`, `05ee117`).
  - On 4.7 an extension must name its own script there, as this one does (`editor/script/script_text_editor.cpp:859-860`, `ed1daf0`; `src/adapter/pet_language.cpp:199`).
  - It also puts the extension's one-based lines into zero-based ranges, so warnings move down a line. GDScript's own conversion subtracts one (`modules/gdscript/gdscript_editor.cpp:160-164`, `05ee117`).
  - Once it merges, one build would need different output per engine version. A fix in the wrapper keeps the contract: take the script's own path as its own, and subtract one.
- Views of the enums, inner classes and static variables of GDScript classes, a later addition (spec §11.1, §14.2). They need a change in Godot first, which the owner may contribute.
- Godot: String has no method that replaces the character at a position. The owner may contribute one.
  - Its methods replace by content, insert or erase (`core/variant/variant_call.cpp:2039-2048`, `ed1daf0`).
  - Here `s[i] = c` is a compile error, and its fix uses `erase()` and `insert()` (spec §5.1).
  - Gaps in the methods of engine types go upstream before the standard library (architecture §5).

## Open questions

- Whether a scene's track or synchronized property is a compile error in these cases (spec §7.7). The engine reports them only as the scene runs (`scene/animation/animation_mixer.cpp:724-727`, `modules/multiplayer/multiplayer_synchronizer.cpp:42`, `ed1daf0`).
  - Its path leads to no node.
  - It names a member that the node's class of this language lacks.
- Whether a scene known statically converts as an assignment does where spec §7.4 does not list it. Its `instantiate(...)` then skips the exports the scene does not assign, which the node's ready check catches. Unchecked now:
  - a function returning it as `PackedScene[T]`;
  - an element of a container, as `[preload("enemy.tscn")]`;
  - a parameter's default value.
- Before 0.9: how `NaN` behaves in containers, which GDScript handles inconsistently (spec §5.2, engine facts). NaN rarely stands for a legal state in game logic. Candidates:
  - forbid `NaN` from entering containers;
  - warn wherever a `NaN` is detected.
  A survey of the corpus decides. Until then, spec §5.2 marks it a known limitation.
- Whether setters that run before ready wait for ready on their own after the first version (spec §6.5, §14.2). The alternative is writing the behavior in the setter. The owner suspects that the implicit suspension makes setters hard to understand.
- Whether the first version (spec §14) leaves out the built-in test runner (§13.2). The FPS and Ring scripts of the second paper migration do not use it.
- Before 0.9: whether a container that holds itself is a program error as a `Dictionary` key (spec §3.8). The engine stops hashing it at a depth of 100 with an error. One that holds itself twice takes about 2^100 steps, in GDScript too (`core/variant/array.cpp:195-208`, `ed1daf0`).
- The long-term boundary of compatibility with GDScript and Godot: which agreements are promises and which merely hold today. How semantics may change is settled (architecture §5, 0.9).
- Tuples, to be discussed after the first version. `await Task.all([...])` gives nothing (spec §9.5). With tuples it could give the results of tasks of different types, which an array cannot type.
- How to test the gain in maintenance. Use the language in a real project through several rounds of changing requirements. See how many problems it points out ahead of renames, interface changes and refactorings across files. The cost of adoption includes migrating in step by step, mixing and leaving. With cross-language inheritance restricted, the unit of migration may be an inheritance cluster or a subsystem.
