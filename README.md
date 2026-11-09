# Petrichor

Petrichor is a statically typed scripting language for Godot 4, delivered as a GDExtension. It replaces GDScript in project `.pet` scripts and coexists with GDScript plugins and C#. Type checking extends into `.tscn` scenes, and bytecode runs on a register VM over native slots. Engine behavior is verified against Godot 4.7.2-stable ([engine facts](docs/engine-facts.md)).

中文：[README.zh.md](README.zh.md)

## Why Petrichor

As Godot projects grow, refactoring depends more and more on errors that only show at run time. GDScript's typing is gradual. Even in fully typed GDScript, node paths, signal connections and exported properties stay dynamic in scenes. Object references are nullable by default, and runtime errors still hand callers a fallback value. C# brings static types, but its bridge has many pitfalls ([survey](docs/surveys/pitfalls.md)).

Petrichor is statically typed from scripts into scenes, without modifying the engine. It keeps GDScript's surface syntax under one hard rule ([§1.2](docs/language-spec.zh.md#12-与-gdscript-的关系)). **A construct spelled as in GDScript either behaves identically or fails at compile time with a suggested fix.** It is never silently different. Where GDScript fails at run time or silently miscomputes, Petrichor raises an error or preserves the intended meaning.

## A Taste

```gdscript
class_name Player extends CharacterBody3D implements Combat.Damageable

pub signal died

enum Command { IDLE, MOVE(target: Vector3), ATTACK(target: Node3D, power: int) }

@export pub var bullet_scene: PackedScene[Bullet]   # checked in the inspector
pub var hp: int = 100

@onready var camera = $Head/Camera                  # Camera3D, typed from player.tscn

pub func take_damage(amount: int):
    hp -= amount
    if hp <= 0:
        died.emit()

async func run(command: Command):
    match command:                                  # must cover every variant
        Command.IDLE:
            velocity = Vector3.ZERO
        Command.MOVE(var target):                   # var target is short for target = var target
            velocity = global_position.direction_to(target)
        Command.ATTACK(var target, var power):
            var bullet := bullet_scene.instantiate() # typed as Bullet
            await bullet.fire_at(camera, target, power)

func max_hp_from(data: Variant) -> int?:
    if data is Dictionary:                          # data narrows to Dictionary
        return int.try_from(data.get("max_hp")?).ok()   # ? returns None if the key is missing
    return None
```

## Capabilities

