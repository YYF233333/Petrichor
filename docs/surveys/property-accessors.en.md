# Survey: How do real Godot 4 projects use property getters?

**2026-10-22** · Related: [language specification](../language-spec.en.md) §8.6 · 中文：[property-accessors.zh.md](property-accessors.zh.md)

This is a record of an empirical survey for an [Open] item in §8.6 of the specification: whether to split properties into stored and computed properties. It follows the method of the [literal-local survey](literal-local-constraints.en.md): first measure how often the pattern in question occurs in real code, then check what each candidate design would do to that code.

## The question

In GDScript 4 a member variable can have a setter, a getter or both, written inline (`var x: T = init:` followed by an indented block with `set(value):` and `get:`) or as named accessors (`var x: T: set = _set_x, get = _get_x`). A getter is always attached to a stored field, and inside its own accessors `x` names that field. There are two candidate designs:

| Design | Stored property | Computed property |
|---|---|---|
| A: GDScript-style accessors on stored fields | A field with an optional setter and an optional getter. Every read runs the getter, including the engine's reads when it saves, duplicates or inspects the object | Not a separate kind; written as a field whose getter ignores it |
| B: stored/computed split | A field with an optional setter and no getter. Reads always read the field, so engine reads never run user code | No field; must have a getter; the setter is optional and the property is read-only without one; cannot be `@export`; visible to the engine without `PROPERTY_USAGE_STORAGE` |

