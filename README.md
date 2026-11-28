# Petrichor

Petrichor is a statically typed scripting language for Godot 4 that keeps GDScript's syntax. It ships as a standard GDExtension for the official Godot editor and export templates, with no custom engine build required. Aside from a few intentional differences documented in the spec, code written like GDScript either behaves the same or fails to compile with a suggested fix.

Latest release: **0.3.0** · 0.x preview (semantics may still change before 1.0; see [Status and Roadmap](#status-and-roadmap)) · 中文：[README.zh.md](README.zh.md)

![When a node path in a script matches no node, the scene dock marks the error on the node that uses it](docs/images/scene-error.png)

## Why Petrichor

As a Godot project grows, refactoring leans more and more on runtime errors to catch breakage. Because GDScript is gradually typed, node paths, signal connections, and exported properties stay dynamic in scenes even when every script carries type annotations. Object references are also nullable by default, and after a runtime error the caller often still gets a fallback value and keeps running. C# brings static typing, though its engine bridge has many pitfalls ([survey](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/pitfalls.md)).

Petrichor extends static checking into scenes: node paths, signal connections, and exported properties are checked at compile time. Whether an object can be null and whether a function can fail are also written in types, which the compiler checks at each use.

These rules cannot be added to GDScript as patches, so Petrichor is a separate language. GDScript has to stay compatible with existing dynamic scripts. Non-null by default, `bool`-only conditions, exhaustive `match`, and mandatory failure handling would break existing projects if they became its defaults. Checking a whole project's scenes and exporting only bytecode also need a different analyzer and VM.

Petrichor ships as a standard GDExtension, so it needs no custom engine build: it runs in the official editor and exports with the official templates. This also lets the language iterate on its semantics before 1.0. Existing GDScript plugins and C# code keep working alongside it in the same project. The migration tool `pet migrate` converts a project's GDScript to Petrichor and back, so scripts that use no Petrichor-only features can return to GDScript at any time.

## At a Glance

### Scene checking and non-null types

This GDScript passes static checks, yet breaks at run time if `Camera` is renamed in the scene or if `target` is null:

```gdscript
@onready var camera: Camera3D = $Head/Camera
var target: Node3D

func aim():
    camera.look_at(target.global_position)
```

In Petrichor, `camera` gets its type from the attached scene, and renaming the node in the scene dock updates the path. Signal connections and exported properties are checked against scenes the same way. Because object types are non-null by default, `target` must be declared `Node3D?` and narrowed before use:

```gdscript
@onready var camera = $Head/Camera     # inferred as Camera3D from the scene
var target: Node3D?                    # nullable; plain Node3D is non-null

func aim():
    # camera.look_at(target.global_position)
    # ^ error: The value may be None (Node3D?); check it before reading "global_position".
    var t := target                    # members do not narrow; bind to a local
    if t != None:                      # also checks that t has not been freed
        camera.look_at(t.global_position)
```

### Value structs, payload enums, and typed failures

Petrichor adds value-type `struct`, payload-carrying `enum`, exhaustive `match`, interfaces, and nested typed containers. Expected failures use `Option` and `Result[T, E]`, with a postfix `?` operator for early return:

```gdscript
struct Hit:
    var damage: int
    var crit: bool = false

enum Action { BLOCK, STRIKE(hit: Hit) }

func apply_action(raw: Variant) -> Result[int, ConversionError]:
    match Action.try_from(raw)?:       # returns Err early on invalid input
        Action.BLOCK:
            return Ok(0)
        Action.STRIKE(var hit):        # exhaustive: missing a branch is a compile error
            return Ok(hit.damage * 2 if hit.crit else hit.damage)
```

### Owned async tasks and cancellation

An `async func` returns an owned `Task` that cancels when its owner is freed. Cancelling a task also cancels the subtasks it started with `await f()`. Awaiting a cancelled or freed target yields a cancellation or program error instead of hanging forever:

```gdscript
var current_cast: Task[void]?

async func cast(enemy: Node3D):
    var first := await Task.any([Task.delay(1.5), Task.of(enemy.tree_exiting)])
    if first == 0:
        print("Hit ", enemy.name)

func restart_cast(enemy: Node3D):
    var prev := current_cast
    if prev != None:
        prev.cancel()          # cancels the old cast and its owned subtasks
    current_cast = cast(enemy) # also cancels automatically if this node is freed
```

### Tooling, performance, and real projects

Built-in editor support covers completion, hover types, go to definition, find references, rename, quick fixes, formatting, breakpoints, and function-body hot reload. Renaming or moving a node in the scene dock updates script paths pointing to it. Petrichor also ships a VS Code extension, an LSP bridge, and the `pet` CLI, and runs alongside GDScript plugins and C# in the same project.

In microbenchmarks, pure computation runs 1.2 to 3.5 times as fast as GDScript, and engine property access runs about 1.6 times as fast. Engine method calls, signal emissions, and object creation stay engine-bound and match GDScript, and project startup time is unchanged ([benchmarks](docs/architecture.md#4-性能), Chinese). Three real projects run end to end after migration, and again after converting back to GDScript: Kenney's FPS starter kit, the breakout minigame from Ring, and deck_builder_tutorial. `pet migrate` converted them and left compile errors, which were then fixed by hand. A [survey](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/tool-migration.md) counts what the tool leaves in three games, by cause. See the [language specification](docs/language-spec.md) for the full rule set.

## Trying It

Petrichor requires the official Godot 4.7 editor (standard or .NET) and ships prebuilt libraries for Windows, Linux, and macOS. Download a package from the [Releases](https://github.com/YYF233333/Petrichor/releases) page:

- Demo (`petrichor-<version>-demo.zip`): Kenney's FPS starter kit with migrated scripts. Unpack it, open the project in Godot, and run it once import finishes.
- Extension (`petrichor-<version>-addon.zip`): Unpack it into your project root to create `addons/petrichor/`, then restart the editor to create `.pet` scripts.

On macOS, clear the download quarantine attribute before loading the extension; see [macOS quarantine](docs/getting-started.md#macos-quarantine).

To see what a migration would do to your own project, unpack the extension into it and open the project once in Godot. Then commit the project to git and run:

```sh
addons/petrichor/pet migrate --dry-run
```

The dry run writes nothing. It prints what the tool would change, and where it would leave a compile error for you to settle. [Migrating from GDScript](docs/getting-started.md#migrating-from-gdscript) explains the report and the steps after it.

Read [Getting Started](docs/getting-started.md) for full installation steps, upgrading, supported platforms, VS Code integration, the `pet` CLI, and building from source.

## Status and Roadmap

Petrichor is in its 0.x preview series to gather feedback on the language semantics before 1.0.

- Suited to projects that already rely heavily on typed GDScript and can accommodate code changes during 0.x.
- Not yet suited to projects exporting to Android, iOS, or Web. Those export libraries begin arriving in 0.4, starting with the platforms verified to work.
- Before 0.9, language semantics may still change, and release notes list every breaking change. From 0.9 onward, upgrades do not silently change what existing code means; any breaking semantic change turns the old form into a compile error.
- A few constructs deliberately behave differently from GDScript without a compile error. The [spec](docs/language-spec.md#12-relation-to-gdscript) lists each one, and they stay open for feedback until 0.9.
- Compiled bytecode is not compatible across builds, so upgrading recompiles every script.
- Editor lints and standard-library extensions are not in yet. Neither is tied to a release; each ships with whichever 0.x it is ready for.
- `pet migrate` leaves a compile error wherever a change needs your judgment, with quick fixes to settle it. Scripts with `@tool` stay in GDScript until 0.4. Files sticking to GDScript-compatible features convert back to GDScript even if Petrichor is discontinued. Files using Petrichor-only features such as `struct` or `Result` are not converted back, and the tool lists each place.
- Supports Godot 4.7 and later.

Roadmap overview:

- **0.2.x**: Core static language, scene checking, built-in editor and LSP/VS Code tooling, `pet` CLI, and prebuilt desktop libraries.
- **0.3 (current: 0.3.0)**: The two-way GDScript migration tool, and toolchain speed on projects with a thousand scripts.
- **0.4**: `@tool`, builds for Godot 4.4–4.6, and Android/iOS/Web export libraries. It also checks double-precision builds, runs the migration tool over more open-source games, and tries keeping `pet`'s background process alive between commands.
- **0.5–0.8**: The open questions that would change what legal code means, such as NaN in containers. The project then stays at 0.8 until at least two outside projects have migrated with the tool and reported back. User-defined generics are implemented during that stay.
- **0.9 (language freeze)**: All of 1.0's technical content. The upgrade guarantee above takes effect with this release.

See the detailed roadmap in [Architecture](docs/architecture.md#5-推进顺序) (Chinese) and open items in [BACKLOG.md](BACKLOG.md).

## How It Is Built

The project maintainer designs the language and decides every rule in the specification. AI coding agents write most of the implementation, tests, documentation, and corpus surveys following [AGENTS.md](AGENTS.md). Generated code is not reviewed line by line; correctness relies on four checks:

- The specification is the single source of truth; whenever the implementation disagrees with it, the implementation changes.
- Tests are derived from the specification and Godot's actual behavior, not from the implementation.
- Every relied-on engine behavior cites the Godot source by commit hash and line number.
- Three real projects run inside the engine with migrated scripts, and again after converting back.

The maintainer is responsible for fixing whatever breaks. At present, the repository accepts issues but not pull requests, because the codebase moves fast enough that external PRs usually go stale before review. File defects in [Issues](https://github.com/YYF233333/Petrichor/issues) and share language feedback in [Discussions](https://github.com/YYF233333/Petrichor/discussions); both receive a reply within 48 hours. Pull requests will open as 1.0 approaches.

## Documentation

### User guides & reference

- [Getting Started](docs/getting-started.md): installation, upgrading, supported platforms, VS Code, `pet` CLI, and building from source
- [Language specification](docs/language-spec.md): normative rules for syntax, types, scenes, async tasks, and runtime behavior

### Design & internals (Chinese)

- [Architecture](docs/architecture.md): implementation overview, benchmarks, and roadmap
- [Design notes](docs/design-notes.md): the trade-offs behind the rules, corpus counts, and rejected alternatives
- [Engine facts](docs/engine-facts.md): relied-on engine behaviors with source citations
- [Development notes](docs/development.md): pitfalls in building, testing, and debugging
- [Corpus surveys](https://github.com/YYF233333/petrichor-corpus): surveys across corpus v1 (20 Godot 4 projects) and v2 (200 projects), with reproduction scripts and a rule-to-evidence index

User-facing guides (`README` and `Getting Started`) are maintained in both English and Chinese. The English [language specification](docs/language-spec.md) is generated from the Chinese version, which prevails wherever they differ. Other design and implementation documents under `docs/` are maintained in Chinese.

## License

[MIT](LICENSE)
