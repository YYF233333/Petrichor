# Survey: A catalog of pitfalls in Godot 4 scripting

**2026-10-23** · Engine: Godot commit `469659e` · Related: [language specification](../language-spec.en.md) · 中文：[pitfalls.zh.md](pitfalls.zh.md)

This survey collects the pitfalls that Godot 4 script authors run into, in two groups, engine and GDScript, and the C# (.NET) bridge, 80 in all. For each one it asks whether a new statically typed script language and its toolchain could remove it, by what means, and whether the fix needs C++ APIs that are open to an engine module but not to GDExtension. The catalog itself is [`pitfalls/catalog.md`](pitfalls/catalog.md), with the data in `pitfalls/catalog.tsv`; this write-up describes its sources and method and summarizes the results. Every fix class in it is an inference, not a decision; the specification cites entries by ID, as listed at the end.

## The question

- What pitfalls do Godot 4 script authors commonly hit? Which come from the engine and GDScript, and which from the C# bridge?
- For code written in the new language, can each one be removed by language semantics, by static checking or tooling, or by a runtime wrapper over public APIs?
- Does any fix need C++ APIs that GDExtension does not expose, so that only a module could do it? Which parts need an engine change whichever form the language is delivered in?

## Sources

| Source | Scope | Use |
|---|---|---|
| Godot source | godotengine/godot commit `469659e0617345149cd1eaec2e643d61d130aa6a` (master, 4.8-beta) | Evidence from reading the source only, cited as `path:line`; no runtime reproduction (see Limitations) |
| Godot documentation | Tutorials and class reference at docs.godotengine.org/en/latest | Quoted, with page addresses |
| GitHub | Issues and PRs of godotengine/godot, and godotengine/godot-proposals | Searched with the GraphQL and REST APIs through the GitHub CLI; states, reactions and comment counts read on 2026-10-23 |
| Forums and Q&A | forum.godotengine.org, godotforums.org, Stack Overflow | Site search APIs and web searches; topic counts and titles only |

We also dumped all 4,377 issues labelled `topic:gdscript` in godotengine/godot and all 941 proposals with the same label in godot-proposals, sorted by reactions, to rank individual issues and proposals, as in "2nd most-reacted topic:gdscript proposal".

Five researchers (AI agents) each covered one area and produced 102 raw rows in all:

| Researcher | Scope (summarized from the rows produced) | Raw rows |
|---|---|---:|
| EG-A | Lifecycle, object liveness and leaks, coroutines, signals, timers, static variables | 20 |
| EG-B | Resource caching and duplication, saving and exports, `@tool` and the editor, node and file paths, hot reload | 20 |
| EG-C | Type system and language semantics: lambda capture, nullability, casts, containers, equality, enums, overrides, namespaces, error handling, interfaces, generics | 20 |
| CS-A | C# script identity, cross-language calls, type mapping, editor integration | 21 |
| CS-B | Export platforms, build and toolchain, reloading, object lifetime and GC, performance, exceptions | 21 |
| **Total** | | **102** |

## Method

1. **Raw rows.** Each row has 13 fields: id, group, title, description, minimal example, root layer, issue evidence, docs evidence, source evidence, frequency signal, status, fix class and the reason for the class. Engine behavior was established only by reading the source; claims that would need a run to confirm are marked "unverified (needs repro)".
2. **Fix classes.** Judged for a new, non-.NET, statically typed script language and its toolchain, and for code written in that language:

   | Class | Meaning |
   |---|---|
   | L | Language semantics |
   | T | Static checking or toolchain |
   | W | Runtime wrapper over the public ClassDB and GDExtension APIs |
   | M | Needs C++ APIs not exposed to GDExtension; module only |
   | E | Needs an engine core change |
   | N/A | Specific to .NET; the parentheses give the analogous question the new language faces and its class |

   Classes combine. "L+W (order itself: E)" means that L and W remove the pitfall for code in the new language, while the engine behavior named in parentheses stays. The primary class is the first letter of the class string; an entry whose class string names E is counted as having an E part.
