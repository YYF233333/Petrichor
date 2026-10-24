# Survey: A paper migration of two real projects to spec v0.1

**2026-10-22** · Related: specification v0.1 (commit `7d3e718`, git tag `spec-v0.1`), [second paper migration](paper-migration-2.en.md) · 中文：[paper-migration.zh.md](paper-migration.zh.md)

This survey evaluates the earlier specification v0.1. The project was then redesigned; the current design is the [language specification](../language-spec.en.md) (v0.2 draft), and the [second paper migration](paper-migration-2.en.md) repeats this evaluation under it. Apart from the "Current specification" columns, section numbers (§) in this survey refer to v0.1.

Earlier surveys each answer one specific choice in the specification. This one measures what the specification as a whole costs when applied to real code. We rewrote the GDScript of two projects, line by line, into code that follows v0.1 and recorded every piece of extra work. We then counted how often some of these idioms occur in a 14-repository corpus, to check whether the two projects are representative. No compiler checked the rewritten code, hence "paper migration". The amendment conventions, candidates and assumptions below were adopted or counted for the migration and were not rules of v0.1 at the time; the "Current specification" column of each table shows what became of them.

## The question

A specification that is self-consistent, an implementation that conforms to it, and a language that is comfortable on real projects are different acceptance criteria. Nearly all of the specification's evidence at the time was of the first kind. We wanted to know:

- How many extra annotations, renames, conversions, rewrites and decisions per 100 lines does rewriting real code to v0.1 take?
- Which rules do these costs come from, and how many come from places the specification does not yet cover?
- How much do the costs fall if some rules are relaxed or the gaps are filled?
- Where does v0.1 help?

## Material

