# 调查：真实的 Godot 4 项目如何使用属性的 getter

**2026-10-22** · 关联：[语言规范](../language-spec.zh.md) §4.4、§6.1、§6.5 · English: [property-accessors.en.md](property-accessors.en.md)

本文调查真实的 Godot 4 代码如何使用属性访问器，用来回答规范 v0.1（git 标签 `spec-v0.1`）的一个设计问题：是否把属性分成存储属性和计算属性。做法是先在真实代码上统计所关心的写法出现得有多频繁，再看各个候选设计会对这些代码产生什么影响。v0.1 采用了下文的设计 B；现行规范沿用 GDScript 的访问器，即设计 A（§4.4），并让就绪前的 setter 自动挂起（§6.5）。结论依赖于已经改变的规则时，文中会注明。

## 问题

GDScript 4 的成员变量可以带 setter、getter，或者两者都带。写法有两种：内联写法（`var x: T = init:` 后接一个缩进块，块内写 `set(value):` 和 `get:`）和具名访问器（`var x: T: set = _set_x, get = _get_x`）。getter 总是挂在一个存储字段上；在属性自己的访问器内部，`x` 指的就是这个字段。v0.1 考虑过两种设计：

| 设计 | 存储属性 | 计算属性 |
|---|---|---|
| A 沿用 GDScript：访问器挂在存储字段上（现行规范，§4.4） | 一个字段，可选 setter，可选 getter。每次读取都会执行 getter，包括引擎保存、复制或检视对象时的读取 | 不单独成类；写成一个 getter 不理会自身字段的属性 |
| B 拆分存储属性与计算属性（v0.1 §8.6） | 一个字段，可选 setter，不能有 getter。读取总是直接读字段，所以引擎的读取不会执行用户代码 | 没有字段；必须有 getter；setter 可选，没有 setter 即为只读；不能 `@export`；引擎看到的是不带 `PROPERTY_USAGE_STORAGE` 的属性 |

B 的用意是让引擎读取存储数据时不执行用户代码，让代理和派生属性不再带着无用的存储，并且可以声明只读属性。在 B 下，getter 读取自身字段、同时还做别的事的 GDScript 属性，必须改写成一个私有存储字段加一个计算属性。我们想知道：getter 出现得有多频繁，它们在做什么，有多少需要这样改写，受影响的属性中有多少是导出的。两种设计都在存储属性上保留 setter，所以我们也记录了 setter 在做什么，特别是在 `_ready` 之前就会执行的导出属性 setter。

## 语料

我们选了 14 个 MIT 许可的 Godot 4 仓库，涵盖官方示例、一个 RPG 示例、小型入门模板、一个像素画编辑器、五个编辑器插件和一个游戏模板。候选指带 setter 或 getter 的成员变量。

