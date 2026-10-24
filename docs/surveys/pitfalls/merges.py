# Merged rows (written by hand from the raw rows they cite in source_rows).
# Field values must not contain tabs or newlines; ' | ' separates items.

MG = 'modules/mono/editor/Godot.NET.Sdk/Godot.SourceGenerators/'
GS = 'modules/mono/glue/GodotSharp/GodotSharp/Core/'
D = 'https://docs.godotengine.org/en/latest/tutorials/'

MERGED = {}

MERGED['M1'] = dict(
    source_rows='CS-A-01, CS-A-03, CS-A-05', group='C#',
    title='Script identity: the class must be named exactly like its .cs file; one top-level partial class per file',
    description='A C# script resource resolves to the non-nested partial class whose name equals the file name (case-sensitive); a mismatching, nested or second class is silently skipped at build time, and the failure appears only on instantiation as a generic "associated class could not be found" error that build and assembly problems also produce; renaming the file or the class (especially from an external IDE) breaks the pairing and scene references; the docs still quote an error text the engine no longer emits.',
    example='// Player.cs: public partial class PlayerController : CharacterBody2D {}  -> attaching Player.cs fails at run time | // Enemy.cs: a nested partial class Hitbox : Area2D cannot be attached to a node',
    root_layer='.NET bridge',
    evidence_issues='#15661 (open, 43 reactions, 53 comments: attach a C# class by name instead of source path) | #85274 (open, 1 reaction, 16 comments: renaming file or class breaks references; reproduced on 4.6.1 in 2026-02) | #85292 (open, 3 comments: same-named partial files in two folders -> class not found) | #71735 (closed, 26 comments) | #66419 (closed, 11 comments) | #74146 (closed not planned) | #81683 (closed by PR #124305, 4.8: nested GodotObject members missing in GDScript completion) | #104268 (closed by PR #104279, 4.5) | #83071 (closed: generator broke with >2 nesting levels)',
    evidence_docs=D + 'scripting/c_sharp/c_sharp_basics.html "Attached C# scripts should refer to a class that has a class name that matches the file name." and (stale) "Cannot find class XXX for script res://XXX.cs" | ' + D + 'scripting/cross_language_scripting.html "the class Godot will use is the one named like the .cs file itself" | ' + D + 'scripting/c_sharp/c_sharp_global_classes.html "The file name must match the class name in case-sensitive fashion." | ' + D + 'scripting/c_sharp/diagnostics/GD0003.html "A script file must only contain one class with a name that matches the file name."',
    evidence_source=MG + 'ScriptPathAttributeGenerator.cs:43-44 (nested classes ignored), :47-52 (non-partial classes skipped), :54-56 (classes whose name differs from the file name dropped, no diagnostic) | modules/mono/csharp_script.cpp:2550-2551 (actual run-time message "...contains a class definition with a name that matches the filename of the script exactly (it\'s case-sensitive)"; "Cannot find class" occurs nowhere in modules/mono) | modules/mono/csharp_script.cpp:393-400 (editor derives the class name from the file name in validate_path) | ' + GS + 'Bridge/ScriptManagerBridge.cs:306-313 (only the first ScriptPathAttribute of a type is registered) | editor/docks/filesystem_dock.cpp:1701 (dependency rewrite lives in the FileSystem dock move path; that external renames bypass it is inferred, as stated by a contributor in #85274)',
    frequency='22 godotengine/godot issues contain "associated class could not be found" (all labels, mixed causes; search 2026-10-23) | >=6 forum.godotengine.org threads with that error on one web-search page | #15661 43 reactions | 13 topic:dotnet issues match "nested class" (0-3 reactions each)',
    status='by-design (documented limitation); edge-case fixes PR #104279 (4.5), PR #124305 (4.8); #85274 open',
    fix_class='L+T',
    fix_reason='Make the file itself the class (GDScript model) or derive the class name from the path, leaving no second name to mismatch; the compiler reports any declaration that would silently not become a script; a rename refactoring in the LSP/CLI rewrites .tscn/.tres/.uid references (T).')

MERGED['M3'] = dict(
    source_rows='CS-A-19, CS-B-19', group='C#',
    title='In the editor, non-[Tool] scripted objects are placeholders that C# sees as their native base (InvalidCastException)',
    description='In the editor only tool scripts get real instances; other scripted objects get placeholder instances and C# wraps them as their native class, so [Tool] code or editor plugins that cast to, export, or assign through typed [Export] setters a non-tool C# type throw InvalidCastException; users end up marking every class [Tool].',
    example='[Tool] partial class Spawner : Node { [Export] public EnemyData Data; }  with non-tool EnemyData : Resource  -> InvalidCastException in the editor',
    root_layer='.NET bridge',
    evidence_issues='#36395 (open, 24 reactions, 22 comments; duplicate #112085; "The exact same structure and code in Gdscript works as intended") | #85459 (open, 19 reactions, 20 comments; still reported 2026-03) | #96306 (open, 4 reactions; duplicate #115304) | #80298 (open, 2 reactions)',
    evidence_docs=D + 'plugins/running_code_in_the_editor.html "any other GDScript that your tool script uses must also be a tool. The editor is not able to construct instances from GDScript files without @tool" | same page: "Code from other nodes doesn\'t run in the editor. Your access to other nodes is limited."',
    evidence_source='core/object/object.cpp:1066-1071 (if Script::can_instantiate() is false in the editor, placeholder_instance_create) | modules/mono/csharp_script.cpp:2540-2554 (CSharpScript::can_instantiate is false for non-tool scripts while scripting is disabled in the editor) | modules/mono/csharp_script.cpp:1145-1165 (an object without a C# instance gets a managed wrapper of its native class; path inferred from reading) | modules/gdscript/gdscript.cpp:265-267 (GDScript makes the same choice)',
    frequency='19 topic:dotnet issues mention InvalidCastException and Tool (GitHub search) | 43 reactions over #36395 and #85459 | #85459 comment (2026-03-07): "I\'m making all my scripts [Tool] and [GlobalClass] without thinking too much about it"',
    status='open',
    fix_class='W+T',
    fix_reason='Whether an object gets a placeholder is decided by the script language itself (Script::can_instantiate, object.cpp:1066; ScriptExtension _can_instantiate/_placeholder_instance_create), and type tests can consult get_script() inheritance, which placeholders keep; the checker can flag tool code that calls into non-tool types (T). No engine change needed (inference).')

