# 调查：一个脚本会见到几种场景结构

**2026-10-23** · 关联：[设计决定](../decisions.zh.md)「场景与节点」 · English: [scene-structures.en.md](scene-structures.en.md)

`$X`、`%X` 和 `get_node("…")` 的静态类型，可以取自挂载脚本的场景（以场景为准），也可以取自脚本中的声明。以场景为准要干净，前提是一个脚本只见到一种场景结构，或者它见到的各种结构对它用到的每个路径给出同一个类型。本文检验这个前提：在 20 个仓库中按引擎的规则合成每个文本场景的完整节点树，统计每个脚本见到几种结构，再在每种结构中解析它的每一处字面路径。

## 问题

GDScript 把 `$A/B` 和 `%A` 编译成以常量 NodePath 调用 `self.get_node`，分析器把结果的类型定为 `Node`。代码补全另有做法：它在当前编辑的场景中找到挂着该脚本的节点，对它调用 `get_node_or_null`，按找到的节点给出补全。所以 GDScript 编译时不看场景，补全时只看当前打开的那一个场景（见下文“引擎依据”）。

候选做法有两种：

| 做法 | 字面路径的静态类型取自 | 场景的作用 |
|---|---|---|
| A 场景为准 | 挂载该脚本或其子类的所有场景：在每个场景中解析路径，取找到的节点的类型 | 类型的来源 |
| B 脚本声明 | 脚本中对所需节点（路径和类型）的声明 | 与声明对照检查 |

一个脚本若只见到一种结构，A 可以直接给出类型。若见到多种，同一个表达式可能在不同场景中指向不同类型的节点，或者在某些场景中不存在，A 就需要额外的规则。B 不依赖结构是否唯一，但要把场景里已有的信息在脚本中再写一遍。我们想知道：

- 有多少脚本见到两种以上的结构，其中多少使用字面路径？
- 字面路径在不同结构中是否指向不同类型的节点？是否在部分结构中缺失？
- 多种结构从何而来，这些脚本是做什么的？
- 有多少节点访问落在挂载场景的子树之外（向上访问、绝对路径、运行时添加的节点），因而两种做法都不直接涉及？

## 语料

