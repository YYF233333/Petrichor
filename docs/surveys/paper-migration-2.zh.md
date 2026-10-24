# 调查：按设计决定纸面迁移两个真实项目

**2026-10-25** · 关联：设计决定（提交 `a099ad4`）、[语言规范](../language-spec.zh.md)、[第一次纸面迁移](paper-migration.zh.md) · English: [paper-migration-2.en.md](paper-migration-2.en.md)

[第一次纸面迁移](paper-migration.zh.md)评估的是规范 v0.1。项目随后重新设计，新的规则先记录为一组设计决定，再据此写成现行的[语言规范](../language-spec.zh.md)（v0.2 草案）。本文按提交 `a099ad4` 时的设计决定重新迁移同一批代码，再推演五个维护场景。目的有三个：测量迁移成本；看各项规则合在一起时哪里有摩擦；找出当时的设计决定还没有写到、迁移时只能假定的地方。这些假定现在都已写入规范，见“迁移时的假定”；迁移暴露的问题和规范的处理见“迁移暴露的问题”。文中的章节号（§）指现行规范。改写没有编译器检查，所以仍称纸面迁移。

## 素材

| 项目 | 提交 | 许可 | 迁移的文件 | 有效代码行 |
|---|---|---|---|---|
| [KenneyNL/Starter-Kit-FPS](https://github.com/KenneyNL/Starter-Kit-FPS) | `185fd23` | MIT | 全部 7 个 `.gd` | 258 |
| [YYF233333/Ring](https://github.com/YYF233333/Ring) | `0ec4161` | 没有许可文件；经仓库所有者同意收录 | breakout 小游戏中的 14 个 `.gd` | 647 |
| **合计** | | | **21** | **905** |

有效代码行指非空、非注释的物理行。Ring 的 14 个文件是第一次迁移的 9 个（622 行），加上 Brick 和 Enemy 仍为 GDScript 的 5 个子类（25 行）。这 5 个文件必须一起迁移：GDScript 解析 `extends 全局类名` 时，会把基类文件当作 GDScript 来解析（`modules/gdscript/gdscript_analyzer.cpp:471-487`），因此 GDScript 脚本不能继承本语言的类。第一次迁移漏掉了这一约束。其余文件不变：Paddle、Ball、Skill、Consumables、ResourceManager、ValueManager 等仍是 GDScript，C# 和 Dialogue Manager 插件不动。

## 方法

1. **改写。** 严格按当时的设计决定改写，保持原有结构和命名。设计决定没有规定、而迁移又必须选一种写法的地方，按下表 S1–S18 假定，并在代码中标出。
2. **账本。** 每一处额外工作记一行：文件、原文行号、类别、来源、次数、性质、说明。
   - 计入成本的类别：**标注**（原文没有、现在要求的类型标注，包括 owner 要求）；**pub**（对外公开的标记）；**转换**（新增的 `as`、`.unwrap()`、`int(x)`）；**改写**（结构改动，例如真值判断、改用接口、声明 async）；**决策**（需要判断的类型选择，例如 `T` 还是 `T?`）；**局部变量**（仅仅因为成员不收窄而新增的局部变量）；**缺口**（按当时的设计无法照原样写）；**胶水**（跨语言的数据适配）。
   - 不计入成本的类别：**未定**（依赖下表的假定）；**收益**（报出或消除了原文的问题）。
   - **性质**分两种：**机械**（M），按规则即可完成，不改变行为，可以由快速修复代劳，例如加 `pub`、补引擎虚函数的参数类型、`if x:` 改为 `if x != None:`；**判断**（J），需要人来决定，例如改用接口、选择 `T?`、收紧导出类型。
   - 计数粒度与第一次相同：每个新增的标注、标记、转换、决策记 1；每处结构改写记 1。
3. **维护场景。** 在迁移后的代码上推演五个常见修改，比较 GDScript 与本语言下要改几处、问题在什么时候暴露。
4. **引擎事实。** 都在 Godot 提交 `469659e` 上核对。

### 迁移时的假定

下表列出迁移时的假定。它们现在都由规范规定；“现行规范”一列给出所在章节，以及规范保留了假定还是作了修改。

| 编号 | 假定 | 现行规范 |
|---|---|---|
| S1 | 省略返回类型表示 void | 保留，限于不是覆盖的函数；覆盖时类型取自被覆盖的声明（§4.6） |
| S2 | int 在赋值、传参和返回时隐式转为 float | 保留（§3.2） |
| S3 | 没有初始化式的值类型成员（int、float、bool、String、向量、容器）取默认值；必须赋值的只有对象引用 | 保留，另规定了 struct 和带载荷的 enum（§4.3） |
| S4 | `x as T`：x 的静态类型已经是 T 时结果为 T，编译器提示多余；否则结果为 `T?`，失败得到 None，与 GDScript 相同。必须成功的转换写作 `(x as T).unwrap()` | 保留（§5.3） |
| S5 | 覆盖引擎虚函数时，参数同样要标注类型 | 改：覆盖时可以省略参数类型和返回类型，取自被覆盖的声明（§4.6）；按此，引擎虚函数覆盖处的 6 处标注不再需要 |
| S6 | 子场景中必须赋值的导出成员，只要实例化它的每个场景都赋了值，就算已赋值 | 保留；没有被引用的场景只给警告（§7.5） |
| S7 | 对有类型的函数值调用 `bind`，结果仍有类型 | 保留（§3.11） |
| S8 | `Array[T]` 的 `pop_front()` 返回 T，数组为空时是程序错误 | 改：返回 `T?`，因为引擎在空数组上返回 null（§3.8）；按此，Kenney FPS 的 audio 多一处转换 |
| S9 | 带类型的循环变量不做隐式向下转换（本次没有出现） | 保留（§5.7） |
| S10 | lerp、clamp、wrap、abs、min、max 等以 Variant 绑定的工具函数，按实参类型定型 | 改：int 与 float 混用是编译错误（§10.3）；按此，Ring 的两处 `get_health_percent` 中 `min(…, 1)` 要改为 `1.0` |
| S11 | GDScript 类中对象类型的成员和返回值为 `T?` | 保留（§11.1） |
| S12 | 审核结果：`get_first_node_in_group` 为 `Node?`，`get_nodes_in_group` 为 `Array[Node]` | 保留（§10.2） |
| S13 | owner 要求写作类级注解 `@requires_owner(T)` | 改：写作 `@owner(T)`（§7.3） |
| S14 | 接口中的属性要求可以由继承来的引擎属性满足，例如 Node2D 的 `global_position` | 保留（§3.9） |
| S15 | GDScript 和 C# 只能访问本语言类的 `pub` 成员；引擎按名字访问的成员除外 | 保留（§10.5、§11.3） |
| S16 | struct 解码时深入容器，例如 `Dictionary[String, ConsumableState]` | 保留（§3.6） |
| S17 | 可以直接 await 一个信号 | 保留（§9.6） |
| S18 | 以字面量名字交给引擎的成员（如 `tween_property(self, "current_speed")`），即使不是 `pub`，引擎也能按名字访问 | 保留（§10.5） |

## 结果

### 总体

| | Kenney FPS | Ring | 合计 | 每百行 |
|---|---:|---:|---:|---:|
| 全部成本 | 48 | 182 | 230 | 25.4 |
| 不计 `pub` | 31 | 97 | 128 | 14.1 |
| 只计判断（J） | 8 | 25 | 33 | 3.6 |

第一次迁移按 v0.1 为每百行 28.1 处。v0.1（git 标签 `spec-v0.1`）没有可见性规则，所以 `pub` 是新增的成本；不计它时，成本约为 v0.1 的一半。230 处中 197 处是机械的，需要人判断的每百行 3.6 处。迁移后的代码有 935 行，比原文多 3.3%，主要来自新增的两个接口文件和消息 struct。

### 成本来源

| 来源 | 处 | 说明 |
|---|---:|---|
| `pub`：被其他文件使用 | 59 | 其中 34 处在 BreakoutManager，它是 autoload，被 UI、Paddle、Ball、Skill 等 GDScript 文件和 Root.cs 访问 |
| `pub`：只为子类 | 29 | 当时子类只能看到、也只能覆盖 `pub` 成员 |
| `pub`：实现接口 | 14 | |
| 函数签名的类型标注 | 24 | 其中 6 处是引擎虚函数的覆盖（S5） |
| 真值判断 | 18 | `if x:` 改为 `!= None`、`!= 0`、`!= ""`、`== 0` |
| Option 类型的成员 | 22 | 决策 6 处、`.unwrap()` 12 处、局部变量 4 处；见下文 |
| 接口取代 `has_method` 和对 owner 的鸭子类型访问 | 13 | 声明接口、`implements`、调用处改用 `is`、owner 要求、去掉运行时检查 |
| 向下转型 | 8 | 按分组判定类型后传参（4 处）、`get_collider()`、`find_children` 等 |
| 协程 | 7 | 含 await 的函数声明为 async（4 处）；不等待的调用写 `.detach()`（3 处） |
| 引擎可空 | 6 | `get_first_node_in_group`、`CollisionShape2D.shape` |
| 其他 | 30 | 字面量成员名迫使参数从 Node 改为 CanvasItem（4）、无类型的动态值（4）、float 伤害转 int（4）、GDScript 对象成员可空（3）、导出类型收紧（3）、空容器与信号参数（4）、消息 struct（2）、子类连带迁移（2）及其他 |
| **合计** | **230** | |

### 只为子类而写的 `pub`

Ring 的 Brick 和 Enemy 各有 2 到 5 个子类，基类有 17 个成员只是因为子类要读取或覆盖才标 `pub`，例如 `type`、`set_type`、`check_init_type`、`update_stat`；子类中覆盖它们的方法也要标 `pub`，共 29 处。[成员可见性调查](member-visibility.zh.md)估计只被子类使用的成员占跨文件使用的 7.2%，本例是 28%。那次调查按名字匹配，`type`、`check_init_type`、`update_stat` 这类在几个脚本中各自声明的名字都归入“无法判定”，所以 7.2% 低估了继承较多的代码中的这项成本。现行规范据此让子类可以读写、调用和覆盖不带 `pub` 的成员（§4.2），这 29 处都不再需要。

### 成员不收窄，以及 Option 类型的成员

因为成员不收窄而新增的局部变量只有 4 处，每千行 4.4 处。

更大的代价来自 Option 类型的成员本身，共 22 处，几乎都在 BreakoutManager。这个 autoload 持有 `breakout`、`skill`、`balls`、`paddle`、`consumables` 5 个场景节点的引用，这些节点要在 autoload 就绪之后才由场景设置。按“没有默认值的非空成员必须在就绪前赋值”（§6.2），它们只能写成 `T?`。原文判空的地方要先读入局部变量（3 处）；原文不判空、默认它们已经设置的地方，要写 `.unwrap()`（12 处），出错时与原文一样是运行时错误。

### 收益

共 7 处：
- Kenney FPS 的 `health_updated` 信号声明时没有参数，发射时带一个参数；
- 切换武器时移出的武器模型从未释放，作为泄漏报出（§6.7）；
- 3 处浮点伤害传给整数生命值，GDScript 静默截断；
- `ValueManager.fire_hurt_interval` 已在 2024-06-27 删除，引用还留在 IntervalHurtComponent 中；
- HealthComponent 原先在运行时检查 owner 是否为 Brick 或 Enemy，否则打印并 `queue_free()`；现在由 owner 要求在场景中检查。

### 维护场景

| 场景 | GDScript | 本语言 |
|---|---|---|
| A. 新增一个可受伤的箱子 | 写一个带 `damage` 方法的脚本，玩家的 `has_method("damage")` 自动识别。方法名拼错时射击静默无效 | 写 `implements Combat.Damageable` 和 `pub func damage(amount: float)`。拼错或签名不符是编译错误。两种写法下，挡在敌人和玩家之间的箱子都会被敌人打中受伤，因为敌人的射线同样检测物体 |
| B. 把 player.tscn 中的 `Head/Camera` 改名为 `Head/MainCamera` | 4 处 `$Head/Camera…` 失效；就绪时 `get_node` 打印错误并返回 null（`scene/main/node.cpp:1987-1994`），第一次使用时出错 | 场景模型立即在 4 处报错，标在脚本和场景中。在场景编辑器中改名时，这 4 处路径随之自动更新（§13.3） |
| C. 把 Enemy 的导出成员 `player` 改名为 `target` | main.tscn 中 4 个实例保存的值在加载时静默丢失（`scene/resources/packed_scene.cpp:567`），下次保存场景时被删掉（`packed_scene.cpp:941-950`）；运行时读取 `player.position` 时出错 | 改名重构同时修改 main.tscn 中的 4 个键，可以预览和撤销（§13.3）；手动改为 0 处 |
| D. 给 `health_updated` 增加参数 `max_health` | 改发射处和 HUD 的处理函数共 2 处。漏改处理函数时，要到玩家受伤、信号发出时才报错（`core/object/object.cpp:1381-1384`） | 同样改 2 处，另加信号声明 1 处。发射处和 main.tscn 中的连接都在编译期检查 |
| E. 给 C# 发来的初始化消息加一个字段 | C# 的类、`AsDict`、`FromDict` 和 GDScript 的读取，共 4 处；C# 漏写时，在 `reset` 读取该键时出错 | 同样 4 处，GDScript 的读取换成 struct 的一个字段；C# 漏写时，在消息入口处报错并写明缺少的字段。字段仍要在两种语言中各写一遍 |

场景 A、B、D 中，本语言把问题提前到编辑时；场景 C 中，本语言连手动修改都省掉了，在场景编辑器中改名时的场景 B 也是如此；场景 E 只是把报错从使用处提前到入口。

## 迁移暴露的问题

以下问题是按当时的设计决定迁移时发现的，每条最后给出现行规范的处理。

1. **`pub` 的数量。** 它占全部成本的 44%，而且全部是机械的。其中只为子类而写的 29 处，远多于成员可见性调查的估计。现行规范让子类可以访问不带 `pub` 的成员，覆盖时可见性不变（§4.2）。
2. **跨语言继承的方向。** GDScript 不能继承本语言的类，所以部分迁移必须整棵继承树一起迁移，而当时的设计决定只写了另一个方向。现行规范写明两个方向都不支持；分析服务在继承本语言类的 GDScript 文件上报错，迁移一个类之前列出它仍为 GDScript 的子类，并提供一起迁移的选项（§4.1、§11.2、§13.4）。
3. **子场景中必须赋值的导出成员。** 按当时的文字，player.tscn 和 enemy.tscn 自己没有为 `crosshair`、`player` 赋值，会被标出；而实际使用它们的 main.tscn 都赋了值（S6）。没有被任何地方引用的模板场景 `enemies/base/enemy.tscn` 也会被标出，并且会挡住发布导出。现行规范按场景的使用方式检查：沿实例和继承链在任何一层赋值都算数；没有被引用的场景只给警告，不阻止导出（§7.5）。
4. **autoload 持有场景节点的引用。** 这类成员只能是 `T?`，带来 22 处成本。GDScript 的写法是先为 null、由场景设置之后再使用。可选的办法有“就绪后才赋值的非空成员”（赋值前读取是程序错误），或者放开成员收窄，但后者只能消除其中 4 处。不过这种写法并不常见：20 个仓库中，在 `_ready` 里把节点登记到 autoload 成员上（`Autoload.x = self`）的只有 6 处，其中 5 处在 Ring（`autoload_self.py`）。现行规范保持 `T?`；需要非空时，把管理器作为场景中的节点，用必须赋值的导出成员引用其他节点。第一版没有“就绪后才赋值的非空成员”（§6.2）。
5. **owner 要求。** 当时写法没有定（S13）。DeathComponent 用 `remove_child` 把自己移出 owner 的子树时，引擎立即把它的 `owner` 置为 null（`scene/main/node.cpp:391-396`，由 `remove_child` 在 `:1799-1801` 调用）；owner 被释放时同样置为 null（`:299-302`）。现行规范写作 `@owner(T)`，在每次读取 `owner` 时检查，不在移动节点时检查；需要判断有没有 owner 时，用类型为 `Node?` 的 `get_owner()`（§7.3）。接口的属性要求可以由继承来的引擎属性满足（S14，§3.9）。
6. **`as` 与必须成功的转换。** GDScript 的 `as` 失败时得到 null，按语法硬规则，`as` 只能得到 `T?`（S4）。迁移后的代码中有 16 处 `(x as T).unwrap()`，而当时的设计决定要求的“失败时报错的显式转换”还没有写法。现行规范规定，必须成功的向下转型就写作 `(x as T).unwrap()`（§5.3）。
7. **签名。** 当时没有写明覆盖引擎虚函数时是否要标注参数（S5，6 处），也没有写明省略返回类型是否表示 void（S1）。现行规范规定：覆盖时可以省略参数类型和返回类型，取自被覆盖的声明；不是覆盖的函数，省略返回类型表示 void（§4.6）。
8. **引擎按名字访问非 `pub` 成员。** 例如 `tween_property(self, "current_speed")`（S18），以及 GDScript、C# 能看到哪些成员（S15）。现行规范规定，经反射只能看到 `pub` 成员和引擎必须按名字找到的成员，后者包括以字面量名字交给引擎的成员（§10.5）。

## 局限

- **没有编译器。** 迁移后的代码没有经过类型检查，可能有漏记。
- **账本由 AI 代理编写和计数。** 机械与判断的划分，以及计数粒度，都由本文决定。
- **只有两个项目、905 行。** Ring 的继承比一般代码多；BreakoutManager 一个文件贡献了 74 处成本。
- **部分迁移的假定。** 仍为 GDScript 的文件决定了哪些成员要标 `pub`。如果全部迁移，这些成员被本语言的其他文件使用，同样要标 `pub`，只有 Root.cs 一处会变成别的形式。
- **维护场景是推演的**，没有在编辑器中实际操作。

## 复现

[`paper-migration-2/`](paper-migration-2/) 目录包含：`ledger.tsv`（账本）；`stats.py`（由账本算出“结果”一节中的数字，原文件只用来计行数）；`autoload_self.py`（在[跨分支重复调查](cross-branch-duplication.zh.md)的 20 个仓库中统计 `Autoload.x = self`）；`migrated/` 中迁移后的代码，每个原文件对应一个 `.pet` 文件，另有两个项目各自新增的 `combat.pet`。Kenney FPS 的许可证随附在 `migrated/fps/LICENSE.md`。只需要 Python 3。

```
git clone https://github.com/KenneyNL/Starter-Kit-FPS fps && git -C fps checkout 185fd23
git clone https://github.com/YYF233333/Ring ring && git -C ring checkout 0ec4161
python docs/surveys/paper-migration-2/stats.py fps ring
python docs/surveys/cross-branch-duplication/gdcorpus.py corpus
python docs/surveys/paper-migration-2/autoload_self.py corpus
```
