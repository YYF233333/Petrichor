# AGENTS.md

## Project

Petrichor designs a statically typed scripting language that replaces GDScript, delivered as a GDExtension: the compiler and the bytecode format do not depend on Godot, and the runtime uses godot-cpp directly. `docs/language-spec.{zh,en}.md` (v0.2 draft) is the single source of truth for language rules; when the implementation runs into a problem with a rule, change the spec in a separate commit rather than relaxing the rule in code. Implementation, the source tree (§2.5) and progress (§5) are in `docs/architecture.md`. The target engine is Godot 4.7; check engine behavior against the `4.7.2-stable` source (commit `ed1daf0`), never from memory, and cite the commit and `path:line` for every engine fact recorded. The spec's engine facts are listed in `docs/engine-facts.md`, whose line numbers are re-verified when moving to a newer engine commit.

## Design

- A restriction must prevent a wrong meaning or keep a later design open. Allow what has a determined meaning (style belongs to lints), provide a default where unsure, consistent with the defaults around it, and never restrict users or work around a design to save implementation work. Do not optimize for hypothetical needs; the goals of spec §1 and §13 are real ones.
- A design proposal shows what users see in two or three concrete code examples; what GDScript does (with engine citations), giving a bug of GDScript or the engine sane semantics rather than copying it; for tools and the runtime, also what mature implementations do, not assuming GDScript's is best; whether it stays consistent with the defaults around it and allows what GDScript allows; and the cost of doing it now against later. Each stage is part of the final design: a shortcut taken now must not show users behavior that the final design changes.
- When real code can answer a design question, survey real Godot projects first (`docs/surveys/`, each a write-up and the scripts that reproduce it; a single count backing one rule goes into `corpus-counts`). A survey records the design it evaluated; when the design changes, point to the current rule rather than rewriting the data.

## Documentation

- The spec and the README are in Chinese and English: the two versions say the same thing and change in the same commit, and those of the spec stay line-aligned. The other documents under `docs/` are in Chinese only.
- Record design changes in the spec as concise normative rules (no inline engine `path:line` citations, not a review history); detailed rationales, corpus counts, post-v1 designs and rejected alternatives go in `docs/design-notes.md`, and engine citations in `docs/engine-facts.md`. Decided content carries no marker; non-normative status is marked 【待定】/[Open], 【已知限制】/[Known limitation] and 【后续】/[Later]. When a rule changes, update §14 if first-version scope or open questions change.

## Code and tests

- Setup: `git submodule update --init`, and in every new clone `pip install pre-commit` and `git config core.hooksPath tools/hooks`. `AGENTS.local.md` (gitignored) holds machine-local paths and notes.
- Build (SCons, MSVC on Windows, output in `tests/engine/project/bin/`): `scons` (`target=editor`) builds the library every test and benchmark loads; `scons target=template_debug` builds the compiler-free export library, which only the `export` test group uses (it reports if stale), so build it before a full run, not while iterating. Close the editor before rebuilding: the extension is not reloadable.
- Compiler tests: `scons compiler_tests`, then `tests/compiler/bin/compiler_tests` (no engine). Engine tests: `python tests/engine/run.py <Godot 4.7 editor binary> [group...]` (groups in `run.py`). Benchmarks: `python benchmarks/run.py <Godot 4.7 editor binary>` on an optimized engine such as the official 4.7.2-stable release, not a `dev_build` (architecture §4).
- Derive tests from the spec, the engine and real code, not from the implementation (architecture §3). A test stays only when what it protects is worth its run time and upkeep: a spec rule or a real defect that no other test runs, in what this language's scripts do; remove tests of engine behavior, of implementation details or of what another test runs. Do not rerun tests; run the full engine suite when a large feature is finished, not before every commit, and bisect regressions. Cut test and build time where it slows development.
- Code is a liability: keep only real complexity, and count fewer lines as progress. Remove dead paths, rules kept in several places, abstractions with one use, generality nothing uses, checks for states that cannot occur, and rare, hard-to-test fallback paths where a definite error would do; refactor to remove code, not to restructure the same amount. Let the engine compute what it defines; where this language must repeat engine behavior, keep the two in step by construction or by a check that fails when they drift apart. Write no comment that the code is understood without.

## Commits

- Other sessions share the working tree and its index, so commit only your own files, by path (`git commit -m … -- <paths>`). Commit locally; the owner pushes.
- Git hooks in `tools/hooks/` run `.pre-commit-config.yaml` (project checks in `tools/`). When a hook fixes a file, the commit stops; commit again.
- Commit messages are in English: a subject, a blank line and a body wrapped at 72 columns. The subject is `Area: Imperative summary` within 72 characters, capitalized and without a full stop. `Area` is one of `Spec`, `Arch`, `Survey`, `Compiler`, `Bytecode`, `Runtime`, `Adapter`, `Editor`, `Tests`, `Bench`, `Tools` and `Docs`; a commit spanning several names the primary one.
- The body gives the problem, the decision with its evidence (spec sections, engine citations) and the alternatives rejected, with `-` lists for parallel points. Do not retell the diff; a small change needs no body.

## Workflow

- Slow iteration is worse than more bugs. Implement with confidence, push work into real use and leave further verification to reviewers. Fix ordinary defects by their actual impact rather than proving hypothetical uses safe; a crash, memory corruption or broken type guarantee on a legal script is not ordinary however rare, and a small gap or cheap fix in the area at hand is made now. Clean up debt in code a change touches without widening into an open-ended refactor; paying down debt is progress too, and the owner decides whether debt or features come next.
- Ask the owner first about what users see (language semantics, spec rules, diagnostics), the roadmap (architecture §5, `BACKLOG.md`) and any new component, pass or process, proposing the smallest; decide the details of an agreed design (format fields, opcodes, test helpers, file splits) yourself and list them in the report. Asked what comes next, choose from the roadmap and the backlog rather than closing a step or proposing a milestone.
- The owner starts reviews when needed; reviewers follow `REVIEW.md` and leave their findings as files in the repository root. Findings are not tasks: they go through the `review-filter` subagent first, and the implementer does not argue them one by one. When a findings file has nothing left to act on, propose deleting it.
- `BACKLOG.md` lists unfinished work and open questions in no fixed order. Remove an item as soon as it is done or settled, leaving no trace; record decisions in the spec or the architecture, and leave history to git.
