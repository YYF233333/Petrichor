# Survey: Which numeric types do literal-initialized locals encounter?

**2026-10-21** · Related: [language specification](../language-spec.en.md) §3 R4 · 中文：[literal-local-constraints.zh.md](literal-local-constraints.zh.md)

This is a record of a small empirical survey that settled one rule of local type inference in the specification. It is written up separately because the method works for many similar design questions: first measure how often the problem actually occurs in real code, then check what each candidate rule would do to that code, and only then decide.

## The question

The language has several fixed-width numeric types (i8…i64, u8…u64, f32, f64). Literals are flexible: a literal takes the type of the other operand or of the expected type. Lossy implicit conversions are allowed but produce one of two kinds of warning: precision loss (off by default) or range loss (on by default).

A local variable initialized only with a literal, such as `var a = 1`, can later run into constraints of several different types:

```
var a = 1
takes_i32(a)
takes_i64(a)
```

Either i32 or i64 for `a` passes the existing conversion rules; the only difference is which warnings appear. The specification therefore has to pick one answer. There were three candidate rules:

| Rule | How the variable's type is chosen | Used by |
|---|---|---|
| A: reject conflicts | Collect constraints across the whole function; use the type only if they all agree, otherwise require an annotation | Rust, Haskell |
| B: initializer only | The initializer alone decides the type (literals take their defaults); every later use is an implicit conversion | Swift, Kotlin, C#, Go |
| C: inflows first | The lossless common type of the initializer and every value assigned into the variable; uses where the variable flows out are implicit conversions | The idea behind Pyright and Crystal, which type a variable by everything assigned to it |

We wanted to know three things: how often real Godot code has more than one type of constraint on such a variable, which type combinations occur, and what each rule would produce.

## Corpus

We used six MIT-licensed Godot 4 projects, covering official demos, small game templates and an RPG framework. "First-party" means excluding the bundled `addons/` directories.

