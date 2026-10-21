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

## Deferred language proposals

These proposals are outside the first-version contract.

- Value-type records (`struct`). The existing proposal follows the payload-bearing enum rule in specification §12.4: a record is encoded as the name-tagged Array `[&"Name", field…]`, including a record with one field. Mutable fields in a container use get-modify-set. A future native engine representation would require a save-format migration.
- Recursive enums with an explicit Swift-style `indirect` annotation, making allocation visible in the declaration. Recursive value layouts remain unsupported in the first version.
- A binding declaration with an else block, like Rust's let-else and Swift's guard let, for early-exit code: on a match, pattern variables are in scope for the rest of the enclosing block; on a mismatch, the else block runs and must end with return, break or continue. The first version does not provide it; early exits are written as nested conditional pattern bindings (specification §6.3). Flow-sensitive narrowing based on None checks can likewise be added compatibly later.
- Statically ruling out reads of members before assignment, for example with Swift-style two-phase initialization: until a constructor has assigned every stored member, it may not call methods or let self escape. The first version uses runtime checks of the uninitialized state instead (specification §8.1).
- dyn traits, which would serve heterogeneous values within the language; default trait methods and associated types; parameter packs that preserve full signatures; implicit generalization for functions not exposed to the engine.
- A top type within the language. If one is needed later, it can be added as a new type (for example `any`) without changing the semantics of `Variant`. Its dynamic boxes would use reserved tags distinct from the encodings in specification §12.4, keeping enum encodings distinguishable from boxes.

Tooling, compatibility checks and evaluation work are described in the [project plan](project-plan.en.md).
