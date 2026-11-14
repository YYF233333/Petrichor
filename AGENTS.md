# AGENTS.md

## Project

Petrichor designs a statically typed scripting language that replaces GDScript, delivered as a GDExtension. The compiler and the bytecode format do not depend on Godot. The runtime uses godot-cpp directly.

`docs/language-spec.zh.md` (v0.2 draft) is the single source of truth for language rules. When the implementation runs into a problem with a rule, change the spec in a separate commit. Do not relax the rule in code. `docs/architecture.md` holds the implementation, the source tree (§2.5) and the roadmap (§5).

The target engine is Godot 4.7. Check engine behavior against the `4.7.2-stable` source (commit `ed1daf0`), never from memory. Cite the commit and `path:line` for every engine fact recorded. `docs/engine-facts.md` lists the spec's engine facts. Re-verify their line numbers when moving to a newer engine commit.

## Design

- A restriction must prevent a wrong meaning or keep a later design open. Allow what has a determined meaning; style belongs to lints. A check that still refuses such code needs real code that meets it: a corpus count or a migrated script. Without one it waits in `BACKLOG.md` as a question. Where unsure, provide a default consistent with the defaults around it. Never restrict users or work around a design to save implementation work. Do not optimize for hypothetical needs; the goals of spec §1 and §13 are real ones.
- A design proposal shows:
  - what users see, in two or three concrete code examples;
  - what GDScript does, with engine citations, giving bugs of GDScript or the engine sane semantics instead of copying them;
  - for tools and the runtime, also what mature implementations do, without assuming GDScript's is best;
  - whether it stays consistent with the defaults around it and allows what GDScript allows;
  - where it crosses freed objects, the time before ready, `await` and cancellation, the engine boundary, scene inheritance and hot reload. Each case with no obvious result is a script of five lines or fewer, checked against the spec's guarantees. It is decided as Workflow says, and becomes a test;
  - the cost of doing it now against later.
- Each stage is part of the final design. A shortcut taken now must not show users behavior that the final design changes.
- When real code can answer a design question, survey real Godot projects first. A survey in `docs/surveys/` is a write-up plus the scripts that reproduce it. A single count backing one rule goes into `corpus-counts`. A survey records the design it evaluated. When the design changes, point to the current rule rather than rewriting the data.

## Documentation

- The README is in Chinese and English. Both versions say the same thing and change in the same commit. The documents under `docs/` are in Chinese only. The English spec is generated before the first release.
- Each document holds one kind of content:
  - the spec: concise normative rules, without engine `path:line` citations or review history;
  - `docs/design-notes.md`: detailed rationales, corpus counts, post-v1 designs and rejected alternatives;
  - `docs/engine-facts.md`: engine citations;
  - `docs/architecture.md`: implementation decisions, the constraints they keep and the roadmap, without progress notes;
  - `BACKLOG.md`: unfinished work and open questions.
- Decided content carries no marker. Non-normative status is marked 【待定】/[Open] or 【已知限制】/[Known limitation] where it applies. Additions after the first version are listed only in spec §14.2. When a rule changes the first-version scope or open questions, update spec §14.
- Write one idea per sentence. `tools/check_doc_style.py` flags sentences over 60 字 or 30 words in the lines a commit changes. It catches several ideas piled into one, and does not ask for natural sentences to be split. One-off texts, such as release notes and reports, are not held to it.
  - Turn an embedded list or a chain of "or" into a list.
  - Put the condition before the result.
  - Put `path:line` citations and links at the end, in brackets.
  - In the spec, refer to another rule by its concept, such as "the comparison rules", not by section number.

## Code and tests

- Setup: run `git submodule update --init`. In every new clone, run `pip install pre-commit` and `git config core.hooksPath tools/hooks`. `AGENTS.local.md` (gitignored) holds machine-local paths and notes; no other tracked file refers to it. `docs/development.md` collects build, test and debugging pitfalls.
- Build with SCons, MSVC on Windows; the output goes to `tests/engine/project/bin/`. Close the editor before rebuilding: the extension is not reloadable.
  - `scons` (`target=editor`) builds the library that every test and benchmark loads.
  - `scons target=template_debug` and `scons target=template_release` build the two export libraries, without the compiler. Only the `export` test group uses them, and reports when one is stale. Build them before a full run, not while iterating.
