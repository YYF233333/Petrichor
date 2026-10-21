# Survey: How often is the result of awaiting a multi-parameter signal used?

**2026-10-22** · Related: [language specification](../language-spec.en.md) §9 · 中文：[await-signal-results.zh.md](await-signal-results.zh.md)

This is a record of a small empirical survey for one open choice in §9 of the specification: what awaiting a signal with several parameters produces. As in the [literal-local survey](literal-local-constraints.en.md), we first measured how often the case occurs in real code, then checked what each candidate rule would do to that code.

## The question

In GDScript, awaiting a signal yields null when the signal has no arguments, the argument when it has one, and an Array of the arguments when it has several (`modules/gdscript/gdscript_function.cpp:237-255` at Godot `469659e`; specification Appendix A). The language types the result instead: awaiting a static signal with no parameters produces no value, and awaiting one with one parameter produces a value of that parameter's type. For a static signal with several parameters there were three candidate rules:

| Rule | What `await s` produces when `s` has several parameters | Origin |
|---|---|---|
| A: statement only | No value; the await can only be a statement | §9 as currently written |
| B: an Array | An `Array[Variant]` of the arguments | GDScript; already rejected in Appendix B |
| C: binding by parameter | An await-only form, `var (result, code, headers, body) = await http.request_completed`, with each variable typed from the signal's parameters | Design notes |

A and C differ only on code that uses the result. Under A such code has to be restructured, typically into a connected handler; under C it keeps its shape, but the language gains a binding form that exists only for await. B keeps GDScript's shape and loses the parameter types. We wanted to know three things: how often real Godot 4 code awaits a signal with two or more parameters, how often it uses the result, and what it does with it.

## Corpus

We used 14 Godot 4 projects: the six from the literal-local survey, the official third-person shooter demo, the image editor Pixelorama, and six widely used plugins and templates. All are MIT-licensed. The LICENSE.md of tps-demo puts its assets under CC-BY 3.0 and its code under MIT; we only read its code. Only `.gd` files and license files were checked out.