3. **Merging.** 42 of the 102 raw rows were the same or closely related and were merged by hand into 20 entries, recorded in `merges.py` with the raw rows each came from; the other 60 were kept as they were. `build.py` checks that every raw row is used exactly once, numbers the entries G-01–G-49 and C-01–C-31 in the order given in `merges.py`, computes the counts below, and writes `catalog.tsv` and `catalog.md`.
4. **Frequency signal.** Each entry records the reactions and comments on the issues and proposals it cites, their rank in the dumps, hit counts from title searches, and topic counts from forum searches. These numbers show how many people care, not how often the pitfall occurs; see "Limitations".
5. **Checking.** Every `path:line` cited in this write-up was read again against the same commit while writing it. Paths are relative to the Godot repository root; `Godot.SourceGenerators/` stands for `modules/mono/editor/Godot.NET.Sdk/Godot.SourceGenerators/` and `GodotSharp/Core/` for `modules/mono/glue/GodotSharp/GodotSharp/Core/`; a bare `:n` refers to the nearest file named before it.

## Results

### Entries by class

| Group | L | T | W | M | E | N/A | Total | With an E part |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Engine and GDScript | 36 | 6 | 7 | 0 | 0 | 0 | 49 | 19 |
| C# | 13 | 3 | 4 | 0 | 1 | 10 | 31 | 2 |
| **Total** | **49** | **9** | **11** | **0** | **1** | **10** | **80** | **21** |

- Of the analogous questions for the 10 N/A entries, 7 have primary class L, 2 T and 1 W.
- Root layers: of the 49 engine and GDScript entries, 25 are in the engine core, 22 in the GDScript language and 2 in the editor and toolchain; of the 31 C# entries, 24 are in the .NET bridge, 5 in the editor and toolchain and 2 in the engine core.
- By this classification, all 49 engine and GDScript entries can be removed for code in the new language, mostly by language semantics; 19 of them keep an engine-level part that GDScript, other languages and the editor still meet. Of the 31 C# entries, 10 disappear with .NET, but each has an analogous question the new language has to answer as well; only C-17 has primary class E.

### Entries with the most attention

