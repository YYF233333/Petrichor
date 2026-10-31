# Survey: constructs of real code that the implementation does not support yet

**2026-11-01** · Related: [language specification](../language-spec.en.md), [architecture](../architecture.en.md) §2.6 · 中文：[implementation-coverage.zh.md](implementation-coverage.zh.md)

## Question

The two milestones run the scripts of FPS and Ring in this language (architecture §5, steps 4 and 6), but they cover only the constructs those two projects use. This survey counts how many files of real GDScript projects use a construct that the first version of the specification includes and the implementation at commit `812aaa2` does not support yet.

## Corpus

The same as the [cross-branch duplication survey](cross-branch-duplication.en.md): 20 repositories at fixed commits, with the same first-party rules (`cross-branch-duplication/gdcorpus.py`). There are 2,800 first-party `.gd` files with 203,720 lines of code (blank and comment lines excluded); the 12 game repositories have 1,223 files and 70,646 lines.

## Method

- The 16 constructs of the table below are found by token. Each belongs to the first version of the specification, and the implementation reports it as not supported yet or as a syntax error.
- `Type.m()` counts only when Type is an engine class, a built-in type or a `class_name` class of the same repository, and m is a static method of it; the engine's static methods come from godot-cpp's `extension_api-4-7.json`. `Type.new()` counts only when Type is a `class_name` class of the same repository; `new()` of engine classes is supported.
- Annotations counted: the class annotations `@tool`, `@icon`, `@static_unload` and `@abstract`; among member annotations, all but `@export`, `@export_range`, `@export_enum`, `@export_multiline`, `@onready` and the group annotations, for example `@export_node_path`, `@export_flags`, `@warning_ignore` and `@rpc`.
- On a line starting with `for`, the first `in` is the loop's; every other `in` is the operator.
- A file that uses any of the constructs counts as using them.

## Results

| Construct | Spec | All: files | All: per 1,000 lines | Games: files | Games: per 1,000 lines |
|---|---|---:|---:|---:|---:|
| Files using none of them | | 29.6% | | 40.6% | |
| `const` | §4.5 | 29.8% | 15.2 | 28.6% | 20.9 |
| `Type.m()`, static method of a class of the repository | §4.8 | 22.9% | 16.2 | 20.4% | 12.1 |
| `&"…"` and `^"…"` literals | §2.2 | 20.3% | 19.5 | 20.4% | 27.5 |
| Other member annotations | §2.3 | 13.4% | 6.2 | 5.2% | 2.4 |
| `@tool`, `@icon`, `@static_unload`, `@abstract` | §2.3 | 13.4% | 3.4 | 5.8% | 1.1 |
| `Type.new()`, class of the repository | §4.6 | 13.2% | 6.8 | 6.3% | 1.5 |
| `Type.m()`, static method of an engine class or built-in type | §10 | 9.9% | 4.9 | 3.6% | 1.8 |
| `preload()` of a resource other than a scene | §7.4 | 9.7% | 3.3 | 8.7% | 4.6 |
| Lambdas | §3.11 | 8.5% | 4.9 | 7.4% | 3.3 |
| `%Name` and `$".."` | §7.1, §7.3 | 7.8% | 4.8 | 3.4% | 2.1 |
| `in` and `not in` | §5.1 | 7.4% | 2.8 | 4.9% | 1.4 |
| `enum` | §3.7 | 6.7% | 1.3 | 4.7% | 1.0 |
| `assert` | §8.3 | 4.4% | 2.9 | 5.2% | 1.7 |
| Inner classes | §2.1 | 4.3% | 0.9 | 1.6% | 0.5 |
| Bitwise operators | §3.2, §5.1 | 3.1% | 1.8 | 1.2% | 1.0 |
| `static var` | §4.8 | 2.7% | 0.9 | 2.9% | 1.2 |
| `**` | §5.1 | 0.1% | 0.0 | 0 | 0 |

Files using none of them, by repository:

| Repository | Group | Files | Using none |
|---|---|---:|---:|
| Starter-Kit-FPS | game | 7 | 7 |
| Starter-Kit-3D-Platformer | game | 9 | 9 |
| Starter-Kit-Racing | game | 3 | 3 |
| Starter-Kit-City-Builder | game | 6 | 5 |
| Ring | game | 66 | 55 |
| tanks-of-freedom-ii | game | 292 | 213 |
| godot_open_star_fighter | game | 40 | 25 |
| deck_builder_tutorial | game | 122 | 44 |
| godot-open-rts | game | 118 | 39 |
| tps-demo | game | 15 | 3 |
| godot-open-rpg | game | 78 | 15 |
| godot-tiny-mmo | game | 467 | 79 |
| godot-demo-projects | demos | 461 | 147 |
| Pixelorama | tool | 210 | 27 |
| godot-statecharts | plugin | 89 | 41 |
| gdUnit4 | plugin | 482 | 92 |
| beehave | plugin | 95 | 9 |
| phantom-camera | plugin | 36 | 3 |
| Godot-Game-Template | plugin | 125 | 9 |
| godot_dialogue_manager | plugin | 79 | 4 |

The counts of each construct are in `implementation-coverage/results/coverage.txt`; running the script with `--samples N` lists the first N locations of each.

## Limitations

- Constructs are found by token, without type resolution. Static methods called through a constant or a variable are not counted, such as `Utils.f()` after `const Utils = preload("utils.gd")`; such files are already counted for `const` and `preload`.
- A file using none of the constructs does not necessarily compile. Migrating it still adds types, about 28 edits per 100 lines in the [second paper migration](paper-migration-2.en.md), and other features not implemented yet are not counted here, among them untyped values (specification §3.13), array and dictionary patterns of match, property names as literals (§10.4) and members that must be assigned without being exported (§6.2).
- Constructs that this language adds, such as Result, generics and enums with payloads, do not occur in GDScript and cannot be counted.

## Implications for the implementation

- Across all repositories, seven files in ten use at least one construct that is not implemented yet; in the game repositories, about six in ten. The most common ones are small features: `const`, static methods and `new()` called through a class name, `&"…"` literals, and annotations.
- The projects of the two milestones hardly use these constructs: all 7 files of FPS and 55 of the 66 files of Ring use none of them, against 29.6% across all repositories. This is why neither migration revealed these gaps.

## Reproduction

```
python docs/surveys/cross-branch-duplication/gdcorpus.py corpus
python docs/surveys/implementation-coverage/coverage.py corpus thirdparty/godot-cpp/gdextension/extension_api-4-7.json > docs/surveys/implementation-coverage/results/coverage.txt
```
