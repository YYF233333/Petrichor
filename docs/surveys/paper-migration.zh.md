# 调查：按规范 v0.1 纸面迁移两个真实项目

**2026-10-22** · 关联：规范 v0.1（提交 `7d3e718`，git 标签 `spec-v0.1`）、[第二次纸面迁移](paper-migration-2.zh.md) · English: [paper-migration.en.md](paper-migration.en.md)

本文评估的是早期的规范 v0.1。项目随后重新设计，现行设计是[语言规范](../language-spec.zh.md)（v0.2 草案），按它重做的评估见[第二次纸面迁移](paper-migration-2.zh.md)。除各表的“现行规范”一列外，文中的章节号（§）都指 v0.1。

此前的调查各回答规范中的一个具体选择。本文测量整份规范落到真实代码上的代价：把两个项目的 GDScript 逐行改写成符合 v0.1 的代码，记下每一处额外工作；再在 14 个仓库的语料上统计其中几类写法的频率，检验这两个项目是否有代表性。改写没有编译器检查，所以称为纸面迁移。文中的修正约定、候选和假定是迁移时采用或计数的，当时不是 v0.1 的规则；它们在现行规范中的去向见各表的“现行规范”一列。

## 问题

规范自洽、实现符合规范，与真实项目用起来顺手，是不同的验收。当时规范的证据几乎都属于前一种。我们想知道：

- 按 v0.1 改写真实代码，每百行需要多少额外的标注、改名、转换、改写和决策？
- 这些成本出自哪些规则，有多少出自规范尚未规定的地方？
- 如果放宽某些规则或补上沉默处，成本下降多少？
- v0.1 在哪里帮了忙？

## 素材

