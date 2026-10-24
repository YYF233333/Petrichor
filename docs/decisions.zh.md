# Petrichor 设计决定

**2026-10-23** · English: [decisions.en.md](decisions.en.md)

项目从 2026-10-23 起从问题本身重新设计。本文记录已经确定的决定，每条先写规则，再写理由和依据。v0.1 规范已冻结为 git 标签 `spec-v0.1`，与本文冲突之处以本文为准；新规范将按本文重写。文中的引擎源码位置都基于 Godot 提交 `469659e`。

## 1. 问题与定位

- **问题**：用动态语言写的大型项目难以管理。GDScript 受历史兼容性约束，摆脱不了动态类型；更根本的是工具链：场景中的静态信息到不了脚本里，即使脚本全部写成静态类型，节点路径、编辑器中连接的信号、动画轨道和资源属性名仍然是动态的。改用 C# 则语言支持降为二等，桥接有暗坑，部分引擎能力受限，工具链与 Godot 的结合也有坑。
- **目标**：全静态类型，只在引擎或第三方强加的地方出现动态值；这种体验延伸到场景。
- **目标用户**：会读、会改代码的开发者，项目规模中等以上，部分代码可以由 AI 生成。小项目用 GDScript 更合适，不是目标。
- **使用方式**：以开新项目为主；第三方插件保持 GDScript，不要求迁移。插件要求继承它的 GDScript 类时（如 Beehave 的 ActionLeaf），第一版由用户手写一层薄的 GDScript 子类，在其中调用本语言的代码；GDScript 通过反射看到本语言的类型，这些调用仍有静态检查（`modules/gdscript/gdscript_analyzer.cpp:804-819`）。语言层面的跨语言继承依赖 GDScript 的内部实现，不做。依据：[暗坑清单](surveys/pitfalls.zh.md) C-17。反方向同样不可行：GDScript 解析 `extends 全局类名` 时，把基类文件当作 GDScript 解析（`modules/gdscript/gdscript_analyzer.cpp:471-487`），所以 GDScript 脚本不能继承本语言的类，而 GDScript 此时只报“Could not resolve super class”（`:479, 485`）。因此迁移一个类时，它仍为 GDScript 的子类必须一起迁移。分析服务在继承了本语言类的 GDScript 文件上报错并说明原因；迁移一个类之前，列出受影响的子类，并提供一起迁移的选项。依据：[第二次纸面迁移](surveys/paper-migration-2.zh.md)中，Ring 的 Brick 和 Enemy 因此连带迁移了 5 个子类。
- **性能**：明显快于 GDScript 即可，不以 C# 为目标。多数脚本碰不到解释器的性能瓶颈。

## 2. 交付与工具链

- **交付形态为 GDExtension。** 访问引擎的代码集中在一层薄的适配层中，保留以后改编为模块的可能。理由：[暗坑清单](surveys/pitfalls.zh.md)中没有只有模块才能修复的条目，模块修不了的部分 GDExtension 同样修不了；GDExtension 可以装进官方编辑器，使用官方导出模板。脚本语言所需的钩子已对扩展公开（`core/object/script_language_extension.h:393, 493, 533, 596, 754`）。代价：Web 导出须启用“Extensions Support”，并配置跨域隔离响应头。
- **分析服务运行在编辑器进程中。** 外部 IDE 通过 LSP 连接；命令行和 CI 以无界面模式运行同一套分析。理由：跨场景的重构要修改编辑器正打开着的场景，只有编辑器能安全地做到。GDScript 的 LSP 同样是编辑器插件（`modules/gdscript/language_server/gdscript_language_server.h:35`，`modules/gdscript/register_types.cpp:170`）。
- **重构包含场景中的那一半。** 机械且不改变含义的修改随重构自动完成，事先预览，可以撤销，包括改名所涉及的导出属性键、信号连接的方法名、动画轨道路径，以及无损的值转换。需要判断的修改只报告并给出选项，不自动执行，例如改变节点类型、现有资源不再符合新类型。理由：目前导出成员改名后，场景中保存的值会静默丢失。加载时不检查 `node->set` 是否成功（`scene/resources/packed_scene.cpp:567`），保存时只写节点当前属性列表中的属性（`packed_scene.cpp:941-950`）。
- **第一版工具**：
  - 实时诊断，编辑器、LSP 和命令行使用同一套分析；
  - 补全、悬停显示类型、跳转到定义、查找引用（包括场景）；
  - 带场景部分的重命名；
  - 场景检查（`$X`、信号连接、`PackedScene[T]`、父节点要求），错误标在场景编辑器中；
  - 快速修复：插入推断出的类型、补全 match 分支、为动态值加显式转换；
  - 接入引擎调试界面的调试器：断点、单步、局部变量、调用栈；
  - 自带的测试运行器：发现、运行、报告，可无界面运行。理由：gdUnit4 和 GUT 要求测试类继承它们的 GDScript 基类，而 Godot 不支持跨语言继承。
