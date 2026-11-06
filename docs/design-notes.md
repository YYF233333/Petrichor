# 设计说明与取舍

本文是非规范性的配套文档，收纳从[语言规范](language-spec.zh.md)移出的三部分内容：

- 第一版之后的完整特性设计；
- 各节规则背后的详细理由与语料计数；
- 考虑过但没有采用的方案。

语言规则以语言规范为准。引擎行为的出处见[引擎事实](engine-facts.md)。

## 1. 第一版之后的设计

### 1.1 用户定义的泛型（规范 §2.4、§3.10）

- GDScript 没有用户定义的泛型，迁移来的代码（含 FPS 和 Ring）不会用到。相关语法在第一版中都是编译错误，所以以后加入是兼容的扩展。
- **可泛型的声明**：函数、struct、enum，以及不继承 Resource 的 RefCounted 类。Node、Resource 脚本和 autoload 不能声明类型参数，因为引擎实例化它们时不提供类型实参。泛型类不登记进引擎的全局类表，GDScript 看不到它，因为 GDScript 给不出类型实参。
- **语法**：类型参数写在名字之后的方括号中，与 `Array[int]` 一致，写法同 Python 3.12（PEP 695）。约束写作 `T: 接口`。文件的类在 `class_name` 上声明类型参数，例如 `class_name Cache[K, V] extends RefCounted`。内部类写作 `class Cache[K, V] extends RefCounted:`。GDScript 在内部类名、`class_name` 的名字、enum 名和函数名之后遇到 `[` 都是语法错误（[引擎事实](engine-facts.md)）。因此这些写法不违反语法硬规则。

  ```gdscript
  func first[T](items: Array[T]) -> T?:
      return None if items.is_empty() else items[0]

  struct Pair[A, B]:
      var first: A
      var second: B

  enum Tree[T] { LEAF, NODE(value: T, children: Array[Tree[T]]) }

  func weakest[T: Damageable](units: Array[T]) -> T?:
      var best: T? = None
      for u in units:
          if best == None or u.hp < best.hp:
              best = u
      return best
  ```

- **类型实参与推断**：调用处通常由实参推断。推断不出时须显式写出（如 `first[int]([])`、`Pair[int, String](first = 1, second = "a")`），否则如 `var x = first([])` 报编译错误。`名字[...]` 与下标同形，由名字解析区分。名字是泛型函数或泛型类型时，方括号中是类型实参，否则是下标。
- **约束与运行时保留**：约束用接口表达。初版泛型每个类型参数至多一个约束，不满足约束时报编译错误。之后可按同一写法扩展到多个约束、以类作约束，以及泛型接口 `interface Source[T]:`（规范 §3.9）。类型实参在运行时保留（实现架构 §6.1）。
- **反射与 Callable**：泛型方法不经反射暴露（规范 §10.5），因为引擎和 GDScript 按名字调用时给不出类型实参。引擎必须按名字找到的方法不能是泛型。作为 Callable 使用时须确定类型实参（如 `sig.connect(handle[int])`），确定不了时报编译错误。

### 1.2 就绪前自动等待就绪的 setter（规范 §6.5）

