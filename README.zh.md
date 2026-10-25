# Petrichor

一门面向中大型 Godot 4 项目的全静态类型脚本语言，在项目代码中替代 GDScript 并与 GDScript 插件共存，以 GDExtension 的形式交付。Petrichor 是项目代号。

**状态**：早期实现阶段。[语言规范](docs/language-spec.zh.md)（v0.2）是发布出来征求意见的草案。语言的第一个子集已经能在 Godot 中运行，实现方式见[实现架构](docs/architecture.zh.md)；第一个里程碑是让一个小型 FPS 游戏的脚本运行起来。引擎行为按 Godot 4.7.2-stable 核对。

English: [README.md](README.md)

## 为什么

用动态语言写的大型项目难以管理。GDScript 受兼容性约束，摆脱不了动态类型：对象引用无法改为默认非空，运行时出错后调用方拿到一个假值继续执行的行为也改不掉。即使脚本全部写成静态类型，节点路径、编辑器中连接的信号、动画轨道和资源的属性名仍然是动态的：场景里的信息到不了脚本里。C# 在 Godot 中是二等的语言支持，跨语言桥接也有自己的暗坑。

Petrichor 从头到尾是静态类型的，检查一直延伸到场景。它沿用 GDScript 的表层语法，并守一条硬规则：**与 GDScript 写法相同的构造，行为要么与 GDScript 一致，要么是编译错误并给出改法**，不会悄悄不同。

## 一瞥

```gdscript
class_name Player extends CharacterBody3D implements Damageable

pub signal died

enum Command { IDLE, MOVE(target: Vector3), ATTACK(target: Node3D, power: int) }

@export var bullet_scene: PackedScene[Bullet]   # 编辑器检查填入的场景
pub var hp: int = 100

@onready var camera = $Head/Camera              # 按 player.tscn 定为 Camera3D

pub func take_damage(amount: int):
    hp -= amount
    if hp <= 0:
        died.emit()

async func run(command: Command):
    match command:                              # 必须覆盖每个分支
        Command.IDLE:
            pass
        Command.MOVE(var target):               # var target 是 target = var target 的简写
            navigate_to(target)
        Command.ATTACK(var target, var power):
            await attack(target, power)

func max_hp_from(data: Variant) -> int?:
    if data is Dictionary:                      # data 收窄为 Dictionary
        return int.try_from(data.get("max_hp")?).ok()   # 缺键时 ? 返回 None
    return None
```

## 设计要点

- **场景有类型。** `$X`、`%X` 和字面量路径的 `get_node` 按使用该脚本的场景定型；编辑器中做的信号连接、动画轨道和导出属性都受检查；给成员改名时连同场景一起修改，在场景编辑器中给节点改名时连同脚本一起修改（[§7](docs/language-spec.zh.md#7-场景模型)、[§13.3](docs/language-spec.zh.md#133-重构)）。
- **不会意外遇到 null。** 对象类型默认非空，可空的写作 `T?`；Option 和 Result 采用 Rust 的模型，唯一的特殊语法是后缀 `?`（[§3.3–§3.5](docs/language-spec.zh.md#33-对象与可空)、[§8.2](docs/language-spec.zh.md#82-预期之内的失败)）。
- **数据类型。** 值语义的 struct、带命名载荷的 enum、穷尽的 `match`、接口和泛型（[§2.4](docs/language-spec.zh.md#24-声明的写法)、[§3](docs/language-spec.zh.md#3-类型)）。
- **有归属的协程。** `async func` 返回 `Task`；取消沿归属传播；等待的对象被释放时报错，而不是永远挂起（[§9](docs/language-spec.zh.md#9-协程)）。
- **与 GDScript 共存。** 第三方插件保持 GDScript，GDScript 使用 Petrichor 类的实例时同样有静态检查（[§11](docs/language-spec.zh.md#11-与-gdscript-和-c-共存)）。

每一条依赖引擎行为的规则，都注明了核对过的 Godot 源码位置（[附录 A](docs/language-spec.zh.md#附录-a引擎事实)）；能用真实代码回答的设计问题，都在 20 个 Godot 项目的代码上做了调查。

## 文档

- [语言规范 v0.2](docs/language-spec.zh.md) · [English](docs/language-spec.en.md)
- [实现架构](docs/architecture.zh.md) · [English](docs/architecture.en.md)
- 调查：
  - [脚本会见到不止一种场景结构吗？](docs/surveys/scene-structures.zh.md)
  - [原生基类不同造成了多少代码重复？](docs/surveys/cross-branch-duplication.zh.md)
  - [引擎、GDScript 和 C# 桥接的暗坑](docs/surveys/pitfalls.zh.md)
  - [真实的 Godot 4 项目怎样使用属性的 getter 和 setter？](docs/surveys/property-accessors.zh.md)
  - [await 多参数信号的结果有多常被使用？](docs/surveys/await-signal-results.zh.md)
  - [脚本成员在哪里被使用：本文件、子类、其他脚本？](docs/surveys/member-visibility.zh.md)
  - [按现行设计迁移真实代码的成本](docs/surveys/paper-migration-2.zh.md)
  - [按 v0.1 规范迁移真实代码的成本](docs/surveys/paper-migration.zh.md)
  - [语料中的零散计数](docs/surveys/corpus-counts.zh.md)

每份调查都有中英文报告，以及可以复现它的脚本目录。

## 下一步

1. 第一个里程碑，进行中：[第二次纸面迁移](docs/surveys/paper-migration-2.zh.md)中的 FPS 项目以 Petrichor 脚本运行，所需的 [§15](docs/language-spec.zh.md#15-一致性检查清单非规范性) 检查清单条目同时写成测试。
2. 之后是同一次迁移中的 Ring 代码、带重命名的语言服务器、迁移用的快速修复，以及与 GDScript 的性能基准。
3. 贯穿始终：收集反馈，把规范收敛到可测试的核心。

## 反馈

欢迎通过 [GitHub issue](https://github.com/YYF233333/Petrichor/issues) 反馈。建议的格式：问题 → 最小反例 → 章节 → 严重程度 → 最小修改 → 测试。

## 许可证

[MIT](LICENSE)
