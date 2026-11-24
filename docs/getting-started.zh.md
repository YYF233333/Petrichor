# 上手指南

English: [getting-started.md](getting-started.md)

本文涵盖 Petrichor 的环境要求、安装、升级、VS Code 集成、`pet` 命令行与从源码构建步骤。语言定位、核心特性与版本路线图见 [README.zh.md](../README.zh.md)。

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