MERGED['M4'] = dict(
    source_rows='CS-A-12, CS-B-08', group='C#',
    title='+= on [Signal] events and capturing lambdas bypass engine connection tracking',
    description='A [Signal] event\'s += only adds to a private managed backing delegate (the engine sees one internal connection per signal), and a lambda that captures a variable yields a Callable with no receiver ObjectID; such handlers are not listed, IsConnected returns false, and they are not disconnected when the receiver is freed (ObjectDisposedException or leaks); signals cannot be declared in interfaces.',
    example='enemy.Died += OnDied; enemy.IsConnected(Enemy.SignalName.Died, Callable.From(OnDied)) -> false | timer.Timeout += () => { x++; GD.Print(Name); }; Free(); // next tick: ObjectDisposedException',
    root_layer='.NET bridge',
    evidence_issues='#70414 (open, confirmed, 40 reactions, 20 comments; duplicate #74984) | #76690 (open, 9 reactions, 12 comments: IsConnected wrong for custom and editor-connected signals) | #80033 (open, 8 reactions: cannot inherit signals from interfaces) | #82346 (closed not planned, 2 reactions) | #66319 (closed 2022)',
    evidence_docs=D + 'scripting/c_sharp/c_sharp_signals.html "Connecting to a custom signal using += doesn\'t disconnect automatically when the receiving node is freed." | same page: "When a signal receiver is freed while the signal emitter is still alive, in some cases automatic disconnection won\'t happen: - The signal is connected to a lambda expression that captures a variable. - The signal is a custom signal." | "In contrast with other C# events, you cannot use Invoke to raise events tied to Godot signals."',
    evidence_source=MG + 'ScriptSignalsGenerator.cs:289-309 (event add/remove only touch backing_<Signal>; TODO at :285-288) | modules/mono/csharp_script.cpp:1949-1963 (one EventSignalCallable per custom signal, connected to the owner itself) | modules/mono/signal_awaiter_utils.cpp:179-186 (that callable re-raises the C# event) | ' + GS + 'Callable.cs:74 (_target = @delegate?.Target as GodotObject; a closure object is not a GodotObject) | modules/mono/managed_callable.cpp:81-86 (without a target the callable\'s object is a shared middleman, never freed with the receiver)',
    frequency='#70414 40 reactions (9th most-reacted topic:dotnet issue) | 7 issues mention "70414" | the C# signals docs page devotes a section to it',
    status='open (documented limitation; workaround: -= in _ExitTree, or Connect())',
    fix_class='N/A (analog: L+W)',
    fix_reason='.NET events are absent in Petrichor; the analog is to make engine connect/disconnect the only subscription mechanism and create closures as CallableCustom carrying their owner\'s object_id (GDExtensionCallableCustomInfo2.object_id, core/extension/gdextension_interface.json:2460-2474), so connection lists, is_connected and auto-disconnect behave as for GDScript.')

MERGED['M5'] = dict(
    source_rows='CS-A-14, CS-B-13', group='C#',
    title='.NET collections, LINQ results, structs, records and Nullable<T> cannot cross to the engine',
    description='Only a closed list of Variant-compatible types is marshallable: List<T>, Dictionary<K,V>, IEnumerable/LINQ results, user structs, records, plain classes, Nullable<T> and some arrays (bool[], Vector2I[], enum arrays) cannot be exported, used in signals or passed as Variant; non-exported members and methods using them are silently not exposed; code keeps two collection families and converts (copying every element) at the boundary.',
    example='[Export] public List<int> Ids = new();  -> GD0102 | public List<int> GetIds()  -> silently not callable from GDScript',
    root_layer='.NET bridge',
    evidence_issues='#70298 (open, 38 reactions, 7 comments; duplicate #105387 bool[]) | godot-proposals#438 (open, 143 reactions, 21 comments: export structs) | godot-proposals#997 (open, 56 reactions: export custom System.Object/struct types) | #95358 (open, 1 reaction, 7 comments: Vector2I[] rejected) | #95813 (closed not planned: enum arrays) | #19868 (open, 9 comments: Nullable<float>) | #65937 (closed, 4 reactions: ArrayMesh needs Vector3[] instead of Godot.Collections.Array)',
    evidence_docs=D + 'scripting/c_sharp/c_sharp_exports.html "only certain C# arrays and the collection types defined in the Godot.Collections namespace are Variant-compatible, therefore, only those types can be exported." but also "C# arrays can exported as long as the element type is a Variant-compatible type." (not true for bool[]/Vector2I[]) | ' + D + 'scripting/c_sharp/c_sharp_collections.html "Godot only understands its own collection types, so it\'s required to use them when talking to the engine." and "this conversion requires marshaling every element in the collection and copies it to a new collection so it can be expensive."',
    evidence_source=MG + 'MarshalUtils.cs:88-249 (closed type list; user structs -> null at :127-157; arrays only for listed element types at :158-208) | ' + MG + 'ExtensionMethods.cs:329-330 and :343-344 (methods using such types dropped silently), :365-378 (properties of such types skipped silently) | ' + MG + 'Common.cs:50-58 (GD0102 raised only for [Export])',
    frequency='godot-proposals#438 143 and #997 56 reactions | #70298 38 reactions (tied 10th most-reacted topic:dotnet issue)',
    status='by-design (documented limitation; .NET collection export was dropped in the .NET 6 move, PR #64089, per a maintainer in #70298)',
    fix_class='N/A (analog: L+T)',
    fix_reason='.NET types are absent in Petrichor, but its own structs, records and collections face the same rule: every type that can cross (export, signal, script method) needs a defined Variant representation and built-in collections can be the engine\'s Array/Dictionary/Packed*Array (L); the compiler rejects a crossing type that has none instead of silently dropping the member (T).')

MERGED['M6'] = dict(
    source_rows='CS-A-06, CS-B-04', group='C#',
    title='The editor reflects the last built assembly: new exports, signals, [GlobalClass] and tool changes need Build',
    description='The inspector, signal list, Create Resource/Add Node dialogs and tool scripts reflect the compiled, loaded assembly, not the source, so a saved .cs edit changes nothing in the editor until Build; Resource/Node subclasses appear in the creation dialogs only with [GlobalClass] (since 4.1) and after a successful build; classes whose base comes from another assembly, or files created by an external editor, have been missed until restart.',
    example='[GlobalClass] public partial class Stats : Resource { [Export] public int Hp { get; set; } }  -> absent from Create Resource and the Inspector until Build',
    root_layer='editor-toolchain',
    evidence_issues='#27470 (closed by PR #72619 [GlobalClass] in 4.1, 71 reactions, 45 comments) | #12518 (closed 2018, 10 reactions, 43 comments: new exports not shown until restart) | #116169 (closed by PR #117617 in 4.7, 12 reactions; duplicate #116812) | #95036 (open, 6 reactions, 7 comments: base class from another assembly -> not listed) | godot-proposals#2920 (open, 33 reactions: auto-rebuild when a C# script changes) | godot-proposals#849 (open, 15 reactions: background building)',
    evidence_docs=D + 'scripting/c_sharp/c_sharp_basics.html "You need to (re)build the project assemblies whenever you want to see new exported variables or signals in the editor." | "You will also need to rebuild the project assemblies to apply changes in "tool" scripts." | ' + D + 'scripting/c_sharp/c_sharp_global_classes.html "Global classes are registered with the [GlobalClass] attribute."',
    evidence_source='modules/mono/glue/GodotSharp/GodotPlugins/Main.cs:156 (script types are discovered by scanning the loaded compiled project assembly) | ' + GS + 'Bridge/ScriptManagerBridge.cs:202-217 (global class info read from the loaded assembly\'s path-to-type map; unknown path -> no class name) | ' + GS + 'Bridge/ScriptManagerBridge.cs:363-374 (editor asked to rescan script paths only after an assembly load)',
    frequency='#27470 71 reactions | 11 topic:dotnet issues have "GlobalClass" in the title | 48 reactions on the two auto-build proposals | recurring forum threads titled "custom resource not showing up in create new resource menu" (web search)',
    status='by-design (manual Build button); #27470 fixed in 4.1 (PR #72619); #95036 open',
    fix_class='T+W',
    fix_reason='If the language front end parses source on save, the editor sees declarations without a build: ScriptLanguageExtension::_get_global_class_name (core/object/script_language_extension.h:754) and ScriptExtension update_exports/_get_script_property_list/reload (:79, :176, :188) are GDExtension virtuals.')

