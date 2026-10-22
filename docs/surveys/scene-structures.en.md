# Survey: How many scene structures does a script see?

**2026-10-23** · Related: [design decisions](../decisions.en.md), "Scenes and nodes" · 中文：[scene-structures.zh.md](scene-structures.zh.md)

The static type of `$X`, `%X` and `get_node("…")` can come from the scenes that attach the script (the scene is the source of truth) or from declarations in the script. Taking it from the scenes is clean only if a script sees one scene structure, or if every structure it sees gives the same type for each path it uses. This survey tests that premise. In 20 repositories we composed the full node tree of every text scene by the engine's rules, counted how many structures each script sees, and resolved each of its literal paths in each structure.

## The question

GDScript compiles `$A/B` and `%A` into a call to `self.get_node` with a constant NodePath, and the analyzer types the result as `Node`. Code completion works differently: it finds the node in the currently edited scene that carries the script, calls `get_node_or_null` on it, and completes from the node it finds. So GDScript ignores scenes when compiling, and completion looks at only the one scene that is open (see "Engine evidence" below).

There are two candidate approaches:

| Approach | Static type of a literal path comes from | Role of the scenes |
|---|---|---|
| A: scene as source of truth | Every scene that attaches the script or a subclass: resolve the path in each and take the type of the node found | Source of the types |
| B: script declarations | Declarations in the script of the nodes it needs (path and type) | Checked against the declarations |

If a script sees one structure, A can give the type directly. If it sees several, the same expression may point to nodes of different types in different scenes, or to no node in some of them, and A needs further rules. B does not depend on the structure being unique, but it requires writing down again in the script what the scenes already contain. We wanted to know:

- How many scripts see two or more structures, and how many of those use literal paths?
- Do literal paths point to nodes of different types in different structures? Are they missing from some structures?
- Where do multiple structures come from, and what are these scripts for?
- How many node accesses fall outside the attaching scene's subtree (upward access, absolute paths, nodes added at runtime), so that neither approach directly covers them?

## Corpus