- **以后再做**：格式化；提取方法、修改签名、把成员提到基类等重构；性能分析的深度集成；文档生成；为要求继承的 GDScript 插件类生成转发用的 GDScript 外壳。
- **热重载分两级。** 只修改函数体时，运行中直接替换，下一次调用使用新代码，正在挂起的协程用旧版本执行完。修改成员、签名或类结构时不迁移状态，提示需要重新运行；编辑器中的 tool 脚本重建实例。

## 3. 场景与节点

- **节点类型以场景为准推断。** 编译器收集脚本能看到的全部场景结构，包括继承场景和实例上的修改，为 `$X`、`%X` 和字面量 `get_node` 定类型。不同结构给出不同类型时报错，要求标注。依据：在[场景结构调查](surveys/scene-structures.zh.md)的游戏仓库中，2,023 处字面量路径没有一处在不同结构中解析为不同类型。
- **三种情况由脚本声明**：基类要求、由继承场景提供的子节点；运行时添加的子节点；可选的子节点，类型为 Option。其余找不到的路径都是错误，标在缺少该节点的场景上。
- **向上访问。** `/root/X` 形式的 autoload 访问改用有类型的 autoload 全局名。父节点和 owner 的要求写作类级注解 `@parent(T)`、`@owner(T)`，与 `@tool` 放在同一位置；`get_parent()`、`..` 和 `owner` 因此具有类型 T，未声明时为 Node，转为具体类型须显式转换。在场景中放置该脚本的位置静态检查，owner 是包含该节点的场景的根节点；挂在场景根上的脚本没有 owner，声明 `@owner` 是编译错误。运行时，父节点要求在节点挂到父节点时检查（`NOTIFICATION_PARENTED`，`scene/main/node.h:466`）；`owner` 在每次读取时检查不为 null 且符合要求，失败是程序错误，诊断写明要求和实际的 owner。不在移动节点时检查 owner：节点被移出 owner 的子树时，引擎把 owner 置为 null（`scene/main/node.cpp:391-396, 1799-1801`），这是合法的操作，例如 Ring 的 DeathComponent 先读 owner，再把自己移到前景层。需要判断有没有 owner 的代码使用 `get_owner()`，其类型为 `Node?`。
- **场景资源。** `preload` 和字面量路径的 `load` 按资源的实际内容定型。导出的场景写作 `PackedScene[T]`，由编辑器检查所填的场景；`instantiate()` 的结果为 T。运行时才确定路径的 `load` 得到 Node，须显式转换。
- **非空的导出引用必须赋值。** 按场景的使用方式检查：场景被其他场景实例化或继承时，沿实例和继承链在任何一层赋值都算数，每个实例分别检查，缺值标在该实例所在的场景上；场景作为根使用时（主场景、以字面量路径 `load` 或 `preload` 的场景、作为 `PackedScene[T]` 导出的场景），检查场景本身；代码中 `instantiate(...)` 的场景，按调用处传入的值检查（见下一条）；没有被任何地方引用的场景只给警告，因为运行时才确定路径的 `load` 看不到。例如 player.tscn 的 `crosshair` 指向只存在于 main.tscn 的节点，由 main.tscn 在实例上赋值，player.tscn 本身不报错。检查分三个时机：编辑时允许清空，Inspector 中的字段和场景树中的节点立即标出错误，但不阻止任何操作，重新赋值后标记消失；运行时允许运行，缺值的节点就绪时报程序错误，检查早于 `@onready` 初始化，诊断写明成员名，例如在编辑器中单独运行 player.tscn 时；导出发布版本时，确定会缺值的不允许导出，只有警告的不阻止。代码中创建的节点，须在就绪前赋值。就绪前读取尚未赋值的成员是程序错误。Resource 没有就绪阶段：`.tres` 中的赋值静态检查，代码中 `new()` 出来的在读取时检查。远程检查器向非空成员写入 null 时，入口检查拒绝写入，原值不变。确实可能缺值的成员写作 `T?`。理由：编辑过程中清空再替换是正常操作，非空约束的是编辑完成的场景；替换本身不需要先清空，节点引用在 `.tscn` 中以路径保存、实例化时解析（`scene/resources/packed_scene.cpp:449-468`）。
- **构造参数表达为必须赋值的成员。** Node 和 Resource 脚本的 `_init` 不能有参数，因为引擎总以零个参数构造它们（[暗坑清单](surveys/pitfalls.zh.md) G-02）。没有默认值的非空成员必须在所在场景就绪完毕之前赋值：导出的可以由场景提供，其余由创建者在就绪前提供，也可以由同一场景中的祖先节点在自己的 `_ready` 中赋值，只要场景模型看得见这次赋值（`$Child.target = v`，或经 `@onready` 引用赋值）。由祖先赋值的成员在整个场景就绪之后检查。就绪在节点入树时同步传播完毕（`scene/main/node.cpp:3425-3429`），每帧回调由主循环发起，所以正常逻辑读不到空值。子节点在自己的 `_ready` 中读取这类成员，是编译错误。场景模型看不见的配置方式（例如遍历 `get_children()` 赋值），成员须写作 `T?`。上一条的检查同样适用于这些成员。`instantiate(...)` 和 `new(...)` 接受按名字传入的成员值，在返回前完成赋值。对 `PackedScene[T]`，编译器在调用处检查场景没有提供的必须赋值成员都已传入，例如 `BulletScene.instantiate(speed = 300.0, target = enemy)` 漏写 `target` 是编译错误。必须赋值的成员若不带 `pub`、又没有场景或祖先为它赋值，在类的定义处报错。运行时才确定路径的场景只有就绪时的检查。只由本语言代码创建的 RefCounted 类照常使用带参数的 `_init`。autoload 比场景活得久，引用场景节点的成员只能写作 `T?`；需要非空时，把管理器作为场景中的节点，用必须赋值的导出成员引用其他节点。第一版不提供“就绪后才赋值的非空成员”（如 Kotlin 的 `lateinit`），以后需要时可以加入。依据：20 个仓库中，在 `_ready` 里把节点登记到 autoload 成员上（`Autoload.x = self`）的写法只有 6 处，其中 5 处在 Ring（[第二次纸面迁移](surveys/paper-migration-2.zh.md)）。
- **`@onready` 沿用 GDScript 的时机**：初始化器在 `_ready` 之前执行，先基类后派生类，各自按声明顺序。以下情况是编译错误：`@onready` 与 `@export` 同时使用；初始化器中使用 await；在 `_init` 或 `_enter_tree` 中直接读取 `@onready` 成员。经调用间接读到尚未初始化的 `@onready` 成员是程序错误，诊断写明成员名。依据：[暗坑清单](surveys/pitfalls.zh.md) G-04。
- **子节点先于父节点就绪，沿用引擎顺序，不新增生命周期钩子。** 子节点在 `_ready` 中读到父节点尚未初始化的成员时，报程序错误并说明原因；快速修复给出两种改法：由父节点在自己的 `_ready` 中配置子节点，或把子节点的 `_ready` 声明为 async 并 `await get_parent().ready`。子节点在运行时加到已就绪的父节点下时，这样的读取是合法的，因此不在编译期一概报错。节点重新入树时不再执行 `_ready`，与引擎一致。依据：[暗坑清单](surveys/pitfalls.zh.md) G-03、G-05。
- **初始化器直接写字段，不经过 setter**，实例成员和静态成员都是如此，与 GDScript 当前行为一致（`modules/gdscript/gdscript_compiler.cpp:2435-2451, 2620-2643`）。初始化器给出第一个值，setter 处理值的变化；初始化时没有观察者，后声明的成员也尚未初始化。派生状态在 `_ready` 或 `_static_init` 中计算；成员同时有初始化式和 setter 时，编译器提示初始化不经过 setter。
- **静态成员**沿用 GDScript 的写法和时机：`static var`、`static func`、`_static_init()`；类加载时先基类后派生类，按声明顺序初始化静态成员，最后调用 `_static_init`（`gdscript_compiler.cpp:2646-2649`）。一个类在静态初始化中读取另一个类的静态成员时，先完成那个类的初始化；静态初始化之间形成环是程序错误，诊断列出依赖链，能静态判断的在编译期报错，不读取默认值或未完成的值（GDScript 的相关问题见 #104161、#82141、#104588）。静态初始化不能 await。修改静态成员属于结构变化，热重载时提示重新运行。理由：语料中 `static var` 约 214 处、`_static_init` 4 处，接触频率低。

