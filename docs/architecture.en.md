# Petrichor implementation architecture

**Draft · 2026-10-25** · 中文: [architecture.zh.md](architecture.zh.md)

This document records implementation decisions: how the components are split, how values are represented, testing, and the order of work. The [language specification](language-spec.en.md) is authoritative for language rules. When the implementation reveals a problem in the specification, the specification is changed in a separate commit; the implementation does not relax its rules. Engine facts are checked against Godot 4.7.2-stable (commit `ed1daf0`).

## 1. Decisions

- **C++17, godot-cpp, SCons.** The interpreter has to run on iOS and consoles (spec §12.2), and an engine-module build may come later (§1.3). The engine is compiled as C++17 (`SConstruct:916, 921`) and engine modules can only be built with SCons, so this project uses both. Since v10, godot-cpp is versioned independently of Godot and one version can target several Godot versions; this project pins it to a commit and builds it with `api_version=4.7`. This project raises its C++ standard only after the engine does: C++20 brings only conveniences, and the guaranteed tail calls an interpreter may use are compiler extensions, independent of the standard.
- **The target engine is Godot 4.7.** The extension declares 4.7 as its minimum version, and the engine loads extensions whose minimum version is not above its own (`core/extension/gdextension_library_loader.cpp:315-329`), so the same build also runs on later versions.
- **Exported games contain only bytecode** (spec §1.3). A `.gdextension` file selects different libraries for the editor and for export templates by feature tags (`gdextension_library_loader.cpp:82-96`): the editor library includes the compiler, the export-template library only the runtime.
- **Registers hold native values according to their static types.** Even where static types are known, GDScript's registers are Variants (`modules/gdscript/gdscript_vm.cpp:536`), its operators run through function pointers on Variants (`:848-862`), and engine method calls take Variant pointers as well (`:2273-2313`). This language uses its fully static types to avoid these costs, which is the main source of being "clearly faster than GDScript" (spec §12.2).
- **When run from the editor, the game reads the bytecode the editor has written and does not compile** (§2.4).

## 2. Components

| Component | Depends on | Contents |
|---|---|---|
| compiler | nothing from Godot | lexing and parsing, semantic analysis, scene checks, typed IR, bytecode generation, diagnostics, symbol index |
| bytecode | nothing from Godot | the format of compiled code, its serialization and its verification, shared by the compiler and the runtime |
| runtime | godot-cpp | bytecode loading, the virtual machine, values and type descriptors, tasks, program errors, checks and conversions at the engine boundary |
| adapter | godot-cpp, runtime; the editor library also compiler | the three extension interfaces for the script language, scripts and script instances, resource loading and saving, the export plugin, the editor plugin, the headless command |

### 2.1 compiler

- It includes no Godot headers; the build does not give compiler sources the godot-cpp include paths. Its tests are therefore plain C++ programs that do not start the engine, and it can be used unchanged in an engine-module build.
- External information is obtained through interfaces:
  - the engine API: classes, methods, properties, signals, enums, and the operator table of the built-in types;
  - the project: scenes, resources, autoloads, global classes;
  - the static view of GDScript classes (spec §11.1);
  - binding review data (§10.3), kept as data files in the repository (`src/compiler/binding_metadata.txt`).
  In the editor and in headless mode the adapter implements these interfaces: engine classes come from ClassDB, because the classes of other extensions and the API of newer engine versions are not in the `extension_api-4-7.json` shipped with godot-cpp; scenes come from the scene states the engine reads (SceneState), so text and binary scenes and instances are read as the engine reads them; GDScript classes come from Script's reflection interface. Built-in types (operators, members, methods, constructors) and utility functions are the exception: ClassDB does not describe them and the engine offers an extension no way to list them, so the editor and the tests both use tables generated from the JSON at build time; they do not vary with extensions, and newer engines only add to them. The engine's own nullability metadata (RequiredResult and RequiredParam, spec §10.2) is in the same position: ClassDB's method information leaves it out (`core/object/method_info.cpp:37-51`), so it is generated from the JSON too, merged with the review data; classes of other extensions follow the default rules of §10.2. The tests currently describe engine classes and scenes by hand.
- Pipeline: lexing → syntax tree → semantic analysis (names, types, narrowing, match, generics, interfaces, visibility) → scene checks (§7) → typed IR → bytecode, debug information and class metadata.
- The IR consists of explicit basic blocks and virtual registers; it is not SSA. Every instruction has a type and a source position. Syntactic sugar is expanded when the IR is generated: the early return of `?`, the write-back of chained assignment (§3.6), the suspension points of await, the branches of match, and the suspension of setters before ready (§6.5). Bytecode corresponds almost one to one to the IR, with virtual registers allocated to slots in the stack frame.
- The first version re-analyses the whole project each time, without incrementality. Each phase depends only on its own inputs, so caches can be added later as needed without changing interfaces.