MERGED['M7'] = dict(
    source_rows='CS-A-17, CS-B-18', group='C#',
    title='async/Task: not engine-visible, exceptions lost, continuations and threads leave the main thread',
    description='Task-returning methods are not Variant-compatible, so they are not exposed to GDScript or Callables and a GDScript await on a call into C# returns immediately; exceptions in fire-and-forget async methods are never reported; continuations return to the main thread only through a frame-polled SynchronizationContext, so Task.Run/ConfigureAwait(false) silently move scene access onto pool threads; worker-thread loading of C# scripts also deadlocked (fixed in 4.4/4.5).',
    example='public async Task LoadAsync()  -> not callable from GDScript | _ = LoadAsync(); // an exception inside LoadAsync is never printed',
    root_layer='.NET bridge',
    evidence_issues='#56468 (open, 3 comments: async signal handler -> "unmarshallable managed type ... Task`1") | #77624 (open, 5 comments: result of GodotObject.Call cannot be awaited) | #25015 (open: no errors reported from async functions) | #85234 (closed 2023-11: .NET by-design) | #90300 (open, 5 reactions: error flood when GodotObject.Set is called from a thread) | #99128 (closed by #99539 in 4.4 and #99798 in 4.5, 18 reactions, 37 comments: LoadThreadedRequest stuck at 0.5) | #104196 (closed 2025-10, 13 reactions, 12 comments)',
    evidence_docs=D + 'scripting/c_sharp/c_sharp_differences.html "An equivalent of awaiting a signal in GDScript can be achieved with the await keyword and GodotObject.ToSignal." | same page: "The await keyword in C# can be used with any awaitable expression." (no thread-affinity warning on the C# pages)',
    evidence_source=MG + 'ExtensionMethods.cs:324-330 (non-Variant return type such as Task -> method not exposed) | ' + GS + 'Callable.generics.cs:258 (Callable.From requires [MustBeVariant] TResult) | modules/gdscript/gdscript_vm.cpp:2572-2596 (GDScript await suspends only on a Signal or GDScriptFunctionState; any other value returns immediately) | ' + GS + 'GodotSynchronizationContext.cs:39-53 (Post only queues; ExecutePendingContinuations drains) | ' + GS + 'Bridge/ScriptManagerBridge.cs:78-82 and modules/mono/csharp_script.cpp:535-537 (drained once per frame from FrameCallback)',
    frequency='loader hangs: 31 reactions over #99128/#104196 | async-exception and await-interop reports recur since 2019, each with few reactions',
    status='mixed: async exceptions by-design (.NET); loader deadlocks fixed (4.4/4.5); interop and off-thread access open',
    fix_class='N/A (analog: L+W)',
    fix_reason='The .NET System.Threading.Tasks.Task is absent in Petrichor; the analog: its coroutines are single-threaded and resumed by the engine, propagate errors to the awaiter (L), and surface as a Signal at the Variant boundary so GDScript can await them (a ScriptExtension can return one, W); its loader must be safe on ResourceLoader worker threads.')

MERGED['M8'] = dict(
    source_rows='CS-B-11, CS-B-12', group='C#',
    title='Struct-typed properties and packed arrays come back as copies',
    description='Vector2/Transform3D etc. are C# structs, so Position.X = 5 is a compile error (CS1612) and modifying a local copy without reassigning it silently does nothing; packed arrays map to System.Array and every property read copies the whole native buffer into a new array, so line.Points[0] = v compiles but changes nothing and indexing a property in a loop copies the buffer on each access.',
    example='Position.X = 100f; // CS1612 -> var p = Position; p.X = 100f; Position = p; | line.Points[0] = new Vector2(10, 10); // no effect',
    root_layer='.NET bridge',
    evidence_issues='godot-proposals#7842 (open, 497 reactions, 97 comments: zero-allocation API; "Memory is copied to Godot native formats every time there is a call") | no engine issue for CS1612 (normal C# behaviour)',
    evidence_docs=D + 'scripting/c_sharp/c_sharp_basics.html "Structs (in this example, a Vector2) in C# are copied on assignment, meaning that when you retrieve such an object from a property or an indexer, you get a copy of it, not the object itself. Modifying said copy without reassigning it afterwards won\'t achieve anything." | ' + D + 'scripting/c_sharp/c_sharp_collections.html "In C#, packed arrays are replaced by System.Array" | doc/classes/PackedByteArray.xml:9 "In these cases the returned packed array is a copy, and changing it will not affect the original value."',
    evidence_source=GS + 'NativeInterop/Marshaling.cs:437-446 (allocates a new array and copies the native buffer on every conversion) | struct copy rule: C# language, no engine code',
    frequency='the only item in the C# basics "Common pitfalls" section | forum threads 2021-2025 (Line2D points, GlobalPosition, Transform) | proposal #7842 (497) concerns copies and allocations in general',
    status='by-design (documented)',
    fix_class='L',
    fix_reason='Define obj.prop.x = v and obj.prop[i] = v as get-modify-set along the property chain (GDScript already does this for value-type members) or reject them statically, and expose packed arrays as engine copy-on-write values without conversion.')

MERGED['M9'] = dict(
    source_rows='CS-B-15, CS-B-16', group='C#',
    title='Hidden per-call costs: string-to-StringName allocations, GC spikes, slow engine-to-script dispatch',
    description='Passing a string where a StringName/NodePath is expected allocates a finalizable wrapper plus a native StringName on every call, and APIs returning Godot collections allocate too, so one such call per frame can cause visible GC stutter; until 4.8 every engine-to-script callback (even an empty _Process) dispatched by string matching, so many scripted nodes cost far more than their script bodies.',
    example='Input.IsActionPressed("jump") // new StringName each call | 300 Buttons with an empty C# script: 1000 -> 400 FPS (#115960)',
    root_layer='.NET bridge',
    evidence_issues='godot-proposals#7842 (open, 497 reactions, 97 comments) | #105750 (open, 20 reactions, 20 comments: MRP with one StringName construction or GetNodesInGroup per frame) | godot-proposals#15089 (open, 24 reactions: cache StringName/NodePath conversions) | #89217 (closed by PR #116300 in 4.8, 36 reactions, 19 comments) | #98175 (closed not planned into #89217, 14 reactions) | #115960 (closed 2026-10-14, 5 reactions) | #96435 (open, 16 comments: C# about 5x slower in Godot than in a console app)',
    evidence_docs=D + 'scripting/c_sharp/c_sharp_basics.html "The implicit conversion from string to NodePath or StringName incur both the native interop and marshalling costs as the string has to be marshalled and passed to the respective native constructor." | "Prefer using the exposed StringName in the PropertyName, MethodName and SignalName to avoid extra StringName allocations"',
    evidence_source=GS + 'StringName.cs:81 (implicit operator StringName(string from) => new StringName(from)) | ' + GS + 'StringName.cs:68-75 (native StringName created, object registered in DisposablesTracker) | ' + GS + 'StringName.cs:22-25 (finalizer) | ' + GS + 'NodePath.cs:135 (same for NodePath) | ' + GS + 'Bridge/CSharpInstanceBridge.cs:10-25 (each ScriptInstance call dispatched by StringName through InvokeGodotClassMethod)',
    frequency='#7842 497 reactions | #105750 20 | #15089 24 | 55 reactions across #89217/#98175/#115960',
    status='allocations open (workaround: cache StringName, use generated SignalName/MethodName); dispatch fixed in 4.8 (PR #116300)',
    fix_class='L+T',
    fix_reason='Type such parameters as StringName, intern literals at compile time and warn on run-time String->StringName conversions in hot paths (L+T); without a tracing GC there is no collection pause to trigger; the script-instance call callback receives a StringName, so methods can be resolved once per class (W).')

