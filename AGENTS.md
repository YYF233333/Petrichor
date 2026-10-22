# AGENTS.md

## Project

Petrichor designs a statically typed scripting language that replaces GDScript. It will be implemented as an out-of-tree Godot module. The project is at the specification stage: there is no code yet. The target engine is Godot master (4.8-beta when spec v0.1 was written). Facts about engine behavior must be checked against the Godot source, not recalled from memory.

## Layout

- `docs/language-spec.{zh,en}.md`: the specification, in Chinese and English.
- `docs/design-notes.{zh,en}.md`: non-normative design rationale and implementation proposals.
- `docs/project-plan.{zh,en}.md`: non-normative development, evaluation and tooling plans.
- `docs/surveys/`: empirical investigations behind specific decisions, and evaluations of the specification against real code. Each has a write-up in both languages and a directory of scripts that reproduce it.

## Documentation rules

- The Chinese and English versions of a document must say the same thing. Change both in the same commit.
- The spec is normative text. Decided content carries no marker. Non-normative status is marked 【待定】/[Open], 【已知限制】/[Known limitation] and 【后续】/[Later]. Do not describe decisions as a review history; state the rule, then give a brief rationale.
- When a rule changes, also update the sections that depend on it: §14 (first-version scope and open questions), §15 (conformance checklist), and Appendix B (alternatives considered) if an option was rejected.
- Every engine fact in the spec must cite the Godot commit it was checked against and a `path:line`, and be listed in Appendix A. Re-verify line numbers when moving to a newer engine commit.
- Design decisions follow the design goals in `docs/design-notes.*.md`: semantics before algorithms; representations the engine can observe are part of the semantics; prefer restrictions that can be relaxed compatibly later; do not optimize for hypothetical needs.
- When a design question can be answered from real code, survey real Godot projects as in `docs/surveys/` before deciding.
- Suggested review format: problem → minimal counterexample → section → severity → minimal revision → test.

## Code

There is no implementation yet. When code is added, document the build and test commands here.