- **资源共享的语义不变，由场景模型发现问题。** 编译器知道哪些资源是共享的：`load`、`preload` 和导出属性得到的资源来自缓存；场景中没有设为“局部于场景”的内嵌子资源由该场景的所有实例共用。对已知共享的资源写属性或调用会修改它的方法时给出警告，列出共享它的位置，并提供两个快速修复：在场景中把它设为局部于场景，或在修改前 `duplicate()`。有意修改共享资源时用 `@warning_ignore` 关闭；运行时才替换的资源追踪不到，不警告。`duplicate()` 返回接收者的静态类型；复制深度用 `duplicate_deep(模式)` 写明。`preload`、导出的 PackedScene 和资源之间的引用环在运行前报告，并建议把其中一条边改为运行时 `load`。依据：[暗坑清单](surveys/pitfalls.zh.md) G-19、G-20、G-34。例子：godot-open-rpg 的 `interaction_popup.tscn` 内嵌的 CircleShape2D 没有设为局部于场景，`interaction_popup.gd:30` 的 setter 修改它的半径，而 `main.tscn:484-486` 中只有一个实例把 radius 设为 64；按引擎的共享规则推断，十几个实例共用同一个形状，半径取决于最后执行的 setter（未运行验证）。

## 4. 类型系统

- **函数签名的类型手写，不推断**；函数内部的局部变量可以推断。
- **成员变量可以由初始化式推断类型**，规则与局部变量相同，例如 `var speed = 5.0` 为 float，`@onready var label = $Label` 按场景推断为 Label。没有初始化式的成员，以及推断不出类型的成员（空容器、None），必须标注。推断出的类型出现在悬停、补全和引擎看到的属性信息中。GDScript 中 `var hp = 10` 是无类型成员；在这里推断为 int 后，赋入其他类型是编译错误，符合语法硬规则。
- **数值。** 第一版只有 `int`（64 位）和 `float`（64 位），与 Variant 一致（`core/variant/variant.h:257-258`）；没有用户可用的窄类型，固定位宽的运算使用 `int` 上的回绕运算和位运算方法。写入引擎的 f32 位置时隐式收窄，例如标准构建中为 float 的 real_t 向量分量（`core/math/math_defs.h:143-146`）。传给引擎 int32 参数时做范围检查，越界是程序错误；引擎自身在这里会直接截断（`core/variant/variant.h:348-349`）。`int` 与 `float` 混合运算时提升为 `float`。把 `float` 赋给 `int` 必须写 `int(x)` 显式转换，否则是编译错误；隐式截断会让 `hp = 2.5` 与 GDScript 悄悄不同。
- **运算的结果类型**按引擎的运算符表确定，与 GDScript 的静态检查相同（`core/variant/variant_op.cpp:256-368`）。例如 `Vector2 * float` 得到 Vector2，`Vector2i * float` 得到 Vector2，`int / int` 是整除，表中没有的组合（如 `Vector2 + Vector2i`）是编译错误。整数除以 0 是程序错误。
- **收窄**采用 Kotlin 的规则：局部变量和参数在 `!= None`、`is` 和提前返回之后收窄，条件表达式 `a if c else b` 的分支以及 `and`、`or` 的右侧同样按条件收窄；成员不收窄，须先读入局部变量。没有真值判断，条件必须是 bool。非空不代表对象仍然存活，访问时照常检查。
- **已释放的对象。** `T?` 类型的引用读到已释放的对象时，结果为 None，因此 `!= None` 检查和收窄同时就是存活检查。非空的 `T` 类型引用可以持有已释放的对象，访问其成员时报程序错误；读取、比较和传参不算访问。debug 构建中，若对象是本语言的实例或由本语言代码释放，诊断写明它在哪里被释放。`is_instance_valid` 与 GDScript 行为相同；`queue_free()` 之后到帧末删除之前，对象仍算存活。理由：语料中 `is_instance_valid` 约每千行 2 处，phantom-camera 中每千行 8 处；引擎从 4.4 起也让已释放的对象在条件中为假（PR #93885）。代价是每次读取 `T?` 类型的对象引用都要做一次存活查询。依据：[暗坑清单](surveys/pitfalls.zh.md) G-07、G-08。
- **引用环与泄漏。** 不做追踪式 GC，RefCounted 使用引擎的引用计数，环不会被释放。闭包捕获 self 的方式与 GDScript 相同，在 RefCounted 类中是强引用（`modules/gdscript/gdscript_lambda_callable.cpp:285-291`）；按语法硬规则，不悄悄改为弱引用。提供带类型的弱引用 `weak[T]`：`weak(x)` 创建，`.get()` 返回 `T?`；需要弱捕获 self 时，在闭包外写 `var me = weak(self)`，闭包内用 `me.get()`，不需要新的捕获语法。编译器对明显的环给出警告和改写为 `weak` 的快速修复：捕获 self 的闭包存入 self 的成员、self 自身的信号连接或 self 持有的容器；两个 RefCounted 类以强引用类型的字段互相指向。Node 不做引用计数：`new()` 或 `instantiate()` 得到的节点在某条路径上既未加入场景树，也未被返回、保存或释放时，给出警告；`remove_child` 之后既未保存也未释放时同样警告。debug 构建退出时列出泄漏的节点和对象及其创建位置。经引擎容器或其他对象间接形成的环，静态分析发现不了。依据：[暗坑清单](surveys/pitfalls.zh.md) G-10。
- **析构不新增语法。** 继续用 `_notification(what)` 处理 `NOTIFICATION_PREDELETE`，与场景树相关的清理放在 `_exit_tree`，写法和时机与 GDScript 相同；析构顺序沿用引擎。运行时保证 RefCounted 在 PREDELETE 期间可以读写自己的字段、调用自己的方法，不重新给 self 加引用。处理 PREDELETE 的分支中，把 self 传出、存入成员或容器，以及使用 await，都是编译错误；这期间指向 self 的 `weak` 返回 None。理由：语料中 `NOTIFICATION_PREDELETE` 只出现 68 次，多数在 gdUnit4 内部。依据：[暗坑清单](surveys/pitfalls.zh.md) G-09。
- **接口。** 名义接口，类必须显式声明实现了哪些接口。接口可以声明方法、属性和信号，可以有默认方法，默认方法通过接口属性读写状态。属性要求可以由继承来的引擎属性满足，例如 `var global_position: Vector2` 由 Node2D 的同名属性满足；类型必须相同，读写能力相符。只有本语言的类能实现接口，外部对象须显式包装。第一版不做带状态的 mixin。依据：[跨分支重复调查](surveys/cross-branch-duplication.zh.md)中，原生基类不同造成的重复在多数游戏仓库中每千行约 4–10 行；按人工判断，游戏中这类重复的 353 行里有 185 行可以用组合或静态函数消除，适合带状态 mixin 的 102 行中 72 行来自 Ring 一个项目。继承本可消除却没有消除的重复更多，分别是它的 4.9 倍（同一原生基类的无关脚本之间）和 8.2 倍（同一继承链上），但主要来自 tanks-of-freedom-ii 一个仓库。
- **泛型。** 函数、struct、enum，以及只由本语言代码创建的 RefCounted 类可以是泛型；Node、Resource 脚本和 autoload 不能，因为引擎实例化它们时不提供类型参数。约束用接口表达。类型参数在运行时保留。
- **封装与命名。** 不带 `pub` 的成员在声明它的文件内可见，包括文件中的内部类，子类也可以读写、调用和覆盖它们；其他类看不到。对外公开的成员标为 `pub`（写法见 §9）。覆盖时可见性不变：覆盖 `pub` 成员须写 `pub`，覆盖不带 `pub` 的成员不能加 `pub`。以后需要更严的一级时，可以加入 `private` 关键字，不影响已有代码。实现接口的成员必须为 `pub`。`pub` 成员的签名中出现非 `pub` 的类型是编译错误。引擎按名字访问的成员（导出属性、信号、引擎虚函数的覆盖、场景中连接的方法）对引擎不受此限制。依据：[成员可见性调查](surveys/member-visibility.zh.md)中，名字可判定的成员约 25% 在其他文件中被访问。[第二次纸面迁移](surveys/paper-migration-2.zh.md)中，如果子类只能看到 `pub` 成员，102 处 `pub` 中有 29 处只是为了子类；那次调查估计的 7.2% 把 `type`、`update_stat` 一类常见名字归为无法判定，因而偏低。类名使用引擎的扁平全局表，编译器检查重名，包括与 GDScript 插件的类名重复；第一版没有命名空间。

