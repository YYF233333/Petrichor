# 开发说明

本文汇总本地环境配置、常用构建与测试命令、开发踩坑，以及设计提案与文档写作约定，面向直接动源码的贡献者。

## 环境与常用命令

新克隆仓库后执行 `git submodule update --init`、`pip install pre-commit` 和 `git config core.hooksPath tools/hooks`。本机路径和笔记写入已忽略的 `AGENTS.local.md`，已跟踪文件不引用它。

Windows 下使用 SCons 与 MSVC 构建，产物输出在 `tests/engine/project/bin/`。扩展不支持热重载，重建前先关闭编辑器。

- `scons`（`target=editor`）：构建所有测试和基准共用的编辑器扩展库，同时构建命令行工具 `pet`（单独构建用 `scons pet`）。
- `scons target=template_debug` 和 `scons target=template_release`：构建不含编译器的导出库。只有 `export` 测试组使用它们并在过期时提示；迭代期间不必构建，跑全套测试前再编。
- `scons compiler_tests`，随后运行 `tests/compiler/bin/compiler_tests`：不启动引擎运行编译器测试。
- `scons compiler_tests use_llvm=yes debug_symbols=yes sanitize=yes`：在 Linux 上用 clang 16 及以上版本构建带 AddressSanitizer 和 UndefinedBehaviorSanitizer 的编译器测试，再以 `ASAN_OPTIONS=detect_leaks=0 tests/compiler/bin/compiler_tests` 运行。CI 的 `linux sanitized compiler tests` 作业以此检查内存错误和未定义行为；带载荷 enum 的循环引用修好之前，泄漏检查要这样关掉（见 `BACKLOG.md`）。
- `python tests/engine/run.py <Godot 4.7 编辑器> [group...]`：运行引擎测试，分组名见 `run.py`。`fps`、`ring` 和 `deck` 组的 “forward dry run” 一行打印正向迁移的计数和预算；CI 只在 Linux 的 4.7.2 上做这一步，其余作业设 `PETRICHOR_MIGRATION_BUDGET=0` 跳过。遗留错误变少，或转换的脚本数、留下的文件数有意改变时，把 `run.py` 的 `MIGRATION_BUDGET` 改成输出的数。
- `python benchmarks/run.py <Godot 4.7 编辑器>`：在官方优化构建（如 `4.7.2-stable`）上运行性能基准，不要使用 `dev_build`（实现架构 §4）。
- `python benchmarks/large.py <Godot 4.7 编辑器>`：生成千脚本项目并无界面构建，计数超出脚本里的预算时失败（实现架构 §4）。计数低于预算时，把预算调低到输出的数。

## 构建

构建产物中的目标文件和静态库统一输出在 `build/` 目录（由 `SConstruct` 的 `VariantDir` 控制）。日常增量构建注意避开几类会污染缓存或锁文件的操作：

- 如果源码目录旁遗留了旧提交生成的 `.obj` 文件，必须先删掉，否则 SCons 会误判为已构建并跳过编译。
- 不要在测试仍占用 DLL 时触发构建，也不要在后台 `scons` 编译过程中修改头文件。
- 同一份代码树里不能并行跑两个 SCons 进程，它们会争用同一个 `.sconsign` 文件。
- 切换 `debug_symbols` 会触发整个 `godot-cpp` 重编（全核耗时约 90 秒），平时尽量保持不变。

其余构建细节：

- 扩展的构建标识直接取自 `HEAD` 的提交哈希，所以每次新提交后的首次构建都会重编 `lsp_server.cpp`。
- 用 MinGW 单独完整编译 `src/pet/pet.cpp` 时需要加上 `-municode -lws2_32`，因为它在 Windows 上的入口函数是 `wmain`。

## 代码

### godot-cpp 生命周期与接口坑

