# Petrichor design decisions

**2026-10-23** · 中文: [decisions.zh.md](decisions.zh.md)

As of 2026-10-23 the project is being redesigned from the problem up. This document records the decisions made so far; each states the rule first, then the rationale and evidence. The v0.1 specification is frozen as the git tag `spec-v0.1`; where it conflicts with this document, this document prevails. A new specification will be written from it. All engine source locations refer to Godot commit `469659e`.

## 1. Problem and positioning

- **Problem**: large projects written in a dynamic language are hard to manage. GDScript is bound by backward compatibility and cannot leave dynamic typing behind; more fundamentally, the toolchain is the problem: static information from scenes never reaches the scripts, so even when every script is statically typed, node paths, signals connected in the editor, animation tracks and resource property names remain dynamic. Switching to C# makes language support second-class; the bridge has hidden pitfalls, some engine capabilities are restricted, and the toolchain's integration with Godot has pitfalls of its own.
- **Goal**: fully static typing, with dynamic values only where the engine or a third party imposes them; the same experience extends into scenes.
- **Target users**: developers who read and modify code, on medium-sized or larger projects; part of the code may be AI-generated. Small projects are better served by GDScript and are not a target.
- **Usage**: mainly new projects; third-party plugins stay in GDScript and are not required to migrate.
- **Performance**: clearly faster than GDScript is enough; C# is not the target. Most scripts never hit the interpreter's performance limits.

## 2. Delivery and toolchain

- **Delivered as a GDExtension.** Code that touches the engine is kept in one thin adaptation layer, so that a module build remains possible later. Rationale: the [pitfall catalog](surveys/pitfalls.en.md) contains no entry that only a module could fix, and what a GDExtension cannot fix, a module cannot fix either; a GDExtension installs into the official editor and uses the official export templates. The hooks a script language needs are exposed to extensions (`core/object/script_language_extension.h:393, 493, 533, 596, 754`). Cost: web export needs "Extensions Support" enabled and cross-origin isolation headers configured.
- **The analysis service runs in the editor process.** External IDEs connect over LSP; the command line and CI run the same analysis headless. Rationale: refactorings that span scenes must modify scenes the editor has open, and only the editor can do that safely. GDScript's LSP is likewise an editor plugin (`modules/gdscript/language_server/gdscript_language_server.h:35`, `modules/gdscript/register_types.cpp:170`).
- **Refactorings include their scene half.** Mechanical, meaning-preserving edits are applied with the refactoring, previewed and undoable: renamed export property keys, method names in signal connections, animation track paths, and lossless value conversions. Edits that need judgment are only reported, with options, and never applied automatically, for example changing a node's type or an existing resource no longer matching a new type. Rationale: today, renaming an exported member silently loses the values saved in scenes. Loading does not check whether `node->set` succeeded (`scene/resources/packed_scene.cpp:567`), and saving writes only properties in the node's current property list (`packed_scene.cpp:941-950`).
- **First-version tools**:
  - live diagnostics, with the editor, LSP and command line using the same analysis;
  - completion, type on hover, go to definition, find references (including scenes);
  - rename including the scene part;
  - scene checks (`$X`, signal connections, `PackedScene[T]`, parent requirements), with errors shown in the scene editor;
  - quick fixes: insert the inferred type, add missing match arms, add an explicit conversion for a dynamic value;
  - a debugger wired into the engine's debugger UI: breakpoints, stepping, locals, call stack;
  - a bundled test runner: discovery, running, reporting, headless. Rationale: gdUnit4 and GUT require test classes to extend their GDScript base classes, and Godot does not support cross-language inheritance.
- **Later**: formatting; refactorings such as extract method, change signature and pull members up to a base class; deeper profiler integration; documentation generation.
- **Hot reload has two levels.** When only function bodies change, they are replaced while running; the next call uses the new code, and suspended coroutines finish on the old version. When members, signatures or class structure change, no state is migrated and a restart is requested; tool scripts in the editor get their instances rebuilt.

## 3. Scenes and nodes

- **Node types are inferred from scenes.** The compiler collects every scene structure a script can see, including inherited scenes and edits on instances, and types `$X`, `%X` and literal `get_node` paths from them. If structures disagree on a type, that is an error requiring an annotation. Evidence: in the game repositories of the [scene-structure survey](surveys/scene-structures.en.md), none of 2,023 literal paths resolves to different types in different structures.
- **Three cases are declared in the script**: children a base class requires and inherited scenes provide; children added at runtime; optional children, typed as Option. Any other path that cannot be found is an error, reported on the scene that lacks the node.
- **Upward access.** Autoload access via `/root/X` uses typed autoload global names instead. The types of `get_parent()`, `owner` and `..` come from the parent or owner requirement the script declares: checked statically where scenes instance the script, and at runtime when the node is attached to a parent (`NOTIFICATION_PARENTED`, `scene/main/node.h:466`). Without a declaration the type is Node and needs an explicit conversion.
- **Scene resources.** `preload` and `load` with a literal path are typed by the resource's actual content. An exported scene is written `PackedScene[T]`, and the editor checks the scene assigned to it; `instantiate()` yields T. `load` with a path known only at runtime yields Node and needs an explicit conversion.