## 5. 动态边界与引擎绑定

- **动态值只有三个来源**：以 Variant 声明的引擎接口，GDScript 插件中没有标注类型的参数和返回值，跨语言传来的消息。
- **能定型的从源头定型。** 引擎实际有约束但没有导出的，我们在自己的绑定元数据中补上；引擎已导出的元数据直接使用。其余动态值进入静态代码时必须显式转换，失败时报错；不做隐式检查。
- **转换。** `x as T` 只用于对象类型，结果为 `T?`，类型不符时为 None，与 GDScript 相同（`modules/gdscript/gdscript_vm.cpp:1665-1666`）；x 的静态类型已是 T 时结果为 T，并提示这个 `as` 多余。`as` 转为内置类型是编译错误：GDScript 在这里按构造函数转换（`gdscript_vm.cpp:1630-1637`），快速修复给出 `T(x)`，它保留这种转换，如 `int("5")`；或者给出 `T.try_from(x)`。`T.try_from(v)` 是严格的显式转换，返回 `Result[T, ConversionError]`，规则与 struct 字段的解码相同，转为 int 时接受恰为整数的浮点数；用 `?` 传播失败，或用 `.unwrap()`、`.expect(…)` 把失败变为程序错误。必须成功的向下转型写作 `(x as T).unwrap()`。以字面量类名过滤的接口按该类定型，例如 `find_children("*", "MeshInstance3D")` 返回 `Array[MeshInstance3D]`；先判断分组、再向下转型的写法，快速修复建议改用 `is`。依据：[第二次纸面迁移](surveys/paper-migration-2.zh.md)中有 16 处 `(x as T).unwrap()`。
- **引擎返回对象的可空性。** 未审核的接口一律可空。审核按语料中的调用频率由高到低进行，结果分三种：非空，类型为 T；只在误用时为空，例如 `get_tree()`，按非空处理，为空时报程序错误；正常情况下可能为空，例如 `get_collider()`，类型为 T?。理由：引擎用 `RequiredResult` 标注的接口声明只有约 50 处，其中 29 处在 Tween。
- **字符串形式的成员名。** `connect`、`emit_signal`、`call`、`call_deferred`、`set_deferred`、`has_method`、`Callable(obj, "m")`、`tween_property` 以及 `"position:x"` 一类属性路径，在目标类型已知时，字面量按静态引用解析：检查名字是否存在及值的类型，参与重命名和查找引用。哪些参数是名字，由我们的绑定元数据标注。非字面量的名字是显式的动态访问。
- **引擎和 GDScript 看到的类型**：

  | 本语言 | 引擎和 GDScript 看到的 |
  |---|---|
  | `int`、`float`、`String`、Node 等类 | 同名类型 |
  | 对象 `T` 和 `T?` | 都是该类；非空由入口检查保证 |
  | `int?` 等可空值 | 无类型的 Variant |
  | 元素可由引擎表示的 `Array[T]`、`Dictionary[K, V]` | 引擎的带类型容器 |
  | 无载荷 enum | int；导出时显示为下拉框 |
  | struct | 按字段名编码的 Dictionary |
  | 带载荷 enum | 按字段名编码的 Dictionary，用一个保留键记录分支名 |

  依据：GDScript 通过 Script 的反射接口为其他语言的全局类定类型（`modules/gdscript/gdscript_analyzer.cpp:804-819, 4329-4370, 6054-6063`）。