| 仓库 | 提交 | `.gd` 文件 | 行数 | 第一方行数 | 候选（第一方） |
|---|---|---|---|---|---|
| [godotengine/godot-demo-projects](https://github.com/godotengine/godot-demo-projects) | `b761b4c` | 461 | 30,819 | 30,819 | 39（39） |
| [gdquest-demos/godot-open-rpg](https://github.com/gdquest-demos/godot-open-rpg) | `d59f2f7` | 342 | 40,662 | 5,422 | 138（59） |
| [KenneyNL/Starter-Kit-3D-Platformer](https://github.com/KenneyNL/Starter-Kit-3D-Platformer) | `3fa8a04` | 9 | 378 | 378 | 0 |
| [KenneyNL/Starter-Kit-City-Builder](https://github.com/KenneyNL/Starter-Kit-City-Builder) | `4535092` | 6 | 311 | 311 | 0 |
| [KenneyNL/Starter-Kit-FPS](https://github.com/KenneyNL/Starter-Kit-FPS) | `185fd23` | 7 | 451 | 451 | 0 |
| [KenneyNL/Starter-Kit-Racing](https://github.com/KenneyNL/Starter-Kit-Racing) | `2f2e5f2` | 3 | 258 | 258 | 0 |
| [godotengine/tps-demo](https://github.com/godotengine/tps-demo) | `a82f154` | 15 | 1,619 | 1,619 | 2（2） |
| [Orama-Interactive/Pixelorama](https://github.com/Orama-Interactive/Pixelorama) | `38a8b03` | 249 | 60,025 | 54,782 | 216（194） |
| [ramokz/phantom-camera](https://github.com/ramokz/phantom-camera) | `21d3762` | 36 | 10,410 | 10,410 | 181（181） |
| [nathanhoad/godot_dialogue_manager](https://github.com/nathanhoad/godot_dialogue_manager) | `7a72dcf` | 79 | 14,619 | 12,434 | 29（29） |
| [bitbrain/beehave](https://github.com/bitbrain/beehave) | `fe58915` | 322 | 35,193 | 4,469 | 41（23） |
| [derkork/godot-statecharts](https://github.com/derkork/godot-statecharts) | `76d226a` | 141 | 18,768 | 4,635 | 51（16） |
| [MikeSchulze/gdUnit4](https://github.com/MikeSchulze/gdUnit4) | `9bc1a45` | 482 | 54,133 | 27,406 | 21（18） |
| [Maaack/Godot-Game-Template](https://github.com/Maaack/Godot-Game-Template) | `6849d6c` | 125 | 8,553 | 5,128 | 41（28） |
| **合计** | | **2,277** | **276,199** | **158,522** | **759（589）** |

**许可。** 14 个仓库都是 MIT 许可。godot-demo-projects 和 Godot-Game-Template 的许可文件只有 MIT 条文、没有标题。tps-demo 的素材和音乐采用 CC-BY 3.0，代码（“All code Copyright (c) 2018-2021 Juan Linietsky, Godot Engine contributors”）采用 MIT，因此其代码计入。

**第一方**指上游就在本仓库的代码：`addons/` 以外的全部代码；仓库本身要发布的那个插件（phantom_camera、dialogue_manager、beehave、godot_state_charts、gdUnit4、maaacks_game_template）；以及 godot-demo-projects 中所有的 `addons/` 目录，因为这些插件本身就是示例的一部分。其余代码不计入：

- 随附的插件：godot-open-rpg 中的 Dialogic（79 个候选）、godot-statecharts 中的 GUT（35）、beehave 中附带的 gdUnit4（18）、Pixelorama 附带的六个插件（22），以及 Godot-Game-Template 附带的另外五个插件（12）。最后这五个与模板同一作者，但各有自己的仓库。
- 重复代码：Godot-Game-Template 的安装器会把插件里的示例复制到项目根目录的 `scenes/` 和 `scripts/`（1 个候选）。
- 单元测试（beehave 的 `test/`、gdUnit4 的 `addons/gdUnit4/test/`、dialogue_manager 的 `tests/`、statecharts 的 `tests/`，共 35,226 行）单独报告。测试中只有 3 个候选，都在 gdUnit4：两个带平凡 getter，一个只有 setter。下文所有比率都不含测试。

没有文件使用 Godot 3 的 `setget`。

## 方法

1. **提取候选。** `extract.py` 去掉注释、清空字符串内容、合并续行，并跟踪类和函数的代码块。它找出类层级的成员变量（含静态成员），两种写法都识别：内联块（`set(v):`、`get:`、`get():`）和具名访问器（`set = f`、`get = g`，写在声明行上或缩进块中）。语法依据 `GDScriptParser::parse_variable` 和 `parse_property`（Godot 提交 `469659e` 的 `modules/gdscript/gdscript_parser.cpp:1241-1387`）。对每个候选，记录它是否有 setter 和 getter、是否带 group、subgroup、category 以外的 `@export*` 注解、是否 `static`、是否 `@onready`、初始化式和声明类型，以及每个访问器的函数体：内联块，或同一个类中的具名函数。用 grep 查找形似访问器的行做交叉核对，没有发现提取脚本漏掉的。
2. **逐个阅读 getter，不抽样。** 有 195 个 getter 字面上就是 `return <字段>`。其余 56 个全部读过，每个 getter 归入一类：
   - (a) 平凡：只返回字段。在 B 下直接删除。
   - (b) 读取字段，同时还做别的事：惰性初始化、回退、副作用。在 B 下需要改写。
   - (c) 不读取字段：派生值、同一对象其他成员的视图，或另一个对象的代理。在 B 下就是计算属性。

   我们还标出了过早读取时会产生动作或可能失败的 getter。
3. **逐个阅读 setter**（第一方 557 个，测试中 3 个），分为四类：*平凡*（只存值）、*副作用*（原样存值，另外还做别的事：发信号、重绘、更新其他节点）、*校验*（存入变换后的值，例如钳制或规范化后的值，或者拒绝某些值）、*代理*（从不写自身字段）。
4. **在 `_ready` 之前执行的 setter。** 对每个带 setter 的导出属性，`extract.py` 列出 setter 直接或经同类函数和访问器（最多两层调用）接触到的东西：子节点（`$`、`%`、`get_node`、`find_child`、`get_child`、`get_children`）、本脚本及其基类脚本的 `@onready` 成员，以及在 `_ready` 中赋值的成员。这样找到 112 个 setter，阅读时又发现 1 个，共 113 个，我们逐个读过并记录了它们各自如何防护。
5. **引擎的读取。** 以下事实在 Godot 提交 `469659e` 上核对：
   - `GDScriptInstance::get` 调用成员的 getter，`set` 调用其 setter（`modules/gdscript/gdscript.cpp:1636-1641`、`1571-1575`）。
   - 导出成员采用导出注解给出的 usage；其他成员只有 `PROPERTY_USAGE_SCRIPT_VARIABLE`，因而没有 `STORAGE`（`modules/gdscript/gdscript_compiler.cpp:2877-2884, 2908`；`modules/gdscript/gdscript_parser.cpp:5420`）。
   - 保存场景和 `Node.duplicate()` 都用 `get` 读取每个带 `STORAGE` 的属性（`scene/resources/packed_scene.cpp:947-964`；`scene/main/node.cpp:3113-3123`）。远程检查器读取每个属性和每个脚本成员，不论是否导出（`scene/debugger/scene_debugger_object.cpp:85, 152`）。
   - 实例化场景时，先设置节点保存的属性（`packed_scene.cpp:567`），再把节点加到父节点下（`:616`），所以这时它自己的子节点还不存在。Node 类型的导出属性要等所有节点都创建后才设置（`:449-468, 677-693`）。被实例化的子场景先完整构建，连同其子节点（`:334-337`），之后才应用外层场景为它设置的值。

逐个分类的工作由 AI 代理完成。分类结果记录在 `classification.json` 中，下文所有数字都可以用 `count.py` 由它重新算出。

## 结果

### 访问器出现得有多频繁

| | 属性 | 导出 | 每千行 |
|---|---|---|---|
| 只有 setter | 338 | 158 | 2.13 |
| 只有 getter | 32 | 0 | 0.20 |
| setter 和 getter 都有 | 219 | 160 | 1.38 |
| **合计** | **589** | **318** | **3.72** |

分布很不均匀：phantom-camera 每千行有 17.4 个带访问器的属性，godot-open-rpg 有 10.9 个，而四个 Kenney 入门模板一个都没有。有 7 个属性是静态的，没有一个是 `@onready`。

### getter 的分类

| 类别 | getter | 导出 | 带 setter | 每千行 | 在 B 下 |
|---|---|---|---|---|---|
| (a) 只返回字段 | 196 | 155 | 191 | 1.24 | 删除 getter |
| (b) 读取字段并做更多的事 | **15** | 1 | 2 | 0.09 | 改写为私有字段加计算属性 |
| (c) 不读取字段 | 40 | 4 | 26 | 0.25 | 成为计算属性 |
| **合计** | **251** | **160** | **219** | **1.58** | |

- (a) 类是三个项目的编码习惯：phantom-camera（153）、godot_dialogue_manager（28）和 gdUnit4（11）占了 196 个中的 192 个。phantom-camera 给大多数导出属性都配了公开的 `set_x`/`get_x` 方法（(a) 类中有 128 个是具名写法）。在 B 下这些方法可以作为普通方法保留，只需去掉 `get = get_x`。有一个 (a) 类 getter（`phantom_camera_2d.gd:475`）带分支，但它只在字段本来就是空 NodePath 时返回空 NodePath，所以与直接返回字段等价。
- (b) 类出现在三个仓库：Pixelorama 10 个、gdUnit4 4 个、beehave 1 个。只有一个是导出的。
- 在全部 56 个非平凡 getter 上，“getter 是否提到自身字段”都能把 (a)、(b) 与 (c) 分开，所以迁移工具可以机械地完成这一分类。

### 全部 (b) 类 getter

| 位置 | getter 做了什么 |
|---|---|
| `beehave/addons/beehave/nodes/beehave_tree.gd:51` `blackboard`（导出） | 回退到一个内部 Blackboard，首次读取时创建它并加为子节点 |
| `gdUnit4/addons/gdUnit4/src/core/attributes/TestCaseAttribute.gd:19` `timeout` | 惰性默认值：首次读取时把哨兵值 `DEFAULT_TIMEOUT` 换成项目设置中的值，并写回字段 |
| `gdUnit4/addons/gdUnit4/src/core/execution/GdUnitExecutionContext.gd:12` `error_monitor` | 有父上下文时返回父上下文的监视器；否则首次读取时创建自己的 |
| `gdUnit4/addons/gdUnit4/src/core/execution/GdUnitExecutionContext.gd:19` `test_suite` | 有父上下文时返回父上下文的值，否则返回自己的字段 |
| `gdUnit4/addons/gdUnit4/src/core/execution/GdUnitExecutionContext.gd:24` `test_case` | 返回自己的字段；字段为 null 时回退到父上下文的值 |
| `Pixelorama/src/Main.gd:16` `splash_dialog` | 惰性创建：首次读取时实例化对话框场景并加为子节点 |
| `Pixelorama/src/Palette/PalettePanel.gd:14` `create_palette_dialog` | 惰性创建：实例化对话框、连接信号并加为子节点 |
| `Pixelorama/src/Palette/PalettePanel.gd:21` `edit_palette_dialog` | 惰性创建：实例化对话框、连接信号并加为子节点 |
| `Pixelorama/src/UI/Recorder/Recorder.gd:31` `_path_dialog` | 惰性创建：构造并配置一个 FileDialog，加为子节点 |
| `Pixelorama/src/Autoload/DrawingAlgos.gd:11, 16, 37, 42`（四个着色器） | 惰性加载：首次读取时加载着色器 |
| `Pixelorama/src/Autoload/DrawingAlgos.gd:21, 29`（两个着色器） | 惰性创建：复制另一个惰性加载的着色器并修改其代码 |

15 个中有 12 个是惰性初始化或缓存，2 个回退到父对象，1 个兼有回退和惰性创建。有 5 个在读取时添加子节点。

### (c) 类

| 种类 | getter | 带 setter | 只读 | 例子 |
|---|---|---|---|---|
| 另一个对象的代理 | 22 | 21 | 1 | phantom-camera 中 `camera_3d_resource` 的 10 个字段和 `tween_resource` 的 6 个字段；子节点的位置；`player.stream_paused` |
| 同一对象其他成员的视图 | 10 | 4 | 6 | 暴露私有成员的只读列表和标志；另一个导出属性的别名；一个静态变量的代理 |
| 派生值 | 8 | 1 | 7 | 布局尺寸、报告路径、一次目录扫描、随机选出的图块 |
| **合计** | **40** | **26** | **14** | |

- GDScript 无法让那 14 个只读属性真正只读：给它们赋值会悄悄写入那个用不到的字段，getter 仍然返回计算出来的值。
- 40 个中有 8 个给一个从不被读取的字段写了初始化式。
- 有两个 setter 写入了那个用不到的字段。`phantom_camera_3d.gd:811` 的 `keep_aspect` 写的是自身字段，而它的 getter 读的是 `camera_3d_resource`，所以资源从未被更新；与它并列的另外九个属性都会更新资源。`LayerButton.gd:22` 的 `button_pressed` 同时写自身字段和一个子按钮，读取时只读按钮。
- 4 个是导出的：`conductor.gd:7` 的 `is_paused` 和 `node_25d.gd:13` 的 `spatial_position` 是另一个节点的代理，`phantom_camera_2d.gd:435` 的 `draw_limits` 是一个静态变量的代理，`phantom_camera_3d.gd:528` 的 `spring_length` 是导出属性 `follow_distance` 的别名。在 B 下它们不能继续作为导出的计算属性。

### 导出的 getter 与引擎的读取

全部 160 个导出 getter 都会在保存场景、复制节点和远程检查器读取时执行。其中 155 个属于 (a) 类，对它们来说没有区别。另外 5 个是 `blackboard`（(b) 类）和上面那 4 个导出的 (c) 类属性。未导出的 getter 只会被远程检查器读取。

过早读取时会产生动作或可能失败的 getter：

| 位置 | 类别 | 行为 |
|---|---|---|
| `beehave_tree.gd:51` `blackboard`（导出） | b | 任何读取（包括保存）都可能添加子节点；保存记录的是内部 blackboard，而不是字段 |
| `Main.gd:16`、`PalettePanel.gd:14, 21`、`Recorder.gd:31` | b | 任何读取（包括远程检查器的读取）都会添加子节点 |
| `node_25d.gd:13` `spatial_position`（导出） | c | 调用 `get_child(0)`；节点还没有子节点时失败 |
| `ExtensionsApi.gd:253` `tabs_visible` | c | 使用 `find_child`；界面尚未建立时失败 |
| `LayerButton.gd:22` `button_pressed` | c | 读取 `@onready` 成员；在 `_ready` 之前失败 |

### setter

| 属性 | 合计 | 平凡 | 副作用 | 校验 | 代理 | 导出 |
|---|---|---|---|---|---|---|
| 只有 setter | 338 | 4 | 299 | 31 | 4 | 158 |
| setter 和 getter 都有 | 219 | 45 | 119 | 31 | 24 | 160 |
| **全部 setter** | **557** | **49** | **418** | **62** | **28** | **318** |

- 大多数 setter 原样存值，然后通知或更新别的东西。做校验的很少。
- 四个只有 setter 的代理中，有两个是误把值丢掉了。`WebSocketServer.gd:14` 的 `refuse_new_connections` 在第 95 行被读取，却从未被存下，所以新连接永远不会被拒绝。`field_camera.gd:10` 的 `gameboard_properties` 从未被存下，所以相机边界永远不会被计算。另外两个，一个是只写的别名（`phantom_camera_3d_gizmo_plugin.gd:9`），一个把值转发给子标签（`PointCollapseContainer.gd:3`）。
- 有 30 个 setter（其中 16 个导出）含有 `await`，其中 25 个是 `await ready`。两种设计和现行规范（§4.4）都不允许 setter 是协程，所以这些 setter 都需要改写。没有 getter 含有 `await`。
- 163 个具名 setter 中有 5 个被子类重新定义函数而覆盖，覆盖时通常会调用 `super`。145 个具名 getter 没有一个被覆盖。

### 在 `_ready` 之前执行的 setter

318 个带 setter 的导出属性中，有 113 个会接触子节点、`@onready` 成员或在 `_ready` 中赋值的成员。如果值保存在场景里，setter 会在实例化期间执行，这时节点自己的子节点还不存在，`@onready` 成员也还没有赋值。

| 结论 | setter |
|---|---|
| 有防护：`is_inside_tree()` 或 `is_node_ready()`，其中 15 个随后 `await ready` | 39 |
| 有防护：对所接触成员做 null 或 `is_instance_valid` 检查 | 16 |
| 有防护，方式不同：检查子节点数量、检查在 `_ready` 中或进入场景树时设置的标志、自定义的首次调用标志 | 6 |
| 无防护：直接用 `$Child` 访问 | 42 |
| 可容忍：`get_children()` 或 `has_node()`，只是找不到子节点 | 3 |
| 加载时不会执行：从不存入 `true` 的检查器按钮；引擎最后才设置的 Node 类型导出属性 | 2 |
| 只接触在 `_ready` 中赋值的普通数据 | 5 |

- 接触 `@onready` 成员的 20 个 setter 全部有防护。
- 42 个无防护的 setter 出自两个项目：Pixelorama 的滑块控件（`BasisSliders.gd` 6 个、`ValueSliderV2.gd` 16 个、`ValueSliderV3.gd` 18 个、`PointCollapseContainer.gd` 1 个），以及 tps-demo 的 `player.gd:38` `player_id`，后者由 `level.gd` 在实例化之后用代码设置。滑块之所以能工作，是因为这些值只在其场景的实例上设置，那时子场景的子节点已经存在。在 `ValueSliderV2.tscn` 和 `BasisSliders.tscn` 中，根节点保持默认值，由使用它们的场景覆盖这些值。
- 有一个带防护的 setter 仍然会丢值：`conductor.gd:7` 的 `is_paused` 检查了 `if player`，但 `player` 是 Node 类型的导出属性，引擎在其他属性之后才设置它，所以场景中保存的 `is_paused = true` 会被丢掉。
- 有 13 个属性在 `_ready` 中用 `x = x` 重新执行自己的 setter（Godot-Game-Template 11 个，godot-demo-projects 2 个）。这依赖于类内部的赋值经过 setter，现行规范保留了这一点（§4.4）；成员的初始化式则不经过 setter（§6.1）。
- 另有 79 个未导出的 setter 也会接触这类成员。它们只在代码给属性赋值时执行，我们没有检查它们的防护。
- 现行规范由编译器完成这类防护：就绪之前，setter 在第一次访问 `@onready` 成员、`$X` 或子节点处挂起，节点就绪后继续执行（§6.5）。

### 各仓库

| 仓库 | 属性 | 每千行 | 只有 setter | (a) | (b) | (c) | 导出 |
|---|---|---|---|---|---|---|---|
| godot-demo-projects | 39 | 1.27 | 36 | 1 | 0 | 2 | 27 |
| godot-open-rpg | 59 | 10.88 | 59 | 0 | 0 | 0 | 30 |
| tps-demo | 2 | 1.24 | 2 | 0 | 0 | 0 | 2 |
| Pixelorama | 194 | 3.54 | 177 | 1 | 10 | 6 | 49 |
| phantom-camera | 181 | 17.39 | 6 | 153 | 0 | 22 | 154 |
| godot_dialogue_manager | 29 | 2.33 | 0 | 28 | 0 | 1 | 4 |
| beehave | 23 | 5.15 | 19 | 1 | 1 | 2 | 17 |
| godot-statecharts | 16 | 3.45 | 11 | 1 | 0 | 4 | 9 |
| gdUnit4 | 18 | 0.66 | 0 | 11 | 4 | 3 | 0 |
| Godot-Game-Template | 28 | 5.46 | 28 | 0 | 0 | 0 | 26 |

四个 Kenney 入门模板中没有带访问器的属性。

## 典型例子

**1. 具名写法的 (a) 类。**
`phantom-camera/addons/phantom_camera/scripts/phantom_camera/phantom_camera_2d.gd:133-135, 1371-1372`

```
@export var priority: int = 0:
	set = set_priority,
	get = get_priority
...
func get_priority() -> int:
	return priority
```

在 B 下去掉 `get = get_priority` 这一行即可；`get_priority()` 可以继续作为公开方法。

**2. (b) 类：惰性创建。**
`Pixelorama/src/Main.gd:16-21`

```
var splash_dialog: AcceptDialog:
	get:
		if not is_instance_valid(splash_dialog):
			splash_dialog = load(SPLASH_DIALOG_SCENE_PATH).instantiate()
			add_child(splash_dialog)
		return splash_dialog
```

在 B 下它变成一个私有存储字段 `_splash_dialog` 加一个计算属性 `splash_dialog`，getter 不变。这一改写是机械的。它并不能阻止远程检查器创建对话框，因为检查器会读取每个属性，计算属性也不例外。

**3. 导出的 (b) 类：引擎的读取改变了场景树。**
`beehave/addons/beehave/nodes/beehave_tree.gd:51-67`

```
@export var blackboard: Blackboard:
	set(b):
		blackboard = b
		...
	get:
		# in case blackboard is accessed before this node is,
		# we need to ensure that the internal blackboard is used.
		if not blackboard and not _internal_blackboard:
			_internal_blackboard = Blackboard.new()
			add_child(_internal_blackboard, false, Node.INTERNAL_MODE_BACK)
		return blackboard if blackboard else _internal_blackboard
```

这个脚本是 `@tool`。编辑器保存一个没有指定 blackboard 的场景时，保存过程会调用这个 getter：它可能添加一个内部子节点，而且保存记录下的是 getter 的结果，即内部 blackboard，而不是值为 null 的字段。在 B 下，保存和复制读取的是字段，回退逻辑则移到一个由树在内部使用的计算属性或方法里。

**4. (c) 类：代理的 setter 写了用不到的字段。**
`phantom-camera/addons/phantom_camera/scripts/phantom_camera/phantom_camera_3d.gd:2850-2859, 2865-2870`

```
func set_keep_aspect(value: int) -> void:
	if not camera_3d_resource:
		printerr("Can't assign a keep_aspect value. No Camera3DResource assigned to ", name)
		return
	keep_aspect = value              # 自身字段，没有任何地方读取
	camera_3d_resource_property_changed.emit("keep_aspect", value)

func get_keep_aspect() -> Variant:
	if not camera_3d_resource: return null
	return camera_3d_resource.keep_aspect
...
func set_cull_mask(value: int) -> void:
	...
	camera_3d_resource.cull_mask = value
```

与它并列的九个属性都写资源，只有这一个写了没有任何地方读取的字段。在 B 下计算属性没有字段可写，这样的错误就不会悄无声息。

**5. 导出的代理，场景中保存的值被丢掉。**
`godot-demo-projects/audio/rhythm_game/game_state/conductor.gd:7-18`

```
@export var is_paused: bool = false:
	get:
		if player:
			return player.stream_paused
		return false
	set(value):
		if player:
			player.stream_paused = value

@export_group("Nodes")
## The song player.
@export var player: AudioStreamPlayer
```

`player` 是 Node 类型的导出属性，引擎在其他属性之后才设置它。因此场景中保存的 `is_paused = true` 到达 setter 时 `player` 还是 null，值就被丢掉了。在 B 下 `is_paused` 不能是导出的计算属性；它会变成一个存储属性，setter 把值转发给 player，并在 `_ready` 中应用保存的值。

**6. 从不存值的 setter。**
`godot-open-rpg/src/field/field_camera.gd:10-12, 38-40`

```
@export var gameboard_properties: GameboardProperties:
	set(value):
		_on_viewport_resized()
...
func _on_viewport_resized() -> void:
	if not gameboard_properties:
		return
```

值从未被存下，所以 `_on_viewport_resized` 总是提前返回。两种设计都允许这样写：这是一个带 setter 的存储属性。

**7. 导出属性的 setter 无防护地访问 `$Child`。**
`Pixelorama/src/UI/Nodes/Sliders/ValueSliderV2.gd:24-30`

```
@export var value := Vector2.ZERO:
	set(val):
		value = val
		$GridContainer/X.set_value_no_signal_update_display(value.x)
		$GridContainer/Y.set_value_no_signal_update_display(value.y)
		if _can_emit_signal:
			value_changed.emit(value)
```

如果 `value` 保存在 `ValueSliderV2.tscn` 的根节点上，这个 setter 会在 `GridContainer` 存在之前执行。它之所以能工作，是因为只有实例化它的场景才保存这个值，而那时子场景的子节点已经存在。按现行规范（§6.5），这种情况下 setter 会在访问 `$GridContainer/X` 处挂起，节点就绪后再继续。

**8. 常见的防护写法，它是一个协程。**
`godot-open-rpg/src/field/cutscenes/popups/interaction_popup.gd:30-37`

```
@export var radius: = 32:
	set(value):
		radius = value

		if not is_inside_tree():
			await ready

		_collision_shape.shape.radius = radius
```

godot-open-rpg 有 20 个 setter 这样防护。因为有 `await`，这个 setter 是协程，两种设计都不允许（§4.4）。现行规范让编译器自动完成这里手写的防护（§6.5）。

## 结论

前六点比较的是 v0.1 的两种设计。现行规范采用设计 A（§4.4），getter 的写法与 GDScript 相同，上文的 getter 都不需要改写。

- **getter 不多见，大多数是平凡的。** 每千行第一方代码有 1.58 个 getter。其中 78% 只返回字段，而这 196 个中的 192 个来自三个项目的习惯。在 B 下直接删掉即可。
- **B 要求改写的写法很少见。** 存储字段加非平凡 getter 出现 15 次（每千行 0.09 个），分布在三个仓库。其中 12 个是惰性初始化或缓存，只有 1 个是导出的。每一个都改写成私有存储字段加计算属性。
- **计算属性确实存在，而且比 (b) 类更常见。** 共 40 个（每千行 0.25 个）：代理、私有成员的视图和派生值。在 A 下它们都带着无用的存储，其中 8 个还给这份存储写了初始化式，14 个无法声明为只读，还有两个 setter 写了这份存储，其中一个是写错了。B 去掉了这份存储，并让那 14 个只读属性可以声明为只读。
- **B 的代价是 158,522 行中 19 处改写（每千行 0.12 处）：** 15 个 (b) 类，加上 4 个不能继续作为导出计算属性的导出 (c) 类属性。其余都是机械的；“getter 是否提到自身字段”能正确区分全部 56 个非平凡 getter。
- **目前引擎的读取很少执行非平凡 getter。** 160 个导出 getter 中有 5 个非平凡，其中一个（`blackboard`）会改变场景树和保存下来的值。在 B 下，保存和复制只读取字段，因此不会执行这类代码；远程检查器仍然会读取计算属性。
- **数据不构成反对 B 的理由。** 它的代价小，而且可以机械地找出来，同时消除了一类实际存在的意外。
- **与设计选择无关的 setter 结论。**
  - 在本语料中，接触 `@onready` 成员的导出 setter 全都有防护。
  - 42 个无防护访问 `$Child` 的 setter，依赖于值只在场景实例上设置。
  - 有 30 个 setter 使用 `await`，大多是 `await ready`，而 setter 不能是协程（§4.4）。因此这条规则本身也有迁移代价，约每千行 0.19 处。`if not is_node_ready(): await ready` 这类防护，由现行规范的自动挂起取代（§6.5）。

## 局限

- **两个项目占了大头。** phantom-camera 和 Pixelorama 贡献了 589 个属性中的 375 个；插件和工具使用访问器远多于游戏代码，Kenney 入门模板一个都没有。
- **游戏代码偏少。** 第一方代码大多是工具、插件和示例。更大的游戏项目可能更依赖惰性 getter。
- **没有调查场景文件。** 只检出了 `.gd` 文件。无防护的 setter 是否真的会出错，取决于值保存在哪里；我们只对 Pixelorama 的滑块场景做了核对，单独取回了 `ValueSliderV2.tscn` 和 `BasisSliders.tscn`。
- **setter 隐患的查找是机械的，而且有边界。** 它只跟踪同一个类内最多两层的调用；除基类脚本的 `@onready` 成员外，不跟踪其他类的成员。它会标出所有在 `_ready` 中赋值的成员，不论赋的是什么值，因此也标出了 5 个只接触普通数据的 setter。未导出的 setter 没有检查防护。
- **分类是人工判断。** 每个 getter 和 setter 都由 AI 代理阅读并分类。类别是判断的结果；引用的代码和统计数字都对照源码核对过。
- **不含随附代码。** Dialogic、GUT 等随附插件中另有 166 个候选，没有分类。

## 复现

脚本放在 [`property-accessors/`](property-accessors/) 目录，只需要 Python 3.11。`scope.py` 定义第一方、随附、重复和测试代码；`extract.py` 提取候选，收集访问器的函数体和供审阅用的证据；`dump.py` 把它们打印出来供阅读；`count.py` 读入 `classification.json` 中的人工分类，输出上文所有表格。

```
mkdir corpus && cd corpus
git clone https://github.com/godotengine/godot-demo-projects
git -C godot-demo-projects checkout b761b4c
# 对其余仓库重复以上两步，提交见“语料”一节的表格
cd ..
python docs/surveys/property-accessors/extract.py corpus           # 输出各仓库统计，写入 corpus/properties.json 和 corpus/corpus.json
python docs/surveys/property-accessors/dump.py corpus > dump.txt   # 每个第一方候选及其访问器
python docs/surveys/property-accessors/count.py corpus             # 本文所有统计数字
```