MERGED['M10'] = dict(
    source_rows='CS-B-01, CS-B-02', group='C#',
    title='Export platform limits: no web export; mobile experimental; NuGet native libraries not packaged',
    description='Godot 4 cannot load the .NET runtime into its WASM build, so a C# project has no web export at all (Godot 3 Mono could); Android/iOS export arrived only in 4.2 and is still experimental (iOS only from macOS); native libraries from NuGet packages are not copied into Android builds and fail with DllNotFoundException.',
    example='SQLite.NET on Android -> System.DllNotFoundException: e_sqlite3 (#97859)',
    root_layer='.NET bridge',
    evidence_issues='#70796 (open/reopened, 103 reactions, 116 comments; locked 2026-06-19 by a maintainer: "The foundation is evaluating solutions for C# web support") | PR #106125 "[.NET] Add web export support" (open draft, 451 reactions) | #68153 (closed for 4.2, 234 reactions, 26 comments: C# on Android) | #97859 (open, 8 reactions, 15 comments; still reported on 4.5 in 2025-11) | #84559 (closed 2024-09, 8 reactions, 25 comments: Android SSL crash with HttpClient)',
    evidence_docs=D + 'export/exporting_for_web.html "Projects written in C# using Godot 4 currently cannot be exported to the web." and, for GDExtensions: "Note that GDExtensions still need to be specifically compiled for the web platform to work. Like thread support, enabling this feature requires the use of cross-origin isolation headers." | ' + D + 'scripting/c_sharp/index.html "Android support is currently experimental." | "iOS support is currently experimental and has a few limitations." | "Exporting to iOS can only be done from a macOS device."',
    evidence_source='unverified (the blocker is loading the .NET WASM runtime, outside modules/mono; a maintainer in #70796 points to dotnet/runtime#75257)',
    frequency='#68153 is the most-reacted topic:dotnet issue (234) and #70796 the 2nd (103); the draft web PR has 451 reactions',
    status='web: open (no C# web export in 4.x); mobile: experimental since 4.2; #97859 open',
    fix_class='N/A (analog: W)',
    fix_reason='Absent without .NET; the analog: Petrichor\'s runtime must target every platform including WASM. Compiled in as a module it ships inside the export templates; as a GDExtension the web export needs the Extensions Support option, and cross-origin isolation headers only when thread support is also enabled (platform/web/doc_classes/EditorExportPlatformWeb.xml:93-95; the tutorial page above says GDExtension support itself requires them). This is a packaging difference between module and GDExtension, not a missing API (inference).')

MERGED['M2'] = dict(
    source_rows='EG-C-14, CS-A-02', group='engine/GDScript',
    title='class_name and [GlobalClass] live in one flat global namespace',
    description='Every class_name is registered in a single project-wide namespace shared with native classes, autoloads, every addon and every language; two addons using the same name, or a user class whose name a later engine version adds natively, fail to parse ("hides a global script class" / "hides a native class"), and stale script-class cache entries after renames produce the same error; C# [GlobalClass] registers the bare type name with its namespace dropped, and the registry updates an existing entry in place without a collision check.',
    example='# addons/a/state_machine.gd: class_name StateMachine ⏎ # addons/b/state_machine.gd: class_name StateMachine  # Parse Error: Class "StateMachine" hides a global script class. | C#: namespace A { [GlobalClass] partial class Item : Resource {} } and namespace B { [GlobalClass] partial class Item : Resource {} }',
    root_layer='engine core',
    evidence_issues='godot-proposals#1566 "Implement namespaces in GDScript" (open, 363 reactions, 56 comments) | godot-proposals#4493 (open, 43 reactions, 22 comments: split class_name responsibilities) | #67056 (closed by PR #90186 in 4.3, 22 reactions, 34 comments: "hides a global script class" after renames) | #78642 (closed, 11 reactions: invalid global_script_class_cache entries) | #99766 class_name tracker (open, 17 reactions) | #95406 (open, C#: [GlobalClass] named like a built-in class hidden from pickers, no warning)',
    evidence_docs=D + 'scripting/gdscript/gdscript_basics.html (Registering named classes) "Named classes are globally registered, which means they become available to use in other scripts without the need to load or preload them" | ' + D + 'scripting/c_sharp/c_sharp_global_classes.html "The MyNode type will be registered as a global class with the same name as the type\'s name."',
    evidence_source='modules/gdscript/gdscript_analyzer.cpp:400-407 (errors: hides a built-in type / native class / global script class / autoload singleton) | core/object/script_language.cpp:408-431 (ScriptServer::add_global_class: one StringName-keyed map for all languages; an existing entry is updated in place, no ClassDB check) | ' + GS + 'Bridge/ScriptManagerBridge.cs:279 (C# global name = scriptType.Name, namespace dropped) | core/object/script_language_extension.h:754 (_get_global_class_name: how an extension language registers its global names)',
    frequency='GraphQL search for "hides a global script class": 39 issues/PRs (many cache bugs); forum.godotengine.org exact phrase: 28 topics; #1566 is the 10th most-reacted topic:gdscript proposal; C# collision reports: 1 (#95406)',
    status='open (namespaces); cache bugs fixed in 4.3 (PR #90186)',
    fix_class='L (registry part: E)',
    fix_reason='The language can have modules/namespaces, export only chosen names globally, map qualified names to unique global names and have its compiler reject collisions against ClassDB and the global class cache; anything registered for the editor, inspector or other languages still goes into ScriptServer\'s flat table, so true namespaces there need an engine change.')

