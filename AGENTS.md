# AGENTS.md

## Project

Petrichor designs a statically typed scripting language that replaces GDScript. It will be delivered as a GDExtension; the compiler and the bytecode format do not depend on Godot, and the runtime uses godot-cpp, whose types correspond to the engine's own, so that a module build remains possible. The language specification `docs/language-spec.{zh,en}.md` (v0.2 draft) is the single source of truth for the design; the previous version is kept at git tag `spec-v0.1`. Implementation follows `docs/architecture.{zh,en}.md`; how far it has come is in its order of work (§5), and the work not done yet in `BACKLOG.md`. The target engine is Godot 4.7; engine facts are checked against `4.7.2-stable` (commit `ed1daf0`). Facts about engine behavior must be checked against the Godot source, not recalled from memory.

## Layout

The source tree is listed in architecture §2.5. Besides it, `docs/surveys/` holds the empirical investigations behind specific rules and the evaluations of the design against real code, each with a write-up in both languages and a directory of scripts that reproduce it, and `tools/` the project's own checks, with the Git hooks that run them in `tools/hooks/`.

## Documentation rules

- The Chinese and English versions of a document must say the same thing. Change both in the same commit; the two versions of the spec are kept line-aligned.
- Record design changes directly in the spec. The spec is normative text: decided content carries no marker, and non-normative status is marked 【待定】/[Open], 【已知限制】/[Known limitation] and 【后续】/[Later]. Do not describe decisions as a review history; state the rule, then give a brief rationale and the evidence.
- When a rule changes, also update the sections that depend on it: §14 (first-version scope and open questions), §15 (conformance checklist), and Appendix B (alternatives considered) if an option was rejected.
- Every engine fact in the spec must cite the Godot commit it was checked against and a `path:line`, and be listed in Appendix A. Re-verify line numbers when moving to a newer engine commit.
- Design decisions follow the problem and positioning in spec §1.1. Prefer restrictions that can be relaxed compatibly later; do not optimize for hypothetical needs.
- When a design question can be answered from real code, survey real Godot projects as in `docs/surveys/` before deciding; single counts that back one rule go into `docs/surveys/corpus-counts` (script `corpus-counts/counts.py`). Surveys record the design they evaluated; when the design later changes, point to the current rule instead of rewriting the data.
- Suggested review format: problem → minimal counterexample → section → severity → minimal revision → test.

## Code

- Setup: `git submodule update --init`, and in every new clone `pip install pre-commit` and `git config core.hooksPath tools/hooks`.
- The hooks run the checks of `.pre-commit-config.yaml`. When one fixes a file, the commit stops; stage the fix and commit again.
- Build (SCons, MSVC on Windows): `scons target=editor` builds the library that every test and benchmark loads, since an editor binary loads the editor library also when it runs the game. `scons target=template_debug` builds the library without the compiler that exported games use; only the engine tests' exported pack loads it, and they report it when it is not built from the current sources, so build it before running them in full, not while iterating. The libraries go to `tests/engine/project/bin/`. Close the editor before rebuilding: the extension is not reloadable. Changing `debug_symbols` or other compiler options rebuilds all of godot-cpp.
- Compiler tests: `scons compiler_tests`, then run `tests/compiler/bin/compiler_tests`. They build without godot-cpp and do not start the engine.
- Engine tests: `python tests/engine/run.py <Godot 4.7 editor binary>` runs the test project headless: the game, editor-side checks, the debugger, hot reload, an exported pack with the template_debug library, and the FPS starter kit and Ring's breakout with their migrated scripts. Extra arguments name the groups to run, for example `fps`.
- Benchmarks: `python benchmarks/run.py <Godot 4.7 editor binary>` compares each case of `benchmarks/project/cases/` with its GDScript twin (architecture §4). It needs an optimized engine, such as the official 4.7.2-stable release, not a dev_build.
- Language rules come from the spec. When the implementation runs into a problem with a rule, change the spec in a separate commit; do not relax the rule in code.
- Tests are derived from the spec (§15), the engine and real code, not from the implementation (architecture §3).

## Commits

- A commit message is in English: a subject, a blank line, and a body wrapped at 72 columns, as in Godot's `CONTRIBUTING.md`. The commit-msg hook checks the length of the subject and the blank line.
- The subject is `Area: Imperative summary`, within 72 characters, capitalized and without a full stop, so that `git log --oneline` can be scanned, as in `Spec: Add a formatter and lints to the first version (§13.2)`. The area is one of `Spec`, `Arch`, `Survey`, `Compiler`, `Bytecode`, `Runtime`, `Adapter`, `Editor`, `Tests`, `Bench`, `Tools` and `Docs`; a commit that spans several names the one it is about.
- The body gives the problem, the decision with its evidence (spec sections, engine `path:line` with the commit) and the alternatives rejected; points that stand side by side go in a `-` list. It does not retell the diff, and a small change needs none.

## Workflow

- Slow iteration is worse than more bugs; without progress there is nothing. Push the work forward and into real use, accept that ordinary defects surface in early use and are fixed by their actual impact, and do not keep proving that nothing goes wrong in uses that do not exist yet. Paying down debt is progress too; the owner decides whether debt or features come next.
- Implement with confidence. Test only what needs it, and do not rerun tests; leave further verification and review to the reviewers, so that they do not interrupt the implementation. Run the full engine suite when a large feature is finished, not before every commit; a regression is found by bisecting. Write few comments, and only valuable ones: a comment that the code is understood without should not be written. Test time matters because it blocks development, not because slowness must be optimized for its own sake. Clean up debt in the code a change touches, without widening the cleanup into an open-ended refactor.
- The owner starts reviews when needed, possibly with models of other vendors, and their findings appear as files in the repository root from time to time. A reviewer reads the current working tree, which it shares with the implementer, changes nothing in it but its own findings file, and uses no separate worktree; how it reviews is its own choice. Findings are not tasks: they first go through the `review-filter` subagent (`.claude/agents/review-filter.md`), which drops false positives and low-value items and grades the rest, and the implementer does not argue them one by one.
- `BACKLOG.md` lists the work not done yet and the open questions, in no fixed order; the owner decides what comes next. Remove an item as soon as it is done or settled, and leave no trace of it: no strike-through, no note that it was done, no history, so that the backlog neither grows nor goes stale. Decisions go into the spec or the architecture, where they belong; git keeps the rest.
- `DASHBOARD.md` holds what everyone needs to know, each entry `- YYYY-MM-DDTHH:MM:SSZ text` in UTC. `tools/check_dashboard.py` limits its size, and an entry older than 72 hours blocks commits; renew its time only after checking that it still holds, otherwise remove it.
