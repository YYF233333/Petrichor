# Reviewing Petrichor

The owner asks for reviews from time to time, often from other vendors' models. This file is the standing brief, and the owner's message adds the scope. Read `AGENTS.md` first.

## How this repository differs

Judge by the principles of `AGENTS.md` rather than by common review taste. Where they differ most:

- **The spec leads.** Code that disagrees with the spec is a code defect, unless the rule itself is wrong, in which case the finding is against the spec.
- **Code is a liability.** What can go without losing behavior is a finding. Refactoring that removes code is welcome; restructuring that keeps as much (splitting files, adding layers) is not a finding. A fix adds no more than the problem needs.
- **Tests clear a high bar.** A test below it is a finding as much as a bug is, and a new test is proposed only when it clears it.
- **Rarity excuses no crash.** A crash, memory corruption or broken type guarantee on a legal script is a finding however rare; only unreasonable input is out, such as thousands of nested levels or bytecode crafted by hand (architecture §6.1). Other defects count by their actual impact, and a cheap fix is worth reporting before any script needs it.

These are settled and need evidence they did not weigh to reopen: what spec §14 leaves out of the first version or keeps open, the alternatives in Appendix B, the unfinished work in `BACKLOG.md`, optimization that does not also simplify the code (step 9 of architecture §5), and the limits in architecture §6.

## Scope

Review what the message names: a commit range, an area, sections of the spec or the whole repository; without one, the commits not yet pushed (`git log origin/main..HEAD`). Implementation goes on while you review, so the scope is a range, not a snapshot: note the HEAD you start from, and before writing a finding check that it still holds at the current HEAD.

- Changes: whether the code does what the spec says, including the cases its tests leave out; whether the spec (§14, §15), the architecture and `BACKLOG.md` changed with it; what it adds that could be less; a shortcut that would show users behavior the final design changes; engine behavior it repeats without a check that keeps the two in step.
- The spec: sections that contradict each other; rules that real code runs into, shown with that code; Appendix A against the engine source; the Chinese and English versions saying different things. Its design is judged by AGENTS.md (Design) and by these leanings of the owner:
  - a restriction that prevents no wrong meaning and keeps no later design open is a finding;
  - so is a concept users must learn where the compiler could handle it for them;
  - so are special cases that one rule could replace, since rules are a liability like code;
  - implicit behavior needs a reason;
  - rarely used features align with GDScript, but a choice GDScript made to suit its own implementation is no precedent;
  - a diagnostic is judged by its message in a concrete case, against CPython 3.10 and rustc.
- The whole repository: crashes, memory corruption, deadlocks and holes in the type guarantees; code and tests that can go; builds and tests slower than they need to be; documents that describe what the code does not do; language features that no test runs.

A review usually looks at the roadmap too (architecture §5, `BACKLOG.md`). The task is to find what decides whether the project succeeds and argue that each can be solved later and has its place in the roadmap, not to solve it now or to narrow the scope: for each deferred item, whether anything blocks doing it later, and in which step it is done.

## Working in the shared tree

Review the shared working tree itself, with no worktree, clone or branch: a fork falls behind faster than a review takes. Write only your findings file.

- While an implementer is working, which you assume unless the message says otherwise, only read: no builds, no tests, no change to files or to the git state, and git only to `log`, `show`, `diff` and `blame`. A probe can still run in a copy of `tests/engine/project` with its `bin/` and `.godot/` outside the repository, using the editor binary named in `AGENTS.local.md`: `<godot> --headless -e --path <copy> -- --petrichor-build` compiles every script and prints the diagnostics; `<godot> --headless --path <copy> <scene>` runs a scene, under a timeout, since a script error can keep it from quitting.
- With no implementer, build and run the tests in the tree as `AGENTS.md` describes.
- Check engine facts against the Godot source at `4.7.2-stable` (`ed1daf0`, local checkout in `AGENTS.local.md`), never from memory.

## The findings file

Name it `review-<model>-<YYYY-MM-DD>.md` in the repository root and write it in Chinese, with identifiers and paths as they are. It starts with the HEAD reviewed and the scope, followed by the findings, numbered, most severe first, each with:

- its severity: 高 when a supported feature crashes, corrupts memory or breaks a type guarantee on a legal script, or when later work would build on the mistake; 中 for other wrong behavior, a document that tells a reader something untrue, or a substantial amount of code or tests that can go; 低 for the rest, in one line each;
- `path:line` and the problem in one sentence;
- for a defect, a minimal counterexample: the script, what it does and what should happen instead, marked as run or as reasoned from the code; for something to remove, what still covers it once it is gone;
- the evidence: spec sections, `path:line` in the repository and in the engine (at `ed1daf0`);
- the smallest fix, preferably one that removes code, and a test only when one clears the bar above.

The roadmap follows the findings, then one line on what was covered. Asked to look again, update the same file, marking each item fixed (with its commit), still open or withdrawn.