MERGED['M11'] = dict(
    source_rows='EG-C-07, EG-B-10', group='engine/GDScript',
    title='Typed arrays are invariant and never convert implicitly; many APIs return untyped Array',
    description='Array[T] carries its element type at run time and assignment requires an exact match: Array[Node2D] cannot be assigned to Array[Node], and untyped arrays (Array.map results, range(), JSON data, literals through a ternary) cannot be assigned to Array[int] even when every element fits; the analyzer accepts untyped-to-typed as merely unsafe, so the failure is a run-time error; `as Array[T]` does not convert; a wrong-typed append only prints an error and drops the element.',
    example='var a: Array[int] = [1, 2, 3] ⏎ var b: Array[int] = a.map(func(x): return x * 2)  # runtime: Trying to assign an array of type "Array" to a variable of type "Array[int]"',
    root_layer='GDScript language',
    evidence_issues='#72566 (open, 93 reactions, 18 comments; duplicates #73309, #82538, #100377) | #54311 (open, confirmed, 8 reactions; duplicate #72627 with 39 reactions) | #86851 (open, 10 reactions, 15 comments: wrong-type append drops the element without a script error) | #72762 (open, 13 reactions: range() should return Array[int]) | #83026 (open, 10 reactions: empty array through ternary) | #59721 (open, 9 reactions) | godot-proposals#13201 (open, 7 reactions: built-ins should return typed arrays)',
    evidence_docs=D + 'scripting/gdscript/gdscript_basics.html (Typed arrays) "you cannot assign an array with a different element type, even if the type is a subtype of the required type." | "The only exception was made for the Array (Array[Variant]) type, for user convenience and compatibility with old code. However, operations on untyped arrays are considered unsafe." | https://docs.godotengine.org/en/latest/classes/class_array.html is_typed: "The methods of a typed array are still expected to return a generic Variant."',
    evidence_source='modules/gdscript/gdscript_analyzer.cpp:6428-6432 (typed element types must be equal) and :6388 (typed vs untyped compatible but unsafe) | modules/gdscript/gdscript_vm.cpp:1463-1468 (OPCODE_ASSIGN_TYPED_ARRAY requires an identical element type, else error) | core/variant/array.cpp:648-650 (map builds an untyped Array) vs :619-622 (filter copies the source typing) | core/variant/array.cpp:280-285 (push_back: failed validation prints and returns) | core/variant/container_type_validate.h:37-47 (element type identity = variant_type + class_name + script)',
    frequency='#72566 is the 6th most-reacted topic:gdscript issue (GraphQL dump of all 4377 such issues) | #72627 39 reactions | 131 issues with "typed array" in the title | forum exact phrase "Trying to assign an array of type": 9 topics',
    status='open (#72566, #54311; PR #124662 open)',
    fix_class='L+T (Variant part: E)',
    fix_reason='The language can type map/filter/range generically, insert checked converting copies (Array.assign) where engine methods return untyped Array, and make covariant or mismatched assignments compile errors; the run-time element-type tag and its exact-match check are Array semantics that every language meets when typed arrays cross the engine boundary.')

MERGED['M12'] = dict(
    source_rows='EG-C-19, EG-B-11', group='engine/GDScript',
    title='No generics; typed collections only one level deep',
    description='Only Array[T] and Dictionary[K, V] (4.4+) take type parameters, and only one level: Array[Array[int]], Dictionary[String, Array[int]], typed Callable/Signal signatures and user-defined generic classes are unsupported, so nested data (grids, maps of lists) is Variant inside and every inner access is unchecked; the engine container type stores a single element type, and typed Dictionary exports have inspector gaps.',
    example='var grid: Array[Array[int]]  # parse error: Nested typed collections are not supported.',
    root_layer='GDScript language',
    evidence_issues='godot-proposals#12224 (open, 260 reactions: nested typed arrays and dictionaries; engine PR #107556 open) | godot-proposals#1207 (open, 241 reactions, 72 comments: generic parameters) | godot-proposals#10807 (open, 141 reactions: statically typed Callable) | godot-proposals#2557 (open, 134 reactions: typed signal parameters) | godot-proposals#56 (closed, implemented by PR #78656 in 4.4, 836 reactions: typed Dictionary) | #104581 (open, 21 reactions: exported typed Dictionary key cannot be changed in the inspector)',
    evidence_docs=D + 'scripting/gdscript/gdscript_basics.html (Typed arrays) "Nested array types (like Array[Array[int]]) are not supported." | ' + D + 'scripting/gdscript/static_typing.html "Nested typed collections (like Dictionary[String, Dictionary[String, int]]) are not supported."',
    evidence_source='modules/gdscript/gdscript_parser.cpp:3879 ("Nested typed collections are not supported.") | core/variant/container_type_validate.h:37-40 (engine element type = variant_type + class_name + script, one level)',
    frequency='the five proposals total 1612 reactions; #56 was the 3rd most-reacted GDScript proposal before it shipped',
    status='open (#12224, #1207; PR #107556 open)',
    fix_class='L+T (engine-visible part: E)',
    fix_reason='The language can have statically checked generics and erase them to one-level or untyped engine containers at run time; making nested element types visible to the engine (run-time validation, inspector, serialization, other languages) needs ContainerType to carry nested types, a core change (PR #107556).')

MERGED['M13'] = dict(
    source_rows='EG-A-18, EG-A-19', group='engine/GDScript',
    title='Timers: create_timer ignores pause by default; Timer nodes are frame-quantized and fail outside the tree',
    description='get_tree().create_timer(t) defaults to process_always=true, so `await get_tree().create_timer(1).timeout` continues while the tree is paused and is not tied to the awaiting node\'s process_mode; Timer nodes subtract delta once per frame and emit at most one timeout per frame (sub-frame wait times are quantized), start() before the node is in the tree only prints an error, and timers scale with Engine.time_scale.',
    example='get_tree().paused = true; await get_tree().create_timer(0.2).timeout  -> resumes while paused | t.wait_time = 0.001  -> one timeout per frame, not 1000 per second',
    root_layer='engine core',
    evidence_issues='godot-proposals#4908 (open, 46 reactions, 22 comments: add Node.create_timer) | godot-proposals#9924 (open, 16 reactions: make create_timer\'s process_always default to false; "a source of headaches and bugs over and over again") | #7656 (closed: create_timer() doesn\'t pause) | godot-proposals#10879 (closed not planned, 5 reactions, 12 comments: frame option for Timer) | godot-proposals#3386 (open, 5 reactions) | #107203 (open, 6 comments: physics Timer with ignore_time_scale)',
    evidence_docs='https://docs.godotengine.org/en/latest/classes/class_scenetree.html (create_timer) "If process_always is false, the timer will be paused when setting SceneTree.paused to true." | https://docs.godotengine.org/en/latest/classes/class_timer.html "Timers can only process once per physics or process frame (depending on the process_callback). An unstable framerate may cause the timer to end inconsistently, which is especially noticeable if the wait time is lower than roughly 0.05 seconds." | (start) "Fails if the timer is not inside the scene tree."',
    evidence_source='scene/main/scene_tree.cpp:1942 (create_timer bound with DEFVAL(true) for process_always) | scene/main/timer.cpp:60-67 (a single if: at most one timeout per tick) | scene/main/timer.cpp:120 (start fails when not inside the tree) | scene/main/timer.cpp:75-76 (physics ticking with ignore_time_scale uses the process step)',
    frequency='#4908 46 reactions | #9924 16 reactions | Timer quantization documented; low GitHub signal',
    status='by-design (documented); proposals open; #107203 open',
    fix_class='W (Timer node behavior: E)',
    fix_reason='A standard-library wait/timer can default to pause-aware, owner-bound timing (public create_timer(t, false), Timer, Tween), catch up missed ticks and queue start until enter-tree, cancelled with its owner; the engine Timer node itself stays as is.')

