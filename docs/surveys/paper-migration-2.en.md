# Survey: paper migration of two real projects under the design decisions

**2026-10-25** · Related: design decisions (commit `a099ad4`), [language specification](../language-spec.en.md), [first paper migration](paper-migration.en.md) · 中文：[paper-migration-2.zh.md](paper-migration-2.zh.md)

The [first paper migration](paper-migration.en.md) evaluated specification v0.1. The project was then redesigned: the new rules were first recorded as a set of design decisions and then written up as the current [language specification](../language-spec.en.md) (v0.2 draft). This survey migrates the same code again under the design decisions as of commit `a099ad4` (then in `docs/decisions.en.md`), and then works through five maintenance scenarios. It has three aims: to measure the cost of migration; to see where the rules rub against each other once combined; and to find what the decisions did not yet say, where the migration had to assume something. All of these assumptions are now part of the specification; see "Assumptions made during the migration". The issues the migration exposed, and how the specification settles them, are in "Issues the migration exposed". Section numbers (§) refer to the current specification. The rewritten code was not type-checked by a compiler, so this is still a paper migration.

## Material

| Project | Commit | License | Files migrated | Code lines |
|---|---|---|---|---|
| [KenneyNL/Starter-Kit-FPS](https://github.com/KenneyNL/Starter-Kit-FPS) | `185fd23` | MIT | all 7 `.gd` files | 258 |
| [YYF233333/Ring](https://github.com/YYF233333/Ring) | `0ec4161` | no license file; included with the repository owner's consent | 14 `.gd` files of the breakout minigame | 647 |
| **Total** | | | **21** | **905** |

Code lines are physical lines that are neither blank nor comments. Ring's 14 files are the 9 of the first migration (622 lines) plus the 5 subclasses of Brick and Enemy that were still GDScript (25 lines). These 5 files have to migrate together with their bases: when GDScript resolves `extends GlobalClassName`, it parses the base class's file as GDScript (`modules/gdscript/gdscript_analyzer.cpp:471-487`), so a GDScript script cannot extend a class written in this language. The first migration missed this constraint. Everything else stays as it was: Paddle, Ball, Skill, Consumables, ResourceManager, ValueManager and the rest remain GDScript, and the C# code and the Dialogue Manager plugin are untouched.

## Method

1. **Rewriting.** The code is rewritten strictly under the design decisions of the time, keeping its structure and names. Where the decisions say nothing and the migration still has to pick a form, it follows assumptions S1–S18 below, which are marked in the code.
2. **Ledger.** Each piece of extra work is one row: file, original line numbers, category, source, count, kind, note.
   - Categories that count as cost: **annotation** (a type annotation the original lacked and the design now requires, including owner requirements); **pub** (a marker for members other code uses); **conversion** (an added `as`, `.unwrap()` or `int(x)`); **rewrite** (a structural change, such as replacing a truthiness test, switching to an interface, or declaring a function async); **decision** (a type choice that needs judgment, such as `T` versus `T?`); **local** (a local variable added only because members do not narrow); **gap** (the original cannot be written as it is under the design of the time); **glue** (adapting data across languages).
   - Categories that do not count as cost: **undefined** (relies on one of the assumptions below); **benefit** (reports or removes a problem in the original).
   - Each item has one of two **kinds**: **mechanical** (M), done by following a rule without changing behavior, so that a quick fix could do it, for example adding `pub`, filling in the parameter types of an engine virtual override, or turning `if x:` into `if x != None:`; and **judgment** (J), which needs a person to decide, for example switching to an interface, choosing `T?`, or narrowing an export type.
   - The counting granularity is that of the first migration: each added annotation, marker, conversion or decision counts 1, and each structural rewrite counts 1.
3. **Maintenance scenarios.** Five common changes are worked through on the migrated code, comparing how many places must change under GDScript and under this language, and when problems surface.
4. **Engine facts.** All were checked against Godot commit `469659e`.

### Assumptions made during the migration

The table lists the assumptions made during the migration. All of them are now specified; the "Current specification" column gives the section, and whether the specification kept the assumption or changed it.

| Id | Assumption | Current specification |
|---|---|---|
| S1 | An omitted return type means void | Kept for functions that are not overrides; an override takes its types from the overridden declaration (§4.6) |
| S2 | int converts implicitly to float in assignments, arguments and returns | Kept (§3.2) |
| S3 | Value-type members (int, float, bool, String, vectors, containers) without an initializer start at their default value; only object references must be assigned | Kept, with rules added for structs and enums with payloads (§4.3) |
| S4 | `x as T`: when the static type of x is already T, the result is T and the compiler notes the cast is redundant; otherwise the result is `T?` and a failed cast yields None, as in GDScript. A conversion that must succeed is written `(x as T).unwrap()` | Kept (§5.3); `as` to a built-in type is now a compile error, so the `as Vector2` in `migrated/ring/oppressive_enemy.pet:14` should be removed |
| S5 | Parameters of engine virtual overrides must be annotated too | Changed: an override may omit its parameter and return types, which are taken from the overridden declaration (§4.6); the 6 annotations on engine virtual overrides are then unnecessary |
| S6 | A required export of a sub-scene counts as assigned if every scene that instances the sub-scene assigns it | Kept; a scene referenced from nowhere gets only a warning (§7.5) |
| S7 | `bind` on a typed function value yields a typed value | Kept (§3.11) |
| S8 | `pop_front()` on `Array[T]` returns T; on an empty array it is a program error | Changed: it returns `T?`, because the engine returns null on an empty array (§3.8); Kenney FPS's audio then needs one more conversion |
| S9 | A typed loop variable does not downcast implicitly (did not occur) | Kept (§5.7) |
| S10 | Utility functions bound with Variant, such as lerp, clamp, wrap, abs, min and max, are typed by their arguments | Changed: mixing int and float is a compile error (§10.3); `min(…, 1)` in Ring's two `get_health_percent` functions must then become `1.0` |
| S11 | Object-typed members and return values of GDScript classes are `T?` | Kept (§11.1) |
| S12 | Audit results: `get_first_node_in_group` is `Node?`, `get_nodes_in_group` is `Array[Node]` | Kept (§10.2) |
| S13 | An owner requirement is written as the class-level annotation `@requires_owner(T)` | Changed: written `@owner(T)` (§7.3) |
| S14 | A property requirement of an interface can be satisfied by an inherited engine property, such as Node2D's `global_position` | Kept (§3.9) |
| S15 | GDScript and C# can reach only the `pub` members of a class in this language, apart from the members the engine accesses by name | Kept (§10.5, §11.3) |
| S16 | Decoding a struct descends into containers, for example `Dictionary[String, ConsumableState]` | Kept (§3.6) |
| S17 | A signal can be awaited directly | Kept (§9.6) |
| S18 | A member handed to the engine by a literal name, as in `tween_property(self, "current_speed")`, is reachable by the engine even if it is not `pub` | Kept (§10.5) |

## Results

### Overall

| | Kenney FPS | Ring | Total | Per 100 lines |
|---|---:|---:|---:|---:|
| All cost | 48 | 182 | 230 | 25.4 |
| Without `pub` | 31 | 97 | 128 | 14.1 |
| Judgment (J) only | 8 | 25 | 33 | 3.6 |

The first migration, under v0.1, cost 28.1 items per 100 lines. v0.1 (git tag `spec-v0.1`) had no visibility rules, so `pub` is a new cost; without it, the cost is about half that of v0.1. Of the 230 items, 197 are mechanical; the items that need a person's judgment come to 3.6 per 100 lines. The migrated code has 935 lines, 3.3% more than the original, mostly from the two new interface files and the message structs.

### Sources of cost

| Source | Items | Note |
|---|---:|---|
| `pub`: used by other files | 59 | 34 of them in BreakoutManager, an autoload reached by GDScript files such as UI, Paddle, Ball and Skill, and by Root.cs |
| `pub`: only for subclasses | 29 | at the time, subclasses could see and override only `pub` members |
| `pub`: implementing an interface | 14 | |
| Type annotations in function signatures | 24 | 6 of them on engine virtual overrides (S5) |
| Truthiness | 18 | `if x:` becomes `!= None`, `!= 0`, `!= ""` or `== 0` |
| Option-typed members | 22 | 6 decisions, 12 `.unwrap()` and 4 locals; see below |
| Interfaces replacing `has_method` and duck-typed access to the owner | 13 | declaring the interface, `implements`, `is` at the call sites, owner requirements, removing a run-time check |
| Downcasts | 8 | passing values whose type was decided by group membership (4), `get_collider()`, `find_children` and others; `find_children` filtered by a literal class name is now typed directly (§10.3) and needs no downcast |
| Coroutines | 7 | functions containing await declared async (4); calls that are not awaited written with `.detach()` (3) |
| Engine nullability | 6 | `get_first_node_in_group`, `CollisionShape2D.shape` |
| Other | 30 | literal member names forcing a parameter from Node to CanvasItem (4), untyped dynamic values (4), float damage to int (4), nullable object members of GDScript classes (3), narrowed export types (3), empty containers and signal parameters (4), message structs (2), subclasses migrating along (2), and others |
| **Total** | **230** | |

### `pub` written only for subclasses

In Ring, Brick and Enemy have between 2 and 5 subclasses each. 17 members of the two base classes are `pub` only because subclasses read or override them, for example `type`, `set_type`, `check_init_type` and `update_stat`, and the overriding methods in the subclasses must be `pub` as well: 29 items in all. The [member visibility survey](member-visibility.en.md) estimated that members used only by subclasses are 7.2% of the members used outside their file; here they are 28%. That survey matched by name, and names like `type`, `check_init_type` and `update_stat`, declared separately in several scripts, fell under "ambiguous"; the 7.2% therefore underestimates this cost in code that uses inheritance heavily. In response, the current specification lets subclasses read, write, call and override members without `pub` (§4.2), so none of these 29 items is needed.

### Members that do not narrow, and Option-typed members

Only 4 locals were added because members do not narrow: 4.4 per 1,000 lines.

The larger cost comes from Option-typed members as such: 22 items, nearly all in BreakoutManager. This autoload holds references to five scene nodes, `breakout`, `skill`, `balls`, `paddle` and `consumables`, which scenes set only after the autoload is ready. Under "a non-null member without a default must be assigned before ready" (§6.2), they can only be `T?`. Where the original tests them for null, they must first be read into a local (3 items); where the original assumes they are set and does not test, they need `.unwrap()` (12 items), which fails at run time just as the original does.

### Benefits

7 in all:
- Kenney FPS's `health_updated` signal is declared without parameters and emitted with one;
- weapon models removed when switching weapons are never freed, and are reported as a leak (§6.7);
- 3 places pass float damage to an integer health, which GDScript truncates silently;
- `ValueManager.fire_hurt_interval` was removed on 2024-06-27, but IntervalHurtComponent still refers to it;
- HealthComponent used to check at run time that its owner is a Brick or an Enemy, printing and calling `queue_free()` otherwise; the owner requirement now checks this in the scenes.

### Maintenance scenarios

| Scenario | GDScript | This language |
|---|---|---|
| A. Add a damageable crate | Write a script with a `damage` method; the player's `has_method("damage")` picks it up. A misspelled method name makes shots silently do nothing | Write `implements Combat.Damageable` and `pub func damage(amount: float)`. A misspelling or a mismatched signature is a compile error. Under both, a crate standing between an enemy and the player gets hit and damaged by the enemy, because the enemy's ray also detects bodies |
| B. Rename `Head/Camera` to `Head/MainCamera` in player.tscn | 4 `$Head/Camera…` paths break; at ready time `get_node` prints an error and returns null (`scene/main/node.cpp:1987-1994`), and the first use fails | The scene model reports the 4 paths at once, in the script and in the scene. When the node is renamed in the scene editor, the 4 paths are updated automatically (§13.3) |
| C. Rename Enemy's export `player` to `target` | The values saved for the 4 instances in main.tscn are dropped silently on load (`scene/resources/packed_scene.cpp:567`) and deleted the next time the scene is saved (`packed_scene.cpp:941-950`); at run time reading `player.position` fails | The rename refactoring also changes the 4 keys in main.tscn, with preview and undo (§13.3); no manual edits |
| D. Add a parameter `max_health` to `health_updated` | 2 edits: the emit and the HUD handler. If the handler is missed, the error appears only when the player is hurt and the signal is emitted (`core/object/object.cpp:1381-1384`) | The same 2 edits plus the signal declaration. The emit and the connection in main.tscn are checked at compile time |
| E. Add a field to the initialization message sent from C# | 4 places: the C# class, `AsDict`, `FromDict`, and the read in GDScript. If C# misses it, the error occurs when `reset` reads the key | The same 4 places, with the GDScript read replaced by a struct field. If C# misses it, the message is rejected at the entry with the missing field named. The field is still written once in each language |

In scenarios A, B and D this language moves the problem to editing time; in scenario C it removes the manual edits altogether, as it also does in scenario B when the node is renamed in the scene editor; in scenario E it only moves the error from the point of use to the entry.

## Issues the migration exposed

These issues were found while migrating under the design decisions of the time; each ends with how the current specification settles it.

1. **The number of `pub` markers.** They are 44% of the cost, all mechanical. The 29 written only for subclasses are far more than the member visibility survey estimated. The current specification lets subclasses access members without `pub`, and overriding keeps visibility (§4.2).
2. **The direction of cross-language inheritance.** GDScript cannot extend a class of this language, so a partial migration has to move whole inheritance trees, while the decisions of the time stated only the other direction. The current specification states that neither direction is supported; the analysis service reports an error on a GDScript file that extends a class of this language, and before a class is migrated it lists the class's GDScript subclasses and offers to migrate them together (§4.1, §11.2, §13.4).
3. **Required exports in sub-scenes.** As written at the time, the rule marks player.tscn and enemy.tscn, which do not assign `crosshair` and `player` themselves, although main.tscn, which uses them, assigns both (S6). The template scene `enemies/base/enemy.tscn`, referenced nowhere, would also be marked and would block a release export. The current specification checks each scene by how it is used: an assignment at any level of the instance and inheritance chain counts, and a scene referenced from nowhere gets only a warning and does not block export (§7.5).
4. **Autoloads holding references to scene nodes.** Such members can only be `T?`, which costs 22 items. The GDScript idiom is to leave them null until a scene sets them. Options are a non-null member assigned after ready, where reading it before assignment is a program error, or relaxing member narrowing, which would remove only 4 of the 22. The idiom is uncommon, though: across 20 repositories, registering a node on an autoload member in `_ready` (`Autoload.x = self`) occurs only 6 times, 5 of them in Ring (`autoload_self.py`). The current specification keeps `T?`; where non-null is wanted, the manager becomes a node in the scene and refers to the other nodes through must-assign exports. The first version has no non-null member assigned after ready (§6.2).
5. **Owner requirements.** Their spelling was undecided (S13). When DeathComponent takes itself out of its owner's subtree with `remove_child`, the engine sets its `owner` to null at once (`scene/main/node.cpp:391-396`, called from `remove_child` at `:1799-1801`); a freed owner is also set to null (`:299-302`). The current specification writes the requirement `@owner(T)` and checks `owner` on every read, not when nodes move; code that needs to test whether there is an owner uses `get_owner()`, whose type is `Node?` (§7.3). A property requirement of an interface can be met by an inherited engine property (S14, §3.9).
6. **`as` and conversions that must succeed.** GDScript's `as` yields null on failure, so under the syntax hard rule `as` can only yield `T?` (S4). The migrated code has 16 occurrences of `(x as T).unwrap()`, while the explicit conversion that "reports an error on failure", required by the decisions of the time, had no spelling yet. The current specification makes `(x as T).unwrap()` the spelling of a downcast that must succeed (§5.3).
7. **Signatures.** The decisions of the time did not say whether parameters of engine virtual overrides must be annotated (S5, 6 items), or whether an omitted return type means void (S1). The current specification lets an override omit its parameter and return types, which are taken from the overridden declaration; for a function that is not an override, an omitted return type means void (§4.6).
8. **The engine reaching members that are not `pub` by name**, as in `tween_property(self, "current_speed")` (S18), and which members GDScript and C# can see (S15). The current specification exposes through reflection only `pub` members and the members the engine must find by name, including members handed to the engine by a literal name (§10.5).

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
