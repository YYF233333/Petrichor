# Survey: Duplicated code between scripts with different native bases

**2026-10-23** · Related: [language specification](../language-spec.en.md) §3.9, §4.1; [scene-structure survey](scene-structures.en.md) (same corpus) · 中文：[cross-branch-duplication.zh.md](cross-branch-duplication.zh.md)

A Godot script can extend only one native class. When two objects with different native classes need the same behavior, for example a StaticBody2D and a CharacterBody2D, they cannot share a script base, so the code is copied, moved into a child node, or written some other way. This survey measures, over 20 repositories, how common the duplication caused by this restriction is and what is duplicated, and judges group by group which mechanism would remove it. All numbers are computed by scripts; the mechanism labels are judgements made by reading the code and are marked as such.

## The question

The following engine facts were checked at Godot commit `469659e`; paths are relative to the Godot repository root:

- An object has a single script instance (`core/object/object.h:451`); `Object::set_script` deletes the existing instance before creating a new one (`core/object/object.cpp:1060-1068`).
- A GDScript has one base script and one native class (`modules/gdscript/gdscript.h:82-83`); `extends` takes one path or one dotted class name (`modules/gdscript/gdscript_parser.cpp:1014-1055`).
- When a script is attached to an object, `GDScript::instance_create` walks up to the topmost native class and fails if the object is not an instance of it (`modules/gdscript/gdscript.cpp:419-430`); `Object::is_class` checks the object's whole class hierarchy (`core/object/object.cpp:2187-2189`).

So a script that extends Control can be attached to a VBoxContainer, but the common ancestor of StaticBody2D and CharacterBody2D is PhysicsBody2D (`doc/classes/StaticBody2D.xml:2`, `doc/classes/CharacterBody2D.xml:2`): a script base they share can extend at most PhysicsBody2D, which lacks CharacterBody2D's own members such as `velocity` (`doc/classes/CharacterBody2D.xml:198`).

Below, the engine class at the top of a script's inheritance chain is called its native base. This survey asks:

- How much code is duplicated between scripts whose native bases differ and that share no script ancestor (cross-branch, below)? How does that compare with duplication that inheritance could already remove but did not?
- What kind of code is duplicated: fields, lifecycle callbacks, or ordinary methods?
- Which mechanism would remove it: interface default methods, stateful compile-time mixins, composition, or static helpers?
- When projects call a member of the same name on objects of different types, how do they write it?

## Corpus

The same 20 repositories as the [scene-structure survey](scene-structures.en.md); commits are in the table under "Per repository": 14 of them are at the same commits as in the [property-accessor survey](property-accessors.en.md), 5 more game repositories are at their latest commit on 2026-10-23, and Ring is at the same commit as in the [paper-migration survey](paper-migration.en.md). They fall into three groups:

- **Games** (12): godot-open-rpg, the four Kenney starter kits, tps-demo, godot-open-rts, tanks-of-freedom-ii, deck_builder_tutorial, godot-tiny-mmo, godot_open_star_fighter, Ring.
- **Demos**: godot-demo-projects, of which 132 Godot projects have first-party code.
- **Plugins and tools** (7): beehave, gdUnit4, godot_dialogue_manager, Godot-Game-Template, godot-statecharts, phantom-camera, Pixelorama.

