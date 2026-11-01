# AGENTS.md

## Project

Petrichor designs a statically typed scripting language that replaces GDScript, delivered as a GDExtension: the compiler and the bytecode format do not depend on Godot, and the runtime uses godot-cpp directly. `docs/language-spec.{zh,en}.md` (v0.2 draft) is the single source of truth for language rules; when the implementation runs into a problem with a rule, change the spec in a separate commit rather than relaxing the rule in code. Implementation, the source tree (§2.5) and progress (§5) are in `docs/architecture.{zh,en}.md`. The target engine is Godot 4.7; check engine behavior against the `4.7.2-stable` source (commit `ed1daf0`), never from memory, and cite the commit and `path:line` for every engine fact recorded; those of the spec are listed in its Appendix A, whose line numbers are re-verified when moving to a newer engine commit.

## Documentation

- The Chinese and English versions of a document must say the same thing and change in the same commit; the two versions of the spec stay line-aligned.
- Record design changes directly in the spec as normative text: state the rule with a brief rationale and its evidence, not as a review history. Decided content carries no marker; non-normative status is marked 【待定】/[Open], 【已知限制】/[Known limitation] and 【后续】/[Later]. When a rule changes, update §14 (first-version scope and open questions), §15 (conformance checklist) and, if an option was rejected, Appendix B.
- Where the need for a language rule is unclear, prefer a restriction that can be relaxed compatibly later, and do not optimize for hypothetical needs; never restrict users or work around a design to save implementation work. When real code can answer a design question, survey real Godot projects first, as `docs/surveys/` does (each survey a bilingual write-up and a directory of scripts that reproduce it; a single count that backs one rule goes into the `corpus-counts` survey). Surveys record the design they evaluated; when the design later changes, point to the current rule instead of rewriting the data.
- A design proposal shows what users see in two or three concrete code examples; what GDScript does (with engine citations), giving a bug of GDScript or the engine sane semantics rather than copying it; whether it stays consistent with the defaults around it and allows what GDScript allows; and the cost of doing it now against later.

## Code and tests

- Setup: `git submodule update --init`, and in every new clone `pip install pre-commit` and `git config core.hooksPath tools/hooks`.
- Build (SCons, MSVC on Windows, output in `tests/engine/project/bin/`): `scons` (`target=editor`) builds the library every test and benchmark loads, since an editor binary loads it also when it runs the game; `scons target=template_debug` builds the compiler-free export library, used only by the `export` test group (which reports if stale), so build it before a full run, not while iterating. Close the editor before rebuilding: the extension is not reloadable. Changing `debug_symbols` or other compiler options rebuilds all of godot-cpp.
- Compiler tests: `scons compiler_tests`, then `tests/compiler/bin/compiler_tests` (no godot-cpp, no engine).
- Engine tests: `python tests/engine/run.py <Godot 4.7 editor binary> [group...]` (headless; groups in `run.py`, e.g. `fps`).
- Benchmarks: `python benchmarks/run.py <Godot 4.7 editor binary>` on an optimized engine such as the official 4.7.2-stable release, not a `dev_build` (architecture §4).
- Derive tests from the spec (§15), the engine and real code, not from the implementation (architecture §3). Test only what a change needs, and do not rerun tests; run the full engine suite when a large feature is finished, not before every commit, and bisect regressions. Test time matters only where it blocks development.
- Write few comments, none that the code is understood without. Clean up debt in code a change touches without widening into an open-ended refactor.

## Commits

- Git hooks in `tools/hooks/` run `.pre-commit-config.yaml` (project checks in `tools/`). When a hook fixes a file, the commit stops; stage the fix and commit again.
- Commit messages are in English: a subject, a blank line and a body wrapped at 72 columns; `tools/check_commit_msg.py` checks the first two. The subject is `Area: Imperative summary` within 72 characters, capitalized and without a full stop (e.g. `Spec: Add a formatter and lints to the first version (§13.2)`). `Area` is one of `Spec`, `Arch`, `Survey`, `Compiler`, `Bytecode`, `Runtime`, `Adapter`, `Editor`, `Tests`, `Bench`, `Tools` and `Docs`; a commit spanning several names the primary one.
- The body gives the problem, the decision with its evidence (spec sections, engine citations) and the alternatives rejected, with `-` lists for parallel points. Do not retell the diff; a small change needs no body.

## Workflow

- Slow iteration is worse than more bugs. Implement with confidence, push work into real use, fix ordinary defects by their actual impact rather than proving hypothetical uses safe, and leave further verification to reviewers. Paying down debt is progress too; the owner decides whether debt or features come next.
- Ask the owner first about what users see (language semantics, spec rules, diagnostics), the roadmap (architecture §5, `BACKLOG.md`) and any new component, pass or process, proposing the smallest; decide the details of an agreed design (format fields, opcodes, test helpers, file splits) yourself and list them in the report. Fix a small gap found in the area at hand now rather than deferring it. Asked what comes next, choose from the roadmap and the backlog rather than closing a step or proposing a milestone.
- The owner starts reviews when needed, including with other vendors' models, and their findings appear as files in the repository root. A reviewer reads the shared working tree without a separate worktree and changes only its own findings file (suggested format: problem → minimal counterexample → section → severity → minimal revision → test). Findings are not tasks: they go through the `review-filter` subagent (`.claude/agents/review-filter.md`) first, and the implementer does not argue them one by one. When a findings file has nothing left to act on, propose deleting it.
- `BACKLOG.md` lists unfinished work and open questions in no fixed order; the owner decides what comes next. Remove an item as soon as it is done or settled, leaving no trace (no strike-through, completion note or history); record decisions in the spec or the architecture, and leave history to git.
- `DASHBOARD.md` holds live facts everyone needs, each `- YYYY-MM-DDTHH:MM:SSZ text` in UTC (`tools/check_dashboard.py` checks its size and 72-hour expiry on commit). Check that an entry still holds before renewing its time; otherwise remove it.
