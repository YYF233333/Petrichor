# AGENTS.md

## Project

Petrichor designs a statically typed scripting language that replaces GDScript. It will be delivered as a GDExtension, with engine access kept in a thin layer so that a module build remains possible. The language specification `docs/language-spec.{zh,en}.md` (v0.2 draft) is the single source of truth for the design; the previous version is kept at git tag `spec-v0.1`. Implementation follows `docs/architecture.{zh,en}.md`; step 1 (engine integration, no language yet) is in place. The target engine is Godot 4.7; engine facts are checked against `4.7.2-stable` (commit `ed1daf0`). Facts about engine behavior must be checked against the Godot source, not recalled from memory.

## Layout

- `README.md`, `README.zh.md`: the project introduction for the public.
- `docs/language-spec.{zh,en}.md`: the specification. Rules come with a brief rationale and their evidence; open questions are listed in §14.3.
- `docs/architecture.{zh,en}.md`: implementation decisions: components, value representation, testing, order of work.
- `src/adapter/`: the GDExtension adapter (architecture §2.3); editor-only parts in `src/adapter/editor/`.
- `tests/engine/`: the headless Godot test project and its runner.
- `thirdparty/godot-cpp`: git submodule, built with `api_version=4.7`.
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
- Build (SCons, MSVC on Windows): `scons target=editor` and `scons target=template_debug`. The libraries go to `tests/engine/project/bin/`. Close the editor before rebuilding: the extension is not reloadable.
- Engine tests: `python tests/engine/run.py <Godot 4.7 editor binary>` runs the test project headless: the game, editor-side checks and an exported pack.
- Language rules come from the spec. When the implementation runs into a problem with a rule, change the spec in a separate commit; do not relax the rule in code.
- Tests are derived from the spec (§15), the engine and real code, not from the implementation (architecture §3).
