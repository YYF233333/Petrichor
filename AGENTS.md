# AGENTS.md

## Project

Petrichor is a statically typed scripting language that replaces GDScript, delivered as a GDExtension. The compiler and bytecode format do not depend on Godot; the runtime uses godot-cpp directly.

`docs/language-spec.zh.md` is the single source of truth for language rules. When the implementation runs into a problem with a rule, change the spec in a separate commit rather than relaxing it in code. `docs/architecture.md` holds the implementation, source tree (§2.5) and roadmap (§5).

The target engine is Godot 4.7. Check engine behavior against the Godot source commit pinned in `docs/engine-facts.md`, never from memory, and cite the commit and `path:line` for every engine fact recorded. Re-verify `docs/engine-facts.md` when moving to a newer engine commit.

## Design

- A restriction must prevent a wrong meaning or keep a later design open. Allow what has a determined meaning; style belongs to lints. Refusing such code requires real code that meets it (a corpus count or migrated script), or it waits in `BACKLOG.md` as a question. Never restrict users or work around a design to save implementation work, and do not optimize for hypothetical needs (the goals of spec §1 and §13 are real).
- Each stage is part of the final design. A shortcut taken now must not show users behavior that the final design changes.
- Where unsure, choose a default consistent with the defaults around it. Before proposing a design or surveying real Godot projects, follow the proposal checklist and corpus rules in `docs/development.md`.

## Documentation

- Each document holds one kind of content:
  - `docs/getting-started.md` / `docs/getting-started.zh.md`: user-facing setup, editor, CLI and workflow guide;
  - `docs/language-spec.zh.md`: concise normative rules, without engine `path:line` citations or review history (regenerate `docs/language-spec.md` via `tools/spec_translation.md`);
  - `docs/design-notes.md`: detailed rationales, corpus counts, post-v1 designs and rejected alternatives;
  - `docs/engine-facts.md`: engine citations;
  - `docs/architecture.md`: implementation decisions, the constraints they keep and the roadmap, without progress notes;
  - `BACKLOG.md`: unfinished work and open questions.
- Keep user-facing pairs (`README.md` / `README.zh.md`, `docs/getting-started.md` / `docs/getting-started.zh.md`, and later user guides) aligned in facts, scope and commitments, written natively rather than mirrored line for line. Other design and implementation files under `docs/` are Chinese only.
- When a rule changes first-version scope or open questions, update spec §14. Follow the house style in `docs/development.md` (文档).

## Code and tests

- Setup, build/test/benchmark commands and pitfalls live in `docs/development.md`. Close the editor before rebuilding: the extension is not reloadable.
- Derive tests from the spec, the engine and real code, not from the implementation (architecture §3). Keep a test only when it protects a spec rule or real script defect that no other test covers; remove tests of engine behavior, implementation details or duplicates.
- Do not rerun tests needlessly. Run the full engine suite when a large feature finishes, not before every commit; bisect regressions and cut slow build or test paths.
- Code is a liability: keep only real complexity, and count fewer lines as progress.
  - Remove dead paths, duplicated rules, single-use abstractions, unused generality and checks for impossible states. Replace rare, hard-to-test fallbacks with definite errors.
  - Restructure only to remove code or make it easier to find.
  - Let the engine compute what it defines; keep any repeated engine behavior in step by construction or a check.
  - Look for an existing helper in the repository, standard library or dependencies before writing a general-purpose one.
  - Write no comment that the code is understood without.

## Commits

- Other sessions share the working tree and index. Commit only your own files by path (`git commit -m … -- <paths>`) and push to `main` after committing. A push that changes code cancels the in-progress CI run.
- Git hooks in `tools/hooks/` run `.pre-commit-config.yaml` (formatting, doc pairs/style, diagnostics and commit message format). When a hook fixes a file, commit again.
- Write commit messages in English; the `commit-msg` hook checks the header and line length. Omit the body when the subject, diff and updated docs already explain the change. Put only what `git blame` cannot find in the body: non-obvious causes, spec or engine citations, measurements and traps in rejected alternatives.

## Workflow

- Slow iteration is worse than more bugs. Implement with confidence and push work into real use; fix ordinary defects by impact, while crashes, memory corruption or broken type guarantees on legal scripts are always high priority. Fix small gaps in the area at hand and clean up debt in touched code without widening into an open-ended refactor.
- Ask the owner first, proposing the smallest option, about:
  - what cannot be changed back compatibly: run-time meaning of legal code, stored formats and user-facing promises;
  - the roadmap (`docs/architecture.md` §5 and `BACKLOG.md`);
  - any new component, pass or process.
- Decide the rest yourself and list it in the report for the owner to overrule: cases settled by the spec, cases left open (as compile errors recorded as open questions) and details of an agreed design.
- Work on the current release's gating list first (`docs/architecture.md` §5, `BACKLOG.md`). Do not close a step or propose a milestone instead; report when its acceptance measure stops moving.
- Each minor release includes one batch that only removes code. It takes `BACKLOG.md`'s First in order and adds no feature.
- End-of-day report covers: release-list movement and blockers, work outside the list, self-made decisions (each new rule with real code that meets it) and items waiting for the owner.
- Reviewers follow `tools/review.md` and upstream scans follow `tools/watch.md`. Treat root `review-*.md` and `watch-*.md` findings as leads to verify against code, spec and engine source rather than tasks to argue one by one, and propose deleting a file once nothing remains.