Only first-party code counts, with the same scope as in the scene-structure survey: everything outside `addons/`, the addons the repositories exist to publish (including godot-tiny-mmo's `addons/tinymmo`), and every `addons/` directory of godot-demo-projects and Godot-Game-Template (`FIRST_PARTY_ADDONS` in `gdcorpus.py`). Unit tests count as code, but unit pairs involving tests are a separate category and are left out of the cross-branch and other figures below. Only `.gd` and `.tscn` files are analyzed; Ring's C# code is not included.

**Licenses.** All are MIT except Ring. [YYF233333/Ring](https://github.com/YYF233333/Ring) (commit `0ec4161`) has no license file; its code excerpts are included with the repository owner's consent.

## Method

1. **Units and logical lines.** A unit is a script class or an inner class in it; there are 2,989 first-party units. The tokenizer in `gdcorpus.py` removes comments and joins lines continued inside brackets or with a trailing backslash. The following lines are trivial and are neither compared nor counted as code: `pass`, `return`, `else:`, `break`, `continue`, a lone string, a line with only annotations, `extends`, `class_name`, a `class X:` header, and the `func _x(...):` header of an engine virtual method. The remaining lines are called logical lines below; first-party code has 163,202, including test and editor scripts.
2. **Clone detection** (`dup.py`). Comparison is only within one Godot project; the 132 projects of godot-demo-projects are not compared with each other. A window is K consecutive logical lines of one unit (K = 5 by default), together with each line's indentation level relative to the first line of the window; two windows with the same content are a match. Adjacent matches between the same pair of units are merged into a clone region; two windows in the same unit may not overlap. There are three comparison modes:
   - **Exact**: tokens are compared as they are.
   - **Near**: literals, and the unit's local variable and parameter names, are replaced by placeholders.
   - **Loose**: every identifier other than keywords and engine API names is replaced by a placeholder. This is an upper bound with many false positives.

   Duplicated lines count every copy, and each line counts once per category. Copies that overlap are joined into one clone group.
3. **Categories of unit pairs**, taking the first that applies: within one unit; test or editor (either side is a test or extends an `Editor*` class); **same chain** (the two share a script ancestor, as parent and child or as siblings); native base unknown; non-node (either side is not a Node); **same native** (same native base, no common script ancestor); **cross-branch** (different native bases, no common script ancestor). By the engine facts above, same-chain duplication can be moved into the common ancestor and same-native duplication into a new common base, so existing inheritance can remove both.
4. **Name evidence.** For a cross-branch pair, take the nearest common ancestor of the two native bases, for example PhysicsBody2D for StaticBody2D and CharacterBody2D. If either unit uses on self (without a receiver, or through `self.` or `super.`) an engine member that the common ancestor lacks and only its own native branch has, the pair **has name evidence**: a common base can extend only the common ancestor and cannot reach those members. A pair without name evidence could, by the engine rules, share a base script whose native base is the common ancestor. The engine member lists come from the Godot class reference XML (`build_api.py`, 1,112 classes).
5. **Kinds of duplicated code** are assigned by where each line is: exported members, `@onready` members, other member declarations and property accessor bodies, signal declarations, lifecycle callbacks (`_ready`, `_enter_tree`, `_init`, `_exit_tree`, `_notification`, `_static_init`), per-frame callbacks (`_process`, `_physics_process`, `_integrate_forces`), input callbacks (`_input`, `_unhandled_input`, `_gui_input` and so on), other engine virtual methods, signal handlers (`_on_*` or `on_*`), and ordinary methods.
6. **Mechanism labels (judged by reading).** For each of the 88 exact cross-branch clone groups at K = 5, a representative copy was read and given one of five labels by the mechanism that would remove it:
   - (a) **interface default methods**: methods that only read members each class declares separately (or engine API on self), and neither declare nor write shared state;
   - (b) **stateful compile-time mixin**: the copy itself declares fields, `@export` members or signals, or writes fields whose declarations are duplicated too;
   - (c) **composition**: a self-contained sub-feature with its own state and nodes that could be a child node or child scene;
   - (d) **static helper**: a computation on parameters, globals or autoloads that needs no state of self;
   - (e) **not removable**: trivial boilerplate, or boilerplate tied to the scene structure.

   55 groups were labelled one by one. The other 33 phantom-camera groups were labelled (b) by one rule: all of them belong to the two pairs of twin scripts PhantomCamera2D/3D and NoiseEmitter2D/3D, and refer to members that are also declared twice. The labels are an AI agent's judgement and are recorded in `labels.csv`. `dup.py` also makes an automatic pre-label, which differs a lot from the labels assigned by reading and is mentioned below only for comparison. Mechanisms (c) and (d) already exist in GDScript (child nodes and `static func`).
7. **Shared capability.** In each project, for each cross-branch pair of Node units (excluding test and editor scripts), we found the public names both declare: a `func`, a non-`@onready` `var` or a `signal` whose name does not start with an underscore and is not an engine member. Each access to such a name in the project through a receiver other than self (`x.name`, or the name as a literal passed to `call`, `has_method`, `get` and so on) is a consumer site, classified by how the receiver is written: a variable annotated with a script class or an engine class, a variable with an inferred type, a class or autoload name, an untyped receiver, and several explicit cross-type forms: a call after `is_in_group`, access through `owner` or `get_parent()`, `is` checks against at least two script classes in the same function together with `or` (typically `is A or is B`), and a `has_method` guard. The same name does not mean the same capability, so these numbers are upper bounds.
8. **Workaround indicators**, counted in first-party non-test code: `has_method`, `is_in_group`, string calls (`call`, `call_deferred` or `callv` whose first argument is a literal), `is` checks against two or more script classes joined by `or` on one line, access to non-engine members through `owner` or `get_parent()`, and `static func`.
9. **Composition** (`components.py`). Every `.tscn` is composed, including inherited scenes, instances and the changes made on instances, to find the nodes each first-party Node script is attached to and the native type of each such node's parent; attachments inherited from a base scene are not counted. A script whose parent types include at least two that are not ancestors of each other is a component already shared across branches.
10. **Checks and sensitivity.** Ring's Brick and Enemy serve to check how much duplication the detection finds (`ring_check.py`); the whole detection was rerun with K = 3 and K = 8.

## Results

### Overall

Exact matching, K = 5. In parentheses: duplicated lines per thousand logical lines. The unit-pair rows give "unit pairs with a clone / all unit pairs".

| | Logical lines | Cross-branch | Same native | Same chain |
|---|---:|---:|---:|---:|
| Games: duplicated lines | 57,621 | 353 (6.1) | 1,725 (29.9) | 2,884 (50.1) |
| Games: unit pairs | | 21 / 36,232 | 115 / 8,672 | 403 / 7,993 |
| Demos: duplicated lines | 16,776 | 106 (6.3) | 199 (11.9) | 0 |
| Demos: unit pairs | | 3 / 1,063 | 7 / 190 | 0 / 57 |
| Plugins and tools: duplicated lines | 88,805 | 1,291 (14.5) | 1,575 (17.7) | 2,449 (27.6) |
| Plugins and tools: unit pairs | | 31 / 20,290 | 34 / 2,356 | 244 / 1,973 |

- **In games, duplication that inheritance could remove but did not is much larger than cross-branch duplication**: same native is 4.9 times cross-branch, same chain 8.2 times. Per unit pair the gap is wider: cross-branch pairs are the most numerous and the least likely to share a clone (0.58 per thousand pairs, against 13.3 and 50.4 for the other two).
- **In the game group the other two categories come mainly from tanks-of-freedom-ii** (1,280 and 1,697 lines). Without it, the three categories are 289, 445 and 1,187 lines.
- **By the engine rules, part of the cross-branch duplication could already be shared through inheritance.** Duplication in pairs with name evidence is 150 lines in games, 94 in demos and 891 in plugins and tools; in pairs without name evidence it is 219, 12 and 400 lines. For games the two add up to more than 353 because 16 lines belong to both kinds of pairs.
- **Near matching gives similar proportions**: 694, 2,315 and 4,417 lines for the three game categories. Loose matching gives upper bounds: 1,677, 4,447 and 5,377 lines.

### Per repository

Exact matching, K = 5. The "Near" column is cross-branch duplication under near matching.

| Repository | Commit | Logical lines | Cross-branch | Per thousand | With name evidence | Near | Same native | Same chain |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| [gdquest-demos/godot-open-rpg](https://github.com/gdquest-demos/godot-open-rpg) | `d59f2f7` | 2,190 | 0 | 0.0 | 0 | 0 | 12 | 68 |
| [KenneyNL/Starter-Kit-3D-Platformer](https://github.com/KenneyNL/Starter-Kit-3D-Platformer) | `3fa8a04` | 178 | 0 | 0.0 | 0 | 0 | 0 | 0 |
| [KenneyNL/Starter-Kit-City-Builder](https://github.com/KenneyNL/Starter-Kit-City-Builder) | `4535092` | 159 | 0 | 0.0 | 0 | 0 | 0 | 0 |
| [KenneyNL/Starter-Kit-FPS](https://github.com/KenneyNL/Starter-Kit-FPS) | `185fd23` | 234 | 0 | 0.0 | 0 | 0 | 0 | 0 |
| [KenneyNL/Starter-Kit-Racing](https://github.com/KenneyNL/Starter-Kit-Racing) | `2f2e5f2` | 123 | 0 | 0.0 | 0 | 0 | 0 | 0 |
| [godotengine/tps-demo](https://github.com/godotengine/tps-demo) | `a82f154` | 1,046 | 0 | 0.0 | 0 | 0 | 18 | 0 |
| [lampe-games/godot-open-rts](https://github.com/lampe-games/godot-open-rts) | `a628ad3` | 3,716 | 14 | 3.8 | 14 | 14 | 118 | 24 |
| [P1X-in/tanks-of-freedom-ii](https://github.com/P1X-in/tanks-of-freedom-ii) | `dbe629e` | 15,359 | 64 | 4.2 | 24 | 84 | 1,280 | 1,697 |
| [guladam/deck_builder_tutorial](https://github.com/guladam/deck_builder_tutorial) | `91b408b` | 2,679 | 26 | 9.7 | 0 | 26 | 12 | 160 |
| [SlayHorizon/godot-tiny-mmo](https://github.com/SlayHorizon/godot-tiny-mmo) | `f4ed04f` | 28,891 | 177 | 6.1 | 40 | 492 | 261 | 911 |
| [teeeece/godot_open_star_fighter](https://github.com/teeeece/godot_open_star_fighter) | `6e7be5c` | 1,242 | 0 | 0.0 | 0 | 0 | 14 | 12 |
| [YYF233333/Ring](https://github.com/YYF233333/Ring) | `0ec4161` | 1,804 | 72 | 39.9 | 72 | 78 | 10 | 12 |
| **Games total** | | **57,621** | **353** | **6.1** | **150** | **694** | **1,725** | **2,884** |
| [godotengine/godot-demo-projects](https://github.com/godotengine/godot-demo-projects) | `b761b4c` | 16,776 | 106 | 6.3 | 94 | 106 | 199 | 0 |
| [bitbrain/beehave](https://github.com/bitbrain/beehave) | `fe58915` | 5,169 | 24 | 4.6 | 24 | 24 | 348 | 419 |
| [MikeSchulze/gdUnit4](https://github.com/MikeSchulze/gdUnit4) | `9bc1a45` | 26,384 | 0 | 0.0 | 0 | 0 | 0 | 143 |
| [nathanhoad/godot_dialogue_manager](https://github.com/nathanhoad/godot_dialogue_manager) | `7a72dcf` | 7,377 | 58 | 7.9 | 0 | 68 | 10 | 0 |
| [Maaack/Godot-Game-Template](https://github.com/Maaack/Godot-Game-Template) | `6849d6c` | 5,878 | 156 | 26.5 | 156 | 156 | 836 | 634 |
| [derkork/godot-statecharts](https://github.com/derkork/godot-statecharts) | `76d226a` | 3,377 | 0 | 0.0 | 0 | 0 | 0 | 36 |
| [ramokz/phantom-camera](https://github.com/ramokz/phantom-camera) | `21d3762` | 5,394 | 792 | 146.8 | 548 | 808 | 0 | 10 |
| [Orama-Interactive/Pixelorama](https://github.com/Orama-Interactive/Pixelorama) | `38a8b03` | 35,226 | 261 | 7.4 | 163 | 259 | 381 | 1,207 |
| **Plugins and tools total** | | **88,805** | **1,291** | **14.5** | **891** | **1,315** | **1,575** | **2,449** |

- **7 of the 12 game repositories have no cross-branch clone**; they are all small, 5,172 lines together. Of the 5 that have one, 4 lie between 3.8 and 9.7 lines per thousand; Ring has 39.9.
- **Among games, only Ring has more cross-branch duplication than either other category** (72 lines, against 10 and 12), all of it between Brick (StaticBody2D) and Enemy (CharacterBody2D). The pair's only name evidence is Enemy's use of CharacterBody2D's `velocity` (`breakout/scenes/enemies/base/enemy.gd:98, 186, 196`). Among plugins, phantom-camera and godot_dialogue_manager also have more cross-branch duplication than either other category.
- **phantom-camera has 146.8 lines per thousand.** PhantomCamera2D and PhantomCamera3D, and NoiseEmitter2D and NoiseEmitter3D, are two pairs of 2D/3D twin scripts; all 35 clone groups are between Node2D and Node3D, and the duplicated lines are 14.7% of the repository's logical lines.
- **godot-tiny-mmo has far more near matches (492 lines) than exact ones (177).** Most of the difference is one 303-line group: 38 fragments in 26 client scripts that build containers and TextureRects in code and set theme constants (`groups_near.csv`).
- Among the 23 exact cross-branch clone groups in games, the most frequent native pairs are Control and VBoxContainer (7 groups, all in godot-tiny-mmo) and CharacterBody2D and StaticBody2D (4 groups, all in Ring). 211 of the 353 lines are in groups whose two native classes are ancestor and descendant (for example Control and VBoxContainer), and 142 lines are between siblings. In plugins and tools, 1,183 of the 1,291 lines are between siblings, mostly Node2D and Node3D.
- Of the 132 projects in godot-demo-projects, 13 have an exact clone and 3 have a cross-branch clone: `2d/physics_tests` (two controllers, for CharacterBody2D and RigidBody2D), `misc/hdr_output` and `xr/openxr_hand_tracking_demo`.

### What is duplicated

Cross-branch duplicated lines, exact matching, K = 5, by position:

| Position | Games | Demos | Plugins and tools |
|---|---:|---:|---:|
| Ordinary methods | 277 | 0 | 817 |
| Signal handlers | 20 | 26 | 31 |
| Lifecycle callbacks | 14 | 8 | 36 |
| Per-frame callbacks | 12 | 56 | 56 |
| Input callbacks | 8 | 0 | 0 |
| Other engine virtual methods | 2 | 0 | 51 |
| Exported members | 12 | 4 | 42 |
| Other member declarations and property accessor bodies | 8 | 12 | 252 |
| Signal declarations | 0 | 0 | 4 |
| Other class-level statements | 0 | 0 | 2 |
| **Total** | **353** | **106** | **1,291** |
| Of which lines containing `.connect` | 10 | 2 | 4 |

- **In games, 78% of cross-branch duplication is in ordinary methods**; member declarations (exported and other members) are only 20 lines, 10 of them Ring's exported members. Lifecycle, per-frame and input callbacks add up to 34 lines.
- **In plugins and tools, member declarations and property accessor bodies take 252 lines**, mostly exported properties with setters and getters in phantom-camera's twin scripts.
- The 56 per-frame lines in demos come from the two physics_tests controllers.

### Which mechanism would remove it (judged by reading)

All exact cross-branch clone groups at K = 5, "groups / lines":

| Label | Games | Demos | Plugins and tools |
|---|---:|---:|---:|
| (a) interface default methods | 4 / 54 | 0 | 2 / 30 |
| (b) stateful compile-time mixin | 7 / 102 | 3 / 70 | 41 / 948 |
| (c) composition | 5 / 81 | 0 | 0 |
| (d) static helper | 6 / 104 | 2 / 36 | 17 / 313 |
| (e) not removable | 1 / 12 | 0 | 0 |
| **Total** | **23 / 353** | **5 / 106** | **60 / 1,291** |

- **Of the 353 game lines, (c) and (d) account for 185**, so more than half could be removed with mechanisms GDScript already has. All 81 lines of (c) are in godot-tiny-mmo; the 104 lines of (d) come from godot-tiny-mmo (64) and tanks-of-freedom-ii (40).
- **(a) has 54 lines**, in deck_builder_tutorial (16), godot-open-rts (14), godot-tiny-mmo (12) and tanks-of-freedom-ii (12).
- **(b) has 102 lines, 72 of them Ring's Brick and Enemy**; the rest are in godot-tiny-mmo (20) and deck_builder_tutorial (10).
- **(b) largely coincides with name evidence.** A group counts as having name evidence if any of its unit pairs does. In games, 92 of the 102 lines of (b) are in such groups; for (a) it is 14 of 54, for (c) 20 of 81, for (d) 40 of 104.
- **Of the 948 lines of (b) in plugins and tools, phantom-camera has 734**; the rest are Godot-Game-Template's input_actions_list and input_actions_tree (Container and Tree, 156 lines) and godot_dialogue_manager's DialogueActionable2D and 3D (Area2D and Area3D, 58 lines). Of the 313 lines of (d), 231 are in Pixelorama, mostly fragments for undo and redo or for animation tag ranges, spread over various dialogs and tools.
- The 70 lines of (b) in demos are the two physics_tests controllers (56 lines: 6 identical fields plus per-frame input and velocity logic) and hdr_output (14 lines).
- The automatic pre-label marks 16 game groups (273 lines) as stateful, against 7 groups (102 lines) by reading: the automatic rule calls a fragment stateful whenever it refers to a member.

### Shared capability and cross-type calls

| | Games | Demos | Plugins and tools |
|---|---:|---:|---:|
| Node units | 752 | 411 | 446 |
| Cross-branch unit pairs declaring a public member of the same name | 681 | 21 | 312 |
| Of which at least one such name is accessed through a receiver other than self | 568 | 19 | 232 |
| Of which at least two such names | 59 | 1 | 40 |
| Shared names (distinct per project) | 181 | 12 | 234 |
| Of which accessed | 143 | 9 | 118 |
| Consumer sites | 2,081 | 21 | 1,387 |

The 2,081 consumer sites in games, by how the receiver is written:

| Form | Consumer sites |
|---|---:|
| Untyped receiver | 1,126 |
| Variable annotated with a script class | 488 |
| Class or autoload name | 180 |
| Variable annotated with an engine class or other type | 118 |
| Variable with an inferred type | 77 |
| Name as a literal passed to `get` or `call_deferred` | 22 |
| Other `is` checks against script classes in the same function | 14 |
| Node path | 1 |
| Explicit cross-type: call after `is_in_group` | 18 |
| Explicit cross-type: through `owner` or `get_parent()` | 13 |
| Explicit cross-type: `is` checks against two or more script classes in the same function, with `or` | 11 |
| Explicit cross-type: `has_method` guard, and the name as a literal passed to `has_method` | 7 + 6 |

- **Shared names are mostly coincidences.** The most accessed shared names are tanks-of-freedom-ii's `model` (146 consumer sites), `map` (137), `side` (101) and `clear` (90), and godot-tiny-mmo's `current` (126). The table above is therefore an upper bound and does not show that these units really share a capability.
- **Explicit cross-type forms are rare**: 55 sites in games, 2.6%. All 18 calls after `is_in_group` are in godot-open-rts; all 13 accesses through `owner` are in Ring's components; the 11 sites with `is` checks against several script classes are in godot-tiny-mmo (7) and deck_builder_tutorial (4); the 13 `has_method` sites are in Starter-Kit-FPS, godot-tiny-mmo and godot_open_star_fighter.
- **More than half of the consumer sites (1,126, 54%) have an untyped receiver**, so the syntax cannot tell whether the call is cross-type.
- Among the explicit forms in plugins and tools, `is` checks against several script classes occur 35 times, 33 of them in Pixelorama; `has_method` occurs 24 times.

### Existing workarounds

First-party non-test code:

| | Games | Demos | Plugins and tools |
|---|---:|---:|---:|
| `has_method` | 18 | 18 | 45 |
| `is` checks against two or more script classes joined by `or` on one line | 4 | 0 | 8 |
| `is_in_group` | 45 | 2 | 18 |
| Non-engine member access through `owner` or `get_parent()` (units) | 20 (10) | 7 (5) | 3 (3) |
| String calls | 41 | 4 | 65 |
| `static func` | 491 | 23 | 909 |

Scripts already used as components under different native types:

| | Games | Demos | Plugins and tools |
|---|---:|---:|---:|
| First-party Node scripts attached to child nodes | 444 | 268 | 216 |
| Whose parent nodes have two or more native types | 17 | 6 | 26 |
| Whose parent types are in different branches | 11 | 4 | 18 |
| Of which the script itself extends Node | 4 | 1 | 6 |

- `static func` in games is concentrated in two repositories: 446 in godot-tiny-mmo and 35 in godot-open-rts; the other game repositories have 10 together. Of the 45 `is_in_group` calls, 35 are in godot-open-rts.
- The 4 game components that extend Node and sit under parents from different branches are Ring's HealthComponent and IntervalHurtComponent (parents: StaticBody2D 296 times, CharacterBody2D 5 times), deck_builder_tutorial's ModifierHandler (Area2D, Node2D, Control) and godot-tiny-mmo's StateSynchronizer (CharacterBody2D 131 times, Area2D once).
- Ring uses both composition and copying: the health logic is in components, which read 6 fields of their host through `owner` (example 2 in the [paper-migration survey](paper-migration.en.md#examples)), while the host-side fields, signal wiring and display code are written once in Brick and again in Enemy.

### The Ring check

- `breakout/scenes/bricks/base/brick.gd` has 77 code lines (physical lines that are neither blank nor comments); `enemy.gd` has 190. Of brick.gd's 77 lines, 67 have an identical line somewhere in enemy.gd, and 70 after replacing enemy with brick. The paper-migration survey counted 64 by hand.
- brick.gd has 67 logical lines. Exact detection at K = 5 finds 4 regions that cover 36 of them; near detection covers 39. At K = 3 the figures are 45 and 48, at K = 8 20 and 29.
- What is missed are short runs broken by a single differing line. For example, the two `@onready` blocks differ only in how `hit_flash_animation_player` is written (brick.gd:32, enemy.gd:36); Brick's `physic_hurt` has an extra `if hit_flash:` (brick.gd:100); in the `Info` and `Basic` export groups the field names carry a brick or enemy prefix and the order differs.
- The detection is therefore conservative: on this pair of files, exact detection at K = 5 finds only about half of what a line-by-line comparison shows.

### Window size

Duplicated lines under exact matching; per thousand lines in parentheses:

| K | Games: cross-branch | Same native | Same chain | Plugins and tools: cross-branch | Same native | Same chain |
|---|---:|---:|---:|---:|---:|---:|
| 3 | 714 (12.4) | 2,571 | 4,205 | 1,960 | 2,005 | 3,592 |
| 5 | 353 (6.1) | 1,725 | 2,884 | 1,291 | 1,575 | 2,449 |
| 8 | 148 (2.6) | 1,016 | 1,770 | 890 | 1,349 | 1,667 |

- A smaller window finds more duplication of every kind, and cross-branch grows fastest: at K = 3 it is 2.0 times its K = 5 value, the other two 1.5 times. At all three window sizes, however, cross-branch duplication in games stays below the other two categories: same native is 3.6, 4.9 and 6.9 times as large, same chain 5.9, 8.2 and 12.0 times.
- For the other 4 game repositories with a cross-branch clone at K = 5 (godot-open-rts, tanks-of-freedom-ii, deck_builder_tutorial, godot-tiny-mmo), cross-branch duplication per thousand lines is 3.8 to 22.4 at K = 3, 3.8 to 9.7 at K = 5 and 0 to 6.0 at K = 8. Ring has 66.5, 39.9 and 22.2, phantom-camera 170.2, 146.8 and 111.6. At K = 3, godot-open-rpg also shows 6 lines.
- The shared-capability and workaround counts do not depend on K.

## Examples

**1. Ring: two hosts copy fields and methods, labelled (b).**
`breakout/scenes/bricks/base/brick.gd:17-22, 64-75`; the same as `breakout/scenes/enemies/base/enemy.gd:22-27, 206-216` except for blank lines

```
@export var point: int
@export var drop_percent_scale: float = 1.0 #1.0表示正常掉率
@export var hit_by_ball_player_charge: int = 1 #被球击中时玩家获得多少充能

@export var lucky_drop: bool = false #会进行lucky drop
@export var unique_drop_name: String
...
func set_type(new_type : int):
	type = new_type
	if type < 0:
		return
	$Sprite2D.texture = sprites[type]


func show_health():
	health_bar.max_value = max_health
	health_bar.value = health_component.current_health

	health_label.text = str(health_component.current_health)
```

Brick extends StaticBody2D and Enemy extends CharacterBody2D, so a common base can extend at most PhysicsBody2D, while Enemy uses CharacterBody2D's `velocity`. The copied part includes both exported fields and methods that read and write them. The health and death-drop logic is already in HealthComponent and DeathComponent, which read 6 fields of their host through `owner`, among them `point`, `drop_percent_scale`, `lucky_drop` and `unique_drop_name` above.

**2. phantom-camera: 2D/3D twin scripts, labelled (b).**
`addons/phantom_camera/scripts/phantom_camera/phantom_camera_2d.gd:407-412`; identical to `phantom_camera_3d.gd:488-493`

```
@export_range(0.0, 1.0) var dead_zone_width: float = 0.0:
	set(value):
		dead_zone_width = value
		dead_zone_changed.emit()
	get:
		return dead_zone_width
```

`phantom_camera_2d.gd:269`; `phantom_camera_3d.gd:361`

```
@export var follow_offset: Vector2 = Vector2.ZERO:
@export var follow_offset: Vector3 = Vector3.ZERO:
```

PhantomCamera2D extends Node2D and PhantomCamera3D extends Node3D; their common ancestor is Node. The exact clones cover only the parts that do not depend on the dimension. Members such as `follow_offset` differ only in the vector type, and neither exact nor near matching counts them as clones, because near matching does not replace type names; under loose matching the repository's cross-branch duplication is 1,178 lines. Inference: sharing these parts would require parameterizing over the vector type; a mixin alone would not do it.

**3. deck_builder_tutorial: the same method reading members each class declares, labelled (a); the caller tests class by class.**
`scenes/enemy/enemy.gd:93-103`; identical to `scenes/player/player.gd:41-51`

```
func take_damage(damage: int, which_modifier: Modifier.Type) -> void:
	if stats.health <= 0:
		return

	sprite_2d.material = WHITE_SPRITE_MATERIAL
	var modified_damage := modifier_handler.get_modified_value(damage, which_modifier)

	var tween := create_tween()
	tween.tween_callback(Shaker.shake.bind(self, 16, 0.15))
	tween.tween_callback(stats.take_damage.bind(modified_damage))
	tween.tween_interval(0.17)
```

`effects/damage_effect.gd:8-14`

```
func execute(targets: Array[Node]) -> void:
	for target in targets:
		if not target:
			continue
		if target is Enemy or target is Player:
			target.take_damage(amount, receiver_modifier_type)
			SFXPlayer.play(sound)
```

Enemy extends Area2D and Player extends Node2D. `stats`, `sprite_2d` and `modifier_handler` are declared separately in each class, and `stats` has a different type in each (EnemyStats and CharacterStats, enemy.gd:7, player.gd:6). The `tween.finished` callback that follows also differs: the enemy emits `enemy_died`, the player `player_died` (enemy.gd:105-112, player.gd:53-60). The caller tests `is Enemy or is Player` and then calls `take_damage` untyped.

**4. godot-tiny-mmo: two hosts each have a countdown, labelled (c).**
`source/client/ui/hud/trade_panel.gd:523-537`; the same as `source/common/gameplay/trade/trade_table.gd:309-323` except for the message text

```
func _run_countdown(seconds: int) -> void:
	_stop_countdown()
	_countdown_tween = create_tween()
	_countdown_tween.tween_method(_set_countdown_text, float(seconds), 0.0, float(seconds))


func _set_countdown_text(value: float) -> void:
	_countdown_label.text = "Trade completes in %d…" % maxi(1, ceili(value))


func _stop_countdown() -> void:
	if _countdown_tween != null and _countdown_tween.is_valid():
		_countdown_tween.kill()
	_countdown_tween = null
	_countdown_label.text = ""
```

The trade panel's native base is Control; the trade table in the world extends Area2D. The countdown has its own state (`_countdown_tween`, `_countdown_label`); the table's copy shows `"Trading in %d…"` (trade_table.gd:316).

**5. tanks-of-freedom-ii: five copies of one computation, labelled (d).**
`scenes/ui/menu/skirmish/skirmish.gd:78-92`; the clone regions of the other 4 copies are at `scripts/services/relay.gd:236-244`, `scripts/services/multiplayer.gd:134-142`, `scenes/ui/multiplayer/lobby.gd:172-180` and `scenes/ui/online/lobby.gd:176-184`

```
func _gather_player_sides(map_data):
	var sides = {}
	var side
	var key

	for y in range(self.map_list_service.MAX_MAP_SIZE):
		for x in range(self.map_list_service.MAX_MAP_SIZE):
			key = str(x) + "_" + str(y)
			if map_data["tiles"].has(key):
				side = self._lookup_side(map_data["tiles"][key])

				if side != null:
					sides[side] = side

	return sides
```

The 5 copies are in 2 services (Node) and 3 menus (Control), and depend only on the parameter `map_data`, a constant of the autoload `MapManager` (skirmish.gd:3) and the same file's `_lookup_side`. The copy in relay.gd writes `side != ""` where the others write `side != null` (relay.gd:246).

## Conclusions

- **Cross-branch duplication exists, but it is small and concentrated.** Games have 6.1 lines per thousand; of the 5 game repositories with any, 4 lie between 3.8 and 9.7. Ring has 39.9 lines and the 2D/3D twin plugin phantom-camera 146.8.
- **Duplication that inheritance could remove but did not is much larger.** In games, same-native and same-chain duplication are 4.9 and 8.2 times the cross-branch amount, and the order holds at all three window sizes; these two categories, however, come mainly from tanks-of-freedom-ii.
- **Cross-branch duplication is mostly ordinary methods**; field declarations are few. The repositories that copy fields and exported properties are mainly Ring and phantom-camera.
- **Judged by reading, more than half of the cross-branch duplication in games (185 of 353 lines) could be removed by composition or static helpers**, which GDScript already has. Interface default methods would cover 54 lines; a stateful mixin 102 lines, 72 of them from Ring. In plugins and tools a stateful mixin would cover 948 lines, 734 of them from phantom-camera, and phantom-camera's parts that differ only in the vector type are not in these numbers.
- **Explicit cross-type calls are rare.** Only 55 of the 2,081 consumer sites in games use them, and more than half of the consumer sites have an untyped receiver.
- **Composition is already used to share code across branches**: 11 such components in games. Ring uses both composition and copying.

The rules based on these data are in §3.9 and §4.1 of the [language specification](../language-spec.en.md): a common base class with state is written with `@abstract`, a capability shared by classes with different engine bases is an interface, and the first version has no stateful mixins. Interface default methods in the specification may read and write state through interface properties, so their scope is wider than label (a) here.

## Limitations

- **The detection is conservative.** The Ring check shows that exact detection at K = 5 finds only about half of what a line-by-line comparison shows. Duplicates broken by a single differing line, or shorter than the window, are missed.
- **Near matching does not replace member or type names.** Renamed fields, and twin code that differs only in the vector type, are neither exact nor near clones. Loose matching is an upper bound with many false positives.
- **Name evidence is judged per unit and by name.** A unit with name evidence may use those members only outside the duplicated fragment; accesses through other receivers, `call` or `get` are not seen. The absence of name evidence only means that the engine does not prevent a shared base: if one side already has a script base, the shared base must be inserted into that chain, and other code may refer to these nodes by their concrete types; none of these costs were checked.
- **The mechanism labels are an AI agent's judgement.** The lines between (a) and (b), and between (b) and (c), are not sharp; 33 phantom-camera groups were labelled by one rule; only the exact clone groups at K = 5 were labelled, so duplication that appears only under near matching (such as godot-tiny-mmo's UI fragments) has no label.
- **Shared capability is judged by name**, is an upper bound, and the names are mostly generic.
- **The corpus is skewed.** 77% of the game code comes from godot-tiny-mmo and tanks-of-freedom-ii, and the Kenney starter kits are all small; the demos are 132 small projects; in plugins and tools, Pixelorama and gdUnit4 account for about 70% of the code.
- **Only text scenes and GDScript are covered.** Components attached in code with `add_child` or `set_script` are not seen; binary `.scn` scenes and C# code are not included.

## Reproducing

The scripts are in [`cross-branch-duplication/`](cross-branch-duplication/) and need only Python 3 (standard library; run with 3.11) and git. `gdcorpus.py` lists the corpus and the first-party rules and can fetch the corpus; `build_api.py` extracts engine class members and inheritance from the Godot class reference XML; `dup.py` does the clone detection and the shared-capability and workaround counts; `components.py` counts components; `make_labels.py` holds the labels assigned by reading and matches them to the detected groups to produce `labels.csv`; `report.py` summarizes everything into `report.txt`, which contains every statistic in this survey or the numbers it is computed from; `ring_check.py` does the Ring check. godot-tiny-mmo's UI fragment group is in the regenerated `groups_near.csv`.

`api.json` is about 770 KB and is not included; `build_api.py` generates it from a Godot checkout at commit `469659e`. The included results are in `out/` (K = 5: `report.txt`, `summary.json`, `workarounds.json`, `components.csv`, `components_summary.json`, `ring_check.txt`), `out_k3/report.txt` and `out_k8/report.txt`. The large files, such as the per-region and per-group CSVs and the code snippets, are regenerated by the scripts.

```
python docs/surveys/cross-branch-duplication/gdcorpus.py corpus     # 20 shallow clones at fixed commits, checking out only .gd, .tscn and other text files
python docs/surveys/cross-branch-duplication/build_api.py ../godot work/api.json   # Godot checked out at 469659e
python docs/surveys/cross-branch-duplication/dup.py --corpus corpus --api work/api.json --out work/out
python docs/surveys/cross-branch-duplication/components.py --corpus corpus --api work/api.json --out work/out
python docs/surveys/cross-branch-duplication/make_labels.py work/out --labels work/labels.csv   # should equal the included labels.csv
python docs/surveys/cross-branch-duplication/report.py work/out    # writes work/out/report.txt
python docs/surveys/cross-branch-duplication/dup.py --corpus corpus --api work/api.json --out work/out_k3 --k 3
python docs/surveys/cross-branch-duplication/report.py work/out_k3
python docs/surveys/cross-branch-duplication/dup.py --corpus corpus --api work/api.json --out work/out_k8 --k 8
python docs/surveys/cross-branch-duplication/report.py work/out_k8
python docs/surveys/cross-branch-duplication/ring_check.py --corpus corpus --api work/api.json work/out work/out_k3 work/out_k8
```
