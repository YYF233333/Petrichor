# AGENTS.md

## Project

Petrichor designs a statically typed scripting language that replaces GDScript. It will be delivered as a GDExtension, with engine access kept in a thin layer so that a module build remains possible. The design was restarted on 2026-10-23: `docs/decisions.{zh,en}.md` records the current decisions and prevails over the frozen v0.1 specification (git tag `spec-v0.1`), which will be rewritten from them. There is no code yet. The target engine is Godot master (4.8-beta). Facts about engine behavior must be checked against the Godot source, not recalled from memory.

## Layout

- `docs/decisions.{zh,en}.md`: the current design decisions, each with its rule, rationale and evidence.
- `docs/language-spec-draft.{zh,en}.md`: the v0.2 specification draft rewritten from the decisions; its 【待定】/[Open] items correspond to §10 of the decisions. It replaces v0.1 once those items are settled.
- `docs/language-spec.{zh,en}.md`: specification v0.1, frozen as a reference.
- `docs/design-notes.{zh,en}.md`, `docs/project-plan.{zh,en}.md`: non-normative rationale and plans written for v0.1, also frozen.
- `docs/surveys/`: empirical investigations behind specific decisions, and evaluations of designs against real code. Each has a write-up in both languages and a directory of scripts that reproduce it.

## Documentation rules

- The Chinese and English versions of a document must say the same thing. Change both in the same commit.
- Until the specification is rewritten, record new decisions in `docs/decisions.*.md` and move items out of its "not yet decided" section as they are settled; apply each settled item to the spec draft in the same commit. Do not edit the frozen v0.1 documents beyond their status line.
- The spec is normative text. Decided content carries no marker. Non-normative status is marked 【待定】/[Open], 【已知限制】/[Known limitation] and 【后续】/[Later]. Do not describe decisions as a review history; state the rule, then give a brief rationale.
- When a rule changes, also update the sections that depend on it: §14 (first-version scope and open questions), §15 (conformance checklist), and Appendix B (alternatives considered) if an option was rejected.
- Every engine fact in the spec must cite the Godot commit it was checked against and a `path:line`, and be listed in Appendix A. Re-verify line numbers when moving to a newer engine commit.
- Design decisions follow the problem and positioning in `docs/decisions.*.md` §1. Prefer restrictions that can be relaxed compatibly later; do not optimize for hypothetical needs.
- When a design question can be answered from real code, survey real Godot projects as in `docs/surveys/` before deciding.
- Suggested review format: problem → minimal counterexample → section → severity → minimal revision → test.

## Code

There is no implementation yet. When code is added, document the build and test commands here.
