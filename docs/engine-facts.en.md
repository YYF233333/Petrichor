# Engine facts

中文: [engine-facts.zh.md](engine-facts.zh.md)

Engine and GDScript behavior on which the [language specification](language-spec.en.md) relies, checked against Godot 4.7.2-stable (commit `ed1daf0`); paths are relative to the root of the Godot repository. Line numbers and behavior are re-verified item by item when moving to a newer engine commit.

| Fact | Source | Section |
|---|---|---|
| Variant integers are int64_t and floats are double | `core/variant/variant.h:252-253` | §3.2 |
| Converting a Variant to an integer narrower than int64 or unsigned truncates or wraps | `core/variant/variant.h:336-353, 411-424` | §10.6 |
| `real_t` is float in standard builds and double in double-precision builds | `core/math/math_defs.h:143-147` | §4.5, §10.6 |
| The engine's operator table | `core/variant/variant_op.cpp:212-1033` | §5.1, §5.2 |
| `==` between int and float compares numerically; `==` on objects compares identity, after turning freed objects into null | `core/variant/variant_op.cpp:490-491, 514`; `core/variant/variant_op.h:471-477` | §5.1, §5.2 |
| A freed object compares equal to NIL | `core/variant/variant_op.h:484-489` | §3.13 |
| `==` between String and StringName compares contents | `core/variant/variant_op.cpp:196-203, 493, 615` | §5.1, §10.7 |
| Evaluating a combination missing from the operator table is invalid | `core/variant/variant_op.cpp:1049-1054` | §5.2 |
| Dividing an int or an integer vector by 0, or taking it modulo 0, is an invalid evaluation | `core/variant/variant_op.h:134-146, 241-250` | §4.5 |
| Shifts reject negative operands only in debug builds | `core/variant/variant_op.h:360-372, 386-398` | §4.5, §5.1 |
| `**` of two ints is computed in double and converted back, inexact beyond 2^53 and undefined behavior in C++ when out of range or infinite | `core/variant/variant_op.h:86-95` | §4.5, §5.1 |
| `in` on a packed array converts the left operand to the element type first, truncating float to int | `core/variant/variant_op.h:1142-1153` | §5.1 |
| `name in obj` checks whether the object has a member of that name, as `obj.get(name)` does | `core/variant/variant_op.h:1217-1252` | §5.1 |
| `%` of a String or StringName formats a value of any type, an Array as its values; on a format error, validated evaluation prints an error and gives the format string, direct evaluation gives the error's text | `core/variant/variant_op.cpp:205-210, 408-449`; `core/variant/variant_op.h:720-837` | §5.1 |
| The utility functions the engine files under math; their call errors are all argument types, and `posmod` prints an error and returns 0 when the divisor is 0 | `core/variant/variant_utility.cpp:383-402, 459-470, 587-606, 653-668, 697-712, 737-751, 1632-1732`; `core/math/math_funcs.h:312-313` | §4.5, §10.3 |
| Color from a string looks the string up as a color name unless it is hexadecimal, printing an error and giving black when not found; the color names are the names of Color's constants | `core/math/color.h:274-280`; `core/math/color.cpp:396-402`; `core/variant/variant_call.cpp:3086-3089` | §4.5 |
| In debug builds, Basis and Quaternion from an axis and an angle require a normalized axis, Quaternion from a Basis requires a rotation, Quaternion from two vectors requires them non-zero, and Quaternion times a vector requires it normalized | `core/math/basis.cpp:716-717, 843-844`; `core/math/quaternion.cpp:289-290`; `core/math/quaternion.h:93-94, 139-140`; `core/math/math_defs.h:56-58` | §4.5 |
| Array's `==` uses `hash_compare` element by element, which requires the same element type | `core/variant/array.cpp:155-158`; `core/variant/variant.cpp:3176-3179` | §5.2 |
| `hash_compare` takes a NaN as matching a NaN | `core/variant/variant.cpp:3138-3139` | §5.2 |
| Dictionary's `==` looks up each key and compares the values, regardless of order | `core/variant/dictionary.cpp:255-285` | §5.2 |
| A freed object is false in conditions: `is_zero` for an object checks that it is still valid | `core/variant/variant.cpp:952-953`; `core/variant/variant_op.cpp:1120-1122` | §3.3 |
| A Variant converts to an object pointer without checking that the object is alive; object arguments of engine methods are taken out through it, used by the class check in debug builds and handed to the method in release builds | `core/variant/variant.cpp:2024-2026`; `core/variant/binder_common.h:58-61`; `core/variant/variant_caster.h:139-140` | §3.3 |
| A typed Array given an object asks ObjectDB first and refuses a freed one in debug builds, and uses the pointer in release builds; `append`, `erase`, `has` and the like all go through this check | `core/variant/container_type_validate.h:89-112`; `core/variant/array.cpp:285, 350, 499` | §3.3, §3.8 |
| `is_instance_valid` checks first that the object is alive | `core/variant/variant_utility.cpp:1133-1137` | §3.3 |
| min, max and clamp return one of their arguments unchanged | `core/variant/variant_utility.cpp:642-759` | §4.5, §10.3 |
| `find_children` collects only nodes for which `is_class` holds with the given name, or whose script is that global class | `scene/main/node.cpp:2031-2042` | §10.3 |
| lerp requires both arguments to have the same type | `core/variant/variant_utility.cpp:465-469` | §4.5, §10.3 |
| `pop_front` and `pop_back` return null on an empty array without an error | `core/variant/array.cpp:781-800` | §3.8 |
| `pop_at` returns null on an empty array without an error, reports an error and returns null when the index is out of range on a non-empty one, and counts a negative index from the end | `core/variant/array.cpp:802-826` | §3.8 |
| `front` and `back` report an error on an empty array | `core/variant/array.cpp:354-362` | §3.8 |
| Dictionary's `get` returns the default for a missing key | `core/variant/dictionary.cpp:178-187` | §3.8 |
| Ordering operators exist for bool, numbers, String, StringName, vectors, RID and Array, not for objects, Dictionary or Callable; Array orders element by element | `core/variant/variant_op.cpp:731-791`; `core/variant/array.cpp:164-179` | §3.6, §5.2 |
| Array's `sort`, `bsearch`, `min` and `max`, and Dictionary's `sort`, compare with the operator table; `sort` takes a comparison it cannot make as false, and `min` and `max` return null, as they do on an empty array, without an error | `core/variant/array.cpp:721-736, 756-760, 828-868`; `core/variant/dictionary.cpp:315-330` | §3.8 |
| `resize` of a typed Array fills new elements with the default of the type, null for objects | `core/variant/array.cpp:306-318` | §3.8 |
| A typed container given a value that does not fit its element type converts it first when `can_convert_strict` allows, so a float is truncated to an int; otherwise it reports an error and neither writes nor looks up | `core/variant/container_type_validate.h:55-75`; `core/variant/variant.cpp:557-566`; `core/variant/array.cpp:285, 374, 499, 520`; `core/variant/dictionary.cpp:205-207, 222` | §3.8, §5.1 |
| `assign` of a typed Array converts elements by `can_convert_strict` too, and reports an error leaving the array unchanged when one cannot be converted | `core/variant/array.cpp:252-266, 276` | §3.8 |
| The engine types the elements of a container, and the keys and values of a Dictionary, one level deep: by a built-in type, a class and a script | `core/variant/container_type_validate.h:37-40` | §3.8 |
| A Dictionary keeps the hash each key had when inserted, also when it grows, and finds a key by that hash before comparing keys | `core/variant/dictionary.cpp:46`; `core/templates/hash_map.h:130, 149-151, 193-198` | §3.8 |
| `make_read_only` affects only the container it is called on, and nothing makes the container writable again | `core/variant/array.cpp:938-942`; `core/variant/dictionary.cpp:599-603` | §3.8 |
| `bind` appends bound arguments after the call's arguments | `core/variant/callable_bind.cpp:141-150` | §3.11 |
| Constructing a StringName from a String needs a lookup under the global table's mutex | `core/string/string_name.cpp:263-285` | §10.7 |
| Numbers in JSON are all parsed as floats | `core/io/json.cpp:390-396` | §10.8 |
| Resources are cached by path, and `load` reuses the cache by default | `core/io/resource_loader.cpp:800-810` | §7.6 |
| A cyclic resource load returns null | `core/io/resource_loader.cpp:1049-1056` | §7.6 |
| A method's reflection information does not include whether it is a coroutine | `core/object/method_info.h:37-44` | §9.8 |
| The hooks a script language needs are exposed to GDExtension | `core/object/script_language_extension.h:304, 391, 441, 497, 655` | §1.3 |
| A script language from an extension reports abstractness through `_is_abstract` and the global class list | `core/object/script_language_extension.h:132-136, 657-670` | §4.1 |
| A script language reports its file extension through `get_extension` | `core/object/script_language_extension.h:236` | §1.4 |
| Script instances are created without constructor arguments | `core/object/script_language.h:152` | §6.2 |
| `RequiredResult` marks a return value that is null only on error | `core/object/object.h:942-944` | §10.2 |
| Freeing an object calls the free callback of each instance binding | `core/object/object.cpp:2360-2365` | §9.7 |
| An object can hold instance bindings for several tokens at once | `core/object/object.cpp:2167-2201` | §9.7 |
| Destroying an object silently drops all its signal connections | `core/object/object.cpp:2327-2342` | §9.7 |
| An object emits `script_changed` after its script is set | `core/object/object.cpp:995` | §9.8 |
| The engine does not set abstract scripts on objects | `core/object/object.cpp:976` | §4.1 |
| PREDELETE is sent in reverse order, reaching the script instance before the native classes | `core/object/object.cpp:173, 928-931` | §9.7 |
| At PREDELETE a RefCounted object's reference count is already zero, and taking a new reference fails | `core/object/ref_counted.cpp:94-95`; `core/templates/safe_refcount.h:136-146`; `core/variant/variant.cpp:1104-1108, 2509-2510` | §6.6 |
| The script language's per-frame callback is called after the scene tree has flushed its deletion queue | `main/main.cpp:5106-5109`; `core/object/script_language_extension.h:651`; `scene/main/scene_tree.cpp:735` | §3.3, §9.7 |
| The parameters of `_process` and `_physics_process` are double | `scene/main/node.h:433-434` | §4.6 |
| `NOTIFICATION_PARENTED` | `scene/main/node.h:465` | §7.3 |
| `get_tree()` reports an error and returns null when the node is not in a tree; `get_parent()` and `get_owner()` return null without an error when there is no parent or owner | `scene/main/node.h:558-561`; `scene/main/node.cpp:2100-2102, 2296-2298` | §7.3, §10.2 |
| `set_owner(null)` clears the owner without an error | `scene/main/node.cpp:2273-2281` | §7.3, §10.2 |
| `set_owner()` makes any ancestor the owner; a node that is not an ancestor is an error, and the owner is then null | `scene/main/node.cpp:2271-2294` | §7.3, §10.2 |
| When a node leaves its owner's subtree, its owner is set to null | `scene/main/node.cpp:391-396, 1781-1783` | §7.3 |
| When an owner is freed, the owner of the nodes it owns is set to null | `scene/main/node.cpp:299-302` | §7.3 |
| In PREDELETE a node deletes its children one by one, starting from the last | `scene/main/node.cpp:308-312` | §9.7 |
| When a node enters the tree, ready propagates synchronously to completion | `scene/main/node.cpp:3368-3372` | §6.2 |
| Children become ready before their parent; re-entering the tree does not run `_ready` again | `scene/main/node.cpp:326-337, 454` | §6.4 |
| `queue_free` puts the node in the deletion queue, flushed at the end of each physics step and each process frame | `scene/main/node.cpp:3461-3471`; `scene/main/scene_tree.cpp:666, 735, 1626-1643` | §3.3, §9.7 |
| Area3D's `body_shape_entered` passes null as the node when the body's node is not in the tree | `scene/3d/physics/area_3d.cpp:233-234` | §10.2 |
| Sub-resources not set to local to scene are shared by all instances of the scene | `scene/resources/packed_scene.cpp:705-706` | §7.6 |
| Instantiating a scene sets saved properties first, then adds the node to its parent | `scene/resources/packed_scene.cpp:494, 543` | §6.5, §13.3 |
| Loading a scene does not check whether property writes succeed | `scene/resources/packed_scene.cpp:494` | §6.5, §13.3 |
| Node references are stored as paths and resolved during instantiation | `scene/resources/packed_scene.cpp:376-395` | §7.5 |
| Saving a scene writes only properties in the node's current property list | `scene/resources/packed_scene.cpp:860-869` | §13.3 |
| When a scene's root is instanced into another scene, its owner is the root of the outer scene | `scene/resources/packed_scene.cpp:567-570` | §7.3 |
| Renaming or moving a node in the editor updates NodePath properties and animation tracks in the scene | `editor/docks/scene_tree_dock.cpp:2143-2248, 2250-2401` (called at `:2426, 2667`) | §13.3 |
| When a node is moved, the editor emits no signal inside the action | `editor/docks/scene_tree_dock.cpp:2535-2677` | §13.3 |
| Committing an action leaves it before executing it, so no operations can be added during execution | `core/object/undo_redo.cpp:313-332` | §13.3 |
| The editor's undo manager emits `history_changed` after a commit | `editor/editor_undo_redo_manager.cpp:298` | §13.3 |
| An action with the same name and the merge mode `MERGE_ALL`, created within 800 ms after the previous one, merges into it, keeping its operations | `core/object/undo_redo.cpp:97, 116-117` | §13.3 |
| The undo manager chooses the undo history by the action's context object; nodes of the edited scene go to that scene's history | `editor/editor_undo_redo_manager.cpp:63-105, 149-156` | §13.3 |
| A Web export with Extensions Support uses the dynamically linked templates; cross-origin isolation headers are needed only with thread support | `platform/web/export/export_plugin.cpp:374`; `platform/web/export/export_plugin.h:59-73`; `platform/web/doc_classes/EditorExportPlatformWeb.xml:93-95` | §1.3 |
| No export plugin hook can make an export fail | `editor/export/editor_export_platform.cpp:2568-2576`; `editor/export/editor_export_plugin.h:120-122` | §7.5 |
| An error reported by an export plugin only makes the export dialog show a failure; command-line export exits non-zero only when the export function returns an error | `editor/export/editor_export_platform.cpp:131, 146-148`; `editor/editor_node.cpp:1408-1424` | §7.5 |
| GDScript types global classes of other languages through the Script reflection interface | `modules/gdscript/gdscript_analyzer.cpp:789-804, 4278-4319, 6005-6014` | §10.1 |
| GDScript gives only an unsafe-access warning for an unknown method called on an object other than self or a built-in type | `modules/gdscript/gdscript_analyzer.cpp:3750-3757` | §10.1 |
| When GDScript resolves `extends GlobalClassName` it parses the base class's file as GDScript, reporting only "Could not resolve super class" on failure | `modules/gdscript/gdscript_analyzer.cpp:469-485` (errors at `:477, 483`); `modules/gdscript/gdscript_cache.cpp:83-92` | §11.2 |
| GDScript requires concrete subclasses to implement abstract methods, and forbids constructing abstract classes and calling abstract methods through `super` | `modules/gdscript/gdscript_analyzer.cpp:1533-1552, 3630-3643` | §4.1 |
| GDScript's override check allows a covariant return type, contravariant parameter types, and extra parameters with defaults | `modules/gdscript/gdscript_analyzer.cpp:1895-1936` | §4.6 |
| GDScript's static checks require typed Arrays and Dictionaries to have exactly the same element types, and do not check when one side is untyped | `modules/gdscript/gdscript_analyzer.cpp:6379-6393` | §3.8 |
| GDScript's strings in single or double quotes may span lines | `modules/gdscript/gdscript_tokenizer.cpp:1086-1098` | §2.2 |
| GDScript takes comment lines beginning with `##`, `###` included, as documentation: a block of them directly above a declaration or its annotations, or one after the declaration on its line, documents it, local variables and constants included; the first block before the first member documents the class | `modules/gdscript/gdscript_parser.cpp:842-862, 1068-1101, 2195-2228, 4066-4075` | §2.2 |
| GDScript's `@warning_ignore` reports a warning name it does not know as an error | `modules/gdscript/gdscript_parser.cpp:5126-5128` | §2.3 |
| GDScript's `@warning_ignore` ignores warnings by lines, from the annotation's line to the end of the head of the statement or declaration it annotates; `@warning_ignore_start` ignores up to the `@warning_ignore_restore` of the same name | `modules/gdscript/gdscript_parser.cpp:291-294, 5131-5189, 5199-5228` | §2.3 |
| GDScript's static checks convert String to and from StringName and NodePath implicitly, and StringName and NodePath not to each other | `modules/gdscript/gdscript_analyzer.cpp:6370-6374`; `core/variant/variant.cpp:579-587, 735-750` | §10.7 |
| GDScript does not allow typed containers whose element types are typed containers | `modules/gdscript/gdscript_parser.cpp:3903-3904` | §3.8 |
| GDScript's rest parameter can only be an Array with Variant elements | `modules/gdscript/gdscript_analyzer.cpp:1803-1810` | §4.6 |
| GDScript reads `METHOD_FLAG_VARARG` from other languages' method information | `modules/gdscript/gdscript_analyzer.cpp:6052-6055` | §4.6 |
| GDScript checks `new` as a static `_init`; GDScript binds `new` itself, the Script base class does not | `modules/gdscript/gdscript_analyzer.cpp:5958-5961`; `modules/gdscript/gdscript.cpp:1078` | §10.1 |
| GDScript's `new` constructs the native object, runs the member initializers of every class and passes its arguments to the nearest `_init` up the inheritance chain | `modules/gdscript/gdscript.cpp:125-152, 176-194, 210-254` | §6.1, §11.1 |
| GDScript does not check the signature of `_init` by the override rules | `modules/gdscript/gdscript_analyzer.cpp:1830-1875` | §4.8, §6.1 |
| The engine attaching a GDScript script calls `_init` without arguments, and creates no script instance when the call fails | `modules/gdscript/gdscript.cpp:192-203, 406-424` | §6.2 |
| GDScript takes a script of another language that reports no `_init` to have only a constructor without arguments | `modules/gdscript/gdscript_analyzer.cpp:6005-6031` | §10.1 |
| GDScript's script object calls static functions by name, and reads constants, static variables and static functions | `modules/gdscript/gdscript.cpp:928-940, 957-997` | §4.8, §11.1 |
| A GDScript instance finds a method by name from its script up, static functions included | `modules/gdscript/gdscript.cpp:1948-1966` | §4.8, §6.3 |
| GDScript by default only warns about a static method called on an instance | `modules/gdscript/gdscript_analyzer.cpp:3699-3703`; `modules/gdscript/gdscript_warning.h:131` | §4.8 |
| GDScript converts an enum value to int silently, an int to an enum with a warning on by default, and not one enum to another; `as` casts an int to an enum without a check | `modules/gdscript/gdscript_analyzer.cpp:3826-3829, 6326-6336, 6375-6378, 6397-6406`; `modules/gdscript/gdscript_warning.h:140` | §3.7, §5.3 |
| GDScript makes an enum's name a read-only Dictionary from the names of its values, as String, to the values, and starts a variable of an enum without the value 0 at 0 with a warning | `modules/gdscript/gdscript_analyzer.cpp:1159-1197, 2207-2217` | §3.7 |
| `get_keycode_with_modifiers()` gives a Key with the bits of the modifiers, a value outside the enum | `core/input/input_event.cpp:394-396` | §3.7 |
| GDScript types a script used as a value by its class, and a constant initialized with a preloaded script can be a type | `modules/gdscript/gdscript_analyzer.cpp:873-892, 5776-5786` | §2.1 |
| In GDScript a GDScript class used as a value is a GDScript object | `modules/gdscript/gdscript_analyzer.cpp:6447-6449` | §2.1 |
| Without a script instance, `callp` looks only for engine-registered methods, so static methods on a Script resource are not found | `core/object/object.cpp:800-826` | §11.3 |
| GDScript cannot refer to a type inside another language's class in a type annotation | `modules/gdscript/gdscript_analyzer.cpp:935-937` | §11.3 |
| GDScript does not treat constants of another language's class as constant expressions | `modules/gdscript/gdscript_analyzer.cpp:4321-4330` | §11.3 |
| GDScript requires the initializer of a constant to be a constant expression | `modules/gdscript/gdscript_analyzer.cpp:2124-2131` | §4.5 |
| GDScript computes an operation whose sides are both constant at compile time, reporting an invalid evaluation as an error | `modules/gdscript/gdscript_analyzer.cpp:3139-3157` | §4.5 |
| GDScript computes a subscript of a constant at compile time, reporting one it cannot get; a repeated constant key in a dictionary literal is a compile error | `modules/gdscript/gdscript_analyzer.cpp:3854-3856, 4920-4927` | §4.5 |
| GDScript makes arrays and dictionaries in constants read-only at every level | `modules/gdscript/gdscript_analyzer.cpp:5318-5367` | §4.5 |
| GDScript types only autoloads written in GDScript | `modules/gdscript/gdscript_analyzer.cpp:806-826, 4579-4616` | §11.3 |
| In GDScript, calling a Callable variable with function-call syntax is a compile error suggesting `.call()` | `modules/gdscript/gdscript_analyzer.cpp:3741-3745` | §3.11 |
| In GDScript, a coroutine call used as a value without await is a compile error; as a statement it is a warning | `modules/gdscript/gdscript_analyzer.cpp:3776-3784` | §9.1, §9.8 |
| `?` is a syntax error in GDScript | `modules/gdscript/gdscript_parser.cpp:3860-3861` | §2.2 |
| GDScript's prefix `!` is logical not | `modules/gdscript/gdscript_parser.cpp:2963-2965` | §2.2 |
| GDScript's `**` is left-associative and binds tighter than unary `-` | `modules/gdscript/gdscript_parser.h:1448-1450`; `modules/gdscript/gdscript_parser.cpp:2942, 3001` | §5.1 |
| In GDScript `[` cannot follow an inner class name, the name in `class_name`, an enum name or a function name | `modules/gdscript/gdscript_parser.cpp:964, 1005-1010, 1602, 1805` | §2.4 |
| In GDScript an inner class without `extends` extends RefCounted, and `extends` may be written on the first line of its body | `modules/gdscript/gdscript_analyzer.cpp:425-429`; `modules/gdscript/gdscript_parser.cpp:974-980` | §2.1, §4.1 |
| In GDScript an inner class resolves the constants, enums and inner classes of the classes enclosing it by bare name, but not their static functions and variables | `modules/gdscript/gdscript_analyzer.cpp:4229, 4249, 4270-4271` | §4.1, §4.8 |
| In GDScript a bare name is looked up in the class, its script bases and the classes around it, then among the properties, methods, signals, enums and integer constants of its native base; a bare name in a type may be an enum of the native base | `modules/gdscript/gdscript_analyzer.cpp:332-343, 837-839, 4185-4275, 4333-4385` | §4.1, §4.8 |
| In GDScript a variable, constant, signal, enum or inner class of a class with the name of a signal, property or integer constant of its native base is a compile error | `modules/gdscript/gdscript_analyzer.cpp:255-276, 1050, 1110, 1121, 1151, 1216, 1232, 1250` | §4.1 |
| A GDScript inner class is a script of its own, with no resource path and no global name, listed among the constants of the class enclosing it | `modules/gdscript/gdscript.cpp:1082-1084`; `modules/gdscript/gdscript_parser.h:778-780`; `modules/gdscript/gdscript_compiler.cpp:3003` | §4.1, §10.5 |
| `class_name`, `@tool`, `@icon` and `@static_unload` cannot be written in a GDScript inner class | `modules/gdscript/gdscript_parser.cpp:146-149, 1215, 1859-1861` | §4.1 |
| An abstract method declaration has no colon | `modules/gdscript/gdscript_parser.cpp:1773-1774` | §2.4 |
| The `@abstract` annotation, which cannot be applied to static methods | `modules/gdscript/gdscript_parser.cpp:4500-4524` | §4.1 |
| A rest parameter must be the last parameter and cannot have a default value | `modules/gdscript/gdscript_parser.cpp:1709-1716` | §4.6 |
| `...` cannot appear in GDScript expressions | `modules/gdscript/gdscript_parser.cpp:4344` | §4.6 |
| A GDScript type can only start with an identifier or `void` | `modules/gdscript/gdscript_parser.cpp:3874-3886` | §2.4 |
| A GDScript lambda containing await is a coroutine, with no marker | `modules/gdscript/gdscript_parser.cpp:3249-3251, 3752-3753` | §9.1 |
| match patterns | `modules/gdscript/gdscript_parser.cpp:2487-2711` | §4.6, §5.6 |
| In a GDScript match, literal and expression patterns compare the type first, String and StringName counting as the same, and then the value | `modules/gdscript/gdscript_compiler.cpp:1451-1477, 1550-1556` | §5.6 |
| `as` to an object type yields null on failure | `modules/gdscript/gdscript_vm.cpp:1677-1678` | §5.3 |
| GDScript's `as` from a value that is not an object to an object type reports an error in debug builds and passes the value on unchanged in release builds | `modules/gdscript/gdscript_vm.cpp:1670-1681` | §5.3 |
| GDScript's `is Array[T]` requires the array's type to match exactly, and so does assigning an array to a typed variable, which otherwise reports an error | `modules/gdscript/gdscript_vm.cpp:879-895, 1454-1481` | §3.8 |
| GDScript types an array or dictionary literal only where it goes into a typed container, and leaves it untyped otherwise | `modules/gdscript/gdscript_analyzer.cpp:2112-2121, 3647-3658` | §3.13 |
| GDScript reports an error and stops on `is` with a freed object, in release builds too, and on `as` only in debug builds | `modules/gdscript/gdscript_vm.cpp:942-947, 1665-1669` | §3.3 |
| When GDScript calls an engine method, debug builds check only whether the base object was freed, and pass the arguments on as they are | `modules/gdscript/gdscript_vm.cpp:2046-2055, 2072` | §3.3 |
| In GDScript, a local variable, a constant, the variable of a `for` and a match binding may not have the name of a local variable or parameter of an enclosing block; parameters are locals of the function body | `modules/gdscript/gdscript_parser.cpp:1731, 1735, 1983-1987, 1993-2002, 2345-2349, 2599-2600, 4384-4392` | §4.6, §5.6 |
| The body of a GDScript lambda has the block around it as its parent, and its parameters are declared without checking the names around it | `modules/gdscript/gdscript_parser.cpp:1728-1735, 3758-3760` | §4.6 |
| `as` to a built-in type converts through the constructor, reporting a failed conversion only in debug builds | `modules/gdscript/gdscript_vm.cpp:1642-1650` | §5.3 |
| At runtime GDScript reports an error for a combination missing from the operator table; when the operand types differ from those of the first execution, only debug builds check, and release builds get null | `modules/gdscript/gdscript_vm.cpp:793-798, 821-843` | §5.2 |
| `for` can iterate over floats and vectors | `modules/gdscript/gdscript_vm.cpp:3092-3229` | §5.7 |
| A failed GDScript `assert` reports "Assertion failed" | `modules/gdscript/gdscript_vm.cpp:3876-3902` | §8.3 |
| GDScript's await accepts any Signal value | `modules/gdscript/gdscript_vm.cpp:2586-2600` | §9.8 |
| GDScript warns of a redundant await on a call whose return type is neither a coroutine, Variant nor Signal | `modules/gdscript/gdscript_analyzer.cpp:3102-3107` | §9.8 |
| The result of an await is written unchecked into a temporary typed by the awaited call's return type | `modules/gdscript/gdscript_vm.cpp:2667-2668`; `modules/gdscript/gdscript_byte_codegen.cpp:998-1001` | §9.8 |
| Assigning a Variant to a typed variable checks the type only in debug builds | `modules/gdscript/gdscript_vm.cpp:1433-1444` | §9.8 |
| A suspended GDScript function state does not copy self and refers to its instance by a raw pointer | `modules/gdscript/gdscript_vm.cpp:2609-2625` | §9.4 |
| GDScript puts the instance into a Variant as self at every call, so a RefCounted object gets null during PREDELETE; calls of its own methods take it as their receiver | `modules/gdscript/gdscript_vm.cpp:656-657`; `modules/gdscript/gdscript_byte_codegen.cpp:1345-1355` | §6.6 |
| After an error GDScript returns a default value built for the return type in debug builds, and null in release builds | `modules/gdscript/gdscript_vm.cpp:3963-3993` (debug); `:535, 3995` (release) | §8.1 |
| GDScript's call stack limit is set by `debug/settings/gdscript/max_call_stack`, 1024 by default, past which it reports a stack overflow | `modules/gdscript/gdscript.cpp:2870`; `modules/gdscript/gdscript.h:489-490` | §8.1 |
| GDScript's `assert` is compiled only in debug builds | `modules/gdscript/gdscript_compiler.cpp:2181-2207` | §8.3 |
| GDScript evaluates the message of an `assert` before testing its condition, and requires it to be a String | `modules/gdscript/gdscript_compiler.cpp:2185-2198`; `modules/gdscript/gdscript_analyzer.cpp:2399-2405` | §8.3 |
| GDScript's chained assignment reads and modifies along the chain, then writes value types back while shared types are modified in place | `modules/gdscript/gdscript_compiler.cpp:1172-1190` | §3.6 |
| Member initializers bypass setters | `modules/gdscript/gdscript_compiler.cpp:2414-2430, 2603-2626` | §4.8, §6.1 |
| Construction does not wait for the return value of `_init`; engine virtuals with return values synchronously read and convert the script result | `modules/gdscript/gdscript.cpp:194`; `core/object/make_virtuals.py:1-10, 173-174` | §9.1 |
| `_static_init` is called after static members are initialized | `modules/gdscript/gdscript_compiler.cpp:2629-2632` | §4.8 |
| GDScript initializes the static members of the class around first, then its inner classes in turn | `modules/gdscript/gdscript.cpp:667-683` | §4.8 |
| GDScript runs static initialization only in a process with scripting enabled, or in tool scripts | `modules/gdscript/gdscript.cpp:803, 882-887` | §4.8 |
| `_static_init` must be static, take no parameters and return nothing | `modules/gdscript/gdscript_parser.cpp:1751-1757`; `modules/gdscript/gdscript_analyzer.cpp:1841-1850` | §4.8, §6.1 |
| Static variables cannot be `@export` or `@onready` | `modules/gdscript/gdscript_parser.cpp:4536-4537, 4665-4666` | §4.8 |
| In GDScript a static variable of an object type starts as null | `modules/gdscript/gdscript_compiler.cpp:2555-2590` | §4.8 |
| Without `@static_unload`, a GDScript script with static variables stays in the cache | `modules/gdscript/gdscript_compiler.cpp:3315-3316`; `modules/gdscript/gdscript_cache.cpp:449-453` | §4.8 |
| GDScript allows access to static variables through an instance | `modules/gdscript/gdscript_analyzer.cpp:4229` | §4.1, §4.8 |
| GDScript's scripts and instances read and write static variables by name | `modules/gdscript/gdscript.cpp:973-985, 1022-1043, 1562-1587, 1634-1646` | §4.8 |
| GDScript's reload that keeps state runs static initialization again, then moves the old static values back by name | `modules/gdscript/gdscript.cpp:714-731, 803-807, 882-892` | §4.8 |
| `@onready` initializers run before the user's `_ready`, base class first | `modules/gdscript/gdscript.cpp:1937-1953` | §6.3 |
| Freeing a GDScript instance clears its suspended function states without resuming them | `modules/gdscript/gdscript.cpp:2069-2079`; `modules/gdscript/gdscript_function.cpp:343-362` | §9.7, §9.8 |
| A GDScript function takes the address of its instance's members when it starts; when the instance is freed while the function runs, the members go with it | `modules/gdscript/gdscript_vm.cpp:751`; `modules/gdscript/gdscript.cpp:2066-2084` | §3.3 |
| `GDScriptFunctionState` has a `completed` signal | `modules/gdscript/gdscript_function.cpp:369` | §9.8 |
| When a GDScript coroutine suspends again, the inner state remembers the first state's `completed` signal only by ObjectID | `modules/gdscript/gdscript_vm.cpp:2634-2639` | §9.8 |
| A GDScript function state leaves the instance's pending list when it resumes | `modules/gdscript/gdscript_function.cpp:329-330` | §9.8 |
| `GDScriptFunctionState` is an internal class that does not expose its instance; `is_valid` is always false once it has resumed | `modules/gdscript/gdscript_function.cpp:288-307, 337, 364-370`; `modules/gdscript/register_types.cpp:140` | §9.8 |
| Reloading a GDScript script clears its suspended functions without emitting a signal | `modules/gdscript/gdscript.cpp:1487-1506`; `modules/gdscript/gdscript_compiler.cpp:2700` | §9.8 |
| A GDScript reload that keeps state switches every instance to the new code and moves old values over by member name without checking their types | `modules/gdscript/gdscript_compiler.cpp:3070-3074`; `modules/gdscript/gdscript.cpp:2038-2064` | §12.4 |
| Awaiting a multi-parameter signal yields an Array of the arguments | `modules/gdscript/gdscript_function.cpp:256-274` | §9.6 |
| When an awaited object is freed, the GDScript waiter stays suspended forever | `modules/gdscript/gdscript.cpp:2069-2079` | §9.7 |
| A lambda in a RefCounted class holds self strongly | `modules/gdscript/gdscript_lambda_callable.cpp:285-291` | §3.11 |
| A GDScript lambda receives its captures as arguments on every call, so assigning one changes neither the outer variable nor the next call; GDScript warns about it | `modules/gdscript/gdscript_lambda_callable.cpp:101-120`; `modules/gdscript/gdscript_analyzer.cpp:2895-2917` | §3.11 |
| A GDScript lambda may be named; the name labels its Callable and is not declared in its body | `modules/gdscript/gdscript_parser.cpp:3731-3733`; `modules/gdscript/gdscript_lambda_callable.cpp:61-62` | §3.11 |
| A GDScript reload moves a lambda to the one at the same position in the new version, and invalidates it when the number of lambdas, the captures or the parameters differ | `modules/gdscript/gdscript_compiler.cpp:3217-3254`; `modules/gdscript/gdscript.cpp:1411-1421` | §12.4 |
| GDScript's LSP is an editor plugin | `modules/gdscript/language_server/gdscript_language_server.h:35`; `modules/gdscript/register_types.cpp:167` | §13.1 |
| The editor indents with tabs by default; a line break after an opening bracket indents one level more, and a closing bracket right after the caret moves to the next line, back at the original indentation | `editor/settings/editor_settings.cpp:830`; `scene/gui/code_edit.cpp:1349-1385, 4128-4131` | §13.2 |
| The editor comments selected lines out by putting `#` after each line's indentation, and at the start of an empty one | `editor/gui/code_editor.cpp:1356-1371`; `scene/gui/text_edit.cpp:4194-4202` | §13.2 |
| GDScript at most converts source to tokens at export, and parses and compiles at load time | `modules/gdscript/register_types.cpp:99-116`; `modules/gdscript/gdscript_cache.cpp:373-379, 392` | §1.3, §4.5 |
