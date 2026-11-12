# 开发说明

构建、测试和调试中反复踩到的坑。

## 构建

- 目标文件和静态库在 `build/` 下（`SConstruct` 的 VariantDir）。
- 源码旁出现 `.obj` 时要删掉，例如构建过旧提交之后。否则 SCons 把它们当作已构建，跳过编译。
- 测试正在用 DLL 时不要构建。
- 后台 `scons` 在编译时，不要改头文件。
- 同一棵树里不要同时跑两个 SCons，它们共用 `.sconsign`。
- 切换 `debug_symbols` 会重编整个 godot-cpp，全核约 90 秒，少做。

## 代码

- godot-cpp 的值（String、StringName、Variant 等）不能放进命名空间作用域或 `thread_local` 的全局变量。
  - 它们在 DLL 加载时构造，早于 godot-cpp 初始化，Windows 于是以 Error 1114 拒绝加载。
  - 症状是间接的：测试报 “No loader found for resource: res://*.pet”，场景不退出。
  - 全局状态改用 std 类型或函数内的 static。扩展突然加载不了时，先查新加的全局变量。
- 不要调用 `GDExtensionManager::get_extension()`，godot-cpp 会为返回的 GDExtension 建包装。
  - 引擎先关闭本库，再释放这个包装，回调落在已卸载的库里（`core/extension/gdextension.cpp:877-880`、`core/object/object.cpp:2363`，`ed1daf0`）。
  - 症状是编辑器退出时崩溃，退出码 0xC0000005，没有回溯。
  - 要找本扩展的 `.gdextension`，在 `get_loaded_extensions()` 给出的路径里按 `entry_symbol` 找。
- 按路径加载资源只用 `load_resource()`（`src/runtime/values.h`），不要自己把 `load()` 的结果装进 Variant。
- godot-cpp 的 String 没有 `quote()` 和 `find_char()`。
- godot-cpp 移动 Variant 时整块交换字节，函数不在头文件中（`thirdparty/godot-cpp/src/variant/variant.cpp:90-92`）。
  - 刚分字段写好的 Variant 一被移动，读取就因存储转发失败而停顿。
  - 热路径上按值返回 Variant 时，写成 if/return，让复制消除生效。
  - 返回带名字的 Variant 的函数只留这一个 return，NRVO 才会生效。
- godot-cpp 的一些便捷接口放在热路径上很贵：
  - `StringName::is_empty()` 和 `Object::get_instance_id()` 各是一次对引擎的调用。
  - `ObjectDB::get_instance()` 和 Variant 转 `Object *` 都要找到或建出包装对象，并持有对象的锁（`thirdparty/godot-cpp/include/godot_cpp/core/object.hpp:110-116`）。
  - 只要引擎的指针或 ID 时，用 `object_pointer()`、`object_id_of()` 或 `object_get_instance_from_id()`。
  - `CallableCustom` 默认的 `is_valid()` 也经过包装对象，本库的 Callable 都覆盖它。
  - `Callable::get_custom()` 带一次 `dynamic_cast`（`thirdparty/godot-cpp/src/variant/callable_custom.cpp:122-125`）。
- checker 给出 Error 类型而没有诊断时，lowering 拿到 `NO_REG` 会崩溃。
- clang-format 按 Godot 的 `.clang-format`（ColumnLimit 0）会把以下写法排得难以阅读：
  - 不加括号的嵌套三元表达式，如 `a ? 1 : b ? -1 : 0`；
  - 混有多行 lambda 的 `&&` 链；
  - 写在一行的 `enum class E { A, B } field = E::A;` 成员。
- 遇到这些写法，改用 if/else、具名临时变量或辅助函数；enum 单独声明，成员另起一行。
- 提交新 C++ 之前，在新文件里搜 `\?[^;]*:[^;]*\?`，把命中的改掉。
- Godot 的 pre-commit 检查中，这些没有采用：ruff 和 mypy、codespell、clang-tidy、版权、头文件守卫和 include 检查。

## 测试

- 编译器测试和引擎测试都在内存与时间看门狗下运行，不要裸跑。
  - 编译器测试超过 1–2 GB，godot 超过约 6 GB，或者超时，就杀掉进程。
  - 引擎测试组被杀时，所有 `godot*` 进程一并杀掉。
  - 做法：PowerShell 用 Start-Process 启动，每 100 ms 读一次 `PrivateMemorySize64`，超限即 `Kill()`。
  - 原因：编译器测试把每份测试源码在每一行截断后再解析（`check_recovery`）。
  - 解析器的恢复路径若不前进，就会无限分配节点，曾两次让整机黑屏。
  - 所以任何不消耗 token 就返回的恢复路径都要有上界。