MERGED['M14'] = dict(
    source_rows='EG-C-09, EG-C-10', group='engine/GDScript',
    title='Variant equality is inconsistent: == across unrelated types is a run-time error, while match, Dictionary keys and "in" are type-strict',
    description='"==" between unrelated types (int vs String, Object vs bool) raises "Invalid operands" at run time and aborts the function in untyped code, while some mixed pairs (int/float, String/StringName, anything vs null) compare; meanwhile match patterns, Dictionary key lookup, Array.has/find/erase and "in" compare the type first, so 1 == 1.0 is true but 1 does not match 1.0, d[2.0] misses a key stored as 2, and JSON numbers (floats) miss int keys.',
    example='var id = data.get("id")  # sometimes String ⏎ if id == 0: pass  # Invalid operands \'String\' and \'int\' in operator \'==\' | var d := {2: "a"} ⏎ print(2 == 2.0, d.has(2.0))  # true false',
    root_layer='engine core',
    evidence_issues='#26375 (closed, 8 reactions; 11 cross-referencing issues: compare variables of different types) | godot-proposals#4571 (closed by PR #71758, which added is_same() instead of changing ==; 26 comments) | #48150 (closed, fixed in 4.0: null vs int) | #82575 (open: Dictionary float/int key confusion) | #71559 (open: JSON numbers and "in") | #80162 (open) | #64171 (closed by PR #68747 "Unify StringName and String" in 4.0, 15 reactions) | #60145 (closed by PR #68747)',
    evidence_docs=D + 'scripting/gdscript/gdscript_basics.html (Operators) "The == and != operators sometimes allow you to compare values of different types (for example, 1 == 1.0 is true), but in other cases it can cause a runtime error. If you\'re not sure about the types of the operands, you can safely use the is_same() function" | same page (match) "match is more type strict than the == operator. For example 1 will not match 1.0. The only exception is String vs StringName matching"',
    evidence_source='core/variant/variant_op.cpp:489-493 (OP_EQUAL registered for INT/INT, INT/FLOAT, FLOAT/INT and string-like pairs; no INT/STRING) | core/variant/variant_op.cpp:1051-1055 (missing evaluator -> r_valid = false) | modules/gdscript/gdscript_vm.cpp:756-761 ("Invalid operands ... in operator", function aborts) | core/variant/variant.cpp:3213-3216 (hash_compare false when types differ) | core/variant/variant.cpp:3437-3447 (StringLikeVariantComparator: only String/StringName cross-type) | core/variant/dictionary.cpp:46 (Dictionary keyed with StringLikeVariantComparator) | core/variant/array.cpp:394 (Array::find) | core/variant/variant_op.h:1195 ("x in array" is find() != -1) | modules/gdscript/gdscript_compiler.cpp:1455-1479 (match literal tests typeof first)',
    frequency='forum exact phrase "in operator \'==\'": 18 topics; "Invalid operands" (all operators): 117 topics; Stack Overflow: 6 questions | key/match strictness: 5 low-reaction issues 2022-2023',
    status='by-design, documented (is_same() added in 4.0, PR #71758)',
    fix_class='L+T (Variant part: E)',
    fix_reason='With static types the compiler rejects comparisons, keys and patterns that mix unrelated or int/float types, and the language can define its own == on "any" and its own match equality; Variant::evaluate and Dictionary/Array hashing and equality remain engine semantics every language observes inside engine containers.')

MERGED['M15'] = dict(
    source_rows='EG-C-05, EG-C-20', group='engine/GDScript',
    title='Gradual typing leaks: Variant flows into typed code unchecked, UNSAFE_* warnings are off by default, false positives push users to disable checks',
    description='Typing is opt-in per declaration and any Variant-typed expression (untyped variable, untyped Array/Dictionary element, Variant-returning engine method such as Object.get or Array.front) is accepted where a concrete type is expected and checked only at run time; the warnings that would flag this are ignored by default and do not cover all unsafe lines, while several warnings fire on correct code (static function parameter "shadows" a member, UNSAFE_* on autoloads, INTEGER_DIVISION after an explicit cast), so users silence whole categories.',
    example='var cfg := {"hp": 10} ⏎ var hp: String = cfg["hp"]  # compiles (Variant source); fails at run time',
    root_layer='GDScript language',
    evidence_issues='godot-proposals#173 force static typing (closed by PR #81355 adding the optional UNTYPED_DECLARATION warning in 4.2, 65 reactions, 34 comments) | #63120 (open, confirmed, 28 reactions, 15 comments; duplicates #101840, #107572) | #42966 (open, 60 reactions: integer-division warning after a cast) | #56592 (closed by PR #83037 in 4.3, 14 reactions: @warning_ignore did not cover all warnings) | #60499 (open, 11 reactions: false-positive UNSAFE_PROPERTY_ACCESS) | #86300 (open, 12 reactions, 15 comments: UNSAFE_METHOD_ACCESS for autoloads) | #59721 (open, 9 reactions: typed arrays lose their type through Variant-returning functions)',
    evidence_docs=D + 'scripting/gdscript/static_typing.html (Warning system) "We have a number of warnings aimed specifically at users of typed GDScript. By default, these warnings are disabled" | "Currently, UNSAFE_* warnings do not cover all cases that unsafe lines cover." | gdscript_basics.html (Arrays) "array methods like front() and back() still have the Variant return type"',
    evidence_source='modules/gdscript/gdscript_analyzer.cpp:6414-6416 (a Variant source is compatible with any target: "This is acceptable but unsafe") | modules/gdscript/gdscript_warning.h:124-129 (UNTYPED_DECLARATION, INFERRED_DECLARATION and UNSAFE_* default to IGNORE) | modules/gdscript/gdscript_analyzer.cpp:6173-6208 (is_shadowing ignores whether the enclosing function is static) | modules/gdscript/gdscript_analyzer.cpp:3161-3169 (INTEGER_DIVISION decided purely from operand static types)',
    frequency='#173 65 reactions | the false-positive issues total 125 reactions with 3 duplicates marked | forum exact phrase "UNSAFE_PROPERTY_ACCESS": 6 topics',
    status='by-design (gradual typing); opt-in UNTYPED_DECLARATION since 4.2; #63120, #60499, #86300, #42966 open',
    fix_class='T+L',
    fix_reason='A statically typed language makes "any" explicit and requires a checked conversion where it meets a concrete type, so fewer heuristic "unsafe" warnings are needed; Variant-returning engine methods need modelled return types, which is toolchain data from extension_api.json, not an engine change.')