| Repository | Group | Commit | First-party `.gd` | Lines | `.tscn` | Inherited scenes | Scripts with literal paths | See ≥2 structures | Of those, with literal paths |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| [godotengine/godot-demo-projects](https://github.com/godotengine/godot-demo-projects) | demos | `b761b4c` | 461 | 30,819 | 396 | 8 | 235 | 29 | 11 |
| [gdquest-demos/godot-open-rpg](https://github.com/gdquest-demos/godot-open-rpg) | game | `d59f2f7` | 78 | 5,422 | 128 | 22 | 34 | 14 | 9 |
| [KenneyNL/Starter-Kit-3D-Platformer](https://github.com/KenneyNL/Starter-Kit-3D-Platformer) | game | `3fa8a04` | 9 | 378 | 10 | 6 | 6 | 0 | 0 |
| [KenneyNL/Starter-Kit-City-Builder](https://github.com/KenneyNL/Starter-Kit-City-Builder) | game | `4535092` | 6 | 311 | 1 | 0 | 1 | 0 | 0 |
| [KenneyNL/Starter-Kit-FPS](https://github.com/KenneyNL/Starter-Kit-FPS) | game | `185fd23` | 7 | 451 | 10 | 6 | 3 | 0 | 0 |
| [KenneyNL/Starter-Kit-Racing](https://github.com/KenneyNL/Starter-Kit-Racing) | game | `2f2e5f2` | 3 | 258 | 4 | 0 | 3 | 1 | 1 |
| [godotengine/tps-demo](https://github.com/godotengine/tps-demo) | game | `a82f154` | 15 | 1,619 | 17 | 1 | 10 | 1 | 1 |
| [Orama-Interactive/Pixelorama](https://github.com/Orama-Interactive/Pixelorama) | tool | `38a8b03` | 210 | 54,782 | 120 | 45 | 102 | 10 | 6 |
| [ramokz/phantom-camera](https://github.com/ramokz/phantom-camera) | plugin | `21d3762` | 36 | 10,410 | 32 | 0 | 13 | 7 | 1 |
| [nathanhoad/godot_dialogue_manager](https://github.com/nathanhoad/godot_dialogue_manager) | plugin | `7a72dcf` | 79 | 14,619 | 24 | 0 | 16 | 1 | 1 |
| [bitbrain/beehave](https://github.com/bitbrain/beehave) | plugin | `fe58915` | 95 | 8,566 | 27 | 0 | 7 | 13 | 0 |
| [derkork/godot-statecharts](https://github.com/derkork/godot-statecharts) | plugin | `76d226a` | 89 | 6,852 | 43 | 0 | 17 | 6 | 0 |
| [MikeSchulze/gdUnit4](https://github.com/MikeSchulze/gdUnit4) | plugin | `9bc1a45` | 482 | 54,133 | 29 | 0 | 22 | 1 | 0 |
| [Maaack/Godot-Game-Template](https://github.com/Maaack/Godot-Game-Template) | plugin | `6849d6c` | 125 | 8,553 | 119 | 56 | 53 | 24 | 12 |
| [lampe-games/godot-open-rts](https://github.com/lampe-games/godot-open-rts) | game | `a628ad3` | 118 | 7,415 | 84 | 9 | 2 | 9 | 1 |
| [P1X-in/tanks-of-freedom-ii](https://github.com/P1X-in/tanks-of-freedom-ii) | game | `dbe629e` | 292 | 24,361 | 661 | 449 | 131 | 15 | 15 |
| [guladam/deck_builder_tutorial](https://github.com/guladam/deck_builder_tutorial) | game | `91b408b` | 122 | 4,830 | 61 | 0 | 43 | 7 | 1 |
| [SlayHorizon/godot-tiny-mmo](https://github.com/SlayHorizon/godot-tiny-mmo) | game | `f4ed04f` | 467 | 54,379 | 123 | 14 | 38 | 20 | 1 |
| [teeeece/godot_open_star_fighter](https://github.com/teeeece/godot_open_star_fighter) | game | `6e7be5c` | 40 | 2,408 | 26 | 3 | 18 | 5 | 3 |
| [YYF233333/Ring](https://github.com/YYF233333/Ring) | game | `0ec4161` | 66 | 3,272 | 76 | 0 | 27 | 3 | 2 |
| **Total** | | | **2,800** | **293,838** | **1,991** | **619** | **781** | **166** | **65** |

The first 14 repositories are those of the [property-accessor survey](property-accessors.en.md#corpus), at the same commits. To add game code we chose five more open-source games, at HEAD on 2026-10-23. Ring is at the same commit as in the [paper-migration survey](paper-migration.en.md#material). The rest of this document uses three groups based on the "Group" column: games (12 repositories), demos (1, godot-demo-projects), and plugins and tools (7). "Inherited scenes" includes scenes that inherit from imported models. "See ≥2 structures" counts the contexts of subclass scripts too (see "Method").

**Licenses.** The licenses of the first 14 repositories are given in the property-accessor survey; their code is MIT. The five added games are MIT; the license file of tanks-of-freedom-ii states that MIT covers both its code and its graphical assets. Ring has no license file; its code excerpts are included with the repository owner's consent.

**First-party** means all code outside `addons/`, plus the addon the repository exists to publish (beehave, gdUnit4, dialogue_manager, godot_state_charts, phantom_camera, and tinymmo in godot-tiny-mmo), every `addons/` directory in godot-demo-projects, and every `addons/` directory in Godot-Game-Template (the five plugins share the same plugin.cfg author). The other `addons/` are vendored, for example Dialogue Manager in Ring and godot-sqlite and httpserver in godot-tiny-mmo; their scripts are not counted, but all scene files take part in composition. Unlike the property-accessor survey, first-party code here includes unit tests, so the line counts of gdUnit4, beehave and others differ from that survey. Lines are physical lines of first-party `.gd` files.

**Checkout.** Each repository was sparsely checked out with only `.gd`, `.tscn`, `.tres`, `.scn`, `.uid`, `.cs`, `project.godot`, `plugin.cfg` and the license files at the root. Imported models (`.glb`, `.dae` and so on) were not checked out.

## Method

1. **Compose scene trees.** `analyze.py` parses every `.tscn` and builds the complete node tree that the engine would produce at run time, following the engine's rules for loading text scenes and instantiating them: instanced sub-scenes are expanded recursively; an inherited scene first builds its base scene and then applies its own changes; nodes that an outer scene adds inside an instance, and its overrides of existing nodes, are applied as well. Each node records its native type, script, owner, and whether `unique_name_in_owner` is set. The insides of imported-model instances and instance placeholders are treated as unknown. All 1,991 text scenes parsed, with no entry whose parent could not be found; the 2 binary `.scn` files were not parsed. The native class hierarchy comes from the class reference XML at Godot commit `469659e` (`build_classes.py`, stored in `classes.json`).
2. **Contexts.** Every node in a composed tree that carries a script is one context of that script: attached directly by that scene file, coming from a base scene, or inside an instanced sub-scene. Contexts of subclass scripts (those that `extends` the script, directly or indirectly) also count as contexts of the script, because the base class's code runs on those nodes too.
3. **Structure.** The structure of a context is the relative path of every descendant of the node together with its native type and script, plus the unique names that `%` can reach from the node. Ancestors are not part of the structure. Two contexts with exactly the same content have the same structure; however many times a scene is instanced unchanged, it counts as one.
4. **Literal paths.** A tokenizer (which skips strings and comments correctly) extracts, on self, `$A/B`, `$"A/B"`, `%A`, `$%A`, and `get_node(…)` and `get_node_or_null(…)` with a string literal argument (including `^"…"` and `NodePath("…")`). `get_node` on other objects, non-literal arguments, `has_node`, `find_child`, `get_parent()` and `owner.` are counted but not resolved; paths with `..` and `/root/` paths are also counted separately (including such paths among the literal paths above).
5. **Resolution.** Each literal path is resolved in every context of its script by the engine's rules: `..` goes to the parent; `%Name` looks first in the unique names the current node owns, then in those of its owner; other names look among the children; absolute paths are not resolved. Each use gets one outcome:
   - Same type in all structures: a node is found in every context where the path can be resolved, and there is only one (native type, script) pair.
   - Different types: the nodes found have two or more (native type, script) pairs.
   - Missing from some structures: found in some contexts and not in others.
   - Otherwise: found in no structure; the path enters an imported model or a placeholder and cannot be judged; it leaves the known tree through `..`; it is absolute; or the script has no context at all.
6. **Mechanisms.** For each script that sees ≥2 structures we marked where the extra structures come from; more than one can apply:
   - M1: attached directly by two or more scene files with different structures. M1b: attached in one scene file to several nodes with different subtrees.
   - M2: a scene that attaches it is inherited by other scenes, and the inherited copies have a different structure.
   - M3: a scene that attaches it is instanced elsewhere, and the outer scene changes the inside of the instance.
   - M4: an outer scene sets it on an instanced node.
   - M5: a subclass script is attached in other scenes and brings new structures.

   For attribution, each script is assigned to the first mechanism that matches, in the order inheritance (M2 or M5), independent attachment (M1 or M1b), instance edits (M3).
7. **Purpose.** An AI agent read each of the 75 game scripts that see ≥2 structures, with its scenes, and classified it by purpose; the labels are in `purpose_labels.csv`.
8. **Self-checks.** `test_fixture.py` checks these rules on a small synthetic project: one script attached in two scenes, an inherited scene, nodes added inside an instance, two subclasses attached in different scenes, and unique names. `check_tokens.py` compares the tokenizer's counts with naive regular expressions; every difference we sampled was a `$` inside a string or a comment, which the regex counts wrongly.

All numbers below are computed by `analyze.py`, `report.py` and `summarize.py`. The quoted code lines and scene contents were checked by opening each file.

### Engine evidence

These facts were checked at Godot commit `469659e`; paths are relative to the Godot repository root:

- GDScript compiles a `$` or `%` expression into a call to `Node.get_node` with a constant NodePath (`modules/gdscript/gdscript_compiler.cpp:748-757`), and the analyzer gives it the type `Node` (`modules/gdscript/gdscript_analyzer.cpp:3942-3946`).
- Code completion looks in the currently edited scene for the first node that is owned by the scene root and carries the script (`editor/script/script_text_editor.cpp:1127-1152`), calls `get_node_or_null` on it, and uses the class and script of the node found as the type to complete from (`modules/gdscript/gdscript_editor.cpp:3413-3437`).
- `Node::get_node_or_null` resolves a path segment by segment: an absolute path starts from the tree root (`scene/main/node.cpp:1933-1940`); `..` goes to the parent (`:1949-1954`); `%Name` looks in the current node's own unique-name table and, failing that, in its owner's (`:1960-1968`); other names look among the children (`:1969-1976`). `get_node` reports an error and returns null when nothing is found (`:1984-1995`).
- Unique names are registered in the owner's table; if the owner already has a node with that name, the newcomer is not registered and a warning is printed (`node.cpp:2266-2278`). During instantiation, each node's owner is set and then its unique name is registered (`scene/resources/packed_scene.cpp:640-647`); in a text scene, a node with no `owner` field is owned by the root (`scene/resources/resource_format_text.cpp:262-265`). A sub-scene is instantiated as a whole (`packed_scene.cpp:334-337`), so the nodes inside a sub-scene are owned by the sub-scene's root, and the sub-scene's root is owned by the outer scene's root. Therefore `%Name` from the root of a sub-scene looks first in the sub-scene's own unique names and then in the outer scene's.
- In a text scene, when the first node has `instance` and no `parent`, the file is an inherited scene (`resource_format_text.cpp:233-239`); instantiation first builds the base scene as the root (`packed_scene.cpp:300-309`). A node entry without `type` refers to a node that already exists in the base scene or an instance (`resource_format_text.cpp:218-222`; `packed_scene.cpp:353-356`); when it is not found by name, the engine also looks it up by `unique_id` (`packed_scene.cpp:357-371`). `[editable path=…]` records an editable instance (`resource_format_text.cpp:384-393`); `instance_placeholder` creates a placeholder (`resource_format_text.cpp:242-254`; `packed_scene.cpp:312-332`).

## Results

### How many scripts see several structures

| | Games | Demos | Plugins and tools |
|---|---:|---:|---:|
| First-party scripts | 1,223 | 461 | 1,116 |
| With contexts (own or subclasses') | 647 | 414 | 390 |
| Using literal paths | 316 | 235 | 230 |
| See ≥2 structures | 75 | 29 | 62 |
| &nbsp;&nbsp;counting own contexts only | 44 | 21 | 38 |
| See ≥2 structures and use literal paths | **34** | 11 | 20 |
| Literal-path uses | 2,023 | 1,674 | 1,534 |
| &nbsp;&nbsp;in scripts that see ≥2 structures | 180 | 43 | 107 |

- In games, 34 scripts see ≥2 structures and use literal paths: 2.8% of all first-party scripts, 5.3% of the scripts with contexts, and 10.8% of the scripts that use literal paths. tanks-of-freedom-ii has 15 of them and godot-open-rpg 9.
- 593 game scripts are not attached directly by any scene file: 330 are not node scripts, 113 are attached to resources, 44 are autoloads, 41 are base classes attached only through subclasses, 7 are editor scripts, and the remaining 58 are node scripts that no scene attaches. 9 game scripts have no context at all and still use literal paths: 6 autoloads and 3 other node scripts.
- Instancing a scene unchanged adds no structures. In games, 323 scripts are instanced unchanged elsewhere, giving 5,914 such contexts; 309 of these scripts still have only one structure.

### Do literal paths agree across structures?

| Outcome | Games | of which ≥2 structures | Demos | of which ≥2 structures | Plugins and tools | of which ≥2 structures |
|---|---:|---:|---:|---:|---:|---:|
| Same type in all structures | 1,849 | 145 | 1,640 | 43 | 1,500 | 98 |
| **Different types** | **0** | **0** | **0** | **0** | **0** | **0** |
| Missing from some structures | 16 | 15 | 0 | 0 | 10 | 8 |
| Found in no structure | 11 | 0 | 10 | 0 | 4 | 0 |
| Enters an imported model, cannot be judged | 14 | 5 | 16 | 0 | 0 | 0 |
| Leaves the known tree through `..` | 0 | 0 | 1 | 0 | 2 | 1 |
| Absolute path | 120 | 15 | 2 | 0 | 0 | 0 |
| Script has no context | 13 | 0 | 5 | 0 | 18 | 0 |
| **Total** | **2,023** | **180** | **1,674** | **43** | **1,534** | **107** |

- Of 5,231 literal-path uses, none points to nodes of different types in two structures. The game scripts that see ≥2 structures have 180 uses; 160 of them resolve in two or more contexts, and none of those differs in type either.
- On literal paths, the differences between structures show up only as whether a node is there: in games 15 uses are missing from some structures (next section); in plugins and tools 8 are, all in Pixelorama's `src/Classes/ImageEffect.gd`, whose nodes exist in 18 contexts and not in `ShaderEffect.tscn`.
- Of the 120 absolute paths, 115 are in tanks-of-freedom-ii, mostly reaching an autoload by name, as in `$"/root/SimpleAudioLibrary"`.

### Paths missing from some structures (games)

| Location | Use | Contexts found / missing | Scene where it is missing |
|---|---|---:|---|
| `Starter-Kit-Racing/scripts/vehicle.gd:23` | `get_node_or_null("Container/TrailRight")` | 2 / 1 | `scenes/vehicle-motorcycle.tscn` |
| `godot-open-rts/source/match/Match.gd:56, 67` | `get_node_or_null("Map")` | 5 / 1 | `source/match/Match.tscn`; the script adds Map itself at `:57-59` |
| `tanks-of-freedom-ii/scenes/tiles/tile.gd:64` | `get_node_or_null("reflection")` | 10 / 335 | most tiles |
| `tanks-of-freedom-ii/scenes/tiles/units/unit.gd:119, 416` | `get_node_or_null("mesh_anchor/mesh2")` | 10 / 42 | most units |
| `tanks-of-freedom-ii/scenes/tiles/units/unit.gd:123, 420` | `get_node_or_null("mesh_anchor/mesh3")` | 5 / 47 | most units |
| `tanks-of-freedom-ii/scenes/tiles/tile.gd:35, 38, 53, 63, 68` | `$"mesh"` | 344 / 1 | `scenes/tiles/ground/base_ground.tscn` |
| `tanks-of-freedom-ii/scenes/tiles/units/templates/heli.gd:8` | `$"mesh_anchor/mesh/rotor"` | 6 / 1 | `scenes/tiles/units/templates/heli.tscn` |
| `tanks-of-freedom-ii/scenes/tiles/units/templates/scout_heli.gd:6` | `$"mesh_anchor/mesh/rotor"` | 4 / 1 | `scenes/tiles/units/templates/scout_heli.tscn` |

- 8 uses are `get_node_or_null` probes: the code expects the node may be absent and checks for null afterwards.
- 7 uses are `$` paths to nodes that a base or template scene lacks and only the derived scenes have. `base_ground.tscn` attaches `ground.gd`, a subclass of `tile.gd`, but has no `mesh` child; `scenes/map/templates.gd:774` preloads it as a placeholder tile. `templates/heli.tscn` inherits `unit.tscn` and has no `rotor`; only the scenes that inherit it, such as `blue/heli.tscn`, add `mesh_anchor/mesh/rotor`. These paths would fail if they ran in the scene that lacks the node; whether they run there cannot be decided statically.
- One more use, outside the table, is in a script that sees only one structure: Ring's `breakout/scenes/paddles/paddle.gd:178` uses `$%HealthIcon`, found in 2 contexts and missing in 1. `paddle.tscn` has no `HealthIcon`; it is a unique-name node of `breakout.tscn` and `goal_test_level.tscn`. In those two scenes the paddle's root is owned by the outer root, so the engine's owner fallback finds it. With `paddle.tscn` as a scene on its own, it is not found. Plugins and tools have 2 more uses of this kind (`player_character_body_2d.gd:87-88` in the phantom-camera examples).

### Paths found in no structure (games)

| Location | Path | What the scenes contain |
|---|---|---|
| `tps-demo/door/door.gd:6` | `$DoorModel/AnimationPlayer` | In `door.tscn` this child is called `DoorModel2`; no other checked-out scene, resource or script refers to `door.tscn` |
| `tanks-of-freedom-ii/scenes/map_editor/story/stories/types/move.gd:53` | `$"player_side/side"` | `move.tscn` has no `player_side`; the line is in the `context["type"] == "side"` branch |
| `tanks-of-freedom-ii/scenes/map_editor/story/stories/types/objective.gd:57, 59` | `$"ban/ban_button/label"` | `objective.tscn` has `clear/clear_button/label` and no `ban`; both lines are in `_on_clear_button_pressed` |
| `godot-tiny-mmo/source/client/ui/hud/hud.gd:418` | `get_node_or_null(^"SparringCountdown")` | Created at `:419-421` when not found |
| `Ring/scenes/main.gd:25, 41, 42, 47, 48` | `$Runtime` | Added by `add_child` at `:17` or `:32` |
| `Ring/scenes/main.gd:46` | `$Tetromino` | Added by `add_child` at `:43` |

The last three rows access nodes added at run time. The first three appear to be paths not updated after a rename or a copy (inference). Of the 10 such uses in demos, 5 (`%CoinCount` in `3d/platformer/player/player.gd:47-51`) are an artifact of the analysis (see "Limitations"); the other 5 were not examined one by one. The 4 in plugins and tools are all tokenizer misreads (also under "Limitations").

### Where multiple structures come from (games)

| Mechanism (first that matches) | Scripts | Of those, with literal paths |
|---|---:|---:|
| Inheritance | **48** | **26** |
| &nbsp;&nbsp;scene inheritance only (M2) | 9 | 7 |
| &nbsp;&nbsp;subclass attached elsewhere only (M5) | 36 | 16 |
| &nbsp;&nbsp;both | 3 | 3 |
| Independent attachment to several scenes or nodes (M1, M1b) | 15 | 2 |
| Instance edited by an outer scene (M3) | 12 | 6 |
| **Total** | **75** | **34** |

- Inheritance is the main source: 48 of the 75 scripts, and 26 of the 34 that use literal paths. Of the 39 M5 scripts, 18 have at least some subclass contexts inside inherited scenes; typically the inherited scene replaces the script on the inherited root with a subclass.
- Of the 15 independently attached scripts, 1 belongs to a manual test scene (`tests/manual/Match.gd` in godot-open-rts). Only 2 use literal paths: tps-demo's `enemies/red_robot/parts/part.gd` (three nodes in one scene) and Ring's `breakout/scenes/levels/goal.gd`.
- Non-exclusive counts (M1 and M2 counted only where the structures differ): M1 15, M1b 4, M2 12, M3 15, M4 1, M5 39.

### What these scripts do (games)

| Purpose | Scripts | Of those, with literal paths | Examples |
|---|---:|---:|---|
| Base class for a kind of entity | 27 | 13 | `unit.gd` and `tile.gd` in tanks-of-freedom-ii; `character.gd` in godot-tiny-mmo; `brick.gd` in Ring |
| UI | 19 | 12 | panels, menus and settings pages of a map editor |
| Container that manages its children | 8 | 0 | `battler_roster.gd` in godot-open-rpg; `enemy_action_picker.gd` in deck_builder_tutorial |
| Base class for a level or match | 7 | 5 | `Match.gd` in godot-open-rts; `board.gd` in tanks-of-freedom-ii |
| One script for several content scenes | 6 | 1 | `map.gd` in godot-tiny-mmo (attached to 21 map scenes); `part.gd` in tps-demo |
| Prefab whose instances are edited | 5 | 3 | `door.gd` and `gamepiece.gd` in godot-open-rpg |
| Test, tool, other | 3 | 0 | |

None of the 8 containers uses literal paths; 5 of them call `get_children()`, `get_child()` or `get_child_count()`.

### Access outside the attaching scene's subtree

Each cell is "uses (scripts)":

| Access | Games | Demos | Plugins and tools |
|---|---:|---:|---:|
| `/root/…` paths | 130 (62) | 3 (3) | 8 (1) |
| `get_parent()` | 166 (95) | 53 (35) | 179 (75) |
| `owner.` | 29 (10) | 18 (9) | 7 (3) |
| Paths with `..` | 10 (6) | 38 (27) | 5 (5) |
| Literal paths on other objects (`x.get_node("…")`) | 133 (27) | 53 (25) | 129 (49) |
| `get_node` with a non-literal argument | 30 (25) | 47 (17) | 64 (29) |
| `find_child`, `find_children` | 162 (68) | 14 (7) | 87 (35) |
| Exported Node-typed members | 100 (59) | 42 (25) | 36 (21) |
| Exported NodePath members | 9 (9) | 11 (9) | 17 (16) |
| Scripts calling `add_child` | 222 | 84 | 162 |

The targets of these accesses depend on where the node is placed at run time, or on run-time values, and lie outside the structure defined in this survey. How many literal paths reach nodes added by `add_child` was not counted separately; such paths showed up only among those found in no structure.

## Cases that strain each approach

The cases below were measured; what each one means for an approach is inference.

For A (scene as source of truth):

- **Optional nodes.** In games, 8 `get_node_or_null` uses are missing from some structures; of these, `Match.gd:56`, and `hud.gd:418` outside the table, probe first and then let the script add the node itself. To type such paths, A has to be able to express "may be absent".
- **Template scenes lacking nodes.** In games, 7 `$` uses hold only in derived scenes. If template scenes count as contexts for A, these paths cannot be typed in all structures; if they do not, there has to be a way to mark which scenes do not count.
- **Depends on the outer scene.** `paddle.gd:178` and the 2 uses in the phantom-camera examples resolve only in the outer scene that instances the scene; the attaching scene alone cannot answer.
- **Nodes added at run time.** The 6 uses in Ring's `main.gd` and `hud.gd:418` access nodes that are in no scene file.
- **Inside imported models.** 14 paths in games and 16 in demos enter imported models. The text scenes do not contain these nodes; A needs the scene structure produced by import.
- **Scripts without scenes.** In games, 13 literal-path uses come from scripts with no context at all; 10 of them are `/root/…` accesses between autoloads.

For B (script declarations):

- **Number of declarations.** In games, 316 scripts use 1,471 distinct literal paths (2,023 uses). One declaration per distinct path would be 1,471 declarations; whether subclasses could reuse their base class's declarations, and how many paths are already typed `@onready var` members, was not measured.
- **Declarations that disagree with the scenes.** In games, 11 paths are found in no scene, and 4 of them (`door.gd:6`, `move.gd:53`, `objective.gd:57, 59`) appear to be paths not updated after a rename or a copy. B reports such errors only if declarations are checked against the scenes.

Neither approach directly covers the upward access, absolute paths and run-time values in the table of the previous section.

## Examples

**1. A placeholder scene lacks a node the base class uses.**
tanks-of-freedom-ii: `scenes/tiles/tile.gd:34-38, 52-53`; `scenes/tiles/ground/base_ground.tscn:6-9`

```
func reset_position_for_tile_view():
	var mesh_position = $"mesh".get_position()
	mesh_position.y = 0

	$"mesh".set_position(mesh_position)
...
func hide_mesh():
	$"mesh".hide()
```

```
[node name="base_ground" type="Node3D"]
script = ExtResource("3")

[node name="mouse_collision" parent="." instance=ExtResource("2")]
```

`tile.gd` appears in 345 contexts with 79 structures, and in 344 of those contexts `$"mesh"` is a MeshInstance3D. The missing one is `base_ground.tscn`: it attaches the subclass `ground.gd`, has only `mouse_collision`, and is preloaded as a placeholder tile at `scenes/map/templates.gd:774`.

**2. Optional nodes fetched with `get_node_or_null`.**
Starter-Kit-Racing: `scripts/vehicle.gd:13-23`

```
# (Optional) wheels

@onready var wheel_fl = get_node_or_null("Container/Model/wheel-front-left")
@onready var wheel_fr = get_node_or_null("Container/Model/wheel-front-right")
@onready var wheel_bl = get_node_or_null("Container/Model/wheel-back-left")
@onready var wheel_br = get_node_or_null("Container/Model/wheel-back-right")

# Effects

@onready var trail_left = get_node_or_null("Container/TrailLeft")
@onready var trail_right = get_node_or_null("Container/TrailRight")
```

`vehicle.tscn` has both trails; `vehicle-motorcycle.tscn`, which attaches the subclass `vehicle-motorcycle.gd`, has only `TrailLeft`. The wheel paths enter an imported model and cannot be judged.

**3. A unique name that comes from the outer scene.**
Ring: `breakout/scenes/paddles/paddle.gd:176-178`; `breakout/scenes/breakout/breakout.tscn:95, 172-173`

```
func _on_health_changed(value: int):
	if value < 0:
		Utility.flicker_red($%HealthIcon)
```

```
[node name="Paddle" parent="." groups=["paddles"] instance=ExtResource("1_n40qs")]
...
[node name="HealthIcon" type="TextureRect" parent="UI/Status/Health/Container"]
unique_name_in_owner = true
```

`paddle.tscn` has no `HealthIcon`. Once the paddle is instanced into `breakout.tscn`, its root is owned by breakout's root; `%HealthIcon` looks in the paddle's own unique names, finds nothing, then looks in its owner's and finds the icon in the UI. The answer for this path depends on which scene instances the paddle.

**4. Nodes added at run time.**
Ring: `scenes/main.gd:14-17, 24-25, 37-46`

```
	tween.tween_callback(func():
		var runtime: Node = load("res://Runtime/Runtime.cs").new()
		runtime.name = "Runtime"
		add_child(runtime)
...
	tween.tween_callback(func():
		$Runtime.process_mode = Node.PROCESS_MODE_INHERIT
...
func load_tetromino():
	var tetromino = minigame_scene.instantiate()
	tetromino.name = "Tetromino"
	...
	add_child(tetromino)

func end_tetromino():
	$Tetromino.queue_free()
```

The root of `main.tscn` has only `Title` and `Black` as children. `Runtime` is a node created from a C# script, and `Tetromino` is a minigame scene instantiated at run time.

**5. A path not updated after a rename.**
tps-demo: `door/door.gd:6`; `door/door.tscn:13, 25`

```
@onready var animation_player: AnimationPlayer = $DoorModel/AnimationPlayer
```

```
[node name="DoorModel2" parent="." unique_id=1109006140 instance=ExtResource("2")]
...
[node name="AnimationPlayer" parent="DoorModel2" index="1" unique_id=1279510582]
```

The model node in the scene is `DoorModel2`; the script says `DoorModel`. No checked-out file refers to this scene, so the error apparently never shows at run time (inference).

## Conclusions

- **Few scripts both see several structures and use literal paths.** In games, 34 of 1,223 scripts, or 10.8% of the 316 that use literal paths; 24 of them come from tanks-of-freedom-ii and godot-open-rpg.
- **Literal paths never point to different types in different structures.** 0 of 5,231 uses across the three groups. On literal paths, the differences between structures show up only as whether a node is there.
- **The missing cases come from two idioms.** Probing optional nodes with `get_node_or_null` (8 in games), and template or placeholder scenes that lack nodes only the derived scenes have (7 in games).
- **Multiple structures come mainly from inheritance.** 48 of the 75 game scripts, and 26 of the 34 that use literal paths. 15 scripts are attached independently to several scenes; 2 of them use literal paths.
- **For some paths the answer is not in the attaching scene.** `%Name` resolved through the outer scene (1 in games, 2 in plugins and tools), nodes added at run time, and the inside of imported models (14 in games).
- **Access outside the attaching scene's subtree is common.** In games, 130 `/root/…` paths, 166 `get_parent()` calls, 29 `owner.` accesses and 10 paths with `..`; 222 scripts call `add_child`.

The decisions taken on the basis of these data are recorded in the "Scenes and nodes" section of the [design decisions](../decisions.en.md).

## Limitations

- **Text scenes only.** The 2 binary `.scn` files were not parsed. Imported models were not checked out, and their insides are unknown: a path that enters a model is recorded as "cannot be judged" (14 in games, 16 in demos), and nodes that an outer scene adds under a model instance are dropped by the analysis. The latter makes the 5 `%CoinCount` uses in godot-demo-projects' `3d/platformer/player/player.gd:47-51` appear as "found in no structure": `CoinCount` is in fact added under `Player/Skeleton`, inside the imported-model instance in `player.tscn`. None of the missing outcomes in the games group arise this way.
- **Nodes are matched by name only.** When a name is not found, the engine also looks nodes up by `unique_id`; no entry in this corpus failed to find its parent, so the fallback had no effect.
- **Static analysis only.** `add_child`, `remove_child`, `queue_free`, reparenting, `set_script` and instantiation at run time are not modeled.
- **Every scene file counts as a context.** Unused scenes (such as tps-demo's `door.tscn`), placeholder scenes, test scenes and example scenes all contribute structures.
- **The tokenizer misreads a `%` at the start of a continued line as `%Name`.** A formatting operator `% x` written on a new line inside parentheses is taken for a unique name. The 4 "found in no structure" uses in plugins and tools (Pixelorama's `HandleExtensions.gd:173`, `Main.gd:667, 680` and `DynamicsPanel.gd:58`) are all of this kind; none of the missing outcomes in the games group arise this way.
- **Only literal paths on self are resolved.** Non-literal `get_node`, paths on other objects, `find_child`, `get_child(i)` and exported Node members are counted but not resolved.
- **The structure excludes ancestors and does not compare node properties, groups or signals.** Types are compared as (native type, script).
- **The game code is concentrated in a few repositories.** 24 of the 34 key scripts come from two repositories. The five added games are not a random sample.
- **The purpose classification is a judgment.** An AI agent read and classified the scripts; the quoted code and scene contents were checked against the source files.

## Reproducing

The scripts are in [`scene-structures/`](scene-structures/) and use only the Python 3 standard library (run with Python 3.11); cloning needs git. `fetch_corpus.py` lists every repository and commit and fetches them as sparse, shallow clones; `analyze.py` composes the scene trees, resolves the paths, and writes `repos.json`, `scripts.csv`, `uses.csv` and `contexts.csv`; `report.py` prints the per-repository table and per-group statistics (`report.txt`) and the list of game scripts that see ≥2 structures (`multi_games.txt`); `summarize.py` prints the mechanism, purpose and other aggregates (`summary.txt`), taking the purpose labels from `purpose_labels.csv`; `classes.json` is the native class hierarchy, which `build_classes.py` regenerates from the Godot source (it reads only the class reference XML and builds nothing); `test_fixture.py` runs the self-check on the synthetic project in `fixture_corpus/`, and with `--write` regenerates that project first; `check_tokens.py` and `check_tokens2.py` are spot checks of the tokenizer. [`results/`](scene-structures/results/) holds the `report.txt`, `summary.txt`, `multi_games.txt` and `repos.json` used in this document.

```
python docs/surveys/scene-structures/fetch_corpus.py corpus        # 20 repositories at fixed commits
python docs/surveys/scene-structures/analyze.py corpus out         # out/repos.json, scripts.csv, uses.csv, contexts.csv
python docs/surveys/scene-structures/report.py corpus out          # out/report.txt, out/multi_games.txt
python docs/surveys/scene-structures/summarize.py out              # out/summary.txt
python docs/surveys/scene-structures/test_fixture.py               # prints fixture OK
# optional
python docs/surveys/scene-structures/build_classes.py <Godot source dir>   # regenerate classes.json at commit 469659e
python docs/surveys/scene-structures/check_tokens.py corpus out            # tokenizer vs regex counts
python docs/surveys/scene-structures/check_tokens2.py corpus Pixelorama    # lines where the two disagree
```

Rerun on Windows with Python 3.11, the four corresponding files in `out/` are byte-identical to those in `results/`; with a different `PYTHONHASHSEED`, all outputs stay the same.
