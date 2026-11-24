# Petrichor

Petrichor 是面向 Godot 4 的静态类型脚本语言，沿用 GDScript 的语法。它以标准 GDExtension 交付，装进官方编辑器即可使用，导出也复用官方模板，无需自编译引擎。除规范明确列出的几处差异外，按 GDScript 习惯写出的代码要么行为一致，要么在编译期报错并给出修改建议。

当前版本：**0.2.1**（0.x 预览阶段，详见[状态与路线图](#状态与路线图)） · English: [README.md](README.md)

![脚本中的节点路径对不上节点时，场景面板会在对应节点上标出错误](docs/images/scene-error.png)

## 为什么选择 Petrichor

Godot 项目变大之后，重构越来越依赖运行游戏才能暴露报错。GDScript 采用渐进类型，即便写满类型注解，场景里的节点路径、信号连接和导出属性依然是动态的。对象引用也默认可为空，运行时出错后调用方往往还会拿到一个兜底值继续执行。C# 虽然带来静态类型，但跨引擎桥接层暗坑不少（[调查](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/pitfalls.md)）。

Petrichor 把静态检查延伸进场景，把可空性与预期失败写进类型签名，由编译器在使用处核对。之所以做成独立语言而非直接改动 GDScript，是因为 GDScript 必须保持渐进类型与动态脚本兼容。默认非空、禁止隐式真值、封闭 `match` 和强制处理失败，都无法直接设为破坏现有项目的默认规则。跨场景静态检查与纯字节码导出，也需要增量场景分析器与原生类型虚拟机，难以用补丁叠在现有运行时上。

以标准 GDExtension 交付无需定制引擎，装入官方编辑器并复用官方导出模板即可运行，也便于在 1.0 前快速验证语义。它可以直接与现有 GDScript 插件和 C# 代码混用。0.3 计划提供双向迁移工具，只用兼容子集的脚本随时可以退回 GDScript，降低试用门槛。

## 核心特性一览

### 场景路径定型与默认非空

下面这段 GDScript 能通过静态检查，但如果场景里的 `Camera` 改了名，或者 `target` 为空，运行时就会出错：

```gdscript
@onready var camera: Camera3D = $Head/Camera
var target: Node3D

func aim():
    camera.look_at(target.global_position)
```

在 Petrichor 里，`camera` 的类型直接从挂载的场景推导。在场景面板重命名节点时路径同步更新，路径断链则直接报编译错误。对象类型默认非空，可空引用写成 `Node3D?`，未经判空收窄无法直接访问：

```gdscript
@onready var camera = $Head/Camera     # 从挂载场景推导为 Camera3D
var target: Node3D?                    # 显式可空；普通 Node3D 默认非空

func aim():
    # camera.look_at(target.global_position)
    # ^ 编译错误：The value may be None (Node3D?); check it before reading "global_position".
    var t := target                    # 成员字段不自动收窄，先绑定到局部变量
    if t != None:                      # 同时检查对象未被释放（已释放对象经 T? 读出为 None）
        camera.look_at(t.global_position)
```

信号连接和导出属性同样按场景做静态检查，报错直接标在对应的场景节点上。

### 值语义 `struct`、载荷 `enum`、穷尽 `match` 与 `Result`

预期内的失败用 `Option` 和 `Result[T, E]` 表达，配合后缀 `?` 提前返回。结合值语义 `struct`、带载荷 `enum` 与穷尽 `match`，引擎边界的数据校验在编译期就能闭合：

```gdscript
struct Hit:
    var damage: int
    var crit: bool = false

enum Action { BLOCK, STRIKE(hit: Hit) }

func apply_action(raw: Variant) -> Result[int, ConversionError]:
    match Action.try_from(raw)?:       # 校验失败时经 ? 提前返回 Err
        Action.BLOCK:
            return Ok(0)
        Action.STRIKE(var hit):        # 穷尽检查：漏写任何分支都会报编译错误
            return Ok(hit.damage * 2 if hit.crit else hit.damage)
```

语言同时提供接口与嵌套带类型容器。

### 有归属的异步 `Task` 与自动取消

`async func` 返回有归属的 `Task`。所属对象被释放时协程自动取消；上层取消时，以 `await f()` 直接启动的子任务一并取消。等待方不会永久挂起：

```gdscript
var current_cast: Task[void]?

async func cast(enemy: Node3D):
    var first := await Task.any([Task.delay(1.5), Task.of(enemy.tree_exiting)])
    if first == 0:
        print("Hit ", enemy.name)

func restart_cast(enemy: Node3D):
    var prev := current_cast
    if prev != None:
        prev.cancel()                  # 取消旧施法及其归属子任务
    current_cast = cast(enemy)         # 当前节点释放时也会自动取消
```

### 工具链、性能与真实项目验证

内置编辑器支持补全、悬停类型、跳转定义、查找引用、重命名、快速修复、格式化、断点和函数体热重载。在场景面板重命名或移动节点时，脚本里的对应路径也会同步更新。除内置编辑器外，还提供 VS Code 扩展、`pet` 命令行工具和 LSP 桥接，并可与项目里的 GDScript 插件和 C# 代码直接混用。完整规则见[语言规范](docs/language-spec.zh.md)。

在微基准测试中，纯计算比 GDScript 快 1.2 到 3.5 倍，引擎属性读写快约 1.6 倍。调用引擎方法、发信号和创建对象主要耗时在引擎内部，两者表现相近，项目启动时间也持平（[基准数据](docs/architecture.md#4-性能)）。

两个真实项目迁移后的脚本已经端到端跑通：Kenney FPS 起步工程与 Ring 打砖块小游戏（[迁移调查](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/paper-migration-2.md)）。

## 试用

运行 Petrichor 需要官方 Godot 4.7 编辑器（标准版或 .NET 版均可），预编译库覆盖 Windows、Linux 和 macOS 桌面三平台。从 [Releases](https://github.com/YYF233333/Petrichor/releases) 页面按需下载：

- **演示包（`petrichor-<版本>-demo.zip`）**：含 Kenney FPS 起步工程与迁移脚本，解压后用 Godot 打开即可运行。
- **插件包（`petrichor-<版本>-addon.zip`）**：解压到项目根目录生成 `addons/petrichor/`，重启编辑器即可新建 `.pet` 脚本。

macOS 预编译库未经公证，解压后需解除系统隔离标记，详见[macOS 解除隔离](docs/getting-started.zh.md#macos-解除隔离)。

完整的安装、升级、支持平台、VS Code 集成、`pet` 命令行与从源码构建步骤，请参阅[**上手指南**](docs/getting-started.zh.md)。

## 状态与路线图

Petrichor 处于 0.x 预览阶段（自首个公开版 0.1 起），目标是在 1.0 之前收集语言语义反馈。

- **适合**：已大量使用带类型 GDScript、能接受 0.x 期间代码随语义调整的项目。
- **暂不适合**：需要导出到 Android、iOS 或 Web 的项目。这些平台的导出库将从 0.4 起陆续提供，优先发布已验证跑通的平台。
- **稳定承诺**：0.9 之前语义仍可能调整，不兼容改动均列在发布说明中。从 0.9 起，跨版本升级不再静默改变现有代码含义，任何不兼容调整都会让旧写法报编译错误。
- **与 GDScript 的刻意差异**：少数写法行为与 GDScript 不同且不报编译错误，[语言规范](docs/language-spec.zh.md#12-与-gdscript-的关系)逐一列出并在 0.9 之前开放讨论。
- **字节码与 lint**：字节码不跨构建版本兼容，升级后需要重编全部脚本；编辑器 lint 计划在 0.3 加入。
- **迁移工具与退路**：手工迁移平均每百行要改二十多处，真实项目建议等 0.3 双向迁移工具。只用兼容特性的脚本可随时转回 GDScript；用到 `struct`、`Result` 等独有特性的文件会跳过并列出具体位置。
- **引擎版本**：支持 Godot 4.7 及之后的版本。

路线图速览：

- **0.2.x（当前 0.2.1）**：核心静态语言、场景检查、内置编辑器与 VS Code/LSP 工具、`pet` 命令行及桌面三平台预编译库。
- **0.3**：GDScript 双向迁移工具、千脚本大项目工具链性能预算，以及编辑器 lint（若超时顺延至 0.4）。
- **0.4**：`@tool`、Android / iOS / Web 导出库进 CI、Godot 4.4–4.6 与双精度探针，以及常驻无界面服务探针。
- **0.9（语言冻结）**：补齐就绪后非空成员、元组与多参信号 `await`、用户定义泛型等 1.0 前设计，开启跨版本无静默破坏承诺。

完整路线图见[实现架构](docs/architecture.md#5-推进顺序)，未完成事项见 [BACKLOG.md](BACKLOG.md)。

## 开发方式

语言设计与规范中的每条规则都由维护者决定，实现、测试、文档和语料调查主要由 AI 编码代理按照 [AGENTS.md](AGENTS.md) 完成。生成的代码不靠人工逐行审阅，正确性由四条约束来保障：

- 语言规范是唯一权威来源，实现一旦与规范不符，改的永远是实现。
- 测试直接从规范和 Godot 引擎行为推导，不照着实现反推。
- 依赖的每一条引擎行为都对照 Godot 源码，并注明提交哈希与行号。
- 两个真实项目迁移后的脚本都在引擎里实际跑通。

出现问题由维护者负责修复。目前仓库只接受 issue、暂不接受 PR，因为当前迭代很快，外部 PR 等到审阅时多半已经过时。遇到缺陷请提 issue，对语言规则有想法请发到 Discussions，两边都会在 48 小时内回复。随着 1.0 临近，仓库会开放 PR。

## 文档

### 用户文档

- [上手指南](docs/getting-started.zh.md)：安装、升级、支持平台、VS Code 集成、`pet` 命令行与从源码构建
- [语言规范](docs/language-spec.zh.md)：完整语言规则（另附[英文版](docs/language-spec.md)）

### 设计与实现

- [实现架构](docs/architecture.md)：编译器、虚拟机实现、性能基准与推进路线
- [设计说明](docs/design-notes.md)：规则背后的取舍、语料计数与被否决的方案
- [引擎事实](docs/engine-facts.md)：所依赖的引擎行为及源码出处
- [开发说明](docs/development.md)：面向源码贡献者的构建、测试与调试说明
- [语料调查](https://github.com/YYF233333/petrichor-corpus)：覆盖语料 v1（20 个 Godot 4 项目）与 v2（200 个项目），附复现脚本与规则依据索引

面向用户的 README 与上手指南维护中英双语。语言规范的英文版由中文版生成，两版有出入时以中文版为准。其余设计与实现文档以中文维护。

## 许可证

[MIT](LICENSE)
