# AGENTS.md

## Project

Petrichor designs a statically typed scripting language that replaces GDScript. It will be delivered as a GDExtension, with engine access kept in a thin layer so that a module build remains possible. The language specification `docs/language-spec.{zh,en}.md` (v0.2 draft) is the single source of truth for the design; the previous version is kept at git tag `spec-v0.1`. Implementation follows `docs/architecture.{zh,en}.md`; steps 1–4 of its order of work are done, step 4 being the first milestone (the FPS starter kit runs with migrated scripts), and step 5, paying down the debt of the first implementation before new features, is under way. The target engine is Godot 4.7; engine facts are checked against `4.7.2-stable` (commit `ed1daf0`). Facts about engine behavior must be checked against the Godot source, not recalled from memory.

## Layout

- `README.md`, `README.zh.md`: the project introduction for the public.
- `docs/language-spec.{zh,en}.md`: the specification. Rules come with a brief rationale and their evidence; open questions are listed in §14.3.
- `docs/architecture.{zh,en}.md`: implementation decisions: components, value representation, testing, order of work.
- `src/compiler/`: the compiler (architecture §2.1), no Godot dependency; `src/bytecode/`: the compiled format and its verifier, shared with the runtime, also without Godot; `src/runtime/`: the runtime (§2.2).
- `src/adapter/`: the GDExtension adapter (architecture §2.3); editor-only parts in `src/adapter/editor/`.
- `src/probe/`: the step-2 probes of the architecture, built into the editor library only.
- `gen/`: generated sources (the tables of built-in types, the bytecode format identifier); `build/`: objects and static libraries.
- `tests/compiler/`: the compiler tests (doctest), built without godot-cpp.
- `tests/engine/`: the headless Godot test project and its runner; `tests/engine/fps/` holds the migrated scripts of the FPS starter kit (architecture §5 step 4).
- `benchmarks/`: cases in this language with their GDScript twins, and their runner (architecture §4).
- `thirdparty/doctest`: the test framework of the compiler tests.
- `thirdparty/godot-cpp`: git submodule, built with `api_version=4.7`.
- `thirdparty/starter-kit-fps`: git submodule, the FPS starter kit at the commit of the second paper migration; the runner copies it and puts the migrated scripts in place.
- `docs/surveys/`: empirical investigations behind specific rules, and evaluations of the design against real code. Each has a write-up in both languages and a directory of scripts that reproduce it. Single counts that back one rule go into `docs/surveys/corpus-counts` (script `corpus-counts/counts.py`).

## Documentation rules

- The Chinese and English versions of a document must say the same thing. Change both in the same commit; the two versions of the spec are kept line-aligned.
- Record design changes directly in the spec. The spec is normative text: decided content carries no marker, and non-normative status is marked 【待定】/[Open], 【已知限制】/[Known limitation] and 【后续】/[Later]. Do not describe decisions as a review history; state the rule, then give a brief rationale and the evidence.
- When a rule changes, also update the sections that depend on it: §14 (first-version scope and open questions), §15 (conformance checklist), and Appendix B (alternatives considered) if an option was rejected.
- Every engine fact in the spec must cite the Godot commit it was checked against and a `path:line`, and be listed in Appendix A. Re-verify line numbers when moving to a newer engine commit.
- Design decisions follow the problem and positioning in spec §1.1. Prefer restrictions that can be relaxed compatibly later; do not optimize for hypothetical needs.
- When a design question can be answered from real code, survey real Godot projects as in `docs/surveys/` before deciding. Surveys record the design they evaluated; when the design later changes, point to the current rule instead of rewriting the data.
- Suggested review format: problem → minimal counterexample → section → severity → minimal revision → test.

## Code

- Setup: `git submodule update --init`.
- Build (SCons, MSVC on Windows): `scons target=editor` builds the library that every test and benchmark loads, since an editor binary loads the editor library also when it runs the game. `scons target=template_debug` builds the library without the compiler that exported games use; only the engine tests' exported pack loads it, and they report it when it is not built from the current sources, so build it before running them in full, not while iterating. The libraries go to `tests/engine/project/bin/`. Close the editor before rebuilding: the extension is not reloadable. Changing `debug_symbols` or other compiler options rebuilds all of godot-cpp.
- Compiler tests: `scons compiler_tests`, then run `tests/compiler/bin/compiler_tests`. They build without godot-cpp and do not start the engine.
- Engine tests: `python tests/engine/run.py <Godot 4.7 editor binary>` runs the test project headless: the game, editor-side checks, the debugger, hot reload, an exported pack with the template_debug library, and the FPS starter kit with its migrated scripts. Extra arguments name the groups to run, for example `fps`.
- Benchmarks: `python benchmarks/run.py <Godot 4.7 editor binary>` compares each case of `benchmarks/project/cases/` with its GDScript twin (architecture §4). It needs an optimized engine, such as the official 4.7.2-stable release, not a dev_build.
- Language rules come from the spec. When the implementation runs into a problem with a rule, change the spec in a separate commit; do not relax the rule in code.
- Tests are derived from the spec (§15), the engine and real code, not from the implementation (architecture §3).
