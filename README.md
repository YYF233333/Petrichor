# Petrichor

A fully statically typed scripting language for medium-to-large Godot 4 projects, replacing GDScript in project code while coexisting with GDScript plugins, delivered as a GDExtension. Petrichor is a codename.

**Status:** early implementation. The [language specification](docs/language-spec.en.md) (v0.2) is a draft published for feedback. A first subset of the language runs in Godot, built as described in the [implementation architecture](docs/architecture.en.md); with it, the first milestone is reached: the scripts of a small FPS game run in Petrichor. Engine behavior is checked against Godot 4.7.2-stable.

中文：[README.zh.md](README.zh.md)

## Why

Large projects written in a dynamic language are hard to manage. GDScript cannot leave dynamic typing behind because of compatibility: object references cannot become non-null by default, nor can callers stop receiving a fake value and carrying on after a runtime error. Even fully typed scripts leave node paths, signals connected in the editor, animation tracks and resource property names dynamic: what the scenes know never reaches the scripts. C# has second-class language support in Godot and a bridge with its own pitfalls.

Petrichor is statically typed throughout, and the checking extends into scenes. It keeps GDScript's surface syntax, with one hard rule: **a construct spelled as in GDScript either behaves as in GDScript or is a compile error with a suggested fix**, never silently different. The exception is where GDScript fails at run time or silently gives a wrong result: there Petrichor raises an error instead, or does what the code meant ([§1.2](docs/language-spec.en.md#12-relationship-to-gdscript)).

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

- **Scenes are typed.** `$X`, `%X` and literal `get_node` paths are typed from the scenes that use the script; signal connections, animation tracks and exported properties made in the editor are checked; renaming a member updates the scenes too, and renaming a node in the scene editor updates the scripts ([§7](docs/language-spec.en.md#7-scene-model), [§13.3](docs/language-spec.en.md#133-refactoring)).
- **No null surprises.** Object types are non-null unless written `T?`; Option and Result follow Rust, and postfix `?` is the only special syntax ([§3.3–§3.5](docs/language-spec.en.md#33-objects-and-nullability), [§8.2](docs/language-spec.en.md#82-expected-failures)).
- **Data types.** Structs with value semantics, enums with named payloads, exhaustive `match`, interfaces and generics ([§2.4](docs/language-spec.en.md#24-declaration-spellings), [§3](docs/language-spec.en.md#3-types)).
- **Coroutines with ownership.** `async func` returns a `Task`; cancellation follows ownership, and awaiting a freed object is an error instead of hanging forever ([§9](docs/language-spec.en.md#9-coroutines)).
- **Lives next to GDScript.** Third-party plugins stay in GDScript, and GDScript's use of Petrichor instances is statically checked ([§11](docs/language-spec.en.md#11-coexisting-with-gdscript-and-c)).

Every rule that depends on engine behavior cites the Godot source it was checked against ([Appendix A](docs/language-spec.en.md#appendix-a-engine-facts)), and design questions that real code can answer were answered by surveys of the code of 20 Godot projects.

## Documents

- [Language specification v0.2](docs/language-spec.en.md) · [中文](docs/language-spec.zh.md)
- [Implementation architecture](docs/architecture.en.md) · [中文](docs/architecture.zh.md)
- Surveys:
  - [Do scripts see more than one scene structure?](docs/surveys/scene-structures.en.md)
  - [How much code duplication do different native base classes force?](docs/surveys/cross-branch-duplication.en.md)
  - [Pitfalls of the engine, GDScript and the C# bridge](docs/surveys/pitfalls.en.md)
  - [How do real Godot 4 projects use property getters and setters?](docs/surveys/property-accessors.en.md)
  - [How often is the result of awaiting a multi-parameter signal used?](docs/surveys/await-signal-results.en.md)
  - [Where are script members used: own file, subclasses, other scripts?](docs/surveys/member-visibility.en.md)
  - [What does it cost to migrate real code to the current design?](docs/surveys/paper-migration-2.en.md)
  - [What did it cost to migrate real code to spec v0.1?](docs/surveys/paper-migration.en.md)
  - [One-off counts in the corpus](docs/surveys/corpus-counts.en.md)

Each survey has a write-up in English and Chinese and a directory of scripts that reproduce it.

## Next steps

1. First milestone, done: the FPS starter kit of the [second paper migration](docs/surveys/paper-migration-2.en.md) runs with its scripts in Petrichor, and the rows of the checklist in [§15](docs/language-spec.en.md#15-conformance-checklist-non-normative) that running it needs became tests; what it leaves for later is listed in the [backlog](BACKLOG.md).
2. Done: paying down the debt the quick first implementation left, in four groups: correctness, verification, code and documents ([architecture §5](docs/architecture.en.md#5-order-of-work)).
3. Now: the second milestone, the breakout of Ring in the same migration running with its scripts in Petrichor ([architecture §5](docs/architecture.en.md#5-order-of-work)).
4. The rest is listed in the [backlog](BACKLOG.md), in no fixed order; among it are the language server with renaming, quick fixes for migration, and performance work against GDScript.
5. Throughout: collect feedback and narrow the specification to a testable core.

## Feedback

Feedback is welcome as [GitHub issues](https://github.com/YYF233333/Petrichor/issues). A suggested format: problem → minimal counterexample → section → severity → minimal revision → test.

## License

[MIT](LICENSE)
