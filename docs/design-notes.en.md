# Petrichor Design Notes

Non-normative. 中文：[design-notes.zh.md](design-notes.zh.md).

These notes collect design rationale and implementation proposals. The [language specification](language-spec.en.md) defines program validity and observable behavior.

## Design goals

The project aims to keep type and memory-management annotations limited while making assignment, sharing, conversion, invalidation and errors explicit. The specification defines semantics before choosing algorithms. Representations visible to the engine are covered by the language contract.

The design favors restrictions that can later be relaxed compatibly. Capabilities without a demonstrated need are deferred rather than optimized in advance.

## Generics and execution

The current execution sketch starts with a typed IR interpreter; JIT execution is a later possibility. The implementation language, IR, JIT backend and distribution choices remain undecided.

The generic implementation sketch passes a runtime type descriptor for each type parameter. It contains size, copy and destruction operations and trait method tables, allowing one copy of generic code. A descriptor instantiated with Object would apply the ownership rules in specification §5.1.

Descriptor-based indirect calls are one way to execute implementations selected by compile-time constraints. IR-level monomorphization and JIT specialization are alternatives or later optimizations. This sketch does not make descriptors part of the language semantics or settle the implementation choice.

## Inference and incremental compilation

A proposed dependency-discovery algorithm requests other declarations' signatures as needed during inference. In `func f(): return make_b().g()`, inferring that `make_b` returns B reveals the dependency on `B.g`.

A declaration can be marked as being inferred. A request that reaches such a declaration is a back edge; a Tarjan-style stack can merge the affected declarations into a strongly connected component (SCC). Results are published when the component completes; failure discards its provisional results.

The proposed module coordinator handles joint inference independently of the engine loading and validating resources one at a time. This approach does not require a new engine joint-compilation interface or changes to core.

An incremental implementation can track dependencies, reverse dependencies and caches. An initial implementation may conservatively recompile the whole project. Unchanged interfaces allow reuse of callers' compiled results. These are implementation strategies; specification §11 requires valid results and atomic publication, without prescribing a stack, coordinator or cache.

## Static-initialization rationale

Specification §8.5 requires completed values to be read and an error when execution dependencies cannot be satisfied. This contract does not prescribe a scheduler or initialization state machine, and does not permit temporary defaults to hide dependency cycles.

The following evidence comes from upstream reports and review of code changes, not local reproductions. It does not establish that the reported problems remain in the selected evaluation version:

- [#104161](https://github.com/godotengine/godot/issues/104161) and [#82141](https://github.com/godotengine/godot/issues/82141) report nested-class static reads or instance initialization preceding required static initialization. Nesting therefore does not replace execution-dependency constraints.
- [#104588](https://github.com/godotengine/godot/issues/104588) reports inconsistent parse errors involving resource loading and static-initialization cycles. Type dependencies and initialization-execution dependencies need separate treatment.
- [#77098](https://github.com/godotengine/godot/issues/77098) reports repeated setter calls during static-field initialization. Declaration initialization does not invoke setters.
- [#105667](https://github.com/godotengine/godot/issues/105667) reports hot reload performing initialization side effects before restoring old static values. The first version does not support hot reload; ordinary access after failure also does not automatically retry initialization.
- [PR #120976](https://github.com/godotengine/godot/pull/120976), merged on 2026-07-13, addresses shutdown lifetimes involving statically held lambdas, suspended functions and the script runtime. Code and runtime dependencies must remain available during value cleanup; the specification does not adopt the PR's cleanup passes.

## Property design rationale

Specification §8.6 splits properties into stored and computed properties. Setget serves side effects on write, validation and normalization, derived and read-only values, proxies for other objects' properties, lazy caching, and engine-visible hooks. The Inspector, AnimationPlayer property tracks, `Tween.tween_property` and `set()` work only with properties, and native engine properties are themselves setter and getter method pairs, so setters cannot be replaced by methods.

In GDScript a getter is attached to a stored field, so the engine's duplication, saving and debugger reads also run it; proxy and derived properties keep useless storage; and read-only properties cannot be declared. With the split, reads of stored data run no user code, which removes these problems structurally. The cost is rewriting GDScript's stored-field-plus-getter pattern as a private stored field plus a computed property; the [survey](surveys/property-accessors.en.md) found about 0.12 manual rewrites per 1,000 lines, and whether a getter refers to its own field classifies getters mechanically.

Setters cannot be coroutines. GDScript's common `await ready` pattern is rewritten to check `is_node_ready()` in the setter and apply the effect once in `_ready`; the rewrite is mechanical, and the rule can be relaxed compatibly if migration evaluation shows significant friction.

## Deferred language proposals

These proposals are outside the first-version contract.

- Value-type records (`struct`). The existing proposal follows the payload-bearing enum rule in specification §12.4: a record is encoded as the name-tagged Array `[&"Name", field…]`, including a record with one field. Mutable fields in a container use get-modify-set. A future native engine representation would require a save-format migration.
- Recursive enums with an explicit Swift-style `indirect` annotation, making allocation visible in the declaration. Recursive value layouts remain unsupported in the first version.
- A binding declaration with an else block, like Rust's let-else and Swift's guard let, for early-exit code: on a match, pattern variables are in scope for the rest of the enclosing block; on a mismatch, the else block runs and must end with return, break or continue. The first version does not provide it; early exits are written as nested conditional pattern bindings (specification §6.3). Flow-sensitive narrowing based on None checks can likewise be added compatibly later.
- Statically ruling out reads of members before assignment, for example with Swift-style two-phase initialization: until a constructor has assigned every stored member, it may not call methods or let self escape. The first version uses runtime checks of the uninitialized state instead (specification §8.1).
- dyn traits, which would serve heterogeneous values within the language; default trait methods and associated types; parameter packs that preserve full signatures; implicit generalization for functions not exposed to the engine.
- Cross-language coroutines: GDScript and other languages awaiting the results of the language's coroutines, and the language awaiting GDScript coroutines or other async values. The first version explicitly does not support them (specification §9); this is an important topic for later research.
- Binding by parameter when awaiting a multi-parameter signal, such as `var (result, code, headers, body) = await http.request_completed`, with each variable typed from the signal parameters. It is provided only for await and introduces no general destructuring assignment; it can later generalize to tuples or general destructuring. In the first version, awaiting a multi-parameter signal is a statement only (specification §9). In the [corpus survey](surveys/await-signal-results.en.md), code using such a result occurred once, and it involved a signal with unknown parameters, so the first version does not provide this form; it can be added compatibly later if needed.
- Generic wrappers whose type arguments are inferred from the expected type, such as `instantiate[T]()` and `load[T]()`, replacing forms like `(scene.instantiate() as Enemy)!`. This first requires rules for inferring type arguments from the expected type at call sites (specification §2.2).
- A top type within the language. If one is needed later, it can be added as a new type (for example `any`) without changing the semantics of `Variant`. Its dynamic boxes would use reserved tags distinct from the encodings in specification §12.4, keeping enum encodings distinguishable from boxes.

Tooling, compatibility checks and evaluation work are described in the [project plan](project-plan.en.md).