- 不要把 godot-cpp 值类型（`String`、`StringName`、`Variant` 等）放进命名空间作用域或 `thread_local` 全局变量。它们会在 DLL 加载时构造，早于 godot-cpp 初始化，Windows 随即以 Error 1114 拒绝加载扩展。症状通常是间接的，例如测试报 “No loader found for resource: res://*.pet” 且场景不退出。全局状态改用标准库类型或函数内 `static`；扩展突然加载失败时，先排查新加的全局变量。
- 不要调用 `GDExtensionManager::get_extension()`。godot-cpp 会为返回的 `GDExtension` 创建包装对象，但引擎先卸载本库再释放该包装，回调会落到已卸载的库里（`core/extension/gdextension.cpp:877-880`、`core/object/object.cpp:2363`，`ed1daf0`）。典型表现是编辑器退出时崩溃（退出码 `0xC0000005`）且没有回溯。需要定位本扩展的 `.gdextension` 时，改在 `get_loaded_extensions()` 返回的路径里按 `entry_symbol` 查找。
- 按路径加载资源统一使用 `load_resource()`（`src/runtime/values.h`），不要自己把 `load()` 的结果再装进 `Variant`。
- godot-cpp 的 `String` 没有 `quote()` 和 `find_char()` 方法。

### 热路径性能约束

- godot-cpp 移动 `Variant` 时会跨编译单元整块交换字节（`thirdparty/godot-cpp/src/variant/variant.cpp:90-92`）。刚分字段写好的 `Variant` 如果立刻被移动，后续读取会因存储转发失败而停顿。热路径按值返回 `Variant` 时尽量写成 `if`/`return` 让复制消除生效；返回具名 `Variant` 的函数只留一个 `return`，NRVO 才会触发。
- godot-cpp 的几个便捷接口放在热路径上开销很大：
  - `StringName::is_empty()` 和 `Object::get_instance_id()` 各对应一次引擎调用。
  - `ObjectDB::get_instance()` 以及 `Variant` 转 `Object *` 都要查找或新建包装对象，并持有对象锁（`thirdparty/godot-cpp/include/godot_cpp/core/object.hpp:110-116`）。如果只需要引擎原始指针或实例 ID，直接调用 `object_pointer()`、`object_id_of()` 或 `object_get_instance_from_id()`。
  - `CallableCustom` 默认的 `is_valid()` 也会经过包装对象，本库自定义的 Callable 都会覆盖它。
  - `Callable::get_custom()` 带一次 `dynamic_cast`（`thirdparty/godot-cpp/src/variant/callable_custom.cpp:122-125`）。

### 编译器约定与代码格式

- 如果 checker 给出 `Error` 类型却没有附带诊断，lowering 拿到 `NO_REG` 后会直接崩溃。
- 仓库沿用 Godot 的 `.clang-format`（`ColumnLimit 0`），以下三种写法格式化后会变得很难阅读：
  - 不加括号的嵌套三元表达式，如 `a ? 1 : b ? -1 : 0`；
  - 混有多行 lambda 的 `&&` 链；
  - 写在一行的 `enum class E { A, B } field = E::A;` 成员声明。

  遇到这类写法请改用 `if`/`else`、具名临时变量或辅助函数，enum 单独声明并把成员另起一行。提交新 C++ 代码前，建议在新文件里搜索 `\?[^;]*:[^;]*\?` 清掉命中的嵌套三元式。
- Godot 上游的 pre-commit 检查中，本项目没有采用 ruff、mypy、codespell、clang-tidy、版权头、头文件守卫和 include 检查。

## 测试

### 内存与时间看门狗

编译器测试和引擎测试一律在内存与时间看门狗下运行，切勿裸跑。编译器测试的 `check_recovery` 会把每份测试源码逐行截断并重新解析。历史上解析器恢复路径一旦卡住不前，就会无限分配节点，曾两次把整机内存吃光黑屏。因此，任何不消耗 token 就返回的恢复路径都必须设置上界。

看门狗的具体约定如下：

- 编译器测试超过 1–2 GB、Godot 进程超过约 6 GB 或运行超时时立即终止进程。
- 引擎测试组被强杀后，需要把残留的 `godot*` 进程一并清理掉。
- Windows 下可用 PowerShell 的 `Start-Process` 拉起测试，每 100 ms 轮询一次 `PrivateMemorySize64`，超限即调用 `Kill()`。

