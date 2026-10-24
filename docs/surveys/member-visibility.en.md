# Survey: where script members are used

**2026-10-24** · Related: [design decisions](../decisions.en.md) §4 encapsulation and naming, §9 syntax · 中文: [member-visibility.zh.md](member-visibility.zh.md)

## Question

Members are private by default, and members meant for other code are marked `pub` (design decisions §4). Two things need deciding: how often `pub` will be written, which decides between a keyword and an annotation; and whether members without `pub` should be visible to subclasses.

## Corpus

The same as the [cross-branch duplication survey](cross-branch-duplication.en.md): 20 repositories at fixed commits, with the same first-party rules (`cross-branch-duplication/gdcorpus.py`). First-party code totals 203,720 lines (blank and comment lines excluded); the 12 game repositories have 70,646 of them.

## Method

- **Members**: top-level `func`, `var`, `signal` and `const` declarations in first-party scripts, static ones included. Overrides of engine virtual methods are not counted. A redeclaration with the same name in a descendant script (an override) is counted once, at the root declaration.
- **Matching is by name, without type resolution.** A member is classified only when its name has exactly one root declaration among the repository's first-party scripts and is not an engine member name; the others are reported as "ambiguous".
- **Categories**:
  - used externally: `.name` appears in a first-party script other than the declaring script and its descendants;
  - used only by subclasses: not used externally, and the name appears in a descendant script, as a call, a read or write, or an override;
  - used only in its own file: neither of the above;
  - scene connection: a function named as the method of a signal connection in a first-party `.tscn`. This is access by name from the engine and is counted separately.
- **Written externally**: an externally used `var` counts as written externally when `.name` is followed by an assignment operator in another script.

## Results

| | All | Games |
|---|---|---|
| First-party code lines | 203,720 | 70,646 |
| Used externally | 4,597 | 2,129 |
| Used only by subclasses | 354 | 124 |
| Used only in own file | 13,471 | 5,614 |
| Ambiguous | 9,429 | 3,714 |
| Scene connection | 1,961 | 400 |
| Used externally, share of classified members | 25.0% | 27.1% |
| Used externally, per 1,000 lines | 22.6 | 30.1 |
| Used only by subclasses, share of members used outside their file | 7.2% | 5.5% |

By kind of member (all repositories):

| | Used externally | Only by subclasses | Only in own file | Ambiguous | Scene connection |
|---|---|---|---|---|---|
| `func` | 2,518 | 232 | 6,430 | 3,824 | 1,961 |
| `var` | 1,332 | 107 | 5,136 | 4,526 | 0 |
| `signal` | 338 | 5 | 119 | 283 | 0 |
| `const` | 409 | 10 | 1,786 | 796 | 0 |

- Of the 354 members used only by subclasses, 85 are overridden by a subclass and 269 are only called, read or written. 144 of them have names starting with `_`, which by GDScript convention means their authors treated them as private.
- In scripts that have subclasses, 690 members are used externally and 354 only by subclasses.
- Of the 1,332 externally used `var`s, 528 are written by other scripts and 804 are only read.
- Per-repository numbers and the full list of members used only by subclasses are in `member-visibility/results/vis.txt`.

## Limitations

- There are more ambiguous members (9,429) than externally used ones, because common names such as `speed` or `health` are often declared separately in several unrelated scripts. Their distribution may differ from that of the classified members. The 22.6 per 1,000 lines is therefore a lower bound on the number of `pub` markers.
- `.name` may refer to a same-named member of a different object. Engine member names and names declared in several places are excluded; the remaining error was not measured.
- Members accessed through strings (`call("m")`, `connect("s", ...)`, `get("x")`) are not counted as used externally.
- Only top-level members are counted; members of inner classes are not.

## Implications for the design

- `pub` is written at least 22 times per 1,000 lines (30 in games), so it is a keyword rather than an annotation.
- Members used only by subclasses are 7.2% of the members used outside their file (5.5% in games). The first version therefore has no subclass-visible level, and these members are marked `pub`. Overriding a member that is not `pub` is a compile error, so adding such a level later does not break existing code.
  - Later: in the [second paper migration](paper-migration-2.en.md) this share is 28%, because this survey counted common names such as `type` and `update_stat` as ambiguous. The design was changed accordingly so that subclasses can access members without `pub` ([design decisions](../decisions.en.md) §4).

## Reproduction

See the [cross-branch duplication survey](cross-branch-duplication.en.md) for how `api.json` is generated.

```
python docs/surveys/cross-branch-duplication/gdcorpus.py corpus
python docs/surveys/cross-branch-duplication/build_api.py ../godot work/api.json   # Godot checkout at 469659e
python docs/surveys/member-visibility/vis.py corpus work/api.json > docs/surveys/member-visibility/results/vis.txt
```