The rationale is in the [design notes](../design-notes.en.md#deferred-language-proposals). Under B, a GDScript property whose getter reads its own field and does more must be rewritten as a private stored field plus a computed property. We wanted to know how often getters occur, what they do, how many would need that rewrite, and how many of the affected properties are exported. Because both designs keep setters on stored properties, we also recorded what setters do, in particular exported setters that run before `_ready`.

## Corpus

We used 14 MIT-licensed Godot 4 repositories: official demos, an RPG demo, small starter kits, a pixel-art editor, five editor plugins and a game template. A candidate is a member variable with a setter or a getter.

| Repository | Commit | `.gd` files | Lines | First-party lines | Candidates (first-party) |
|---|---|---|---|---|---|
| [godotengine/godot-demo-projects](https://github.com/godotengine/godot-demo-projects) | `b761b4c` | 461 | 30,819 | 30,819 | 39 (39) |
| [gdquest-demos/godot-open-rpg](https://github.com/gdquest-demos/godot-open-rpg) | `d59f2f7` | 342 | 40,662 | 5,422 | 138 (59) |
| [KenneyNL/Starter-Kit-3D-Platformer](https://github.com/KenneyNL/Starter-Kit-3D-Platformer) | `3fa8a04` | 9 | 378 | 378 | 0 |
| [KenneyNL/Starter-Kit-City-Builder](https://github.com/KenneyNL/Starter-Kit-City-Builder) | `4535092` | 6 | 311 | 311 | 0 |
| [KenneyNL/Starter-Kit-FPS](https://github.com/KenneyNL/Starter-Kit-FPS) | `185fd23` | 7 | 451 | 451 | 0 |
| [KenneyNL/Starter-Kit-Racing](https://github.com/KenneyNL/Starter-Kit-Racing) | `2f2e5f2` | 3 | 258 | 258 | 0 |
| [godotengine/tps-demo](https://github.com/godotengine/tps-demo) | `a82f154` | 15 | 1,619 | 1,619 | 2 (2) |
| [Orama-Interactive/Pixelorama](https://github.com/Orama-Interactive/Pixelorama) | `38a8b03` | 249 | 60,025 | 54,782 | 216 (194) |
| [ramokz/phantom-camera](https://github.com/ramokz/phantom-camera) | `21d3762` | 36 | 10,410 | 10,410 | 181 (181) |
| [nathanhoad/godot_dialogue_manager](https://github.com/nathanhoad/godot_dialogue_manager) | `7a72dcf` | 79 | 14,619 | 12,434 | 29 (29) |
| [bitbrain/beehave](https://github.com/bitbrain/beehave) | `fe58915` | 322 | 35,193 | 4,469 | 41 (23) |
| [derkork/godot-statecharts](https://github.com/derkork/godot-statecharts) | `76d226a` | 141 | 18,768 | 4,635 | 51 (16) |
| [MikeSchulze/gdUnit4](https://github.com/MikeSchulze/gdUnit4) | `9bc1a45` | 482 | 54,133 | 27,406 | 21 (18) |
| [Maaack/Godot-Game-Template](https://github.com/Maaack/Godot-Game-Template) | `6849d6c` | 125 | 8,553 | 5,128 | 41 (28) |
| **Total** | | **2,277** | **276,199** | **158,522** | **759 (589)** |

**Licenses.** All 14 are MIT. godot-demo-projects and Godot-Game-Template carry the MIT text without a title. tps-demo licenses its assets and music under CC-BY 3.0 and its code ("All code Copyright (c) 2018-2021 Juan Linietsky, Godot Engine contributors") under MIT, so its code is included.

**First-party** means code whose upstream home is the repository itself: everything outside `addons/`, the addon the repository exists to publish (phantom_camera, dialogue_manager, beehave, godot_state_charts, gdUnit4, maaacks_game_template), and in godot-demo-projects every `addons/` directory, because those plugins are part of the demos. The rest is excluded:

- Vendored addons: Dialogic in godot-open-rpg (79 candidates), GUT in godot-statecharts (35), the copy of gdUnit4 inside beehave (18), Pixelorama's six bundled addons (22), and the five other plugins bundled in Godot-Game-Template (12). The last are by the same author, but each has its own repository.
- Duplicates: Godot-Game-Template's installer copies the addon's examples into `scenes/` and `scripts/` at the project root (1 candidate).
- Unit tests (beehave `test/`, gdUnit4 `addons/gdUnit4/test/`, dialogue_manager `tests/`, statecharts `tests/`; 35,226 lines) are reported separately. They contain only 3 candidates, all in gdUnit4: two have a trivial getter and one has only a setter. They are left out of all rates below.

Because the demo projects' `addons/` now count, the first-party line counts differ from those of the literal-local survey. No file uses Godot 3 `setget`.

## Method

1. **Extract candidates.** `extract.py` strips comments and blanks out strings, joins continued lines, and tracks class and function blocks. It finds member variables at class level, static or not, in both forms: inline blocks (`set(v):`, `get:`, `get():`) and named accessors (`set = f`, `get = g`, on the declaration line or in an indented block). The grammar follows `GDScriptParser::parse_variable` and `parse_property` (`modules/gdscript/gdscript_parser.cpp:1241-1387` at Godot commit `469659e`). For each candidate it records whether it has a setter and a getter, any `@export*` annotation other than group, subgroup and category, `static`, `@onready`, the initializer and the declared type, and the body of each accessor: the inline block, or the named function in the same class. A grep for accessor-like lines found none that the extractor missed.
2. **Read every getter; no sampling.** 195 getters are literally `return <field>`. We read the other 56 and assigned each getter one category:
   - (a) trivial: it returns the field. Under B it is deleted.
   - (b) it reads the field and also does more: lazy initialization, a fallback, a side effect. Under B it needs the rewrite.
   - (c) it does not read the field: a derived value, a view of other members, or a proxy for another object. Under B it is a computed property.

   We also noted getters that act or can fail when read early.
3. **Read every setter** (557 first-party, 3 in tests) and classify it as *plain* (only stores the value), *effect* (stores the value unchanged and also does something else: emits a signal, redraws, updates other nodes), *validate* (stores a transformed value such as a clamped or normalized one, or refuses some values), or *proxy* (never writes its own field).
4. **Setters that run before `_ready`.** For each exported property with a setter, `extract.py` lists what the setter reaches directly or through same-class functions and accessors up to two calls deep: child nodes (`$`, `%`, `get_node`, `find_child`, `get_child`, `get_children`), `@onready` members of the script and its base scripts, and members assigned in `_ready`. We read the 112 setters found this way and one more found while reading, 113 in all, and recorded how each one guards.
5. **Engine reads.** These facts were checked at Godot commit `469659e`:
   - `GDScriptInstance::get` calls a member's getter and `set` calls its setter (`modules/gdscript/gdscript.cpp:1636-1641`, `1571-1575`).
   - An exported member takes the usage of its export annotation; any other member has only `PROPERTY_USAGE_SCRIPT_VARIABLE`, and therefore no `STORAGE` (`modules/gdscript/gdscript_compiler.cpp:2877-2884, 2908`; `modules/gdscript/gdscript_parser.cpp:5420`).
   - Saving a scene and `Node.duplicate()` read every `STORAGE` property with `get` (`scene/resources/packed_scene.cpp:947-964`; `scene/main/node.cpp:3113-3123`). The remote inspector reads every property and every script member, exported or not (`scene/debugger/scene_debugger_object.cpp:85, 152`).
   - When a scene is instantiated, a node's saved properties are set (`packed_scene.cpp:567`) before the node is added to its parent (`:616`), so its own children do not exist yet. Node-typed exported properties are set only after all nodes exist (`:449-468, 677-693`). An instanced sub-scene is built completely, children included (`:334-337`), before the outer scene's values for it are applied.

An AI agent classified each case. The classification is recorded in `classification.json`, and `count.py` reproduces every count below from it.

## Results

### How often accessors occur

| | Properties | Exported | Per 1,000 lines |
|---|---|---|---|
| Setter only | 338 | 158 | 2.13 |
| Getter only | 32 | 0 | 0.20 |
| Setter and getter | 219 | 160 | 1.38 |
| **Total** | **589** | **318** | **3.72** |

Use is very uneven. phantom-camera has 17.4 properties with accessors per 1,000 lines and godot-open-rpg 10.9, while the four Kenney starter kits have none. 7 properties are static, and none is `@onready`.

### Getters by category

| Category | Getters | Exported | With setter | Per 1,000 lines | Under B |
|---|---|---|---|---|---|
| (a) returns the field | 196 | 155 | 191 | 1.24 | Delete the getter |
| (b) reads the field and does more | **15** | 1 | 2 | 0.09 | Rewrite as a private field plus a computed property |
| (c) does not read the field | 40 | 4 | 26 | 0.25 | Becomes a computed property |
| **Total** | **251** | **160** | **219** | **1.58** | |

- Category (a) is the coding style of three projects: phantom-camera (153), godot_dialogue_manager (28) and gdUnit4 (11) account for 192 of the 196. phantom-camera pairs most exported properties with public `set_x`/`get_x` methods (128 of the (a) getters are named). Under B those methods can stay as ordinary methods; only `get = get_x` is removed. One (a) getter, `phantom_camera_2d.gd:475`, contains a branch, but it returns an empty NodePath only when the field already holds one, so it is equivalent to returning the field.
- Category (b) occurs in three repositories: Pixelorama 10, gdUnit4 4 and beehave 1. Only one is exported.
- Whether a getter mentions its own field separates (a) and (b) from (c) in all 56 non-trivial cases, so a migration tool could sort them mechanically.

### Every category (b) getter

| Location | What the getter does |
|---|---|
| `beehave/addons/beehave/nodes/beehave_tree.gd:51` `blackboard` (exported) | Falls back to an internal Blackboard, creating it and adding it as a child on first read |
| `gdUnit4/addons/gdUnit4/src/core/attributes/TestCaseAttribute.gd:19` `timeout` | Lazy default: on first read, replaces the `DEFAULT_TIMEOUT` sentinel with the project setting, writing the field |
| `gdUnit4/addons/gdUnit4/src/core/execution/GdUnitExecutionContext.gd:12` `error_monitor` | Returns the parent context's monitor if there is a parent; otherwise creates its own on first read |
| `gdUnit4/addons/gdUnit4/src/core/execution/GdUnitExecutionContext.gd:19` `test_suite` | Returns the parent context's value if there is a parent, otherwise its own field |
| `gdUnit4/addons/gdUnit4/src/core/execution/GdUnitExecutionContext.gd:24` `test_case` | Returns its own field, falling back to the parent context's value when the field is null |
| `Pixelorama/src/Main.gd:16` `splash_dialog` | Lazy: instantiates the dialog scene and adds it as a child on first read |
| `Pixelorama/src/Palette/PalettePanel.gd:14` `create_palette_dialog` | Lazy: instantiates the dialog, connects its signals and adds it as a child |
| `Pixelorama/src/Palette/PalettePanel.gd:21` `edit_palette_dialog` | Lazy: instantiates the dialog, connects its signals and adds it as a child |
| `Pixelorama/src/UI/Recorder/Recorder.gd:31` `_path_dialog` | Lazy: constructs and configures a FileDialog and adds it as a child |
| `Pixelorama/src/Autoload/DrawingAlgos.gd:11, 16, 37, 42` (four shaders) | Lazy: loads a shader on first read |
| `Pixelorama/src/Autoload/DrawingAlgos.gd:21, 29` (two shaders) | Lazy: duplicates another lazily loaded shader and patches its code |

Twelve of the fifteen are lazy initialization or caching, two fall back to a parent object, and one combines a fallback with lazy creation. Five add a child node when read.

### Category (c)

| Kind | Getters | With setter | Read-only | Examples |
|---|---|---|---|---|
| Proxy for another object | 22 | 21 | 1 | 10 fields of `camera_3d_resource` and 6 of `tween_resource` in phantom-camera; a child node's position; `player.stream_paused` |
| View of other members of the same object | 10 | 4 | 6 | Read-only lists and flags that expose private members; an alias of another exported property; a proxy for a static variable |
| Derived value | 8 | 1 | 7 | A layout size, a report path, a directory scan, a randomly chosen tile |
| **Total** | **40** | **26** | **14** | |

- GDScript cannot make the 14 read-only ones read-only: an assignment to one silently writes the unused field, and the getter keeps returning the computed value.
- 8 of the 40 have an initializer for a field that is never read.
- Two setters write the unused field. `phantom_camera_3d.gd:811` `keep_aspect` stores into its own field while its getter reads `camera_3d_resource`, so the resource is never updated; its nine sibling properties update the resource. `LayerButton.gd:22` `button_pressed` writes both its field and a child button, and reads only the button.
- 4 are exported: `conductor.gd:7` `is_paused` and `node_25d.gd:13` `spatial_position` are proxies for another node, `phantom_camera_2d.gd:435` `draw_limits` is a proxy for a static variable, and `phantom_camera_3d.gd:528` `spring_length` is an alias of the exported `follow_distance`. Under B they cannot remain exported computed properties.

### Exported getters and engine reads

All 160 exported getters run when the scene is saved, when the node is duplicated, and when the remote inspector reads it. 155 of them are category (a), so for them it makes no difference. The other five are `blackboard` (b) and the four exported (c) properties above. Non-exported getters are read by the engine only through the remote inspector.

Getters that act or can fail when read early:

| Location | Category | Behavior |
|---|---|---|
| `beehave_tree.gd:51` `blackboard` (exported) | b | May add a child node during any read, including saving; saving records the internal blackboard, not the field |
| `Main.gd:16`, `PalettePanel.gd:14, 21`, `Recorder.gd:31` | b | Add a child node during any read, including a remote-inspector read |
| `node_25d.gd:13` `spatial_position` (exported) | c | Calls `get_child(0)`; fails when the node has no children yet |
| `ExtensionsApi.gd:253` `tabs_visible` | c | Uses `find_child`; fails if the UI does not exist yet |
| `LayerButton.gd:22` `button_pressed` | c | Reads an `@onready` member; fails before `_ready` |

### Setters

| Properties | Total | Plain | Effect | Validate | Proxy | Exported |
|---|---|---|---|---|---|---|
| Setter only | 338 | 4 | 299 | 31 | 4 | 158 |
| Setter and getter | 219 | 45 | 119 | 31 | 24 | 160 |
| **All setters** | **557** | **49** | **418** | **62** | **28** | **318** |

- Most setters store the value unchanged and then notify or update something. Few validate.
- Of the four setter-only proxies, two discard the value by mistake. `WebSocketServer.gd:14` `refuse_new_connections` is read at line 95 but never stored, so new connections are never refused. `field_camera.gd:10` `gameboard_properties` is never stored, so the camera limits are never computed. The other two are a write-only alias (`phantom_camera_3d_gizmo_plugin.gd:9`) and a setter that forwards to a child label (`PointCollapseContainer.gd:3`).
- 30 setters (16 exported) contain `await`, 25 of them `await ready`. §8.6 does not allow setters to be coroutines, so these need rewriting under either design. No getter contains `await`.
- Subclasses override 5 of the 163 named setters by redefining the function, usually calling `super`. None of the 145 named getters is overridden.

### Setters that run before `_ready`

Of the 318 exported properties with a setter, 113 reach a child node, an `@onready` member or a member assigned in `_ready`. If the value is stored in the scene, the setter runs during instantiation, before the node's own children exist and before its `@onready` members are set.

| Verdict | Setters |
|---|---|
| Guarded: `is_inside_tree()` or `is_node_ready()`; 15 of these then `await ready` | 39 |
| Guarded: a null or `is_instance_valid` check on the member it touches | 16 |
| Guarded in another way: a child count, a flag set in `_ready` or on entering the tree, a custom first-call flag | 6 |
| Unguarded: direct `$Child` access | 42 |
| Tolerant: `get_children()` or `has_node()`, which simply find no children | 3 |
| Not run at load: an inspector button that never stores `true`; a Node-typed export, which the engine sets last | 2 |
| Reaches only plain data assigned in `_ready` | 5 |

- All 20 setters that reach an `@onready` member are guarded.
- The 42 unguarded setters are in two projects: Pixelorama's slider widgets (`BasisSliders.gd` 6, `ValueSliderV2.gd` 16, `ValueSliderV3.gd` 18, `PointCollapseContainer.gd` 1) and tps-demo's `player.gd:38` `player_id`, which `level.gd` sets from code after instantiation. The sliders work because the values are set only on instances of their scenes, after the sub-scene's children exist. In `ValueSliderV2.tscn` and `BasisSliders.tscn` the root node keeps its defaults, and the scenes that use them override the values.
- One guarded setter still loses its value: `conductor.gd:7` `is_paused` checks `if player`, but `player` is a Node-typed export, which the engine sets after the other properties, so a stored `is_paused = true` is dropped.
- 13 properties re-run their setters in `_ready` with `x = x` (Godot-Game-Template 11, godot-demo-projects 2). This relies on an assignment inside the class going through the setter, which §8.6 keeps.
- 79 non-exported setters also reach such members. They run only when code assigns the property, and we did not review them for guards.

### By repository

| Repository | Properties | Per 1,000 lines | Setter only | (a) | (b) | (c) | Exported |
|---|---|---|---|---|---|---|---|
| godot-demo-projects | 39 | 1.27 | 36 | 1 | 0 | 2 | 27 |
| godot-open-rpg | 59 | 10.88 | 59 | 0 | 0 | 0 | 30 |
| tps-demo | 2 | 1.24 | 2 | 0 | 0 | 0 | 2 |
| Pixelorama | 194 | 3.54 | 177 | 1 | 10 | 6 | 49 |
| phantom-camera | 181 | 17.39 | 6 | 153 | 0 | 22 | 154 |
| godot_dialogue_manager | 29 | 2.33 | 0 | 28 | 0 | 1 | 4 |
| beehave | 23 | 5.15 | 19 | 1 | 1 | 2 | 17 |
| godot-statecharts | 16 | 3.45 | 11 | 1 | 0 | 4 | 9 |
| gdUnit4 | 18 | 0.66 | 0 | 11 | 4 | 3 | 0 |
| Godot-Game-Template | 28 | 5.46 | 28 | 0 | 0 | 0 | 26 |

The four Kenney starter kits have no properties with accessors.

## Examples

**1. Category (a) in named form.**
`phantom-camera/addons/phantom_camera/scripts/phantom_camera/phantom_camera_2d.gd:133-135, 1371-1372`

```
@export var priority: int = 0:
	set = set_priority,
	get = get_priority
...
func get_priority() -> int:
	return priority
```

Under B the line `get = get_priority` is removed; `get_priority()` can remain a public method.

**2. Category (b): lazy creation.**
`Pixelorama/src/Main.gd:16-21`

```
var splash_dialog: AcceptDialog:
	get:
		if not is_instance_valid(splash_dialog):
			splash_dialog = load(SPLASH_DIALOG_SCENE_PATH).instantiate()
			add_child(splash_dialog)
		return splash_dialog
```

Under B this becomes a private stored field `_splash_dialog` plus a computed `splash_dialog` with the same getter. The rewrite is mechanical. It does not stop the remote inspector from creating the dialog, because the inspector reads every property, computed ones included.

**3. Category (b), exported: an engine read changes the tree.**
`beehave/addons/beehave/nodes/beehave_tree.gd:51-67`

```
@export var blackboard: Blackboard:
	set(b):
		blackboard = b
		...
	get:
		# in case blackboard is accessed before this node is,
		# we need to ensure that the internal blackboard is used.
		if not blackboard and not _internal_blackboard:
			_internal_blackboard = Blackboard.new()
			add_child(_internal_blackboard, false, Node.INTERNAL_MODE_BACK)
		return blackboard if blackboard else _internal_blackboard
```

The script is `@tool`. When the editor saves a scene in which no blackboard is assigned, saving calls this getter: it may add an internal child, and saving records the getter's result, the internal blackboard, instead of the field, which is null. Under B, saving and duplication read the field, and the fallback moves to a computed property or method that the tree uses internally.

**4. Category (c): a proxy whose setter writes the unused field.**
`phantom-camera/addons/phantom_camera/scripts/phantom_camera/phantom_camera_3d.gd:2850-2859, 2865-2870`

```
func set_keep_aspect(value: int) -> void:
	if not camera_3d_resource:
		printerr("Can't assign a keep_aspect value. No Camera3DResource assigned to ", name)
		return
	keep_aspect = value              # the own field, which nothing reads
	camera_3d_resource_property_changed.emit("keep_aspect", value)

func get_keep_aspect() -> Variant:
	if not camera_3d_resource: return null
	return camera_3d_resource.keep_aspect
...
func set_cull_mask(value: int) -> void:
	...
	camera_3d_resource.cull_mask = value
```

The nine sibling properties write the resource; this one writes a field that nothing reads. Under B a computed property has no field to write, so this mistake could not go unnoticed.

**5. An exported proxy whose stored value is dropped.**
`godot-demo-projects/audio/rhythm_game/game_state/conductor.gd:7-18`

```
@export var is_paused: bool = false:
	get:
		if player:
			return player.stream_paused
		return false
	set(value):
		if player:
			player.stream_paused = value

@export_group("Nodes")
## The song player.
@export var player: AudioStreamPlayer
```

`player` is a Node-typed export, which the engine sets after the other properties. A stored `is_paused = true` therefore reaches the setter while `player` is still null, and is dropped. Under B `is_paused` could not be an exported computed property; it would become a stored property whose setter forwards to the player and whose value `_ready` applies.

**6. A setter that never stores.**
`godot-open-rpg/src/field/field_camera.gd:10-12, 38-40`

```
@export var gameboard_properties: GameboardProperties:
	set(value):
		_on_viewport_resized()
...
func _on_viewport_resized() -> void:
	if not gameboard_properties:
		return
```

The value is never stored, so `_on_viewport_resized` always returns early. Both designs allow this: it is a stored property with a setter.

**7. Unguarded `$Child` access in an exported setter.**
`Pixelorama/src/UI/Nodes/Sliders/ValueSliderV2.gd:24-30`

```
@export var value := Vector2.ZERO:
	set(val):
		value = val
		$GridContainer/X.set_value_no_signal_update_display(value.x)
		$GridContainer/Y.set_value_no_signal_update_display(value.y)
		if _can_emit_signal:
			value_changed.emit(value)
```

If `value` were stored on the root of `ValueSliderV2.tscn`, this setter would run before `GridContainer` exists. It works because only the scenes that instance it store a value, and by then the sub-scene's children exist.

**8. The common guard, which is a coroutine.**
`godot-open-rpg/src/field/cutscenes/popups/interaction_popup.gd:30-37`

```
@export var radius: = 32:
	set(value):
		radius = value

		if not is_inside_tree():
			await ready

		_collision_shape.shape.radius = radius
```

godot-open-rpg guards this way in 20 setters. Because of the `await`, the setter is a coroutine, which §8.6 does not allow.

## Conclusions

- **Getters are uncommon, and most are trivial.** There are 1.58 getters per 1,000 first-party lines. 78% only return the field, and 192 of those 196 come from the habits of three projects. Under B they are simply deleted.
- **The pattern that B makes people rewrite is rare.** A stored field with a non-trivial getter occurs 15 times (0.09 per 1,000 lines), in three repositories. Twelve are lazy initialization or caching, and only one is exported. Each one becomes a private stored field plus a computed property.
- **Computed properties are real and more common than (b).** There are 40 (0.25 per 1,000 lines): proxies, views of private members and derived values. Under A they keep useless storage, 8 even initialize it, 14 cannot be made read-only, and two setters write that storage, one by mistake. B removes the storage and lets the 14 read-only ones be declared read-only.
- **The cost of B is 19 rewrites in 158,522 lines (0.12 per 1,000 lines):** the 15 (b) cases and the 4 exported (c) properties, which cannot remain exported computed properties. Everything else is mechanical, and whether a getter mentions its own field sorts all 56 non-trivial getters correctly.
- **Engine reads rarely run non-trivial getters today.** Of the 160 exported getters, 5 are non-trivial, and one of them (`blackboard`) changes the tree and the saved value. Under B, saving and duplication read only fields, so they cannot run such code; the remote inspector still reads computed properties.
- **The data does not argue against B.** Its cost is small and mechanical to detect, and it removes a real class of surprises.
- **Setter findings that hold under either design.**
  - Exported setters that reach `@onready` members are always guarded in this corpus.
  - The 42 unguarded `$Child` setters depend on values being set only on scene instances.
  - 30 setters use `await`, mostly `await ready`, and §8.6 forbids coroutine setters. That rule therefore has a migration cost of its own, about 0.19 per 1,000 lines.

## Limitations

- **Two projects dominate.** phantom-camera and Pixelorama contribute 375 of the 589 properties, and plugins and tools use accessors far more than game code: the Kenney starter kits have none.
- **Little game code.** Most first-party lines are tools, plugins and demos. Larger game projects might lean more on lazy getters.
- **Scene files were not surveyed.** Only `.gd` files were checked out. Whether an unguarded setter actually fails depends on where the value is stored; we checked this only for Pixelorama's slider scenes, by fetching `ValueSliderV2.tscn` and `BasisSliders.tscn`.
- **The search for setter hazards is mechanical and bounded.** It follows calls within the same class up to two levels deep, and apart from `@onready` members of base scripts it does not track members of other classes. It flags every member assigned in `_ready`, whatever the value, so it also caught 5 setters that touch only plain data. Non-exported setters were not reviewed for guards.
- **The classification is manual.** An AI agent read and classified every getter and setter. The categories are judgments; the cited code and counts were checked against the source.
- **Vendored code is excluded.** Dialogic, GUT and the other bundled addons contain 166 more candidates, which were not classified.

## Reproducing

The scripts are in [`property-accessors/`](property-accessors/) and need only Python 3.11. `scope.py` defines first-party, vendored, duplicate and test code; `extract.py` finds the candidates and collects their accessor bodies and the evidence for review; `dump.py` prints them for reading; `count.py` loads the manual classification in `classification.json` and prints every table above.

```
mkdir corpus && cd corpus
git clone https://github.com/godotengine/godot-demo-projects
git -C godot-demo-projects checkout b761b4c
# clone and check out the other repositories the same way (commits are listed in the corpus table)
cd ..
python docs/surveys/property-accessors/extract.py corpus           # per-repository stats; writes corpus/properties.json and corpus/corpus.json
python docs/surveys/property-accessors/dump.py corpus > dump.txt   # every first-party candidate with its accessors
python docs/surveys/property-accessors/count.py corpus             # all counts in this document
```