| Repository | Commit | `.gd` files | Lines | First-party lines | Awaits (first-party) |
|---|---|---|---|---|---|
| [godotengine/godot-demo-projects](https://github.com/godotengine/godot-demo-projects) | `b761b4c` | 461 | 30,819 | 30,819 | 171 (171) |
| [gdquest-demos/godot-open-rpg](https://github.com/gdquest-demos/godot-open-rpg) | `d59f2f7` | 342 | 40,662 | 5,422 | 236 (111) |
| [KenneyNL/Starter-Kit-3D-Platformer](https://github.com/KenneyNL/Starter-Kit-3D-Platformer) | `3fa8a04` | 9 | 378 | 378 | 1 (1) |
| [KenneyNL/Starter-Kit-City-Builder](https://github.com/KenneyNL/Starter-Kit-City-Builder) | `4535092` | 6 | 311 | 311 | 0 |
| [KenneyNL/Starter-Kit-FPS](https://github.com/KenneyNL/Starter-Kit-FPS) | `185fd23` | 7 | 451 | 451 | 0 |
| [KenneyNL/Starter-Kit-Racing](https://github.com/KenneyNL/Starter-Kit-Racing) | `2f2e5f2` | 3 | 258 | 258 | 0 |
| [godotengine/tps-demo](https://github.com/godotengine/tps-demo) | `a82f154` | 15 | 1,619 | 1,619 | 8 (8) |
| [Orama-Interactive/Pixelorama](https://github.com/Orama-Interactive/Pixelorama) | `38a8b03` | 249 | 60,025 | 56,024 | 92 (83) |
| [ramokz/phantom-camera](https://github.com/ramokz/phantom-camera) | `21d3762` | 36 | 10,410 | 10,410 | 7 (7) |
| [nathanhoad/godot_dialogue_manager](https://github.com/nathanhoad/godot_dialogue_manager) | `7a72dcf` | 79 | 14,619 | 14,619 | 240 (240) |
| [bitbrain/beehave](https://github.com/bitbrain/beehave) | `fe58915` | 322 | 35,193 | 8,566 | 240 (28) |
| [derkork/godot-statecharts](https://github.com/derkork/godot-statecharts) | `76d226a` | 141 | 18,768 | 6,852 | 87 (70) |
| [MikeSchulze/gdUnit4](https://github.com/MikeSchulze/gdUnit4) | `9bc1a45` | 482 | 54,133 | 54,133 | 586 (586) |
| [Maaack/Godot-Game-Template](https://github.com/Maaack/Godot-Game-Template) | `6849d6c` | 125 | 8,553 | 8,553 | 51 (51) |
| **Total** | | **2,277** | **276,199** | **198,415** | **1,719 (1,356)** |

"First-party" means code written by the repository's own authors. Where a repository's product is itself an addon (beehave, gdUnit4, Dialogue Manager, Godot State Charts, Phantom Camera, and Maaack's add-ons in the game template), that addon is first-party, as are the plugins that godot-demo-projects ships as part of its demos. Bundled addons by other authors are vendored and excluded: gdUnit4 inside beehave, Dialogic inside godot-open-rpg, GUT inside godot-statecharts, and four of Pixelorama's six addons (aimg_io, dockable_container, gdgifexporter, SmartSlicer), whose authorship is not evident from the repository. Pixelorama's keychain (same organization) and applinks (written for Pixelorama) are first-party. This rule is narrower than the one in the literal-local survey, which excluded every `addons/` directory.

Four repositories have test suites: `beehave/test`, `gdUnit4/addons/gdUnit4/test`, `godot_dialogue_manager/tests` and `godot-statecharts/tests`. Together they have 35,226 lines and 625 awaits, nearly half of all first-party awaits, so we report first-party results both with and without them. The `physics_tests` directories of godot-demo-projects are demos, not test suites.

## Method

1. **Extract every await.** A script blanks comments and the contents of string literals, then finds every `await` token: 1,858 in the source, of which 1,719 are in code. The operand is parsed as GDScript parses it. await binds tighter than every operator except call, attribute access and subscript (`modules/gdscript/gdscript_parser.h:1490` at `469659e`), so the operand is a primary expression followed by its chain of calls, attributes and subscripts. Parenthesized, multi-line and backslash-continued forms are handled.
2. **Classify the operand.** An operand ending in a call is a coroutine call if the called function's body contains await; dynamic calls through `call()`, `callv()` or `Expression.execute()`, calls of overridable methods whose base implementation has no await, and calls of functions without await are counted as other calls. An operand ending in a name is looked up as a signal in the receiver's type. Receiver types come from self, annotations, `as` casts, `.new()`, and the return and property types of engine methods. User signals come from the `signal` declarations in the corpus, and engine signals from the class reference XML (`doc/classes` and `modules/*/doc_classes`, `platform/*/doc_classes` at `469659e`). If the receiver's type is unknown, as with `$AnimationPlayer` or an autoload, the signal is identified by name when every engine signal and every signal declared in the same Godot project with that name has the same number of parameters.
3. **Decide whether the result is used.** An await is a statement if it begins a statement (at the start of a line, after `;`, or after the `:` of a block or one-line lambda) and its operand ends that statement. Otherwise its result is used, and the script records how: variable initializer, assignment, return, argument, condition, and so on.
4. **Read every case that matters; no sampling.** We read all 632 signal awaits in a listing of one line per site, to check the arity and the statement-or-used decision; all 21 signal awaits whose result is used, in context, together with the later lines that use the result; and every await whose operand is neither a resolved signal nor a coroutine call. This produced 27 manual decisions, recorded in `classification.json`: four ambiguous `animation_finished` signals (no parameters on AnimatedSprite2D/3D, one on AnimationMixer), resolved from the scene files; six signals declared outside the corpus (three in C#, three in an Android plugin); 15 Signal values returned by GUT's `wait_seconds()` and `wait_frames()`; and two GDScriptFunctionState values. Three scene files and one C# file, which the checkout lacked, were read to settle these cases.
5. **Context.** To see whether multi-parameter signals are rare in general, the script also counts `<signal>.connect(...)` sites and `signal` declarations by number of parameters, using the same resolution. These counts are automatic and were not reviewed one by one.

An AI agent did the reading and classification described above.

## Results

### Awaits by operand

| Operand | All | First-party | First-party, excluding tests |
|---|---|---|---|
| Signal, engine class | 508 | 361 | 331 |
| Signal, user-declared | 82 | 58 | 54 |
| Signal, engine or user, identified by name | 21 | 8 | 8 |
| Signal value returned by a call | 15 | 15 | 0 |
| Signal declared outside the corpus | 6 | 5 | 5 |
| Coroutine call | 1,025 | 876 | 308 |
| Other call | 59 | 31 | 24 |
| Other | 3 | 2 | 1 |
| **Total** | **1,719** | **1,356** | **731** |

That is 6.22 awaits per 1,000 lines, 6.83 in first-party code, and 4.48 excluding tests. Test code is dominated by coroutine calls into test frameworks (568 of its 625 awaits), so the number of signal awaits changes little when tests are excluded (447 → 398). The Signal values returned by a call are all from GUT's `wait_seconds()` and `wait_frames()` in godot-statecharts' tests, which return a signal with no parameters. "Other" is two GDScriptFunctionState values (one of them in vendored code) and one awaited lambda.

### Signal awaits by number of parameters

Each cell is statement / result used.

| Parameters | All | First-party | First-party, excluding tests |
|---|---|---|---|
| 0 | 590 / 2 | 415 / 1 | 366 / 1 |
| 1 | 19 / 18 | 17 / 12 | 17 / 12 |
| 2 or more | 0 / 1 | 0 / 1 | 0 / 1 |
| Unknown | 2 / 0 | 1 / 0 | 1 / 0 |
| **Total** | **611 / 21** | **433 / 14** | **384 / 14** |

**One await in the whole corpus is on a signal with two or more parameters**, and it uses the result. That is about 0.005 per 1,000 first-party lines. Its signal is declared in C# (example 1). No engine signal and no signal declared in GDScript with two or more parameters is awaited at all, not even as a statement. The unknown row is `ExecutionCompleted`, a C# signal of gdUnit4's .NET API, awaited as a statement.

The results that are used, in first-party code:

- **No parameters, 1:** a `return await` that returns null from a function returning Variant (example 5).
- **One parameter, 12 of 29:** 7 variable initializers (5 with a type annotation), 4 returns and 1 assignment.
- **Two or more parameters, 1:**

| Site | Signal | Parameters | How the result is used |
|---|---|---|---|
| `godot_dialogue_manager/addons/dialogue_manager/dialogue_manager.gd:1956` | `DialogueManager.Resolved`, declared in C# (`addons/dialogue_manager/DialogueManager.cs:78`) | 2 (`double id, Variant value`) | Stored as an `Array`; `[0]` is compared with a request id, `[1]` is returned |

For comparison, the result of a coroutine call is used in 328 of the 876 first-party coroutine awaits.

One case is not in these tables because it awaits a coroutine call: gdUnit4's own test of its helper that waits for a signal by name reads the Array of a three-parameter signal (`gdUnit4/addons/gdUnit4/test/core/GdUnitSignalAwaiterTest.gd:19`, example 4).

### Context: multi-parameter signals are connected, not awaited

First-party code; the engine column counts the signals in the class reference at `469659e`.

| Parameters | `.connect()` sites | Of which outside tests | GDScript signal declarations | Engine signals |
|---|---|---|---|---|
| 0 | 542 | 530 | 254 | 246 |
| 1 | 397 | 384 | 198 | 194 |
| 2 or more | 94 | 87 | 64 | 78 |
| Unknown | 60 | 60 | – | – |

Signals with two or more parameters account for 9% of the connect sites of known arity (94 of 1,033) and 12% of GDScript signal declarations (64 of 516), but for none of the awaits of engine or GDScript signals. `HTTPRequest.request_completed`, the example in the design notes, has 4 connect sites in code and no await.

## Examples

**1. The only await of a multi-parameter signal.**
`godot_dialogue_manager/addons/dialogue_manager/dialogue_manager.gd:1950-1961`

```
func callv_dotnet(thing: Variant, method: String, args: Array) -> Variant:
    var dotnet_dialogue_manager: RefCounted = _get_dotnet_dialogue_manager()
    var id: float = _rng.randf()
    dotnet_dialogue_manager.ResolveThingMethod(id, thing, method, args)
    var x: int = 0
    while x < 1000:
        var result: Array = await dotnet_dialogue_manager.Resolved   # C#: ResolvedEventHandler(double id, Variant value)
        if result[0] == id:
            return result[1]
        x += 1

    return null
```

The signal is declared in C#, and the receiver is typed RefCounted, so no static signature is visible at the await. In the language this falls under §9's rule for signals with unknown parameters, which keeps GDScript's result shape with type `Variant?`; rules A and C both leave it unchanged. The loop also shows why the arguments matter here: responses to concurrent requests share one signal, and the first argument tells them apart.

**2. When a result is used, it is usually one typed value.**
`godot-open-rpg/src/field/cutscenes/templates/combat/combat_trigger.gd:7-11`

```
func _execute() -> void:
    FieldEvents.combat_triggered.emit(combat_arena)

    var did_player_win: bool = await CombatEvents.combat_finished   # signal with one parameter
```

§9 already types this result from the signal's parameter.

**3. A multi-parameter signal is wrapped before it is awaited.**
`gdUnit4/addons/gdUnit4/src/update/GdUnitUpdateClient.gd:4-69`

```
signal request_completed(response: HttpResponse)

var _http_request := HTTPRequest.new()

func _ready() -> void:
    add_child(_http_request)
    _http_request.request_completed.connect(_on_request_completed)

func request_latest_version() -> HttpResponse:
    var error := _http_request.request("https://api.github.com/repos/MikeSchulze/gdUnit4/tags")
    ...
    return await self.request_completed

func _on_request_completed(_result: int, response_http_status: int, _headers: PackedStringArray, body: PackedByteArray) -> void:
    ...
    request_completed.emit(HttpResponse.new(response_http_status, body))
```

The four arguments of `HTTPRequest.request_completed` arrive in a typed handler, which emits a one-parameter signal carrying an object, and that signal is what gets awaited. This is the shape rule A asks for. The author chose it even though GDScript would have allowed `await _http_request.request_completed` directly.

**4. The Array shape appears where a test framework waits for a signal by name.**
`gdUnit4/addons/gdUnit4/src/core/GdUnitSignalAwaiter.gd:19-60` and `gdUnit4/addons/gdUnit4/test/core/GdUnitSignalAwaiterTest.gd:19-20`

```
func _on_signal_emmited(
    arg0: Variant = NO_ARG,
    ...
    arg9: Variant = NO_ARG) -> void:
    var signal_args: Variant = GdArrayTools.filter_value([arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9], NO_ARG)
    signal_emitted.emit(signal_args)

func on_signal(source: Object, signal_name: String, expected_signal_args: Array) -> Variant:
    source.connect(signal_name, _on_signal_emmited)
    ...
        value = await signal_emitted

# in the test of this helper
var signal_args: Variant = await await_signal_on(monster, "slide", [1.0, 1, 2])
assert_array(signal_args).is_equal([1.0, 1, 2])
```

gdUnit4 connects a callback with ten optional parameters and packs the arguments into an array itself. The signal is named by a string, so the language would treat it as a signal with unknown parameters whatever rule is chosen. gdUnit4's helpers that wait for a signal by name are awaited 93 times in first-party code; apart from the helpers passing the value on, a caller reads it only in this test (lines 11 and 19), and only line 19 is a multi-parameter signal.

**5. A zero-parameter result that §9 rejects.**
`gdUnit4/addons/gdUnit4/src/GdUnitAwaiter.gd:10-18`

```
func await_signal_on(source: Object, signal_name: String, args: Array = [], timeout: int = 2000) -> Variant:
    ...
    if not is_instance_valid(source):
        ...
        return await (Engine.get_main_loop() as SceneTree).process_frame
```

The function returns null after one frame. Under §9, using the result of awaiting a signal with no parameters is a compile error; the rewrite is an `await` statement followed by `return null`. This is the only such case in first-party code; beehave's vendored gdUnit4 has a copy.

## Conclusions

- **Using the result of awaiting a multi-parameter signal is essentially absent.** There is one case in 1,356 first-party awaits, about 0.005 per 1,000 first-party lines. Its signal is declared in C# and reached through a receiver typed RefCounted, so §9's rule for signals with unknown parameters applies to it whichever rule is chosen. Engine signals and GDScript signals with two or more parameters are never awaited, not even as statements.
- **Multi-parameter signals are common, but they are connected.** Code that needs their arguments connects a handler (94 sites). When it then wants to await, it re-emits a one-parameter signal that carries an object.
- **Rule C would serve no code in this corpus.** Rule A costs nothing here, and C can be added later without breaking code, because it only gives meaning to code that A rejects. The data support keeping A in the first version and leaving C in the design notes. B stays rejected.
- **Signal results are rarely used at all**: in 14 of 447 first-party signal awaits, against 328 of 876 coroutine awaits. Signals with one parameter account for 12 of the 14.
- **A side finding for the zero-parameter rule:** one first-party await uses the result of a signal with no parameters, to return null. §9 rejects this, and the rewrite is trivial.

## Limitations

- **The corpus is mostly tools, plugins and demos.** Code that talks to web services or game servers, where awaiting `request_completed` would be natural, is underrepresented: the corpus's HTTP code is in plugin updaters and a palette downloader. Such code may await multi-parameter signals more often.
- **Only GDScript was analyzed.** Connections made in scene files and code written in C# were not; one C# file and three scene files were read to settle individual cases.
- **Receiver types come from a small resolver.** Where it fails, a signal is identified by name only if all signals of that name agree on the number of parameters; the four that did not were resolved by hand.
- **Whether a result is used is decided syntactically.** All 632 signal awaits were checked in the one-line listing, and the 21 used ones in context.
- **Coroutine detection is approximate.** A function counts as a coroutine if its body contains await. This affects only the coroutine-call and other-call rows.
- **Some code is duplicated.** Godot-Game-Template copies its example scenes into `scenes/`, and godot-demo-projects ships the editor scripts of the 2.5D demo in both its GDScript and C# versions. None of the duplicated awaits uses its result.
- **The connect and declaration counts are automatic** and were not reviewed one by one; some name-based identifications may be wrong.
- **The classification was done by an AI agent**, which read every case listed in the method section.

## Reproducing

The scripts are in [`await-signal-results/`](await-signal-results/): `extract.py` finds and classifies every await and prints the tables above, `dump.py` prints selected awaits with their context, and `classification.json` holds the manual decisions, each with a note on its evidence. The engine signals come from a Godot checkout at `469659e`.

```
mkdir corpus && cd corpus
git clone https://github.com/godotengine/godot-demo-projects
git -C godot-demo-projects checkout b761b4c
# clone and check out the other repositories the same way (commits are listed in the corpus table)
cd ..
python docs/surveys/await-signal-results/extract.py corpus ../godot          # tables; writes corpus/awaits.json and corpus/connects.json
python docs/surveys/await-signal-results/dump.py corpus used-signals > used.txt   # signal awaits whose result is used, with context
```

`dump.py` also accepts `all`, `signals`, `used`, `multi`, `unknown`, `byname`, `nonsignal` and `manual`. A sparse checkout of `.gd` files gives the same counts, because the script reads the locations of `project.godot` from the git tree when the files are absent.
