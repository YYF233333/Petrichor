# Petrichor Project Plan

Non-normative. 中文：[project-plan.zh.md](project-plan.zh.md).

This document records existing development and evaluation plans. It does not define language semantics or supported release guarantees. See the [specification](language-spec.en.md) and [design notes](design-notes.en.md).

## Status and engine version

The project is at the specification stage; there is no language implementation yet. The engine facts cited by the specification were checked against Godot commit `469659e` (master, 4.8-beta). This is a verification baseline. The supported engine version has not been selected.

Compatibility tests for initialization, await and typed containers will be pinned to the engine version selected for evaluation.

## Development and evaluation

1. Collect public feedback and converge on a minimal, testable core.
2. Assemble tests, considering reusable suites and their licenses. Distinguish inherited behavior, deliberate differences and new capabilities, and cover positive, negative, runtime and engine-integration cases.
3. Implement the out-of-tree module against the converged specification and its tests.
4. Evaluate the language through an AI-assisted port of a mid-sized game. Brotato is a reference for scale and shape, not a selected source project. The port depends on legally available source and assets with suitable licenses; a comparable licensable project can be used instead.

The port evaluation records annotation and adaptation effort, language ambiguities, missing engine contracts, behavioral differences, performance bottlenecks and debugging costs. Success is measured against the specification and valid tests; discrepancies are recorded rather than hidden by relaxing types or removing those tests.

## Tools and distribution

The implementation language, IR and JIT decisions remain in the design notes. Distribution and the depth of editor integration are undecided.

Code completion, breakpoints and executable export can be accepted in stages; not all of them are prerequisites for an alpha. The first version does not plan special extensions for `@tool` or other editor execution: support may follow naturally from integration, otherwise it is deferred.

Other existing tooling questions and proposals include:

- Range hints for narrow exported integer types such as i32, while the Inspector edits a 64-bit int.
- Debugger display of enum encodings using static type information.
- Fix-it suggestions and full inference traces, beyond the diagnostic obligations in specification §10.
- Additional floating-point arithmetic diagnostics may be considered later; existing numeric-conversion warnings remain as specified in §7.3.
- Hints about the cost of runtime String-to-StringName conversion on hot paths (specification §7.7).
- A lint for encoding allocation costs of enums and proposed records, particularly in containers such as `Array[enum]`.
- Contributing engine contract metadata upstream.

## Compatibility checks

The order specified in §8.2 is a language rule. Comparing it with engine behavior does not reopen it as an unspecified rule. Scene-property injection, entering the tree and their interaction with inheritance remain language questions in §8.3. Reads of uninitialized non-null exports follow §8.1; static initialization follows §8.5.

The existing compatibility work also covers await resumption and reentrancy, destruction of instances or signal sources, node-tree transitions and frame cleanup (§9); typed containers and external read checks (§12); and scripts, callbacks, properties and scene saving/loading (§13, §15).

Evidence is tied to the selected engine revision. Engine facts are recorded with source locations in specification Appendix A.