- **String 与 StringName。** 两个方向都在需要时隐式转换，与 GDScript 相同；`==` 按内容比较。期望类型为 StringName 或 NodePath 时，字符串字面量在编译期构造为该类型，运行时不再转换；从 String 构造 StringName 需要在全局表的互斥锁下查找（`core/string/string_name.cpp:280-302`）。`&"x"` 和 `^"x"` 前缀照旧。热路径上的运行时转换给出性能提示。

## 6. 数据

- **带载荷的 enum 与穷尽的 match。** 载荷字段必须命名，例如 `Move(target: Vector3)`。理由：新增分支时，编译器列出所有没有处理它的位置；命名字段使编码不随字段顺序改变。
- **struct** 具有值语义，只存在于本语言内部。跨引擎边界（信号参数、Variant、消息）时自动编码为按字段名的 Dictionary，解码时检查。第一版不能 `@export`，也不能存入 `.tres`；需要在编辑器中编辑的数据使用 Resource。
- **容器。** 对外只有 `Array[T]` 和 `Dictionary[K, V]`，API 相同。元素可由引擎表示时，它就是引擎的容器，与引擎共享；元素为本语言独有的类型时使用自己的表示，交给引擎须显式转换，转换生成副本。理由：隐式复制会造出“修改的其实是副本”一类的坑。
- **JSON。** 引擎把 JSON 中的所有数字解析为浮点数（`core/io/json.cpp:390-396`）。解码到 `int` 时，接受恰为整数且在 int64 范围内的浮点数，否则报错，不截断。
- **标准库**不重写引擎已有的功能，只补三类：绑定审核得到的带类型封装，例如把“失败返回 null、错误另取”的接口（如 `FileAccess.open`）封装为返回 Result；本语言类型的配套，包括 struct 与 enum 和 JSON、`var_to_str`、ConfigFile 之间的编码解码，Option 和 Result 上的组合操作，原生容器上接受带类型 lambda 的 map、filter、sort；测试运行器。不做包管理器。