MERGED['M16'] = dict(
    source_rows='EG-A-10, EG-C-02, EG-A-11', group='engine/GDScript',
    title='Leaks without a collector: RefCounted cycles (incl. self-capturing lambdas) and orphan Nodes',
    description='RefCounted objects that reference each other, or that store a Callable/lambda/connection bound to themselves (a self-using lambda in a RefCounted script keeps a strong Ref to its owner), are never freed; Node is not reference-counted, so a node that is removed with remove_child()/replace_by() or never parented leaks when its last variable drops; leaks show only as object or orphan-node counts or "ObjectDB instances leaked at exit"; GDScript has only untyped weakref().',
    example='a.other = b; b.other = a | extends RefCounted ⏎ signal changed ⏎ func _init(): changed.connect(func(): print(self))  # never freed | remove_child(n); n = null  # leaked',
    root_layer='engine core',
    evidence_issues='#7038 (open since 2016, 8 reactions, 18 comments: leak with circular references) | #102327 (open, 4 reactions, 7 comments: self-capturing lambdas leak; a commenter is "removing almost all lambdas from the codebase") | #81210 (open, 8 reactions, 19 comments) | #121618 (open: chained awaits leak through cycles) | #97523 (open, confirmed: method callables do not keep self alive) | godot-proposals#9174 (open, 35 reactions: typed WeakRefs) | godot-proposals#540 (open, 17 reactions: better orphan-node and leak tracking) | #16815 (closed, 10 comments: warn about orphan nodes) | #47581 (open: orphan node count wrong)',
    evidence_docs='https://docs.godotengine.org/en/latest/classes/class_refcounted.html "RefCounted instances caught in a cyclic reference will not be freed automatically." | https://docs.godotengine.org/en/latest/classes/class_node.html (remove_child) "The node, along with its children, are not deleted. To delete a node, see queue_free." | (replace_by) "The replaced node is removed from the tree, but it is not deleted. To prevent memory leaks, store a reference to the node in a variable, or use Object.free."',
    evidence_source='modules/gdscript/gdscript_lambda_callable.cpp:285-291 (self lambda on a RefCounted owner stores a strong Ref<RefCounted>) | modules/gdscript/gdscript_lambda_callable.cpp:296-301 (other owners stored as raw Object*) | modules/gdscript/gdscript_lambda_callable.cpp:106-112 (a freed capture is replaced by null with an error) | core/object/ref_counted.cpp:67 (get_reference_count is bound) | doc/classes/Node.xml:835 (remove_child does not delete) | absence of cycle detection: stated by the docs, no single line',
    frequency='proposal #9174 35 reactions | #7038 18 comments over 10 years | proposal #540 17 reactions | lambda-leak reports recur (4 found by title search)',
    status='by-design (documented); #102327, #97523 open',
    fix_class='L+W (Node ownership model: E)',
    fix_reason='The language can add typed weak references and explicit capture lists (weak self by ObjectID in its own CallableCustom), flag self-capturing closures stored on RefCounted and Nodes that are created or removed but never parented or freed on some path (T), and track or collect cycles among objects whose fields it owns; making Node reference-counted would be an engine change.')

MERGED['M17'] = dict(
    source_rows='EG-B-16, EG-B-17', group='engine/GDScript',
    title='Node references are strings resolved at run time: renames and moves break $paths silently; %UniqueName resolves only via the owner scene',
    description='$A/B, get_node("A/B") and NodePath literals are not tied to the scene file: the editor rewrites node paths stored in the edited scene on rename but not paths written in scripts, so a rename or reparent appears only as a run-time "Node not found" error and null; %Name is looked up in the calling node\'s own unique-name map or its owner\'s, so nodes instantiated at run time (no owner) get null, and from an instanced sub-scene root a missing name silently resolves to a same-named unique node of the outer scene; Resources cannot hold Node references.',
    example='@onready var hp_bar = $UI/HPBar  # after moving HPBar under UI/Top: null at run time | @onready var spawner = %Spawner  # in a scene spawned at run time: null',
    root_layer='GDScript language',
    evidence_issues='godot-proposals#8465 (closed, 12 reactions: make $ and % use get_node_or_null) | #80538 (closed, 14 comments: exported Node in a custom Resource) | #76714 (open) | #107678 (open, 6 comments: unique-name search finds a node outside the sub-scene) | #92679 (closed 2025-02, 18 comments: runtime-instanced nodes get null for %) | #117871 (closed) | #76614 (closed) | godot-proposals#14675 (open, 4 reactions: refactor when renaming a unique name)',
    evidence_docs=D + 'scripting/scene_unique_nodes.html "If you move a button in a UI scene from one panel to another, the button\'s node path changes, and if a script uses get_node() with a hard-coded node path, the script will not be able to find the button anymore." | same page: "A scene unique node can only be retrieved by a node inside the same scene." | https://docs.godotengine.org/en/latest/classes/class_node.html (get_node) "If path does not point to a valid node, generates an error and returns null."',
    evidence_source='scene/main/node.cpp:1984-1994 (get_node: run-time "Node not found" error and null) | scene/main/node.cpp:1960-1968 (% lookup: own owned_unique_nodes, then the owner\'s map, else null) | editor/docks/scene_tree_dock.cpp:2107 and :2280 (_update_node_path/perform_node_renames rewrite node paths in the edited scene; no script-source edits found in that file by grep) | modules/gdscript/gdscript_parser.cpp:4858 (Node exports only in Node-derived classes)',
    frequency='one web search ("godot 4 Node not found after renaming node") returned 6+ forum threads titled "Node not found" in the top 9 | 26 issues with "Node not found" in the title | 40 issues with "unique name" in the title (each <=3 reactions)',
    status='by-design (documented); #107678 open',
    fix_class='T+L',
    fix_reason='A toolchain that reads the .tscn owning each script can check every path literal and %Name, reject uses from run-time-instanced scripts or ones that hit the outer-scene fallback, and rewrite paths on rename; the language can expose typed scene members generated from the scene instead of strings.')

MERGED['M18'] = dict(
    source_rows='EG-C-04, EG-C-06', group='engine/GDScript',
    title='"as" casts silently yield null and "is" checks do not narrow the type',
    description='For object types, x as T returns null instead of failing when x is another class, so a wrong cast surfaces later and elsewhere as a null-instance error (a built-in target type raises an error instead, and "as Array[T]" does not convert); inside "if x is Player:" the analyzer still types x as its declared type, so member access in the branch is unchecked unless the user adds a redundant cast.',
    example='var player := body as Player  # null when body is an Enemy; the error appears only at player.hp | if b is Player: b.take_damage(1)  # not type-checked',
    root_layer='GDScript language',
    evidence_issues='#54311 (open, confirmed, 8 reactions: as with typed arrays; fix PR #124662 open) | #72627 (closed as duplicate of #54311, 39 reactions) | godot-proposals#8530 (open, 26 reactions: ignore unsafe access inside is blocks; duplicates #14632, #14804) | godot-proposals#6941 (open, 46 reactions, 19 comments: safe type narrowing via match) | #93510 (closed not planned) | #111923 (closed, consolidated into #6941)',
    evidence_docs=D + 'scripting/gdscript/static_typing.html "The as keyword silently casts the variable to null in case of a type mismatch at runtime, without an error/warning. While this may be convenient in some cases, it can also lead to bugs." | same page: "If you try to cast with a built-in type and it fails, Godot will throw an error." | gdscript_basics.html (Casting) "If the value is not a subtype, the casting operation will result in a null value."',
    evidence_source='modules/gdscript/gdscript_vm.cpp:1665-1666 (OPCODE_CAST_TO_NATIVE: invalid cast assigns NULL) | modules/gdscript/gdscript_vm.cpp:1714-1717 (OPCODE_CAST_TO_SCRIPT: same) | modules/gdscript/gdscript_vm.cpp:1630-1637 (OPCODE_CAST_TO_BUILTIN errors, only under DEBUG_ENABLED) | modules/gdscript/gdscript_analyzer.cpp:5264-5303 (reduce_type_test types "is" as bool; the operand is never refined in the guarded block)',
    frequency='the docs carry an explicit "can lead to bugs" note | narrowing proposals total 72 reactions with 2 duplicates | #72627 39 reactions',
    status='by-design (object casts); proposals open; #54311 open',
    fix_class='L+T',
    fix_reason='The language can split a checked cast (fails loudly) from a conditional cast returning T? that the checker forces to be null-checked, and narrow types after "is" (flow-sensitive typing); the run-time type test already exists.')

