# 调查：真实代码用到而实现尚不支持的写法

**2026-11-01** · 关联：[语言规范](../language-spec.zh.md)、[实现架构](../architecture.zh.md) §2.6 · English: [implementation-coverage.en.md](implementation-coverage.en.md)

## 问题

两个里程碑让 FPS 和 Ring 的脚本以本语言运行（架构 §5 第 4、6 步），但只覆盖了这两个项目用到的写法。本文统计：在真实的 GDScript 项目中，有多少文件用到了规范第一版包含、而提交 `812aaa2` 的实现尚不支持的写法。

## 语料

与[跨分支重复调查](cross-branch-duplication.zh.md)相同：20 个仓库，固定提交，第一方代码的判定规则也相同（`cross-branch-duplication/gdcorpus.py`）。第一方 `.gd` 文件 2,800 个，共 203,720 行代码（不计空行和注释行）；其中 12 个游戏仓库有 1,223 个文件、70,646 行。

## 方法

- 按词法识别下表中的 16 种写法，每种都是规范第一版的内容，而实现会报“尚不支持”或语法错误。
- `Type.m()` 只在 Type 是引擎类、内置类型或本仓库 `class_name` 的类，且 m 是它的静态方法时计入；引擎一侧的静态方法取自 godot-cpp 的 `extension_api-4-7.json`。`Type.new()` 只在 Type 是本仓库 `class_name` 的类时计入；引擎类的 `new()` 已经支持。
- 计入的注解：类注解 `@tool`、`@icon`、`@static_unload`、`@abstract`；成员注解中除 `@export`、`@export_range`、`@export_enum`、`@export_multiline`、`@onready` 和分组注解以外的全部，例如 `@export_node_path`、`@export_flags`、`@warning_ignore`、`@rpc`。
- 行首为 `for` 的一行中，第一个 `in` 算作循环的，其余的 `in` 算作运算符。
- 一个文件用到其中任何一种，就计为“用到”。

## 结果

| 写法 | 规范 | 全部：文件 | 全部：每千行 | 游戏：文件 | 游戏：每千行 |
|---|---|---:|---:|---:|---:|
| 一种都没用到的文件 | | 29.6% | | 40.6% | |
| `const` | §4.5 | 29.8% | 15.2 | 28.6% | 20.9 |
| `Type.m()`，本仓库类的静态方法 | §4.8 | 22.9% | 16.2 | 20.4% | 12.1 |
| `&"…"`、`^"…"` 字面量 | §2.2 | 20.3% | 19.5 | 20.4% | 27.5 |
| 其他成员注解 | §2.3 | 13.4% | 6.2 | 5.2% | 2.4 |
| `@tool`、`@icon`、`@static_unload`、`@abstract` | §2.3 | 13.4% | 3.4 | 5.8% | 1.1 |
| `Type.new()`，本仓库的类 | §4.6 | 13.2% | 6.8 | 6.3% | 1.5 |
| `Type.m()`，引擎类或内置类型的静态方法 | §10 | 9.9% | 4.9 | 3.6% | 1.8 |
| `preload()` 场景以外的资源 | §7.4 | 9.7% | 3.3 | 8.7% | 4.6 |
| lambda | §3.11 | 8.5% | 4.9 | 7.4% | 3.3 |
| `%Name`、`$".."` | §7.1、§7.3 | 7.8% | 4.8 | 3.4% | 2.1 |
| `in`、`not in` | §5.1 | 7.4% | 2.8 | 4.9% | 1.4 |
| `enum` | §3.7 | 6.7% | 1.3 | 4.7% | 1.0 |
| `assert` | §8.3 | 4.4% | 2.9 | 5.2% | 1.7 |
| 内部类 | §2.1 | 4.3% | 0.9 | 1.6% | 0.5 |
| 位运算 | §3.2、§5.1 | 3.1% | 1.8 | 1.2% | 1.0 |
| `static var` | §4.8 | 2.7% | 0.9 | 2.9% | 1.2 |
| `**` | §5.1 | 0.1% | 0.0 | 0 | 0 |

各仓库中一种都没用到的文件：

| 仓库 | 类别 | 文件 | 一种都没用到 |
|---|---|---:|---:|
| Starter-Kit-FPS | game | 7 | 7 |
| Starter-Kit-3D-Platformer | game | 9 | 9 |
| Starter-Kit-Racing | game | 3 | 3 |
| Starter-Kit-City-Builder | game | 6 | 5 |
| Ring | game | 66 | 55 |
| tanks-of-freedom-ii | game | 292 | 213 |
| godot_open_star_fighter | game | 40 | 25 |
| deck_builder_tutorial | game | 122 | 44 |
| godot-open-rts | game | 118 | 39 |
| tps-demo | game | 15 | 3 |
| godot-open-rpg | game | 78 | 15 |
| godot-tiny-mmo | game | 467 | 79 |
| godot-demo-projects | demos | 461 | 147 |
| Pixelorama | tool | 210 | 27 |
| godot-statecharts | plugin | 89 | 41 |
| gdUnit4 | plugin | 482 | 92 |
| beehave | plugin | 95 | 9 |
| phantom-camera | plugin | 36 | 3 |
| Godot-Game-Template | plugin | 125 | 9 |
| godot_dialogue_manager | plugin | 79 | 4 |

每种写法的分布和抽样位置见 `implementation-coverage/results/coverage.txt`，加 `--samples N` 运行脚本可列出每种的前 N 处。

## 局限

- 按词法识别，不做类型解析。经常量或变量间接调用的静态方法不计，例如 `const Utils = preload("utils.gd")` 之后的 `Utils.f()`；这类文件已经因为 `const` 和 `preload` 计入。
- 一种都没用到，不等于文件能编译。迁移还要补类型，[第二次纸面迁移](paper-migration-2.zh.md)中每百行约 28 处修改；尚未实现、本文也不统计的还有无类型的值（规范 §3.13）、match 的数组和字典模式、字面量形式的属性名（§10.4）、不导出的必须赋值成员（§6.2）等。
- 规范中本语言新增的构造，如 Result、泛型、带载荷的 enum，不在 GDScript 中出现，本文无法统计。

## 对实现的意义

- 在全部仓库中，七成文件至少用到一种尚未实现的写法，游戏仓库中约六成。最常见的几种都是小特性：`const`、经类名调用静态方法和 `new()`、`&"…"` 字面量、注解。
- 两个里程碑的项目几乎不用这些写法：FPS 的 7 个文件全部、Ring 的 66 个文件中 55 个一种都没用到，而全部仓库中只有 29.6%。所以两次迁移都没有暴露这些缺口。

## 复现

```
python docs/surveys/cross-branch-duplication/gdcorpus.py corpus
python docs/surveys/implementation-coverage/coverage.py corpus thirdparty/godot-cpp/gdextension/extension_api-4-7.json > docs/surveys/implementation-coverage/results/coverage.txt
```
