# Survey: constant expressions

**2026-11-01** · Related: [language specification](../language-spec.en.md) §4.5 · 中文: [constant-expressions.zh.md](constant-expressions.zh.md)

## Question

GDScript computes the values of constants at compile time with the engine; the compiler of this language does not depend on the engine. This survey counts what computing the initializers of constants in real code takes, and which positions need the value of a constant at compile time, so that §4.5 of the specification can decide which part the compiler computes and which part the engine computes when the class loads.

## Corpus

The same as the [cross-branch duplication survey](cross-branch-duplication.en.md): 20 repositories at fixed commits, with the same rules for first-party code. There are 2,800 first-party `.gd` files, 1,223 of them in the 12 game repositories.

## Method

- `const` declarations are found by tokens and classified by the form of their initializer, each declaration into the first category it fits.
- They are classified again by what computing them takes:
  - no computation: literals, names, `preload`, constructors that only store components (such as `Vector2(1, 2)` and `Plane(Vector3.UP, 0)`), and arrays and dictionaries made of these;
  - scalar computation: operations on int, float, bool and String, `int()`, `float()`, and math functions that return int or bool;
  - operations on engine types: a constructor or constant of a built-in type appears in the operation, or a constant of the same file initialized with one;
  - math functions that return float;
  - Color from a string, checking whether the string is hexadecimal.
- Positions that need the value at compile time: annotation arguments and enum values; constants in match patterns and default parameter values are counted apart.
- Other uses: how constants that preload scripts are used, accesses to constants from other files, writes to containers in constants.

## Results

3,094 constants in 835 files (30%); 1,474 in 350 files of the game repositories.

### Forms of initializers

| Form | All | Games |
|---|---:|---:|
| Literal (int 510, float 362, String 1,101, bool 24) | 1,997 | 987 |
| `&"…"` | 130 | 50 |
| `preload`: scripts 208, scenes 109, other resources 92 | 409 | 179 |
| Array literal | 106 | 49 |
| Dictionary literal | 84 | 49 |
| Constructor of a built-in type (Color 117, Vector2 21, Vector2i 6, Vector3 4, Plane 3 and others) | 156 | 101 |
| Constant of a built-in type, such as `Color.RED` | 8 | 7 |
| Name or expression with operators | 198 | 48 |
| Utility function (`deg_to_rad` 4, `tan(deg_to_rad(…))` 1, `int()` 1) | 6 | 4 |

### What computing them takes

| Computation | All | Games |
|---|---:|---:|
| None | 2,920 | 1,434 |
| Scalar computation | 141 | 35 |
| Color from a hexadecimal string | 24 | 2 |
| Math functions returning float | 5 | 3 |
| Operations on engine types | 4 | 0 |

- Scalar computations are such as `24 * 60 * 60 * 1000`, `1 << 14`, `1.0 / 60.0 * 30.0`, `MOUSE_BUTTON_MASK_LEFT | MOUSE_BUTTON_MASK_RIGHT`, `GROUP_TEST + "/flaky_check_enable"` (60 such concatenations in one file of gdUnit4) and `int(1.10 * 60 * 1000)`.
- There are only four operations on engine types: `Vector3.ONE / 2.0`, `Vector3(0.5, 0.5, 0.5) * Chunk.CHUNK_SIZE` and `Color.WHITE * Color(1, 1, 1, 0.5)`, all in godot-demo-projects, and `-Vector2i.ONE` in an array in Pixelorama's `Drawers.gd:39`.
- The math functions returning float are `deg_to_rad`, 4 times, and `tan(deg_to_rad(30))` once.
- Every Color built from a string uses hexadecimal; none uses a color name.
- Absent: `%` formatting, `is`, `as`, `in`, static methods, GDScript's own utility functions (`Color8`, `len`, `char`, `ord` and others), and constructors of Basis, Quaternion and the like.

### Positions that need the value at compile time

- Of 1,622 annotations with arguments, 7 use a constant of the repository: 6 `@export_placeholder` and 1 `@export_enum`. Each of these constants is initialized with a literal, such as `const EXPRESSION_PLACEHOLDER: String = "Insert an expression..."` in beehave's `leaf.gd:6`. Another 11 annotations use engine constants.
- Of 140 enum members with a written value, 136 are literals and 4 are shifts of literals; none uses a constant.
- Match patterns name a constant 339 times (181 bare names, 158 dotted), and default parameter values use a constant 56 times.

### Other uses

- Writes to containers in constants: none.
- The 208 constants that preload scripts are used, in the same file, as types 140 times, in `X.new()` 55 times and to call static methods 111 times.
- Constants are accessed from other files as `ClassName.CONST` 1,721 times in 263 files; `self.CONST` 721 times, all in tanks-of-freedom-ii; through instances about 35 times.

Samples of each category are in `constant-expressions/results/constants.txt`, which lists every initializer that needs the engine.

## Limits

- Matching is by tokens, without type resolution. Engine types are recognized only by the names of built-in types in the same expression and by constants of the same file initialized with a constructor, so an engine-type value computed through a constant of another file is counted as scalar computation.
- Inherited constants are missed in inner classes; accesses through instances are a rough estimate.
- Only forms that GDScript already has are counted.

## Design evaluated

§4.5 of the specification: the compiler computes scalars, the engine computes operations on engine types and math functions returning float when the class loads, and every case in which loading could fail on a value is checked at compile time. Under this design, 3,061 of the 3,094 constants get their values at compile time and 33 need the engine; the constants used in positions that need a value at compile time are all scalars initialized with literals; and none of the forms the specification does not support appears in the corpus.

## Reproduction

```
python docs/surveys/cross-branch-duplication/gdcorpus.py corpus
python docs/surveys/constant-expressions/constants.py corpus thirdparty/godot-cpp/gdextension/extension_api-4-7.json > docs/surveys/constant-expressions/results/constants.txt
```
