# 调查：Godot 4 脚本的暗坑清单

**2026-10-23** · 引擎：Godot 提交 `469659e` · 关联：[语言规范](../language-spec.zh.md) · English: [pitfalls.en.md](pitfalls.en.md)

本文整理写 Godot 4 脚本时会踩到的暗坑，分引擎与 GDScript、C#（.NET）桥接两组，共 80 条，并逐条判断：一门新的静态类型脚本语言及其工具链能否消除它，靠什么消除，修复是否需要只对引擎模块开放、不对 GDExtension 开放的 C++ 接口。清单本身在 [`pitfalls/catalog.md`](pitfalls/catalog.md)，数据在 `pitfalls/catalog.tsv`；本文说明它的来源和做法，并汇总结果。文中的修复类别都是推断，不是决定；规范按编号引用条目，引用处列在文末。

## 问题

- 写 Godot 4 脚本的人常踩哪些坑？引擎与 GDScript 的有哪些，C# 桥接的有哪些？
- 对用新语言写的代码，每一条能否靠语言语义、静态检查或工具链、基于公开 API 的运行时封装来消除？
- 有没有哪条的修复需要不对 GDExtension 公开的 C++ 接口，因而只能做成模块？哪些部分无论采用哪种交付形态都要改引擎？

## 来源

| 来源 | 范围 | 用法 |
|---|---|---|
| Godot 源码 | godotengine/godot 提交 `469659e0617345149cd1eaec2e643d61d130aa6a`（master，4.8-beta） | 只读，不编译、不运行；引用为 `path:line` |
| Godot 文档 | docs.godotengine.org/en/latest 的教程和类参考 | 引用原文和页面地址 |
| GitHub | godotengine/godot 的 issue 和 PR，godotengine/godot-proposals | 用 GitHub CLI 的 GraphQL 和 REST 接口检索；状态、反应数和评论数于 2026-10-23 读取 |
| 论坛与问答 | forum.godotengine.org、godotforums.org、Stack Overflow | 站内检索接口和网页搜索，只统计主题数或列出标题 |

另外转储了 godotengine/godot 中带 `topic:gdscript` 标签的全部 4,377 个 issue，以及 godot-proposals 中带同一标签的全部 941 个 proposal，按反应数排序，用来给单个 issue 或 proposal 定位次，例如“topic:gdscript proposal 中反应数第 2”。

调查由五名研究者（AI 代理）分头完成，共产出 102 条原始行：

| 研究者 | 范围（按产出的行归纳） | 原始行 |
|---|---|---:|
| EG-A | 生命周期、对象存活与泄漏、协程、信号、计时器、静态变量 | 20 |
| EG-B | 资源缓存与复制、保存与导出、`@tool` 与编辑器、节点路径和文件路径、热重载 | 20 |
| EG-C | 类型系统与语言语义：lambda 捕获、可空性、类型转换、容器、相等、枚举、覆盖、命名空间、错误处理、接口、泛型 | 20 |
| CS-A | C# 脚本身份、跨语言调用、类型映射、编辑器集成 | 21 |
| CS-B | 导出平台、构建与工具链、重新加载、对象生命周期与 GC、性能、异常 | 21 |
| **合计** | | **102** |

## 方法

1. **原始行。** 每行 13 个字段：编号、分组、标题、描述、最小例子、根源所在的层、issue 证据、文档证据、源码证据、频率信号、状态、修复类别、类别理由。引擎行为只靠阅读源码确认；需要运行才能确认的说法标为“unverified (needs repro)”。
2. **修复类别。** 针对一门不基于 .NET 的新静态类型脚本语言及其工具链判断，判断的对象是用这门语言写的代码：

   | 类别 | 含义 |
   |---|---|
   | L | 语言语义 |
   | T | 静态检查或工具链 |
   | W | 基于公开的 ClassDB 和 GDExtension 接口的运行时封装 |
   | M | 需要不对 GDExtension 公开的 C++ 接口，只有模块能做 |
   | E | 需要修改引擎核心 |
   | N/A | .NET 特有；括号中给出新语言会遇到的对应问题及其类别 |

   类别可以组合。“L+W (order itself: E)”表示 L 和 W 能为新语言的代码消除这个坑，但括号中点名的引擎行为仍然存在。主类别取类别字符串中的第一个字母；类别字符串中写有 E 的条目计为“含 E 部分”。