### 2.2 runtime

- Values: int and float take 8 bytes each; the engine's built-in value types are stored in place and copied and destroyed by the engine's rules; String, Array and Dictionary use the engine's types; Variant is one slot type; Option, structs and enums with payloads have their own layouts with a discriminant.
- Objects: an object that is not RefCounted is stored as an ObjectID and a pointer and checked for liveness on access (spec §3.3); RefCounted is held by a strong reference; `weak` holds no reference (§3.12).
- Each type has a type descriptor that provides copying, moving, destruction, and conversion to and from Variant (§10.1, §10.8). The compiler emits a slot type table for each stack frame; destruction, moving the frame of a suspending coroutine, and the debugger's display of locals all rely on it.
- Compiled functions are immutable and versioned. Hot reload of a function body replaces the class's function table; running and suspended frames keep using the old version (§12.4).
- Coroutine frames live on the heap and form a tree by ownership, along which cancellation propagates (§9.3). Notice that an object has been freed comes from the runtime's own instance binding: an object can hold several instance bindings with different tokens at once (`core/object/object.cpp:2167-2201`), so the runtime's binding does not conflict with godot-cpp's; whether to report an error is decided in the script language's per-frame callback (§9.7).
- A program error unwinds this language's call stack, releasing locals, back to the engine's call entry, which returns the default value of its type (§8.1).
- Calls to methods of classes of this language do not go through the engine: the virtual machine finds the object's instance of this language, finds the method by name, checks that the method's signature is the one the caller recorded when it was compiled, and passes arguments and result in their slots' native layout (§8.1). The scripts of one run are always compiled together (§12.4); only bytecode that was not built together can make the signatures differ, and the call is then a program error. Calls to engine methods pass pointers to native values directly to ptrcall; calls coming from the engine pass the entry checks (§10.1).
- The virtual machine can run on any thread and uses no global mutable state that is not protected by a lock (§12.3).
- The first version dispatches with a switch only, so there is one code path to test on every platform. GDScript uses computed goto on GCC and Clang (`modules/gdscript/gdscript_vm.cpp:254-450`); this language's main advantage lies in registers typed by static type, not in dispatch. Each instruction's handler is written once and the interpreter's state is kept in a few local variables, so computed-goto or tail-call dispatch can later be generated from the same definitions; whether to add them is decided by the benchmarks in §4. Switch dispatch stays in any case, since not every toolchain guarantees tail calls, MSVC debug builds for one.

### 2.3 adapter

- It implements ScriptLanguageExtension, ScriptExtension and ScriptInstanceExtension, and forwards property reads and writes, method calls and notifications of instances to the runtime.
- Loading and saving of `.pet` sources and bytecode files.
- Export plugin: it replaces `.pet` with bytecode, creating the remap through `add_file(..., true)` as GDScript does for `.gdc` (`modules/gdscript/register_types.cpp:99-116`).
- Editor plugin: the analysis service (§13.1), error marks in the scene editor, renaming with the scene part and undo (§13.3), compiling before running.
- Headless command: runs the same analysis and exits with a non-zero code on compile errors (§13.1).

### 2.4 Editing, running, export and hot reload

