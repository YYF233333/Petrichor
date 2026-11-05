# Reviewing Petrichor

The owner asks for reviews from time to time, often from other vendors' models. This file is the standing brief, and the owner's message adds only the scope. Read `AGENTS.md` first: what the project is, where each kind of fact is recorded and the rules the implementer works by.

## Scope

Review what the message names: a commit range, a feature, sections of the spec or the whole repository. Without one, review the commits not yet pushed (`git log origin/main..HEAD`); if there are none, say so and stop. Uncommitted changes are work in progress, reviewed only when the message asks.

- Changes: whether the code does what the spec says, including the cases its tests leave out; whether the spec (§14, §15), the architecture and `BACKLOG.md` changed with it; debt left in the code it touched.
- The spec: sections that contradict each other; rules that real code runs into, shown with that code; Appendix A against the engine source; the Chinese and English versions saying different things.
- The whole repository: crashes, memory corruption, deadlocks and holes in the type guarantees; documents that describe what the code does not do; language features that no test runs.

## What is not a finding

The repository has already settled these, so a finding against them needs evidence they did not weigh:

- what the spec leaves out of the first version or keeps open (§14), the alternatives it rejected (Appendix B), and the unfinished work in `BACKLOG.md`;
- the order of work in architecture §5 (optimization waits for step 9 and its measurements) and the limits in architecture §6, such as bytecode files crafted by hand (§6.1);
- few comments, and tests of what this language's scripts do rather than of engine behavior.

Leave out style and refactoring that no script could observe, risks that no plausible script runs into, and, unless the message asks, scores, praise, time estimates and advice on the roadmap.

## Working in the shared tree

The implementer keeps working in this tree while you review.

- Write only your findings file. Do not build, run tests, or change other files or the git state; use git only to read (`log`, `show`, `diff`, `blame`).
- Check engine facts against the Godot source at `4.7.2-stable` (`ed1daf0`, local checkout in `AGENTS.local.md`), never from memory.
- To try something, copy `tests/engine/project` with its `bin/` and `.godot/` to a directory outside the repository, add your scripts there and use the editor binary named in `AGENTS.local.md`: `<godot> --headless -e --path <copy> -- --petrichor-build` compiles every script and prints the diagnostics; `<godot> --headless --path <copy> <scene>` runs a scene, under a timeout, since a script error can keep it from quitting.

## The findings file

Name it `review-<model>-<YYYY-MM-DD>.md` in the repository root and write it in Chinese, with identifiers and paths as they are. It starts with the commit reviewed (`git rev-parse --short HEAD`) and the scope, followed by the findings, numbered, most severe first, each with:

- its severity: 高 when a supported feature crashes, corrupts memory or breaks a type guarantee on code that real scripts write, or when later work would build on the mistake; 中 for other wrong behavior, or a document that tells a reader something untrue; 低 for the rest, in one line each;
- `path:line` and the problem in one sentence;
- a minimal counterexample: the script, what it does and what should happen instead, marked as run or as reasoned from the code;
- the evidence: spec sections, `path:line` in the repository and in the engine (at `ed1daf0`);
- the minimal fix, and the test that would show it, in the compiler tests or the engine tests (architecture §3).

It ends with one line on what was covered. Asked to look again, update the same file, marking each item fixed (with its commit), still open or withdrawn.
