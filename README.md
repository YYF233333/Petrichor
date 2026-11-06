# Petrichor

A fully statically typed scripting language for medium-to-large Godot 4 projects, delivered as a GDExtension. It replaces GDScript in project code and coexists with GDScript plugins. Petrichor is a codename.

**Status:** early implementation. The [language specification](docs/language-spec.en.md) (v0.2) is a draft published for feedback. A first subset of the language runs in Godot, built as the [implementation architecture](docs/architecture.md) describes. With it, two milestones are reached. The scripts of a small FPS game run in Petrichor. So do those of the breakout minigame in Ring, a game written mostly in C#. Engine behavior is checked against Godot 4.7.2-stable.

中文：[README.zh.md](README.zh.md)

## Why

Large projects written in a dynamic language are hard to manage. GDScript cannot leave dynamic typing behind, because it must stay compatible. Object references cannot become non-null by default. Nor can callers stop receiving a fake value and carrying on after a runtime error. Fully typed scripts still leave node paths, editor signal connections, animation tracks and resource property names dynamic. What the scenes know never reaches the scripts. C# has second-class language support in Godot, and its bridge has pitfalls of its own.

Petrichor is statically typed throughout, and the checking extends into scenes. It keeps GDScript's surface syntax, with one hard rule. **A construct spelled as in GDScript either behaves as in GDScript or is a compile error with a suggested fix.** It is never silently different. The exception is where GDScript fails at run time or silently gives a wrong result. There Petrichor raises an error instead, or does what the code meant ([§1.2](docs/language-spec.en.md#12-relationship-to-gdscript)).

## A taste

```gdscript
class_name Player extends CharacterBody3D implements Damageable

pub signal died

enum Command { IDLE, MOVE(target: Vector3), ATTACK(target: Node3D, power: int) }

@export var bullet_scene: PackedScene[Bullet]   # the editor checks the scene filled in
pub var hp: int = 100

@onready var camera = $Head/Camera              # Camera3D, typed from player.tscn

pub func take_damage(amount: int):
    hp -= amount
    if hp <= 0:
        died.emit()

async func run(command: Command):
    match command:                              # must cover every variant
        Command.IDLE:
            pass
        Command.MOVE(var target):               # var target is short for target = var target
            navigate_to(target)
        Command.ATTACK(var target, var power):
            await attack(target, power)

func max_hp_from(data: Variant) -> int?:
    if data is Dictionary:                      # data narrows to Dictionary
        return int.try_from(data.get("max_hp")?).ok()   # ? returns None if the key is missing
    return None
```

## Design highlights

- **Scenes are typed.** `$X`, `%X` and literal `get_node` paths are typed from the scenes that use the script. Signal connections, animation tracks and exported properties made in the editor are checked. Renaming a member updates the scenes too. Renaming a node in the scene editor updates the scripts ([§7](docs/language-spec.en.md#7-scene-model), [§13.3](docs/language-spec.en.md#133-refactoring)).
- **No null surprises.** Object types are non-null unless written `T?`. Option and Result follow Rust, and postfix `?` is the only special syntax ([§3.3–§3.5](docs/language-spec.en.md#33-objects-and-nullability), [§8.2](docs/language-spec.en.md#82-expected-failures)).
- **Data types.** Structs with value semantics, enums with named payloads, exhaustive `match`, interfaces and generics ([§2.4](docs/language-spec.en.md#24-declaration-spellings), [§3](docs/language-spec.en.md#3-types)).
- **Coroutines with ownership.** `async func` returns a `Task`. Cancellation follows ownership. Awaiting a freed object is an error instead of hanging forever ([§9](docs/language-spec.en.md#9-coroutines)).
- **Lives next to GDScript.** Third-party plugins stay in GDScript. GDScript's use of Petrichor instances is statically checked ([§11](docs/language-spec.en.md#11-coexisting-with-gdscript-and-c)).

Every rule that depends on engine behavior cites the Godot source it was checked against ([engine facts](docs/engine-facts.md)). Design questions that real code can answer were answered by surveys of 20 Godot projects.

## Documents

- [Language specification v0.2](docs/language-spec.en.md) · [中文](docs/language-spec.zh.md)
- [Implementation architecture](docs/architecture.md)
- [Design notes and trade-offs](docs/design-notes.md)
- [Engine facts](docs/engine-facts.md)
- Surveys:
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

The documents other than the specification are in Chinese only. Each survey comes with a directory of scripts that reproduce it.

## Next steps

1. First milestone, done: the FPS starter kit of the [second paper migration](docs/surveys/paper-migration-2.md) runs with its scripts in Petrichor.
2. Done: paying down the debt the quick first implementation left ([architecture §5](docs/architecture.md#5-推进顺序)).
3. Second milestone, done: the breakout minigame of Ring in the same migration runs with its scripts in Petrichor.
4. Next come steps 7–11 of [architecture §5](docs/architecture.md#5-推进顺序): the rest of the language, tools, performance and the first public release. That the current design allows the later ones that adoption depends on is shown in [architecture §6](docs/architecture.md#6-以后必须能做到的事). The [backlog](BACKLOG.md) lists the work itself.
5. Throughout: collect feedback and narrow the specification to a testable core.

## License

[MIT](LICENSE)
