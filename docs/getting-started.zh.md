# 上手指南

English: [getting-started.md](getting-started.md)

本文涵盖 Petrichor 的环境要求、安装、升级、VS Code 集成、`pet` 命令行、从 GDScript 迁移与从源码构建步骤。语言定位、核心特性与版本路线图见 [README.zh.md](../README.zh.md)。

## 环境要求与支持平台

运行 Petrichor 需要官方的 Godot 4.7 编辑器（标准版或 .NET 版均可）。每次发布都会在 CI 中使用官方编辑器和官方导出模板做完整验证，覆盖编辑、运行以及 debug / release 导出。

| 平台 | 库 | 测试环境 |
|---|---|---|
| Windows | x86_64 | Windows Server 2022 |
| Linux | x86_64（需要 glibc 2.35 及以上） | Ubuntu 22.04 |
| macOS | 通用二进制（x86_64 与 arm64，需要 macOS 11 及以上） | macOS 15（arm64；x86_64 经 Rosetta 运行） |

## 安装与运行

从 [Releases](https://github.com/YYF233333/Petrichor/releases) 页面按需下载预编译包（同一页面也提供下文的 VS Code 扩展）：

- **演示包（`petrichor-<版本>-demo.zip`）**：包含 Kenney FPS 起步工程与迁移后的脚本。解压后用 Godot 打开，等资源导入完成就能直接运行。
- **插件包（`petrichor-<版本>-addon.zip`）**：解压到项目根目录生成 `addons/petrichor/`，重启编辑器即可新建 `.pet` 脚本。

### macOS 解除隔离

macOS 上的预编译库仅做了临时签名、未经公证，浏览器下载后会被系统打上隔离标记而拒绝加载。解压后在项目目录下执行以下命令即可解除隔离：

```sh
xattr -dr com.apple.quarantine addons/petrichor
```

## 升级插件

编译产物字节码不跨构建版本兼容，升级插件时请按以下步骤操作：

1. 先关闭 Godot 编辑器。
2. 用新版本的 `addons/petrichor/` 覆盖项目中的旧目录。
3. 重新打开项目并从编辑器运行一次，让全部脚本重新编译。

## 在 VS Code 中编辑

如果想在 VS Code 里写 `.pet` 脚本，从同一个 [Releases](https://github.com/YYF233333/Petrichor/releases) 页面下载 `petrichor-<版本>.vsix`。在 VS Code 中执行 **Extensions: Install from VSIX…** 即可安装。

先用 Godot 编辑器打开项目，再在 VS Code 中打开同一项目目录。VS Code 会自动连上该编辑器，提供诊断以及除断点外的全部语言工具。在 VS Code 中保存脚本，同样会触发运行中游戏的热重载。

使用 VS Code 时有几点需要注意：

- 静态检查要通过引擎读取场景和类，所以未打开 Godot 编辑器时只有语法高亮。当前标签页为 `.pet` 脚本时，底部状态栏会显示连接状态。
- 在 VS Code 中撤销重命名后需要手动保存被改回的文件（VS Code 重命名后会自动保存，但撤销重命名不会）。
- Godot 切到后台后默认每帧休眠 100 毫秒，补全等请求可能产生对应延迟。觉得响应偏慢时，可调低编辑器设置 **Interface → Editor → Timers → Unfocused Low Processor Mode Sleep (µsec)**，代价是后台更耗电。

## 命令行工具 pet

在项目根目录运行 `pet`，可以对整个项目做检查、构建、格式化和迁移，也可以启动 LSP 桥接：

```sh
addons/petrichor/pet check            # 检查磁盘上的文件，不写出字节码
addons/petrichor/pet build            # 检查并写出字节码
addons/petrichor/pet format --check   # 列出未格式化的脚本；不带 --check 则直接就地格式化
addons/petrichor/pet migrate          # 把 GDScript 脚本转成 Petrichor，见下文“从 GDScript 迁移”
addons/petrichor/pet lsp              # 经 stdio 桥接 Neovim、Helix 等 LSP 客户端
```

Windows 下命令路径为 `addons\petrichor\pet`。如果 Godot 编辑器正开着当前项目，`pet` 会直接复用该编辑器进程执行，不另起实例。否则 `pet` 会按上次打开该项目时记录的路径启动无界面 Godot。如果没有记录（例如在 CI 中），用 `--godot <path>` 手动指定即可：

```sh
addons/petrichor/pet build --godot <godot>
addons/petrichor/pet format --check --godot <godot>
```

每条诊断占一行（如 `player.pet:12:5: error: ...`）。退出码 `0` 表示通过，`1` 表示存在错误或未格式化脚本，`2` 表示未能正常执行。由于 `pet check` 直接读取磁盘上的当前文件，AI 编码代理改完代码后运行它，就能立刻拿到最新检查结果。

## 从 GDScript 迁移

`pet migrate` 把项目里的 GDScript 脚本转成 Petrichor。它把 `.gd` 改名为 `.pet`，写下能从代码和场景推断出的类型，并改好指向这些脚本的路径。拿不准的地方它不猜，而是留作编译错误，由你用编辑器的快速修复逐个处理。试用后想退出，`pet migrate --reverse` 可以把代码转回 GDScript。

```sh
addons/petrichor/pet migrate [--reverse] [--dry-run] [--allow-dirty] [<路径>...]
```

迁移分四步：

1. 把项目提交到 git，保存并关闭要迁移的脚本。
2. 预演一遍，读报告。
3. 正式迁移。
4. 处理留下的编译错误。

### 第一步：准备

迁移会给脚本改名，还会改写场景、资源和 `project.godot` 里的路径。所以先把项目提交到 git，结果不满意时可以整体还原。

迁移要改动的文件里，只要有一个带着未提交的修改，或者没有被 git 跟踪，工具就拒绝运行并列出这些文件。确实不想提交，或者项目不在 git 仓库里，可以加 `--allow-dirty` 跳过这项检查。

然后把 Godot 和其他编辑器里没保存的修改存盘，并在 Godot 的脚本编辑器里关掉要迁移的脚本。否则工具同样什么都不写，只列出是哪些文件挡住了它。Godot 本身不用关，迁移后它会重新读取项目。

### 第二步：预演

加上 `--dry-run`，工具只输出报告，不写任何文件：

```sh
addons/petrichor/pet migrate --dry-run
```

下面是 Kenney FPS 起步工程的预演报告，`...` 处有删节：

```text
objects/enemy.gd → objects/enemy.pet
objects/player.gd → objects/player.pet
...
8 × Write the type
2 slot(s) received both int and float, which a use tells apart.
objects/enemy.pet:30:17: hint: "amount" is left without a type, as no type fits all the values it receives: int (res://objects/enemy.pet:68), float (res://objects/player.pet:205).
...
5 × Make it pub
5 × Write .unwrap()
...
objects/enemy.pet:30:17: error: Parameter "amount" needs a type.
...
petrichor migrate: 7 script(s), 0 added, 0 left out, 8 type(s), 3 withdrawn, 16 fix(es), 9 path(s) renamed, 7 error(s), dry run
```

报告里有五种行，最后一行是各项的总数：

| 行 | 含义 |
|---|---|
| `a.gd → a.pet` | 这个文件将被迁移。行尾带 `added:` 的是你没有点名、但必须一起迁移的文件，后面写着原因 |
| `a.gd: left out: …` | 这个文件留在 GDScript，冒号后是原因 |
| `N × …` | 工具自动做的一种修改和它的次数，如写下类型、补上 `pub` |
| `hint:` | 请你过目的地方 |
| `error:` | 迁移后留下的编译错误，格式与 `pet check` 相同 |

`hint:` 值得逐条看，它有四种：

- 没能写出类型的变量或参数，后面列出它收到的每个值的类型和位置；
- 依据不完整的类型，例如只凭按名字的调用推断出来的；
- 仍按名字动态访问的成员。GDScript 原本如此，工具照原样保留，编译器检查不到它们；
- 写成 `x as T` 的转换。`x` 不是 `T` 时结果是 `None`，而 GDScript 的 debug 构建会在这里报错。

以报告中的 `amount` 为例。`damage(amount)` 从敌人那里收到 `int`，从玩家那里收到 `float`。从整数生命值里减去它时，两种类型的结果不同，工具无法替你选。于是它不写类型，留下一条说明原因的 `hint:` 和一条 `error:`，等你来定。`2 slot(s) received both int and float` 一行数的就是这种位置。

### 第三步：迁移

报告没有意外，就去掉 `--dry-run` 再运行一次。所有文件要么全部写入，要么一个都不写。

```sh
addons/petrichor/pet migrate                       # 迁移整个项目
addons/petrichor/pet migrate objects/enemy.gd ui   # 只迁移这些文件和目录
```

路径从项目根目录算起。只迁移一部分时，有些文件必须跟着一起迁移，见下文“只迁移一部分”。

脚本、文本场景（`.tscn`）、文本资源（`.tres`）和 `project.godot` 中写明的脚本路径，工具会一并改好。有三种路径它不改：

- 二进制场景和资源（`.scn`、`.res`）里的路径。它们靠 uid 仍能找到脚本，报告用一条 `warning:` 列出；
- C# 代码里的路径；
- 运行时拼出来的路径，如 `"res://levels/" + name + ".gd"`。

留下编译错误不算失败，报告里写明原因留下的文件也不算：退出码是 `0`。被拒绝或写入失败时什么都不写，退出码是 `1`。所给路径下找不到可迁移的文件时，退出码也是 `1`。预演的退出码与真正迁移时相同，所以预演以 `0` 退出，就说明迁移不会被拒绝。

### 第四步：处理编译错误

凡是需要你判断的地方，工具都留作编译错误。在 Godot 中打开项目，或运行 `pet check`，就能逐个处理。

在 Godot 的脚本编辑器里，点错误行旁的灯泡、按 Alt+Enter，或在右键菜单里选 **Quick Fix...**，都能列出这一处可用的修复。一个脚本里有两处以上错误能用同一种自动修复时，**Fix all '…' in file** 会一次全部应用。VS Code 里，同样的修复出现在代码操作中。

工具围绕 null 写下了三种写法，迁移后值得看一遍：

- **`.unwrap()`**：原代码没有判空就用了这个值。值真是 null 时，程序在 `.unwrap()` 这里报错，位置可能比 GDScript 早。确实可能为 null 的地方，请改成判空。
- **`T?`**：这个值可能为 null，例如没有场景给它赋值的导出、没有初始值的成员，或被赋过 `null` 的变量。
- **`@late T`**：成员在创建之后才赋值，而每处读取都要补 `.unwrap()` 时，工具改用 `@late`，读取处就不必再写 `.unwrap()`。赋值之前读它，仍在读取处报错。

工具不调整格式，收尾后可以运行 `pet format`。

### 留在 GDScript 的文件

即使迁移整个项目，下面几种文件也留在 GDScript，报告里各有一行 `left out:`：

- 带 `@tool` 的脚本和它的整棵继承树。`@tool` 要到 0.4 才支持；
- `addons/` 下的插件，每个插件一行。想迁移某个插件，把它目录里的路径传给命令；
- 脚本模板，也就是编辑器新建脚本时用的文件。它们在项目设置 `editor/script/templates_search_path` 所指的目录里，默认是 `res://script_templates`；
- 同一目录下已有同名 `.pet` 的脚本和它的整棵继承树；
- 被以上文件通过类名直接使用的脚本。

最后一条的原因在 GDScript 一侧。GDScript 可以创建 Petrichor 类的实例并调用实例的方法，但不能通过类名调用它的静态方法或读它的 enum 值。它也不能把 Petrichor 类的 enum、常量或内部类用作类型。所以这样使用某个类的 GDScript 文件留下时，这个类也得留下。

### 只迁移一部分

把文件或目录传给 `pet migrate`，就只迁移它们。必须一起迁移的文件，工具会自动加入，并在报告里用 `added:` 标出：

- 同一棵继承树上的脚本，因为两种语言的类不能互相继承；
- 通过类名直接使用这些类的 GDScript 文件，原因见上一节。

例如只迁移 `boss.gd` 时，它的基类也会迁移，报告里是 `enemy.gd → enemy.pet, added: in the inheritance tree of res://boss.gd`。其余的 GDScript 文件，工具只读不改，唯一的例外是把其中指向已改名脚本的路径改过来。

### 转回 GDScript

`pet migrate --reverse` 把 `.pet` 转回 `.gd`。它的用法与迁移相同：同样先提交到 git，同样可以预演，也可以只给一部分路径。

转回是为迁移之后新写的 Petrichor 代码准备的，比如团队试用一段时间后决定退出。如果只是想撤销刚做的迁移，用 git 还原更直接。

转回时，工具去掉 GDScript 没有的写法，如 `pub`、`async`、`@late`、`.unwrap()` 和对象类型后的 `?`。类型只写到 GDScript 能表达的程度。

有两种文件转不回去，它们连同整棵继承树保持为 `.pet`：

- 有编译错误的文件；
- 用了 GDScript 没有的特性的文件，如 `struct`、带载荷的 enum、接口和 `Result`。

报告给这样的文件一行 `left out:`，并用 `hint:` 列出每一处，例如 `hint: GDScript has no struct.`。留下文件不算失败：即使一个文件都转不回，命令也以 `0` 退出。

只转回一部分时，工具同样会加入必须一起转回的文件：所给文件的继承树，以及这些文件通过类名直接使用的 `.pet` 类的继承树。留下的 `.pet` 文件从此面对的是 GDScript 类，工具会重新检查它们，列出因此新出现的错误。例如转回的类的对象成员，此后读出来是 `T?`。这些错误不妨碍写入，留给你处理。调用转回代码的 GDScript 文件不在检查之列，有问题时由 Godot 在加载时报告。

写入之前，工具把每个转回的文件重新解析一遍，与原来的 `.pet` 比对。两者不一致说明转换器有缺陷。这时它什么都不写，以 `1` 退出，并请你把输出提交到 [Issues](https://github.com/YYF233333/Petrichor/issues)。预演做同样的比对，退出码也相同。

## 从源码构建

从源码构建需要 C++17 工具链和 SCons，克隆仓库后依次执行：

```sh
git submodule update --init
scons
```

完整的本地环境配置、测试运行、导出库构建与开发踩坑，见[开发说明](development.md)。

## 下一步

- 阅读[语言规范](language-spec.zh.md)，了解完整语法、类型规则以及与 GDScript 的差异。
- 遇到缺陷请在 [Issues](https://github.com/YYF233333/Petrichor/issues) 反馈，对语言规则有想法请发到 [Discussions](https://github.com/YYF233333/Petrichor/discussions)，两边都会在 48 小时内回复。