- Compiler tests: `scons compiler_tests`, then `tests/compiler/bin/compiler_tests`, with no engine.
- Engine tests: `python tests/engine/run.py <Godot 4.7 editor binary> [group...]`, with the groups listed in `run.py`.
- Benchmarks: `python benchmarks/run.py <Godot 4.7 editor binary>`. Run them on an optimized engine such as the official 4.7.2-stable release, not a `dev_build` (architecture §4).
- Derive tests from the spec, the engine and real code, not from the implementation (architecture §3).
  - A test stays only when what it protects is worth its run time and upkeep.
  - It protects a spec rule or a real defect in this language's scripts, and no other test runs it.
  - Remove tests of engine behavior, of implementation details or of what another test runs.
  - Do not rerun tests. Run the full engine suite when a large feature is finished, not before every commit. Bisect regressions.
  - Cut test and build time where it slows development.
- Code is a liability: keep only real complexity, and count fewer lines as progress.
  - Remove dead paths, rules kept in several places, abstractions with one use and generality nothing uses.
  - Remove checks for states that cannot occur. Replace a rare, hard-to-test fallback path with a definite error where one would do.
  - Restructure only to remove code or to make it easier to find. Moving code without either is not progress.
  - Let the engine compute what it defines. When this language repeats engine behavior, keep both in step by construction or by a check that catches drift.
  - Write no comment that the code is understood without.

## Commits

- Other sessions share the working tree and its index. Commit only your own files, by path (`git commit -m … -- <paths>`). Commit locally; the owner pushes.
- Git hooks in `tools/hooks/` run `.pre-commit-config.yaml`, with project checks in `tools/`. When a hook fixes a file, the commit stops; commit again.
- Commit messages are in English: a subject, a blank line and a body wrapped at 72 columns.
  - The subject is `Area: Imperative summary` within 72 characters, capitalized and without a full stop.
  - `Area` is one of `Spec`, `Arch`, `Survey`, `Compiler`, `Bytecode`, `Runtime`, `Adapter`, `Editor`, `Tests`, `Bench`, `Tools` and `Docs`. A commit spanning several areas names the primary one.
  - The body gives the problem, the decision with its evidence and the alternatives rejected. Evidence means spec sections and engine citations. Use `-` lists for parallel points.
  - Do not retell the diff; a small change needs no body.

## Workflow

- Slow iteration is worse than more bugs. Implement with confidence, push work into real use and leave further verification to reviewers.
  - Fix ordinary defects by their actual impact rather than proving hypothetical uses safe.
  - A crash, memory corruption or broken type guarantee on a legal script is not ordinary, however rare.
  - Make a small gap or cheap fix in the area at hand now.
  - Clean up debt in code a change touches, without widening into an open-ended refactor. Paying down debt is progress too. The owner decides whether debt or features come next.
- Ask the owner first, proposing the smallest option, about:
  - what cannot be changed back compatibly: the run-time meaning of legal code, stored formats and what the project promises users;
  - the roadmap: architecture §5 and `BACKLOG.md`;
  - any new component, pass or process.
- Decide the rest yourself and list it in the report, for the owner to overrule:
  - a case that the spec's rules and guarantees already settle;
  - a case they leave open, as a compile error that can be allowed later, recorded among the open questions;
  - the details of an agreed design, such as format fields, opcodes, test helpers and file splits.
- Each release has a date and a list that gates it (architecture §5, `BACKLOG.md`). Work on the list first, and choose from it when asked what comes next. Do not close a step or propose a milestone instead. When the measure that accepts a step has stopped moving, say so.
- Report at the end of each working day:
  - the release list: what moved and what is stuck;
  - the work that went elsewhere;
  - what you decided yourself, each new rule with the real code that meets it;
  - what waits for the owner.
- The owner starts reviews when needed. Reviewers follow `REVIEW.md` and leave their findings as files in the repository root.
  - Findings are not tasks: they are first checked against the tree, the spec and the engine source. The implementer does not argue them one by one.
  - When a findings file has nothing left to act on, propose deleting it.
- At Godot's pre-releases and releases, the owner starts a scan of upstream changes. Scanners follow `WATCH.md`, and their reports are handled as findings.
- `BACKLOG.md` lists unfinished work and open questions in no fixed order. Remove an item as soon as it is done or settled, leaving no trace. Record decisions in the spec or the architecture, and leave history to git.