## 4. Type system

- **Function signature types are written by hand, not inferred**; local variables inside functions may be inferred.
- **Numbers.** The first version has only `int` (64-bit) and `float` (64-bit), matching Variant (`core/variant/variant.h:257-258`); there are no narrow types for users, and fixed-width arithmetic uses wrapping and bit-operation methods on `int`. Writing to an engine f32 location narrows implicitly, for example real_t vector components, which are float in standard builds (`core/math/math_defs.h:143-146`). Passing a value to an engine int32 parameter is range-checked, and out-of-range values are a program error; the engine itself truncates there (`core/variant/variant.h:348-349`). Mixed `int`/`float` arithmetic promotes to `float`.
- **Narrowing** follows Kotlin's rule: locals and parameters narrow after `!= None`, `is` and early returns; members do not narrow and must first be read into a local. There is no truthiness; conditions must be bool. Non-null does not mean the object is still alive; access still checks.
- **Interfaces.** Nominal: a class must declare which interfaces it implements. Interfaces may declare methods, properties and signals, and may have default methods, which read and write state through interface properties. Only classes of this language can implement interfaces; foreign objects need an explicit wrapper. The first version has no stateful mixins. Evidence: in the [cross-branch duplication survey](surveys/cross-branch-duplication.en.md), duplication caused by different native base classes is about 4–10 lines per kLOC in most game repositories; by manual judgement, 185 of its 353 lines in games could be removed with composition or static functions, and 72 of the 102 lines suited to stateful mixins come from a single project, Ring. Duplication that inheritance could have removed but did not is larger, 4.9 times as much (between unrelated scripts on the same native base) and 8.2 times as much (along one inheritance chain), but it comes mostly from a single repository, tanks-of-freedom-ii.
- **Generics.** Functions, structs, enums, and RefCounted classes created only by code in this language may be generic; Node and Resource scripts and autoloads may not, because the engine instantiates them without type arguments. Constraints are expressed with interfaces. Type arguments are kept at runtime.
- **Encapsulation and names.** Members are private by default; the public interface is marked explicitly. Members the engine accesses by name (exported properties, signals, overrides of engine virtuals, methods connected in scenes) are not restricted for the engine. Class names use the engine's flat global table, and the compiler checks for duplicates, including clashes with GDScript plugin class names; the first version has no namespaces.

## 5. Dynamic boundary and engine bindings

- **Dynamic values have only three sources**: engine interfaces declared as Variant, parameters and return values left untyped in GDScript plugins, and messages from other languages.
- **Type at the source wherever possible.** Constraints the engine actually has but does not export are added in our own binding metadata; metadata the engine already exports is used directly. Any other dynamic value entering static code needs an explicit conversion, which reports an error on failure; there are no implicit checks.
- **Nullability of engine object returns.** Unaudited interfaces are nullable. Auditing proceeds in descending order of call frequency in the corpus and has three outcomes: non-null, typed T; null only on misuse, for example `get_tree()`, treated as non-null with a program error if null; legitimately nullable, for example `get_collider()`, typed T?. Rationale: only about 50 engine interface declarations use the `RequiredResult` annotation, 29 of them in Tween.
- **Member names as strings.** For `connect`, `emit_signal`, `call`, `call_deferred`, `set_deferred`, `has_method`, `Callable(obj, "m")`, `tween_property` and property paths such as `"position:x"`, a literal is resolved as a static reference when the target type is known: the name's existence and the value's type are checked, and it takes part in rename and find references. Which parameters are names is marked in our binding metadata. A non-literal name is explicit dynamic access.
- **Types as the engine and GDScript see them**:

  | This language | Seen by the engine and GDScript |
  |---|---|
  | `int`, `float`, `String`, classes such as Node | the type of the same name |
  | object `T` and `T?` | both the class; non-null is enforced by entry checks |
  | nullable values such as `int?` | untyped Variant |
  | `Array[T]`, `Dictionary[K, V]` with engine-representable elements | the engine's typed containers |
  | payload-less enum | int; shown as a drop-down when exported |
  | struct | Dictionary keyed by field name |
  | enum with payloads | Dictionary keyed by field name, with one reserved key holding the variant name |

  Evidence: GDScript types global classes of other languages through the Script reflection interface (`modules/gdscript/gdscript_analyzer.cpp:804-819, 4329-4370, 6054-6063`).

## 6. Data

