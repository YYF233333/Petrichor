# Petrichor

Petrichor is a statically typed scripting language for Godot 4 that keeps GDScript's syntax. Code written as in GDScript either behaves the same or fails at compile time with a suggested fix. It ships as a GDExtension for the official editor. Exports use the official templates, with no custom engine build.

The project is in development toward 0.1. Semantics may still change before 1.0; see [Status and Limitations](#status-and-limitations).

中文：[README.zh.md](README.zh.md)

![After a node is renamed, the scene dock marks the error on the node that uses it](docs/images/scene-error.png)

## Why

As Godot projects grow, refactoring depends more and more on errors that only show at run time. GDScript's typing is gradual. Even in fully typed GDScript, node paths, signal connections and exported properties stay dynamic in scenes. Object references are nullable by default, and runtime errors still hand callers a fallback value. C# brings static types, but its bridge has many pitfalls ([survey](docs/surveys/pitfalls.md)).

Petrichor extends static checking into scenes, and makes null values and failures visible at compile time.

## Compared with GDScript

This GDScript passes the checks but fails at run time. It does so when `Camera` is renamed in the scene, or when `target` is null:

```gdscript
@onready var camera: Camera3D = $Head/Camera
var target: Node3D

func aim():
    camera.look_at(target.global_position)
```

In Petrichor, the scene the script is attached to gives `camera` its type. Renaming the node fails at compile time. `target` becomes `Node3D?`, which must be checked before use:

```gdscript
@onready var camera = $Head/Camera
var target: Node3D?

func aim():
    camera.look_at(target.global_position)
    # error: The value may be None (Node3D?); check it before reading "global_position".
```

```gdscript
func aim():
    var t := target
    if t != None:
        camera.look_at(t.global_position)
```

Other differences:

- Signal connections and exported properties are checked against scenes too, with errors marked on scene nodes.
- Object types are non-null by default. A freed object reads as `None` through `T?`.
- Expected failures use `Option` and `Result[T, E]`, with a postfix `?` for early return.
- Value-type `struct`, `enum` with payloads, exhaustive `match`, interfaces and nested typed containers.
- `async func` returns an owned `Task`. When its owner is freed, the coroutine is cancelled instead of hanging forever.
- It mixes with GDScript plugins and C# code.
- The editor has completion, hover types, go to definition, formatting, breakpoints and hot reload of function bodies.

The full rules are in the [language specification](docs/language-spec.zh.md).

In microbenchmarks, computation runs 1.2 to 3.5 times as fast as GDScript, and engine property access about 1.6 times. Calls to engine methods, signals and object creation spend their time in the engine, so the two are close. Project startup takes the same time ([benchmarks](docs/architecture.md#4-性能)).

The migrated scripts of two real projects run: Kenney's FPS starter kit and the breakout minigame in Ring ([migration survey](docs/surveys/paper-migration-2.md)).

## Trying It

With 0.1, GitHub Releases will offer two packages:

- A demo: the FPS starter kit with migrated scripts, ready to run in Godot 4.7.
- The extension: unpack it into a project and restart the editor to create `.pet` scripts.

To check a whole project in CI:

```sh
godot --headless -e --path <project> -- --petrichor-build
godot --headless -e --path <project> -- --petrichor-format --check
```

Until then, build from source with a C++17 toolchain and SCons. Run `git submodule update --init`, then `scons`.

## Status and Limitations

0.1 is the first public release. It gathers feedback on the semantics from real projects before 1.0.

- Suited to: projects that already use typed GDScript heavily and accept code changes during 0.x.
- Not yet suited to: projects that export to Android, iOS or Web. Their libraries come with 0.9.
- Before 0.9, the language semantics may change, and release notes list every incompatible change. From 0.9, a change in what code means shows as a compile error. Fixes that change only runtime behavior are listed apart in release notes.
- Bytecode is not compatible across builds. After upgrading the extension, rebuild the project once.
- The editor lacks find references, rename, quick fixes, lints and LSP. They are planned for 0.2–0.3.
- There is no GDScript migration tool yet, so 0.1 needs manual migration. The tool is planned for 0.2–0.3.
- Godot 4.7 and later are supported.

The full roadmap is in the [architecture](docs/architecture.md#5-推进顺序), and unfinished work in [BACKLOG.md](BACKLOG.md).

## How It Is Built

The author decides the language design and every rule of the specification. AI coding agents write most of the implementation, tests, documentation and corpus surveys. The author does not review the code line by line. The agents follow [AGENTS.md](AGENTS.md) in this repository. Correctness rests on these:

- The specification is the only source of rules. Where the implementation disagrees with it, the implementation changes.
- Tests are derived from the specification and the engine's behavior, not from the implementation.
- Each engine behavior relied on cites the Godot source, with commit and line.
- The migrated scripts of two real projects run in the engine.

The author is responsible for fixing what goes wrong. During 0.1, the project takes issues but not pull requests. Development moves fast, so a pull request is usually stale by the time it is reviewed. Report defects as issues and opinions on the language rules in Discussions; both get a reply within 48 hours. Pull requests open as 1.0 approaches.

## Documentation

- [Language specification](docs/language-spec.zh.md)
- [Architecture](docs/architecture.md)
- [Design notes](docs/design-notes.md): the trade-offs behind the rules, corpus counts and rejected alternatives
- [Engine facts](docs/engine-facts.md): the engine behavior relied on, with source citations
- [Development notes](docs/development.md): pitfalls in building, testing and debugging
- [Corpus surveys](docs/surveys/README.md): across 20 Godot 4 projects, each with scripts to reproduce it

Documents under `docs/` are currently in Chinese. The English language specification comes with 0.1.

## License

[MIT](LICENSE)