- **Typed scenes.** `$X`, `%X` and literal `get_node` paths take their types from the scenes using the script. `@owner(T)` types ancestor access, and `PackedScene[T]` types scene instantiation. Editor signal connections and exported scene properties are statically checked, with errors marked on scene nodes ([§7](docs/language-spec.zh.md#7-场景模型)).
- **Null safety and explicit errors.** Object types are non-null by default, and nullable references are written `T?`. Freed objects read as `None` on `T?` instead of dangling silently. Expected failures use `Option` and `Result[T, E]` as in Rust, with postfix `?` ([§3.3–§3.5](docs/language-spec.zh.md#33-对象与可空), [§8.2](docs/language-spec.zh.md#82-预期之内的失败)).
- **Rich data types.** Value-type `struct` declarations, payload `enum` types, exhaustive `match`, interfaces and nested typed containers are built in ([§2.4](docs/language-spec.zh.md#24-声明的写法), [§3](docs/language-spec.zh.md#3-类型)).
- **Structured coroutines.** `async func` returns a `Task[T]`, and cancellation follows task ownership. Awaiting a freed object raises an error instead of hanging forever ([§9](docs/language-spec.zh.md#9-协程)).
- **GDScript and C# coexistence.** Third-party plugins stay in GDScript, and C# code keeps working alongside Petrichor. Petrichor builds static views of foreign classes and statically checks GDScript callers of Petrichor instances ([§11](docs/language-spec.zh.md#11-与-gdscript-和-c-共存)).
- **Editor and CLI tools.** Editor support includes checked completion, hover types, go-to-definition, a lossless formatter, debugging and function-body hot reload. Exports ship bytecode without the compiler, and headless CLI flags run builds and formatting in CI ([§12](docs/language-spec.zh.md#12-运行时), [§13](docs/language-spec.zh.md#13-工具链)).

## Performance and Validation

Petrichor already runs the migrated scripts of two real Godot 4 games ([migration survey](docs/surveys/paper-migration-2.md)). One is Godot's 3D FPS starter kit, and the other is the breakout minigame inside Ring, a C# game. Design choices that real code can answer are backed by reproducible surveys across 20 Godot 4 projects.

The VM stores scalars, engine math values, `struct` values and payload `enum` values unboxed in typed slots. Selected median times in milliseconds on official Godot 4.7.2-stable are shown below ([full 14-benchmark suite and methodology](docs/architecture.md#4-性能)):

| Group | Benchmark | GDScript (ms) | Petrichor (ms) |
|---|---|---:|---:|
| Compute | `int_loop` | 34.94 | 9.98 |
| Compute | `calls` | 79.44 | 23.34 |
| Compute | `members` | 30.98 | 12.09 |
| Compute | `vectors` | 34.48 | 16.35 |
| Engine boundary | `property_get` | 10.29 | 6.22 |
| Allocation | `closures` | 26.45 | 18.32 |
| Allocation | `coroutines` | 25.94 | 20.47 |
| 32-script startup | `cold` (build + run) | 3069.6 | 3078.6 |

## Status and Limitations

0.1 is the first public release. It gathers feedback on the semantics from real projects before 1.0.

- Suited to: projects that already use typed GDScript heavily and accept code changes during 0.x.
- Not yet suited to: projects that export to Android, iOS or Web. Their libraries come with 1.0.
- The language semantics may change before 1.0. Release notes list every incompatible change.
- Bytecode is not compatible across builds. After upgrading the extension, rebuild the project once.
- The editor lacks find references, rename, quick fixes, lints and LSP. They are planned for 0.2–0.3.
- There is no GDScript migration tool yet, so 0.1 needs manual migration. The tool is planned for 0.2–0.3.
- Godot 4.7 and later are supported.

## How It Is Built

The author decides the language design and every rule of the specification. AI coding agents write most of the implementation, tests, documentation and corpus surveys. The author does not review the code line by line. The agents follow [AGENTS.md](AGENTS.md) in this repository. Correctness rests on these:

- The specification is the only source of rules. Where the implementation disagrees with it, the implementation changes.
- Tests are derived from the specification and the engine's behavior, not from the implementation.
- Each engine behavior relied on cites the Godot source, with commit and line.
- The migrated scripts of two real projects run in the engine.

The author is responsible for fixing what goes wrong. During 0.1, the project takes issues but not pull requests. Development moves fast, so a pull request is usually stale by the time it is reviewed. Please report problems as issues instead. Pull requests open as 1.0 approaches.

## Building and CLI

Release 0.1 will provide prebuilt GDExtension binaries for Windows, Linux and macOS. To build from source with SCons and a C++17 toolchain, initialize submodules first:

```sh
git submodule update --init
scons                        # editor and compiler library
scons target=template_debug  # export runtime library without compiler
scons compiler_tests         # standalone compiler test suite
```

With a Godot 4.7 editor binary, projects can also be checked and formatted headlessly in CI:

```sh
godot --headless -e --path <project> -- --petrichor-build
godot --headless -e --path <project> -- --petrichor-format --check
```

## Roadmap

- **0.1 (Initial public release, in progress).** Complete first-version language coverage, scene checking and editor tools, and provide the English language specification. Ship prebuilt desktop binaries to gather real-project feedback.
- **0.2–0.3.** Add cross-scene renaming, quick fixes, lints, LSP support and two-way GDScript migration.
- **1.0.** Stabilize language semantics and bytecode versioning, and support Godot 4.4 and later. Ship prebuilt export libraries for Android, iOS and Web.
- **Post-1.0.** Add native code generation, checked mod loading, inspector export for `struct` and payload `enum`, and generics ([§14.2](docs/language-spec.zh.md#142-以后可以兼容地加入)).

[Architecture §5–§6](docs/architecture.md#5-推进顺序) details the milestone order and invariants for mods, exports and team refactoring. Unfinished work and open questions are tracked in [BACKLOG.md](BACKLOG.md).

## Documentation

- [Language specification (v0.2)](docs/language-spec.zh.md) — normative rules for the language, scene model and runtime.
- [Implementation architecture](docs/architecture.md) — compiler, bytecode VM, Godot adapter, benchmarks and roadmap.
- [Design notes](docs/design-notes.md) — trade-offs, corpus counts and rejected alternatives.
- [Engine facts](docs/engine-facts.md) — Godot 4.7.2-stable source citations verified for the specification.
- [Development notes](docs/development.md) — build, testing and debugging pitfalls for contributors.
- Empirical surveys across 20 Godot 4 projects (each with reproducible scripts):
  - [Do scripts see more than one scene structure?](docs/surveys/scene-structures.md)
  - [How much code duplication do different native base classes force?](docs/surveys/cross-branch-duplication.md)
  - [Pitfalls of the engine, GDScript and the C# bridge](docs/surveys/pitfalls.md)
  - [How do real Godot 4 projects use property getters and setters?](docs/surveys/property-accessors.md)
  - [How often is the result of awaiting a multi-parameter signal used?](docs/surveys/await-signal-results.md)
  - [Where are script members used: own file, subclasses, other scripts?](docs/surveys/member-visibility.md)
  - [What does it cost to migrate real code to the current design?](docs/surveys/paper-migration-2.md)
  - [What did it cost to migrate real code to spec v0.1?](docs/surveys/paper-migration.md)
  - [What does computing the constants of real code take?](docs/surveys/constant-expressions.md)
  - [Which constructs of real code does the implementation not support yet?](docs/surveys/implementation-coverage.md)
  - [One-off counts in the corpus](docs/surveys/corpus-counts.md)

Documents under `docs/` are currently in Chinese. The English language specification comes with 0.1.

## License

[MIT](LICENSE)