Taking the highest reaction count among the issues, proposals or PRs each entry cites, 15 entries reach at least 100 (G-27 cites proposals#162 only as related and is not counted). G-25, which has few reactions but loses data silently, is added, for 16 in all. Reactions were read on 2026-10-23.

| Entry | Pitfall | Frequency signal | Engine evidence | Class |
|---|---|---|---|---|
| G-37 | Object types are implicitly nullable; no null safety | proposals#162: 1,038, 2nd among topic:gdscript proposals; at least 350 forum topics contain "null instance" | The analyzer treats null as compatible with every object type (`modules/gdscript/gdscript_analyzer.cpp:6460-6462`) | L+T |
| G-49 | No generics; typed collections are one level deep | proposals#12224: 260; 5 related proposals total 1,612, of which 836 are for typed Dictionary, implemented in 4.4 | `Array[Array[int]]` is a parse error (`modules/gdscript/gdscript_parser.cpp:3879`); engine containers record one level of element type (`core/variant/container_type_validate.h:37-40`) | L+T (engine-visible part: E) |
| G-48 | No interfaces or traits; contracts across inheritance branches fall back to duck typing | proposals#4872: 830; #6416: 672; 4th and 5th | Object type compatibility is decided only along the inheritance chain (`modules/gdscript/gdscript_analyzer.cpp:6458-6507`) | L |
| G-15 | Signal connections made in the editor name the handler as a string in `.tscn`; after the handler is renamed or deleted, the connection is skipped silently | proposals#899 (rename tooling, implemented in 4.8, scripts only): 520; the scene part is tracked only in #123958: 6 | At load a Callable is built from the name without checking that the method exists (`scene/resources/packed_scene.cpp:747, 763`); emission skips invalid Callables without reporting (`core/object/object.cpp:1328-1331`) | T |
| C-26, C-28 | Struct-typed properties and packed arrays come back as copies; implicit string-to-StringName conversion and similar allocate on every call | proposals#7842: 497 | Each packed-array conversion copies the native buffer (`GodotSharp/Core/NativeInterop/Marshaling.cs:437-446`); each implicit string conversion creates a new StringName (`GodotSharp/Core/StringName.cs:81`) | L; L+T |
| C-19 | C# projects cannot be exported to the web; mobile is still experimental | Draft PR #106125 (web export): 451; #68153 (Android, closed for 4.2): 234; #70796 (web): 103 | The blocker is the .NET WASM runtime, outside `modules/mono`; not verified in source | N/A (analog: W) |
| G-45 | class_name and [GlobalClass] share one flat global namespace | proposals#1566: 363, 10th | Global classes of every language are registered in one name-keyed table, and an existing entry is updated in place (`core/object/script_language.cpp:408-431`) | L (registry part: E) |
| G-46 | get_class() and is_class() ignore class_name | #21789: 170, 2nd among topic:gdscript issues | `get_class()` returns the native class name (`core/object/object.h:677`) | L (engine part: E) |
| C-11 | .NET collections, LINQ results, structs, records and Nullable<T> cannot cross the engine boundary | proposals#438: 143 | The marshallable types are a closed list (`Godot.SourceGenerators/MarshalUtils.cs:88-249`); methods that use other types are left out silently (`Godot.SourceGenerators/ExtensionMethods.cs:321-344`) | N/A (analog: L+T) |
| G-14 | Signal parameter types and counts are not checked at connect or emit | proposals#2557: 134 | emit does not check its arguments; a mismatch only prints an error when the handler is called (`core/object/object.cpp:1256-1283, 1367-1385`) | L+T |
| G-02 | An `_init` with required parameters breaks scene instantiation, duplicate() and set_script() | proposals#1513: 130 | The interface that creates script instances takes no arguments (`core/object/script_language.h:156`); GDScript constructs with zero arguments (`modules/gdscript/gdscript.cpp:434`) | L+W (zero-argument construction: E) |
| C-18 | Classes registered by GDExtensions have no C# type | proposals#8191: 129 | Creating the managed wrapper skips up to the first engine-registered base class (`modules/mono/csharp_script.cpp:1145-1153`) | W |
| G-20 | Resource.duplicate() depth: shallow by default, and deep copies skip external subresources | #74918 (closed in 4.5): 120 | `duplicate(true)` always uses INTERNAL mode and copies only built-in subresources (`core/io/resource.cpp:296-305, 517-529`) | W+L |
| G-34 | Scenes and resources that reference each other fail to load | proposals#7363: 110; #21461 (class_name cycles, fixed in 4.0): 110 | A load re-entered on the same thread is treated as a cycle and returns null (`core/io/resource_loader.cpp:1049-1056`) | T+L (loader cycles: E) |
| G-25 | Renaming or removing an exported member silently discards the values saved in every scene and resource | proposals#10644: 42; 5 proposals make the same request | During instantiation the result of `node->set` is not checked, so unknown names are dropped (`scene/resources/packed_scene.cpp:567`) | L+T |

### No entry needs a module

- None of the 80 entries is classed M. The catalog's reason is that every hook the entries rely on is already exposed to GDExtension: the script-instance callbacks (`GDExtensionScriptInstanceInfo3`, `core/extension/gdextension_interface.json:3242-3349`) and the virtuals of ScriptExtension and ScriptLanguageExtension for script reloading (`core/object/script_language_extension.h:79, 686, 690`), exports and the property list (`:176, 188`), validation, completion and symbol lookup (`:393, 493, 533`), and global class names (`:754`). The specification gives this as one reason to deliver the language as a GDExtension (§1.3; Appendix B).
- Compared with the engine's internal ScriptInstance interface (`core/object/script_instance.h:40-96`), the extension script-instance callbacks lack only two entries: `call_const` (`:70`) and `get_rpc_config` (`:93`). The default `call_const` simply calls `callp` (`core/object/script_instance.cpp:57-59`); the default `get_rpc_config` takes the script's RPC configuration (`:88-90`), which an extension script can supply through `_get_rpc_config` (`core/object/script_language_extension.h:220-224`). No entry depends on either (inference).
- A module cannot fix the E parts either: it can call C++ APIs that are not exported, but it cannot change what the engine does (inference). In G-06, for example, a node's lock count `data.blocked` is a private member with only the protected `_block` and `_unblock`, and there is no public way to read it even from C++ (`scene/main/node.h:217, 375-376`).
- The two forms of delivery differ in packaging, not in APIs (inference; see C-19). On the web, GDExtension needs Extensions Support enabled in the export preset (`variant/extensions_support`, off by default, `platform/web/export/export_plugin.cpp:374`), and cross-origin isolation headers are needed only when thread support is also enabled (`platform/web/doc_classes/EditorExportPlatformWeb.xml:93-95`; the tutorial [Exporting for the Web](https://docs.godotengine.org/en/latest/tutorials/export/exporting_for_web.html) says enabling GDExtension support itself requires them). A module is compiled into the engine and needs a self-built editor and export templates.

### What stays in the engine

21 entries have an E part. The table groups them by theme (the grouping is this write-up's) and gives the engine behavior that remains. Most of these can be worked around for code in the new language, but engine-owned objects and containers, other languages and the editor still see the behavior.

| Theme | Entry | Engine behavior that remains |
|---|---|---|
| Lifecycle order | G-01 | During instantiation a node's saved properties are set (`scene/resources/packed_scene.cpp:567`) before the node is added to its parent (`:616`), when its own children do not exist yet |
| | G-02 | Script instances are created without arguments (`core/object/script_language.h:156`); scene nodes are constructed by ClassDB first and get their script afterwards (`scene/resources/packed_scene.cpp:389, 496`) |
| | G-03 | `_ready` runs on the children before the parent (`scene/main/node.cpp:326-336`) |
| | G-06 | A node is locked while it propagates enter_tree and ready to its children (`scene/main/node.cpp:325-330, 374-383`), and add_child on it fails meanwhile (`:1721`); the lock count is a private member (`scene/main/node.h:217`) |
| | G-09 | PREDELETE reaches the script first (`core/object/object.cpp:140, 1009-1011`); the node then removes itself from its parent and deletes its children (`scene/main/node.cpp:304-312`); when a subtree exits, children go before their parent (`:421-427`) |
| Object model | G-10 | Node derives directly from Object and is not reference-counted (`scene/main/node.h:54`), so a node removed from the tree and never freed leaks |
| | G-46 | `get_class()` returns the native class name (`core/object/object.h:677`) |
| | C-17 | Engine classes registered as abstract (such as `CollisionObject3D` and `PhysicsBody3D`, `scene/register_scene_types.cpp:736-737`) cannot be instantiated through a script subclass in any language (#71834); a script cannot inherit from a script in another language (documentation) |
| Global registry | G-45 | Global classes of every language share one flat table in ScriptServer (`core/object/script_language.cpp:408-431`) |
| Containers and Variant | G-27 | Arrays whose element type is an object accept null (`core/variant/container_type_validate.h:60-61`) |
| | G-28 | A typed array records its element type at run time and compares built-in type, class name and script exactly (`core/variant/container_type_validate.h:37-47`) |
| | G-49 | The element type is one level deep (`core/variant/container_type_validate.h:37-40`) |
| | G-41 | Variant `==` is registered only for some pairs of types (`core/variant/variant_op.cpp:489-493`); Dictionary keys compare the type first, with String and StringName as the only exception (`core/variant/dictionary.cpp:46`; `core/variant/variant.cpp:3213-3216, 3437-3447`) |
| | G-42 | Enum values cross the engine boundary and are saved as int |
| Resources and scenes | G-19 | A resource loaded by path exists once in the cache and is shared by every user (`core/io/resource_loader.cpp:800-810`) |
| | G-22 | Only values set during instantiation go through local-to-scene handling (`scene/resources/packed_scene.cpp:509-514`) |
| | G-31 | Which nodes and changes an inherited scene or an instance saves is decided by owner and instancing (`scene/resources/packed_scene.cpp:880-882, 1153-1154`); refreshing open instances after the base scene changes is also in the editor |
| | G-34 | A cyclic load on the same thread returns null (`core/io/resource_loader.cpp:1049-1056`) |
| Other | G-17 | A Timer node emits at most one timeout per frame (`scene/main/timer.cpp:60-67`) |
| | G-47 | Errors inside engine C++ methods are printed and a default value is returned; scripts can only observe them afterwards through `OS.add_logger` (`core/core_bind.cpp:870`) |
| | C-10 | The editor inserts a generated handler at the end of the file (`editor/script/script_text_editor.cpp:430-434`), which for a language that wraps the class in braces lies outside the class |

### Three C# problems

The survey followed up three problems with the C# bridge in particular. Their entries in the catalog, and what was found:

**1. The file name must equal the class name.** C-01; see also G-45.

- The source generator registers as a script only a top-level partial class whose name equals the file name; nested classes, non-partial classes and classes with another name are skipped without a diagnostic (`Godot.SourceGenerators/ScriptPathAttributeGenerator.cs:43-56`).
- The error appears only at instantiation: "Cannot instantiate C# script because the associated class could not be found …" (`modules/mono/csharp_script.cpp:2550-2551`). Build failures and assembly problems give the same message; 22 issues contain it, with varied causes. The error text quoted in the C# basics documentation, "Cannot find class XXX for script res://XXX.cs", no longer exists anywhere in `modules/mono`.
- Renaming the file or the class in an external IDE breaks the pairing and the references in scenes (#85274, open, reproduced on 4.6.1 in 2026-02).
- `[GlobalClass]` registers only the type name and drops the namespace (`GodotSharp/Core/Bridge/ScriptManagerBridge.cs:279`), into the flat table of G-45.
- The class is L+T. Inference: when the file itself is the class, there is no second name to mismatch.

**2. Cross-language calls contrary to the documentation, and crashes.** C-05, C-06, C-07, C-08; crashes and breakage in C-25, C-22 and C-16.

| What the documentation says | What the source does | Entry |
|---|---|---|
| "Accessing C# fields from GDScript is straightforward, you shouldn't have anything to worry about." | In editor builds a C# script reports only its exported members (`modules/mono/csharp_script.cpp:2952-2961`), and static members do not cross the bridge: GDScript cannot read static fields, completion misses members, and the member list differs between the editor and an exported build | C-06 |
| "calling C# methods from GDScript should be straightforward" | Methods are looked up by name and argument count (`modules/mono/csharp_script.cpp:1767-1782`), so omitting a default argument finds nothing; overloads with the same argument count collapse to one; generic methods, methods with ref/out parameters and methods that do not map to Variant are left out silently (`Godot.SourceGenerators/ExtensionMethods.cs:321-344`) | C-05 |
| A C# call to a GDScript method with wrong arguments "will fail silently and won't error out" | It is reported through `GD.PushError` in debug builds only (`GodotSharp/Core/NativeInterop/ExceptionUtils.cs:145-155`) and never throws; a GDScript lambda passed to C# becomes an empty Callable (`modules/mono/glue/runtime_interop.cpp:533-538`) | C-07 |
| An untyped array "can be converted to a typed array using the Godot.Collections.Array<T>(Godot.Collections.Array) constructor" | The constructor only wraps the original array, which stays untyped on the engine side (`GodotSharp/Core/Array.cs:1188-1193`) | C-08 |

The catalog found no crash caused by a cross-language call as such. The crashes and breakage it found are where the .NET runtime meets the engine:

- C-25: a RefCounted held only by C# is released by a GC finalizer on another thread; the resource cache can resurrect a resource whose wrapper was already collected, which in debug builds trips `CRASH_COND(gchandle.is_released())` (`modules/mono/csharp_script.cpp:1916`). #83762 has been open for three years with 39 comments, and was traced to the resource cache only in 2026-08.
- C-22: when the editor rebuilds and assembly unloading fails, C# scripts stay unusable until the editor is restarted (`modules/mono/glue/GodotSharp/GodotPlugins/Main.cs:254-277`). The error message links straight to the tracking issue #78513 (`modules/mono/mono_gd/gd_mono.cpp:811`), which has 95 reactions and 194 comments.
- C-16: in the editor, scripted objects without [Tool] are placeholder instances that C# wraps as their native base class, so [Tool] code that casts them to their C# type throws InvalidCastException (#36395: 24; #85459: 19).

**3. C# features that do not connect back to the engine.** C-02, C-09, C-11, C-12, C-13, C-14, C-15.

- The types that can cross the boundary are a closed list (`Godot.SourceGenerators/MarshalUtils.cs:88-249`). Exporting another type is a compile error (GD0102), and so is using one as a signal parameter (GD0202); but ordinary methods that use such types are left out silently (`Godot.SourceGenerators/ExtensionMethods.cs:321-344`). So .NET collections, LINQ results, structs, records and Nullable<T> cannot be exported, used as signal parameters or passed as Variant (C-11).
- Interfaces do not derive from GodotObject and have no marshal type (`Godot.SourceGenerators/MarshalUtils.cs:210-213, 248`), so they cannot be signal parameters or export types (C-12).
- A method returning Task does not map to Variant and so is not exposed to the engine (`Godot.SourceGenerators/ExtensionMethods.cs:324-330`); GDScript's await on a value that is not a signal continues at once (`modules/gdscript/gdscript_vm.cpp:2591-2596`) (C-14).
- `+=` on a `[Signal]` event only adds to a private backing delegate (`Godot.SourceGenerators/ScriptSignalsGenerator.cs:289-309`), so the engine never sees the handler: it is not in the connection list, IsConnected returns false, and it is not disconnected automatically when the receiver is freed (#70414: 40) (C-09).
- A generic class has a script file but cannot be instantiated (`modules/mono/csharp_script.h:137-139`), attached to a node or made a global class (C-02).
- Enums are reported as plain int in method and signal metadata (C-13); for scripts without [Tool], the exported defaults shown in the inspector are guessed by analyzing the source rather than by running the initializers (C-15).
- Of these 7, C-02, C-09, C-11, C-12 and C-14 are N/A, C-13 is L+W and C-15 is L+T. Inference: the new language's own generics, structs, interfaces and coroutines meet the same question, namely that every type that crosses the engine boundary needs a defined Variant representation; the analogous questions are classed L (C-02), L+W (C-09, C-14) and L+T (C-11, C-12).

## Limitations

- **Issue-tracker bias.** Most evidence comes from GitHub. Issues and proposals are mostly written by experienced users and plugin authors; behavior that is by design and documented is often closed as not planned and stops gathering reactions. Beginners mostly ask on the forums, where only topic counts were taken.
- **Reactions are not frequency.** Reactions show how many people agree or want something, not how many are bitten. Feature requests gather reactions more easily than bug reports, older issues have more than newer ones, and duplicates split them. G-15's 520 belongs to the rename tooling as a whole; the scene part on its own has 6.
- **Searches are selective.** Counts come from GitHub GraphQL and REST searches, title searches and web searches, and depend on the terms the researchers chose. A figure such as "39 issues have this phrase in the title" includes unrelated hits; forum searches read a limited number of pages and record only "at least" when there are more.
- **No runtime reproduction.** No entry was confirmed by running the engine. 12 entries have part of their evidence or class marked as unverified or inferred from reading (G-04, G-09, G-18, G-23, G-24, G-31, G-34, G-48, C-01, C-16, C-19, C-21), and G-04, G-18, G-23 and G-24 explicitly need a reproduction; for 7 more no documentation statement was found (G-25, G-26, G-27, G-31, G-33, G-34, G-35). One researcher did run a locally built Godot editor headless about 10 times on a throwaway project; the catalog cites only source and web evidence and uses nothing from those runs.
- **The classification is inference.** The fix classes were judged by AI agents for a language that does not exist yet, and have not been tested by an implementation. Merging 102 raw rows into 80 entries was also a judgement; every merge is recorded in `merges.py`.
- **Coverage is incomplete.** The catalog has no entries on threads on the GDScript side, multiplayer and RPC, property paths in animation tracks and `tween_property`, input action and group names, GDScript run-time performance, shaders, or translation. Feature requests with many reactions that do not present as pitfalls are absent too, such as proposals#7329 structs (1,086, 1st among topic:gdscript proposals), #737 union types (510) and #641 access modifiers (423).
- **Master only.** Engine facts were checked only at commit `469659e`; behavior in older versions rests on the descriptions in issues.

## Reproducing

The [`pitfalls/`](pitfalls/) directory contains:

- `catalog.tsv`: 80 entries in 15 columns, the 13 raw columns plus `primary_class` and `source_rows`;
- `catalog.md`: the same content as tables;
- `raw/`: the 102 raw rows of the five researchers;
- `merges.py`: the 20 entries merged by hand, and the final order;
- `build.py`: checks that every raw row is used exactly once, numbers the entries, computes the counts in "Entries by class", and writes `catalog.tsv` and `catalog.md`;
- `helpers/`: the PowerShell scripts used for searching.

`build.py` needs only the Python 3 standard library:

```
python docs/surveys/pitfalls/build.py out    # writes out/catalog.tsv and out/catalog.md and prints the counts
```

Without an argument it writes into its own directory. The two files it produces are byte-for-byte identical to those in the directory.

The web evidence cannot be reproduced mechanically: issue states, reactions and comment counts were read on 2026-10-23 and will change, and so will the results of GitHub and web searches. The scripts in `helpers/` record how the searches were made; they need a logged-in GitHub CLI (`gh`):

| Script | Purpose |
|---|---|
| `gq.ps1` | GraphQL search for issues and PRs, sorted by reactions |
| `gs.ps1` | GraphQL search that also lists creation dates |
| `egb_search.ps1` | REST search sorted by reactions, retrying on rate limits |
| `iss.ps1` | State, labels, duplicates, cross-references and closing PR of one issue or PR |
| `csa_dup.ps1` | Duplicates and cross-references only |
| `dump.ps1` | Dumps every issue in a repository with a given label; used for the topic:gdscript dumps above |
| `freq.ps1` | Stack Overflow question totals and forum.godotengine.org topic counts |

```
pwsh docs/surveys/pitfalls/helpers/dump.ps1 godotengine/godot topic:gdscript godot_gdscript.tsv
pwsh docs/surveys/pitfalls/helpers/dump.ps1 godotengine/godot-proposals topic:gdscript proposals_gdscript.tsv
pwsh docs/surveys/pitfalls/helpers/gq.ps1 'repo:godotengine/godot "previously freed" in:title'
pwsh docs/surveys/pitfalls/helpers/iss.ps1 godotengine/godot 21789
```

The dumps are large and reflect only the day they were read, so they are not in the repository. The copies of documentation source files the researchers read are separately licensed and are not included either; every documentation quote in the catalog gives the page address. Engine facts can be checked against the `path:line` citations in a checkout of Godot commit `469659e`.

## Use in the specification

The [language specification](../language-spec.en.md) cites these entries: G-07 and G-08 (§3.3), G-02 (§6.2), G-03 and G-05 (§6.4), G-04 (§6.3), G-09 (§6.6), G-10 (§6.7), G-11 (§9.7), G-12 and G-13 (§9.5), G-19, G-20 and G-34 (§7.6), and C-17 (§11.2).