## 7. 错误、协程与信号

- **程序错误**中止本语言的整条调用链，退回引擎调用进来的入口；退出时释放持有的引用，记录错误和调用栈，在编辑器中运行时进入调试器；游戏继续运行。debug 和 release 构建的行为一致。若该入口需要返回值，引擎得到默认值。预期之内的失败用 Result 或 Option 表达。理由：GDScript 出错后按返回类型构造默认值返回，调用方继续执行（`modules/gdscript/gdscript_vm.cpp:3975-3979`）；在静态类型下，这会让代码拿到假值。
- **Option 与 Result** 采用 Rust 的模型：Option 是值，Result 用于控制流。唯一的特殊语法是后缀 `?`，表示短路返回。作用于 `Result[T, E]` 时，遇到 `Err(e)` 当前函数返回 `Err(e)`，否则得到 T；当前函数须返回 `Result[_, E]`，E 相同，不同时先用 `map_err` 转换。作用于 `T?` 时，遇到 None 当前函数返回 None；当前函数须返回 `_?`。在返回 void 的函数中使用 `?`，或在 Option 与 Result 之间传播，都是编译错误。`await f()?` 解析为 `(await f())?`。其余操作都是方法，名字与 Rust 相同，例如 `unwrap`、`expect`（失败时是程序错误）、`unwrap_or`、`unwrap_or_else`、`map`、`and_then`、`or`、`or_else`、`filter`、`map_err`、`ok`、`ok_or`、`ok_or_else`；没有 `!`、`??`、`?.` 运算符。Result 用 `Ok(v)` 和 `Err(e)` 构造，成功值不隐式包装；Option 隐式包装，空值写 None。丢弃 Result 是编译错误，确实要忽略时写 `.ok()`。理由：短路返回改变控制流，需要语法；其余都是对值的变换，用方法就够了，也可以直接参照 Rust 的文档。若同时提供可空链 `?.`，`x?.y` 是从函数返回还是得到 None，就要看 x 的类型。
- **协程在签名上声明**，例如 `async func`；函数体中有 await 而没有声明，是编译错误。调用 async 函数得到 `Task[T]`，任务立即开始，运行到第一次挂起为止；`await f()` 和 `await t` 得到 T。丢弃调用结果是编译错误，确实不等待时写 `f().detach()`。GDScript 中把协程调用的结果当值使用本来就是编译错误，所以 `var t = f()` 获得新含义不违反语法硬规则。引擎虚函数可以声明为 async。
- **任务的结束状态有三种**：完成，得到返回值；取消，由 `t.cancel()`、所属对象被释放或所等待的对象被释放引起；失败，即任务内部发生程序错误。等待被取消的任务是程序错误；失败沿 await 链向上传播到根任务（detach 的任务或被引擎调用的任务），不能拦截。`await t.settled()` 等待任务以任何方式结束而不报错，返回 `Settled[T]`：`Finished(value: T)` 或 `Cancelled(reason: CancelReason)`，`CancelReason` 为 `Requested`、`OwnerFreed` 或 `AwaitedFreed`。不用 Result 表达，因为被取消不是错误。
- **取消。** 对挂起中的任务，`cancel()` 立即断开它等待的信号或任务，任务结束为取消。对正在执行的任务（只在它自己的同步代码间接取消自己时出现），执行到下一个 await 或返回时结束为取消，返回值丢弃。对已结束的任务没有效果。调用 `cancel()` 之后，结果一定是取消。取消沿归属传播：任务以 `await f(...)` 亲自启动并直接等待的子任务归它所有，随它一起取消；`await` 一个从别处取得的任务值时，取消只是停止等待，那个任务照常运行。两种写法在编译期即可区分。例如过场 `cutscene` 停在 `await walk_to(door)` 时被取消，`walk_to` 随之取消，角色停下。
- **释放。** 协程帧（局部变量和挂起点）在任务结束时立即释放，持有的引用按作用域退出处理，await 之后的代码不再执行；第一版没有 finally 或 defer。Task 对象是只保存结果的 RefCounted 句柄，变量、等待方以及等待信号期间的运行时都不再引用它时释放。
- **组合与重入。** `Task.of(信号)` 把信号包装为任务；`Task.delay(秒)` 计时；`Task.any(列表)` 返回第一个完成的任务的下标，并按上述归属规则取消其余任务；`Task.all(列表)` 等待全部完成。重入不做自动策略，需要时用句柄取消旧调用；以后可以兼容地加入 `@restart` 一类注解。依据：[暗坑清单](surveys/pitfalls.zh.md) G-12、G-13；提案 #8838、#6243。
- **setter 和 getter 不能是协程。**
- **就绪前的 setter 自动挂起。** setter 照常立即执行；执行到第一处需要节点就绪的语句时，若节点尚未就绪，就在此挂起，就绪后从这里继续。需要就绪的语句由静态分析确定：访问 `@onready` 成员、`$X` 或子节点，或者调用会访问它们的本类方法（包括子类的覆盖）。写在 `if is_node_ready():` 分支里的访问不算。就绪前多次写入时，只有最后一次挂起的调用继续执行，之前的全部取消；挂起的调用在 `@onready` 初始化之后、`_ready` 之前按写入顺序继续；节点在就绪前被释放时，挂起的调用随之丢弃。就绪以后 setter 不再挂起。挂起点的位置在编辑器中悬停可见。理由：这就是 GDScript 中手写 `if not is_node_ready(): await ready` 的写法，由编译器自动完成，用户不需要新的概念。变换和存储在挂起点之前完成，所以就绪前读到的是变换后的值；只推迟整个 setter、记下原始输入的做法会让就绪前读到未经变换的输入（例如 Ring 的 `Runtime/AVGRuntime/UI/UI.cs:16-35` 给角色名加【】）。抽查语料中 6 处 `await ready` 的 setter，恢复后都读取字段的当前值（[属性访问器调查](surveys/property-accessors.zh.md)）。
- **值只存在节点中的属性**（没有字段，getter 读取子节点）在就绪前读取是程序错误，诊断写明读取了哪个尚未就绪的节点。需要就绪前读取时，用字段保存。
- **从未被写入的属性，就绪时不执行 setter**，与 GDScript 一致。导出属性的 setter 会同步节点、而默认值从不经过 setter 时，给出提示。
- **跨语言 await，两个方向都支持。** 对 GDScript 方法的调用可以写 await：运行时得到 `GDScriptFunctionState` 时等待其 `completed` 信号，再按声明的返回类型检查结果；没有写 await 却得到它时，报程序错误。协程性不在方法的反射信息中（`core/object/method_info.h:37-44`），因此无法在编译期判断。GDScript await 本语言的协程时，协程第一次挂起返回其 Task 的 `completed` 信号。Task 完成时发出 `completed(值)`；取消或失败时发出 `completed(null)`，使 GDScript 的等待方不会永远挂起，另有 `cancelled` 信号供需要区分的代码使用。两个方向都须在能编译引擎的环境中实测。
- **对象释放与挂起的协程。** 协程所属的对象被释放时，协程静默取消，挂起帧持有的引用随之释放，与 GDScript 相同。被等待的对象（另一个对象上的协程，或另一个对象的信号源）在完成之前被释放时，等待方在 await 处报程序错误并中止到引擎边界，诊断写明被释放的对象及释放位置（用 `settled()` 等待时得到 `Cancelled(AwaitedFreed)`）；GDScript 在这种情况下让等待方永远挂起（`modules/gdscript/gdscript_function.cpp:335-349`）。挂起的协程不使 self 保持存活，与 GDScript 相同。任何对象的释放都可以通过 GDExtension 公开的实例绑定释放回调感知。依据：[暗坑清单](surveys/pitfalls.zh.md) G-11。
- **信号与函数值。** 信号参数必须标注类型；emit 和 connect 都做静态检查；场景中的连接由场景模型检查；引擎类的信号按 ClassDB 的信息定型，未审核的对象参数可空。函数类型记录参数、返回值和是否为 async。lambda 按值捕获，与 GDScript 相同。签名未知的 Callable 须显式转换为函数类型，每次调用时检查。