### 过滤与增量运行

- doctest 的 `-tc` 参数按逗号切分用例名，带逗号的名称用 `*part*` 通配匹配。
- 迭代期间只跑改动涉及的测试组，因为 fps、ring 和 deck 三个迁移组占了整套测试的大部分时间。

### 基准测量与性能分析

做耗时优化前，先量化时间花在哪里（例如 `scons --debug=time`，或在 `tests/engine/run.py` 的 `run.run` 外层包一段计时）。跑基准时注意排除环境干扰：

- 计时前先确认机器接着交流电源（检查 `Get-CimInstance Win32_Battery` 的 `BatteryStatus` 为 2），电池模式下的数字偏差很大。
- 基准必须使用普通 `scons` 构建。`debug_symbols=yes` 会给链接器加上 `/DEBUG`，连带改变 `/OPT` 默认行为和代码布局。
- 在大小核混合 CPU 上不绑核会导致计时分成两档，手动对比时用 `cmd /c start /affinity 4 /wait /b "" <godot> ...` 固定核心。
- 纯计算组的耗时对代码布局非常敏感，只作记录参考（architecture §4）。比较两个提交时请另开 `git worktree` 构建，不要在共享工作区里来回切换；如果想确认波动是否来自布局，改动解释器之前一段无关代码再重测即可。

采样运行时热点使用 `samply`（需要管理员权限）：

- 先用 `scons debug_symbols=yes` 构建，确保 DLL 附带 PDB。
- 对只跑单个用例的项目执行 `samply record --save-only --unstable-presymbolicate -o <名字>.json.gz -- <godot> --headless --path <项目>`。
- 符号表保存在旁边的 `.syms.json` 中，按帧地址查表可得函数名，再用 `llvm-symbolizer` 还原行号。
- 官方 Godot 二进制不带符号，可用 `llvm-objdump -d` 按 `ImageBase` 加偏移反汇编确认函数；注意停顿采样点常落在停顿指令的下一条上。

## 调试

### 崩溃符号化与残留进程

- **DLL 内崩溃**：回溯里出现 `petrichor.windows.editor.x86_64.dll+0x…` 时，Godot 不会解析扩展 DLL 的 PDB。先用 `scons debug_symbols=yes` 重新构建，再运行 `llvm-symbolizer --obj=tests/engine/project/bin/petrichor.windows.editor.x86_64.dll --relative-address 0x<offset>` 解析源码位置。
- **macOS 上无声退出**：Godot 在 macOS 上只截获 `SIGSEGV`、`SIGFPE`、`SIGILL` 和 `SIGTRAP`（`platform/macos/crash_handler_macos.mm:276-279`，`ed1daf0`），libmalloc 发现坏指针时的 `SIGABRT` 不打印任何东西。引擎测试失败时，CI 把系统的崩溃报告传为 `crash-reports-*` 产物（`.ips`，JSON）。CI 的库不带符号：用 `llvm-objdump --macho --arch=arm64 --function-starts` 取函数起点，再按函数引用的字符串常量认出帧所在的函数。
- **挂起或残留的 Godot 进程**：`quit()` 之前的 GDScript 报错会让测试挂起并锁住 DLL；把原生进程输出管道接到 `Select-Object -First N` 时，读取停了但进程还在后台跑。用 `Get-CimInstance Win32_Process -Filter "Name like 'godot%'"` 排查，只杀自己启动的 `--headless` 进程；手动跑游戏建议放进 `Start-Job` 并配合 `Wait-Job -Timeout` 限时。

### 导入崩溃与 FPS 差异排查