- Editing: the analysis service holds the compiled state of the whole project in the editor process, recompiles when sources or scenes change, and shows diagnostics in the script editor and the scene editor.
- When the editor loads a script, it compiles only the declarations (class header, members, signatures): enough for the reflection of exported properties and methods, but not runnable. At that point the scenes that use the script cannot be loaded, since the engine refuses such a load on the same thread as cyclic (`core/io/resource_loader.cpp:1049-1056`). The full compilation, with function bodies and scene checks, happens outside of loading: checks while editing, saving, the build before running, and export. Compiling only the declarations reads no scenes, so a member typed from them (such as `@onready var camera = $Head/Camera`) is a Variant until the full compilation. Only tool scripts run this language's code in the editor; when they are supported, a full compilation follows the load.
- Running from the editor: the project is compiled before it runs (the editor plugin's `_build`) and the bytecode is written to `.godot/petrichor/`; the game process only reads it. Rationale: the types of `$X` and the like depend on scenes (spec §7), so a game process compiling by itself would analyse all of the project's scenes again, start more slowly, and might reach different results from the editor.
- The headless command writes bytecode to `.godot/petrichor/` as well; tests and CI use it in place of the editor.
- Export: the export plugin puts the bytecode in the exported package (§1).
- Bytecode carries no version number before the first release. A file carries an identifier of its format, a hash of `src/bytecode/` computed at build time, and is read only by builds with the same identifier; the editor library and the export templates built from the same sources therefore agree. The build before running recompiles every script, so the cache needs no other check of freshness.
- Hot reload: when a script is saved, the editor recompiles it and tells the game over the debugger connection, and the game reloads the script from `.godot/petrichor/`. When only function bodies changed, running instances switch to the new code; when declarations changed, the game keeps the code it has and applies no reload until it runs again (spec §12.4).

### 2.5 Repository layout

```
SConstruct
src/compiler/          no Godot dependency
src/bytecode/          no Godot dependency, shared by the compiler and the runtime
src/runtime/
src/adapter/           editor parts in src/adapter/editor/
src/probe/             the step-2 probes, in non-release builds only
tests/compiler/        doctest, as in the engine's unit tests
tests/engine/          a Godot project run in headless mode; fps/ holds the migrated scripts of the FPS project
thirdparty/godot-cpp/  git submodule
thirdparty/starter-kit-fps/  git submodule: the FPS project at the commit used by the second paper migration
```

## 3. Testing

Tests come from three sources, the specification, the engine and real code, and are not derived from the implementation:

- **Specification conformance**: each row of §15 becomes tests. Compile-time checks go into the compiler's tests, which do not start the engine; run-time behavior goes into the engine tests.
- **Engine compatibility**: a Godot test project run in headless mode checks attaching, saving and reloading scripts, calls between GDScript and this language in both directions, and lifecycles: the owner freed during an await, the signal source freed, a signal emitted synchronously followed by another suspension, the same suspension point resumed twice, and GDScript calling this language, which then awaits an external signal.
- **Real code**: the FPS and Ring code of the [second paper migration](surveys/paper-migration-2.en.md) actually runs.

## 4. Performance

Four groups of benchmarks compare against GDScript, measuring the virtual machine itself and the cost of crossing the engine boundary separately:

| Benchmark | Measures |
|---|---|
| Startup and compilation | cold start, repeated runs, re-running after a code change |
| Pure computation | integer and float loops, arrays, function calls, generics |
| Engine boundary | property reads and writes, engine method calls, Callable, signals |
| Allocation | enums with payloads, closures, coroutine suspension, object creation |

Comparisons use optimized builds, not `dev_build`. These data decide whether a JIT is needed and which paths to optimize.

## 5. Order of work

1. **Engine integration**, without the language: the extension loads in the 4.7.2 editor; a `.pet` script can be created and attached to a node, and still works after the scene is saved and reopened; hard-coded behavior responds to `_ready`; properties and methods appear in the inspector and can be called from GDScript; the export plugin replaces the file. Acceptance: a real scene runs in the editor and after export.
2. **Probes**: verify on a running engine the points of the specification that were derived only from source:
   - undo merging with `history_changed` and `MERGE_ALL` (spec §13.3);
   - cross-language await in both directions (§9.8);
   - GDScript's static checking of this language's classes: the types of `new` and of members (§11.3);
   - the runtime's own instance binding receives a callback when the object is freed, without affecting godot-cpp's binding (§9.7).
   If a probe fails, the question goes back to the specification.
3. **Minimal compilation chain**: a subset of the language with `extends Node`, typed members, functions, int and float arithmetic, if and while, calls to engine methods, and `_process`. Acceptance: the script runs attached in a scene, GDScript can call it, a breakpoint stops, and a change to a function body hot-reloads.
4. **First milestone**: the FPS code of the second paper migration actually runs. Features are added as needed, and the corresponding rows of §15 become tests at the same time. The engine tests copy the project and replace each migrated script; the scripts not yet migrated stay GDScript, and the migrated run may only report the errors that the original reports and keep at exit only the resources that the original keeps. Scripts are migrated in the order cloud, impact, hud, audio, weapon, then combat, enemy and player together, so that this language never calls into GDScript and the static view of GDScript classes (spec §11.1) is not needed for this milestone. The last three go together because the enemy recognizes the player by the interface Combat.Damageable, which only this language's classes can implement (spec §3.9): with enemy migrated alone, the enemies would no longer hurt the player, and the tests would not notice. Done: all eight scripts run migrated. Left for later steps: the static checks of exports that must be assigned, in scenes and resources and at export (spec §7.5), of which the check at ready is done; the warning for removed nodes that are neither stored nor freed, which would have reported the weapon models the original leaks (§15); preload cycles (§7.6) and preload of resources other than scenes; inheritance between classes of this language and the static view of GDScript classes (§11.1); members that are not pub of another instance of the same class, which the engine reaches by name while reflection lists only pub ones; the check of `@onready` members of value types read before ready; `Dictionary[K, V]`, StringName and NodePath literals, and `self` as a value.
5. The plan after that is made once the first milestone is done, including Ring, LSP and renaming, migration quick fixes, and performance benchmarks.

## 6. Not decided yet

- a JIT;
- caching for incremental analysis;
- details of the LSP;
- the final name and file extension (spec §1.4); `.pet` is used until then.
