# Petrichor

Petrichor 是面向 Godot 4 的全静态类型脚本语言，以 GDExtension 形式交付。它在项目的 `.pet` 脚本中替代 GDScript，并与 GDScript 插件及 C# 共存。类型检查延伸至 `.tscn` 场景，字节码在原生值槽位的寄存器虚拟机上运行。引擎行为按 Godot 4.7.2-stable 逐条核对（[引擎事实](docs/engine-facts.md)）。

English: [README.md](README.md)

## 为什么选择 Petrichor

动态语言让不断增长的 Godot 项目难以维护和重构。GDScript 受兼容性约束，无法摆脱动态类型。对象引用默认仍可为空，运行时出错后调用方仍会拿到假值继续执行。即使脚本写满类型，场景中的节点路径、信号连接和导出属性仍是动态的。C# 虽有静态类型，但在 Godot 中属二等支持，桥接层也有不少暗坑（[调查](docs/surveys/pitfalls.md)）。

Petrichor 在不修改引擎的前提下，将静态类型从脚本延伸到场景。它沿用 GDScript 的表层语法，并守一条硬规则（[§1.2](docs/language-spec.zh.md#12-与-gdscript-的关系)）。**与 GDScript 写法相同的构造，要么行为一致，要么在编译期报错并给出改法。** 它绝不悄悄不同。在 GDScript 运行时出错或悄悄算错的地方，Petrichor 改为报错或兑现代码本意。

## 代码一瞥

```gdscript
class_name Player extends CharacterBody3D implements Combat.Damageable

pub signal died

enum Command { IDLE, MOVE(target: Vector3), ATTACK(target: Node3D, power: int) }

@export pub var bullet_scene: PackedScene[Bullet]   # 检查器中校验填入的场景
pub var hp: int = 100

@onready var camera = $Head/Camera                  # 按 player.tscn 定为 Camera3D

pub func take_damage(amount: int):
    hp -= amount
    if hp <= 0:
        died.emit()

async func run(command: Command):
    match command:                                  # 必须覆盖每个分支
        Command.IDLE:
            velocity = Vector3.ZERO
        Command.MOVE(var target):                   # var target 是 target = var target 的简写
            velocity = global_position.direction_to(target)
        Command.ATTACK(var target, var power):
            var bullet := bullet_scene.instantiate() # 定型为 Bullet
            await bullet.fire_at(camera, target, power)

func max_hp_from(data: Variant) -> int?:
    if data is Dictionary:                          # data 收窄为 Dictionary
        return int.try_from(data.get("max_hp")?).ok()   # 缺键时 ? 返回 None
    return None
```

## 核心能力

- **场景带类型。** `$X`、`%X` 和字面量路径的 `get_node` 按挂载该脚本的场景定型。`@owner(T)` 为向上访问定型，`PackedScene[T]` 为场景实例化定型。编辑器中的信号连接与场景导出属性均受静态检查，错误直接标在场景节点上（[§7](docs/language-spec.zh.md#7-场景模型)）。
- **空安全与显式错误。** 对象类型默认非空，可空引用写作 `T?`。已释放的对象在 `T?` 上读作 `None`，不会静默悬空。预期内的失败采用 Rust 风格的 `Option` 与 `Result[T, E]`，特殊语法只有后缀 `?`（[§3.3–§3.5](docs/language-spec.zh.md#33-对象与可空)、[§8.2](docs/language-spec.zh.md#82-预期之内的失败)）。
- **丰富的数据类型。** 内置值语义的 `struct`、带命名载荷的 `enum`、穷尽的 `match`、接口与嵌套带类型容器（[§2.4](docs/language-spec.zh.md#24-声明的写法)、[§3](docs/language-spec.zh.md#3-类型)）。
- **有归属的协程。** `async func` 返回 `Task[T]`，取消沿任务归属传播。等待已释放的对象会立即报错，而不会永远挂起（[§9](docs/language-spec.zh.md#9-协程)）。
- **与 GDScript 和 C# 共存。** 第三方插件可保持为 GDScript，C# 代码也照常协同运行。Petrichor 为跨语言类建立静态视图，并静态检查使用 Petrichor 实例的 GDScript 代码（[§11](docs/language-spec.zh.md#11-与-gdscript-和-c-共存)）。
- **编辑器与命令行工具。** 编辑器支持经检查过滤的补全、悬停类型、跳转定义、无损格式化、调试与函数体热重载。导出包只含字节码而不带编译器，无界面命令行可在 CI 中执行构建与格式化（[§12](docs/language-spec.zh.md#12-运行时)、[§13](docs/language-spec.zh.md#13-工具链)）。

## 性能与真实项目验证

Petrichor 已能运行两个真实 Godot 4 游戏迁移后的脚本（[迁移调查](docs/surveys/paper-migration-2.md)）。其一是 Godot 的 3D FPS 示例项目，其二是 C# 游戏 Ring 中的打砖块小游戏。能由真实代码回答的设计取舍，均以 20 个 Godot 4 项目的可复现调查为依据。

虚拟机将标量、引擎数学值、`struct` 和带载荷 `enum` 直接存放在原生槽位中，无需装箱。下表为官方 Godot 4.7.2-stable 上的部分中位数耗时，单位毫秒（[完整 14 项基准与测量方法](docs/architecture.md#4-性能)）：

| 组别 | 基准 | GDScript (ms) | Petrichor (ms) |
|---|---|---:|---:|
| 纯计算 | `int_loop` | 34.94 | 9.98 |
| 纯计算 | `calls` | 79.44 | 23.34 |
| 纯计算 | `members` | 30.98 | 12.09 |
| 纯计算 | `vectors` | 34.48 | 16.35 |
| 引擎边界 | `property_get` | 10.29 | 6.22 |
| 分配 | `closures` | 26.45 | 18.32 |
| 分配 | `coroutines` | 25.94 | 20.47 |
| 32 脚本启动 | `cold`（构建加运行） | 3069.6 | 3078.6 |

## 构建与命令行

0.1 版本将提供 Windows、Linux 和 macOS 的预编译 GDExtension 库。使用 SCons 和 C++17 工具链从源码构建时，先初始化子模块：

```sh
git submodule update --init
scons                        # 编辑器与编译器库
scons target=template_debug  # 不含编译器的导出运行时库
scons compiler_tests         # 独立编译器测试套件
```

配合 Godot 4.7 编辑器二进制，还可在 CI 中以无界面模式执行项目构建与格式化检查：

```sh
godot --headless -e --path <project> -- --petrichor-build
godot --headless -e --path <project> -- --petrichor-format --check
```

## 路线图

- **0.1（首次公开发布，进行中）。** 补齐第一版语言覆盖面、场景检查与编辑器工具，提供英文版语言规范。发布三大桌面平台预编译库，收集真实项目反馈。
- **0.2–0.3。** 加入跨场景重命名、快速修复、lint、LSP 及 GDScript 双向迁移工具。
- **1.0。** 稳定语言语义与字节码格式版本号，支持 Godot 4.4 及之后的版本。提供 Android、iOS 和 Web 的预编译导出库。
- **1.0 之后。** 加入原生代码生成、带检查的 Mod 加载、`struct` 与带载荷 `enum` 的导出，以及泛型（[§14.2](docs/language-spec.zh.md#142-以后可以兼容地加入)）。

里程碑顺序，以及为 Mod、全平台导出和多人重构保留的设计约束，见[实现架构 §5–§6](docs/architecture.md#5-推进顺序)。未完成的工作与待定问题记录在 [BACKLOG.md](BACKLOG.md)。

## 文档

- [语言规范（v0.2）](docs/language-spec.zh.md)：语言、场景模型与运行时的规范性规则。
- [实现架构](docs/architecture.md)：编译器、字节码虚拟机、引擎适配层、性能基准与推进路线。
- [设计说明与取舍](docs/design-notes.md)：规则背后的取舍依据、语料计数与被否决的方案。
- [引擎事实](docs/engine-facts.md)：按 Godot 4.7.2-stable 源码核对的引擎行为出处。
- [开发说明](docs/development.md)：供贡献者参考的构建、测试与调试经验。
- 基于 20 个 Godot 4 项目的实证调查（每份均附复现脚本目录）：
  - [脚本会见到不止一种场景结构吗？](docs/surveys/scene-structures.md)
  - [原生基类不同造成了多少代码重复？](docs/surveys/cross-branch-duplication.md)
  - [引擎、GDScript 和 C# 桥接的暗坑](docs/surveys/pitfalls.md)
  - [真实的 Godot 4 项目怎样使用属性的 getter 和 setter？](docs/surveys/property-accessors.md)
  - [await 多参数信号的结果有多常被使用？](docs/surveys/await-signal-results.md)
  - [脚本成员在哪里被使用：本文件、子类、其他脚本？](docs/surveys/member-visibility.md)
  - [按现行设计迁移真实代码的成本](docs/surveys/paper-migration-2.md)
  - [按 v0.1 规范迁移真实代码的成本](docs/surveys/paper-migration.md)
  - [计算真实代码中的常量需要什么？](docs/surveys/constant-expressions.md)
  - [真实代码用到而实现尚不支持的写法有哪些？](docs/surveys/implementation-coverage.md)
  - [语料中的零散计数](docs/surveys/corpus-counts.md)

`docs/` 下的文档目前为中文。英文版语言规范随 0.1 提供。

## 许可证

[MIT](LICENSE)