3. **合并。** 102 条原始行中，有 42 条因内容相同或相近，手工合并为 20 条，记在 `merges.py` 中，每条注明来自哪些原始行；其余 60 条原样保留。`build.py` 检查每条原始行恰好用到一次，按 `merges.py` 中的顺序编号为 G-01–G-49 和 C-01–C-31，计算下文的统计，生成 `catalog.tsv` 和 `catalog.md`。
4. **频率信号。** 每条都记下所引 issue 和 proposal 的反应数与评论数、它在转储中的位次、标题检索的命中数，以及论坛检索的主题数。这些数只表示有多少人关心，不是发生频率，见“局限”。
5. **核对。** 本文引用的每个 `path:line` 都在写作时对照同一提交重新读过。路径相对于 Godot 仓库根目录；`Godot.SourceGenerators/` 指 `modules/mono/editor/Godot.NET.Sdk/Godot.SourceGenerators/`，`GodotSharp/Core/` 指 `modules/mono/glue/GodotSharp/GodotSharp/Core/`；单独的 `:n` 指它前面最近提到的文件。

## 结果

### 各类别的条目数

| 分组 | L | T | W | M | E | N/A | 合计 | 含 E 部分 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 引擎与 GDScript | 36 | 6 | 7 | 0 | 0 | 0 | 49 | 19 |
| C# | 13 | 3 | 4 | 0 | 1 | 10 | 31 | 2 |
| **合计** | **49** | **9** | **11** | **0** | **1** | **10** | **80** | **21** |

- 10 条 N/A 的对应问题中，主类别为 L 的 7 条，T 2 条，W 1 条。
- 根源所在的层：引擎与 GDScript 的 49 条中，引擎核心 25 条，GDScript 语言 22 条，编辑器与工具链 2 条；C# 的 31 条中，.NET 桥接 24 条，编辑器与工具链 5 条，引擎核心 2 条。
- 按这一分类，引擎与 GDScript 的 49 条对新语言的代码都可以消除，大多靠语言语义；其中 19 条留有引擎层的部分，GDScript、其他语言和编辑器仍会遇到。C# 的 31 条中，10 条随 .NET 一起消失，但每条都有新语言同样要回答的对应问题；主类别为 E 的只有 C-17。

### 关注度最高的条目

取每条所引 issue、proposal 或 PR 中最高的反应数，不少于 100 的有 15 条（G-27 只把 proposals#162 列为相关，不计入）；再加上反应数不高、但会静默丢失数据的 G-25，共 16 条。反应数于 2026-10-23 读取。