| Repository | Commit | `.gd` files | Lines | First-party lines | Candidates (first-party) |
|---|---|---|---|---|---|
| [godotengine/godot-demo-projects](https://github.com/godotengine/godot-demo-projects) | `b761b4c` | 461 | 30,819 | 28,577 | 27 (24) |
| [gdquest-demos/godot-open-rpg](https://github.com/gdquest-demos/godot-open-rpg) | `d59f2f7` | 342 | 40,662 | 5,422 | 80 (3) |
| [KenneyNL/Starter-Kit-3D-Platformer](https://github.com/KenneyNL/Starter-Kit-3D-Platformer) | `3fa8a04` | 9 | 378 | 378 | 0 |
| [KenneyNL/Starter-Kit-City-Builder](https://github.com/KenneyNL/Starter-Kit-City-Builder) | `4535092` | 6 | 311 | 311 | 0 |
| [KenneyNL/Starter-Kit-FPS](https://github.com/KenneyNL/Starter-Kit-FPS) | `185fd23` | 7 | 451 | 451 | 0 |
| [KenneyNL/Starter-Kit-Racing](https://github.com/KenneyNL/Starter-Kit-Racing) | `2f2e5f2` | 3 | 258 | 258 | 1 (1) |
| **Total** | | **828** | **72,879** | **35,397** | **108 (28)** |

77 of godot-open-rpg's 80 candidates come from its bundled Dialogic plugin. Cogito was also planned, but cloning it failed (repository not found), so it is not included.

## Method

1. **Extract candidates.** A script finds declarations of the form `var name = literal` or `var name := literal` inside function bodies. Literals can be integer or float, may have a sign, and may be written in hex or binary. Class members are left out, because R6 already types a member from its annotation or initializer alone.
2. **Read every case; no sampling.** For each candidate, we printed every line of its function that mentions the variable, then read it to determine the constraints. We read all 108.
3. **What counts as a constraint.**
   - Inflows: values assigned into the variable, and the right-hand side of compound assignments.
   - Outflows: passing the variable as an argument to a parameter of known type; assigning it to a property, member or variable of known type; using it as one side of a comparison or binary operator whose other side has a known type; returning it from a function with an annotated return type.
4. **Type mapping.** Types come from the engine's C++ signatures: `double` → f64, `float` → f32, `real_t` → f32 (standard build), `int` → i32, `int64_t` → i64, `uint64_t` → u64, `uint32_t` → u32. Some common cases:
   - `delta` is f64.
   - Transform-like properties of Node2D, Node3D and Control, and Color components, are f32.
   - Global functions such as `lerpf`, `clampf`, `move_toward` and `randf` are f64; `wrapi`, `clampi` and `randi_range` are i64.
   - `Array.size()`, `String.length()` and `get_child_count()` are i32; the `size()` of Packed arrays is i64.

   Array indexing, `range()` and enum constants do not count as constraints. In user code, an `int` annotation counts as i32 and a `float` annotation as f64. User parameters and members without annotations count as unknown.
5. **Evaluation.** For each variable we recorded the set of constraint types. For variables with more than one type, we worked out what rules A, B and C each produce. Over all variables, we counted the compile errors and warnings each rule would cause.

An AI agent classified each case using the mapping above. The cited snippets and the counts were checked by hand.

## Results

### How many types constrain each variable

| Distinct constraint types | Variables |
|---|---|
| 0 (no constraint of known type) | 42 |
| 1 | 61 (i32 42, f64 8, f32 5, i64 3, u32 3) |
| 2 or more | **5** |

That is 4.6% of candidates, about 0.07 per 1,000 lines, or 0.14 per 1,000 first-party lines; all five are in first-party code. Two of the five only have conflicting types because untyped constants such as `const ACCELERATION = 4` were counted as i32, the default for an integer literal. If such constants were flexible like literals, three would remain.

### Type combinations among the five

| Combination | n | A | B | C |
|---|---|---|---|---|
| f64 flows in, flows out to an f32 engine API | 2 | error | f64, precision loss (off by default) | same as B |
| i32 flows in (`int()`), combined with f64, flows out to f32 (`rotation`) | 1 | error | i32, i32→f32 precision loss (off by default) | same as B |
| i32 flows in from `const X = 4`, combined with f64 (`delta`) | 1 | error | f64, no warning | f64, no warning |
| Several f64 inflows, combined with `const BPM = 116` (i32) | 1 | error | f64, no warning | f64, no warning |

### Each rule across all 108 variables

| Rule | Compile errors | Variables with a range-loss warning (on by default) | Variables with only precision loss (off by default) |
|---|---|---|---|
| A: reject conflicts | 6 (4 if constants are flexible) | 0 | 0 |
| B: initializer only | 0 | 4 (three i32→u32, one f64→i32) | 8 |
| C: inflows first | 0 | 3 (all i32→u32) | 8 |
| **Adopted: C with an outflow fallback** | **0** | **0 or 2** (see below) | ≤ 8 |

Of A's six errors, five come from the variables in the combination table above. The sixth comes from a float literal being compared with an i32 (`var wall_side := 0.0` compared against an i32). **None of the six points to a real bug.**

B and C disagree on only one variable, and there B gives the wrong answer (example 2 below).

C's three range-loss warnings all come from integer variables that are only passed to u32 parameters:

- one is `var z_groups := 1`, which is never reassigned;
- two are Dialogic's `var flags := 0`, which is only combined with engine enum constants using bitwise OR.

Such values are harmless, so the adopted rule adds a fallback to C: **if no value of known type flows in other than the initializer, look at where the variable flows out to. If those types all agree, use that type; otherwise use the default type.**

- With the fallback, `z_groups` becomes u32, and its warning disappears.
- The two `flags` depend on how engine enum constants are typed, which the specification still lists as [Open]:
  - If enum constants are flexible like literals, `flags | SEARCH_MATCH_CASE` carries no type information, so `flags` becomes u32 and there is no warning.
  - If enum constants are i64, `flags` becomes i64 and the two warnings remain.
- The fallback can never produce more warnings than C. When the outflows agree, no use needs a conversion; when they disagree, the result is the same as C. The "≤ 8" in the table follows from this; we did not recount each variable.

## Examples

**1. The most common shape: f64 flows in, then flows out to f32.**
`godot-demo-projects/audio/spectrum/show_spectrum.gd:52-60`

```
var prev_hz := 0.0
for i in range(1, VU_COUNT + 1):
    var hz := i * FREQ_MAX / VU_COUNT                                      # f64
    var magnitude := spectrum.get_magnitude_for_frequency_range(prev_hz, hz).length()   # float parameters, i.e. f32
    ...
    prev_hz = hz
```

Rule A would require an annotation here. Rule C picks f64, and passing the value to the f32 parameter is a precision loss that is off by default. GDScript does the same conversion silently at the engine boundary.

**2. B gives the wrong result.**
`godot-open-rpg/src/field/gamepieces/animation/gamepiece_animation.gd:98-101`

```
var current_position_ratio = 0
if keep_position:
    current_position_ratio = \
        _anim.current_animation_position / _anim.current_animation_length   # double
```

Under B the variable is an i32, so the ratio is truncated to 0, although the author clearly wants a fraction. Both A and C give f64.

**3. The variable only flows out to u32.**
`godot-demo-projects/compute/post_shader/post_process_grayscale.gd:58, 87`

```
var z_groups := 1
...
rd.compute_list_dispatch(compute_list, x_groups, y_groups, z_groups)   # uint32_t parameter
```

Rule C warns about range loss (i32→u32). With the fallback, the variable becomes u32 and the warning disappears.

**4. The only inflows are enum constants.**
`godot-open-rpg/addons/dialogic/Editor/TimelineEditor/TextEditor/timeline_editor_text.gd:249-261`

```
var flags := 0
if match_case:
    flags = flags | SEARCH_MATCH_CASE
...
set_search_flags(flags)                                  # uint32_t
var result := search(search_text, flags, ...)            # uint32_t
```

The outcome depends on how engine enum constants are typed; see above.

## Conclusions

- **Conflicting constraints are rare**: about 3–5% of literal-initialized locals, under 0.1 per 1,000 lines. The typical case is a value that comes in as f64 and goes out to an f32 engine API.
- **Rule A makes people write annotations for no benefit.** None of the errors it reports on real code point to an actual bug.
- **Rule B can silently change what a program does.** A variable initialized with an integer literal and later assigned a float gets truncated.
- **Rule C fits real code best.** With the outflow fallback, it also removes the remaining harmless warnings. This is what R4 in the specification now says.

## Limitations

- **The corpus is small and mostly typed.** Most of it is modern, typed GDScript; 37 locals annotated like `var x: float = 0.0` were excluded. Older, untyped code may have more conflicts, although AI-assisted migration adds annotations anyway.
- **Little first-party code.** 77 of the 108 candidates come from a single plugin, Dialogic.
- **Engine types were mapped by hand.** We read the C++ signatures ourselves, so an occasional `float`/`double` mix-up is possible.
- **Some counts depend on open questions.** How untyped constants and engine enum constants are typed is still undecided in the specification, and this affects some numbers. We report both possibilities above.
- **Untyped node access was treated as unknown.** Nodes obtained through `$` or `%` have no static type here. If they did, Dialogic would contribute one more conflicting variable (f32 inflow, i32 inflow via `max`, f32 outflow). On that variable, B would give a range-loss warning (on by default) for float→int, while C would give f64.
- **B and C differ in only one case.** That case matters, though, because it changes what the program computes rather than just adding a warning.

## Reproducing

The scripts are in [`literal-local-constraints/`](literal-local-constraints/): `extract.py` finds the candidates, and `dump.py` prints each candidate with the surrounding lines of its function. Classification is manual and follows the mapping in the Method section.

```
mkdir corpus && cd corpus
git clone https://github.com/godotengine/godot-demo-projects
git -C godot-demo-projects checkout b761b4c
# clone and check out the other repositories the same way (commits are listed in the corpus table)
cd ..
python docs/surveys/literal-local-constraints/extract.py corpus           # per-repository stats; writes corpus/locals.json
python docs/surveys/literal-local-constraints/dump.py corpus > dump.txt   # every candidate with its context
```