| 项目 | 提交 | 许可 | 迁移的文件 | 有效代码行 |
|---|---|---|---|---|
| [KenneyNL/Starter-Kit-FPS](https://github.com/KenneyNL/Starter-Kit-FPS) | `185fd23` | MIT | 全部 7 个 `.gd` | 258 |
| [YYF233333/Ring](https://github.com/YYF233333/Ring) | `0ec4161` | 没有许可文件；经仓库所有者同意收录 | breakout 小游戏中的 9 个 `.gd` | 622 |
| **合计** | | | **16** | **880** |

有效代码行指非空、非注释的物理行。

**Kenney FPS** 是一个第一人称射击入门模板，全部迁移，包括 autoload `Audio` 和资源类 `Weapon`。场景和资源文件（`.tscn`、`.tres`）用来核对节点类型、信号连接和导出值。

**Ring** 是一个已停止维护的视觉小说引擎。主体用 C#（.NET）编写，附带一个用 GDScript 写的打砖块小游戏 breakout，并使用 Dialogue Manager 插件（GDScript）。C# 运行时与 breakout 之间用 Dictionary 传递消息。迁移的 9 个文件是：breakout_manager（autoload，消息入口兼信号中心）；health、death、interval_hurt 三个组件；Brick 及其 sturdy、indestructible 两个子类；Enemy 基类；Utility（autoload）。按部分迁移处理：这些文件改为本语言，Paddle、Skill、Consumables、Ball、ResourceManager、ValueManager 等仍是 GDScript，C# 和插件不动。这样能测到与 GDScript、C# 和插件共存时的代价。

**频率语料**与[属性访问器调查](property-accessors.zh.md#语料)相同：14 个仓库，提交相同，只计第一方代码（`property-accessors/scope.py`）。频率的单位是逻辑行，即去掉注释和空行、合并续行之后的行，共 99,410 行，因此与其他调查的物理行数不同。另把 Ring 的整个 breakout（1,903 行）单列一行作对照。

## 方法

1. **改写。** 严格按 v0.1 改写，保持原有结构和命名。规范没有规定、或照规定写很别扭的地方，采用一条有编号的修正约定（A1–A8），在代码中以 `# [An]` 标出，账本同时记下 v0.1 下必须怎么写。另有 5 条候选只计数、不采用（C-*）。规范沉默而又必须选一种写法的地方，按 S1–S8 假定，每项只记一次。
2. **账本。** 每一处额外工作记一行：文件、原文行号、类别、来源、次数、可消除它的约定、说明。
   - 计入成本的类别：**标注**（原文没有、v0.1 要求的类型标注）；**改名**（`int`／`float` 改为 i32、i64、f64）；**转换**（新增的 `as`、`!`、`T(x)`）；**改写**（结构改动，例如真值判断、`is` 不收窄、换用带类型的工具函数）；**决策**（文字不变但需要查证的选择，例如对象成员取 `T` 还是 `T?`）；**缺口**（v0.1 无法表达，只能逐类分派、重复代码或写不出来）；**胶水**（跨语言的数据适配）。
   - 不计入成本的类别：**未定**（依赖规范没有规定的规则）；**静默截断**（默认不警告的有损转换）；**收益**（v0.1 报出或消除了原文的问题）。
   - 计数粒度：每个新增的标注、改名、转换、决策记 1；每处原文结构的改写记 1；缺口和胶水每条记 1，规模写在说明里。所以“处”是工作项，不是字符数或行数。
3. **引擎事实。** 引用的引擎行为都在 Godot 提交 `469659e` 上核对，见下文“引擎依据”。
4. **频率。** `idioms.py` 用正则在清理后的代码上统计 11 类写法，每类都抽样阅读了匹配结果。`$X.成员` 也计入 Node 本身就有的成员（如 `queue_free()`），是上限；`for in 整数` 只识别整数字面量、`.size()` 和 `len()`，是下限。

### 修正约定（迁移中采用）

| 编号 | 内容 | 与 v0.1 的关系 | 现行规范 |
|---|---|---|---|
| A1 | 引擎以 `RequiredResult<T>`／`RequiredParam<T>` 标注的返回值和参数视为非空 | §6.5 允许“有了证据再补全绑定描述”，引擎已经提供了证据 | 采用（§10.3） |
| A2 | 只在出错时返回 null 的引擎接口（如 `Node.get_tree()`）在绑定描述中标为必须成功：返回非空类型，返回 null 时报程序错误，与 `$X` 相同 | 修改：v0.1 下为 `T?` | 采用（§10.2） |
| A3 | 以 Variant 绑定的数学工具函数（abs、sign、clamp、lerp、min、max、wrap 等）按实参的公共类型定型，返回同一类型 | 填补沉默处：v0.1 下返回 `Variant?`，须改用 absf、clampi 等定型版本或写 `T(f(…)!)` | 改：实参类型相同时返回该类型，int 与 float 混用是编译错误（§10.3） |
| A4 | core 值类型的运算符遵循引擎的 Variant 运算符表；标量操作数在检查边界转换为分量类型，损失警告按 §7.3 | 填补待定项（引擎运算符的混合位宽） | 采用（§5.1） |
| A5 | 允许 `for i in n`（n 为整数），循环变量取 n 的类型 | 放宽 §2.4 | 采用（§5.7） |
| A6 | GDScript 类和 autoload 有静态视图：按其声明类型可见；未标注的参数、返回值和成员为 `Variant?`；对象类型的成员和返回值为 `T?`；GDScript 的 int 为 i64；autoload 名是全局标识符，类型为其脚本类 | 填补 §13 的沉默处 | 采用（§11.1），有修改：未标注的为 Variant，不是 `Variant?`；GDScript 的 int 为 int |
| A7 | 跨语言协程：本语言调用 GDScript 函数得到 `GDScriptFunctionState` 时，可以 await 其 `completed`；引擎或 GDScript 调用本语言协程时，第一次挂起处返回一个 Signal 而不是 NIL，GDScript 的 await 会等待它 | 解除 §9 的已知限制；待实测 | 采用，仍待实测（§9.8） |
| A8 | 记录类型：按字段名编码为 Dictionary，按目标类型解码，并逐字段执行入口检查 | 新能力；与 v0.1 设计说明中的 `struct` 提案（按位置编码为带标记的 Array）冲突 | 采用：struct 按字段名编码为 Dictionary（§3.6、§11.3） |

### 只计数、迁移中不采用的候选

| 编号 | 内容 | 现行规范 |
|---|---|---|
| C-int | `int`、`float` 可以作类型名（例如分别对应 i64、f64） | 采用：只有 int 和 float，都是 64 位（§3.2） |
| C-scene | 按脚本挂载的场景给 `$X` 定型 | 采用（§7.1） |
| C-is | `is` 对局部值和参数收窄类型 | 采用（§5.4） |
| C-r3 | R3 把“参数与柔性字面量做算术、结果流向已知类型”算作推断依据 | 不再适用：函数签名不推断（§4.6） |
| C-trait | 运行时 trait 对象，可以要求属性 | 改为名义接口，可以要求属性（§3.9） |

### 沉默处的假定

| 编号 | 假定 | 现行规范 |
|---|---|---|
| S1 | 脚本以 `class_name` 命名后可以作类型；没有 class_name 的脚本不能按类型引用 | 没有 class_name 的文件经 `preload` 引用（§2.1） |
| S2 | 布尔取反写作 `not`；前缀 `!` 未规定 | 前缀 `!` 也是逻辑非（§2.2） |
| S3 | 存在条件表达式 `a if c else b` | 保留（§5.5） |
| S4 | 存在 `preload`、`load`、`str`、`print_debug`、`is_instance_valid` 等全局函数；`preload` 按扩展名定型，`load` 返回 `Resource?` | 改：`preload` 和字面量路径的 `load` 按资源的实际内容定型（§7.4） |
| S5 | 存在 Dictionary 字面量 `{k: v}` | 保留（§2.2） |
| S6 | `Array[T].pop_front()` 返回 `T?`；无类型 Dictionary 的 `keys()` 返回无类型 Array | 保留（§3.8） |
| S7 | 场景文件中保存的信号连接不做静态检查，调用时按 §12.6 执行入口检查 | 改：由场景模型检查（§7.7） |
| S8 | 句柄类型的存活检查使用 `is_instance_valid` | 改：经 `T?` 读到已释放的对象时为 None，所以 `!= None` 同时是存活检查（§3.3） |

### 引擎依据

以下事实在 Godot 提交 `469659e` 上核对，路径相对于 Godot 仓库根目录：

- `RequiredResult<T>` 表示只在出错时返回 null，`RequiredParam<T>` 表示传入 null 是错误（`core/object/object.h:950-954, 1062-1067`）。它们对应元数据 `METADATA_OBJECT_IS_REQUIRED`（`core/variant/type_info.h:54`），API 导出中记作 `required`（`core/extension/extension_api_dump.cpp:92`）。
- 带这类标注的例子：`Node.add_child`、`add_sibling`、`remove_child` 的参数（`scene/main/node.h:526-528`）；`Node.create_tween`（`node.h:622`）；`SceneTree.get_root`、`create_timer`、`create_tween`（`scene/main/scene_tree.h:308, 438-439`）；Tween 的链式方法（`scene/animation/tween.h:148, 176, 181`）。
- 没有这类标注的例子：`Node.get_tree()` 不在场景树中时报错并返回 null（`node.h:559-562`）；`Node.get_parent`（`node.h:549`）；`SceneTree.get_first_node_in_group`（`scene_tree.h:420`）；`RayCast3D.get_collider`（`scene/3d/physics/ray_cast_3d.h:125`）；`PackedScene.instantiate`（`scene/resources/packed_scene.h:281`）。
- floor、ceil、round、abs、sign、snapped、lerp、wrap、max、min、clamp 以 Variant 绑定（`core/variant/variant_utility.cpp:1656-1728`），另有 absf、lerpf、wrapi、clampf 等定型版本（`core/variant/variant_utility.h:64, 83, 104, 114`）。GDScript 分析器按绑定信息把它们的返回类型定为 Variant（`modules/gdscript/gdscript_analyzer.cpp:3546-3595`）。
- GDScript 按 autoload 的脚本给 autoload 名定型（`gdscript_analyzer.cpp:4637-4653`）。
- GDScript 函数挂起时，把一个 `GDScriptFunctionState` 返回给调用方，调用方是哪种语言都一样（`modules/gdscript/gdscript_vm.cpp:2603-2640`）。该对象有 `completed` 信号（`modules/gdscript/gdscript_function.cpp:327`）。GDScript 的 await 遇到 `GDScriptFunctionState` 时等待其 `completed`，遇到任意 Signal 值时等待该信号，遇到其他值立即继续（`gdscript_vm.cpp:2585-2599`）。
- GDScript 的条件跳转使用 `Variant::booleanize`（`gdscript_vm.cpp:2753, 2770`）；对象已被释放时该值为假（`core/variant/variant_op.cpp:1122-1123`；`core/variant/variant.cpp:980-981`）。所以 `if node:` 同时排除 null 和已释放的节点。

## 结果

### 总体

| 场景 | Kenney FPS | Ring | 合计 | 每百行 |
|---|---:|---:|---:|---:|
| v0.1 | 64 | 183 | 247 | 28.1 |
| v0.1 + A1 | 62 | 171 | 233 | 26.5 |
| + A1–A8 | 49 | 128 | 177 | 20.1 |
| + A1–A8 + C-int | 35 | 82 | 117 | 13.3 |
| + A1–A8 + C-int + C-scene | 25 | 74 | 99 | 11.2 |
| + 全部 A 和 C | 19 | 69 | 88 | 10.0 |

A1 只是使用引擎已经提供的标注，按 §6.5 也可以算在 v0.1 之内，所以单列一行。

### 成本来源（v0.1）

| 来源 | 处 | 可消除它的约定 |
|---|---:|---|
| `int`／`float` 不是类型 | 60 | C-int |
| 引擎可空性（`get_tree()`、`RequiredResult`、`RequiredParam` 等） | 35 | A2 18、A1 14，其余 3 处保留 |
| Variant 数学工具函数 | 23 | A3 |
| 下转型（`instantiate()`、`get_collider()`、按分组判定类型后传参等） | 19 | — |
| 对象成员和参数的非空选择，及由此需要的 `!` | 18 | — |
| `$X` 没有期望类型时为 Node | 18 | C-scene |
| `$X as T` 得到 Option，改为带标注的 `@onready` | 14 | — |
| 真值判断 | 13 | — |
| 消息字段的 `(d[k] as T)!` | 11 | A8 |
| 与仍是 GDScript 的代码交互（未标注成员、可为 null 的返回；8 处）和消息胶水（1 处） | 9 | A8 消除胶水 1 处 |
| 推断要求的标注（信号参数、空容器成员、只用于方法调用或 Variant 的参数） | 9 | C-r3 消除其中 1 处 |
| 共同能力、重复代码、跨语言协程 | 6 | C-trait 5、A7 1 |
| 其他（`is` 不收窄、数值转换、整数循环、类型名、容器、存活检查） | 12 | C-is 3、A5 2、C-trait 2，其余保留 |
| **合计** | **247** | |

### 不计入成本的项

- **未定：53 处。** 14 处向量与标量、Basis 与向量的混合运算（A4）；39 处 autoload 访问（A6）。
- **静默截断：4 处。** 都是 GDScript 的 int（i64）赋给 i32 的成员或参数，默认不警告。
- **收益：6 处。**
  - Kenney FPS 的 `health_updated` 信号声明时没有参数，发射时却带一个参数；v0.1 在编译期报出。
  - 3 处浮点伤害传给整数生命值：原文静默截断，v0.1 给出警告，要求显式转换。
  - `ValueManager.fire_hurt_interval` 已在 2024-06-27 的提交中删除，引用却留在组件里；在 A6 下编译期报出。
  - 一处返回类型推断为 Tween，原文的 `as Tween` 可以删掉。

这 880 行里没有出现带载荷的 enum、嵌套 Option、u64、显式回绕、静态初始化或 `weak`。规范中篇幅最大的 enum 编码和数值转换细则，在这里只通过 `int`／`float` 改名和 4 处静默截断与代码发生接触；Variant 映射只出现在消息字段、工具函数的返回值和未标注的 GDScript 成员上。

### 语料中的频率

每千行逻辑行中的出现次数：

| | 行数 | int/float 类型位置 | `$X.成员` | `get_tree()` | `is` 后用成员 | `$X as T` | for in 整数 | Variant 数学工具 | `instantiate()` | create_tween/timer | 未标注的 `@onready $X` | `has_method` |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 语料合计 | 99,410 | 40.6 | 16.3 | 4.8 | 4.8 | 4.0 | 2.5 | 2.2 | 1.7 | 1.3 | 1.1 | 0.7 |
| 其中游戏与示例 | 23,095 | 45.6 | 51.1 | 9.2 | 4.8 | 5.3 | 1.0 | 6.9 | 3.2 | 2.9 | 2.0 | 1.0 |
| 其中插件与工具 | 76,315 | 39.1 | 5.8 | 3.5 | 4.9 | 3.6 | 3.0 | 0.8 | 1.3 | 0.8 | 0.9 | 0.6 |
| 10 个 ≥1,000 行仓库的中位数 | — | 46.7 | 2.2 | 3.8 | 3.2 | 0.0 | 0.4 | 1.6 | 1.9 | 1.4 | 0.1 | 0.5 |
| Ring breakout（全部） | 1,903 | 87.2 | 13.7 | 17.3 | 1.1 | 17.9 | 0.0 | 17.9 | 8.9 | 7.9 | 1.1 | 0.0 |

“游戏与示例”指 godot-demo-projects、godot-open-rpg、tps-demo 和四个 Kenney 入门模板，其中 godot-demo-projects 占 18,728 行。Kenney FPS 本身在语料中，其 258 行里有 14 处 int/float、7 处 Variant 数学工具、4 处 `get_tree()` 和 9 处未标注的 `@onready $X`。

- **`int`／`float` 在类型位置出现得最多，而且不只这两个项目如此。** 语料中每千行 40.6 处，仓库中位数 46.7，大约等于表中其他各项之和。Ring 是语料的两倍。
- **`$X.成员` 在游戏代码中远比在插件中常见**：每千行 51.1 处对 5.8 处。迁移样本中这一项不多：Kenney FPS 把节点放进 `@onready` 成员，Ring 多用 `$X as T`。
- **Ring 放大了几项。** Variant 数学工具、`get_tree()` 和 `$X as T` 的密度分别是语料的 8 倍、3.6 倍和 4.5 倍。在一般代码中，Variant 数学工具约每千行 2 处，游戏代码约 7 处；`get_tree()` 约 5 处，游戏代码约 9 处。
- **`is` 之后接着使用成员**在语料的各类代码中都是每千行 3 到 5 处。
- **`$X as T` 是个别项目的风格**：中位数为 0，集中在 Pixelorama、godot-demo-projects、godot-open-rpg 和 Ring。

## 典型例子

**1. C# 与 GDScript 之间的消息。**
Ring：`breakout/Root.cs:28-50, 85`；`breakout/scenes/managers/breakout_manager.gd:121-133`

```
public Godot.Collections.Dictionary AsDict()
{
	var dict = new Godot.Collections.Dictionary()
	{
		{ "selected_skill", selected_skill },
		...
	};
	...
}
...
GetNode("/root/BreakoutManager").Call("receive_init_message", data.AsDict());
```

```
func reset():
	if init_message:
		var player_consumables: Dictionary = init_message["player_consumables"]
		...
		for consumable_name in player_consumables.keys():
			var rest_times: int = player_consumables[consumable_name]["rest_times"]
			var transformed: bool = player_consumables[consumable_name]["transformed"]
```

字段名和类型在三处手写：C# 的 `BreakoutMessage` 类、C# 的 `AsDict`／`FromDict`、GDScript 的 `reset`／`send_message`。breakout 的笔记里还记着一次因 JSON 解析而读不出 `player_consumables` 的错误。GDScript 靠带类型的局部变量隐式转换；v0.1 不做隐式转换，每个字段要写成 `(entry["rest_times"] as i64)!`，共 11 处。在 A8 下，GDScript 一侧变成一个记录声明和 `func receive_init_message(message: InitMessage)`，入口检查按字段解码，C# 一侧不变（`paper-migration/migrated/ring/breakout_manager_a8.pet`）。

**2. 组件按鸭子类型访问 owner，以及重复代码。**
Ring：`breakout/scenes/components/health_component.gd:13-28`

```
func _ready():
	if (owner is Brick) || (owner is Enemy):
		pass
	...
	current_health = owner.init_health

func take_damage(value: int):
	var prev_health = current_health
	current_health = min(max(current_health - value, 0), owner.max_health)
```

Brick 继承 StaticBody2D，Enemy 继承 CharacterBody2D，所以不能共享基类。HealthComponent 和 DeathComponent 直接读写 owner 的 6 个字段（`init_health`、`max_health`、`unique_drop_name`、`lucky_drop`、`drop_percent_scale`、`point`）。v0.1 没有 trait 对象，每个字段都要按具体类分派，两个组件共新增约 20 行。另外，Brick 的 77 行有效代码中有 64 行原样出现在 Enemy 中：导出、组件接线、`set_type`、`show_health`、`hit_by_ball`、`physic_hurt` 等。运行时 trait 对象（C-trait）能去掉分派，去不掉重复：要消除重复，需要共享实现，而 v0.1 §4 明确排除了 mixin 和字段注入。

**3. `has_method` 实际只有一个目标。**
Kenney FPS：`objects/player.gd:200-205`；`objects/enemy.gd:52-54, 68`

```
var collider = raycast.get_collider()
if collider.has_method("damage"):
	collider.damage(weapon.damage)
```

玩家和敌人都有 `damage` 方法，分别继承 CharacterBody3D 和 Node3D。但玩家的射线（`player.tscn` 中 `collide_with_areas = true`）只会打到敌人的 Area3D 根节点；敌人的射线不检测 Area，只会打到玩家或静态墙体。所以两处改成 `(collider as Enemy)?.damage(…)` 和 `if var Some(target) = … as Player` 后行为不变，代价要到以后新增可受伤的对象时才出现。

**4. 引擎可空性。**
Kenney FPS：`objects/player.gd:214, 242`

```
get_tree().root.add_child(impact_instance)
tween = get_tree().create_tween()
```

`root` 和 `create_tween` 在引擎中标注为 `RequiredResult`，`get_tree()` 没有标注，只在节点不在树中时报错并返回 null。v0.1 把没有证据的对象返回值都视为可空，因此这两行要写成 `get_tree()!.root!` 和 `get_tree()!.create_tween()!`。在 A1 和 A2 下，两行与原文相同。

**5. Variant 数学工具函数。**
Kenney FPS：`objects/player.gd:72, 84`；Ring：`breakout/scenes/managers/breakout_manager.gd:45`

```
container.position = lerp(container.position, container_offset - ..., delta * 10)
camera.position.y = lerp(camera.position.y, 0.0, delta * 5)
ammo = max(value, 0)
```

这些函数在引擎中以 Variant 绑定，按 v0.1 §12.6 返回 `Variant?`。v0.1 下要分别改为 `container.position.lerp(…)`、`lerpf(…)` 和 `maxi(…)`，迁移者需要知道每个函数的定型版本叫什么。在 A3 下，三行与原文相同。

**6. 跨语言协程。**
Ring：`breakout/scenes/utility/utility.gd:54-58`

```
func dialogue(str: String):
	var res = DialogueManager.create_resource_from_text(str)
	await DialogueManager.get_next_dialogue_line(res)
	DialogueManager.show_dialogue_balloon(res)
```

`get_next_dialogue_line` 是插件中的 GDScript 协程。v0.1 §9 不允许本语言 await 其他语言的协程，所以这个函数在 v0.1 下写不出来。上文“引擎依据”表明，GDScript 函数挂起时会把 `GDScriptFunctionState` 交给调用方，而 GDScript 的 await 接受任意 Signal 值；A7 据此让两个方向都可以 await。

## 结论

- **v0.1 的迁移成本约为每百行 28 处。** 采用 A1–A8 后为 20 处，再允许 `int`／`float` 作类型名后为 13 处。
- **最大的一项是 `int`／`float` 不是类型。** 它占 v0.1 成本的四分之一，语料中每千行 40.6 处，说明这不是两个项目的特例。每处改名还隐含一次宽度选择；GDScript 的 int 是 i64，回到 i32 时有 4 处静默截断。880 行中没有一处从窄整数获益。
- **第二组成本来自规范没写或只写了一行的地方。** 引擎可空性和 Variant 数学工具产生 58 处 v0.1 成本，其中 14 处只需使用引擎已经提供的标注（A1）；混合运算符和 autoload 访问另有 53 处依赖尚未规定的规则。
- **规范篇幅最大的部分在这批代码中几乎没被触及。** 带载荷的 enum、嵌套 Option、u64、显式回绕、静态初始化和 `weak` 都没有出现。
- **游戏代码大量直接访问 `$X` 的成员**（每千行最多 51 处）。其中不属于 Node 的成员在 v0.1 下需要标注或转换，而迁移样本低估了这一项。
- **不同类上的共同能力有实证。** Ring 的 Brick 与 Enemy 有 64 行重复，组件按鸭子类型访问 6 个字段。Kenney FPS 的 `has_method` 不构成这方面的证据。trait 对象只能解决访问，解决不了重复。
- **跨语言方面：** 部分迁移离不开 A6；A7 看来可以用引擎现有机制实现；A8 去掉本语言一侧的消息胶水，但 C# 一侧不受影响。
- **收益存在，但不多。** 880 行中有 6 处，包括一个信号参数错误、3 处静默截断和一个悬空引用。

## 局限

- **没有编译器。** 迁移后的代码没有经过类型检查，违反 v0.1 的地方可能有漏记，所以 v0.1 的成本更可能被低估。
- **账本由 AI 代理编写和计数，计数粒度也由本文决定。** “决策”是否算成本，可以有不同看法。
- **只有两个项目、880 行。** Ring 的写法在几项上与语料差别很大。部分迁移的假定使跨语言成本偏高；如果全部迁移，这部分会变成本语言内部的调用。
- **只测了迁移成本。** 新写代码、长期维护和性能都没有测；改名这类成本主要在迁移时出现，下转型、Option 和分派在新代码中同样存在。
- **频率是正则统计。** `$X.成员` 是上限，`for in 整数` 是下限。
- **场景数据只抽查了一部分。** 只核对了迁移涉及的节点类型、信号连接和导出值。

## 复现

[`paper-migration/`](paper-migration/) 目录包含：`ledger.tsv`（账本）；`stats.py`（由账本算出“结果”一节中的成本数字和不计入成本的项）；`idioms.py`（语料频率，复用 `property-accessors/` 中的 `extract.py` 和 `scope.py`）；以及 `migrated/` 中迁移后的代码，每个原文件对应一个 `.pet` 文件，Ring 的消息部分另有 A8 写法 `breakout_manager_a8.pet`。Kenney FPS 的许可证随附在 `migrated/fps/LICENSE.md`。Ring 的迁移代码经原作者同意，以本仓库的 MIT 许可证发布。只需要 Python 3.11。

```
git clone https://github.com/KenneyNL/Starter-Kit-FPS fps && git -C fps checkout 185fd23
git clone https://github.com/YYF233333/Ring ring && git -C ring checkout 0ec4161
python docs/surveys/paper-migration/stats.py fps ring
# 语料：按属性访问器调查“复现”一节克隆 14 个仓库到 corpus/
python docs/surveys/paper-migration/idioms.py corpus Ring-breakout=ring/breakout/scenes
```
