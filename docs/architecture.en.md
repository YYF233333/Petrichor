# Petrichor implementation architecture

**Draft · 2026-10-25** · 中文：[architecture.zh.md](architecture.zh.md)

This document records implementation decisions: how the components are split, how values are represented, testing, and the order of work. The [language specification](language-spec.en.md) is authoritative for language rules. When the implementation reveals a problem in the specification, the specification is changed in a separate commit; the implementation does not relax its rules. Engine facts are checked against Godot 4.7.2-stable (commit `ed1daf0`).

## 1. Decisions

- **C++17, godot-cpp, SCons.** The interpreter has to run on iOS and consoles (spec §12.2), and an engine-module build may come later (§1.3). The engine is compiled as C++17 (`SConstruct:916, 921`) and engine modules can only be built with SCons, so this project uses both. Since v10, godot-cpp is versioned independently of Godot and one version can target several Godot versions; this project pins it to a commit and builds it with `api_version=4.7`.
- **The target engine is Godot 4.7.** The extension declares 4.7 as its minimum version, and the engine loads extensions whose minimum version is not above its own (`core/extension/gdextension_library_loader.cpp:315-329`), so the same build also runs on later versions.
- **Exported games contain only bytecode** (spec §1.3). A `.gdextension` file selects different libraries for the editor and for export templates by feature tags (`gdextension_library_loader.cpp:82-96`): the editor library includes the compiler, the export-template library only the runtime.
- **Registers hold native values according to their static types.** Even where static types are known, GDScript's registers are Variants (`modules/gdscript/gdscript_vm.cpp:536`), its operators run through function pointers on Variants (`:848-862`), and engine method calls take Variant pointers as well (`:2273-2313`). This language uses its fully static types to avoid these costs, which is the main source of being "clearly faster than GDScript" (spec §12.2).

## 2. Components

| Component | Depends on | Contents |
|---|---|---|
| compiler | nothing from Godot | lexing and parsing, semantic analysis, scene checks, typed IR, bytecode generation, diagnostics, symbol index |
| runtime | godot-cpp | bytecode loading, the virtual machine, values and type descriptors, tasks, program errors, checks and conversions at the engine boundary |
| adapter | godot-cpp, runtime; the editor library also compiler | the three extension interfaces for the script language, scripts and script instances, resource loading and saving, the export plugin, the editor plugin, the headless command |

### 2.1 compiler

- It includes no Godot headers; the build does not give compiler sources the godot-cpp include paths. Its tests are therefore plain C++ programs that do not start the engine, and it can be used unchanged in an engine-module build.
- External information is obtained through interfaces:
  - the engine API: classes, methods, properties, signals, enums, and the operator table of the built-in types;
  - the project: scenes, resources, autoloads, global classes;
  - the static view of GDScript classes (spec §11.1);
  - binding review data (§10.3), kept as data files in the repository.
  In the editor and in headless mode the adapter implements these interfaces through ClassDB, ResourceLoader and Script's reflection interface; in tests they are implemented from the `extension_api-4-7.json` shipped with godot-cpp and from scenes built in the tests. In the editor the engine API must come from ClassDB, because the classes of other extensions and the API of newer engine versions are not in that JSON.
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
- Calls to engine methods pass pointers to native values directly to ptrcall; calls coming from the engine pass the entry checks (§10.1).
- The virtual machine can run on any thread and uses no global mutable state that is not protected by a lock (§12.3).

### 2.3 adapter

- It implements ScriptLanguageExtension, ScriptExtension and ScriptInstanceExtension, and forwards property reads and writes, method calls and notifications of instances to the runtime.
- Loading and saving of `.pet` sources and bytecode files.
- Export plugin: it replaces `.pet` with bytecode, creating the remap through `add_file(..., true)` as GDScript does for `.gdc` (`modules/gdscript/register_types.cpp:99-116`).
- Editor plugin: the analysis service (§13.1), error marks in the scene editor, renaming with the scene part and undo (§13.3), compiling before running.
- Headless command: runs the same analysis and exits with a non-zero code on compile errors (§13.1).

### 2.4 Repository layout

```
SConstruct
src/compiler/          no Godot dependency
src/runtime/
src/adapter/           editor parts in src/adapter/editor/
tests/compiler/        doctest, as in the engine's unit tests
tests/engine/          a Godot project run in headless mode
thirdparty/godot-cpp/  git submodule
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
4. **First milestone**: the FPS code of the second paper migration actually runs. Features are added as needed, and the corresponding rows of §15 become tests at the same time.
5. The plan after that is made once the first milestone is done, including Ring, LSP and renaming, migration quick fixes, and performance benchmarks.

## 6. Not decided yet

- a JIT;
- caching for incremental analysis;
- details of the LSP;
- the final name and file extension (spec §1.4); `.pet` is used until then.