- **Enums with payloads and exhaustive match.** Payload fields must be named, for example `Move(target: Vector3)`. Rationale: when a variant is added, the compiler lists every place that does not handle it; named fields keep the encoding independent of field order.
- **Structs** have value semantics and exist only inside this language. When crossing the engine boundary (signal arguments, Variant, messages) they are encoded automatically as Dictionaries keyed by field name and checked when decoded. In the first version they cannot be `@export`ed or stored in `.tres`; data edited in the editor uses Resource.
- **Containers.** There is one `Array[T]` and one `Dictionary[K, V]`, with the same API. When elements are engine-representable, it is the engine's container, shared with the engine; when elements are of a type only this language has, it uses its own representation, and passing it to the engine requires an explicit conversion, which makes a copy. Rationale: implicit copying would create pitfalls of the "you modified a copy" kind.
- **JSON.** The engine parses every JSON number as a float (`core/io/json.cpp:390-396`). Decoding into `int` accepts floats that are exactly integral and within int64 range, and otherwise reports an error instead of truncating.
- **The standard library** does not reimplement what the engine provides; it adds three things: typed wrappers produced by the binding audit, for example turning interfaces that return null on failure and report the error separately (such as `FileAccess.open`) into ones that return Result; support for this language's types, namely encoding and decoding structs and enums to and from JSON, `var_to_str` and ConfigFile, combinators on Option and Result, and map, filter and sort with typed lambdas on native containers; and the test runner. There is no package manager.

## 7. Errors, coroutines and signals

- **A program error** aborts the entire call chain of this language back to the entry point through which the engine called in; on the way out held references are released, the error and call stack are logged, and when running in the editor the debugger breaks; the game keeps running. Debug and release builds behave the same. If that entry point must return a value, the engine receives a default value. Expected failures are expressed with Result or Option. Rationale: after an error GDScript builds a default value for the return type and the caller continues (`modules/gdscript/gdscript_vm.cpp:3975-3979`); under static typing this would hand code fake values.
- **Coroutines are declared in the signature**, for example `async func`; a function whose body contains await without the declaration is a compile error. Calling a coroutine requires `await` or an explicit start-without-waiting form (spelling to be decided); a bare call is not allowed. Engine virtual functions may be declared async.
- **Setters and getters cannot be coroutines.** In a sample of 6 corpus setters that use `await ready`, every one reads the field's current value after resuming, meaning "synchronize state once ready". A declarative "apply when ready" form will be provided; its exact shape will be decided together with initialization order. Evidence: [property-accessor survey](surveys/property-accessors.en.md).
- **Cross-language await is supported in both directions.** A call to a GDScript method may be awaited: if a `GDScriptFunctionState` comes back at runtime, its `completed` signal is awaited and the result is checked against the declared return type; receiving one without await is a program error. Whether a function is a coroutine is not part of method reflection data (`core/object/method_info.h:37-44`), so it cannot be decided at compile time. When GDScript awaits a coroutine of this language, the coroutine's first suspension returns a Signal of a completion object held by the suspended frame. Both directions must be tested in an environment that can build the engine.
- **Signals and function values.** Signal parameters must be typed; emit and connect are checked statically; connections stored in scenes are checked by the scene model; engine class signals are typed from ClassDB, with unaudited object parameters nullable. Function types record parameters, return value and whether they are async. Lambdas capture by value, as in GDScript. A Callable of unknown signature needs an explicit conversion to a function type and is checked on every call.

## 8. Runtime

- **The interpreter must meet the performance goal on its own.** iOS and most console platforms do not allow generating machine code at runtime; a copy-and-patch style JIT is only an extra benefit on desktop.
- **Threads.** The virtual machine must run safely on whatever thread the engine chooses, for example background resource-loading threads and the physics thread. Data races on shared objects are the user's responsibility. The first version adds no threading API and uses the engine's Thread, WorkerThreadPool and Mutex. Evidence: across the 20 surveyed repositories, `Thread.new` occurs 11 times and `WorkerThreadPool` twice.

## 9. Syntax

- The surface follows GDScript: indentation, `func`, `var`, `extends`, `signal`, `@export`, `$X`. New constructs extend it in the same style, including structs, enums with payloads, match, interfaces, generics `[T]` and `T?`.
- **Hard rule**: a construct spelled as in GDScript either behaves as in GDScript or is a compile error with a suggested fix; it never behaves differently in silence.

## 10. Not yet decided

- Initialization order: the "apply when ready" form; unassigned non-null exported members; `_init` parameters (the engine always constructs instances with zero arguments); `@onready`; children running `_ready` before their parent.
- Object lifetime: access to freed objects; Node not being reference-counted; RefCounted cycles; destruction order.
- Resource sharing: loaded resources shared by every user; the depth of `duplicate()`; `local_to_scene`; loads failing on cyclic references.
- How to extend GDScript plugin classes, for example generating a GDScript shell for Beehave's ActionLeaf.
- Whether member variables may infer their type from an initializer; the spelling of start-without-waiting for coroutines; conversion between String and StringName; static members and static initialization.

## Evidence

Surveys: [scene structures](surveys/scene-structures.en.md), [cross-branch duplication](surveys/cross-branch-duplication.en.md), [pitfall catalog](surveys/pitfalls.en.md), [paper migration](surveys/paper-migration.en.md), [property accessors](surveys/property-accessors.en.md), [await signal results](surveys/await-signal-results.en.md).
