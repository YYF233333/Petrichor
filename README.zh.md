# Petrichor

Petrichor 是一门面向 Godot 4 的静态类型脚本语言，沿用 GDScript 的写法。与 GDScript 写法相同的代码，除规范点名的几处差异外，要么行为一致，要么在编译期报错并给出改法。它以 GDExtension 交付，装进官方编辑器即可使用。导出用官方模板，不需要自己编译引擎。

0.2.1 已发布。1.0 之前语义仍可能调整，详见[状态与限制](#状态与限制)。

English: [README.md](README.md)

![脚本中的节点路径对不上节点时，场景面板在用到它的节点上标出错误](docs/images/scene-error.png)

## 为什么

Godot 项目变大以后，重构越来越依赖运行时才暴露的错误。GDScript 的类型是渐进的。即使脚本写满类型，场景中的节点路径、信号连接和导出属性仍是动态的。对象引用默认可为空，运行时出错后调用方仍会拿到假值。C# 有静态类型，但桥接层有不少暗坑（[调查](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/pitfalls.md)）。

Petrichor 把静态检查延伸到场景，并让空值和失败在编译期可见。

## 和 GDScript 的区别

下面的 GDScript 能通过检查，却会在运行时出错：场景里的 `Camera` 改了名，或者 `target` 是 null。

```gdscript
@onready var camera: Camera3D = $Head/Camera
var target: Node3D

func aim():
    camera.look_at(target.global_position)
```

在 Petrichor 中，`camera` 的类型由挂载脚本的场景推出。在场景面板中给节点改名，路径随之更新。对不上节点的路径在编译期报错。`target` 写成 `Node3D?`，读取前要先检查：

```gdscript
@onready var camera = $Head/Camera
var target: Node3D?

func aim():
    camera.look_at(target.global_position)
    # 编译错误：The value may be None (Node3D?); check it before reading "global_position".
```

```gdscript
func aim():
    var t := target
    if t != None:
        camera.look_at(t.global_position)
```

其余的区别：

- 信号连接和导出属性也按场景检查，错误标在场景节点上。
- 对象类型默认非空。已释放的对象在 `T?` 上读作 `None`。
- 预期内的失败用 `Option` 和 `Result[T, E]` 表示，后缀 `?` 提前返回。
- 有值语义的 `struct`、带载荷的 `enum`、穷尽的 `match`、接口和嵌套的带类型容器。
- `async func` 返回有归属的 `Task`。所属对象被释放时，协程随之取消。等待它的一方得到取消或程序错误，不会永远挂起。
- 可以与 GDScript 插件和 C# 代码混用。
- 编辑器提供补全、悬停类型、跳转、查找引用、重命名、快速修复、格式化、断点和函数体热重载。在场景面板中给节点改名或移动节点时，脚本中到它的路径随之更新。

完整规则见[语言规范](docs/language-spec.zh.md)。

在微基准上，纯计算比 GDScript 快 1.2 到 3.5 倍，读写引擎属性快约 1.6 倍。调用引擎方法、发信号和创建对象时，时间主要花在引擎里，两者相近。项目启动时间持平（[基准](docs/architecture.md#4-性能)）。

两个真实项目迁移后的脚本已能运行：Kenney 的 FPS 起步工程和 Ring 中的打砖块小游戏（[迁移调查](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/paper-migration-2.md)）。

## 试用

需要官方的 Godot 4.7 编辑器，标准版和 .NET 版都可以。[Releases](https://github.com/YYF233333/Petrichor/releases) 页面提供两个包：

- 演示包 `petrichor-<版本>-demo.zip`：FPS 起步工程加迁移后的脚本。解压后用编辑器打开，导入结束后即可运行。
- 插件包 `petrichor-<版本>-addon.zip`：解压到项目根目录，得到 `addons/petrichor/`。重启编辑器后就能新建 `.pet` 脚本。

升级时先关闭编辑器，再用新版的 `addons/petrichor/` 替换旧的。之后从编辑器运行一次项目，脚本会全部重新编译。

macOS 上的库只有临时签名，没有公证。浏览器下载的包带有隔离标记，系统会拒绝加载其中的库。解压后在项目目录运行：

```sh
xattr -dr com.apple.quarantine addons/petrichor
```

要在 VS Code 中编辑脚本，再从同一页面下载 `petrichor-<版本>.vsix`，经 Extensions: Install from VSIX… 安装。Godot 编辑器打开项目后，在 VS Code 中打开项目的文件夹。VS Code 连上这个编辑器，得到它的诊断和各项工具，断点除外。在 VS Code 中保存脚本，运行中的游戏随之热重载，与在 Godot 中保存相同。在 VS Code 中撤销重命名后，要保存它改过的文件：VS Code 重命名后自动保存，撤销后却不保存。检查要经引擎读取场景和类，所以 Godot 编辑器没有运行时只有语法高亮。显示脚本时，状态栏标出 VS Code 是否已连上。Godot 在后台时每帧睡 100 毫秒，补全等请求最多因此晚这么久。嫌慢时，调低编辑器设置 Interface → Editor → Timers 中的 Unfocused Low Processor Mode Sleep (µsec)，代价是更耗电。

每个版本都在 CI 中用官方编辑器和官方导出模板测试：编辑、运行、导出 debug 包和 release 包。

| 平台 | 库 | 测试环境 |
|---|---|---|
| Windows | x86_64 | Windows Server 2022 |
| Linux | x86_64，需要 glibc 2.35 或更新 | Ubuntu 22.04 |
| macOS | x86_64 和 arm64 合一，需要 macOS 11 或更新 | macOS 15，arm64；x86_64 经 Rosetta 运行 |

在项目的根目录下，命令行工具 `pet` 检查、构建和格式化整个项目：

```sh
addons/petrichor/pet check            # 检查磁盘上的文件，不写入任何文件
addons/petrichor/pet build            # 检查并写出字节码
addons/petrichor/pet format --check   # 列出未格式化的脚本，不带 --check 时格式化它们
addons/petrichor/pet lsp              # 经 stdio 为 Neovim、Helix 等 LSP 客户端转接
```

在 Windows 上写作 `addons\petrichor\pet`。Godot 编辑器开着这个项目时，命令在其中运行，不另起进程。否则 `pet` 运行无界面的 Godot，即编辑器上次打开项目时记下的那个。没有记下时，用 `--godot <path>` 指定，在 CI 中就是这样：

```sh
addons/petrichor/pet build --godot <godot>
addons/petrichor/pet format --check --godot <godot>
```

每个错误占一行，如 `player.pet:12:5: error: ...`。退出码 0 表示没有错误，1 表示有错误或有未格式化的脚本，2 表示没能运行。代理写完文件后运行 `pet check`，得到的就是磁盘上此刻的结果。

从源码构建需要 C++17 工具链和 SCons。先运行 `git submodule update --init`，再运行 `scons`。

## 状态与限制

0.1 是首次公开发布，目的是在 1.0 之前收集对语义的反馈。

- 适合：已大量使用带类型 GDScript、愿意随 0.x 调整代码的项目。
- 暂不适合：要导出到 Android、iOS 或 Web 的项目。这些平台的库从 0.4 起提供，先提供试通的平台。
- 0.9 之前，语言语义可能调整，不兼容的改动都在发布说明中列出。0.9 起，改变代码含义的改动都以编译错误出现。只改运行时行为的修正，在发布说明中单独列出。
- 少数写法在设计需要之处与 GDScript 行为不同，却不报编译错误。[规范](docs/language-spec.zh.md#12-与-gdscript-的关系)逐一点名，0.9 之前仍征求意见。
- 字节码不跨构建兼容，所以升级后脚本要全部重新编译。
- 编辑器还没有 lint。它排在 0.3。
- 还没有 GDScript 迁移工具。手工迁移每百行要改二十多处，所以真实项目最好等 0.3 的迁移工具再试。工具是双向的。不用 GDScript 所缺特性的文件，在本项目停止维护后也能转回 GDScript。用到这些特性（如 struct、`Result`）的文件不转换，工具只列出位置。
- 支持 Godot 4.7 及之后的版本。

完整路线见[实现架构](docs/architecture.md#5-推进顺序)，未完成的工作见 [BACKLOG.md](BACKLOG.md)。

## 开发方式

Petrichor 的语言设计和每条规范由作者决定。实现、测试、文档和语料调查主要由 AI 编码代理完成。作者不逐行审阅代码。代理遵循仓库中的 [AGENTS.md](AGENTS.md)。正确性靠下面几点保证：

- 规范是唯一的规则来源。实现与规范不符时，改的是实现。
- 测试从规范和引擎行为推出，不从实现推出。
- 引擎行为逐条对照 Godot 源码，并记下提交和行号。
- 两个真实项目迁移后的脚本在引擎中运行。

出了问题由作者负责修复。0.1 期间只接受 issue，不接受 PR。开发进展很快，PR 等到审阅时多半已经过时。缺陷请提 issue，对语言规则的意见请发到 Discussions，都会在 48 小时内回复。接近 1.0 时再开放 PR。

## 文档

- [语言规范](docs/language-spec.zh.md)
- [实现架构](docs/architecture.md)
- [设计说明](docs/design-notes.md)：规则背后的取舍、语料计数和被否决的方案
- [引擎事实](docs/engine-facts.md)：依赖的引擎行为及其源码出处
- [开发说明](docs/development.md)：构建、测试和调试中的坑
- [语料调查](https://github.com/YYF233333/petrichor-corpus)：基于语料 v1 的 20 个和 v2 的 200 个 Godot 4 项目，每份附复现脚本，并有从每条规则到其依据的索引

`docs/` 下的文档为中文。语言规范另有[英文版](docs/language-spec.md)，由中文版生成，两者不一致时以中文版为准。

## 许可证

[MIT](LICENSE)