- [属性访问器调查](surveys/property-accessors.md)中有 25 个 setter 用 `await ready` 等待节点就绪。第一版中 setter 立即执行，就绪前访问 `@onready` 成员或子节点是程序错误。以后可兼容地放宽为由编译器自动挂起（是否采用见 `BACKLOG.md` 待定问题）：
  - setter 执行到第一处需要节点就绪的语句时，若节点尚未就绪，就在此挂起，就绪后从这里继续；
  - 需要就绪的语句由静态分析确定，包括：
    - 访问 `@onready` 成员、`$X` 或子节点；
    - 调用会访问它们的本类方法，含子类覆盖；
  - 只有在 `is_node_ready()` 为真时才能执行到的访问不算。方法中的这类访问全部受保护时，调用它也不算需要就绪。只认 `is_node_ready()`，因为 `is_inside_tree()` 为真时 `@onready` 成员仍可能尚未初始化。判断规则与收窄相同（规范 §5.4），受保护的位置包括：
    - `if is_node_ready():` 分支内；
    - `if not is_node_ready(): return` 之后；
    - `is_node_ready() and …` 右侧；
  - 挂起点的位置在编辑器中悬停可见；
  - 同一属性在就绪前再次被写入时，之前挂起的调用立即取消，不论新写入是否挂起。挂起的调用在 `@onready` 初始化之后、`_ready` 之前按写入顺序继续。节点在就绪前被释放时，挂起的调用随之丢弃。就绪以后 setter 不再挂起；
  - 挂起点之前的语句照常执行。setter 通常先变换并存储值、再同步节点，这样就绪前读到的是变换后的值。若推迟整个 setter 而只记下原始输入，就绪前会读到未变换的值。例如 Ring 给角色名加【】（`Runtime/AVGRuntime/UI/UI.cs:16-35`）；
  - 用户不需要新概念。setter 中手写的 `await ready` 仍是编译错误，快速修复将其删去。

## 2. 各节理由与语料依据

### §1–§2 交付、词法与表层语法

- **GDExtension 与只发字节码（§1.3）**：[暗坑清单](surveys/pitfalls.md)中没有只有引擎模块才能修复的条目。GDExtension 修不了的部分需要修改引擎，模块同样修不了。`$X`、`@parent` 和 `PackedScene[T]` 的类型取决于场景（§7）。若在加载时编译源码，就要在游戏进程内重新分析场景，并把分析器带到所有导出平台。
- **preload 脚本作为类（§2.1）**：[语料计数](surveys/corpus-counts.md)中 preload `.gd` 文件 351 处，其中 207 处初始化常量。直接跟 `.new()` 的 preload 有 69 处。初始化出的常量在同一文件中的用途：
  - 用于 `new()` 54 处；
  - 用于其他调用 124 处；
  - 用于其他成员 134 处；
  - 用在 `is` 之后 69 处；
  - 作为类型 46 处。
- **`null` 别名与单行引号字符串（§2.2）**：[语料计数](surveys/corpus-counts.md)中 `null` 每千行 22.2 处。改为编译错误只会增加机械迁移工作。单引号和双引号跨行字符串在语料中只有 97 个，其中 95 个在 `godot_dialogue_manager` 的测试中。禁止跨行可在漏写右引号时把错误报在本行，避免吞掉后续代码。
- **`###` 注释不是文档（§2.2）**：[语料计数](surveys/corpus-counts.md)中，紧贴在声明上方、全由 `###` 开头的行组成的注释有 41 段。它们都是 `### Public variables ###` 这样的分节标题或分隔线，GDScript 却把它们显示为该声明的文档。没有一段文档把这样的行和其他行混写。Rust 同样把 `////` 当作普通注释。
- **第一版不支持 `@tool`（§2.3）**：在编辑器中运行脚本需要在加载时完整编译。它还要求在编辑器中实例化和重载脚本。[语料计数](surveys/corpus-counts.md)中 326 个含 `@tool` 的文件有 211 个在插件仓库。游戏仓库中有 58 个，其中 31 个集中在 `godot-open-rpg`。
- **`pub` 用关键字（§2.4）**：[成员可见性调查](surveys/member-visibility.md)中每千行至少出现 22 处，写成 `@public` 注解过于冗长。

### §3 类型

