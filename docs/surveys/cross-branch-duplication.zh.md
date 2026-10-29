# 调查：原生基类不同的脚本之间的重复代码

**2026-10-23** · 关联：[语言规范](../language-spec.zh.md) §3.9、§4.1；[场景结构调查](scene-structures.zh.md)（语料相同） · English: [cross-branch-duplication.en.md](cross-branch-duplication.en.md)

Godot 的脚本只能继承一个原生类。两个原生类不同的对象需要同样的行为时，例如一个 StaticBody2D 和一个 CharacterBody2D，它们无法共用一个脚本基类，代码只能复制、放进子节点或改写成别的形式。本文在 20 个仓库上测量这种限制造成的重复有多常见、重复的是什么，并逐组判断哪种机制能消除它。数字都由脚本算出；机制归类是阅读代码后的判断，文中单独标出。

## 问题

以下引擎事实在 Godot 提交 `469659e` 上核对，路径相对于 Godot 仓库根目录：

- 一个对象只有一个脚本实例（`core/object/object.h:451`）；`Object::set_script` 先删除已有的实例，再创建新的（`core/object/object.cpp:1060-1068`）。
- 一个 GDScript 只有一个基类脚本和一个原生类（`modules/gdscript/gdscript.h:82-83`）；`extends` 后只能写一个路径或一个带点的类名（`modules/gdscript/gdscript_parser.cpp:1014-1055`）。
- 把脚本挂到对象上时，`GDScript::instance_create` 沿基类找到最顶层的原生类，对象不是该类的实例就失败（`modules/gdscript/gdscript.cpp:419-430`）；`Object::is_class` 检查对象的整条类层级（`core/object/object.cpp:2187-2189`）。

因此，继承 Control 的脚本可以挂在 VBoxContainer 上；而 StaticBody2D 和 CharacterBody2D 的公共祖先是 PhysicsBody2D（`doc/classes/StaticBody2D.xml:2`、`doc/classes/CharacterBody2D.xml:2`），两者能共用的脚本基类最多继承到 PhysicsBody2D，CharacterBody2D 自己的成员（例如 `velocity`，`doc/classes/CharacterBody2D.xml:198`）不在其中。

下文把脚本继承链顶端的引擎类称为原生基类。本文回答以下问题：

- 原生基类不同、又没有共同脚本祖先的脚本之间（下称跨分支），有多少重复代码？与继承本来就能消除、却没有消除的重复相比如何？
- 重复的是哪类代码：字段、生命周期回调，还是普通方法？
- 哪种机制能消除它：接口默认方法、带状态的编译期 mixin、组合，还是静态辅助函数？
- 项目在不同类型的对象上调用同名成员时，用的是什么写法？

## 语料

与[场景结构调查](scene-structures.zh.md)相同的 20 个仓库，提交见“各仓库”一节的表格：其中 14 个与[属性访问器调查](property-accessors.zh.md)的提交相同，另有 5 个游戏仓库取 2026-10-23 的最新提交，Ring 与[纸面迁移调查](paper-migration.zh.md)的提交相同。按用途分为三组：

- **游戏**（12 个）：godot-open-rpg、四个 Kenney 入门模板、tps-demo、godot-open-rts、tanks-of-freedom-ii、deck_builder_tutorial、godot-tiny-mmo、godot_open_star_fighter、Ring。
- **示例**：godot-demo-projects，其中 132 个 Godot 项目有第一方代码。
- **插件与工具**（7 个）：beehave、gdUnit4、godot_dialogue_manager、Godot-Game-Template、godot-statecharts、phantom-camera、Pixelorama。

只计第一方代码，范围与场景结构调查相同：`addons/` 以外的全部代码，加上仓库本身发布的插件（包括 godot-tiny-mmo 的 `addons/tinymmo`），以及 godot-demo-projects 和 Godot-Game-Template 的全部 `addons/`（`gdcorpus.py` 中的 `FIRST_PARTY_ADDONS`）。单元测试计入代码量，但涉及测试的单元对单独归类，不计入下文的跨分支等数字。只分析 `.gd` 和 `.tscn`，Ring 的 C# 代码不在内。

