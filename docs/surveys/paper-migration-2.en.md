# Survey: paper migration of two real projects under the design decisions

**2026-10-25** · Related: [design decisions](../decisions.en.md) (commit `a099ad4`), [first paper migration](paper-migration.en.md) · 中文: [paper-migration-2.zh.md](paper-migration-2.zh.md)

The [first paper migration](paper-migration.en.md) followed specification v0.1. After the design restart, this survey migrates the same code again under the design decisions and then works through five maintenance scenarios. It has three aims: to measure the cost of migration; to see where the decisions rub against each other once combined; and to find what the decisions do not yet say, where the migration had to assume something, so that the rewritten specification can fill it in. The rewritten code was not type-checked by a compiler, so this is still a paper migration. This survey does not change the design decisions.

## Material

| Project | Commit | License | Files migrated | Code lines |
|---|---|---|---|---|
| [KenneyNL/Starter-Kit-FPS](https://github.com/KenneyNL/Starter-Kit-FPS) | `185fd23` | MIT | all 7 `.gd` files | 258 |
| [YYF233333/Ring](https://github.com/YYF233333/Ring) | `0ec4161` | no license file; included with the repository owner's consent | 14 `.gd` files of the breakout minigame | 647 |
| **Total** | | | **21** | **905** |

Code lines are physical lines that are neither blank nor comments. Ring's 14 files are the 9 of the first migration (622 lines) plus the 5 subclasses of Brick and Enemy that were still GDScript (25 lines). These 5 files have to migrate together with their bases: when GDScript resolves `extends GlobalClassName`, it parses the base class's file as GDScript (`modules/gdscript/gdscript_analyzer.cpp:471-487`), so a GDScript script cannot extend a class written in this language. The first migration missed this constraint. Everything else stays as it was: Paddle, Ball, Skill, Consumables, ResourceManager, ValueManager and the rest remain GDScript, and the C# code and the Dialogue Manager plugin are untouched.

## Method

1. **Rewriting.** The code is rewritten strictly under the design decisions, keeping its structure and names. Where the decisions say nothing and the migration still has to pick a form, it follows assumptions S1–S18 below, which are marked in the code.
2. **Ledger.** Each piece of extra work is one row: file, original line numbers, category, source, count, kind, note.
   - Categories that count as cost: **annotation** (a type annotation the original lacked and the design now requires, including owner requirements); **pub** (a marker for members other code uses); **conversion** (an added `as`, `.unwrap()` or `int(x)`); **rewrite** (a structural change, such as replacing a truthiness test, switching to an interface, or declaring a function async); **decision** (a type choice that needs judgment, such as `T` versus `T?`); **local** (a local variable added only because members do not narrow); **gap** (the original cannot be written as it is under the decisions); **glue** (adapting data across languages).
   - Categories that do not count as cost: **undefined** (relies on one of the assumptions below); **benefit** (reports or removes a problem in the original).
   - Each item has one of two **kinds**: **mechanical** (M), done by following a rule without changing behavior, so that a quick fix could do it, for example adding `pub`, filling in the parameter types of an engine virtual override, or turning `if x:` into `if x != None:`; and **judgment** (J), which needs a person to decide, for example switching to an interface, choosing `T?`, or narrowing an export type.
   - The counting granularity is that of the first migration: each added annotation, marker, conversion or decision counts 1, and each structural rewrite counts 1.
3. **Maintenance scenarios.** Five common changes are worked through on the migrated code, comparing how many places must change under GDScript and under this language, and when problems surface.
4. **Engine facts.** All were checked against Godot commit `469659e`.

### Assumptions made during the migration

| Id | Assumption |
|---|---|
| S1 | An omitted return type means void |
| S2 | int converts implicitly to float in assignments, arguments and returns |
| S3 | Value-type members (int, float, bool, String, vectors, containers) without an initializer start at their default value; only object references must be assigned |
| S4 | `x as T`: when the static type of x is already T, the result is T and the compiler notes the cast is redundant; otherwise the result is `T?` and a failed cast yields None, as in GDScript. A conversion that must succeed is written `(x as T).unwrap()` |
| S5 | Parameters of engine virtual overrides must be annotated too |
| S6 | A required export of a sub-scene counts as assigned if every scene that instances the sub-scene assigns it |
| S7 | `bind` on a typed function value yields a typed value |
| S8 | `pop_front()` on `Array[T]` returns T; on an empty array it is a program error. A later decision makes it return `T?`, because the engine simply returns null on an empty array (design decisions §6); under that rule Kenney FPS's audio needs one more conversion |
| S9 | A typed loop variable does not downcast implicitly (did not occur) |
| S10 | Utility functions bound with Variant, such as lerp, clamp, wrap, abs, min and max, are typed by their arguments (the binding metadata of design decisions §5). A later decision makes mixing int and float a compile error (design decisions §5); under that rule `min(…, 1)` in Ring's two `get_health_percent` functions must become `1.0` |
| S11 | Object-typed members and return values of GDScript classes are `T?` |
| S12 | Audit results: `get_first_node_in_group` is `Node?`, `get_nodes_in_group` is `Array[Node]` |
| S13 | An owner requirement is written as the class-level annotation `@requires_owner(T)` |
| S14 | A property requirement of an interface can be satisfied by an inherited engine property, such as Node2D's `global_position` |
| S15 | GDScript and C# can reach only the `pub` members of a class in this language, apart from the members the engine accesses by name listed in design decisions §4 |
| S16 | Decoding a struct descends into containers, for example `Dictionary[String, ConsumableState]` |
| S17 | A signal can be awaited directly |
| S18 | A member handed to the engine by a literal name, as in `tween_property(self, "current_speed")`, is reachable by the engine even if it is not `pub` |

## Results

### Overall

| | Kenney FPS | Ring | Total | Per 100 lines |
|---|---:|---:|---:|---:|
| All cost | 48 | 182 | 230 | 25.4 |
| Without `pub` | 31 | 97 | 128 | 14.1 |
| Judgment (J) only | 8 | 25 | 33 | 3.6 |

The first migration, under v0.1, cost 28.1 items per 100 lines. v0.1 had no visibility rules (`language-spec.en.md:830`), so `pub` is a new cost; without it, the cost is about half that of v0.1. Of the 230 items, 197 are mechanical; the items that need a person's judgment come to 3.6 per 100 lines. The migrated code has 935 lines, 3.3% more than the original, mostly from the two new interface files and the message structs.

### Sources of cost

| Source | Items | Note |
|---|---:|---|
| `pub`: used by other files | 59 | 34 of them in BreakoutManager, an autoload reached by GDScript files such as UI, Paddle, Ball and Skill, and by Root.cs |
| `pub`: only for subclasses | 29 | subclasses see and override only `pub` members |
| `pub`: implementing an interface | 14 | |
| Type annotations in function signatures | 24 | 6 of them on engine virtual overrides (S5) |
| Truthiness | 18 | `if x:` becomes `!= None`, `!= 0`, `!= ""` or `== 0` |
| Option-typed members | 22 | 6 decisions, 12 `.unwrap()` and 4 locals; see below |
| Interfaces replacing `has_method` and duck-typed access to the owner | 13 | declaring the interface, `implements`, `is` at the call sites, owner requirements, removing a run-time check |
| Downcasts | 8 | passing values whose type was decided by group membership (4), `get_collider()`, `find_children` and others |
| Coroutines | 7 | functions containing await declared async (4); calls that are not awaited written with `.detach()` (3) |
| Engine nullability | 6 | `get_first_node_in_group`, `CollisionShape2D.shape` |
| Other | 30 | literal member names forcing a parameter from Node to CanvasItem (4), untyped dynamic values (4), float damage to int (4), nullable object members of GDScript classes (3), narrowed export types (3), empty containers and signal parameters (4), message structs (2), subclasses migrating along (2), and others |
| **Total** | **230** | |

### `pub` written only for subclasses

In Ring, Brick and Enemy have between 2 and 5 subclasses each. 17 members of the two base classes are `pub` only because subclasses read or override them, for example `type`, `set_type`, `check_init_type` and `update_stat`, and the overriding methods in the subclasses must be `pub` as well: 29 items in all. The [member visibility survey](member-visibility.en.md) estimated that members used only by subclasses are 7.2% of the members used outside their file; here they are 28%. That survey matched by name, and names like `type`, `check_init_type` and `update_stat`, declared separately in several scripts, fell under "ambiguous"; the 7.2% therefore underestimates this cost in code that uses inheritance heavily.

### Members that do not narrow, and Option-typed members

Only 4 locals were added because members do not narrow: 4.4 per 1,000 lines, the same order as the earlier corpus estimate of an upper bound (about 3.3 per 1,000 lines in game code).

The larger cost comes from Option-typed members as such: 22 items, nearly all in BreakoutManager. This autoload holds references to five scene nodes, `breakout`, `skill`, `balls`, `paddle` and `consumables`, which scenes set only after the autoload is ready. Under "a non-null member without a default must be assigned before ready", they can only be `T?`. Where the original tests them for null, they must first be read into a local (3 items); where the original assumes they are set and does not test, they need `.unwrap()` (12 items), which fails at run time just as the original does.

### Benefits

7 in all:
- Kenney FPS's `health_updated` signal is declared without parameters and emitted with one;
- weapon models removed when switching weapons are never freed, and are reported as a leak (design decisions §4);
- 3 places pass float damage to an integer health, which GDScript truncates silently;
- `ValueManager.fire_hurt_interval` was removed on 2024-06-27, but IntervalHurtComponent still refers to it;
- HealthComponent used to check at run time that its owner is a Brick or an Enemy, printing and calling `queue_free()` otherwise; the owner requirement now checks this in the scenes.

### Maintenance scenarios

| Scenario | GDScript | This language |
|---|---|---|
| A. Add a damageable crate | Write a script with a `damage` method; the player's `has_method("damage")` picks it up. A misspelled method name makes shots silently do nothing | Write `implements Combat.Damageable` and `pub func damage(amount: float)`. A misspelling or a mismatched signature is a compile error. Under both, a crate standing between an enemy and the player gets hit and damaged by the enemy, because the enemy's ray also detects bodies |
| B. Rename `Head/Camera` to `Head/MainCamera` in player.tscn | 4 `$Head/Camera…` paths break; at ready time `get_node` prints an error and returns null (`scene/main/node.cpp:1987-1994`), and the first use fails | The scene model reports the 4 paths at once, in the script and in the scene. Whether renaming the node in the scene editor updates the paths in the script automatically is not stated in the design decisions |
| C. Rename Enemy's export `player` to `target` | The values saved for the 4 instances in main.tscn are dropped silently on load (`scene/resources/packed_scene.cpp:567`) and deleted the next time the scene is saved (`packed_scene.cpp:941-950`); at run time reading `player.position` fails | The rename refactoring also changes the 4 keys in main.tscn, with preview and undo (design decisions §2); no manual edits |
| D. Add a parameter `max_health` to `health_updated` | 2 edits: the emit and the HUD handler. If the handler is missed, the error appears only when the player is hurt and the signal is emitted (`core/object/object.cpp:1381-1384`) | The same 2 edits plus the signal declaration. The emit and the connection in main.tscn are checked at compile time |
| E. Add a field to the initialization message sent from C# | 4 places: the C# class, `AsDict`, `FromDict`, and the read in GDScript. If C# misses it, the error occurs when `reset` reads the key | The same 4 places, with the GDScript read replaced by a struct field. If C# misses it, the message is rejected at the entry with the missing field named. The field is still written once in each language |

In scenarios A, B and D this language moves the problem to editing time; in scenario C it removes the manual edits altogether; in scenario E it only moves the error from the point of use to the entry.

## What the design decisions need to add or reconsider

1. **The number of `pub` markers.** They are 44% of the cost, all mechanical. The 29 written only for subclasses are far more than the member visibility survey estimated. Whether subclasses should see members that are not `pub` deserves another discussion.
2. **The direction of cross-language inheritance.** GDScript cannot extend a class of this language, so a partial migration has to move whole inheritance trees. Design decisions §1 states only the other direction; this one should be added, and migration tooling should point it out.
3. **Required exports in sub-scenes.** As written, the rule marks player.tscn and enemy.tscn, which do not assign `crosshair` and `player` themselves, although main.tscn, which uses them, assigns both (S6). The template scene `enemies/base/enemy.tscn`, referenced nowhere, would also be marked and would block a release export. The rule needs to cover values assigned by the instancing scene, and scenes that nothing references.
4. **Autoloads holding references to scene nodes.** Such members can only be `T?`, which costs 22 items. The GDScript idiom is to leave them null until a scene sets them. Options are a non-null member assigned after ready, where reading it before assignment is a program error, or relaxing member narrowing, which would remove only 4 of the 22. The idiom is uncommon, though: across 20 repositories, registering a node on an autoload member in `_ready` (`Autoload.x = self`) occurs only 6 times, 5 of them in Ring (`autoload_self.py`).
5. **Owner requirements.** Their spelling is undecided (S13). When DeathComponent takes itself out of its owner's subtree with `remove_child`, the engine sets its `owner` to null at once (`scene/main/node.cpp:391-396`, called from `remove_child` at `:1799-1801`); a freed owner is also set to null (`:299-302`). The type and the timing of the check need to account for this. Whether a property requirement of an interface can be met by an engine property (S14) also needs to be settled.
6. **`as` and conversions that must succeed.** GDScript's `as` yields null on failure, so under the syntax hard rule `as` can only yield `T?` (S4). The migrated code has 16 occurrences of `(x as T).unwrap()`; the explicit conversion that "reports an error on failure", required by design decisions §5, has no spelling yet.
7. **Signatures.** Whether parameters of engine virtual overrides must be annotated (S5, 6 items), and whether an omitted return type means void (S1), need to be stated.
8. **The engine reaching members that are not `pub` by name**, as in `tween_property(self, "current_speed")` (S18), and which members GDScript and C# can see (S15).
9. **The remaining assumptions** (S2, S3, S7, S8, S10, S11, S16, S17) all match GDScript's behavior and only need to be written down when the specification is rewritten.

## Limitations

- **No compiler.** The migrated code was not type-checked, and items may have been missed.
- **The ledger was written and counted by an AI agent.** The split into mechanical and judgment items and the counting granularity are choices made here.
- **Two projects, 905 lines.** Ring uses more inheritance than typical code, and BreakoutManager alone accounts for 74 items.
- **Partial migration.** The files still in GDScript determine which members must be `pub`. Under a full migration, these members would be used by other files in this language and would need `pub` just the same; only the one use from Root.cs would take another form.
- **The maintenance scenarios were reasoned through**, not carried out in the editor.

## Reproduction

The [`paper-migration-2/`](paper-migration-2/) directory contains `ledger.tsv` (the ledger); `stats.py` (computes the numbers in "Results" from the ledger, reading the originals only to count lines); `autoload_self.py` (counts `Autoload.x = self` in the 20 repositories of the [cross-branch duplication survey](cross-branch-duplication.en.md)); and in `migrated/` the migrated code, one `.pet` file per original file, plus a new `combat.pet` in each project. Kenney FPS's license is included in `migrated/fps/LICENSE.md`. Only Python 3 is needed.

```
git clone https://github.com/KenneyNL/Starter-Kit-FPS fps && git -C fps checkout 185fd23
git clone https://github.com/YYF233333/Ring ring && git -C ring checkout 0ec4161
python docs/surveys/paper-migration-2/stats.py fps ring
python docs/surveys/cross-branch-duplication/gdcorpus.py corpus
python docs/surveys/paper-migration-2/autoload_self.py corpus
```
