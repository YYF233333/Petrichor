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

在项目根目录运行 `pet`，可以对整个项目做检查、构建、格式化或启动 LSP 桥接：

```sh
addons/petrichor/pet check            # 检查磁盘上的文件，不写出字节码
addons/petrichor/pet build            # 检查并写出字节码
addons/petrichor/pet format --check   # 列出未格式化的脚本；不带 --check 则直接就地格式化
addons/petrichor/pet lsp              # 经 stdio 桥接 Neovim、Helix 等 LSP 客户端
```

Windows 下命令路径为 `addons\petrichor\pet`。如果 Godot 编辑器正开着当前项目，`pet` 会直接复用该编辑器进程执行，不另起实例。否则 `pet` 会按上次打开该项目时记录的路径启动无界面 Godot。如果没有记录（例如在 CI 中），用 `--godot <path>` 手动指定即可：

```sh
addons/petrichor/pet build --godot <godot>
addons/petrichor/pet format --check --godot <godot>
```

每条诊断占一行（如 `player.pet:12:5: error: ...`）。退出码 `0` 表示通过，`1` 表示存在错误或未格式化脚本，`2` 表示未能正常执行。由于 `pet check` 直接读取磁盘上的当前文件，AI 编码代理改完代码后运行它，就能立刻拿到最新检查结果。

## 从 GDScript 迁移

`pet migrate` 把 GDScript 文件转为 Petrichor，`pet migrate --reverse` 把 `.pet` 文件转回 GDScript。命令后可跟从项目根算起的文件或目录，只转换这些；不给路径则转换整个项目。

### 迁移之前

先把项目提交到 git。工具只写 git 能还原的文件，所以要写的文件有未提交或已暂存的改动、未被跟踪或被忽略时，它都拒绝运行；项目不在 git 仓库中时也一样。要写的文件包括改名的脚本及其 `.uid`，以及需要改写路径的场景、资源、脚本和 `project.godot`。报告会逐个列出被拒的文件和原因，加上 `--allow-dirty` 可跳过这项检查。

此外，先在 Godot 和其他编辑器中保存改动，并在 Godot 脚本编辑器中关闭要迁移的脚本。否则工具什么都不写，并列出卡住的文件。

### 预演与迁移

```sh
addons/petrichor/pet migrate --dry-run   # 只输出报告，不写文件，也不检查 git
addons/petrichor/pet migrate             # 执行迁移并输出报告
```

工具把每个 `.gd` 连同其 `.uid` 改名为 `.pet`，并改写所有指向它们的路径。所有文件要么一起写入，要么都不写。迁移时 Godot 可以开着，写入后它会重新读取项目。

下面节选自 Kenney FPS 起步工程的预演报告：

```text
objects/enemy.gd → objects/enemy.pet
objects/player.gd → objects/player.pet
8 × Write the type
2 slot(s) received both int and float, which a use tells apart.
objects/enemy.pet:30:13: hint: "amount" is left without a type, as no type fits all the values it receives: int (res://objects/enemy.pet:68), float (res://objects/player.pet:205).
5 × Make it pub
5 × Write .unwrap()
objects/enemy.pet:30:17: error: Parameter "amount" needs a type.
petrichor migrate: 7 script(s), 0 added, 0 left out, 8 type(s), 3 withdrawn, 16 fix(es), 9 path(s) renamed, 7 error(s), dry run
```

- 带 `→` 的行是文件的去向。因所给路径连带加入的文件，后面注明 `added:` 和原因；留在 GDScript 的文件则改为一行 `left out:`。
- `N ×` 开头的行统计写下的类型数和每种修复的应用次数。
- `hint:` 行值得逐条过目，包括撤回的或依据不全的类型（附流入的值），以及照 GDScript 原样保留的写法（如按名调用）。
- `error:` 行是迁移后留下的编译错误，写法与 `pet check` 相同。末行汇总全部计数。

例中 `damage(amount)` 从敌人处收到 `int`，从玩家处收到 `float`，而从 `int` 类型的生命值中减去它时两者结果不同。报告单列一行统计这类位置，工具不为它们写 `float`，`amount` 的类型就留给你决定。

留下的编译错误不算命令失败：只有因拒绝或失败而什么都没写时，`migrate` 才以 `1` 退出。

### 留在 GDScript 的文件

即使迁移整个项目，以下文件也会留在 GDScript，报告中各有一行 `left out:`：

- 含 `@tool` 的脚本及其继承树，等 0.4 支持 `@tool` 后再迁移；
- `addons/` 下的插件，每个插件列一行。只有所给路径落在插件内时，该插件才迁移；
- 脚本模板，即项目设置 `editor/script/templates_search_path` 所指目录（默认 `res://script_templates`）下供编辑器新建脚本用的文件；
- 被以上文件做类级访问的文件，如被调用静态方法、被当作类型使用其 enum。GDScript 无法这样访问 Petrichor 类。

只迁移部分文件时，工具会连带加入必须一起迁移的文件：整棵继承树（两种语言的类不能互相继承），以及对它做类级访问的 GDScript 文件。例如只迁移 `boss.gd` 时，其基类报告为 `enemy.gd → enemy.pet, added: in the inheritance tree of res://boss.gd`。其余 GDScript 文件只读取，除了改写指向已改名文件的路径外不做改动。

### 人工收尾

凡是需要判断的地方，工具都留作编译错误，在 Godot 中打开项目或运行 `pet check` 即可逐个处理。在 Godot 脚本编辑器中，点击错误行旁的灯泡、按 Alt+Enter，或从右键菜单选 **Quick Fix...**，就能列出可用的修复。同一脚本中有两处以上错误可用同一种自动修复时，**Fix all '…' in file** 会一次全部应用。VS Code 以代码操作提供同样的修复。

工具围绕 null 写下的几种写法值得过目：

- `.unwrap()` 标出原文未判空就使用的值。它与 GDScript 在同一处出错；确实可能为 null 的地方，请改成判空。
- `T?` 标出可能为 null 的值，如没有场景赋值的导出、没有初始值的成员，或被赋予 `null` 的变量。
- 成员在创建后才赋值、且每处读取都已补上 `.unwrap()` 时，工具把 `T?` 改写为 `@late T` 并去掉这些 `.unwrap()`。赋值前读取它照样在同一处出错。

工具不做格式化，收尾后可运行 `pet format`。

### 转回 GDScript

转回针对的是迁移之后用 Petrichor 写的代码，例如团队试用后决定退出。刚迁移完就想撤销的，直接用 git 还原。`--reverse` 同样检查 git，所以先提交迁移结果和之后的改动。

转回时，工具去掉 GDScript 没有的写法，如 `pub`、`async`、`@late`、`.unwrap()` 和对象类型的 `?`，类型则按 GDScript 能表达的程度写出。文件有编译错误，或用到 GDScript 没有的特性（如 `struct`、带载荷的 enum、接口或 `Result`）时，连同整棵继承树保持为 `.pet`。报告会列出每一处，如 `error: GDScript has no struct.`。

只转回部分文件时，工具会连带转换所给文件的继承树，以及它们做类级访问的 `.pet` 继承树。随后它把转回的类当作 GDScript 类，检查留下的 `.pet` 文件，并列出由此新出现的错误。例如转回类的对象成员此后读作 `T?`。这些错误不妨碍写入。调用转回代码的 GDScript 文件不在检查之列，由 Godot 加载时报告。

写入之前，工具会把每个转回的文件重新解析，与原 `.pet` 比对。两者不一致时什么都不写，并请你把输出提交到 [Issues](https://github.com/YYF233333/Petrichor/issues)。

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