MERGED['M19'] = dict(
    source_rows='EG-B-19, EG-C-16', group='engine/GDScript',
    title='Cyclic references: scenes and resources that reference each other fail to load; script cycles were fixed only for type references',
    description='An @export PackedScene or preload cycle (two levels whose doors point at each other) makes the loader return null or crash, often without a useful message, and resources cannot reference each other through exported properties; cycles between class_name type references were fixed by the 4.0 analyzer rewrite, but preload, autoload-plus-preload ordering and scene/resource cycles still fail.',
    example='level1.tscn: @export var next: PackedScene = level2.tscn / level2.tscn: next = level1.tscn',
    root_layer='engine core',
    evidence_issues='#24146 (open, 39 reactions, 34 comments: export PackedScene crash with circular references; reproduced on 4.6.2 in 2026-05; 6 issues reference it as duplicate) | godot-proposals#7363 (open, 110 reactions: allow circular references for resources in exports) | #80981 (open, 11 reactions) | #102073 (open, 7 reactions: cyclic scene preloading) | #21461 (closed by PR #67714 in 4.0, 110 reactions, 81 comments: class_name cyclic errors) | #80877 cyclic dependencies tracker (open, 23 reactions; 52 linked issues, 30 unchecked) | #80645 (open, 5 reactions: autoload + preload ordering)',
    evidence_docs='unverified (no docs statement found)',
    evidence_source='core/io/resource_loader.cpp:1049-1056 (a load re-entered on the same thread is treated as cyclic and returns null with ERR_BUSY) | script-level cycle handling: unverified (not traced)',
    frequency='#24146 39 reactions + 6 duplicates | proposal #7363 110 reactions | #21461 110 reactions (5th most-reacted topic:gdscript issue) | forum exact phrase "Cyclic reference": 39 topics (mixed with RefCounted cycles)',
    status='open (#24146, #80877); class_name type cycles fixed in 4.0 (PR #67714)',
    fix_class='T+L (loader cycles: E)',
    fix_reason='A toolchain can detect reference cycles across .tscn/.tres/scripts before run time, and a language that resolves a module\'s scripts together can allow mutually recursive types and offer lazy scene references (path/uid loaded on first use) instead of eager preload/PackedScene exports; real load cycles need ResourceLoader changes.')

MERGED['M20'] = dict(
    source_rows='EG-B-12, EG-B-13', group='engine/GDScript',
    title='@tool model: non-tool scripts are inert placeholders in the editor; tool scripts run everything and their edits are saved without undo',
    description='Without @tool a script gets a placeholder instance in the editor (no code, setters or getters run; only exported values are kept), extending a @tool script without @tool turns tool behaviour off, and a tool script calling into non-tool nodes fails confusingly; with @tool, _ready/_process/_notification and every exported setter run while editing (setters fire during scene load before children exist), so gameplay code must be fenced with Engine.is_editor_hint(), and whatever the script changes is saved with the scene with no undo, while freeing edited nodes can crash the editor.',
    example='@tool extends Node ⏎ func _ready(): $Child.setup()  # Child lacks @tool: placeholder, call fails | @tool ⏎ func _process(d): position.x += 1  # drifts in the editor; drifted value saved',
    root_layer='GDScript language',
    evidence_issues='godot-proposals#754 (closed 2024-08, 34 reactions: warn if a script inheriting a tool script is not tool; MISSING_TOOL warning added) | godot-proposals#3354 (open, 6 reactions) | #36592 (closed, 9 reactions: better error when calling non-tool scripts) | godot-proposals#900 (open, 22 reactions: separate tool and plugin code) | godot-proposals#250 (open, 7 reactions: protect against script-triggered node deletion in tool mode) | godot-proposals#9639 (open, 6 reactions) | #5983 (open, 37 reactions: improve tool script debugging)',
    evidence_docs=D + 'plugins/running_code_in_the_editor.html "The general rule is that any other GDScript that your tool script uses must also be a tool." | "Extending a @tool script does not automatically make the extending script a @tool." | "Modifications in the editor are permanent, with no undo/redo possible." | "Be extremely cautious when manipulating the scene tree, especially via Node.queue_free, as it can cause crashes if you free a node while the editor runs logic involving it." | gdscript_exports.html "getters and setters will only be used if the script is in Tool mode."',
    evidence_source='core/object/object.cpp:1066-1071 (can_instantiate() false in the editor -> placeholder_instance_create; true -> real instance, all code runs) | modules/gdscript/gdscript.cpp:265-267 (GDScript can_instantiate requires tool while scripting is disabled in the editor) | modules/gdscript/gdscript_analyzer.cpp:456-457 (MISSING_TOOL warning) | scene/resources/packed_scene.cpp:1061-1077 (non-default values left by tool code are packed on save)',
    frequency='proposal #754 34 reactions; #5983 37; #900 22 | 250 issues with "tool script" in the title | one web search on tool setters running before _ready returned 7+ forum threads in the top 10',
    status='by-design (documented); MISSING_TOOL warning added',
    fix_class='L+T',
    fix_reason='Whether and what a script runs in the editor is decided by the language (ScriptExtension _can_instantiate, _is_tool, _placeholder_instance_create), so editor execution can be inherited, per member or limited to explicit editor entry points, and calls from editor code into runtime-only code can be checked statically; undo integration stays editor-side.')

# Final order. Raw ids are carried over unchanged; 'M*' keys refer to MERGED.
ORDER_ENGINE = [
    'EG-A-01', 'EG-A-02', 'EG-A-03', 'EG-A-04', 'EG-A-05', 'EG-A-06',
    'EG-A-07', 'EG-A-08', 'EG-A-09', 'M16', 'EG-A-12', 'EG-A-13', 'EG-A-14',
    'EG-A-15', 'EG-A-16', 'EG-A-17', 'M13', 'EG-A-20',
    'EG-B-01', 'EG-B-02', 'EG-B-03', 'EG-B-04', 'EG-B-05', 'EG-B-06',
    'EG-B-07', 'EG-B-08', 'EG-B-09', 'M11', 'M20', 'EG-B-14', 'EG-B-15',
    'M17', 'EG-B-18', 'M19', 'EG-B-20',
    'EG-C-01', 'EG-C-03', 'M18', 'M15', 'EG-C-08', 'M14', 'EG-C-11',
    'EG-C-12', 'EG-C-13', 'M2', 'EG-C-15', 'EG-C-17', 'EG-C-18', 'M12',
]
ORDER_CS = [
    'M1', 'CS-A-04', 'M6', 'CS-A-07', 'CS-A-08', 'CS-A-09', 'CS-A-10',
    'CS-A-11', 'M4', 'CS-A-13', 'M5', 'CS-A-15', 'CS-A-16', 'M7', 'CS-A-18',
    'M3', 'CS-A-20', 'CS-A-21',
    'M10', 'CS-B-03', 'CS-B-05', 'CS-B-06', 'CS-B-07', 'CS-B-09', 'CS-B-10',
    'M8', 'CS-B-14', 'M9', 'CS-B-17', 'CS-B-20', 'CS-B-21',
]
