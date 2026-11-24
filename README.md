# Petrichor

Petrichor is a statically typed scripting language for Godot 4 that keeps GDScript's syntax. It ships as a GDExtension for the official Godot editor and exports with the official templates, with no custom engine build required. Aside from a few intentional differences documented in the spec, code written like GDScript either behaves the same or fails to compile with a suggested fix.

Latest release: **0.2.1**. Semantics may still change before 1.0; see [Status and Limitations](#status-and-limitations).

中文：[README.zh.md](README.zh.md)

![When a node path in a script matches no node, the scene dock marks the error on the node that uses it](docs/images/scene-error.png)

## Why

As a Godot project grows, refactoring leans more and more on runtime errors to catch breakage. Because GDScript is gradually typed, node paths, signal connections, and exported properties remain dynamic in scenes even when every script carries type annotations. Object references are also nullable by default, and runtime failures still return a fallback value to callers. C# brings static typing, though its engine bridge has many pitfalls ([survey](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/pitfalls.md)).

Petrichor extends static checking into scenes, encoding nullability and expected failures in types so the compiler checks them at every use site.

## Compared with GDScript

This GDScript passes static checks, yet breaks at run time if `Camera` is renamed in the scene or if `target` is null:

```gdscript
@onready var camera: Camera3D = $Head/Camera
var target: Node3D

func aim():
    camera.look_at(target.global_position)
```

In Petrichor, `camera` gets its type from the attached scene. Renaming the node in the scene dock updates the path, and a broken path fails at compile time. Because `target` is `Node3D?`, reading it without checking fails to compile:

```gdscript
@onready var camera = $Head/Camera
var target: Node3D?

func aim():
    camera.look_at(target.global_position)
    # error: The value may be None (Node3D?); check it before reading "global_position".
```

Checking against `None` narrows the type so the call compiles:

```gdscript
func aim():
    var t := target
    if t != None:
        camera.look_at(t.global_position)
```

Other differences:

- Signal connections and exported properties are also checked against scenes, with errors marked on the scene nodes.
- Object types are non-null by default, and a freed object reads as `None` through `T?`.
- Expected failures use `Option` and `Result[T, E]`, with a postfix `?` operator for early return.
- Value-type `struct`, payload-carrying `enum`, exhaustive `match`, interfaces, and nested typed containers.
- `async func` returns an owned `Task` that cancels when its owner is freed; anything awaiting it gets a cancellation or program error instead of hanging forever.
- Works alongside GDScript plugins and C# code in the same project.
- Editor support includes completion, hover types, go to definition, find references, rename, quick fixes, formatting, breakpoints, and function-body hot reload. Renaming or moving a node in the scene dock also updates script paths pointing to it.

See the [language specification](docs/language-spec.md) for the full rule set.

In microbenchmarks, pure computation runs 1.2 to 3.5 times as fast as GDScript, and engine property access runs about 1.6 times as fast. Engine method calls, signal emissions, and object creation are engine-bound, so performance stays close to GDScript, and project startup time is unchanged ([benchmarks](docs/architecture.md#4-性能), Chinese).

Migrated scripts from two real projects already run end to end: Kenney's FPS starter kit and the breakout minigame from Ring ([migration survey](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/paper-migration-2.md)).

## Trying It

Petrichor requires the official Godot 4.7 editor (standard or .NET). Download one of the packages from the [Releases](https://github.com/YYF233333/Petrichor/releases) page (the same page also includes the optional VS Code extension below):

- Demo (`petrichor-<version>-demo.zip`): Kenney's FPS starter kit with migrated scripts. Unpack it, open the project in Godot, and run it once import finishes.
- Extension (`petrichor-<version>-addon.zip`): Unpack it into your project root to create `addons/petrichor/`, then restart the editor to create `.pet` scripts.

To upgrade, close the editor and replace `addons/petrichor/` with the new release. Run the project once from the editor afterward to recompile every script.

On macOS, the shipped binaries are ad-hoc signed and not notarized, so macOS quarantines browser downloads and blocks them from loading. After unpacking, clear the quarantine attribute from your project directory:

```sh
xattr -dr com.apple.quarantine addons/petrichor
```

### VS Code

To edit `.pet` scripts in VS Code, download `petrichor-<version>.vsix` from the Releases page and install it via **Extensions: Install from VSIX…**. Open your project in Godot first, then open the same folder in VS Code. VS Code connects to that editor for diagnostics and language tools, except breakpoints. Saving a script in VS Code hot reloads a running game the same way saving in Godot does.

A few notes on the VS Code workflow:

- Type checking reads scenes and classes through Godot, so without a running editor you only get syntax highlighting. When a `.pet` file is active, the status bar shows whether VS Code is connected.
- After undoing a rename in VS Code, save the modified files manually (VS Code auto-saves after a rename, not after undoing one).
- When Godot is in the background, it sleeps 100 ms per frame, which can delay completion and other requests by up to that amount. Lowering **Interface → Editor → Timers → Unfocused Low Processor Mode Sleep (µsec)** reduces that delay at the cost of higher power usage.

### Supported platforms

Every release is tested in CI with the official Godot editor and official export templates, covering editing, running, and exporting both debug and release builds.

| Platform | Library | Tested on |
|---|---|---|
| Windows | x86_64 | Windows Server 2022 |
| Linux | x86_64 (requires glibc 2.35+) | Ubuntu 22.04 |
| macOS | universal (x86_64 and arm64; requires macOS 11+) | macOS 15 on arm64; x86_64 via Rosetta |

### Command-line tool

Run `pet` from the project root to check, build, or format the entire project:

```sh
addons/petrichor/pet check            # check files on disk without writing bytecode
addons/petrichor/pet build            # check files and write bytecode
addons/petrichor/pet format --check   # list unformatted scripts; omit --check to format in place
addons/petrichor/pet lsp              # bridge stdio for LSP clients such as Neovim and Helix
```

On Windows, use `addons\petrichor\pet`. If the Godot editor already has the project open, `pet` runs inside that editor without spawning another process. Otherwise, it launches a headless copy of the Godot binary last recorded by the editor, or you can pass `--godot <path>` explicitly (for example in CI):

```sh
addons/petrichor/pet build --godot <godot>
addons/petrichor/pet format --check --godot <godot>
```

Diagnostics are printed one per line (`player.pet:12:5: error: ...`). Exit codes are `0` for success, `1` for errors or unformatted scripts, and `2` if the command could not run. Because `pet check` inspects files directly on disk, AI coding agents can run it right after editing files to verify their changes.

### Building from source

Building from source requires a C++17 toolchain and SCons: run `git submodule update --init`, then `scons`.

## Status and Limitations

Petrichor is in its 0.x preview series (following the initial 0.1 release) to gather feedback on the language semantics before 1.0.

- Suited to projects that already rely heavily on typed GDScript and can accommodate code changes during 0.x.
- Not yet suited to projects exporting to Android, iOS, or Web. Those export libraries begin arriving in 0.4, starting with the platforms verified to work.
- Before 0.9, language semantics may still change, and release notes list every breaking change. From 0.9 onward, upgrades do not silently change what existing code means; any breaking semantic change turns the old form into a compile error.
- A few constructs deliberately behave differently from GDScript without a compile error. The [spec](docs/language-spec.md#12-relation-to-gdscript) lists each one, and they stay open for feedback until 0.9.
- Compiled bytecode is not compatible across builds, so upgrading recompiles every script.
- Editor lints are not implemented yet and are planned for 0.3.
- There is no GDScript migration tool yet; manual migration takes over twenty edits per hundred lines, so real projects are best tried with the 0.3 tool. That tool works in both directions. Files sticking to GDScript-compatible features convert back to GDScript even if Petrichor is discontinued. Files using `struct`, `Result`, or other Petrichor-only features are skipped, with their locations listed.
- Supports Godot 4.7 and later.

See the full roadmap in [Architecture](docs/architecture.md#5-推进顺序) (Chinese) and open items in [BACKLOG.md](BACKLOG.md).

## How It Is Built

I design the language and decide every rule in the specification. AI coding agents write most of the implementation, tests, documentation, and corpus surveys, following [AGENTS.md](AGENTS.md), and I do not review the generated code line by line. Instead, correctness relies on four checks:

- The specification is the single source of truth; whenever the implementation disagrees with it, the implementation changes.
- Tests are derived from the specification and Godot's actual behavior, not from the implementation.
- Every relied-on engine behavior cites the Godot source by commit hash and line number.
- Migrated scripts from two real projects run inside the engine.

When something breaks, I am responsible for fixing it. At present, the repository accepts issues but not pull requests, because the codebase moves fast enough that external PRs usually go stale before review. File defects in Issues and share language feedback in Discussions; both receive a reply within 48 hours. Pull requests will open as 1.0 approaches.

## Documentation

- [Language specification](docs/language-spec.md)
- [Architecture](docs/architecture.md) (Chinese)
- [Design notes](docs/design-notes.md) (Chinese): the trade-offs behind the rules, corpus counts, and rejected alternatives
- [Engine facts](docs/engine-facts.md) (Chinese): relied-on engine behaviors with source citations
- [Development notes](docs/development.md) (Chinese): pitfalls in building, testing, and debugging
- [Corpus surveys](https://github.com/YYF233333/petrichor-corpus): surveys across corpus v1 (20 Godot 4 projects) and v2 (200 projects), with reproduction scripts and a rule-to-evidence index

Documents under `docs/` are maintained in Chinese. The English [language specification](docs/language-spec.md) is generated from the Chinese version, which prevails wherever they differ.

## License

[MIT](LICENSE)