**许可。** 除 Ring 外都是 MIT 许可。[YYF233333/Ring](https://github.com/YYF233333/Ring)（提交 `0ec4161`）没有许可文件；经仓库所有者同意，本文收录其代码片段。

## 方法

1. **单元与逻辑行。** 单元是一个脚本类或其中的内部类，第一方共 2,989 个。`gdcorpus.py` 中的词法分析去掉注释，按括号和行尾反斜杠合并续行。以下行视为平凡，不参与检测，也不计入代码量：`pass`、`return`、`else:`、`break`、`continue`、单独的字符串、只有注解的行、`extends`、`class_name`、`class X:` 头，以及引擎虚函数的 `func _x(...):` 头。其余的行下文称为逻辑行，第一方共 163,202 行，包括测试和编辑器脚本。
2. **克隆检测**（`dup.py`）。只在同一个 Godot 项目内比较，godot-demo-projects 的 132 个项目互不比较。窗口是同一单元中连续的 K 个逻辑行（默认 K = 5），连同每行相对于窗口首行的缩进层级；两个窗口的内容相同即为一处匹配。同一对单元中相邻的匹配合并为克隆区域；同一单元内的两个窗口不能重叠。有三种比较方式：
   - **精确**：词法单元原样比较。
   - **近似**：字面量，以及本单元的局部变量名和参数名，换成占位符。
   - **宽松**：关键字和引擎 API 名字以外的标识符全部换成占位符。这是上限，误报很多。

   重复行数计入每一份副本，每行在每个类别中只计一次。互相重叠的副本连成一个克隆组。
3. **单元对的类别**，按以下顺序取第一个成立的：同一单元内部；测试或编辑器（任一方是测试，或继承 `Editor*` 类）；**同继承链**（两者有共同的脚本祖先，父子或兄弟）；原生基类无法确定；非节点（任一方不是 Node）；**同原生类**（原生基类相同，没有共同的脚本祖先）；**跨分支**（原生基类不同，没有共同的脚本祖先）。按上面的引擎事实，同继承链的重复可以提到共同祖先中，同原生类的重复可以提取一个共同基类，所以这两类重复用现有的继承就能消除。
4. **名字证据。** 对一对跨分支单元，取两者原生基类的最近公共祖先，例如 StaticBody2D 与 CharacterBody2D 的 PhysicsBody2D。若任一单元在 self 上（不带接收者，或经 `self.`、`super.`）使用了公共祖先没有、只有自己原生类那一支才有的引擎成员，就说这对单元**有名字证据**：共同基类只能继承公共祖先，用不到这些成员。没有名字证据的单元对，按引擎规则可以共用一个以公共祖先为原生基类的基类脚本。引擎成员的名单来自 Godot 类参考 XML（`build_api.py`，1,112 个类）。
5. **重复内容的种类**按每行所在的位置划分：导出成员、`@onready` 成员、其他成员声明和属性访问器体、信号声明、生命周期回调（`_ready`、`_enter_tree`、`_init`、`_exit_tree`、`_notification`、`_static_init`）、每帧回调（`_process`、`_physics_process`、`_integrate_forces`）、输入回调（`_input`、`_unhandled_input`、`_gui_input` 等）、其他引擎虚函数、信号处理函数（`_on_*` 或 `on_*`）、普通方法。
6. **机制标签（阅读判断）。** 对 K = 5 时全部 88 个精确跨分支克隆组，阅读代表副本，按能消除它的机制归为五类：
   - (a) **接口默认方法**：方法只读取两个类各自声明的成员（或 self 上的引擎 API），既不声明也不写入共有的状态；
   - (b) **带状态的编译期 mixin**：副本本身声明了字段、`@export` 或信号，或者写入了声明也重复的字段；
   - (c) **组合**：自成一体的子功能，有自己的状态和节点，可以做成子节点或子场景；
   - (d) **静态辅助函数**：只对参数、全局量或 autoload 计算，不需要 self 的状态；
   - (e) **无法消除**：琐碎的、或与场景结构绑定的样板。

   其中 55 个组逐个标注。phantom-camera 其余 33 个组按一条规则统一标为 (b)：它们都属于 PhantomCamera2D/3D 或 NoiseEmitter2D/3D 这两对孪生脚本，引用的成员在另一份中也重复声明。标签由 AI 代理判断，记录在 `labels.csv` 中。`dup.py` 另有一个自动的预标注，与阅读所得的标签出入很大，下文只在对照时提到。(c) 和 (d) 两种机制在 GDScript 中已经有（子节点和 `static func`）。
7. **共同能力。** 在每个项目中，对每对跨分支的 Node 单元（不含测试和编辑器脚本），找出两者都声明的公开名字：不以下划线开头、不是引擎成员的 `func`、非 `@onready` 的 `var` 和 `signal`。这个名字在项目中每一次经非 self 接收者的访问（`x.name`，或作为字面量传给 `call`、`has_method`、`get` 等）记为一个消费点，并按接收者的写法分类：标注为脚本类或引擎类的变量、推断类型的变量、类名或 autoload 名、无类型的接收者，以及几种显式的跨类型写法：`is_in_group` 之后调用、经 `owner` 或 `get_parent()` 访问、同一函数中对至少两个脚本类做 `is` 判断并含有 `or`（典型写法是 `is A or is B`）、`has_method` 守卫。名字相同不等于能力相同，所以这些数字是上限。
8. **变通写法的指标**：在第一方非测试代码中数 `has_method`、`is_in_group`、字符串调用（`call`、`call_deferred`、`callv` 的第一个参数是字面量）、同一行用 `or` 连接两个以上脚本类的 `is` 判断、经 `owner` 或 `get_parent()` 访问非引擎成员，以及 `static func`。
9. **组合**（`components.py`）。组合全部 `.tscn`，包括继承场景、实例和实例上的修改，找出每个第一方 Node 脚本挂载的节点，记录父节点的原生类型；从基场景继承来的挂载不计。如果一个脚本的父节点类型中至少有两种互不为祖先，它就是已经在不同分支之间共用的组件。
10. **核对与敏感性。** 用 Ring 的 Brick 与 Enemy 核对检测能找到多少重复（`ring_check.py`）；用 K = 3 和 K = 8 重跑全部检测。

## 结果

### 总体

精确匹配，K = 5。括号内是每千行逻辑行中的重复行数；单元对一行是“有克隆的单元对 / 全部单元对”。

| | 逻辑行 | 跨分支 | 同原生类 | 同继承链 |
|---|---:|---:|---:|---:|
| 游戏：重复行 | 57,621 | 353 (6.1) | 1,725 (29.9) | 2,884 (50.1) |
| 游戏：单元对 | | 21 / 36,232 | 115 / 8,672 | 403 / 7,993 |
| 示例：重复行 | 16,776 | 106 (6.3) | 199 (11.9) | 0 |
| 示例：单元对 | | 3 / 1,063 | 7 / 190 | 0 / 57 |
| 插件与工具：重复行 | 88,805 | 1,291 (14.5) | 1,575 (17.7) | 2,449 (27.6) |
| 插件与工具：单元对 | | 31 / 20,290 | 34 / 2,356 | 244 / 1,973 |

- **游戏中，继承本可消除却没有消除的重复比跨分支重复多得多**：同原生类是跨分支的 4.9 倍，同继承链是 8.2 倍。按单元对算差距更大：跨分支的单元对最多，有克隆的比例最低（每千对 0.58 对，另两类为 13.3 对和 50.4 对）。
- **游戏组的后两类主要来自 tanks-of-freedom-ii**（1,280 行和 1,697 行）。去掉它，三类分别为 289、445、1,187 行。
- **按引擎规则，跨分支重复中有一部分本来就可以用继承共享。** 有名字证据的单元对中的重复，游戏 150 行，示例 94 行，插件与工具 891 行；没有名字证据的，分别为 219、12、400 行。游戏的两数之和大于 353，是因为有 16 行同时属于两种单元对。
- **近似匹配下比例相近**：游戏三类为 694、2,315、4,417 行。宽松匹配给出上限：1,677、4,447、5,377 行。

### 各仓库

精确匹配，K = 5。“近似匹配”一列是近似匹配下的跨分支重复行。

| 仓库 | 提交 | 逻辑行 | 跨分支 | 每千行 | 其中有名字证据 | 近似匹配 | 同原生类 | 同继承链 |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| [gdquest-demos/godot-open-rpg](https://github.com/gdquest-demos/godot-open-rpg) | `d59f2f7` | 2,190 | 0 | 0.0 | 0 | 0 | 12 | 68 |
| [KenneyNL/Starter-Kit-3D-Platformer](https://github.com/KenneyNL/Starter-Kit-3D-Platformer) | `3fa8a04` | 178 | 0 | 0.0 | 0 | 0 | 0 | 0 |
| [KenneyNL/Starter-Kit-City-Builder](https://github.com/KenneyNL/Starter-Kit-City-Builder) | `4535092` | 159 | 0 | 0.0 | 0 | 0 | 0 | 0 |
| [KenneyNL/Starter-Kit-FPS](https://github.com/KenneyNL/Starter-Kit-FPS) | `185fd23` | 234 | 0 | 0.0 | 0 | 0 | 0 | 0 |
| [KenneyNL/Starter-Kit-Racing](https://github.com/KenneyNL/Starter-Kit-Racing) | `2f2e5f2` | 123 | 0 | 0.0 | 0 | 0 | 0 | 0 |
| [godotengine/tps-demo](https://github.com/godotengine/tps-demo) | `a82f154` | 1,046 | 0 | 0.0 | 0 | 0 | 18 | 0 |
| [lampe-games/godot-open-rts](https://github.com/lampe-games/godot-open-rts) | `a628ad3` | 3,716 | 14 | 3.8 | 14 | 14 | 118 | 24 |
| [P1X-in/tanks-of-freedom-ii](https://github.com/P1X-in/tanks-of-freedom-ii) | `dbe629e` | 15,359 | 64 | 4.2 | 24 | 84 | 1,280 | 1,697 |
| [guladam/deck_builder_tutorial](https://github.com/guladam/deck_builder_tutorial) | `91b408b` | 2,679 | 26 | 9.7 | 0 | 26 | 12 | 160 |
| [SlayHorizon/godot-tiny-mmo](https://github.com/SlayHorizon/godot-tiny-mmo) | `f4ed04f` | 28,891 | 177 | 6.1 | 40 | 492 | 261 | 911 |
| [teeeece/godot_open_star_fighter](https://github.com/teeeece/godot_open_star_fighter) | `6e7be5c` | 1,242 | 0 | 0.0 | 0 | 0 | 14 | 12 |
| [YYF233333/Ring](https://github.com/YYF233333/Ring) | `0ec4161` | 1,804 | 72 | 39.9 | 72 | 78 | 10 | 12 |
| **游戏合计** | | **57,621** | **353** | **6.1** | **150** | **694** | **1,725** | **2,884** |
| [godotengine/godot-demo-projects](https://github.com/godotengine/godot-demo-projects) | `b761b4c` | 16,776 | 106 | 6.3 | 94 | 106 | 199 | 0 |
| [bitbrain/beehave](https://github.com/bitbrain/beehave) | `fe58915` | 5,169 | 24 | 4.6 | 24 | 24 | 348 | 419 |
| [MikeSchulze/gdUnit4](https://github.com/MikeSchulze/gdUnit4) | `9bc1a45` | 26,384 | 0 | 0.0 | 0 | 0 | 0 | 143 |
| [nathanhoad/godot_dialogue_manager](https://github.com/nathanhoad/godot_dialogue_manager) | `7a72dcf` | 7,377 | 58 | 7.9 | 0 | 68 | 10 | 0 |
| [Maaack/Godot-Game-Template](https://github.com/Maaack/Godot-Game-Template) | `6849d6c` | 5,878 | 156 | 26.5 | 156 | 156 | 836 | 634 |
| [derkork/godot-statecharts](https://github.com/derkork/godot-statecharts) | `76d226a` | 3,377 | 0 | 0.0 | 0 | 0 | 0 | 36 |
| [ramokz/phantom-camera](https://github.com/ramokz/phantom-camera) | `21d3762` | 5,394 | 792 | 146.8 | 548 | 808 | 0 | 10 |
| [Orama-Interactive/Pixelorama](https://github.com/Orama-Interactive/Pixelorama) | `38a8b03` | 35,226 | 261 | 7.4 | 163 | 259 | 381 | 1,207 |
| **插件与工具合计** | | **88,805** | **1,291** | **14.5** | **891** | **1,315** | **1,575** | **2,449** |

- **游戏中，12 个仓库有 7 个没有跨分支克隆**，它们都较小，合计 5,172 行。有跨分支克隆的 5 个仓库中，4 个在每千行 3.8 到 9.7 行之间，Ring 为 39.9 行。
- **游戏中只有 Ring 的跨分支重复多于另两类**（72 行，另两类为 10 行和 12 行），全部来自 Brick（StaticBody2D）与 Enemy（CharacterBody2D）。这对单元的名字证据只有一条：Enemy 使用 CharacterBody2D 的 `velocity`（`breakout/scenes/enemies/base/enemy.gd:98, 186, 196`）。插件中 phantom-camera 和 godot_dialogue_manager 也是跨分支重复多于另两类。
- **phantom-camera 每千行 146.8 行。** PhantomCamera2D 与 PhantomCamera3D、NoiseEmitter2D 与 NoiseEmitter3D 是两对 2D/3D 孪生脚本，35 个克隆组都在 Node2D 与 Node3D 之间，重复行占该仓库逻辑行的 14.7%。
- **godot-tiny-mmo 的近似匹配（492 行）远多于精确匹配（177 行）。** 多出的部分主要是一个 303 行的组：26 个客户端脚本中的 38 处用代码创建容器和 TextureRect、设置主题常量的片段（`groups_near.csv`）。
- 游戏的 23 个精确跨分支克隆组中，原生类型对最多的是 Control 与 VBoxContainer（7 组，全在 godot-tiny-mmo）和 CharacterBody2D 与 StaticBody2D（4 组，全在 Ring）。353 行中有 211 行所在的组，两个原生类一个是另一个的祖先（例如 Control 与 VBoxContainer），142 行在兄弟类之间。插件与工具的 1,291 行中，兄弟类之间有 1,183 行，主要是 Node2D 与 Node3D。
- godot-demo-projects 的 132 个项目中，13 个有精确克隆，3 个有跨分支克隆：`2d/physics_tests`（CharacterBody2D 与 RigidBody2D 的两个控制器）、`misc/hdr_output` 和 `xr/openxr_hand_tracking_demo`。

### 重复的是什么

精确匹配、K = 5 的跨分支重复行，按所在位置：

| 位置 | 游戏 | 示例 | 插件与工具 |
|---|---:|---:|---:|
| 普通方法 | 277 | 0 | 817 |
| 信号处理函数 | 20 | 26 | 31 |
| 生命周期回调 | 14 | 8 | 36 |
| 每帧回调 | 12 | 56 | 56 |
| 输入回调 | 8 | 0 | 0 |
| 其他引擎虚函数 | 2 | 0 | 51 |
| 导出成员 | 12 | 4 | 42 |
| 其他成员声明和属性访问器体 | 8 | 12 | 252 |
| 信号声明 | 0 | 0 | 4 |
| 其他类层级语句 | 0 | 0 | 2 |
| **合计** | **353** | **106** | **1,291** |
| 其中含 `.connect` 的行 | 10 | 2 | 4 |

- **游戏中 78% 的跨分支重复在普通方法中**，成员声明（导出成员和其他成员）只有 20 行，其中 10 行是 Ring 的导出成员。生命周期、每帧和输入回调合计 34 行。
- **插件与工具中，成员声明和属性访问器体有 252 行**，主要是 phantom-camera 孪生脚本中带 setter 和 getter 的导出属性。
- 示例的 56 行每帧回调来自 physics_tests 的两个控制器。

### 哪种机制能消除它（阅读判断）

精确匹配、K = 5 的全部跨分支克隆组，“组 / 行”：

| 标签 | 游戏 | 示例 | 插件与工具 |
|---|---:|---:|---:|
| (a) 接口默认方法 | 4 / 54 | 0 | 2 / 30 |
| (b) 带状态的编译期 mixin | 7 / 102 | 3 / 70 | 41 / 948 |
| (c) 组合 | 5 / 81 | 0 | 0 |
| (d) 静态辅助函数 | 6 / 104 | 2 / 36 | 17 / 313 |
| (e) 无法消除 | 1 / 12 | 0 | 0 |
| **合计** | **23 / 353** | **5 / 106** | **60 / 1,291** |

- **游戏的 353 行中，(c) 和 (d) 共 185 行**，即一半多可以用 GDScript 已有的机制消除。(c) 的 81 行全在 godot-tiny-mmo；(d) 的 104 行来自 godot-tiny-mmo（64 行）和 tanks-of-freedom-ii（40 行）。
- **(a) 54 行**，分布在 deck_builder_tutorial（16）、godot-open-rts（14）、godot-tiny-mmo（12）和 tanks-of-freedom-ii（12）。
- **(b) 102 行，其中 72 行是 Ring 的 Brick 与 Enemy**，其余来自 godot-tiny-mmo（20）和 deck_builder_tutorial（10）。
- **(b) 与名字证据高度重合。** 只要组中有一对单元有名字证据，就把整组算作有。游戏中，(b) 的 102 行有 92 行在这样的组中；(a) 为 54 行中的 14 行，(c) 为 81 行中的 20 行，(d) 为 104 行中的 40 行。
- **插件与工具的 (b) 948 行中，phantom-camera 占 734 行**；其余是 Godot-Game-Template 的 input_actions_list 与 input_actions_tree（Container 与 Tree，156 行）和 godot_dialogue_manager 的 DialogueActionable2D 与 3D（Area2D 与 Area3D，58 行）。(d) 的 313 行中有 231 行在 Pixelorama，多是撤销和重做、动画标签区间一类的片段，散在各种对话框和工具中。
- 示例的 (b) 70 行是 physics_tests 的两个控制器（56 行，6 个相同字段加每帧的输入和速度逻辑）和 hdr_output（14 行）。
- 自动预标注把游戏的 16 个组（273 行）标为带状态，阅读标注只有 7 个组（102 行）：只要片段引用了成员，自动规则就判为带状态。

### 共同能力与跨类型调用

| | 游戏 | 示例 | 插件与工具 |
|---|---:|---:|---:|
| Node 单元 | 752 | 411 | 446 |
| 声明了同名公开成员的跨分支单元对 | 681 | 21 | 312 |
| 其中至少有一个名字经非 self 接收者被访问 | 568 | 19 | 232 |
| 其中至少有两个这样的名字 | 59 | 1 | 40 |
| 共有的名字（按项目去重） | 181 | 12 | 234 |
| 其中被访问的 | 143 | 9 | 118 |
| 消费点 | 2,081 | 21 | 1,387 |

游戏的 2,081 个消费点，按接收者的写法：

| 写法 | 消费点 |
|---|---:|
| 无类型的接收者 | 1,126 |
| 标注为脚本类的变量 | 488 |
| 类名或 autoload 名 | 180 |
| 标注为引擎类等的变量 | 118 |
| 推断类型的变量 | 77 |
| 字面量名字传给 `get`、`call_deferred` | 22 |
| 同一函数中有对脚本类的其他 `is` 判断 | 14 |
| 节点路径 | 1 |
| 显式跨类型：`is_in_group` 之后调用 | 18 |
| 显式跨类型：经 `owner` 或 `get_parent()` | 13 |
| 显式跨类型：同一函数中对两个以上脚本类做 `is` 判断并含有 `or` | 11 |
| 显式跨类型：`has_method` 守卫，及字面量名字传给 `has_method` | 7 + 6 |

- **名字相同多半是巧合。** 被访问最多的共有名字是 tanks-of-freedom-ii 的 `model`（146 个消费点）、`map`（137）、`side`（101）、`clear`（90），以及 godot-tiny-mmo 的 `current`（126）。所以上表是上限，不代表这些单元真有共同能力。
- **显式的跨类型写法很少**：游戏中共 55 处，占 2.6%。`is_in_group` 之后调用的 18 处都在 godot-open-rts；经 `owner` 的 13 处都在 Ring 的组件中；同一函数中对多个脚本类做 `is` 判断的 11 处在 godot-tiny-mmo（7）和 deck_builder_tutorial（4）；`has_method` 的 13 处在 Starter-Kit-FPS、godot-tiny-mmo 和 godot_open_star_fighter。
- **一半以上的消费点（1,126 处，54%）接收者没有类型**，从语法上无法判断是不是跨类型调用。
- 插件与工具的显式写法中，对多个脚本类做 `is` 判断有 35 处，其中 33 处在 Pixelorama；`has_method` 有 24 处。

### 已有的变通写法

第一方非测试代码：

| | 游戏 | 示例 | 插件与工具 |
|---|---:|---:|---:|
| `has_method` | 18 | 18 | 45 |
| 同一行中用 `or` 连接、对两个以上脚本类的 `is` 判断 | 4 | 0 | 8 |
| `is_in_group` | 45 | 2 | 18 |
| 经 `owner` 或 `get_parent()` 访问非引擎成员（单元数） | 20 (10) | 7 (5) | 3 (3) |
| 字符串调用 | 41 | 4 | 65 |
| `static func` | 491 | 23 | 909 |

已经作为组件在不同原生类型之间共用的脚本：

| | 游戏 | 示例 | 插件与工具 |
|---|---:|---:|---:|
| 挂在子节点上的第一方 Node 脚本 | 444 | 268 | 216 |
| 其父节点有两种以上原生类型 | 17 | 6 | 26 |
| 其父节点类型分属不同分支 | 11 | 4 | 18 |
| 其中脚本本身继承 Node | 4 | 1 | 6 |

- 游戏的 `static func` 集中在两个仓库：godot-tiny-mmo 446 个，godot-open-rts 35 个；其余游戏仓库合计 10 个。`is_in_group` 的 45 处中，godot-open-rts 占 35 处。
- 游戏中继承 Node、挂在不同分支父节点下的 4 个组件是：Ring 的 HealthComponent 和 IntervalHurtComponent（父节点为 StaticBody2D 296 次、CharacterBody2D 5 次），deck_builder_tutorial 的 ModifierHandler（Area2D、Node2D、Control），godot-tiny-mmo 的 StateSynchronizer（CharacterBody2D 131 次、Area2D 1 次）。
- Ring 同时用了组合和复制：生命值逻辑放在组件中，组件经 `owner` 读取宿主的 6 个字段（见[纸面迁移调查](paper-migration.zh.md#典型例子)的例 2）；宿主一侧的字段、信号接线和显示代码则在 Brick 与 Enemy 中各写一份。

### Ring 的核对

- `breakout/scenes/bricks/base/brick.gd` 有 77 行有效代码（非空、非注释的物理行），`enemy.gd` 有 190 行。brick.gd 的 77 行中，有 67 行能在 enemy.gd 中找到逐字相同的行；把 enemy 换成 brick 后为 70 行。纸面迁移调查人工数出的是 64 行。
- brick.gd 有 67 个逻辑行。K = 5 的精确检测找到 4 个区域，覆盖其中 36 行；近似检测覆盖 39 行。K = 3 时为 45 行和 48 行，K = 8 时为 20 行和 29 行。
- 漏掉的是被一行差异打断的短片段。例如两份 `@onready` 块只有 `hit_flash_animation_player` 一行写法不同（brick.gd:32，enemy.gd:36）；`physic_hurt` 中 Brick 多一行 `if hit_flash:`（brick.gd:100）；`Info` 和 `Basic` 两组导出中，字段名带 brick 或 enemy 前缀，顺序也不同。
- 所以检测是保守的：在这对文件上，K = 5 的精确检测只找到逐行比较所见重复的一半左右。

### 窗口大小

精确匹配下的重复行；括号内为每千行：

| K | 游戏：跨分支 | 同原生类 | 同继承链 | 插件与工具：跨分支 | 同原生类 | 同继承链 |
|---|---:|---:|---:|---:|---:|---:|
| 3 | 714 (12.4) | 2,571 | 4,205 | 1,960 | 2,005 | 3,592 |
| 5 | 353 (6.1) | 1,725 | 2,884 | 1,291 | 1,575 | 2,449 |
| 8 | 148 (2.6) | 1,016 | 1,770 | 890 | 1,349 | 1,667 |

- 窗口越小，各类重复越多，跨分支增加得最快：K = 3 时是 K = 5 的 2.0 倍，另两类为 1.5 倍。但三种窗口下，游戏的跨分支重复都少于另两类：同原生类是它的 3.6、4.9、6.9 倍，同继承链是 5.9、8.2、12.0 倍。
- K = 5 时有跨分支克隆的另外 4 个游戏仓库（godot-open-rts、tanks-of-freedom-ii、deck_builder_tutorial、godot-tiny-mmo），每千行的跨分支重复在 K = 3 时为 3.8 到 22.4 行，K = 5 时为 3.8 到 9.7 行，K = 8 时为 0 到 6.0 行。Ring 为 66.5、39.9、22.2 行，phantom-camera 为 170.2、146.8、111.6 行。K = 3 时 godot-open-rpg 也出现 6 行。
- 共同能力和变通写法的统计与 K 无关。

## 典型例子

**1. Ring：两个宿主复制了字段和方法，标为 (b)。**
`breakout/scenes/bricks/base/brick.gd:17-22, 64-75`；与 `breakout/scenes/enemies/base/enemy.gd:22-27, 206-216` 除空行外相同

```
@export var point: int
@export var drop_percent_scale: float = 1.0 #1.0表示正常掉率
@export var hit_by_ball_player_charge: int = 1 #被球击中时玩家获得多少充能

@export var lucky_drop: bool = false #会进行lucky drop
@export var unique_drop_name: String
...
func set_type(new_type : int):
	type = new_type
	if type < 0:
		return
	$Sprite2D.texture = sprites[type]


func show_health():
	health_bar.max_value = max_health
	health_bar.value = health_component.current_health

	health_label.text = str(health_component.current_health)
```

Brick 继承 StaticBody2D，Enemy 继承 CharacterBody2D，共同基类最多继承到 PhysicsBody2D，而 Enemy 用到了 CharacterBody2D 的 `velocity`。复制的部分既有导出字段，也有读写这些字段的方法。生命值和死亡掉落的逻辑已经放在 HealthComponent 和 DeathComponent 中，组件经 `owner` 读取宿主的 6 个字段，上面的 `point`、`drop_percent_scale`、`lucky_drop`、`unique_drop_name` 就在其中。

**2. phantom-camera：2D/3D 孪生脚本，标为 (b)。**
`addons/phantom_camera/scripts/phantom_camera/phantom_camera_2d.gd:407-412`；与 `phantom_camera_3d.gd:488-493` 逐字相同

```
@export_range(0.0, 1.0) var dead_zone_width: float = 0.0:
	set(value):
		dead_zone_width = value
		dead_zone_changed.emit()
	get:
		return dead_zone_width
```

`phantom_camera_2d.gd:269`；`phantom_camera_3d.gd:361`

```
@export var follow_offset: Vector2 = Vector2.ZERO:
@export var follow_offset: Vector3 = Vector3.ZERO:
```

PhantomCamera2D 继承 Node2D，PhantomCamera3D 继承 Node3D，公共祖先是 Node。精确克隆只包括与维度无关的部分。`follow_offset` 这类成员只差向量类型，精确和近似匹配都不把它们算作克隆，因为近似匹配不替换类型名；宽松匹配下该仓库的跨分支重复为 1,178 行。推断：要共享这部分，需要按向量类型参数化，单靠 mixin 不够。

**3. deck_builder_tutorial：读取各自成员的同一方法，标为 (a)；调用方逐类判断。**
`scenes/enemy/enemy.gd:93-103`；与 `scenes/player/player.gd:41-51` 逐字相同

```
func take_damage(damage: int, which_modifier: Modifier.Type) -> void:
	if stats.health <= 0:
		return

	sprite_2d.material = WHITE_SPRITE_MATERIAL
	var modified_damage := modifier_handler.get_modified_value(damage, which_modifier)

	var tween := create_tween()
	tween.tween_callback(Shaker.shake.bind(self, 16, 0.15))
	tween.tween_callback(stats.take_damage.bind(modified_damage))
	tween.tween_interval(0.17)
```

`effects/damage_effect.gd:8-14`

```
func execute(targets: Array[Node]) -> void:
	for target in targets:
		if not target:
			continue
		if target is Enemy or target is Player:
			target.take_damage(amount, receiver_modifier_type)
			SFXPlayer.play(sound)
```

Enemy 继承 Area2D，Player 继承 Node2D。`stats`、`sprite_2d`、`modifier_handler` 在两个类中分别声明，`stats` 的类型也不同（EnemyStats 与 CharacterStats，enemy.gd:7，player.gd:6）。之后的 `tween.finished` 回调也不同：敌人发出 `enemy_died`，玩家发出 `player_died`（enemy.gd:105-112，player.gd:53-60）。调用方先判断 `is Enemy or is Player`，再无类型地调用 `take_damage`。

**4. godot-tiny-mmo：两个宿主各有一份倒计时，标为 (c)。**
`source/client/ui/hud/trade_panel.gd:523-537`；`source/common/gameplay/trade/trade_table.gd:309-323` 除提示文字外相同

```
func _run_countdown(seconds: int) -> void:
	_stop_countdown()
	_countdown_tween = create_tween()
	_countdown_tween.tween_method(_set_countdown_text, float(seconds), 0.0, float(seconds))


func _set_countdown_text(value: float) -> void:
	_countdown_label.text = "Trade completes in %d…" % maxi(1, ceili(value))


func _stop_countdown() -> void:
	if _countdown_tween != null and _countdown_tween.is_valid():
		_countdown_tween.kill()
	_countdown_tween = null
	_countdown_label.text = ""
```

交易面板的原生基类是 Control，场景中的交易桌继承 Area2D。倒计时有自己的状态（`_countdown_tween`、`_countdown_label`），交易桌一份的提示文字是 `"Trading in %d…"`（trade_table.gd:316）。

**5. tanks-of-freedom-ii：五份相同的计算，标为 (d)。**
`scenes/ui/menu/skirmish/skirmish.gd:78-92`；另外 4 份的克隆区域在 `scripts/services/relay.gd:236-244`、`scripts/services/multiplayer.gd:134-142`、`scenes/ui/multiplayer/lobby.gd:172-180`、`scenes/ui/online/lobby.gd:176-184`

```
func _gather_player_sides(map_data):
	var sides = {}
	var side
	var key

	for y in range(self.map_list_service.MAX_MAP_SIZE):
		for x in range(self.map_list_service.MAX_MAP_SIZE):
			key = str(x) + "_" + str(y)
			if map_data["tiles"].has(key):
				side = self._lookup_side(map_data["tiles"][key])

				if side != null:
					sides[side] = side

	return sides
```

5 份分布在 2 个服务（Node）和 3 个菜单（Control）中，只依赖参数 `map_data`、autoload `MapManager` 的常量（skirmish.gd:3）和同文件的 `_lookup_side`。relay.gd 中的一份把 `side != null` 写成了 `side != ""`（relay.gd:246）。

## 结论

- **跨分支重复确实存在，但不多，而且集中。** 游戏中每千行 6.1 行；有这类重复的 5 个游戏仓库中，4 个在 3.8 到 9.7 行之间。Ring 为 39.9 行，2D/3D 孪生插件 phantom-camera 为 146.8 行。
- **继承本可消除却没有消除的重复要多得多。** 游戏中同原生类和同继承链的重复分别是跨分支的 4.9 倍和 8.2 倍，三种窗口下顺序不变；不过这两类主要来自 tanks-of-freedom-ii。
- **跨分支重复主要是普通方法**，字段声明很少。复制了字段和导出属性的主要是 Ring 和 phantom-camera。
- **按阅读判断，游戏中一半多的跨分支重复（353 行中的 185 行）可以用组合或静态辅助函数消除**，这两种机制 GDScript 已经有。接口默认方法对应 54 行；带状态的 mixin 对应 102 行，其中 72 行来自 Ring。插件与工具中带状态的 mixin 对应 948 行，其中 734 行来自 phantom-camera，而 phantom-camera 中只差向量类型的部分不在这些数字里。
- **显式的跨类型调用很少。** 游戏的 2,081 个消费点中只有 55 处，一半以上的消费点接收者没有类型。
- **组合已经被用来跨分支共用代码**，游戏中有 11 个这样的组件。Ring 同时用了组合和复制。

依据这些数据所作的决定见[语言规范](../language-spec.zh.md) §3.9、§4.1：带状态的共同基类用 `@abstract`，不同引擎基类的类共有的能力用接口，第一版没有带状态的 mixin。规范中接口的默认方法可以通过接口属性读写状态，范围比本文的标签 (a) 宽。

## 局限

- **检测是保守的。** Ring 的核对表明，K = 5 的精确检测只找到逐行比较所见重复的一半左右。被一行差异打断、或短于窗口的重复都会漏掉。
- **近似匹配不替换成员名和类型名。** 改了名的字段，以及只差向量类型的孪生代码，都不算精确或近似克隆。宽松匹配是上限，误报很多。
- **名字证据是按单元、按名字判断的。** 有名字证据的单元可能只在重复片段以外用到那些成员；经其他接收者、`call` 或 `get` 的访问看不到。没有名字证据只说明引擎不阻止共用基类：若一方已有脚本基类，共用基类要插进那条继承链；其他代码可能按具体类型引用这些节点；这些代价都没有检查。
- **机制标签是 AI 代理的判断。** (a) 与 (b)、(b) 与 (c) 的界线并不清楚；phantom-camera 的 33 个组按一条规则统一标注；只标注了 K = 5 的精确克隆组，只在近似匹配下出现的重复（例如 godot-tiny-mmo 的界面片段）没有标注。
- **共同能力按名字判断**，是上限，名字多为通用词。
- **语料偏斜。** 游戏代码的 77% 来自 godot-tiny-mmo 和 tanks-of-freedom-ii，Kenney 入门模板都很小；示例由 132 个小项目组成；插件与工具中 Pixelorama 和 gdUnit4 占约 70% 的代码。
- **只看文本场景和 GDScript。** 用代码 `add_child` 或 `set_script` 挂上的组件看不到；`.scn` 二进制场景和 C# 代码不在内。

## 复现

脚本放在 [`cross-branch-duplication/`](cross-branch-duplication/) 目录，只需要 Python 3（标准库，在 3.11 上运行）和 git。`gdcorpus.py` 列出语料和第一方规则，并能拉取语料；`build_api.py` 从 Godot 类参考 XML 提取引擎类的成员和继承关系；`dup.py` 做克隆检测、共同能力和变通写法的统计；`components.py` 统计组件；`make_labels.py` 保存阅读所得的标签，并把它们与检测结果对应起来，生成 `labels.csv`；`report.py` 汇总出 `report.txt`，本文的统计数字都在其中或由其中的数字算出；`ring_check.py` 做 Ring 的核对。godot-tiny-mmo 的界面片段组见重新生成的 `groups_near.csv`。

`api.json` 约 770 KB，不随附，由 `build_api.py` 从 Godot 提交 `469659e` 的检出生成。随附的结果在 `out/`（K = 5：`report.txt`、`summary.json`、`workarounds.json`、`components.csv`、`components_summary.json`、`ring_check.txt`）、`out_k3/report.txt` 和 `out_k8/report.txt` 中。逐区域和逐组的 CSV、代码片段等大文件由脚本重新生成。

```
python docs/surveys/cross-branch-duplication/gdcorpus.py corpus     # 20 个固定提交的浅克隆，只检出 .gd、.tscn 等文本文件
python docs/surveys/cross-branch-duplication/build_api.py ../godot work/api.json   # Godot 检出在 469659e
python docs/surveys/cross-branch-duplication/dup.py --corpus corpus --api work/api.json --out work/out
python docs/surveys/cross-branch-duplication/components.py --corpus corpus --api work/api.json --out work/out
python docs/surveys/cross-branch-duplication/make_labels.py work/out --labels work/labels.csv   # 应与随附的 labels.csv 相同
python docs/surveys/cross-branch-duplication/report.py work/out    # 写出 work/out/report.txt
python docs/surveys/cross-branch-duplication/dup.py --corpus corpus --api work/api.json --out work/out_k3 --k 3
python docs/surveys/cross-branch-duplication/report.py work/out_k3
python docs/surveys/cross-branch-duplication/dup.py --corpus corpus --api work/api.json --out work/out_k8 --k 8
python docs/surveys/cross-branch-duplication/report.py work/out_k8
python docs/surveys/cross-branch-duplication/ring_check.py --corpus corpus --api work/api.json work/out work/out_k3 work/out_k8
```