- **首次 `--import` 退出崩溃**：新副本第一次执行 `--import`（或任何很快退出的编辑器运行）时，Godot 退出阶段可能崩溃且无回溯，退出码为 `0xC0000005`（Godot #111048，见 `import_project()`）。因此所有需要检查结果的运行都从已经导入过的副本开始。
- **定位 FPS 行为差异**：先确认差异出在本项目还是原版 kit。在临时目录里用 `run.prepare_fps(p)` 准备两份副本，其中一份再用 `run.migrate(p, run.FPS_MIGRATED)` 换上迁移后的脚本；两份各自执行 `--import` 和 `-e -- --petrichor-build`，再运行同一份 `fps_check.gd` 对比调试输出。
  - GDScript 协程里的报错只会中止当前协程，“fps original run” 仍可能误报通过，所以 `fps_check.gd` 依赖 “finish” 哨兵确认完整跑完。
  - `fps_check.gd` 的同一个函数里如果出现与前文同名的变量，脚本会直接加载失败，连带导致全部 fps 检查失败。

### 字节码与语言服务器

- **查看真实字节码**：由于 `FakeEngine` 缺少大多数引擎类，排查字节码时可在临时 doctest 中反序列化 `.godot/petrichor/<path>.petc`，再用 `bytecode::disassemble` 反汇编。
- **调试语言服务器**：
  - 在已导入的项目副本上启动 `godot --headless -e --path <副本> -- --petrichor-lsp`（先 `--import` 原因同上）。
  - 监听端口写在副本的 `.godot/petrichor/lsp.json` 里；编辑器正常退出时会清理该文件，被强杀则会残留。
  - 手动收发协议消息可用 `tests/engine/run.py` 里的 `LspClient`：先 `LspClient(port).initialize()`，再调用 `open()`、`notify()` 和 `take()`。
  - 不要直接在 `tests/engine/project` 上开图形界面编辑器调试，测试并行跑起时后开的进程会无法加载扩展。
  - 无界面编辑器不会重新获得窗口焦点，引擎本身不主动扫盘。服务器每秒比对一次项目文件来读取改动，发送 `didSave` 或 `didChangeWatchedFiles` 可立即触发比对。
  - 刚打开文件会立刻推送诊断；后续修改的诊断会在工作区静止 `idle_parse_delay` 秒后发送（默认 1.5 秒）。

## 发布包

- **版本号**：唯一来源是仓库根目录的 `VERSION`。构建把它编进 `pet --version`，CI 打插件包、演示包和 VS Code 扩展时也读它。准备新版本时只改这一处。
- **打包**：运行 `python tools/package.py <库目录> <版本> <输出目录>`。
  - `<库目录>` 可直接传入 `gh run download` 拉下的 CI 产物目录，库文件嵌套在任意层级都行。
  - 默认只要缺少任一平台库就会失败；本地只有本机库时加上 `--allow-missing`。
  - 传入 `--godot <编辑器>` 可顺便格式化演示包里的 `.pet` 文件（要求 `<库目录>` 中包含本机 editor 库）。仓库里的 `tests/engine/fps/` 保持迁移留下的格式，因为 fps 测试组会验证格式化结果；编辑器格式化时会导入项目并生成 `.uid` 文件，打包脚本只取改动后的 `.pet` 文件。
- **验收**：运行 `python tests/acceptance/run.py <官方编辑器> <模板目录> <插件包> [<演示包>]`。
  - 导出模板取自官方 `Godot_v4.7.2-stable_export_templates.tpz`（本质是 zip，只需解压当前平台的模板）。
  - 验收脚本会把模板直接写进导出预设的 `custom_template`，不会改动编辑器的数据目录。

## VS Code 扩展

- **打包**：在 `editors/vscode/` 中依次运行 `npm ci` 和 `npm run package -- <版本>`，产出 `petrichor-<版本>.vsix`（版本号默认为 `0.0.0`）。
  - `vsce` 要求扩展目录包含许可证，`vscode:prepublish` 会自动从仓库根目录复制 `LICENSE`。
  - npm 会把输出里形似 UUID 的路径段脱敏成 `***`，导致 `vsce` 找不到文件，因此不要在路径含 UUID 的临时目录里打包。
