# Survey: one-off counts in the corpus

**2026-10-25** · Related: [design decisions](../decisions.en.md) · 中文: [corpus-counts.zh.md](corpus-counts.zh.md)

Some design decisions need only a single count as evidence. This document collects such counts over the corpus of the [cross-branch duplication survey](cross-branch-duplication.en.md): 20 repositories at fixed commits, first-party `.gd` files only, 203,720 code lines (blank and comment lines excluded).

| Count | Result | Used by |
|---|---|---|
| The `null` literal | 4,521, 22.2 per 1,000 lines; 2,541 comparisons (`== null`, `!= null`) and 1,980 assignments, returns and arguments | Design decisions §9: `null` is an alias of None |
| Prefix `!` and `not` | 460 prefix `!`, 4,504 `not` (including `not in`) | Background for the discussion of postfix `!` |
| Registering a node on an autoload member in `_ready` (`Autoload.x = self`) | 6, 5 of them in Ring | Design decisions §3: autoload members referring to scene nodes are `T?` (script: `paper-migration-2/autoload_self.py`) |
| `@abstract` classes and methods | 28 classes: 25 in gdUnit4, 2 in Pixelorama, 1 in godot-open-rpg; 250 abstract methods; of the abstract classes declared by a file, 17 have only abstract methods | Design decisions §9: `@abstract` |
| Rest parameters `...args` | 78: 76 in gdUnit4, 2 in Pixelorama | Design decisions §9: rest parameters |

The counts come from lexical analysis without type resolution; `!` is counted only when not directly followed by `=`. In GDScript `...` appears only in function signatures (`modules/gdscript/gdscript_parser.cpp:1714, 4336`), so every `...` directly followed by a name is a rest parameter.

## Reproduction

```
python docs/surveys/cross-branch-duplication/gdcorpus.py corpus
python docs/surveys/corpus-counts/counts.py corpus
python docs/surveys/paper-migration-2/autoload_self.py corpus
```
