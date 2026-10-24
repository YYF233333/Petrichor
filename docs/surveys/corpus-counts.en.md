# Survey: one-off counts in the corpus

**2026-10-25** · Related: [language specification](../language-spec.en.md) · 中文：[corpus-counts.zh.md](corpus-counts.zh.md)

Some rules in the specification need only a single count as evidence. This document collects such counts over the corpus of the [cross-branch duplication survey](cross-branch-duplication.en.md): 20 repositories at fixed commits, first-party `.gd` files only, 203,720 code lines (blank and comment lines excluded). The "Used by" column gives the sections of the specification that rest on each count.

| Count | Result | Used by |
|---|---|---|
| The `null` literal | 4,521, 22.2 per 1,000 lines; 2,541 comparisons (`== null`, `!= null`) and 1,980 assignments, returns and arguments | §2.2: `null` is an alias of None |
| Prefix `!` and `not` | 460 prefix `!`, 4,504 `not` (including `not in`) | Appendix B: background for not adopting postfix `!`; prefix `!` remains logical not (§2.2) |
| Registering a node on an autoload member in `_ready` (`Autoload.x = self`) | 6, 5 of them in Ring | §6.2: autoload members referring to scene nodes are `T?` (script: `paper-migration-2/autoload_self.py`) |
| `@abstract` classes and methods | 28 classes: 25 in gdUnit4, 2 in Pixelorama, 1 in godot-open-rpg; 250 abstract methods; of the abstract classes declared by a file, 17 have only abstract methods | §4.1: `@abstract` |
| Rest parameters `...args` | 78: 76 in gdUnit4, 2 in Pixelorama | §4.6: rest parameters |
| Lambdas containing await | 35 of 979 lambdas, 33 of them in gdUnit4's tests | §9.1: lambdas are also written `async func` |
| Coroutine methods used as values (passed by name as an argument in the same file) | 91, 60 of them `connect(name)`, across 12 repositories | §3.11: positions the engine calls accept async functions |
| `assert(...)` statements | 595, 406 of them in godot_dialogue_manager; 0 with await in their arguments | §8.3: `assert` is removed in release builds |
| `is_instance_valid(...)` | 678, 3.3 per 1,000 lines; 12.8 per 1,000 lines in phantom-camera | §3.3: a freed object read through `T?` is None |
| `static var` and `_static_init` | 188 `static var`, 4 `_static_init` | §4.8: static members keep GDScript's spelling and timing |
| `NOTIFICATION_PREDELETE` | 40, 32 of them in gdUnit4 | §6.6: no new syntax for destruction |
| `Thread.new` and `WorkerThreadPool` | 5 `Thread.new`, 2 `WorkerThreadPool` | §12.3: the first version adds no threading API |

The counts come from lexical analysis without type resolution; `!` is counted only when not directly followed by `=`. In GDScript `...` appears only in function signatures (`modules/gdscript/gdscript_parser.cpp:1714, 4336`), so every `...` directly followed by a name is a rest parameter. A coroutine is a method whose body contains await; references to it are looked for only in the same file, and signal connections in scenes are not counted.

## Reproduction

```
python docs/surveys/cross-branch-duplication/gdcorpus.py corpus
python docs/surveys/corpus-counts/counts.py corpus
python docs/surveys/paper-migration-2/autoload_self.py corpus
```