## 8. 运行时

- **解释器须独自达到性能目标。** iOS 和多数主机平台不允许运行时生成机器码；copy-and-patch 一类的 JIT 只是桌面端的额外收益。
- **线程。** 虚拟机必须能在引擎选定的任意线程上安全运行，例如后台加载资源的线程和物理线程。共享对象上的数据竞争由用户负责。第一版不新增线程 API，使用引擎的 Thread、WorkerThreadPool 和 Mutex。依据：调查所用的 20 个仓库中，`Thread.new` 共 11 处，`WorkerThreadPool` 2 处。

## 9. 语法

- 表层沿用 GDScript：缩进、`func`、`var`、`extends`、`signal`、`@export`、`$X`。新增的构造按同一风格扩展，包括 struct、带载荷的 enum、match、interface、泛型 `[T]` 和 `T?`。
- **硬规则**：与 GDScript 写法相同的构造，行为要么与 GDScript 一致，要么编译报错并给出改法，不能悄悄不同。
- **声明的写法**：
  - struct：`struct Hit:` 之下用 `var` 声明字段，字段可以有默认值。构造时只能按名字传参，例如 `Hit(damage = 3, point = p)`，有默认值的字段可以省略；以后放开位置参数不会破坏已有代码。
  - 带载荷的 enum 扩展 GDScript 的花括号写法，例如 `enum Command { IDLE, MOVE(target: Vector3), ATTACK(target: Node3D, power: int) }`，构造写作 `Command.MOVE(target = p)`。无载荷的 enum 含义不变。
  - interface 是缩进块，可以声明 `var`、`signal` 和 `func`。没有函数体的方法不写冒号，与 GDScript 的 `@abstract func` 相同（`modules/gdscript/gdscript_parser.cpp:1790-1791`）；有函数体的是默认方法。接口的成员都是公开的。
  - 类头用 `implements A, B` 声明实现的接口，可以与 `extends` 写在同一行，例如 `class_name Player extends CharacterBody3D implements Damageable`，也可以单独一行。不并入 `extends`，使基类与接口一眼可分。
  - `pub` 是关键字，写在注解之后、`static` 之前，例如 `@export pub var max_hp: int`、`pub static func`、`pub signal died`。用关键字而不用注解，因为它写得频繁：[成员可见性调查](surveys/member-visibility.zh.md)中每千行至少 22 处。
  - 放置与命名沿用 GDScript：一个文件就是一个类，文件中声明的 struct、enum 和 interface 都属于这个类。在本文件中直接写名字，在其他文件中写 `类名.名字`，例如 `Combat.Hit`、`Player.Command.MOVE(target = p)`；没有 `class_name` 的文件经 `preload` 访问。全局名只来自 `class_name`。几个类共用的类型放进一个专门承载它们的类。以后可以加入别名或 `import`，不改变现有写法的含义。