| 条目 | 暗坑 | 频率信号 | 引擎依据 | 类别 |
|---|---|---|---|---|
| G-37 | 对象类型隐式可空，没有空安全 | proposals#162：1,038，topic:gdscript proposal 中第 2；论坛中含 “null instance” 的主题至少 350 个 | 分析器认为 null 与任何对象类型兼容（`modules/gdscript/gdscript_analyzer.cpp:6460-6462`） | L+T |
| G-49 | 没有泛型；带类型的容器只有一层 | proposals#12224：260；相关的 5 个 proposal 共 1,612，其中已在 4.4 实现的带类型 Dictionary 占 836 | `Array[Array[int]]` 是解析错误（`modules/gdscript/gdscript_parser.cpp:3879`）；引擎容器只记一层元素类型（`core/variant/container_type_validate.h:37-40`） | L+T（引擎可见的部分：E） |
| G-48 | 没有接口或 trait，跨继承分支的约定只能靠鸭子类型 | proposals#4872：830；#6416：672；分列第 4、第 5 | 对象类型的兼容只沿继承链判断（`modules/gdscript/gdscript_analyzer.cpp:6458-6507`） | L |
| G-15 | 编辑器中建立的信号连接在 `.tscn` 中以字符串记下处理函数名；处理函数改名或删除后，连接被静默跳过 | proposals#899（重命名工具，4.8 实现，只改脚本）：520；场景部分只在 #123958 中跟踪：6 | 加载时按名字构造 Callable，不检查方法是否存在（`scene/resources/packed_scene.cpp:747, 763`）；发射时跳过无效的 Callable，不报告（`core/object/object.cpp:1328-1331`） | T |
| C-26、C-28 | 结构体属性和 packed 数组读出的是副本；字符串隐式转换为 StringName 等，每次调用都要分配 | proposals#7842：497 | packed 数组每次转换都复制原生缓冲区（`GodotSharp/Core/NativeInterop/Marshaling.cs:437-446`）；字符串隐式转换每次新建 StringName（`GodotSharp/Core/StringName.cs:81`） | L；L+T |
| C-19 | C# 项目不能导出到 Web，移动平台仍为实验性 | 草稿 PR #106125（Web 导出）：451；#68153（Android，4.2 关闭）：234；#70796（Web）：103 | 阻碍在 .NET 的 WASM 运行时，不在 `modules/mono` 中；未在源码中核实 | N/A（对应：W） |
| G-45 | class_name 和 [GlobalClass] 共用一个扁平的全局命名空间 | proposals#1566：363，第 10 | 所有语言的全局类登记在同一张以名字为键的表中，已有条目原地更新（`core/object/script_language.cpp:408-431`） | L（登记表部分：E） |
| G-46 | get_class() 和 is_class() 忽略 class_name | #21789：170，topic:gdscript issue 中第 2 | `get_class()` 返回原生类名（`core/object/object.h:677`） | L（引擎部分：E） |
| C-11 | .NET 集合、LINQ 结果、struct、record 和 Nullable<T> 不能越过引擎边界 | proposals#438：143 | 可封送的类型是一张封闭的列表（`Godot.SourceGenerators/MarshalUtils.cs:88-249`）；用到其他类型的方法被静默略去（`Godot.SourceGenerators/ExtensionMethods.cs:321-344`） | N/A（对应：L+T） |
| G-14 | 信号参数在 connect 和 emit 时不检查类型和个数 | proposals#2557：134 | emit 不检查参数，不匹配只在调用处理函数时打印错误（`core/object/object.cpp:1256-1283, 1367-1385`） | L+T |
| G-02 | `_init` 有必需参数时，场景实例化、duplicate() 和 set_script() 失败 | proposals#1513：130 | 创建脚本实例的接口不带参数（`core/object/script_language.h:156`）；GDScript 以零个参数构造（`modules/gdscript/gdscript.cpp:434`） | L+W（零参数构造：E） |
| C-18 | GDExtension 注册的类没有 C# 类型 | proposals#8191：129 | 创建托管包装时跳到第一个由引擎注册的基类（`modules/mono/csharp_script.cpp:1145-1153`） | W |
| G-20 | Resource.duplicate() 的深度：默认浅复制，深复制不复制外部子资源 | #74918（4.5 关闭）：120 | `duplicate(true)` 固定使用 INTERNAL 模式，只复制内置子资源（`core/io/resource.cpp:296-305, 517-529`） | W+L |
| G-34 | 互相引用的场景和资源加载失败 | proposals#7363：110；#21461（class_name 循环，4.0 修复）：110 | 同一线程上重入的加载被当作循环，返回 null（`core/io/resource_loader.cpp:1049-1056`） | T+L（加载器的循环：E） |
| G-25 | 导出成员改名或删除后，所有场景和资源中保存的值静默丢失 | proposals#10644：42；同一请求共有 5 个 proposal | 实例化时不检查 `node->set` 是否成功，未知的名字被丢弃（`scene/resources/packed_scene.cpp:567`） | L+T |

### 没有只能由模块修复的条目