| 仓库 | 类别 | 提交 | 第一方 `.gd` | 行数 | `.tscn` | 继承场景 | 用字面路径的脚本 | 见到 ≥2 种结构 | 其中用字面路径 |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| [godotengine/godot-demo-projects](https://github.com/godotengine/godot-demo-projects) | 示例 | `b761b4c` | 461 | 30,819 | 396 | 8 | 235 | 29 | 11 |
| [gdquest-demos/godot-open-rpg](https://github.com/gdquest-demos/godot-open-rpg) | 游戏 | `d59f2f7` | 78 | 5,422 | 128 | 22 | 34 | 14 | 9 |
| [KenneyNL/Starter-Kit-3D-Platformer](https://github.com/KenneyNL/Starter-Kit-3D-Platformer) | 游戏 | `3fa8a04` | 9 | 378 | 10 | 6 | 6 | 0 | 0 |
| [KenneyNL/Starter-Kit-City-Builder](https://github.com/KenneyNL/Starter-Kit-City-Builder) | 游戏 | `4535092` | 6 | 311 | 1 | 0 | 1 | 0 | 0 |
| [KenneyNL/Starter-Kit-FPS](https://github.com/KenneyNL/Starter-Kit-FPS) | 游戏 | `185fd23` | 7 | 451 | 10 | 6 | 3 | 0 | 0 |
| [KenneyNL/Starter-Kit-Racing](https://github.com/KenneyNL/Starter-Kit-Racing) | 游戏 | `2f2e5f2` | 3 | 258 | 4 | 0 | 3 | 1 | 1 |
| [godotengine/tps-demo](https://github.com/godotengine/tps-demo) | 游戏 | `a82f154` | 15 | 1,619 | 17 | 1 | 10 | 1 | 1 |
| [Orama-Interactive/Pixelorama](https://github.com/Orama-Interactive/Pixelorama) | 工具 | `38a8b03` | 210 | 54,782 | 120 | 45 | 102 | 10 | 6 |
| [ramokz/phantom-camera](https://github.com/ramokz/phantom-camera) | 插件 | `21d3762` | 36 | 10,410 | 32 | 0 | 13 | 7 | 1 |
| [nathanhoad/godot_dialogue_manager](https://github.com/nathanhoad/godot_dialogue_manager) | 插件 | `7a72dcf` | 79 | 14,619 | 24 | 0 | 16 | 1 | 1 |
| [bitbrain/beehave](https://github.com/bitbrain/beehave) | 插件 | `fe58915` | 95 | 8,566 | 27 | 0 | 7 | 13 | 0 |
| [derkork/godot-statecharts](https://github.com/derkork/godot-statecharts) | 插件 | `76d226a` | 89 | 6,852 | 43 | 0 | 17 | 6 | 0 |
| [MikeSchulze/gdUnit4](https://github.com/MikeSchulze/gdUnit4) | 插件 | `9bc1a45` | 482 | 54,133 | 29 | 0 | 22 | 1 | 0 |
| [Maaack/Godot-Game-Template](https://github.com/Maaack/Godot-Game-Template) | 插件 | `6849d6c` | 125 | 8,553 | 119 | 56 | 53 | 24 | 12 |
| [lampe-games/godot-open-rts](https://github.com/lampe-games/godot-open-rts) | 游戏 | `a628ad3` | 118 | 7,415 | 84 | 9 | 2 | 9 | 1 |
| [P1X-in/tanks-of-freedom-ii](https://github.com/P1X-in/tanks-of-freedom-ii) | 游戏 | `dbe629e` | 292 | 24,361 | 661 | 449 | 131 | 15 | 15 |
| [guladam/deck_builder_tutorial](https://github.com/guladam/deck_builder_tutorial) | 游戏 | `91b408b` | 122 | 4,830 | 61 | 0 | 43 | 7 | 1 |
| [SlayHorizon/godot-tiny-mmo](https://github.com/SlayHorizon/godot-tiny-mmo) | 游戏 | `f4ed04f` | 467 | 54,379 | 123 | 14 | 38 | 20 | 1 |
| [teeeece/godot_open_star_fighter](https://github.com/teeeece/godot_open_star_fighter) | 游戏 | `6e7be5c` | 40 | 2,408 | 26 | 3 | 18 | 5 | 3 |
| [YYF233333/Ring](https://github.com/YYF233333/Ring) | 游戏 | `0ec4161` | 66 | 3,272 | 76 | 0 | 27 | 3 | 2 |
| **合计** | | | **2,800** | **293,838** | **1,991** | **619** | **781** | **166** | **65** |

前 14 个仓库与[属性访问器调查](property-accessors.zh.md#语料)相同，提交也相同。为了增加游戏代码，另选了 5 个开源游戏，取 2026-10-23 的 HEAD。Ring 与[纸面迁移调查](paper-migration.zh.md#素材)是同一提交。下文按“类别”一列分三组：游戏 12 个仓库，示例 1 个（godot-demo-projects），插件与工具 7 个。“继承场景”包括继承自导入模型的场景。“见到 ≥2 种结构”把子类脚本的上下文也算在内（见“方法”）。

**许可。** 前 14 个仓库的许可见属性访问器调查，代码都是 MIT。新增的 5 个游戏都是 MIT，其中 tanks-of-freedom-ii 的许可文件写明 MIT 同时适用于代码和图形素材。Ring 没有许可文件；经仓库所有者同意，本文收录其代码片段。

**第一方**指 `addons/` 以外的全部代码，加上仓库本身发布的插件（beehave、gdUnit4、dialogue_manager、godot_state_charts、phantom_camera，以及 godot-tiny-mmo 的 tinymmo）、godot-demo-projects 的全部 `addons/`，和 Godot-Game-Template 的全部 `addons/`（五个插件的 plugin.cfg 作者相同）。其余 `addons/` 是随附代码，例如 Ring 中的 Dialogue Manager、godot-tiny-mmo 中的 godot-sqlite 和 httpserver，它们的脚本不计入，但场景文件全部参与合成。与属性访问器调查不同，这里的第一方包括单元测试，因此 gdUnit4、beehave 等仓库的行数与那份调查不同。行数是第一方 `.gd` 文件的物理行数。

**检出范围。** 每个仓库只稀疏检出 `.gd`、`.tscn`、`.tres`、`.scn`、`.uid`、`.cs`、`project.godot`、`plugin.cfg` 和根目录的许可文件。导入的模型（`.glb`、`.dae` 等）没有检出。

## 方法

1. **合成场景树。** `analyze.py` 解析每个 `.tscn`，按引擎加载文本场景和实例化的规则，构造运行时会得到的完整节点树：递归展开实例化的子场景；继承场景先构造基础场景，再应用自己的改动；外层场景在实例内部添加的节点和对已有节点的覆盖照样应用。每个节点记下原生类型、脚本、owner，以及是否设置了 `unique_name_in_owner`。导入模型的实例和实例占位符的内部视为未知。1,991 个文本场景全部解析成功，没有找不到父节点的条目；2 个二进制 `.scn` 没有解析。原生类的继承关系取自 Godot 提交 `469659e` 的类参考 XML（`build_classes.py`，结果在 `classes.json`）。
2. **上下文。** 合成树中每个挂着某脚本的节点，都是该脚本的一个上下文：可能由该场景文件直接挂载，可能来自基础场景，也可能位于实例化的子场景内。子类脚本（直接或间接 `extends` 该脚本）的上下文也算作该脚本的上下文，因为基类的代码同样在这些节点上运行。
3. **结构。** 一个上下文的结构是：该节点所有后代的相对路径及各自的原生类型和脚本，加上 `%` 从该节点能找到的唯一名称。祖先不属于结构。两个上下文的这些内容完全相同，就算同一种结构；同一场景被原样实例化多少次，都只算一种。
4. **字面路径。** 分词器（正确跳过字符串和注释）提取对 self 的 `$A/B`、`$"A/B"`、`%A`、`$%A`，以及参数为字符串字面量（包括 `^"…"` 和 `NodePath("…")`）的 `get_node(…)` 和 `get_node_or_null(…)`。对其他对象调用的 `get_node`、非字面量参数、`has_node`、`find_child`、`get_parent()` 和 `owner.` 只计数，不解析；含 `..` 的路径和 `/root/` 路径另外单独计数（包括上述字面路径中的这两类）。
5. **解析。** 每处字面路径在该脚本的每个上下文中按引擎规则解析：`..` 取父节点；`%名称` 先查当前节点自己拥有的唯一名称，再查其 owner 的；其他名称查子节点；绝对路径不解析。每处用法得到一个结果：
   - 各结构中类型相同：在每个能解析的上下文中都找到节点，且（原生类型，脚本）只有一种。
   - 类型不同：找到的节点有两种以上（原生类型，脚本）。
   - 部分结构中缺失：在一些上下文中找到，在另一些中找不到。
   - 其余：所有结构中都找不到；路径进入导入模型或占位符，无法判断；经 `..` 离开已知的树；绝对路径；脚本没有任何上下文。
6. **形成机制。** 对见到 ≥2 种结构的脚本，标出多出来的结构从何而来，可以有多个：
   - M1：被两个以上场景文件直接挂载，结构不同。M1b：在同一个场景文件中挂在多个子树不同的节点上。
   - M2：挂载它的场景被其他场景继承，继承场景中的副本结构不同。
   - M3：挂载它的场景在别处被实例化，外层场景改动了实例内部。
   - M4：外层场景把它设到一个实例化的节点上。
   - M5：子类脚本挂在别的场景上，带来新的结构。

   归类时按“继承（M2 或 M5）、独立挂载（M1 或 M1b）、实例改动（M3）”的先后，把每个脚本归入第一个符合的机制。
7. **用途。** 游戏组中见到 ≥2 种结构的 75 个脚本，由 AI 代理逐个阅读脚本和场景后按用途归类，结果记录在 `purpose_labels.csv`。
8. **自检。** `test_fixture.py` 在一个合成的小项目上检查以上规则：同一脚本挂在两个场景、继承场景、在实例内部添加节点、两个子类挂在不同场景、唯一名称。`check_tokens.py` 用朴素的正则表达式对照分词器的计数；抽查到的差异都是字符串或注释中的 `$`，是正则误计。

下文的数字都由 `analyze.py`、`report.py` 和 `summarize.py` 算出。引用的代码行和场景内容，是逐个打开文件核对的。

### 引擎依据

以下事实在 Godot 提交 `469659e` 上核对，路径相对于 Godot 仓库根目录：

- GDScript 把 `$`、`%` 表达式编译为以常量 NodePath 调用 `Node.get_node`（`modules/gdscript/gdscript_compiler.cpp:748-757`），分析器把它的类型定为 `Node`（`modules/gdscript/gdscript_analyzer.cpp:3942-3946`）。
- 代码补全在当前编辑的场景中找第一个归场景根节点所有、且挂着该脚本的节点（`editor/script/script_text_editor.cpp:1127-1152`），对它调用 `get_node_or_null`，以找到的节点的类和脚本作为补全依据的类型（`modules/gdscript/gdscript_editor.cpp:3413-3437`）。
- `Node::get_node_or_null` 逐段解析路径：绝对路径从树根开始（`scene/main/node.cpp:1933-1940`）；`..` 取父节点（`:1949-1954`）；`%名称` 先查当前节点自己的唯一名称表，查不到再查其 owner 的（`:1960-1968`）；其他名称查子节点（`:1969-1976`）。`get_node` 找不到时报错并返回 null（`:1984-1995`）。
- 唯一名称登记在 owner 的表中；同一 owner 下已有同名节点时，后来者不登记并给出警告（`node.cpp:2266-2278`）。实例化时，每个节点设好 owner 后登记唯一名称（`scene/resources/packed_scene.cpp:640-647`）；文本场景中没有写 `owner` 的节点归根节点所有（`scene/resources/resource_format_text.cpp:262-265`）。子场景作为一个整体实例化（`packed_scene.cpp:334-337`），所以子场景中的节点归子场景的根节点所有，而子场景的根节点归外层场景的根节点所有。因此，从子场景根节点出发的 `%名称` 先查子场景自己的唯一名称，再查外层场景的。
- 文本场景中第一个节点带 `instance` 而没有 `parent` 时，该文件是继承场景（`resource_format_text.cpp:233-239`）；实例化时先构造基础场景作为根（`packed_scene.cpp:300-309`）。没有 `type` 的节点条目指向基础场景或实例中已有的节点（`resource_format_text.cpp:218-222`；`packed_scene.cpp:353-356`），按名称找不到时引擎还会按 `unique_id` 找（`packed_scene.cpp:357-371`）。`[editable path=…]` 记录可编辑的实例（`resource_format_text.cpp:384-393`）；`instance_placeholder` 生成占位符（`resource_format_text.cpp:242-254`；`packed_scene.cpp:312-332`）。

## 结果

### 有多少脚本见到多种结构

| | 游戏 | 示例 | 插件与工具 |
|---|---:|---:|---:|
| 第一方脚本 | 1,223 | 461 | 1,116 |
| 有上下文（自己的或子类的） | 647 | 414 | 390 |
| 使用字面路径 | 316 | 235 | 230 |
| 见到 ≥2 种结构 | 75 | 29 | 62 |
| 　只算自己的上下文时 | 44 | 21 | 38 |
| 见到 ≥2 种结构且使用字面路径 | **34** | 11 | 20 |
| 字面路径用法 | 2,023 | 1,674 | 1,534 |
| 　其中在见到 ≥2 种结构的脚本中 | 180 | 43 | 107 |

- 在游戏中，见到 ≥2 种结构且使用字面路径的脚本有 34 个，占全部第一方脚本的 2.8%，占有上下文的脚本的 5.3%，占使用字面路径的脚本的 10.8%。其中 tanks-of-freedom-ii 有 15 个，godot-open-rpg 有 9 个。
- 593 个游戏脚本没有被任何场景文件直接挂载：330 个不是节点脚本，113 个挂在资源上，44 个是 autoload，41 个是只通过子类挂载的基类，7 个是编辑器脚本，其余 58 个是没有场景挂载的节点脚本。没有任何上下文却使用字面路径的游戏脚本有 9 个：6 个 autoload，3 个其他节点脚本。
- 原样实例化不会增加结构。游戏中有 323 个脚本在别处被原样实例化，共 5,914 个这样的上下文；其中 309 个脚本始终只有一种结构。

### 字面路径在各结构中是否一致

| 结果 | 游戏 | 其中 ≥2 种结构 | 示例 | 其中 ≥2 种结构 | 插件与工具 | 其中 ≥2 种结构 |
|---|---:|---:|---:|---:|---:|---:|
| 各结构中类型相同 | 1,849 | 145 | 1,640 | 43 | 1,500 | 98 |
| **类型不同** | **0** | **0** | **0** | **0** | **0** | **0** |
| 部分结构中缺失 | 16 | 15 | 0 | 0 | 10 | 8 |
| 所有结构中都找不到 | 11 | 0 | 10 | 0 | 4 | 0 |
| 进入导入模型，无法判断 | 14 | 5 | 16 | 0 | 0 | 0 |
| 经 `..` 离开已知的树 | 0 | 0 | 1 | 0 | 2 | 1 |
| 绝对路径 | 120 | 15 | 2 | 0 | 0 | 0 |
| 脚本没有上下文 | 13 | 0 | 5 | 0 | 18 | 0 |
| **合计** | **2,023** | **180** | **1,674** | **43** | **1,534** | **107** |

- 5,231 处字面路径用法中，没有一处在两种结构中指向不同类型的节点。见到 ≥2 种结构的游戏脚本有 180 处用法，其中 160 处在两个以上的上下文中解析成功，同样没有一处类型不同。
- 在字面路径上，结构之间的差别只表现为节点在不在：游戏中 15 处在部分结构中缺失（见下一节）；插件与工具中 8 处，都在 Pixelorama 的 `src/Classes/ImageEffect.gd`，这些节点在 18 个上下文中存在，在 `ShaderEffect.tscn` 中没有。
- 120 处绝对路径中有 115 处在 tanks-of-freedom-ii，大多是 `$"/root/SimpleAudioLibrary"` 这样按名字访问 autoload。

### 部分结构中缺失的路径（游戏）

| 位置 | 用法 | 找到／缺失的上下文 | 缺失的场景 |
|---|---|---:|---|
| `Starter-Kit-Racing/scripts/vehicle.gd:23` | `get_node_or_null("Container/TrailRight")` | 2／1 | `scenes/vehicle-motorcycle.tscn` |
| `godot-open-rts/source/match/Match.gd:56, 67` | `get_node_or_null("Map")` | 5／1 | `source/match/Match.tscn`；脚本在 `:57-59` 自己添加 Map |
| `tanks-of-freedom-ii/scenes/tiles/tile.gd:64` | `get_node_or_null("reflection")` | 10／335 | 大多数地块 |
| `tanks-of-freedom-ii/scenes/tiles/units/unit.gd:119, 416` | `get_node_or_null("mesh_anchor/mesh2")` | 10／42 | 大多数单位 |
| `tanks-of-freedom-ii/scenes/tiles/units/unit.gd:123, 420` | `get_node_or_null("mesh_anchor/mesh3")` | 5／47 | 大多数单位 |
| `tanks-of-freedom-ii/scenes/tiles/tile.gd:35, 38, 53, 63, 68` | `$"mesh"` | 344／1 | `scenes/tiles/ground/base_ground.tscn` |
| `tanks-of-freedom-ii/scenes/tiles/units/templates/heli.gd:8` | `$"mesh_anchor/mesh/rotor"` | 6／1 | `scenes/tiles/units/templates/heli.tscn` |
| `tanks-of-freedom-ii/scenes/tiles/units/templates/scout_heli.gd:6` | `$"mesh_anchor/mesh/rotor"` | 4／1 | `scenes/tiles/units/templates/scout_heli.tscn` |

- 8 处是 `get_node_or_null` 探测：代码本来就预期节点可能不存在，随后检查 null。
- 7 处是 `$`，指向基础场景或模板场景中没有、派生场景中才有的节点。`base_ground.tscn` 挂着 `tile.gd` 的子类 `ground.gd`，却没有 `mesh` 子节点；`scenes/map/templates.gd:774` 把它作为占位地块 preload。`templates/heli.tscn` 继承 `unit.tscn`，没有 `rotor`；`blue/heli.tscn` 等继承它的场景才添加 `mesh_anchor/mesh/rotor`。这些路径若在缺失的场景中执行就会出错；它们会不会在那里执行，静态分析无法判断。
- 表外还有 1 处，在只见到一种结构的脚本中：Ring 的 `breakout/scenes/paddles/paddle.gd:178` 用 `$%HealthIcon`，在 2 个上下文中找到，在 1 个中缺失。`paddle.tscn` 里没有 `HealthIcon`，它是 `breakout.tscn` 和 `goal_test_level.tscn` 中的唯一名称节点；paddle 的根节点在这两个场景中归外层根节点所有，引擎的 owner 回退因而找到它。`paddle.tscn` 单独作为场景时则找不到。插件与工具中另有 2 处同类情况（phantom-camera 示例的 `player_character_body_2d.gd:87-88`）。

### 所有结构中都找不到的路径（游戏）

| 位置 | 路径 | 场景中的情况 |
|---|---|---|
| `tps-demo/door/door.gd:6` | `$DoorModel/AnimationPlayer` | `door.tscn` 中这个子节点叫 `DoorModel2`；检出的场景、资源和脚本中没有别的文件引用 `door.tscn` |
| `tanks-of-freedom-ii/scenes/map_editor/story/stories/types/move.gd:53` | `$"player_side/side"` | `move.tscn` 中没有 `player_side`；这一行在 `context["type"] == "side"` 分支中 |
| `tanks-of-freedom-ii/scenes/map_editor/story/stories/types/objective.gd:57, 59` | `$"ban/ban_button/label"` | `objective.tscn` 中有 `clear/clear_button/label`，没有 `ban`；这两行在 `_on_clear_button_pressed` 中 |
| `godot-tiny-mmo/source/client/ui/hud/hud.gd:418` | `get_node_or_null(^"SparringCountdown")` | 找不到时由 `:419-421` 创建 |
| `Ring/scenes/main.gd:25, 41, 42, 47, 48` | `$Runtime` | 由 `:17` 或 `:32` 的 `add_child` 添加 |
| `Ring/scenes/main.gd:46` | `$Tetromino` | 由 `:43` 的 `add_child` 添加 |

后三行访问的是运行时添加的节点。前三行看来是改名或复制后没有更新的路径（推断）。示例组的 10 处中，有 5 处（`3d/platformer/player/player.gd:47-51` 的 `%CoinCount`）是分析造成的（见“局限”），其余 5 处没有逐一查看。插件与工具的 4 处都是分词器误读（同见“局限”）。

### 多种结构从何而来（游戏）

| 机制（归入第一个符合的） | 脚本 | 其中使用字面路径 |
|---|---:|---:|
| 继承 | **48** | **26** |
| 　只有场景继承（M2） | 9 | 7 |
| 　只有子类挂在别处（M5） | 36 | 16 |
| 　两者都有 | 3 | 3 |
| 独立挂到多个场景或多个节点（M1、M1b） | 15 | 2 |
| 实例被外层场景改动（M3） | 12 | 6 |
| **合计** | **75** | **34** |

- 继承是主要来源：75 个脚本中 48 个，使用字面路径的 34 个中 26 个。39 个 M5 脚本中，有 18 个的子类上下文至少有一部分位于继承场景中，常见的是继承场景把继承来的根节点的脚本换成子类。
- 独立挂到多个场景的 15 个中，1 个是手动测试场景的脚本（godot-open-rts 的 `tests/manual/Match.gd`）。使用字面路径的只有 2 个：tps-demo 的 `enemies/red_robot/parts/part.gd`（同一场景中的三个节点）和 Ring 的 `breakout/scenes/levels/goal.gd`。
- 各机制不互斥时的计数（M1、M2 只计结构不同的）：M1 15，M1b 4，M2 12，M3 15，M4 1，M5 39。

### 这些脚本做什么（游戏）

| 用途 | 脚本 | 其中使用字面路径 | 例子 |
|---|---:|---:|---|
| 一类实体的基类 | 27 | 13 | tanks-of-freedom-ii 的 `unit.gd`、`tile.gd`；godot-tiny-mmo 的 `character.gd`；Ring 的 `brick.gd` |
| 界面 | 19 | 12 | 地图编辑器的面板、菜单、设置页 |
| 管理子节点的容器 | 8 | 0 | godot-open-rpg 的 `battler_roster.gd`；deck_builder_tutorial 的 `enemy_action_picker.gd` |
| 关卡或对局的基类 | 7 | 5 | godot-open-rts 的 `Match.gd`；tanks-of-freedom-ii 的 `board.gd` |
| 同一脚本用于多个内容场景 | 6 | 1 | godot-tiny-mmo 的 `map.gd`（挂在 21 个地图场景上）；tps-demo 的 `part.gd` |
| 实例被改动的预制场景 | 5 | 3 | godot-open-rpg 的 `door.gd`、`gamepiece.gd` |
| 测试、工具、其他 | 3 | 0 | |

管理子节点的 8 个脚本都不用字面路径，其中 5 个调用 `get_children()`、`get_child()` 或 `get_child_count()`。

### 挂载场景子树之外的访问

每格为“处（脚本数）”：

| 访问 | 游戏 | 示例 | 插件与工具 |
|---|---:|---:|---:|
| `/root/…` 路径 | 130（62） | 3（3） | 8（1） |
| `get_parent()` | 166（95） | 53（35） | 179（75） |
| `owner.` | 29（10） | 18（9） | 7（3） |
| 含 `..` 的路径 | 10（6） | 38（27） | 5（5） |
| 对其他对象的字面路径（`x.get_node("…")`） | 133（27） | 53（25） | 129（49） |
| 非字面量参数的 `get_node` | 30（25） | 47（17） | 64（29） |
| `find_child`、`find_children` | 162（68） | 14（7） | 87（35） |
| Node 类型的导出成员 | 100（59） | 42（25） | 36（21） |
| NodePath 类型的导出成员 | 9（9） | 11（9） | 17（16） |
| 调用 `add_child` 的脚本 | 222 | 84 | 162 |

这些访问的目标取决于节点运行时放在哪里，或者取决于运行时的值，不在本文定义的结构之内。`add_child` 之后有多少字面路径访问被添加的节点，本次只在“所有结构中都找不到”的路径中看到，没有专门统计。

## 对两种做法构成压力的情形

以下列出测到的情形；每种情形对哪种做法意味着什么，是推断。

对 A（场景为准）：

- **可选节点。** 游戏中 8 处 `get_node_or_null` 在部分结构中缺失；其中 `Match.gd:56` 和表外的 `hud.gd:418` 先探测，再由脚本自己添加节点。A 要给这类路径定型，需要能表达“可能不存在”。
- **模板场景缺节点。** 游戏中 7 处 `$` 只在派生场景中成立。如果模板场景也算作 A 的上下文，这些路径无法在所有结构中定型；如果不算，就需要一种方式标出哪些场景不算。
- **取决于外层场景。** `paddle.gd:178` 和 phantom-camera 示例的 2 处，只能在实例化该场景的外层场景中解析，挂载场景本身给不出答案。
- **运行时添加的节点。** Ring `main.gd` 的 6 处和 `hud.gd:418` 访问的节点不在任何场景文件中。
- **导入模型内部。** 游戏 14 处、示例 16 处路径进入导入的模型。文本场景中没有这些节点，A 需要导入后的场景结构。
- **没有场景的脚本。** 游戏中 13 处字面路径出自没有任何上下文的脚本，其中 10 处是 autoload 之间的 `/root/…` 访问。

对 B（脚本声明）：

- **声明量。** 游戏中 316 个脚本共用了 1,471 个不同的字面路径（2,023 处用法）。若每个路径各需一条声明，就是 1,471 条；子类能否沿用基类的声明、其中多少已经写成带类型的 `@onready var`，本次没有统计。
- **声明与场景不符。** 游戏中 11 处路径在任何场景中都找不到，其中 4 处（`door.gd:6`、`move.gd:53`、`objective.gd:57, 59`）看来是改名或复制后没有更新的路径。只有声明与场景对照检查时，B 才能报出这类错误。

两种做法都不直接涉及上一节表中的向上访问、绝对路径和运行时值。

## 典型例子

**1. 占位场景缺少基类用到的节点。**
tanks-of-freedom-ii：`scenes/tiles/tile.gd:34-38, 52-53`；`scenes/tiles/ground/base_ground.tscn:6-9`

```
func reset_position_for_tile_view():
	var mesh_position = $"mesh".get_position()
	mesh_position.y = 0

	$"mesh".set_position(mesh_position)
...
func hide_mesh():
	$"mesh".hide()
```

```
[node name="base_ground" type="Node3D"]
script = ExtResource("3")

[node name="mouse_collision" parent="." instance=ExtResource("2")]
```

`tile.gd` 在 345 个上下文中出现，有 79 种结构，`$"mesh"` 在其中 344 个上下文中是 MeshInstance3D。缺失的那个是 `base_ground.tscn`：它挂着子类 `ground.gd`，只有 `mouse_collision`，在 `scenes/map/templates.gd:774` 中作为占位地块 preload。

**2. 用 `get_node_or_null` 取可选节点。**
Starter-Kit-Racing：`scripts/vehicle.gd:13-23`

```
# (Optional) wheels

@onready var wheel_fl = get_node_or_null("Container/Model/wheel-front-left")
@onready var wheel_fr = get_node_or_null("Container/Model/wheel-front-right")
@onready var wheel_bl = get_node_or_null("Container/Model/wheel-back-left")
@onready var wheel_br = get_node_or_null("Container/Model/wheel-back-right")

# Effects

@onready var trail_left = get_node_or_null("Container/TrailLeft")
@onready var trail_right = get_node_or_null("Container/TrailRight")
```

`vehicle.tscn` 有两条尾迹；子类 `vehicle-motorcycle.gd` 所在的 `vehicle-motorcycle.tscn` 只有 `TrailLeft`。车轮路径进入导入的模型，分析无法判断。

**3. 唯一名称来自外层场景。**
Ring：`breakout/scenes/paddles/paddle.gd:176-178`；`breakout/scenes/breakout/breakout.tscn:95, 172-173`

```
func _on_health_changed(value: int):
	if value < 0:
		Utility.flicker_red($%HealthIcon)
```

```
[node name="Paddle" parent="." groups=["paddles"] instance=ExtResource("1_n40qs")]
...
[node name="HealthIcon" type="TextureRect" parent="UI/Status/Health/Container"]
unique_name_in_owner = true
```

`paddle.tscn` 中没有 `HealthIcon`。paddle 被实例化到 `breakout.tscn` 后，它的根节点归 breakout 的根节点所有，`%HealthIcon` 先查 paddle 自己的唯一名称，找不到，再查 owner 的，于是找到界面上的图标。这个路径的答案取决于谁实例化了 paddle。

**4. 运行时添加的节点。**
Ring：`scenes/main.gd:14-17, 24-25, 37-46`

```
	tween.tween_callback(func():
		var runtime: Node = load("res://Runtime/Runtime.cs").new()
		runtime.name = "Runtime"
		add_child(runtime)
...
	tween.tween_callback(func():
		$Runtime.process_mode = Node.PROCESS_MODE_INHERIT
...
func load_tetromino():
	var tetromino = minigame_scene.instantiate()
	tetromino.name = "Tetromino"
	...
	add_child(tetromino)

func end_tetromino():
	$Tetromino.queue_free()
```

`main.tscn` 的根节点下只有 `Title` 和 `Black`。`Runtime` 是一个 C# 脚本创建的节点，`Tetromino` 是运行时实例化的小游戏场景。

**5. 改名后没有更新的路径。**
tps-demo：`door/door.gd:6`；`door/door.tscn:13, 25`

```
@onready var animation_player: AnimationPlayer = $DoorModel/AnimationPlayer
```

```
[node name="DoorModel2" parent="." unique_id=1109006140 instance=ExtResource("2")]
...
[node name="AnimationPlayer" parent="DoorModel2" index="1" unique_id=1279510582]
```

场景中的模型节点叫 `DoorModel2`，脚本写的是 `DoorModel`。检出的文件中没有引用这个场景的，所以这个错误看来不会在运行时出现（推断）。

## 结论

- **见到多种结构又使用字面路径的脚本是少数。** 游戏中 1,223 个脚本里有 34 个，占使用字面路径的 316 个脚本的 10.8%；其中 24 个来自 tanks-of-freedom-ii 和 godot-open-rpg。
- **字面路径在不同结构中从未指向不同类型。** 三组共 5,231 处用法，0 处。结构之间的差别在字面路径上只表现为节点在不在。
- **缺失集中在两种写法上。** 一是用 `get_node_or_null` 探测可选节点（游戏 8 处），二是模板或占位场景缺少派生场景才有的节点（游戏 7 处）。
- **多种结构主要来自继承。** 游戏中 75 个脚本里有 48 个，使用字面路径的 34 个里有 26 个。把同一脚本独立挂到多个场景的有 15 个，其中使用字面路径的 2 个。
- **有些路径的答案不在挂载场景里。** 经外层场景解析的 `%名称`（游戏 1 处，插件与工具 2 处）、运行时添加的节点、导入模型的内部（游戏 14 处）。
- **挂载场景子树之外的访问很常见。** 游戏中 `/root/…` 130 处、`get_parent()` 166 处、`owner.` 29 处、`..` 10 处，222 个脚本调用 `add_child`。

依据这些数据所作的决定，记录在[设计决定](../decisions.zh.md)的「场景与节点」一节。

## 局限

- **只看文本场景。** 2 个二进制 `.scn` 没有解析。导入的模型没有检出，模型内部是未知的：路径进入模型时记为“无法判断”（游戏 14 处，示例 16 处）；外层场景在模型实例下添加的节点被分析丢弃。后者使 godot-demo-projects 的 `3d/platformer/player/player.gd:47-51` 中 5 处 `%CoinCount` 被误记为“所有结构中都找不到”：`CoinCount` 实际添加在 `player.tscn` 中导入模型实例的 `Player/Skeleton` 下。游戏组中缺失类的结果没有一处是这样造成的。
- **只按名称匹配节点。** 引擎在按名称找不到时还会按 `unique_id` 找；本次没有找不到父节点的条目，所以这一回退没有影响。
- **只做静态分析。** 运行时的 `add_child`、`remove_child`、`queue_free`、改父节点、`set_script` 和运行时实例化都没有建模。
- **所有场景文件都算作上下文。** 没有使用的场景（如 tps-demo 的 `door.tscn`）、占位场景、测试场景和示例场景都会贡献结构。
- **分词器会把续行开头的 `%` 误读为 `%名称`。** 括号内另起一行写的格式化运算符 `% x` 被当成唯一名称。插件与工具中“所有结构中都找不到”的 4 处（Pixelorama 的 `HandleExtensions.gd:173`、`Main.gd:667, 680`、`DynamicsPanel.gd:58`）都是这样；游戏组中缺失类的结果没有一处是这样造成的。
- **只解析对 self 的字面路径。** 非字面量的 `get_node`、对其他对象的路径、`find_child`、`get_child(i)` 和导出的 Node 成员只计数，没有解析。
- **结构不含祖先，也不比较节点的属性、分组或信号。** 类型比较的是（原生类型，脚本）。
- **游戏代码集中在少数仓库。** 34 个关键脚本中 24 个来自两个仓库。新增的 5 个游戏不是随机抽样。
- **用途分类是人工判断。** 由 AI 代理阅读后归类；引用的代码和场景内容都对照源文件核对过。

## 复现

脚本在 [`scene-structures/`](scene-structures/) 目录，只用 Python 3 标准库（在 Python 3.11 上运行过），克隆需要 git。`fetch_corpus.py` 列出全部仓库和提交，并以稀疏、浅克隆取回；`analyze.py` 合成场景树、解析路径，写出 `repos.json`、`scripts.csv`、`uses.csv`、`contexts.csv`；`report.py` 输出各仓库表和各组统计（`report.txt`）以及游戏组中见到 ≥2 种结构的脚本清单（`multi_games.txt`）；`summarize.py` 输出机制、用途等汇总（`summary.txt`），用途标签取自 `purpose_labels.csv`；`classes.json` 是原生类的继承关系，可用 `build_classes.py` 从 Godot 源码重新生成（只读类参考 XML，不构建引擎）；`test_fixture.py` 用 `fixture_corpus/` 中的合成项目自检，加 `--write` 时先重新生成这个项目；`check_tokens.py`、`check_tokens2.py` 是分词器的抽查工具。[`results/`](scene-structures/results/) 中是本文所用的 `report.txt`、`summary.txt`、`multi_games.txt` 和 `repos.json`。

```
python docs/surveys/scene-structures/fetch_corpus.py corpus        # 20 个仓库，固定提交
python docs/surveys/scene-structures/analyze.py corpus out         # out/repos.json、scripts.csv、uses.csv、contexts.csv
python docs/surveys/scene-structures/report.py corpus out          # out/report.txt、out/multi_games.txt
python docs/surveys/scene-structures/summarize.py out              # out/summary.txt
python docs/surveys/scene-structures/test_fixture.py               # 输出 fixture OK
# 可选
python docs/surveys/scene-structures/build_classes.py <Godot 源码目录>   # 在提交 469659e 上重新生成 classes.json
python docs/surveys/scene-structures/check_tokens.py corpus out          # 分词器与正则的计数对照
python docs/surveys/scene-structures/check_tokens2.py corpus Pixelorama  # 列出两者不一致的行
```

在 Windows 上用 Python 3.11 重跑，`out/` 中的四个对应文件与 `results/` 逐字节相同；换用不同的 `PYTHONHASHSEED`，全部输出也不变。