- **表达式。** 条件表达式只有 GDScript 的 `a if c else b`，条件必须是 bool；不提供以 if/else 块为值的写法。后缀 `?` 的含义见 §7；GDScript 中 `?` 目前是语法错误（`modules/gdscript/gdscript_parser.cpp:3834-3835`）。
- **match** 保留 GDScript 的全部模式，含义不变：字面量和常量表达式按相等比较，`var x` 绑定，`_` 通配，数组和字典模式及其中的 `..`，逗号分隔的多个模式（其中不能绑定），`when` 守卫（`modules/gdscript/gdscript_parser.cpp:2499-2723`）。新增的模式：
  - enum 分支写成 `类型.分支(字段 = 模式, ...)`，`var 字段` 是 `字段 = var 字段` 的简写，例如 `Command.MOVE(var target)`、`Command.ATTACK(target = var enemy, power = 0)`。未列出的字段须由 `..` 覆盖，与字典模式相同。模式可以嵌套。struct 用同样的写法，例如 `Hit(damage = var d, ..)`。
  - `Ok(var v)`、`Err(var e)` 按位置书写，与构造一致。Option 用 `None` 和 `var x` 两个分支，后者绑定的值已收窄为 T。
  - 对 enum（无论有无载荷）、Result 和 Option 的 match 必须穷尽或带 `_`，带 `when` 的分支不计入覆盖；对其他类型不要求穷尽，与 GDScript 相同。
  - 第一版没有类型模式（用 `if x is T`）、match 表达式，也不能在逗号分隔的多个模式中绑定；以后加入不改变现有写法的含义。
- **for** 的写法与 GDScript 相同：`for i in n` 遍历 0 到 n-1，i 为 int；`range(...)`、容器、String、Dictionary 的键都可以遍历；循环变量可以标注类型，如 `for i: int in ...`。遍历 float 和向量是编译错误，提示改用 `range(...)`；GDScript 允许这样写，把向量当作范围（`modules/gdscript/gdscript_vm.cpp:3093-3230`），在静态类型下这几乎总是误用。

## 10. 尚未决定

- 成员不收窄的代价：在纸面迁移中测量。可以兼容地放开的方向有两个：条件绑定，例如 `if var t = _tween:`；检查与使用之间没有调用、await 或赋值时，让没有 getter 的字段收窄。

## 依据

调查：[场景结构](surveys/scene-structures.zh.md)、[跨分支重复](surveys/cross-branch-duplication.zh.md)、[暗坑清单](surveys/pitfalls.zh.md)、[纸面迁移](surveys/paper-migration.zh.md)、[属性访问器](surveys/property-accessors.zh.md)、[await 信号结果](surveys/await-signal-results.zh.md)、[成员可见性](surveys/member-visibility.zh.md)。