- 80 条中没有一条归为 M。目录给出的理由是：各条用到的钩子都已对 GDExtension 公开，包括脚本实例的回调（`GDExtensionScriptInstanceInfo3`，`core/extension/gdextension_interface.json:3242-3349`），以及 ScriptExtension 和 ScriptLanguageExtension 的虚函数：脚本的重新加载（`core/object/script_language_extension.h:79, 686, 690`）、导出成员和属性列表（`:176, 188`）、校验、补全和符号查找（`:393, 493, 533`）、全局类名（`:754`）。规范把这一点作为以 GDExtension 交付本语言的理由之一（§1.3，附录 B）。
- 与引擎内部的 ScriptInstance 接口（`core/object/script_instance.h:40-96`）相比，扩展的脚本实例回调只缺两项：`call_const`（`:70`）和 `get_rpc_config`（`:93`）。前者的默认实现直接调用 `callp`（`core/object/script_instance.cpp:57-59`）；后者默认取脚本的 RPC 配置（`:88-90`），扩展的脚本可以通过 `_get_rpc_config` 提供（`core/object/script_language_extension.h:220-224`）。目录中没有条目依赖这两项（推断）。
- 含 E 部分的条目，模块同样修不了：模块能调用未导出的 C++ 接口，但改变不了引擎的行为（推断）。例如 G-06 中节点的锁定计数 `data.blocked` 是私有成员，只有 protected 的 `_block` 和 `_unblock`，C++ 中也没有读取它的公开接口（`scene/main/node.h:217, 375-376`）。
- 两种交付形态的差别在打包，不在接口（推断，见 C-19）。GDExtension 在 Web 上需要在导出预设中启用 Extensions Support（`variant/extensions_support`，默认关闭，`platform/web/export/export_plugin.cpp:374`），文档还要求站点配置跨域隔离响应头（[Exporting for the Web](https://docs.godotengine.org/en/latest/tutorials/export/exporting_for_web.html)）。模块则与引擎一起编译，需要自行构建的编辑器和导出模板。

### 引擎层的残余

21 条含 E 部分。下表按主题归组，归组是本文的整理；每行写出留在引擎中的行为。对用新语言写的代码，这些条目大多可以绕开，但引擎持有的对象和容器、其他语言以及编辑器仍然看到这些行为。

| 主题 | 条目 | 留在引擎中的行为 |
|---|---|---|
| 生命周期顺序 | G-01 | 实例化时先设置节点保存的属性（`scene/resources/packed_scene.cpp:567`），再把节点加到父节点下（`:616`），这时它自己的子节点还不存在 |
| | G-02 | 创建脚本实例时不传参数（`core/object/script_language.h:156`）；场景中的节点先由 ClassDB 构造，再设置脚本（`scene/resources/packed_scene.cpp:389, 496`） |
| | G-03 | `_ready` 先在子节点上执行，再在父节点上执行（`scene/main/node.cpp:326-336`） |
| | G-06 | 节点向子节点传播 enter_tree 和 ready 时处于锁定状态（`scene/main/node.cpp:325-330, 374-383`），这时对它调用 add_child 会失败（`:1721`）；锁定计数是私有成员（`scene/main/node.h:217`） |
| | G-09 | PREDELETE 先送到脚本（`core/object/object.cpp:140, 1009-1011`），然后节点把自己从父节点移除，再删除子节点（`scene/main/node.cpp:304-312`）；子树退出时子节点先于父节点（`:421-427`） |
| 对象模型 | G-10 | Node 直接继承 Object，不做引用计数（`scene/main/node.h:54`），从树中移除而没有释放的节点会泄漏 |
| | G-46 | `get_class()` 返回原生类名（`core/object/object.h:677`） |
| | C-17 | 注册为抽象的引擎类（例如 `CollisionObject3D`、`PhysicsBody3D`，`scene/register_scene_types.cpp:736-737`）不能由任何语言的脚本子类实例化（#71834）；脚本不能继承另一种语言的脚本（文档） |
| 全局登记 | G-45 | 所有语言的全局类共用 ScriptServer 中一张扁平的表（`core/object/script_language.cpp:408-431`） |
| 容器与 Variant | G-27 | 元素类型为对象的数组接受 null（`core/variant/container_type_validate.h:60-61`） |
| | G-28 | 带类型的数组在运行时记录元素类型，按内置类型、类名和脚本三者精确比较（`core/variant/container_type_validate.h:37-47`） |
| | G-49 | 元素类型只有一层（`core/variant/container_type_validate.h:37-40`） |
| | G-41 | Variant 的 `==` 只为部分类型组合注册（`core/variant/variant_op.cpp:489-493`）；Dictionary 的键先比较类型，只有 String 与 StringName 例外（`core/variant/dictionary.cpp:46`；`core/variant/variant.cpp:3213-3216, 3437-3447`） |
| | G-42 | 枚举值在引擎边界上作为 int 传递和保存 |
| 资源与场景 | G-19 | 按路径加载的资源在缓存中只有一份，所有使用者共享（`core/io/resource_loader.cpp:800-810`） |
| | G-22 | 只有实例化时设置的值经过 local-to-scene 处理（`scene/resources/packed_scene.cpp:509-514`） |
| | G-31 | 继承场景和实例保存哪些节点和修改，由 owner 和实例关系决定（`scene/resources/packed_scene.cpp:880-882, 1153-1154`）；基础场景修改后已打开实例的刷新也在编辑器中 |
| | G-34 | 同一线程上的循环加载返回 null（`core/io/resource_loader.cpp:1049-1056`） |
| 其他 | G-17 | Timer 节点每帧最多发出一次 timeout（`scene/main/timer.cpp:60-67`） |
| | G-47 | 引擎 C++ 方法中的错误只打印并返回默认值；脚本只能通过 `OS.add_logger` 事后观察（`core/core_bind.cpp:870`） |
| | C-10 | 编辑器把新生成的处理函数插在文件末尾（`editor/script/script_text_editor.cpp:430-434`）；对用花括号包住整个类的语言，这个位置在类外 |

### 三个 C# 问题

调查专门追查了 C# 桥接的三个问题。它们在目录中的对应条目和发现如下。

**1. 文件名必须与类名相同。** C-01；另见 G-45。

- 源码生成器只把名字与文件名相同的顶层 partial 类登记为脚本；嵌套类、非 partial 类和名字不同的类被跳过，不给诊断（`Godot.SourceGenerators/ScriptPathAttributeGenerator.cs:43-56`）。
- 错误要到实例化时才出现：“Cannot instantiate C# script because the associated class could not be found …”（`modules/mono/csharp_script.cpp:2550-2551`）。构建失败和程序集问题也给出同一条信息；含这句话的 issue 有 22 个，原因各不相同。C# 基础文档中引用的错误信息 “Cannot find class XXX for script res://XXX.cs” 在 `modules/mono` 中已不存在。
- 在外部 IDE 中给文件或类改名，会断开两者的对应以及场景中的引用（#85274，开放，2026-02 在 4.6.1 上复现）。
- `[GlobalClass]` 只登记类型名，丢掉命名空间（`GodotSharp/Core/Bridge/ScriptManagerBridge.cs:279`），登记进 G-45 中那张扁平的表。
- 类别为 L+T。推断：如果文件本身就是类，就没有第二个名字可以对不上。

**2. 跨语言调用与文档不符，以及崩溃。** C-05、C-06、C-07、C-08；崩溃与失效见 C-25、C-22、C-16。

| 文档的说法 | 源码中的行为 | 条目 |
|---|---|---|
| “Accessing C# fields from GDScript is straightforward, you shouldn't have anything to worry about.” | 编辑器构建中，C# 脚本只报告导出成员（`modules/mono/csharp_script.cpp:2952-2961`），静态成员不经过桥接：GDScript 读不到静态字段，补全中缺少成员，编辑器中与导出后的成员列表不同 | C-06 |
| “calling C# methods from GDScript should be straightforward” | 按方法名和参数个数查找（`modules/mono/csharp_script.cpp:1767-1782`），省略默认参数就找不到；参数个数相同的重载只留一个；泛型方法、带 ref/out 参数或不能映射到 Variant 的方法被静默略去（`Godot.SourceGenerators/ExtensionMethods.cs:321-344`） | C-05 |
| C# 用错误的参数调用 GDScript 方法时 “will fail silently and won't error out” | 只在 debug 构建中以 `GD.PushError` 报告（`GodotSharp/Core/NativeInterop/ExceptionUtils.cs:145-155`），从不抛异常；GDScript 的 lambda 传给 C# 后变成空的 Callable（`modules/mono/glue/runtime_interop.cpp:533-538`） | C-07 |
| 无类型数组 “can be converted to a typed array using the Godot.Collections.Array<T>(Godot.Collections.Array) constructor” | 这个构造函数只包装原数组，引擎一侧仍是无类型数组（`GodotSharp/Core/Array.cs:1188-1193`） | C-08 |

目录中没有找到由跨语言调用本身引起的崩溃。找到的崩溃和失效出在 .NET 运行时与引擎的结合处：

- C-25：只由 C# 持有的 RefCounted 要等 GC 终结器在另一个线程上释放；资源缓存可能复活一个包装已被回收的资源，在 debug 构建中触发 `CRASH_COND(gchandle.is_released())`（`modules/mono/csharp_script.cpp:1916`）。#83762 已开放三年，有 39 条评论，到 2026-08 才追查到资源缓存。
- C-22：编辑器重新构建时，只要程序集卸载失败，到重启编辑器为止 C# 脚本都不可用（`modules/mono/glue/GodotSharp/GodotPlugins/Main.cs:254-277`）。报错信息直接链接跟踪 issue #78513（`modules/mono/mono_gd/gd_mono.cpp:811`），该 issue 有 95 个反应和 194 条评论。
- C-16：编辑器中非 [Tool] 的脚本对象是占位实例，C# 把它们包装成原生基类，[Tool] 代码把它们转换为其 C# 类型时抛出 InvalidCastException（#36395：24；#85459：19）。

**3. C# 的功能接不回引擎。** C-02、C-09、C-11、C-12、C-13、C-14、C-15。

- 能越过边界的类型是一张封闭的列表（`Godot.SourceGenerators/MarshalUtils.cs:88-249`）。导出其他类型是编译错误（GD0102），用作信号参数也是（GD0202）；但用到这些类型的普通方法会被静默略去（`Godot.SourceGenerators/ExtensionMethods.cs:321-344`）。因此 .NET 集合、LINQ 结果、struct、record 和 Nullable<T> 不能导出，不能作信号参数，也不能作为 Variant 传递（C-11）。
- 接口不派生自 GodotObject，没有对应的封送类型（`Godot.SourceGenerators/MarshalUtils.cs:210-213, 248`），不能作信号参数或导出类型（C-12）。
- 返回 Task 的方法不能映射到 Variant，因而不暴露给引擎（`Godot.SourceGenerators/ExtensionMethods.cs:324-330`）；GDScript await 一个不是信号的值时立即继续（`modules/gdscript/gdscript_vm.cpp:2591-2596`）（C-14）。
- `[Signal]` 事件的 `+=` 只加到一个私有的后备委托上（`Godot.SourceGenerators/ScriptSignalsGenerator.cs:289-309`），引擎看不到这个处理函数：连接列表中没有它，IsConnected 返回 false，接收者释放时也不会自动断开（#70414：40）（C-09）。
- 泛型类有脚本文件，但不能实例化（`modules/mono/csharp_script.h:137-139`），也不能挂到节点上或作全局类（C-02）。
- 枚举在方法和信号的元数据中报告为普通 int（C-13）；非 tool 脚本在检查器中显示的导出默认值由分析源码猜出，而不是执行初始化式得到（C-15）。
- 这 7 条中，C-02、C-09、C-11、C-12、C-14 为 N/A，C-13 为 L+W，C-15 为 L+T。推断：新语言自己的泛型、struct、接口和协程会遇到同样的问题，即每一种要越过引擎边界的类型都需要确定的 Variant 表示；对应问题的类别分别为 L（C-02）、L+W（C-09、C-14）和 L+T（C-11、C-12）。

## 局限

- **偏向 issue 跟踪器。** 证据主要来自 GitHub。写 issue 和 proposal 的多是有经验的用户和插件作者；按设计如此、已有文档的行为常以“不计划修复”关闭，之后不再积累反应。初学者多在论坛提问，而论坛只统计了主题数。
- **反应数不等于发生频率。** 反应表示有多少人赞同或想要，不表示有多少人踩到。功能请求比缺陷报告更容易积累反应，旧的 issue 比新的多，重复的 issue 会分散反应。G-15 的 520 是整个重命名工具的反应数，场景部分本身只有 6。
- **检索有选择。** 计数来自 GitHub 的 GraphQL 和 REST 检索、标题检索和网页搜索，取决于研究者选用的检索词。“标题含某句话的 issue 有 39 个”一类的数包含不相关的命中；论坛检索只翻有限的页数，超出时只记“至少”。
- **没有运行复现。** 目录中没有一条经运行引擎确认。12 条的依据或类别中有一部分标为未核实或由阅读推断（G-04、G-09、G-18、G-23、G-24、G-31、G-34、G-48、C-01、C-16、C-19、C-21），其中 G-04、G-18、G-23、G-24 明确需要复现；另有 7 条没有找到文档说明（G-25、G-26、G-27、G-31、G-33、G-34、G-35）。曾有一名研究者用本地构建的 Godot 编辑器以无界面模式在一个一次性项目上运行了约 10 次；目录只引用源码和网页证据，没有用到这些运行的结果。
- **分类是推断。** 修复类别由 AI 代理判断，针对的是一门尚未实现的语言，没有经过实现检验。把 102 条原始行合并为 80 条也是人工判断，每次合并都记在 `merges.py` 中。
- **覆盖不全。** 目录中没有以下方面的条目：GDScript 一侧的多线程、多人联机与 RPC、动画轨道和 `tween_property` 中的属性路径、输入动作名和分组名、GDScript 的运行性能、着色器、翻译。反应数很高、但不表现为“坑”的功能请求也不在其中，例如 proposals#7329 struct（1,086，topic:gdscript proposal 中第 1）、#737 联合类型（510）、#641 访问修饰符（423）。
- **只对照 master。** 引擎事实只在提交 `469659e` 上核对；旧版本中的行为只依据 issue 中的描述。

## 复现

[`pitfalls/`](pitfalls/) 目录包含：

- `catalog.tsv`：80 条，15 列，即原始行的 13 列加上 `primary_class` 和 `source_rows`；
- `catalog.md`：同样的内容排成表格；
- `raw/`：五名研究者的 102 条原始行；
- `merges.py`：手工合并的 20 条，以及最终的顺序；
- `build.py`：检查每条原始行恰好用到一次，编号，计算“各类别的条目数”一节的统计，生成 `catalog.tsv` 和 `catalog.md`；
- `helpers/`：检索用的 PowerShell 脚本。

`build.py` 只需要 Python 3 标准库：

```
python docs/surveys/pitfalls/build.py out    # 写出 out/catalog.tsv 和 out/catalog.md，并打印统计
```

不带参数时写回脚本所在的目录。这样生成的两个文件与目录中的文件逐字节相同。

网页证据不能机械地复现：issue 的状态、反应数和评论数是 2026-10-23 读取的，之后会变；GitHub 检索和网页搜索的结果也会变。`helpers/` 中的脚本记录了检索的方式，需要已登录的 GitHub CLI（`gh`）：

| 脚本 | 用途 |
|---|---|
| `gq.ps1` | GraphQL 检索 issue 和 PR，按反应数排序 |
| `gs.ps1` | GraphQL 检索，同时列出创建日期 |
| `egb_search.ps1` | REST 检索，按反应数排序，遇到限流时重试 |
| `iss.ps1` | 一个 issue 或 PR 的状态、标签、重复关系、交叉引用和关闭它的 PR |
| `csa_dup.ps1` | 只查重复关系和交叉引用 |
| `dump.ps1` | 转储一个仓库中带某个标签的全部 issue，用于上文的 topic:gdscript 转储 |
| `freq.ps1` | Stack Overflow 的问题总数和 forum.godotengine.org 的主题数 |

```
pwsh docs/surveys/pitfalls/helpers/dump.ps1 godotengine/godot topic:gdscript godot_gdscript.tsv
pwsh docs/surveys/pitfalls/helpers/dump.ps1 godotengine/godot-proposals topic:gdscript proposals_gdscript.tsv
pwsh docs/surveys/pitfalls/helpers/gq.ps1 'repo:godotengine/godot "previously freed" in:title'
pwsh docs/surveys/pitfalls/helpers/iss.ps1 godotengine/godot 21789
```

转储文件较大，而且只反映读取当天的状态，没有收入仓库。研究者阅读文档时用的文档源文件副本另有许可，也没有收入；目录中的文档引文都给出了页面地址。引擎事实可以检出 Godot 提交 `469659e` 后按 `path:line` 核对。

## 在规范中的引用

[语言规范](../language-spec.zh.md)引用了以下条目：G-02（§6.2），G-03 和 G-05（§6.4），G-04（§6.3），G-09（§6.6），G-10（§6.7），G-11（§9.7），G-12 和 G-13（§9.5），G-19、G-20 和 G-34（§7.6），C-17（§11.2）。