- **依赖策略**：全部直接和传递依赖均固定精确版本，且只选用发布满两周的版本：`npm install --save-exact --before=<两周前的日期>`。
- **本地隔离试用**（不污染日常 VS Code 配置）：
  - 用 `code --user-data-dir <目录> --extensions-dir <目录> --install-extension <vsix>` 安装，再用同样的两个参数打开项目；
  - 或者免安装，直接跑 `code --extensionDevelopmentPath=<仓库>/editors/vscode <项目>`。
- **连接与协议日志**：VS Code 同样可以连上无界面编辑器拉起的 LSP 服务器（见上文“字节码与语言服务器”）。排查收发消息时，在输出面板切到 Petrichor 并把日志级别调成 Trace；将 `petrichor.trace.server` 设为 `verbose` 即可打印完整消息体。

## Godot 二进制

按开发任务选用对应的 Godot 二进制，没有明确需要时不要重新编译引擎：

- **引擎测试**：使用带优化的 dev 编辑器，在 Godot 源码树中以 `scons platform=windows target=editor dev_build=yes optimize=speed_trace extra_suffix=opt` 构建。
- **调试崩溃**：使用未优化的 dev 编辑器。
- **性能基准**：必须使用官方 `4.7.2-stable` 发布版，因为 dev 构建会扭曲与 GDScript 的对比结果。
- **C# 互操作**：跑 dotnet 测试组或带 C# 手动运行 Ring 时使用官方 .NET 版，并确保 `PATH` 中有 .NET SDK 8 或更新版本。

## 设计提案与语料

提出语言或工具设计方案时，提案应逐项说明以下内容：

- 用户看到的效果，用两三段具体代码示例呈现；
- GDScript 的做法与引擎出处，对 GDScript 或引擎缺陷给出合理语义而非照搬；
- 若成熟语言或工具已有同类组件，先读它们的源码与文档，调研实现方式与踩过的坑，不要假定 GDScript 的方案最好；
- 是否与周边默认规则一致，以及是否允许 GDScript 允许的写法；
- 与已释放对象、就绪前、`await` 与取消、引擎边界、场景继承、热重载的交叉点。无显而易见结论的情形各写不超过五行的脚本，对照规范保证判定并转为测试；
- 现在实现与延后实现的成本权衡。

能由真实代码回答的设计问题，先在真实 Godot 项目中调查。调查报告连同复现脚本存放在 [petrichor-corpus](https://github.com/YYF233333/petrichor-corpus) 仓库，获取语料和复跑方式见其 README。支撑单条规则的计数放入 `corpus-counts`。每份调查都记录当时评估的设计；设计调整后只需指向现行规则，不要重写历史数据。

## 文档

文档写法应匹配各自的角色。提交钩子 `tools/check_doc_style.py` 会标记改动行里超过 60 字或 40 词的句子（状态标记不计入）。它用来防多条意思挤在一句里，不是要求拆碎自然句子；发布说明、报告等一次性文本不受此限。

- 规范与速查表：一句只讲一件事，条件放在结果前面，句内并列或连串的“或”改写成列表。
- `README`、`docs/getting-started.md`（含中文版）、`docs/design-notes.md`、`docs/architecture.md` 和 `docs/development.md`：优先使用节奏自然的短散文段落，避免大段条目；列表留给并列项、步骤或对照，长实现段落用子标题分组。
- `README` 和上手指南写给第一次接触本语言的 Godot 开发者，另有四条：
  - 按读者的操作顺序写。每一步先说做什么、会看到什么，再说原因；例外和边界放到后面的小节。
  - 命令先给完整写法。截短的程序输出用 `...` 标出删节处。
  - 不用实现里的词，如“槽位”“流入”“撤回”。规范术语第一次出现时用一句白话或一个例子说明，程序输出里的词照录后再解释。
  - 中英文两版先对齐事实，再各自成文，不逐句对译，也不搬规范的句式。
- 通用要求：用具体主语与主动动词，避免宣传或居高临下的空话，`path:line` 出处与链接放在句末括号里。
- 规范内引用其他规则时写概念名（如“比较规则”），不写章节号。