| Project | Commit | License | Files migrated | Code lines |
|---|---|---|---|---|
| [KenneyNL/Starter-Kit-FPS](https://github.com/KenneyNL/Starter-Kit-FPS) | `185fd23` | MIT | all 7 `.gd` files | 258 |
| [YYF233333/Ring](https://github.com/YYF233333/Ring) | `0ec4161` | no license file; included with the repository owner's consent | 9 `.gd` files of the breakout minigame | 622 |
| **Total** | | | **16** | **880** |

Code lines are physical lines that are neither blank nor comments.

**Kenney FPS** is a first-person shooter starter kit. It was migrated in full, including the autoload `Audio` and the resource class `Weapon`. The scene and resource files (`.tscn`, `.tres`) were used to check node types, signal connections and exported values.

**Ring** is a visual novel engine that is no longer maintained. Most of it is written in C# (.NET); it includes a breakout minigame written in GDScript and uses the Dialogue Manager plugin (GDScript). The C# runtime and breakout exchange messages as Dictionary values. The 9 migrated files are breakout_manager (an autoload that receives the messages and serves as the signal hub); the health, death and interval_hurt components; Brick and its sturdy and indestructible subclasses; the Enemy base class; and Utility (an autoload). This is treated as a partial migration: these files are rewritten in the language, while Paddle, Skill, Consumables, Ball, ResourceManager, ValueManager and others stay GDScript, and the C# code and the plugin are left alone. This measures the cost of coexisting with GDScript, C# and plugins.

**The frequency corpus** is the one from the [property-accessor survey](property-accessors.en.md#corpus): the same 14 repositories at the same commits, first-party code only (`property-accessors/scope.py`). Frequencies are counted per logical line, that is, after removing comments and blank lines and joining continuation lines: 99,410 lines in all, which is why the figure differs from the physical line counts of the other surveys. The whole of Ring's breakout (1,903 lines) is added as a separate row for comparison.

## Method

1. **Rewriting.** The code was rewritten strictly by v0.1, keeping its structure and names. Where the specification says nothing, or following it would be awkward, we adopted a numbered amendment convention (A1–A8), marked in the code with `# [An]`; the ledger also records what v0.1 itself would require. Five further candidates are counted but not adopted (C-*). Where the specification is silent but some form had to be chosen, we assumed S1–S8, each counted once.
2. **Ledger.** Each piece of extra work is one row: file, original line numbers, category, source, count, the convention that would remove it, and a note.
   - Categories that count as cost: **annotation** (a type annotation v0.1 requires and the original lacks); **rename** (`int`/`float` changed to i32, i64 or f64); **conversion** (an added `as`, `!` or `T(x)`); **rewrite** (a structural change such as a truth test, `is` not narrowing, or switching to a typed utility function); **decision** (a choice that needs checking even though the text is unchanged, such as `T` or `T?` for an object member); **gap** (v0.1 cannot express it, leaving per-class dispatch, duplicated code or nothing); **glue** (cross-language data adaptation).
   - Categories reported apart from cost: **undefined** (depends on a rule the specification does not define); **silent narrowing** (a lossy conversion that does not warn by default); **benefit** (v0.1 reports or removes a problem in the original).
   - Granularity: each added annotation, rename, conversion or decision counts 1; each structural rewrite of an original site counts 1; each gap or glue row counts 1, with its size in the note. A count is therefore a work item, not a number of characters or lines.
3. **Engine facts.** Every engine behavior cited was checked at Godot commit `469659e`; see "Engine evidence" below.
4. **Frequencies.** `idioms.py` counts 11 idioms with regular expressions over the cleaned code; we read a sample of the matches for each. `$X.member` also counts members that Node itself has (such as `queue_free()`), so it is an upper bound; `for in integer` recognizes only integer literals, `.size()` and `len()`, so it is a lower bound.

### Amendment conventions (adopted for the migration)

| Id | Content | Relation to v0.1 | Current specification |
|---|---|---|---|
| A1 | Engine return values and parameters annotated `RequiredResult<T>`/`RequiredParam<T>` are non-null | §6.5 allows completing the binding description once there is evidence; the engine already provides it | Adopted (§10.3) |
| A2 | Engine interfaces that return null only on error (such as `Node.get_tree()`) are marked must-succeed in the binding description: the return type is non-null, and a null result is a program error, as with `$X` | Change: v0.1 gives `T?` | Adopted (§10.2) |
| A3 | Math utilities bound with Variant (abs, sign, clamp, lerp, min, max, wrap and others) are typed by the common type of their arguments and return that type | Fills a gap: under v0.1 they return `Variant?`, so code must switch to typed versions such as absf and clampi or write `T(f(…)!)` | Changed: the result has the arguments' type when they agree, and mixing int and float is a compile error (§10.3) |
| A4 | Operators on core value types follow the engine's Variant operator table; a scalar operand converts to the component type at the checking boundary, with loss warnings as in §7.3 | Fills an open item (engine operators with mixed widths) | Adopted (§5.1) |
| A5 | `for i in n` is allowed for an integer n; the loop variable takes n's type | Relaxes §2.4 | Adopted (§5.7) |
| A6 | GDScript classes and autoloads have a static view: they are visible with their declared types; unannotated parameters, returns and members are `Variant?`; object-typed members and returns are `T?`; a GDScript int is i64; an autoload name is a global identifier whose type is its script class | Fills a gap in §13 | Adopted with changes (§11.1): unannotated ones are Variant, not `Variant?`; a GDScript int is int |
| A7 | Cross-language coroutines: when a call into a GDScript function returns a `GDScriptFunctionState`, the language can await its `completed`; when the engine or GDScript calls a coroutine of the language, the first suspension returns a Signal instead of NIL, which GDScript's await then waits on | Lifts the known limitation in §9; not yet tested | Adopted, still to be tested (§9.8) |
| A8 | Record types, encoded as a Dictionary keyed by field name, decoded by the target type with an entry check on every field | New capability; conflicts with the `struct` proposal in the v0.1 design notes, which encodes positionally as a tagged Array | Adopted: structs are encoded as Dictionaries keyed by field name (§3.6, §11.3) |

### Candidates counted but not adopted in the migration

| Id | Content | Current specification |
|---|---|---|
| C-int | `int` and `float` may be used as type names (for example for i64 and f64) | Adopted: there are only int and float, both 64-bit (§3.2) |
| C-scene | `$X` is typed from the scene the script is attached to | Adopted (§7.1) |
| C-is | `is` narrows the type of locals and parameters | Adopted (§5.4) |
| C-r3 | R3 counts "a parameter used in arithmetic with flexible literals whose result flows to a known type" as evidence | No longer applies: function signatures are not inferred (§4.6) |
| C-trait | Run-time trait objects, which may require properties | Became nominal interfaces, which may require properties (§3.9) |

### Assumptions where the specification is silent

| Id | Assumption | Current specification |
|---|---|---|
| S1 | A script named with `class_name` can be used as a type; a script without class_name cannot be referred to by type | Files without class_name are referred to through `preload` (§2.1) |
| S2 | Boolean negation is written `not`; prefix `!` is unspecified | Prefix `!` is also logical not (§2.2) |
| S3 | The conditional expression `a if c else b` exists | Kept (§5.5) |
| S4 | Global functions such as `preload`, `load`, `str`, `print_debug` and `is_instance_valid` exist; `preload` is typed by file extension, and `load` returns `Resource?` | Changed: `preload` and `load` with a literal path are typed by the resource's actual contents (§7.4) |
| S5 | Dictionary literals `{k: v}` exist | Kept (§2.2) |
| S6 | `Array[T].pop_front()` returns `T?`; `keys()` of an untyped Dictionary returns an untyped Array | Kept (§3.8) |
| S7 | Signal connections saved in scene files are not checked statically; calls go through the entry check of §12.6 | Changed: checked by the scene model (§7.7) |
| S8 | Liveness of handle types is checked with `is_instance_valid` | Changed: a freed object reads as None through `T?`, so `!= None` is also a liveness check (§3.3) |

### Engine evidence

Checked at Godot commit `469659e`; paths are relative to the Godot repository root:

- `RequiredResult<T>` means null is returned only on error, and `RequiredParam<T>` means passing null is an error (`core/object/object.h:950-954, 1062-1067`). They correspond to the metadata `METADATA_OBJECT_IS_REQUIRED` (`core/variant/type_info.h:54`), which the API dump records as `required` (`core/extension/extension_api_dump.cpp:92`).
- Examples with these annotations: the parameters of `Node.add_child`, `add_sibling` and `remove_child` (`scene/main/node.h:526-528`); `Node.create_tween` (`node.h:622`); `SceneTree.get_root`, `create_timer` and `create_tween` (`scene/main/scene_tree.h:308, 438-439`); the chaining methods of Tween (`scene/animation/tween.h:148, 176, 181`).
- Examples without them: `Node.get_tree()` reports an error and returns null when the node is not in a tree (`node.h:559-562`); `Node.get_parent` (`node.h:549`); `SceneTree.get_first_node_in_group` (`scene_tree.h:420`); `RayCast3D.get_collider` (`scene/3d/physics/ray_cast_3d.h:125`); `PackedScene.instantiate` (`scene/resources/packed_scene.h:281`).
- floor, ceil, round, abs, sign, snapped, lerp, wrap, max, min and clamp are bound with Variant (`core/variant/variant_utility.cpp:1656-1728`); typed versions such as absf, lerpf, wrapi and clampf also exist (`core/variant/variant_utility.h:64, 83, 104, 114`). The GDScript analyzer takes their return type from the binding, which is Variant (`modules/gdscript/gdscript_analyzer.cpp:3546-3595`).
- GDScript types an autoload name by the autoload's script (`gdscript_analyzer.cpp:4637-4653`).
- When a GDScript function suspends, it returns a `GDScriptFunctionState` to its caller, whatever language the caller is written in (`modules/gdscript/gdscript_vm.cpp:2603-2640`). That object has a `completed` signal (`modules/gdscript/gdscript_function.cpp:327`). GDScript's await waits on `completed` when given a `GDScriptFunctionState`, waits on any Signal value it is given, and continues at once for any other value (`gdscript_vm.cpp:2585-2599`).
- GDScript's conditional jumps use `Variant::booleanize` (`gdscript_vm.cpp:2753, 2770`), which is false for a freed object (`core/variant/variant_op.cpp:1122-1123`; `core/variant/variant.cpp:980-981`). So `if node:` rejects both null and freed nodes.

## Results

### Overall

| Scenario | Kenney FPS | Ring | Total | Per 100 lines |
|---|---:|---:|---:|---:|
| v0.1 | 64 | 183 | 247 | 28.1 |
| v0.1 + A1 | 62 | 171 | 233 | 26.5 |
| + A1–A8 | 49 | 128 | 177 | 20.1 |
| + A1–A8 + C-int | 35 | 82 | 117 | 13.3 |
| + A1–A8 + C-int + C-scene | 25 | 74 | 99 | 11.2 |
| + all A and C | 19 | 69 | 88 | 10.0 |

A1 only uses annotations the engine already provides and, by §6.5, could count as part of v0.1, so it has a row of its own.

### Sources of cost (v0.1)

| Source | Count | Convention that removes it |
|---|---:|---|
| `int`/`float` are not types | 60 | C-int |
| Engine nullability (`get_tree()`, `RequiredResult`, `RequiredParam` and others) | 35 | A2 18, A1 14; 3 remain |
| Variant math utilities | 23 | A3 |
| Downcasts (`instantiate()`, `get_collider()`, passing a value on after deciding its type by group, and others) | 19 | — |
| Choosing non-null or not for object members and parameters, and the `!` this requires | 18 | — |
| `$X` is Node without an expected type | 18 | C-scene |
| `$X as T` yields an Option and is rewritten as an annotated `@onready` | 14 | — |
| Truth tests | 13 | — |
| `(d[k] as T)!` for message fields | 11 | A8 |
| Interaction with code that stays GDScript (unannotated members, returns that may be null; 8) and message glue (1) | 9 | A8 removes the glue (1) |
| Annotations required by inference (signal parameters, empty-container members, parameters used only as a method-call receiver or passed to Variant) | 9 | C-r3 removes 1 |
| Shared capability, duplicated code, cross-language coroutine | 6 | C-trait 5, A7 1 |
| Other (`is` not narrowing, numeric conversion, integer loops, type names, containers, liveness check) | 12 | C-is 3, A5 2, C-trait 2; the rest remain |
| **Total** | **247** | |

### Items not counted as cost

- **Undefined: 53.** 14 mixed operations between vectors and scalars or between Basis and vectors (A4); 39 autoload accesses (A6).
- **Silent narrowing: 4.** All are a GDScript int (i64) assigned to an i32 member or parameter, which does not warn by default.
- **Benefits: 6.**
  - Kenney FPS declares the `health_updated` signal with no parameters but emits it with one; v0.1 reports this at compile time.
  - 3 places pass float damage to integer health: the original truncates silently, while v0.1 warns and asks for an explicit conversion.
  - `ValueManager.fire_hurt_interval` was deleted in a commit on 2024-06-27, but a component still refers to it; under A6 this is reported at compile time.
  - One return type is inferred as Tween, so the original `as Tween` can go.

These 880 lines contain no payload enum, nested Option, u64, explicit wrapping, static initialization or `weak`. The enum encoding and the detailed numeric conversion rules, which take up the most room in the specification, touch this code only through the `int`/`float` renames and the 4 silent narrowings; the Variant mapping appears only in the message fields, utility return values and unannotated GDScript members.

### Frequencies in the corpus

Occurrences per 1,000 logical lines:

| | Lines | int/float in type position | `$X.member` | `get_tree()` | member use after `is` | `$X as T` | for in integer | Variant math utility | `instantiate()` | create_tween/timer | unannotated `@onready $X` | `has_method` |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Corpus | 99,410 | 40.6 | 16.3 | 4.8 | 4.8 | 4.0 | 2.5 | 2.2 | 1.7 | 1.3 | 1.1 | 0.7 |
| Games and demos | 23,095 | 45.6 | 51.1 | 9.2 | 4.8 | 5.3 | 1.0 | 6.9 | 3.2 | 2.9 | 2.0 | 1.0 |
| Plugins and tools | 76,315 | 39.1 | 5.8 | 3.5 | 4.9 | 3.6 | 3.0 | 0.8 | 1.3 | 0.8 | 0.9 | 0.6 |
| Median of 10 repositories ≥1,000 lines | — | 46.7 | 2.2 | 3.8 | 3.2 | 0.0 | 0.4 | 1.6 | 1.9 | 1.4 | 0.1 | 0.5 |
| Ring breakout (all) | 1,903 | 87.2 | 13.7 | 17.3 | 1.1 | 17.9 | 0.0 | 17.9 | 8.9 | 7.9 | 1.1 | 0.0 |

"Games and demos" means godot-demo-projects, godot-open-rpg, tps-demo and the four Kenney starter kits; godot-demo-projects accounts for 18,728 of those lines. Kenney FPS is itself in the corpus: its 258 lines have 14 int/float, 7 Variant math utilities, 4 `get_tree()` and 9 unannotated `@onready $X`.

- **`int`/`float` in type positions is the most frequent idiom, and not only in these two projects.** The corpus has 40.6 per 1,000 lines, with a repository median of 46.7, about as many as all the other columns together. Ring has twice the corpus rate.
- **`$X.member` is far more common in game code than in plugins**: 51.1 against 5.8 per 1,000 lines. It was rare in the migration sample: Kenney FPS keeps its nodes in `@onready` members, and Ring mostly uses `$X as T`.
- **Ring inflates several items.** Its density of Variant math utilities, `get_tree()` and `$X as T` is 8, 3.6 and 4.5 times the corpus rate. In code at large, Variant math utilities occur about 2 times per 1,000 lines and about 7 times in game code; `get_tree()` about 5 times, and about 9 times in game code.
- **Member use right after `is`** occurs 3 to 5 times per 1,000 lines in every part of the corpus.
- **`$X as T` is the style of a few projects**: the median is 0, and it concentrates in Pixelorama, godot-demo-projects, godot-open-rpg and Ring.

## Examples

**1. Messages between C# and GDScript.**
Ring: `breakout/Root.cs:28-50, 85`; `breakout/scenes/managers/breakout_manager.gd:121-133`

```
public Godot.Collections.Dictionary AsDict()
{
	var dict = new Godot.Collections.Dictionary()
	{
		{ "selected_skill", selected_skill },
		...
	};
	...
}
...
GetNode("/root/BreakoutManager").Call("receive_init_message", data.AsDict());
```

```
func reset():
	if init_message:
		var player_consumables: Dictionary = init_message["player_consumables"]
		...
		for consumable_name in player_consumables.keys():
			var rest_times: int = player_consumables[consumable_name]["rest_times"]
			var transformed: bool = player_consumables[consumable_name]["transformed"]
```

Field names and types are written by hand in three places: the C# `BreakoutMessage` class, the C# `AsDict`/`FromDict`, and the GDScript `reset`/`send_message`. Breakout's notes also record a bug where `player_consumables` could not be read because of JSON parsing. GDScript relies on typed locals converting implicitly; v0.1 does no implicit conversion, so each field becomes `(entry["rest_times"] as i64)!`, 11 places in all. Under A8 the GDScript side becomes a record declaration and `func receive_init_message(message: InitMessage)`, the entry check decodes the fields, and the C# side is unchanged (`paper-migration/migrated/ring/breakout_manager_a8.pet`).

**2. Components duck-typing their owner, and duplicated code.**
Ring: `breakout/scenes/components/health_component.gd:13-28`

```
func _ready():
	if (owner is Brick) || (owner is Enemy):
		pass
	...
	current_health = owner.init_health

func take_damage(value: int):
	var prev_health = current_health
	current_health = min(max(current_health - value, 0), owner.max_health)
```

Brick extends StaticBody2D and Enemy extends CharacterBody2D, so they cannot share a base class. HealthComponent and DeathComponent read and write 6 fields of their owner directly (`init_health`, `max_health`, `unique_drop_name`, `lucky_drop`, `drop_percent_scale`, `point`). v0.1 has no trait objects, so every field access dispatches on the concrete class, adding about 20 lines across the two components. Besides, 64 of Brick's 77 code lines appear verbatim in Enemy: exports, component wiring, `set_type`, `show_health`, `hit_by_ball`, `physic_hurt` and more. Run-time trait objects (C-trait) would remove the dispatch but not the duplication: removing the duplication needs shared implementation, and v0.1 §4 explicitly excludes mixins and field injection.

**3. `has_method` with a single target in practice.**
Kenney FPS: `objects/player.gd:200-205`; `objects/enemy.gd:52-54, 68`

```
var collider = raycast.get_collider()
if collider.has_method("damage"):
	collider.damage(weapon.damage)
```

Both the player and the enemy have a `damage` method, and they extend CharacterBody3D and Node3D. But the player's ray (`collide_with_areas = true` in `player.tscn`) can only hit the enemy's Area3D root, and the enemy's ray ignores areas and can only hit the player or static walls. Rewritten as `(collider as Enemy)?.damage(…)` and `if var Some(target) = … as Player`, both sites behave the same; the cost appears only when a new damageable kind is added later.

**4. Engine nullability.**
Kenney FPS: `objects/player.gd:214, 242`

```
get_tree().root.add_child(impact_instance)
tween = get_tree().create_tween()
```

`root` and `create_tween` are annotated `RequiredResult` in the engine; `get_tree()` is not, and returns null only, with an error, when the node is not in a tree. v0.1 treats every object return without evidence as nullable, so these lines become `get_tree()!.root!` and `get_tree()!.create_tween()!`. Under A1 and A2 both lines are as in the original.

**5. Variant math utilities.**
Kenney FPS: `objects/player.gd:72, 84`; Ring: `breakout/scenes/managers/breakout_manager.gd:45`

```
container.position = lerp(container.position, container_offset - ..., delta * 10)
camera.position.y = lerp(camera.position.y, 0.0, delta * 5)
ammo = max(value, 0)
```

These functions are bound with Variant in the engine and so return `Variant?` by v0.1 §12.6. Under v0.1 they become `container.position.lerp(…)`, `lerpf(…)` and `maxi(…)`, and the migrator has to know the name of each typed version. Under A3 all three lines are as in the original.

**6. Cross-language coroutines.**
Ring: `breakout/scenes/utility/utility.gd:54-58`

```
func dialogue(str: String):
	var res = DialogueManager.create_resource_from_text(str)
	await DialogueManager.get_next_dialogue_line(res)
	DialogueManager.show_dialogue_balloon(res)
```

`get_next_dialogue_line` is a GDScript coroutine in the plugin. v0.1 §9 does not let the language await another language's coroutine, so this function cannot be written in v0.1. As "Engine evidence" shows, a suspending GDScript function hands its caller a `GDScriptFunctionState`, and GDScript's await accepts any Signal value; A7 uses both facts to allow awaiting in either direction.

## Conclusions

- **v0.1 costs about 28 items per 100 lines to migrate to.** With A1–A8 this falls to 20, and to 13 if `int`/`float` are also allowed as type names.
- **The largest single item is that `int`/`float` are not types.** It is a quarter of the v0.1 cost, and the corpus has 40.6 per 1,000 lines, so it is not peculiar to the two projects. Every rename also hides a choice of width; a GDScript int is i64, and returning to i32 narrows silently in 4 places. Nothing in the 880 lines gained from narrow integers.
- **The next group of costs comes from places the specification leaves out or covers in one line.** Engine nullability and Variant math utilities cost 58 items under v0.1, 14 of which only need the annotations the engine already provides (A1); mixed operators and autoload accesses add 53 places that depend on rules not yet defined.
- **The largest parts of the specification were barely touched by this code.** No payload enum, nested Option, u64, explicit wrapping, static initialization or `weak` appears.
- **Game code accesses members of `$X` directly a great deal** (up to 51 per 1,000 lines). Members that Node does not have need an annotation or conversion under v0.1, and the migration sample understates this item.
- **Shared capability across classes has real evidence.** Ring's Brick and Enemy duplicate 64 lines, and its components duck-type 6 fields. The `has_method` sites in Kenney FPS are not evidence for it. Trait objects solve the access but not the duplication.
- **Cross-language:** partial migration depends on A6; A7 appears implementable with existing engine mechanisms; A8 removes the message glue on the language's side but does not touch the C# side.
- **There are benefits, but few.** The 880 lines have 6, including a signal parameter error, 3 silent truncations and a dangling reference.

## Limitations

- **No compiler.** The migrated code was not type-checked and some v0.1 violations may have gone unrecorded, so the v0.1 cost is more likely understated than overstated.
- **The ledger was written and counted by an AI agent, and its granularity is this survey's choice.** Whether a "decision" is a cost is open to argument.
- **Only two projects and 880 lines.** Ring's style differs strongly from the corpus on several items. The partial-migration assumption inflates the cross-language costs; in a full migration these would become calls within the language.
- **Only migration cost was measured.** Writing new code, long-term maintenance and performance were not measured. Costs such as renames arise mainly in migration, while downcasts, Option handling and dispatch arise in new code as well.
- **Frequencies come from regular expressions.** `$X.member` is an upper bound and `for in integer` a lower bound.
- **Scene data was checked only in part.** We checked only the node types, signal connections and exported values the migrated code touches.

## Reproducing

The [`paper-migration/`](paper-migration/) directory contains `ledger.tsv` (the ledger); `stats.py` (computes the cost figures in "Results" and the items not counted as cost from the ledger); `idioms.py` (the corpus frequencies, reusing `extract.py` and `scope.py` from `property-accessors/`); and the migrated code in `migrated/`, one `.pet` file per original file, plus `breakout_manager_a8.pet` with the message part of Ring written under A8. The Kenney FPS license is included as `migrated/fps/LICENSE.md`. The migrated Ring code is released under this repository's MIT license with the original author's consent. Only Python 3.11 is needed.

```
git clone https://github.com/KenneyNL/Starter-Kit-FPS fps && git -C fps checkout 185fd23
git clone https://github.com/YYF233333/Ring ring && git -C ring checkout 0ec4161
python docs/surveys/paper-migration/stats.py fps ring
# corpus: clone the 14 repositories into corpus/ as in the reproduction section of the property-accessor survey
python docs/surveys/paper-migration/idioms.py corpus Ring-breakout=ring/breakout/scenes
```