- **`float` 不隐式转 `int`（§3.14）**：`var hp = 10` 在本语言中推断为 `int`（§4.3），在 GDScript 中则是无类型变量。GDScript 中 `hp = 2.5` 之后值为 `2.5`。若隐式截断，结果会悄悄不同。
- **对象可空与已释放对象（§3.3）**：[语料计数](surveys/corpus-counts.md)中 `is_instance_valid` 每千行 3.3 处，`phantom-camera` 中每千行 12.8 处。Godot 4.4 起也让已释放对象在条件中为假（[godotengine/godot#93885](https://github.com/godotengine/godot/pull/93885)）。相关问题见[暗坑清单](surveys/pitfalls.md) G-07、G-08。
- **嵌套 Option 保留两层（§3.4）**：若合并为一层，泛型代码的行为会随 `T` 改变。例如 `first[T]` 在 `T` 为 `Enemy?` 时分不清空数组和首元素为空。通用缓存也会把存入的 `None` 当作未命中。JSON 中缺少的键与 `null` 同样无法区分。本语言没有 `?.`，嵌套基本只出现在泛型和容器取值上。
- **struct 方法只读与默认运算（§3.6）**：修改 `self` 的方法要求调用处可写回。在临时值、经 getter 读出的属性和循环变量上，这样的调用要么报错，要么悄悄改副本。解码忽略多余键，使较新发送方多带字段的消息或面向多读取方的 JSON 仍能解码。默认提供 `==`、`<` 和哈希，以像脚本语言一样开箱即用。
- **赋值的求值顺序（§3.6）**：GDScript 先沿链读出副本，再求右边（[引擎事实](engine-facts.md)）。右边挂起或修改了这条链时，写回的旧副本会覆盖期间的修改。例如 `position.x = await slide_x()` 会丢掉等待期间物理过程对 `y` 的修改。本语言采用 Swift 形式访问的顺序：先求位置，再求右边，最后连续读改写。getter 因此在右边之后调用。最后一个下标与其他下标一样在右边之前求值，GDScript 则在之后。修改接收者的方法调用按同一顺序，实参等同于右边。
- **enum（§3.7）**：引擎 enum 开放，是因为引擎会产生枚举外的值（如带修饰键位的 `Key`）。后续 4.x 小版本还会新增枚举值，而同一份构建须继续运行（实现架构 §6.3）。[语料计数](surveys/corpus-counts.md)中：
  - 用 `as` 把 `int` 转为 enum 10 处，忽略 `INT_AS_ENUM_WITHOUT_CAST` 警告 4 处；
  - 把 enum 名当 Dictionary 用 38 处，常量作 enum 别名 3 处；
  - 258 个 enum 中有 8 个没有值 0。
- **容器（§3.8）**：经无类型视图写入或查找不合元素类型的值报程序错误。原因是引擎在写入报错后跳过写入继续运行，查找和 `assign()` 则把 `1.5` 截断为 `1`。元素为本语言独有类型的容器交给引擎须写 `Array(hits)`，以免隐式复制造成“修改了副本”的坑。[语料计数](surveys/corpus-counts.md)中 14 处 `pop_at` 都按算出的下标移出元素，没有一处判空。Dictionary 插入键时复制并逐层冻结容器，是因为引擎保存插入时的哈希，且 `make_read_only` 不可撤销。不冻结会使修改后的键按新旧内容都查不到。直接冻结原值又会让调用方的容器永久只读。
- **接口与无带状态 mixin（§3.9）**：以下数据来自[跨分支重复调查](surveys/cross-branch-duplication.md)。原生基类不同造成的重复，在多数游戏仓库中每千行约 4–10 行。游戏中这类重复共 353 行，其中 185 行可用组合或静态函数消除。适合带状态 mixin 的 102 行中，72 行来自 Ring 一个项目。同一原生基类无关脚本间和同一继承链上的重复，分别是它的 4.9 倍和 8.2 倍（主要在 `tanks-of-freedom-ii`）。`gdUnit4` 中 16 个只有抽象方法的类，有 14 个继承 `GdUnitAssert` 并以协变返回类型重新声明方法。例如 `GdUnitAssert.gd:7` 的 `is_null() -> GdUnitAssert`，在 `GdUnitIntAssert.gd:7` 中为 `-> GdUnitIntAssert`。第一版不支持泛型接口，以免在没有真实代码需求时过早规定型变和自引用约束。
- **函数类型与 lambda（§3.11）**：[语料计数](surveys/corpus-counts.md)中，协程方法作为值使用 91 处，其中 60 处是 `connect(名字)`。方法里的 961 个 lambda 没有一个给外层局部变量赋值。经成员或下标赋值的 15 个，改的都是对象或容器。979 个 lambda 中命名的有 18 个，带参数默认值的为 0 个。

### §4 声明

- **类、抽象类与内部类（§4.1）**：内部类直接解析外层的静态函数和静态变量，GDScript 因其查找实现而不解析。这与 Java、C#、Kotlin 一致，也不改变 GDScript 已解析名字的含义。[实现覆盖调查](surveys/implementation-coverage.md)中 4.9% 的文件以裸名使用引擎基类常量。[语料计数](surveys/corpus-counts.md)中：
  - 28 个 `@abstract` 类有 25 个在 `gdUnit4`；
  - 文件级抽象类中 17 个只有抽象方法，可改写为接口。`godot-open-rpg` 的 `BattlerAction` 则不同，同时带导出字段和抽象协程；
  - 2,800 个文件中有 121 个含内部类，共 192 个。游戏仓库中 19 个文件含 37 个；
  - 7 个内部类嵌套在内部类中，10 个把 `extends` 写在类体开头；
  - 经 autoload 访问内部类的 175 处中，有 168 处在 `godot-open-rts`（如 `Constants.Match.Units.PROJECTILES`）。
- **可见性（§4.2）**：[成员可见性调查](surveys/member-visibility.md)中，名字可判定的成员约 25% 在其他文件中被访问。[第二次纸面迁移](surveys/paper-migration-2.md)中，若子类只能看到 `pub` 成员，102 处 `pub` 里有 29 处只是为了子类。
- **常量（§4.5）**：向量分量 `real_t` 在单双精度构建中不同。`sin` 等数学函数的末位随平台数学库而异。因此引擎类型和返回 `float` 的数学函数在类加载时求值一次。同时，编译期排除所有因值出错的情形。[常量表达式调查](surveys/constant-expressions.md)中，3,094 个常量里 2,920 个无需计算、141 个是标量计算、33 个需引擎求值。注解参数用到的 7 个常量都是字面量，enum 值未用到常量，规范排除的写法均未出现。
- **函数与剩余参数（§4.6）**：[第二次纸面迁移](surveys/paper-migration-2.md)中，引擎虚函数覆盖省略参数标注 6 处。[语料计数](surveys/corpus-counts.md)中 78 个剩余参数有 76 个在 `gdUnit4` 断言接口中。缺少调用处展开写法时，容易把数组整个当作单个实参传出。例如 Pixelorama 的 `_debug_msg` 调 `_prefix_print`（`src/Classes/ObjParse.gd:59-66`）。禁止遮蔽外层块局部变量是因为 GDScript 已禁止，以后放开也兼容。
- **静态成员（§4.8）**：[语料计数](surveys/corpus-counts.md)中 `static var` 188 处，其中无初始化式的类类型 29 个（如 `static var instance: DMPlugin`）。`_static_init` 有 4 个。GDScript 静态初始化顺序与环的相关问题见 [godotengine/godot#104161](https://github.com/godotengine/godot/issues/104161)、[#82141](https://github.com/godotengine/godot/issues/82141)、[#104588](https://github.com/godotengine/godot/issues/104588)。

### §5–§7 表达式、生命周期与场景模型

- **表达式与收窄（§5.1–§5.4）**：`godot-open-rts` 用 `"hp" in unit` 检查单位成员 16 次。[第二次纸面迁移](surveys/paper-migration-2.md)中 `(x as T).unwrap()` 向下转型 16 处。因成员不收窄而新增的局部变量每千行 4.4 个。
- **生命周期与析构（§6.2–§6.7）**：相关引擎暗坑见[暗坑清单](surveys/pitfalls.md) G-02、G-03、G-04、G-05、G-09、G-10。[语料计数](surveys/corpus-counts.md)中，在 `_ready` 里把节点登记到 autoload 成员上的写法只有 6 处（5 处在 Ring）。`NOTIFICATION_PREDELETE` 仅 40 次（32 次在 `gdUnit4`）。
- **场景定型与差异结构（§7.1–§7.2）**：数据来自[场景结构调查](surveys/scene-structures.md)的游戏仓库。2,023 处字面量路径中，没有一处在不同结构中解析为不同类型。任何场景都没有的 `$X` 也能抓出改名遗漏。例如 `tps-demo` 的 `door.gd:6` 写 `$DoorModel/AnimationPlayer`，而 `door.tscn` 中叫 `DoorModel2`。部分结构缺失的路径中：
  - 7 处是模板或占位场景缺节点。如 `tanks-of-freedom-ii` 的 `base_ground.tscn` 被 preload 作占位地块，却缺少 `tile.gd` 的 `$"mesh"`；
  - 8 处用 `get_node_or_null`，如 `Starter-Kit-Racing` 两种车的尾迹。
- **向上访问与资源共享（§7.3、§7.6）**：不在移动节点时检查 `owner`，因为先读 `owner` 再移走自己是合法操作。Ring 的 `DeathComponent` 就先读 `owner`，再把自己移到前景层。共享子资源被改写的实例见 `godot-open-rpg` 的 `interaction_popup.tscn`。其中内嵌的 `CircleShape2D` 未设局部于场景，`interaction_popup.gd:30` 改它的半径。而 `main.tscn:484-486` 中只有一个实例设为 64。资源环见[暗坑清单](surveys/pitfalls.md) G-19、G-20、G-34。[语料计数](surveys/corpus-counts.md)中：
  - 自身的 `get_parent()` 174 处，直接访问成员 62 处，分布在 46 个文件。判空 3 处，含 `beehave` 在挂上父节点前运行的 setter；
  - `owner` 直接访问成员 49 处，分布在 21 个文件。

### §8–§13 错误、协程、引擎边界与工具链

- **程序错误与断言（§8.1–§8.3）**：GDScript 出错后让调用方继续执行。此时 debug 构建返回类型默认值，release 构建返回 `null`。在静态类型下，这会产生假值。`?` 短路返回不用 `?.` 伴随，以免 `x?.y` 取决于 `x` 是 Option 还是 Result。[语料计数](surveys/corpus-counts.md)中，595 个 `assert` 的条件基本都是纯查询，实参含 `await` 的 0 个。539 个消息无一依赖在条件为真时求值。
- **协程与信号（§9.1–§9.7）**：[语料计数](surveys/corpus-counts.md)中 979 个 lambda 只有 35 个含 `await`（33 个在 `gdUnit4` 测试中）。重入与释放暗坑见[暗坑清单](surveys/pitfalls.md) G-11、G-12、G-13，及提案 [godot-proposals#8838](https://github.com/godotengine/godot-proposals/issues/8838)、[#6243](https://github.com/godotengine/godot-proposals/issues/6243)。[await 信号结果调查](surveys/await-signal-results.md)中，1,356 个 `await` 只有 1 个使用了多参数信号的结果。也只有 1 个使用了签名未知的 `Signal` 结果（`godot_dialogue_manager` `:1956` 经 `RefCounted` 接收者等待 C# 两参数信号）。
- **引擎边界（§10.2–§10.6）**：引擎用 `RequiredResult` 标注的 API 仅约 50 处（29 处在 `Tween`）。引擎传入虚函数和信号的对象几乎总是非空。默认非空加入口检查，避免每个 `_input` 和信号处理函数重复判空。引擎 API 中窄于 `int64` 或无符号的方法参数有 3,700 多个、属性约 460 个。例如 `uint32` 的 `collision_layer` 写入 `-1` 会回绕为 `4294967295`。
- **跨语言、线程与工具链（§11.2、§12.3、§13.2–§13.4）**：跨语言继承限制见[暗坑清单](surveys/pitfalls.md) C-17。[第二次纸面迁移](surveys/paper-migration-2.md)中，Ring 的 `Brick` 和 `Enemy` 因此连带迁移了 5 个子类。全部 230 处迁移修改中，有 197 处是机械修改。[语料计数](surveys/corpus-counts.md)中 `Thread.new` 仅 5 处、`WorkerThreadPool` 2 处。括号续行在真实代码中大多只缩进一层，风格指南要求两层。GDScript 格式化器草稿因解析器丢弃字面量写法与括号而受阻（[godotengine/godot#76211](https://github.com/godotengine/godot/pull/76211)）。自带测试运行器，是因为 `gdUnit4` 和 `GUT` 要求继承其 GDScript 基类。

## 3. 考虑过但没有采用的方案

| 方案 | 不采用的原因 | 现行规则 |
|---|---|---|
| 以引擎模块交付（v0.1） | 暗坑清单中没有只有模块才能修复的条目；GDExtension 可以用官方编辑器和导出模板 | §1.3 |
| 窄整数类型 i32、u8 等（v0.1） | 迁移成本最大的一项（[纸面迁移](surveys/paper-migration.md)）；脚本很少在意数值类型的性能 | §3.2 |
| 推断函数签名（v0.1） | 签名是对外的约定，不应随函数体变化 | §4.6 |
| 以 trace 为单位编译的 JIT | 类型在编译期已知，追踪运行带来的类型特化本来就有。事先生成没有可追踪的运行，两条路就不能共用一个生成模块 | §12.2 |
| 内层块遮蔽外层的局部变量，如 C++、Rust、Swift | GDScript 禁止，迁移来的代码中不会出现；禁止以后可以兼容地放开 | §4.6 |
| 语言层面的跨语言继承 | 依赖 GDScript 的内部实现 | §11.2 |
| 可空链 `?.`、默认值 `??`、后缀 `!` | 与 Result 的短路返回合用时，`x?.y` 的含义要看 x 的类型；Option 的操作用方法就够了 | §3.4、§8.2 |
| Swift 式的 `try` 覆盖整条表达式 | 要按类型决定哪些子表达式被解开；Rust 的后缀 `?` 更直接 | §8.2 |
| Result 也支持链式的 `?.` | 两种类型嵌套时，一个运算符无法说明处理的是哪一层 | §8.2 |
| 在属性中另设 `apply:` 块，存放须等到节点就绪才执行的代码 | 让用户思考逻辑在何时执行、执行几次；自动挂起可以由编译器完成 | §6.5 |
| 就绪前只记下 setter 的原始输入 | 先变换再存储的 setter（如给角色名加【】）就绪前会读到未变换的值 | §6.5 |
| `@late`（类似 `lateinit`） | 需要它的写法很少见；父节点配置子节点已由祖先赋值的规则覆盖 | §6.2 |
| 子类只能看到 `pub` 成员 | [第二次纸面迁移](surveys/paper-migration-2.md)中 102 处 `pub` 有 29 处只是为了子类 | §4.2 |
| 公开标记用 `@public` 注解 | 写得频繁，[成员可见性调查](surveys/member-visibility.md)中每千行至少 22 处 | §2.4 |
| 文件顶层的类型直接成为全局名 | 打破“文件就是类”，需要另一张全局名表 | §2.4 |
| 接口的交集类型 `@owner(Node2D & Combatant)` | 新概念；允许引擎属性满足接口的属性要求即可 | §3.9 |
| 追踪式 GC，或闭包自动弱捕获 self | 与 GDScript 的行为悄悄不同 | §3.11、§6.7 |
| 像 GDScript 那样允许给捕获的变量赋值，只给出警告 | 赋值既不改变外面的变量，也影响不到下一次调用，与字面意思相反；语料中没有 lambda 这样写 | §3.11 |
| 调用本语言的函数值像引擎调用 Callable 那样开始新的链 | 调用方会拿着返回类型的默认值这个假值继续执行，而直接调用同一个方法时调用方会中止 | §8.1 |
| 在移动节点时检查 owner | 节点移出 owner 的子树是合法操作 | §7.3 |
| 没有被引用的场景缺值时也算导出错误 | 运行时才确定路径的 `load` 看不到，无法断定场景不被使用 | §7.5 |
| `as` 转为内置类型时严格检查 | 与 GDScript 的构造转换悄悄不同 | §5.3 |
| Array 的 `<=`、`>=` 照引擎取 `not (b < a)`、`not (a < b)`，`<` 跳过比不出大小的元素 | 元素比不出大小时结果自相矛盾：`[NAN] <= [1.0]` 和 `[NAN] >= [1.0]` 都为真，`==` 却为假。`NAN <= 1.0` 也为假 | §5.2 |
| 未审核的引擎 API 中对象类型一律可空 | 引擎传入的虚函数参数和信号参数几乎总是非空，一律可空会让每个处理函数都先判空。少数为空的情况由入口检查报告 | §10.2 |
| int 与 float 混用的工具函数统一提升为 float | GDScript 原样返回实参，结果会悄悄不同 | §10.3 |
| `pop_front()` 在空数组上是程序错误 | 引擎在这里返回 null 而不报错，会悄悄不同 | §3.8 |
| `pop_at` 与 `pop_back` 一样返回 `T?` | 真实代码都按已知的下标用 `pop_at` 移出元素，不判断结果为空。返回 `T?` 让每处都要解开，而越界本身就是错误 | §3.8 |
| await 多参数信号得到 Array，或按参数解构 | 真实代码几乎不使用这种结果；解构可以以后加入 | §9.6 |
| 以 if/else 块为值 | 与条件表达式重复 | §5.5 |
| 为基类要求的、可选的和运行时添加的子节点各设一种声明 | 按使用方式的检查和 `get_node_or_null` 已经覆盖这三种情况，新的声明只会增加概念 | §7.2 |
| 反射交出元素为本语言独有类型的容器成员的编码副本 | 外部修改副本不影响原容器，即 §3.8 的显式转换要避免的情况 | §10.5 |
| 覆盖时写出的类型必须与被覆盖的完全一致 | 比 GDScript 严，拒绝 gdUnit4 一类流式接口所用的协变返回类型 | §4.6 |
| 嵌套的 Option 合并为一层（Kotlin 的做法） | 泛型代码的行为随 T 改变；容器中存入的 None 与缺少的值无法区分 | §3.4 |
| lambda 不写 async，按函数体或期望类型推断 | 判断函数是否为 async 要用多条规则，lambda 和方法值能传到哪里也会不同 | §9.1 |
| 允许 `_init` 和非 void 引擎虚函数覆盖声明为 async | 构造不等待初始化完成；同步引擎调用无法取得协程最终返回值 | §9.1 |
| async 方法向引擎报告声明的返回类型 | GDScript 中每处 await 都有警告；取消时的 null 写入值类型的临时变量会使进程崩溃 | §9.8 |
| 热重载修改声明后，让之后新建的实例使用新版本 | 同一次运行中同一个类有两个版本，按旧声明编译的代码可能调用新实例。主要用途只是调整不断新建的对象 | §12.4 |
| 热重载修改声明后，像 GDScript 那样按成员名迁移状态 | 成员的类型可能已经改变，静态类型下搬过去的值不可信 | §12.4 |
| 在自身上调用 `free()`、`set_script()` 是编译错误 | [语料计数](surveys/corpus-counts.md)中这两种写法都没有真正出现。处理函数释放发射者、被调方释放调用方等间接情况，在运行之前判断不出来。运行时的检查无论如何都需要 | §3.3 |
| 解码 struct 时拒绝不是字段的键 | 较新的发送方多带字段的消息、为多个读取方写的 JSON 会在入口处失败。而 struct 需要的字段其实都在 | §3.6 |
| 元素为本语言独有类型的容器上，`sort`、`min`、`max`、`hash` 等报编译错误 | GDScript 代码会调用它们，迁移来的代码就得改写；运行时可以用本语言的运算完成它们 | §3.8 |
| struct 的哈希跳过容器字段，或 struct 只有 `==` 和哈希而没有 `<` | 哪些字段参与由用户决定，而不是由实现决定。开箱即用时各个默认运算应当配套，以后可以用 struct 方法重新定义 | §3.6 |
| 禁止含容器字段的 struct 作 Dictionary 的键 | GDScript 允许以容器作键；只读的副本使保存的哈希始终有效 | §3.8 |
| 把原值在作键时设为只读 | `make_read_only` 不能撤销，调用方的容器，以及其他持有它的地方，从此再也不能修改 | §3.8 |
| 按 GDScript 在空数组上返回 null 的样子，让 `min()`、`max()` 返回 `T?` | 这个 null 是元素类型没有的值，GDScript 默默地把它传下去；程序错误指出哪里没料到空数组 | §3.8 |
| 编译器求出全部常量，包括引擎类型的运算 | 向量的分量随引擎的构建是 float 或 double，编译一次的字节码只能写死其中一种结果。编译器还要重写引擎的运算 | §4.5 |
| 编译期经引擎求出常量 | 编译器就要依赖引擎，编译器测试要么测不了常量，要么在测试桩中再写一遍求值 | §4.5 |
| 全部常量在类加载时求值 | 注解参数、enum 的值在编译期拿不到值，`10 / 0` 一类错误推迟到加载时才报 | §4.5 |
| 单引号和双引号字符串可以跨行，与 GDScript 相同 | 漏写右引号时错误报在远处；语料中 97 个这样的字符串，95 个在同一个仓库的测试中 | §2.2 |
| 内部类只解析外层类的常量、enum 和内部类，与 GDScript 相同 | GDScript 的限制是其查找方式的副产品；静态成员不需要实例，Java、C#、Kotlin 的嵌套类都能直接用 | §4.1 |
| 没有初始化式的对象类型静态变量从 None 开始，与 GDScript 相同 | 非空的类型没有 None 这个值；需要空值时写作 `T?` | §4.8 |
| 无类型的容器隐式转为带类型的容器，与 GDScript 的静态检查相同 | GDScript 运行时只有容器恰为该类型时才成功，隐式转换把这个一定会做的检查藏到运行时。显式的 `is` 写出了它 | §3.8 |
| 经无类型的视图写入不合元素类型的值时跳过这次写入，与引擎相同 | 程序带着没写进去的值继续运行；查找时引擎还把 1.5 截断为 1 | §3.8 |
| `as` 遇到 Variant 中不是对象的值时是程序错误，与 GDScript 的 debug 构建相同 | `as` 是可以失败的转换，类型不符时就是 None，`v is Node` 在这里同样为假 | §5.3 |
| 进入 Variant 的字面量也由元素定型 | 空字面量无从定型，`d[k] = []` 要写出类型。有元素时得到带类型的容器，之后经 Variant 写入其他类型的值是程序错误，GDScript 中则可以 | §3.13 |
| 编译期检查跨类的静态初始化环 | 要知道其他文件的初始化读取什么；这样的环每次运行加载这些类时都会报出 | §4.8 |
| 热重载时重新运行静态初始化，再按名字搬回旧值，与 GDScript 相同 | `_static_init` 的副作用（如登记）会再发生一次；只修改函数体时，正在运行的程序只换之后的调用（§12.4） | §4.8 |
| `new(...)` 跟随类的可见性，不看 `_init` 是否 `pub`，与 GDScript 相同 | 与其他成员不一致：不带 `pub` 的 `_init` 改签名时，调用方不只在本文件和子类中。类也无法只经自己的方法构造 | §4.2 |
| 引擎基类的常量必须写出类名，如 `Object.NOTIFICATION_PREDELETE`，快速修复补上类名 | 引擎基类的属性、方法和信号都用裸名，只有常量例外；GDScript 代码中 4.9% 的文件要逐处改写 | §4.1 |