- doctest 的 `-tc` 按逗号切分名字，用 `*part*` 匹配。
- 只跑改动涉及的测试组。FPS 和 Ring 迁移组占了套件的大部分时间。
- 计时前先确认接着交流电源（`Get-CimInstance Win32_Battery`，BatteryStatus 为 2）。用电池时测得的数字偏差很大。
- 优化之前先测时间花在哪里（`scons --debug=time`，或在 `tests/engine/run.py` 的 `run.run` 外包一层计时）。
- 跑基准用普通的 `scons` 构建。`debug_symbols=yes` 让链接器带上 `/DEBUG`，它改变 `/OPT` 的默认值和代码布局。
- CPU 混有两种核心时，不固定核心的计时分成两档。手动比较时用 `cmd /c start /affinity 4 /wait /b "" <godot> ...` 固定。
- 纯计算组的数字随代码布局变化，无关的改动也会让它变化 20% 以上，只作记录（architecture §4）。
  - 要比较两个提交时，在 `git worktree` 中构建另一个提交，不要在共享的工作区里切换。
  - 判断一个变化是否来自布局：改动解释器之前无关的代码再测一次。
- 运行时的热点用 samply 采样，它需要管理员权限。
  - 先用 `scons debug_symbols=yes` 构建，DLL 才有 PDB。
  - 只跑一个用例的项目，用 `samply record --save-only --unstable-presymbolicate -o <名字>.json.gz -- <godot> --headless --path <项目>` 运行。
  - 符号在旁边的 `.syms.json` 中，按帧地址查表即得函数名；`llvm-symbolizer` 给出行号。
  - 官方的 Godot 没有符号。用 `llvm-objdump -d` 按 ImageBase 加地址反汇编，看是哪个函数。
  - 停顿的样本常落在停顿指令的下一条上。

## 调试

- DLL 内的崩溃：回溯里出现 `petrichor.windows.editor.x86_64.dll+0x…` 时，Godot 不会解析这个 DLL 的 PDB。
  - 先用 `scons debug_symbols=yes` 构建。
  - 再运行 `llvm-symbolizer --obj=tests/engine/project/bin/petrichor.windows.editor.x86_64.dll --relative-address 0x<offset>`。
- 挂起或残留的 godot 进程：
  - `quit()` 之前的 GDScript 错误会让运行挂住，并锁住 DLL。
  - 把原生进程的输出接到 `Select-Object -First N` 时，读取停止，进程却还在运行。
  - 用 `Get-CimInstance Win32_Process -Filter "Name like 'godot%'"` 检查，只停自己启动的 `--headless` 进程。
  - 手动运行游戏时，放进 `Start-Job`，再用 `Wait-Job -Timeout` 限时。
- 新副本第一次 `--import`，编辑器退出时可能崩溃（Godot #111048，见 `import_project()`）。
  - 新副本上任何很快退出的编辑器运行也会这样，退出码 0xC0000005，没有回溯。
  - 所以需要检查的运行都从已导入的副本开始。
- FPS 结果有差异时，先判断问题在本项目还是在 kit：
  - 在 scratchpad 用 `run.prepare_fps(p)` 做两份副本，其中一份再用 `run.migrate(p, run.FPS_MIGRATED)` 换上迁移后的脚本。
  - 两份各自 `--import`，再 `-e -- --petrichor-build`，然后运行同一个 fps_check.gd，比较调试输出。
  - GDScript 协程中的错误只结束那个协程，所以检查出错时 “fps original run” 仍可能通过。fps_check.gd 因此用 “finish” 哨兵。
  - fps_check.gd 的同一函数里若有与前面同名的变量，它就加载失败，所有 fps 检查随之失败。
- 读真实的字节码：在临时 doctest 中反序列化 `.godot/petrichor/<path>.petc`，再用 `bytecode::disassemble` 反汇编。FakeEngine 缺大多数引擎类。

## 发布包

- 打包：`python tools/package.py <库目录> 0.1.0 <输出目录>`。
  - 库目录可以是 `gh run download` 取下的 CI 产物，库在哪一层都行。
  - 本地只有本机的库，所以加 `--allow-missing`。不加时缺一个库就失败。

## Godot 二进制

- 按用途选二进制：
  - 引擎测试用优化过的 dev 编辑器，在 Godot 源码树中用 `scons platform=windows target=editor dev_build=yes optimize=speed_trace extra_suffix=opt` 构建；
  - 调试崩溃用未优化的 dev 编辑器；
  - 基准用官方 4.7.2-stable 发布版，dev 构建会扭曲与 GDScript 的比较；
  - 带 C# 运行 Ring 用官方 .NET 版。
- 没有要求时不要重建引擎。

## 语料

- 调查用的 20 个仓库按固定提交克隆，只取第一方代码：`python docs/surveys/cross-branch-duplication/gdcorpus.py <目录>`。
- 克隆放在临时目录时，目录被清理后用上面的命令重建。
