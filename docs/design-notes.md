# 设计说明与取舍

本文是非规范性的配套手记，收纳从[语言规范](language-spec.zh.md)移出的三部分内容。这三部分分别是：待定与后续特性设计、各节规则背后的权衡与语料依据，以及考虑过但未采用的方案。语言规则始终以语言规范为准，引擎行为出处见[引擎事实](engine-facts.md)。

## 1. 待定与后续设计

### 1.1 用户定义的泛型（规范 §2.4、§3.10）

用户定义的泛型属于第一版范围，计划在 0.9 冻结语言前定下。GDScript 本身没有用户泛型，从它迁移来的代码（含 FPS 和 Ring）不会依赖这一特性，但泛型定义必须留在字节码模块中（实现架构 §6.1）。

**可泛型的声明**限定在函数、struct、enum，以及不继承 Resource 的 RefCounted 类。Node、Resource 脚本和 autoload 不能声明类型参数，因为引擎实例化它们时无法传入类型实参。同理，泛型类不登记进引擎的全局类表，GDScript 看不到它，也给不出类型实参。

**语法**沿用方括号，把类型参数紧跟在名字后面，与 `Array[int]` 保持一致，也与 Python 3.12（PEP 695）同形。约束写作 `T: 接口`。文件级类在 `class_name` 上声明类型参数，如 `class_name Cache[K, V] extends RefCounted`；内部类则写作 `class Cache[K, V] extends RefCounted:`。由于 GDScript 在内部类名、`class_name` 的名字、enum 名和函数名后遇到 `[` 都是语法错误（[引擎事实](engine-facts.md)），这套语法不会引入静默差异。下面是几类泛型声明的写法：

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

**类型实参与推断**：调用处通常由实参自动推断类型实参。推断不出时必须显式写出，例如 `first[int]([])`、`Pair[int, String](first = 1, second = "a")`，否则像 `var x = first([])` 这样直接报编译错误。`名字[...]` 与下标同形，由名字解析区分：名字指向泛型函数或泛型类型时方括号里是类型实参，否则按普通下标处理。

**约束与运行时保留**：约束通过接口表达，初版每个类型参数至多带一个约束，不满足即报编译错误。以后可以沿同一语法扩展到多约束、以类作约束，以及泛型接口 `interface Source[T]:`（规范 §3.9）。类型实参在运行时完整保留（实现架构 §6.1）。

**反射与 Callable**：泛型方法不经反射暴露（规范 §10.5），因为引擎和 GDScript 按名调用时给不出类型实参。凡是引擎必须按名查找的方法也不能写成泛型。把泛型函数当作 Callable 使用时须确定类型实参，如 `sig.connect(handle[int])`，确定不了时报编译错误。

**迁移**：正向迁移不会引入泛型；反向转换遇到使用了泛型的文件则跳过转换，仅列出所在位置（规范 §13.4）。双向转换因此不约束泛型本身的设计。

**【待定】实现泛型前还需敲定以下问题：**

- 无约束的 `T` 允许哪些操作（如 `==`、转为 Variant 或打印、`is T`、默认值），以及 `T` 本身可空时 `T?` 的含义；
- 泛型类的静态变量是每个实例化各一份，还是共用一份；
- 多态递归会在运行时产生无界的特化，是否需要设上限；
- 泛型类继承泛型类时，基类布局随 `T` 变化，加载时的布局检查需要按每个实例化各做一次；
- 函数类型作类型实参（如 `first[func(int) -> bool](...)`）在表达式位置会读成 lambda，需要一条专门的解析规则；
- 为每个实例化合成派生类映像，还是把类映像拆成共享与特化两部分（前者需先实测内存和加载时间）；
- 引擎边界：GDScript 不带实参调用泛型类的 `new()` 时应给出明确错误；此外，带类型参数的信号在脚本信号表中给不出具体类型。

## 2. 各节理由与语料依据

### §1–§2 交付、词法与表层语法

#### 可还原规则（§1.2、§13.4）

用户愿不愿意试用，很大程度上取决于反悔的成本。像 Jolt 物理引擎那样改一项设置就能切换、不满意也能原样换回，门槛才低；语言若要支持逐文件渐进迁移，只能靠双向转换做到这一点。

在[第二次纸面迁移](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/paper-migration-2.md)的 230 处修改中，只有引入接口的 13 处和消息 struct 的 2 处不可还原，而且它们都是迁移者主动做的结构改进，并非必需。有了自动迁移后，机械改写不再消耗人力。衡量与 GDScript 的迁移距离，改看需要人工判断的修改（每百行 3.6 处）以及能否无损还原，不必为了少改几行而放宽 `pub` 这类规则。这条原则同样约束后续新增的规则。

#### 无静默差异的例外（§1.2）

0.1 之后，我们清点出 17 处与 GDScript 写法相同、都能编译但行为不同的代码，逐条复核后归为三类：

- **10 处属于 GDScript 本身出错或给出错误结果**，例如中止调用链、等待已释放对象以及 `int` 的 `**`。用作字典键的容器也在审查时归入此类，因为在 GDScript 中，插入后再修改容器键就再也查不到它。
- **2 处改为与 GDScript 一致或直接报编译错误**：`is_instance_of` 搭配 `TYPE_*`，以及两层 Option 与 `null` 比较。
- **5 处出于设计需要**，由语言规范逐条点名。

审查过程另发现 2 处：`T?` 转为 `Variant` 时丢弃已释放对象，已改为与 GDScript 保持一致；两个两层 Option 互相比较则一并列入规范点名。此外，字面量由元素定型在 0.9 之前继续公开征询，它的风险集中在引擎边界：若 GDScript 向推断出的 `Array[int]` 写入 `0.5`，引擎会静默截断为 `0`（[引擎事实](engine-facts.md)）。

有一条备选方案在审查中被否决：脚本实例收到未标记 `pub` 的成员名时打印错误。问题在于 `"hp" in obj` 同样走 `Object::get`，鸭子类型的试探会因此到处报错（[引擎事实](engine-facts.md)）；v2 语料中抽样的 `obj.get(名字) == null` 也多是这类试探（[语料计数](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/corpus-counts.md)）。

#### GDExtension 与只发字节码（§1.3）

选择 GDExtension 而非引擎模块，是因为[暗坑清单](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/pitfalls.md)中没有任何一条只有模块才能修复。GDExtension 绕不开的限制都得改动引擎源码，做成模块同样无能为力。

导出产物只发字节码，则取决于场景定型机制：`$X`、`@parent` 和 `PackedScene[T]` 的类型都依赖场景树（§7）。如果在加载时才编译源码，就必须在游戏进程内重新分析场景，还连带要把整个分析器打包进所有导出平台。

#### preload 脚本作为类（§2.1）

把 `preload` 进来的脚本直接当作类使用，是真实项目的高频惯用法。[语料计数](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/corpus-counts.md)记录了 351 处 `preload` `.gd` 文件，其中 207 处用于初始化常量，69 处直接跟 `.new()`。这些常量在同一文件中的具体用途分布如下：

- 用于 `new()`：54 处；
- 用于其他调用：124 处；
- 用于其他成员访问：134 处；
- 用在 `is` 之后：69 处；
- 作为类型标注：46 处。

#### 只当类型用的 preload 常量不加载（§2.1、§7.6）

没有 `class_name` 的类只能经路径引用，迁移工具要写它的类型，就得在用到的文件里写 `const Board = preload(...)`。若这样的常量照常随文件加载，核心类之间会成环：tanks-of-freedom-ii 的 board 预加载 abilities 等逻辑对象并把 `self` 传给它们，这些对象又要写 Board 类型。迁移在 tanks 中因类没有名字而写不出类型的槽位有 162 个，涉及 43 个类。按加载边算，其中 37 个会成环，正是 board 和 map 两个核心类。这些数字取自迁移工具在 Petrichor `bbdd797` 上对 tanks 的报告（[迁移工具调查](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/tool-migration.md)）。

检查一个值是否某类的实例并不需要加载这个类：值若是它的实例，它必已加载。本语言运行时按脚本路径做类型检查，`new()`、静态调用和建带类型的容器都在用到时才按路径加载，只有 preload 常量在类加载时求值。因此只当类型用的常量跳过这一步即可。GDScript 本身允许脚本之间互相 preload，它先给浅脚本，编译完再补全（[引擎事实](engine-facts.md)）。

另外两种做法都没有采用。按文件名补 `class_name` 会把名字放进全局命名空间，Node 脚本还会出现在新建节点对话框里。tanks 的 277 个无名文件中也有 104 个文件名相同，如 3 个 `camera.gd`。让所有 preload 都推迟到首次使用时加载则改动太大：被预加载脚本的静态初始化都会挪到首次使用时。

别的语言里最接近的是 JVM：只在签名里出现的类不初始化，建实例、调静态方法和读写静态字段时才初始化（[JLS §12.4.1](https://docs.oracle.com/javase/specs/jls/se21/html/jls-12.html#jls-12.4.1)）。TypeScript 默认删去只当类型用的 import，结果一是模块的副作用丢了（[TypeScript#9191](https://github.com/microsoft/TypeScript/issues/9191)），二是删不删要看另一个文件的声明，单文件工具判断不了（[TypeScript#51479](https://github.com/microsoft/TypeScript/issues/51479)）。5.0 起改为只删写明 `import type` 的。本语言只看本文件的写法分类，避开了第二个问题。

第一个问题对应的差别是静态初始化的时机。只当类型用的脚本，其 `_static_init` 不随本文件加载而运行，GDScript 则只要 preload 就会运行。迁移工具新写的常量在原文中并无 preload，行为反而与原文一致。原文已有且只当类型用的常量才有差别，而 v1 语料中 `_static_init` 一共只有 4 个。

带类型的容器是例外。引擎的带类型容器持有元素类型的 Script 对象（[引擎事实](engine-facts.md)），类加载过程中再去加载同一个环上的脚本只会得到空值，容器便只按原生类定型。所以类加载时就要建的容器，如静态变量初始化式中的 `Array[Unit]`，仍以元素类型为边。

#### `null` 别名与单行引号字符串（§2.2）

保留 `null` 作为 `None` 的别名，纯属降低机械迁移成本。[语料计数](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/corpus-counts.md)中 `null` 每千行出现 22.2 次，把它改成编译错误除了增加无意义的机械改写，没有额外收益。

单引号和双引号字符串则禁止跨行。这样漏写右引号时错误会立刻停在本行，不会一路吞掉后续代码。v1 语料里跨行单双引号字符串共 97 个，其中 95 个集中在 `godot_dialogue_manager` 的测试里；v2 中有 185 个（分布于 9 库），抽样的 50 个里 23 个是界面文字（[v2 复跑](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/v2-rerun.md)）。它们都能机械改写为三引号，改造成本可控。

#### 上下文关键字（§2.2）

新增的六个关键字做成上下文关键字，不占硬关键字，这与 C#、Python 和 Kotlin 引入新词的做法一致。

虽然[语料计数](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/corpus-counts.md)里这六个词在第一方代码中都没当过标识符，但[v2 复跑](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/v2-rerun.md)已在 7 个仓库里检出 32 处冲突。更棘手的是插件与生态兼容性：Ring 附带的 `dialogue_manager` 声明了 `TranslationSource.None`，如果 `None` 是硬关键字，Petrichor 连这个插件的值都写不出来。有 Rust 背景的开发者也会在 GDScript 里自建 Option 类，迁移后同样带有这些名字。

不过，裸名 `None` 如果既能表示空值又能指向用户声明，同一写法就会出现两种含义，因此此时直接报编译错误。

#### `###` 注释不是文档（§2.2）

全由 `###` 开头的注释行按普通注释处理，不计作文档注释，正如 Rust 把 `////` 当普通注释对待。

在[语料计数](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/corpus-counts.md)中，紧贴声明上方、全由 `###` 开头的注释有 41 段，全都是 `### Public variables ###` 之类的分节标题或分隔线。这些注释里没有一段把 `###` 和其他文档行混写，GDScript 却把它们当成了下方声明的文档。v2 抽样的 50 段纯 `###` 注释中，40 段是标题或分隔线、6 段描述声明，混写另有 7 段（[v2 复跑](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/v2-rerun.md)）。

#### `@tool` 在语义冻结之前加入（§2.3）

`@tool` 安排在 0.9 冻结语义前完成。在编辑器里运行脚本不仅要求加载时完整编译，还涉及编辑器内的实例化与重载，这类大功能很容易牵动其他语言规则，不宜留到冻结之后。

它在实际生态中的占比也不容忽视：[语料计数](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/corpus-counts.md)中 326 个含 `@tool` 的文件有 211 个位于插件仓库，游戏仓库占 58 个（其中 31 个在 `godot-open-rpg`）。到 v2 语料中，含 `@tool` 的文件达到 2,752 个，分布在 135 个库（[v2 复跑](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/v2-rerun.md)）。

#### `is_instance_of` 接受脚本值（§2.6）

脚本本身就是一等值（§2.1），拿它做实例检测的含义清晰确定，直接放行。至于 GDScript 里作为值的引擎类，它底层是对扩展不公开的内部对象，留待“类作为值”特性一并设计。

#### `is_instance_of` 的 `TYPE_*` 只比较类型（§2.6）

当第二个参数是 `TYPE_*` 时，Petrichor 沿用 GDScript 的定义，只比较值的类型而不检查对象是否已释放（[引擎事实](engine-facts.md)）。如果强行改成 `is` 的语义，反而会与 `typeof(v) == TYPE_OBJECT` 互相打架；而且收窄出来的非空 `Object` 本就可能持有已释放对象，类型保证并不会被打破。

引擎返回的空对象则是例外：收窄后的非空 `Object` 不允许为 null，因此对空对象判定为假。GDScript 在此处返回真，调用方紧接着使用它就会报错。

### §3 类型

#### `float` 不隐式转 `int`（§3.14）

在 Petrichor 中，`var hp = 10` 推断为 `int`（§4.3），而在 GDScript 里它是无类型变量。GDScript 执行 `hp = 2.5` 后值会变成 `2.5`；如果允许 `float` 隐式截断为 `int`，相同写法就会悄悄产生不同结果。

#### 对象可空与已释放对象（§3.3）

检查对象存活在 Godot 项目里十分常见。[语料计数](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/corpus-counts.md)中 `is_instance_valid` 每千行出现 3.3 处，`phantom-camera` 里更是达到每千行 12.8 处。Godot 4.4 起也让已释放对象在条件判断中为假（[godotengine/godot#93885](https://github.com/godotengine/godot/pull/93885)）；相关引擎行为问题见[暗坑清单](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/pitfalls.md) G-07、G-08。

`T?` 转为 `Variant` 时会保留已释放对象。如果把它擦成 null，`print` 就会输出 `<null>` 而非 `<Freed Object>`，`typeof` 也会退化成 `TYPE_NIL`。由于 `Variant` 本身就可能持有已释放对象，且其中的已释放对象同样等于 `None`，判等结果不受影响。

#### `T?` 的快速修复（§3.3、§3.4）

针对对象和可空类型的修复，排序兼顾安全与改动波及面，并且都不自动应用：

- **`T?` 传给 `T`**：`.unwrap()` 排在第一，但不自动应用。GDScript 会先存下 null、到真正使用时才出错，而 `.unwrap()` 会把它提前变成当前位置的运行时程序错误。Swift 对同类错误的 `!` 也只放在 note 备选里，Roslyn 出于同样理由把“实现缺失成员”移出了自动清理。把目标类型改成 `T?` 排在第二，同样不自动应用，因为它会把类型错误转移到其他使用处。
- **非空对象与 `null` 比较**：`x == null` 优先改为 `not is_instance_valid(x)`，因为 GDScript 中已释放对象也等于 null。把变量改成 `T?` 会波及其余使用处，排在第二。

#### 带类型的弱引用在第一版之后（§3.12）

带类型的弱引用超出 GDScript 现有能力，留待第一版后公开征询（godot-proposals#9174）。“对象可能已释放”的主流需求已经由 `T?` 覆盖，每次读取都会检查存活；引擎原生的 `weakref()` 和 `WeakRef` 也照常可用，并未损失既有能力。

语料也印证了低优先级：[语料计数](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/corpus-counts.md)中 `weakref` 全量仅 3 处，游戏仓库为 0。其中 2 处在 gdUnit4 测试里用于验证对象已释放，唯一的产品代码是 `dialogue_manager` 的父指针，写了却从不读。v2 语料里 `weakref` 增至 35 处（13 库，游戏 13 处），用于父链、缓存与对象跟踪（[v2 复跑](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/v2-rerun.md)）。

#### `Variant` 只能判等（§3.13）

对 `Variant` 取下标、读成员、调方法和遍历都要先转换。这层摩擦是有意的：它把结构固定的数据推向在入口处解码为 struct，之后的代码都带类型。逐层写 `Dictionary(v)["k"]` 比解码一次更费事，为的就是让后者成为省事的写法。转换本身也只在运行时检查，它的作用不在检查，而在让凑合的写法显眼。

迁移工具在 tanks-of-freedom-ii 中留下 794 处对 `Variant` 的访问，取下标 454 处，调用方法 195 处，读属性 29 处，大多落在存剧情和地图数据的多层无类型字典上（[迁移工具调查](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/tool-migration.md)）。迁移工具不替它们写转换：它定不出数据的结构，成批写下的 `Dictionary(...)` 正是这条规则要指出的写法。

#### 嵌套 Option 保留两层（§3.4）

`T??` 保留两层而不拍平。如果合并为一层，泛型代码的行为就会随 `T` 漂移。例如 `first[T]` 在 `T` 为 `Enemy?` 时分不清空数组和首元素为空，通用缓存会把存入的 `None` 当成未命中，JSON 里缺失键和显式 `null` 也无法区分。由于 Petrichor 没有 `?.`，嵌套基本只出现在泛型和容器取值上。

出于无静默差异原则，两层 Option 与 `null` 比较直接报编译错误。GDScript 的 `d.get("hp") == null` 对缺键和存了 null 都为真，而 Petrichor 只对缺键为真；改写 `None` 属于 Petrichor 自己的写法，含义确定，照常合法。实际冲击很小：[语料计数](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/corpus-counts.md)中单参数 `get(k)` 与 `null` 比较仅 1 处；v2 有 194 处，但抽样的 14 处里只有 2 处是字典，其余都是 `Object.get`，不受影响。

#### struct 方法只读与默认运算（§3.6）

struct 的方法限制为只读，因为修改 `self` 要求调用处具备写回能力。面对临时值、经 getter 读出的属性和循环变量，可变方法要么报错，要么悄悄改掉一份副本。与此同时，编译器为 struct 默认生成 `==`、`<` 和哈希，让它像脚本语言的值类型一样开箱即用。

#### 赋值的求值顺序（§3.6）

Petrichor 在普通赋值和复合赋值中都先求右边，再求目标位置，最后连续完成读改写。GDScript 恰恰相反，会先沿链读出副本再求右边（[引擎事实](engine-facts.md)）；一旦右边挂起或改写这条链，写回旧副本就会覆盖期间的修改。例如 `position.x = await slide_x()` 会丢掉等待期间物理过程对 `y` 的更新。

这一取舍在主流语言中也分属两派：

- **先求右边**：普通赋值先求右边的有 Python、Rust 和 C++17；复合赋值同样先求右边的有 C++17 和 Rust 的原始类型（[Python](https://docs.python.org/3/reference/expressions.html#evaluation-order)，[Rust](https://doc.rust-lang.org/reference/expressions/operator-expr.html)）。
- **先读旧值**：Python、Java、C# 和 JavaScript 的复合赋值会先读旧值（[Python](https://docs.python.org/3/reference/simple_stmts.html#augmented-assignment-statements)，[JLS §15.26.2](https://docs.oracle.com/javase/specs/jls/se21/html/jls-15.html#jls-15.26.2)）。这类语言跨 `await` 都会丢更新，C# 和 ESLint 都专门发文提醒（[Raymond Chen](https://devblogs.microsoft.com/oldnewthing/20140814-00/?p=243)，[require-atomic-updates](https://eslint.org/docs/latest/rules/require-atomic-updates)）。

先求右边的代价在于，目标位置上的错误要等右边副作用执行完才会报出，C# 正因此坚持从左到右求值（[Raymond Chen](https://devblogs.microsoft.com/oldnewthing/20170718-00/?p=96635)）。在 Petrichor 里，非空类型已经排除了空对象，只剩已释放对象和越界下标两类错误。

我们否决了照搬 GDScript 的折中顺序（先求位置中的对象和下标，仅把最后一个下标推迟到右边之后），那是 GDScript 代码生成的副产品，很难向用户讲清。修改接收者的方法调用不受影响，仍与其他调用一样先求接收者再求实参；依赖求值顺序的代码本身就有害，交由 lint 提示。

#### enum（§3.7）

引擎 enum 保持开放，因为引擎在运行时会产生枚举清单之外的值，如带修饰键位的 `Key`。后续 4.x 小版本还会新增枚举值，同一份构建必须能继续运行（实现架构 §6.3）。

[语料计数](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/corpus-counts.md)记录了真实项目中与 enum 相关的几类用法：

- 用 `as` 把 `int` 转为 enum 10 处，忽略 `INT_AS_ENUM_WITHOUT_CAST` 警告 4 处；
- 把 enum 名当作 Dictionary 使用 38 处，用常量作 enum 别名 3 处；
- 258 个 enum 中有 8 个不包含值 0。

#### 容器（§3.8）

容器的四条核心边界规则各自针对一类引擎或迁移暗坑：

- **无类型视图的类型检查**：经无类型视图向带类型容器写入或查找不合元素类型的值时，直接报程序错误。引擎的做法是在写入报错后跳过该写入并继续运行，查找和 `assign()` 甚至会把 `1.5` 静默截断为 `1`。
- **自有元素容器交出给引擎**：元素属于 Petrichor 独有类型的容器不能隐式交出，须显式写 `Array(hits)`，防止隐式复制悄悄制造“改了副本却没改原容器”的坑。
- **`pop_at` 返回 `T` 而非 `T?`**：[语料计数](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/corpus-counts.md)中全部 14 处 `pop_at` 都按算好的下标移出元素，没有一处判空。
- **Dictionary 键深层复制并冻结**：插入容器作键时会先复制再逐层冻结。因为引擎只缓存插入那一刻的哈希，且 `make_read_only` 不可撤销：不冻结会让修改后的键按新旧内容都查不到，直接冻结原值又会让调用方的容器永久只读。

#### 接口与无带状态 mixin（§3.9）

第一版只提供无状态接口，不包含带状态 mixin、泛型接口和默认方法。

不引入带状态 mixin，主要基于[跨分支重复调查](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/cross-branch-duplication.md)的实测比例。因原生基类不同而产生的重复，在多数游戏仓库中每千行仅 4–10 行。全部游戏代码里这类重复共 353 行，其中 185 行可用组合或静态函数消除，适合带状态 mixin 的 102 行里有 72 行集中在 Ring 单个项目。相比之下，同一原生基类无关脚本间和同一继承链上的重复，分别是它的 4.9 倍和 8.2 倍（集中在 `tanks-of-freedom-ii`）。v2 中这两个倍数是 3.3 和 2.9，抽样的 50 组里有 12 组可用组合或静态函数化解（[v2 复跑](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/v2-rerun.md)）。由于两者统计分母不同，v1 按行数算出的过半可消除比例不能直接套到 v2。

泛型接口暂不纳入，避免在缺乏足够用例时过早规定型变和自引用约束。`gdUnit4` 中 16 个纯抽象方法类里，有 14 个继承 `GdUnitAssert` 并用协变返回类型重写方法，例如 `GdUnitAssert.gd:7` 的 `is_null() -> GdUnitAssert` 在 `GdUnitIntAssert.gd:7` 中变为 `-> GdUnitIntAssert`。

默认方法同样排在第一版之后。第二次纸面迁移中的接口都不需要它，GDScript 本身也没有接口。调查中对应默认方法的 54 行大多要调用 `self` 上的引擎 API；由于接口不知道实现类的基类，默认方法应连同基类约束一并设计。此外，默认方法会带来多继承菱形问题，届时须同时定清五点：

- 继承来的类方法与默认方法谁优先；
- 无关父接口之间的同名冲突；
- 默认方法中的 `super` 语义；
- 默认方法体能访问哪些成员；
- 子接口重新声明父接口的方法时，能否把参数改为可省略，如在 `func f(x: int)` 之上写 `func f(x: int = ...)`。这等于为少传实参的调用提供默认实现，现在报编译错误。

#### 函数类型与 lambda（§3.11）

lambda 的三条关键限制都直接对应真实语料的用法分布：

- **禁止给捕获的局部变量赋值**：v1 语料方法体里的 961 个 lambda 中没有一个改写外层局部变量，经成员或下标赋值的 15 处改的都是对象或容器（[语料计数](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/corpus-counts.md)）。v2 的 3,518 个 lambda 里仅确认 8 处直接改写捕获变量，其中还包含因此漏掉累加的缺陷（[v2 复跑](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/v2-rerun.md)）。
- **lambda 默认参数依赖函数类型的可选参数**：v1 的 979 个 lambda 里，命名的有 18 个，带参数默认值的为 0 个。v2 检出 10 处带默认参数，其中 5 处经 `.call()` 少传实参且全在 `godots`（4 处删掉默认值即可，1 处用于适配任意信号）。样本外的 GodSVG 还有 1 处经存下的 Callable 少传实参。因此 lambda 默认值随函数类型的可选参数一起加入。
- **协程方法作为值**：[语料计数](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/corpus-counts.md)中协程方法作值使用 91 处，其中 60 处直接用于 `connect(名字)`。

#### 函数类型的可选参数（§3.9、§3.11）

函数类型只记末尾哪些参数可省略，不记默认值，省略时由被调函数求值。这与 GDScript 的运行时行为一致：函数经 `Callable` 被少传实参时，补上的是被调函数自己的默认值（[引擎事实](engine-facts.md)）。Dart 的函数类型也这样做，`void Function(String, [bool])` 不带值。它的子类型规则与本语言相同，目标允许的每种实参个数都须接受（[Dart 子类型规则](https://github.com/dart-lang/language/blob/main/resources/type-system/subtyping.md)）。

另外两种做法都有已知的坑。C#、C++ 和 Swift 从接收者的静态类型取默认值，经基类、接口或委托调用时会悄悄用错值，C# 为此专门设了警告 CS1066 和 CS9099（[C# 标准](https://github.com/dotnet/csharpstandard/blob/dce7bdbaa8b508adfb50889fe9eaa29e33c85b26/standard/expressions.md#L587)）。本语言的接口因此只写 `= ...`，不写值。TypeScript 只查一半：源的必填个数不超过目标的参数总数即可。于是 `(a = 1) => a` 能经 `() => number` 当作 `(a: string) => number` 使用（[TypeScript#13043](https://github.com/microsoft/TypeScript/issues/13043)）。本语言两个条件都查，第二步就会拒绝。

`bind` 是 Godot 特有的难点。TypeScript 的 `bind`、Python 的 `functools.partial` 和 Haxe 都把绑定值放在前面，绑定时就定下它填哪个参数。引擎却把绑定值接在调用方的实参之后，函数有默认参数时，绑定值落在哪个参数就取决于调用方传几个。原先的规则是"去掉末尾 n 个参数"，只在调用方填满默认参数时与引擎一致。语料 v2 的 bind 中，调用方不填默认参数的有 22 处，填满的有 17 处，两种都真实存在（[语料计数](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/corpus-counts.md)）。现在的规则按引擎的顺序算出所有可行的调用个数。`spawn(kind := 0, count := 1).bind(3)` 不传实参时 3 填进 `kind`，传一个时填进 `count`。这个含义确定，所以不加限制，留给 lint。

还否掉了一种更小的改法：只在 `connect` 里按引擎顺序检查，别处保留旧规则。那样同一个 `f.bind(x)` 写在 `connect` 里能过，存进变量就过不了。方法值原先也有这种分裂：场景连接一直算默认参数，代码里的连接不算。语料 v1 有 12 处、v2 有 74 处把方法值直接连到传参更少的信号上，如 tanks-of-freedom-ii 把无参的 `close_requested` 连到 `toggle_radial_menu(context_object=null)`（`scenes/board/board.gd:165`）。

#### `Callable` 的显式转换在第一版之后（§3.11）

`Callable` 向函数类型的显式转换留待第一版之后。迁移过来的 `Callable` 参数一旦写成函数类型，引擎传入时就已会逐次检查调用。真正需要显式转换的只剩静态类型仍为 `Callable` 的值，例如 GDScript 类成员和方法名在运行时才确定的 `Callable(obj, name)`。这类场景很少，且当前仍可直接用 `call()` 调用。

后续预定的写法是 `(func(int) -> bool).try_from(c)`，返回 `Result`，与其余值转换保持一致。转换时会校验对象存活与参数个数，目标为具名方法时还会核对参数类型，后续每次调用照常检查实参和返回值。转为 `async` 函数类型报编译错误（隐式 `async` 另待后续加入）。不用 `as` 是因为它只面向对象且对 lambda 只能检查参数个数；不用 `is` 收窄则是因为失败时说不出原因。

### §4 声明

#### 类、抽象类与内部类（§4.1）

内部类可以直接按裸名解析外层类的静态函数和静态变量。这与 Java、C#、Kotlin 一致，也不会改变 GDScript 已有解析结果的含义；GDScript 不解析它们只是自身查找实现的局限。在引擎基类常量方面，[实现覆盖调查](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/implementation-coverage.md)显示有 4.9% 的文件以裸名引用它们。

抽象类与内部类在真实语料中的分布如下（[语料计数](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/corpus-counts.md)）：

- **抽象类**：v1 有 28 个 `@abstract` 类，其中 25 个在 `gdUnit4`；v2 增至 98 个，分布在 18 库（[v2 复跑](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/v2-rerun.md)）。文件级抽象类里有 17 个仅含抽象方法，可以直接改写为接口；但 `godot-open-rpg` 的 `BattlerAction` 同时带有导出字段和抽象协程，无法简化成接口。
- **内部类**：2,800 个文件中有 121 个含内部类，共 192 个（游戏仓库 19 个文件、37 个内部类）。其中 7 个嵌套在内部类中，10 个把 `extends` 写在类体开头。经 autoload 访问内部类的 175 处中，有 168 处集中在 `godot-open-rts`（如 `Constants.Match.Units.PROJECTILES`）。

#### 可见性（§4.2）

在[成员可见性调查](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/member-visibility.md)中，名字可判定的类成员约有 25% 会在其他文件里被访问。

#### 补上 `pub` 的快速修复（§4.2、§10.4、§10.5）

补上 `pub` 的快速修复默认自动应用：它完全可还原，而且在 GDScript 里原本所有成员都对外可见。

- **字面量名字改用强类型引用**：当字面量名字指向本类方法或信号时，在 `call_deferred`、`Callable(o, "m")`、`connect`，以及作为语句的 `call` 和 `emit_signal` 中，优先改为方法值或信号本身。后两者作为表达式时的结果与改写后不同，其余按名访问则直接补 `pub`。
- **覆盖可见性不一致**：`pub` 补在缺失的一侧。若补在基类上，基类的其他覆盖会随之报错，各自套用同一修复即可。

#### 常量（§4.5）

并非所有常量都能在编译期算出字面值。向量分量 `real_t` 随单双精度构建不同而变，`sin` 等数学函数的末位也随平台数学库漂移。因此，涉及引擎类型和返回 `float` 的数学函数的常量不在字节码里写死，而是推迟到类加载时求值一次；编译期仍会排除所有因值出错的情形。

[常量表达式调查](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/constant-expressions.md)验证了这一划分的合理性：3,094 个常量里，2,920 个无需计算、141 个是标量计算、仅 33 个需要引擎求值。注解参数引用的 7 个常量全是字面量，enum 值没有用到常量，规范排除的写法一处未见。到 v2 语料中，15,151 个常量里需非标量求值的增至 365 个，分布在 49 库（[v2 复跑](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/v2-rerun.md)）。

通过实例读取常量时，GDScript 会直接折叠为常量而不求值实例，导致 `make().MAX` 根本不调用 `make()`（`modules/gdscript/gdscript_analyzer.cpp:4890-4891`，`modules/gdscript/gdscript_compiler.cpp:255-256`）。悄悄丢掉调用属于规范 §1.2 所说的错误结果，Petrichor 选择照常求值实例。唯独实例只是单个名字时跳过求值：读名字本身没有副作用，经 autoload 名字读常量又很常见（如 `Game.MAX_SPEED`）。如果强行求值，既会让每次读取多查一次 autoload，又会让 `const X = Game.N` 变成编译错误。

#### 函数与剩余参数（§4.6）

在[第二次纸面迁移](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/paper-migration-2.md)中，引擎虚函数覆盖省略参数类型标注出现 6 处。

剩余参数在[语料计数](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/corpus-counts.md)中有 78 个，其中 76 个位于 `gdUnit4` 的断言接口。缺少调用处展开语法时，很容易误把整个数组当作单个实参传下去，例如 Pixelorama 的 `_debug_msg` 调用 `_prefix_print`（`src/Classes/ObjParse.gd:59-66`）。

带剩余参数的方法作值时暂定型为 `Callable`，与引擎变参方法作值时一致（GDScript 的 Callable 本就不携带签名）。日后若要在函数类型里加入剩余参数，必须先界定 `func(...Array[int])` 能否当作 `func(int, int)` 使用。

#### 静态成员（§4.8）

[语料计数](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/corpus-counts.md)中共有 188 处 `static var`，其中 29 个是没有初始化式的类类型变量（如 `static var instance: DMPlugin`）；`_static_init` 则有 4 个。GDScript 静态初始化顺序与环的既有问题见 [godotengine/godot#104161](https://github.com/godotengine/godot/issues/104161)、[#82141](https://github.com/godotengine/godot/issues/82141) 与 [#104588](https://github.com/godotengine/godot/issues/104588)。

编译器照收 `@static_unload` 注解，但不会真的卸载类。GDScript 4.7 自身也从不释放脚本：脚本缓存会持有每个加载过的脚本直到进程退出（`modules/gdscript/gdscript_cache.h:90`），其官方文档也把这记为缺陷（`modules/gdscript/doc_classes/@GDScript.xml:818`）。真实代码也几乎用不到它：v1 和 v2 合计 28,162 个 `.gd` 文件中，只有 gdUnit4 的 `GdUnit4CSharpApiLoader.gd` 写了这一注解（复制在 7 个仓库里）。真要做卸载，必须先界定类何时不再被使用（包括内部类与热重载）；确实需要的项目可开 issue 讨论。

### §5–§7 表达式、生命周期与场景模型

#### 表达式与收窄（§5.1–§5.4）

在语料和迁移样本中，`godot-open-rts` 用 `"hp" in unit` 检查单位成员 16 次；[第二次纸面迁移](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/paper-migration-2.md)出现 `(x as T).unwrap()` 向下转型 16 处，因成员不收窄而新增的局部变量每千行 4.4 个。

#### 真值判断的快速修复（§5.4）

真值判断的改写必须忠实保持 GDScript 中 `if x:` 的原义，即判断 `x` 是否不是该类型的零值或空值，按静态类型分别处理：

- **非空对象**：改为 `is_instance_valid(x)`。非空类型 `T` 仍可能持有已释放对象，而 `x != None` 在 Petrichor 中是编译错误。
- **向量、`Color`、变换等**：改为 `x != T()`，不写 `ZERO`。引擎判定真值时比较的是默认值，而 `Projection.ZERO` 并不是默认值。
- **`int?`、`float?`、`String?`**：分别改为 `x.unwrap_or(0) != 0` 和 `x.unwrap_or("") != ""`。若直接写 `x != None`，`Some(0)` 就会被误判为真。
- **`bool?`**：改为 `x == true`，让 `None` 和 `Some(false)` 都为假，与 GDScript 的 null 和 false 保持一致。
- **`Variant`**：不提供修复。严格保义的 `bool(type_convert(v, TYPE_BOOL))` 并非惯用写法，而 `v != None` 遇到 `0`、`""` 或空值时语义会变化。
- **其余值类型的 `T?`**（如 `Vector2?` 和容器的 `T?`）：暂无修复。

#### 循环变量标注子类（§5.7）

`for player: AudioStreamPlayer in get_children()` 是常见写法，`get_children()` 的元素类型却只是 `Node`。迁移工具在 deck_builder_tutorial 中留下的 60 个隐式向下转换里，这种循环变量占 44 个（[迁移工具调查](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/tool-migration.md)）。GDScript 对它每次迭代做一次带检查的赋值，元素类型不符时报运行时错误（[引擎事实](engine-facts.md)）。C# 的 `foreach` 同样把每个元素显式转换为循环变量的类型。

这不违背"不做隐式向下转换"（§3.14）：写在 `for` 上的类型就是作者显式的要求，不是赋值时悄悄收窄。跳过类型不符的元素会与 GDScript 悄悄不同；让迁移改写成 `as` 加判断，写法更长，出错时的行为也变了。所以按 GDScript 的做法检查，不符时报程序错误。元素为 `Variant` 的情形不在此列，它属于从 `Variant` 的转换。

#### 遍历对象（§5.7）

Petrichor 要求可遍历类自行定义 `_iter_init`、`_iter_next` 和 `_iter_get` 三个方法，也不支持遍历 `PackedDataContainer`。GDScript 编译时只查 `_iter_get`，缺少另两个方法要到运行时才报错；再加上编辑器构建中 `Object` 自带这三个虚函数，`for x in node` 竟然也能通过编译（[引擎事实](engine-facts.md)）。引擎内置类里只有已经弃用的 `PackedDataContainer` 定义了这组方法，因此不必兼容。

自定义迭代在真实项目中也极其少见：[覆盖率调查](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/implementation-coverage.md)里定义 `_iter_init` 的类为 0 个，v2 语料中也只有 6 个类，分布在 5 库（[v2 复跑](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/v2-rerun.md)）。

#### 生命周期与析构（§6.2–§6.7）

节点初始化和析构相关的引擎暗坑汇总在[暗坑清单](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/pitfalls.md) G-02、G-03、G-04、G-05、G-09、G-10。

[语料计数](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/corpus-counts.md)中，在 `_ready` 里把节点登记到 autoload 成员的写法有 6 处（5 处在 Ring），`NOTIFICATION_PREDELETE` 出现 40 次（32 次在 `gdUnit4`）；v2 语料中两者分别增至 64 处（17 库）和 79 次（32 库，[v2 复跑](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/v2-rerun.md)）。

#### 等待就绪的 setter（§4.4、§6.5）

场景给导出赋值时节点的子节点还不可用，要同步子节点的 setter 得等到就绪。GDScript 的通行写法是在 setter 里写 `if not is_node_ready(): await ready`：[属性访问器调查](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/property-accessors.md)中含 `await` 的 setter 有 30 个，其中 25 个是 `await ready`；v2 语料中分别增至 186 个（25 库）和 174 个（21 库，[v2 复跑](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/v2-rerun.md)）。迁移工具在 deck_builder_tutorial 中遇到 12 个（[迁移工具调查](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/tool-migration.md)）。

GDScript 调用 setter 后丢弃它的返回值，setter 首次挂起时写入处就接着执行；`ready` 信号在 `_ready` 返回之后发出，setter 这时才继续（[引擎事实](engine-facts.md)）。这与 `detach` 相同，所以 setter 可以声明为 `async`，迁移这些 setter 只需补上 `async`。挂起点写在代码里。多次写入、节点提前释放和任务失败都按协程已有的规则处理，不另设规则。getter 必须当场给出值，所以仍不能是协程。

代价是写入处看不出 setter 稍后才完成，`async` 只写在声明上。GDScript 中同样如此。

#### `_init` 参数（§6.2）

Petrichor 不为 `Node` 和 `Resource` 脚本的 `_init` 另设必须无参的特例。引擎经无参途径构造时，通用规则已经会报程序错误，场景里的不当用法则会在编译期直接拦截。

真实代码里带参 `_init` 并不少见。[语料计数](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/corpus-counts.md)统计了 28 个带参 `_init` 的 `Node` 脚本和 2 个 `Resource` 脚本，多数含必填参数，如 `godot-open-rts` 的 11 个动作节点由代码以 `Moving.new(p)` 构造。v2 中更增至 109 个 `Node` 脚本和 89 个 `Resource` 脚本，其中 `Resource` 分布在 30 个库（[v2 复跑](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/v2-rerun.md)）。

#### 释放时机（§6.8）

对象释放的时机用户看得到，因此 Petrichor 按词法作用域释放局部变量和临时值，与 C++、Rust 保持一致。这一选择吸收了多门语言的实践经验：

- **GDScript**：GDScript 晚释放的值在代码里早已访问不到，只有析构副作用和 WeakRef 看得出差别。它源码中在循环内逐轮清空的代码至今注释留作 TODO（[引擎事实](engine-facts.md)），说明晚释放本身就是未完成的实现。
- **Swift**：早期允许在最后一次使用后提前释放，但 Swift 5.7 改为锚定在作用域结束，优化器只在析构屏障之间提前（[Swift 论坛](https://forums.swift.org/t/what-is-the-current-status-of-lexical-lifetimes/76587)）。
- **Rust**：Rust 2021 的 `if let` 把头部临时值带入 `else` 块，导致锁在 `else` 执行期间一直被持有；Rust 2024 改为进入 `else` 之前释放（[LWN](https://lwn.net/Articles/1002456)）。

#### 场景定型与差异结构（§7.1–§7.2）

场景静态定型的可行性来自[场景结构调查](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/scene-structures.md)对游戏仓库的普查：2,023 处字面量节点路径里，没有一处在不同场景结构下解析为不同类型。v2 语料检出 60 处类型差异（向下路径占 44 处，多数只是换了附加脚本），标注共同基类或接口即可承接（[v2 复跑](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/v2-rerun.md)）。

静态检查还能揪出任何场景里都不存在的 `$X` 改名遗漏：比如 `tps-demo` 的 `door.gd:6` 写着 `$DoorModel/AnimationPlayer`，而 `door.tscn` 里的节点实际叫 `DoorModel2`。调查中只在部分场景结构缺失的路径分为两类：

- 7 处来自模板或占位场景缺节点，如 `tanks-of-freedom-ii` 的 `base_ground.tscn` 被 preload 作占位地块，却缺了 `tile.gd` 访问的 `$"mesh"`；
- 8 处通过 `get_node_or_null` 处理可选子节点，如 `Starter-Kit-Racing` 里两种赛车的尾迹。

#### 向上访问与资源共享（§7.3、§7.6）

移动节点时不检查 `owner`，因为先读 `owner` 再移走自己是常见且合法的操作，Ring 的 `DeathComponent` 就先读 `owner`，再把自己移到前景层。

共享子资源被意外改写的典型案例出现在 `godot-open-rpg` 的 `interaction_popup.tscn`：内嵌的 `CircleShape2D` 没有设为局部于场景，`interaction_popup.gd:30` 修改了它的半径，而 `main.tscn:484-486` 中只有一个实例将其设为 64。资源环相关问题见[暗坑清单](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/pitfalls.md) G-19、G-20、G-34。

向上访问在[语料计数](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/corpus-counts.md)中的分布如下：

- 调用自身的 `get_parent()` 共 174 处，其中 62 处直接访问成员（分布在 46 个文件），判空仅 3 处（含 `beehave` 在挂上父节点前运行的 setter）；v2 中判空增至 34 处，分布在 17 库（[v2 复跑](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/v2-rerun.md)）；
- 经 `owner` 直接访问成员共 49 处，分布在 21 个文件。

### §8–§13 错误、协程、引擎边界与工具链

#### 程序错误与断言（§8.1–§8.3）

GDScript 运行出错后会放任调用方继续执行：debug 构建返回类型默认值，release 构建直接返回 `null`。在静态类型语言里，这两种兜底值都会把错误伪装成合法数据继续传播，因此程序错误必须中止整条调用链。

语法上，短路返回 `?` 不配可空链 `?.`，防止 `x?.y` 的语义随着 `x` 是 Option 还是 Result 来回摇摆。

`assert` 的消息表达式只在条件失败时求值，与 Python、Java、Rust 保持一致。[语料计数](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/corpus-counts.md)中的 595 个 `assert` 条件基本都是纯查询，实参含 `await` 的为 0 个；539 条断言消息无一依赖条件为真时求值。GDScript 把消息求值放在判断之前，副作用又只在 debug 构建触发，依赖它的代码在 debug 和 release 构建下会跑出不同结果。

#### 诊断（§8.4）

语言规范不会随编辑器或导出游戏一起分发，用户也经常在离线环境里排查问题。编译期和运行时的诊断都应把来龙去脉讲清，不能靠丢一个规范章节号打发读者。

#### 子任务的归属（§9.3）

为子任务建立归属关系，是为了让取消能够顺着结构传下去。`Task.any` 超时后需要立刻停掉其余落选任务，`cancel()` 打断当前行为时也要停掉正在等待的 `move_to`。如果没有归属，运行时无从判断一个任务还有没有别人在等。Rust 同样采用父 future 拥有所等待子 future 的模型。

不过 Godot 的协程还有第二个主人，即运行它的对象，而对象会被显式释放。两重归属只会在一种情形下冲突：等待方的对象已被释放，但子任务所属的对象仍然存活。GDScript 中此时子任务会照常跑完，相同写法必须与其一致；归属的价值本就来自主动取消，因此对象释放触发的取消不向下传播。

#### 协程与信号（§9.1–§9.7）

协程的重入与对象释放暗坑汇总在[暗坑清单](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/pitfalls.md) G-11、G-12、G-13，以及上游提案 [godot-proposals#8838](https://github.com/godotengine/godot-proposals/issues/8838) 和 [#6243](https://github.com/godotengine/godot-proposals/issues/6243)。

语料中的两类相关统计如下：

- **lambda 中的 `await`**：[语料计数](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/corpus-counts.md)里 979 个 lambda 有 35 个含 `await`（其中 33 个在 `gdUnit4` 测试里）；v2 的 3,518 个 lambda 中有 125 个，分布在 11 库。
- **`await` 多参数或未知签名信号**：[await 信号结果调查](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/await-signal-results.md)中，1,356 个 `await` 只有 1 个使用了多参数信号的结果，也只有 1 个使用了签名未知的 `Signal` 结果。后者位于 `godot_dialogue_manager` `:1956`，经 `RefCounted` 接收者等待 C# 两参数信号。v2 中使用多参数信号结果增至 24 处，分布在 9 库，其中 15 处接收者签名静态已知（[v2 复跑](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/v2-rerun.md)）。
  - 这些代码拿到数组后都在给各项起名字：godot-tiny-mmo 把 `args[0]` 到 `args[3]` 逐个存进带类型的局部变量（`source/client/gateway/gateway.gd:342`），godots 把数组包进类再写访问方法（`src/http_client.gd:29`），material-maker 直接读 `result[1]` 和 `result[3]`（`addons/material_maker/engine/loader.gd:81`）。信号的声明已经给出每个参数的名字和类型，所以结果按参数名给出字段。
  - 它转为 `Variant` 时仍是 GDScript 给出的数组（[引擎事实](engine-facts.md)）。按 struct 的规则编码成字典，会让同样的写法悄悄不同。

#### 协程的快速修复（§9.1）

- **补上 `async`**：GDScript 中只要函数体含 `await` 就是协程，因此提供补写 `async` 的修复；但它会连带影响调用方和覆盖方法，不自动应用。覆盖须保持 `async` 的错误也给出相同修复。签名不允许声明为 `async` 的地方（如 `_init`、getter 和覆盖非 `async` 方法的函数）不提供该修复。
- **独立调用协程**：GDScript 里作为语句调用的协程会独立运行，因此 `.detach()` 排在第一并自动应用；`await` 会把控制流改为等待，排在第二。

#### 引擎边界（§10.2–§10.6）

引擎用 `RequiredResult` 标注的 API 仅约 50 处，其中 29 处在 `Tween` 上。与此同时，引擎传入虚函数和信号回调的对象几乎总是非空，因此默认按非空类型建模并配合入口检查，避免每个 `_input` 和信号处理函数都重复判空。

整数宽度的隐患覆盖面则大得多：引擎 API 中窄于 `int64` 或无符号的方法参数超过 3,700 个、属性约 460 个。例如 `uint32` 类型的 `collision_layer` 一旦写入 `-1`，就会静默回绕为 `4294967295`。

#### `float` 实参的快速修复（§10.3）

该修复只在期望类型为 `float` 处自动应用。GDScript 的 `min`、`max` 和 `clamp` 原样返回选中的实参本身，返回类型也随实参变化，如 `str(min(1.5, 1))` 得到 `"1"`。

`snapped` 另有特殊规则：步长为 `int` 时 GDScript 返回 `int`；步长为 `0` 时则返回截断后的原值，例如 `snapped(1.6, 0)` 得到 `1`。因此只有当步长是非零整数字面量时，这项修复才会自动应用。

#### 引擎 API 的 `Result` 版本（§10.3）

为引擎 API 提供 `Result` 返回值时，选择在原类上另增 `try_` 版本，而不是直接改原 API 的返回类型。[语料计数](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/corpus-counts.md)中 `FileAccess.open` 调用 125 处，只有 21 处随后读取错误；改动原签名会让迁移来的每处调用都被迫改写。之所以不另立独立模块，是因为那样既多一个全局名字，用户在 `FileAccess` 下也找不到它。

为了降低与引擎升级撞名的风险，引擎类上只添加按固定前缀从既有成员派生的名字（如 `try_from`、`try_open`）。Godot 4.7 的引擎 API 里只有 `Mutex.try_lock` 和 `Semaphore.try_wait` 用了这个前缀；几乎每个 4.x 版本都会给 `FileAccess` 和 `DirAccess` 加新方法，但目前没有一个以 `try_` 开头。即便日后与新版引擎成员撞名，构建阶段也会直接报错。其他新功能的安放位置见[实现架构](architecture.md#5-推进顺序)。

#### 按名访问（§10.4、§10.5）

引擎无法区分按名访问的调用方是谁。只要对引擎暴露了私有成员，GDScript 的无类型访问就能随意读写它，私有成员的不变量就不得不防备外部篡改。[按名访问调查](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/by-name-access.md)的数据也支持按 `pub` 显式暴露：

- 引擎按名访问的脚本成员几乎全来自场景信号连接、导出和 `@rpc`，编译器本来就读取了这些信息；
- 代码里指向本脚本的字面量名字每千行仅 0.5 处，多数可改用方法值。v2 中有 2,750 处，分布在 146 库，不再少见，但依然能机械找出（[v2 复跑](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/v2-rerun.md)）；
- 要求这些成员标记 `pub` 每千行只多写约 16 个，且几乎都能由编译器定位并给出快速修复；
- 导出变量和 `@rpc` 函数不必再写 `pub`（§4.2）：不存在私有的导出，注解本身已经声明对外可见，迁移代码也能少改一处；
- Petrichor 的 `connect` 和 `emit` 同样会经引擎按名核对信号，因此私有信号采用改名注册。

#### 引擎属性的读写（§10.5）

访问引擎属性时，Petrichor 直接调用底层的 getter 和 setter，省去每次的按名查找和 ClassDB 锁。这与 C#、godot-cpp 和 Rust 的绑定一致，也与 Petrichor 调用引擎方法保持同一路线。

GDScript 按名字读写引擎属性，顺带让对象脚本的 `_get` 和 `_set` 能够拦截它们。但这只是按名查找这条老路径的副作用，并非有意设计：GDScript 直接调用 `label.set_text(s)` 时就不经拦截，Object 文档也明确写着它们不处理内置属性。

真实项目也几乎不依赖这一副作用：[语料计数](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/corpus-counts.md)中的 8 个 `_get`、`_set` 全都只处理本类的动态属性。Ring 附带的 Dialogic 插件有一处拦截了 Label 的 `text`，从 Petrichor 写入时改用 `set` 即可；v2 抽样的 50 处样本中也只有 1 处监听 `Node3D` 的 `transform`（[v2 复跑](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/v2-rerun.md)）。通过按名访问、检查器、动画和 Tween 写入时仍照旧经过拦截。

#### 字面量名字的检查（§10.4）

按字面量名字访问成员时，找不到就直接报错，与点号访问保持一致。例外只按三条原则划出：

- `Object` 是规范指定的动态访问入口；
- 运行时查询本来就要处理目标不存在的情形；
- 引擎按实例列出的属性族都带 `/`，而 ClassDB 注册的属性名都不带。

按实例列出却不带 `/` 的属性大约分布在 20 个类中，例如 Bone2D 的 `length` 和每个对象的 `script`。把它们写成 `.名字` 同样报错，须经 `Object` 动态访问。

#### autoload 名可写在类型中（§11.1）

Petrichor 沿用 GDScript 的做法，允许把 autoload 名直接写在类型位置（[引擎事实](engine-facts.md)）。因为编辑器拒绝与全局类同名的 autoload（[引擎事实](engine-facts.md)），autoload 脚本往往不写 `class_name`，autoload 名就是该类唯一的名字。[语料计数](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/corpus-counts.md)中有 35 处这类用法（33 处在 Pixelorama），v2 中增至 212 处，分布在 27 库（[v2 复跑](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/v2-rerun.md)）。

查找规则上，Petrichor 与其余名字保持一致，作用域中更近的名字优先；而 GDScript 会让 autoload 优先。为了防止两者悄悄指向不同类型，当常量与 autoload 同名时，在类型位置写这个名字会直接报编译错误。

#### 跨语言、线程与工具链（§11.2、§11.3、§12.3、§13.2–§13.4）

- **跨语言继承与连带迁移**：跨语言继承的引擎限制见[暗坑清单](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/pitfalls.md) C-17。在[第二次纸面迁移](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/paper-migration-2.md)中，Ring 的 `Brick` 和 `Enemy` 因此连带迁移了 5 个子类；全部 230 处修改中有 197 处属于机械改写。
- **GDScript 访问静态方法与 enum**：GDScript 无法调用 Petrichor 的静态方法，也不能以其 enum 作类型。[语料计数](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/corpus-counts.md)中，游戏仓库 514 个具名类里有 97 个受第一方访问影响（静态方法 77 个、作类型 29 个，其中 82 个在 `godot-tiny-mmo`）。v2 中游戏的 4,834 个具名类里有 448 个受影响，分布在 121 个游戏仓库中的 67 个（[v2 复跑](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/v2-rerun.md)）。其中静态方法涉及 302 个（51 库），作类型涉及 186 个（55 库），另有 35 个未命名 preload 脚本受影响。根本解法在引擎上游，静态方法一项已有提案（godot-proposals#14614）；在此之前以及旧引擎版本上，迁移工具会把这些使用方列入连带迁移。
- **多线程使用频率**：[语料计数](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/corpus-counts.md)中 `Thread.new` 仅 5 处、`WorkerThreadPool` 2 处；v2 中两者合计 100 处，分布在 34 库（[v2 复跑](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/v2-rerun.md)）。
- **续行缩进与测试框架**：真实代码里括号续行大多只缩进一层，而官方风格指南要求两层。由于 Petrichor 的类不能继承 `gdUnit4` 和 `GUT` 的 GDScript 基类，测试先留在 GDScript 中，通过 `pub` 成员驱动（v2 的 200 个仓库里有 23 个写了测试，全部使用这两者）。GDScript 本身没有内置测试运行器，Petrichor 的测试运行器同样留到第一版之后。

## 3. 考虑过但没有采用的方案

表中的“规范 v0.1”指上一版规范，保存在 git 标签 `spec-v0.1` 中。

| 方案 | 不采用的原因 | 现行规则 |
|---|---|---|
| 以引擎模块交付（规范 v0.1） | 暗坑清单中没有只有模块才能修复的条目；GDExtension 可以用官方编辑器和导出模板 | §1.3 |
| 窄整数类型 i32、u8 等（规范 v0.1） | 迁移成本最大的一项（[纸面迁移](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/paper-migration.md)）；脚本很少在意数值类型的性能 | §3.2 |
| 推断函数签名（规范 v0.1） | 签名是对外的约定，不应随函数体变化 | §4.6 |
| 以 trace 为单位编译的 JIT | 类型在编译期已知，追踪运行带来的类型特化本来就有。事先生成没有可追踪的运行，两条路就不能共用一个生成模块 | §12.2 |
| 内层块遮蔽外层的局部变量，如 C++、Rust、Swift | GDScript 禁止，迁移来的代码中不会出现；禁止以后可以兼容地放开 | §4.6 |
| 语言层面的跨语言继承 | 依赖 GDScript 的内部实现 | §11.2 |
| 可空链 `?.`、默认值 `??`、后缀 `!` | 与 Result 的短路返回合用时，`x?.y` 的含义要看 x 的类型；Option 的操作用方法就够了 | §3.4、§8.2 |
| Swift 式的 `try` 覆盖整条表达式 | 要按类型决定哪些子表达式被解开；Rust 的后缀 `?` 更直接 | §8.2 |
| Result 也支持链式的 `?.` | 两种类型嵌套时，一个运算符无法说明处理的是哪一层 | §8.2 |
| 允许对 `Variant` 取下标和用 `in`，运行时检查，与 GDScript 相同 | 先转换的摩擦是有意的，它把结构固定的数据推向解码为 struct；放开后报错少了，凑合的写法也不再被指出 | §3.13 |
| 在属性中另设 `apply:` 块，存放须等到节点就绪才执行的代码 | 让用户思考逻辑在何时执行、执行几次；`async` setter 中的 `await ready` 已能表达 | §6.5 |
| 就绪前由编译器自动挂起 setter，挂起点由静态分析确定 | 挂起点不写在代码里，读 setter 看不出它分两段执行。设计还在两处偏离 GDScript：再次写入时取消前一次挂起的调用，并在 `_ready` 之前恢复，所以迁移不能照搬手写的 `await ready` | §4.4、§6.5 |
| 就绪前只记下 setter 的原始输入 | 先变换再存储的 setter（如给角色名加【】）就绪前会读到未变换的值 | §6.5 |
| `@late`（类似 `lateinit`） | 需要它的写法很少见；父节点配置子节点已由祖先赋值的规则覆盖 | §6.2 |
| 在最后一次使用后释放，如 Objective-C ARC 和 5.7 之前的 Swift | 守卫写法失效，要另给延长寿命的写法，如 `objc_precise_lifetime` | §6.8 |
| 不规定释放时机 | 挂起的协程和长循环会一直持有用过的临时值 | §6.8 |
| `match` 的被匹配值保留到分支结束，如 GDScript | 与 `if` 的条件不一致；分支要用的部分已由绑定持有 | §6.8 |
| 子类只能看到 `pub` 成员 | [第二次纸面迁移](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/paper-migration-2.md)中 102 处 `pub` 有 29 处只是为了子类 | §4.2 |
| 公开标记用 `@public` 注解 | 写得频繁，[成员可见性调查](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/member-visibility.md)中每千行至少 22 处 | §2.4 |
| 文件顶层的类型直接成为全局名 | 打破“文件就是类”，需要另一张全局名表 | §2.4 |
| 接口的交集类型 `@owner(Node2D & Combatant)` | 新概念；允许引擎属性满足接口的属性要求即可 | §3.9 |
| 追踪式 GC，或闭包自动弱捕获 self | 与 GDScript 的行为悄悄不同 | §3.11、§6.7 |
| 像 GDScript 那样允许给捕获的变量赋值，只给出警告 | 赋值既不改变外面的变量，也影响不到下一次调用，与字面意思相反。语料中确有因此漏掉外层累加的缺陷 | §3.11 |
| 方法值使 RefCounted 接收者保持存活（Python、C# 等的做法） | 信号连接会留住监听者，即 C# 中遗忘的监听者造成的泄漏。RefCounted 把自身信号连到自身方法时成环。引擎的 `Callable` 不持有对象，接收者释放时连接随之断开 | §3.11 |
| 调用本语言的函数值像引擎调用 Callable 那样开始新的链 | 调用方会拿着返回类型的默认值这个假值继续执行，而直接调用同一个方法时调用方会中止 | §8.1 |
| 只按声明的类型判定调用链，`Callable` 变量不收窄 | `var f: Callable = func…` 的值明显是函数值，却被永久划为新链；以后再并入就是不兼容的改动 | §5.4、§8.1 |
| 按运行时的值判定调用链 | 同一个调用处，出错后调用方是否继续，要看值从哪里来，读代码判断不了 | §8.1 |
| 跨函数或经成员推断 `Callable` 的值 | 结果取决于分析做得多深；分析一改，已有程序的行为就跟着变 | §5.4 |
| 在移动节点时检查 owner | 节点移出 owner 的子树是合法操作 | §7.3 |
| 没有被引用的场景缺值时也算导出错误 | 运行时才确定路径的 `load` 看不到，无法断定场景不被使用 | §7.5 |
| `as` 转为内置类型时严格检查 | 与 GDScript 的构造转换悄悄不同 | §5.3 |
| Array 的 `<=`、`>=` 照引擎取 `not (b < a)`、`not (a < b)`，`<` 跳过比不出大小的元素 | 元素比不出大小时结果自相矛盾：`[NAN] <= [1.0]` 和 `[NAN] >= [1.0]` 都为真，`==` 却为假。`NAN <= 1.0` 也为假 | §5.2 |
| 未审核的引擎 API 中对象类型一律可空 | 引擎传入的虚函数参数和信号参数几乎总是非空，一律可空会让每个处理函数都先判空。少数为空的情况由入口检查报告 | §10.2 |
| int 与 float 混用的工具函数统一提升为 float | GDScript 原样返回实参，结果会悄悄不同 | §10.3 |
| `pop_front()` 在空数组上是程序错误 | 引擎在这里返回 null 而不报错，会悄悄不同 | §3.8 |
| `pop_at` 与 `pop_back` 一样返回 `T?` | 真实代码都按已知的下标用 `pop_at` 移出元素，不判断结果为空。返回 `T?` 让每处都要解开，而越界本身就是错误 | §3.8 |
| await 多参数信号得到无类型 Array，与 GDScript 相同 | 会丢失各参数的静态类型，结果改为按参数名给出字段 | §9.6 |
| await 多参数信号得到元组，或只用于 await 的解构绑定 | 按位置取值会留下 `r[3]` 这类写法，而信号的声明已有各参数的名字。元组是整套新写法，作为独立的功能以后再议 | §9.6 |
| 引擎属性照 GDScript 按名读写，对象脚本的 `_get`、`_set` 可以拦截 | 每次访问都要先问对象有没有脚本。经 GDExtension 问一次，就抵掉了直接调用的大部分收益；GDScript 自己调 setter 时也不经拦截 | §10.5 |
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
| 在自身上调用 `free()`、`set_script()` 是编译错误 | [语料计数](https://github.com/YYF233333/petrichor-corpus/blob/main/surveys/corpus-counts.md)中未出现，v2 也仅见 1 处自身 `free()` 和 2 处移除自身脚本。信号回调释放发射者、被调方释放调用方等间接释放无法静态判定，运行时检查本就省不掉 | §3.3 |
| 解码 struct 时拒绝不是字段的键 | 较新的发送方多带字段的消息、为多个读取方写的 JSON 会在入口处失败，而 struct 需要的字段其实都在 | §3.6 |
| 元素为本语言独有类型的容器上，`sort`、`min`、`max`、`hash` 等报编译错误 | GDScript 代码会调用它们，迁移来的代码就得改写；运行时可以用本语言的运算完成它们 | §3.8 |
| struct 字段的默认值读前面的字段 | 含义有两种：像类成员那样先求初始化式再赋具名实参，或像 Kotlin 的构造参数那样读传入的值。以后可以兼容地放开 | §3.6 |
| 给 String 的字符赋值，照搬 GDScript，或照搬后把截断改为程序错误 | GDScript 只取值首字符，空串则删去该位；多数语言的字符串不支持下标修改，仅 PHP 与之相近。v1 仅 1 处写 String，v2 抽样 50 处候选也仅 3 处替换字符。迁移工具保留编译错误并提示人工改写，非越界情形可改用 `erase` 与 `insert`（实现架构 §6.6） | §5.1 |
| 按调用处的静态类型取参数默认值（C# 的做法） | GDScript 由被调函数求值，经基类调用覆盖方法时用覆盖方法的默认值 | §4.6 |
| struct 的哈希跳过容器字段，或 struct 只有 `==` 和哈希而没有 `<` | 哪些字段参与由用户决定，而不是由实现决定。开箱即用时各个默认运算应当配套，以后可以用 struct 方法重新定义 | §3.6 |
| 禁止含容器字段的 struct 作 Dictionary 的键 | GDScript 允许以容器作键；只读的副本使保存的哈希始终有效 | §3.8 |
| 把原值在作键时设为只读 | `make_read_only` 不能撤销，调用方的容器，以及其他持有它的地方，从此再也不能修改 | §3.8 |
| 按 GDScript 在空数组上返回 null 的样子，让 `min()`、`max()` 返回 `T?` | 这个 null 是元素类型没有的值，GDScript 默默地把它传下去；程序错误指出哪里没料到空数组 | §3.8 |
| 编译器求出全部常量，包括引擎类型的运算 | 向量的分量随引擎的构建是 float 或 double，编译一次的字节码只能写死其中一种结果。编译器还要重写引擎的运算 | §4.5 |
| 编译期经引擎求出常量 | 编译器就要依赖引擎，编译器测试要么测不了常量，要么在测试桩中再写一遍求值 | §4.5 |
| 全部常量在类加载时求值 | 注解参数、enum 的值在编译期拿不到值，`10 / 0` 一类错误推迟到加载时才报 | §4.5 |
| 单引号和双引号字符串可以跨行，与 GDScript 相同 | 漏写右引号时错误报在远处；跨行的这类字符串少，能机械改写 | §2.2 |
| 内部类只解析外层类的常量、enum 和内部类，与 GDScript 相同 | GDScript 的限制是其查找方式的副产品；静态成员不需要实例，Java、C#、Kotlin 的嵌套类都能直接用 | §4.1 |
| 没有初始化式的对象类型静态变量从 None 开始，与 GDScript 相同 | 非空的类型没有 None 这个值；需要空值时写作 `T?` | §4.8 |
| 无类型的容器隐式转为带类型的容器，与 GDScript 的静态检查相同 | GDScript 运行时只有容器恰为该类型时才成功，隐式转换把这个一定会做的检查藏到运行时。显式的 `is` 写出了它 | §3.8 |
| 经无类型的视图写入不合元素类型的值时跳过这次写入，与引擎相同 | 程序带着没写进去的值继续运行；查找时引擎还把 1.5 截断为 1 | §3.8 |
| `as` 遇到 Variant 中不是对象的值时是程序错误，与 GDScript 的 debug 构建相同 | `as` 是可以失败的转换，类型不符时就是 None，`v is Node` 在这里同样为假 | §5.3 |
| 进入 Variant 的字面量也由元素定型 | 空字面量无从定型，`d[k] = []` 要写出类型。有元素时得到带类型的容器，之后经 Variant 写入其他类型的值是程序错误，GDScript 中则可以 | §3.13 |
| 编译期检查跨类的静态初始化环 | 要知道其他文件的初始化读取什么；这样的环每次运行加载这些类时都会报出 | §4.8 |
| 热重载时重新运行静态初始化，再按名字搬回旧值，与 GDScript 相同 | `_static_init` 的副作用（如登记）会再发生一次；只修改函数体时，正在运行的程序只换之后的调用（§12.4） | §4.8 |
| `new(...)` 跟随类的可见性，不看 `_init` 是否 `pub`，与 GDScript 相同 | 与其他成员不一致：不带 `pub` 的 `_init` 改签名时，调用方不只在本文件和子类中。类也无法只经自己的方法构造 | §4.2 |
| 一键应用本脚本中各种自动修复，跳过区间重叠的 | 不同修复会远距离互相影响：若同时把声明改为 `T?` 又给使用处加 `.unwrap()`，原本在 GDScript 能运行的代码会变成运行时错误。成熟工具都只按种类批量修复（Roslyn、IntelliJ 如此，VS Code 的 TypeScript fixAll 只收 3 种，rust-analyzer 无 fix-all，[TypeScript#29452](https://github.com/microsoft/TypeScript/issues/29452) 至今未做） | §13.2 |
| 声明可与引擎类或内置类型同名，近处的名字优先 | 同名声明只会误导读者；GDScript 也拒绝这类成员、对局部变量和参数报警告（不查函数）。具名 enum 的值和分支须经 enum 名访问（如 `enum Value { Vector2(v: Vector2), Color(c: Color) }` 的 `Value.Vector2`），不在此列 | §4.1 |
| 成员可与引擎基类的属性同名，遮蔽它 | 引擎按名读写先交给脚本实例。带 `pub` 时，动画、tween 和 GDScript 写的是这个成员，节点不受影响；不带 `pub` 时，同一对象上的同一个名字有两个含义 | §4.1 |
| 引擎基类的常量必须写出类名，如 `Object.NOTIFICATION_PREDELETE`，快速修复补上类名 | 引擎基类的属性、方法和信号都用裸名，只有常量例外；GDScript 代码中 4.9% 的文件要逐处改写 | §4.1 |
| 字面量名字的目标为 `RefCounted`、`Node` 等常用基类时也不检查 | 豁免的边界无从划定，拼错的名字就查不出了。动态访问先转为 `Object` | §10.4 |
| 像 C# 的 `dynamic` 那样，有一种成员和下标都在运行时绑定的类型 | 失败散到每一次访问，补全和重命名都失效。结构固定的数据解码为 struct，一次检查整棵树。C# 自带的 System.Text.Json 也不支持 `dynamic` | §3.13、§10.8 |
| 引擎按名找到的成员不带 `pub` 也暴露，由编译器收集 | 暴露后任何调用方都能按名读写，私有成员的不变量要防外部修改 | §10.5 |
| 全部成员对按名查找可见（C# 的做法） | 同上；GDScript 的无类型访问就是按名访问 | §10.5 |
| 实现接口的成员不写 `pub` 也视为 `pub` | 与继承的规则相冲突，要另加三条规则。读声明看不出它是不是 `pub`，要去翻类头和接口 | §3.9、§4.2 |
| 信号一律须带 `pub` | 只在文件内使用的信号也得公开，外部就能发出它们 | §10.5 |
| 私有信号由本语言自行派发 | 要重写连接语义（deferred、one-shot、释放时断开），还要与 `await` 和 `Signal` 值配合 | §10.5 |
| 读取私有信号的名字时去掉 `@` | 真实代码读名字多是为了按名用回去，如 gdUnit4 先 `has_signal(s.get_name())` 再断开。去掉后连接被悄悄留下。引擎打印 `Signal` 时也不经本语言 | §10.5 |
| 以 `try/catch` 等原语同步捕获程序错误 | 可预期的失败由 `Result` 表达。无代价捕获程序错误并接回正常控制流的做法都会被当作分支滥用，使被破坏的不变量继续传播 | §8.1 |
| 重命名时由引擎重存场景：改 `PackedScene._bundled` 后整篇保存，二进制文件也能改 | 整篇重写会让 4.7 之前保存的场景每个节点行都变动，多人合并冲突遍地；且 SceneState 不对扩展开放写入，`_bundled` 是未承诺的内部结构。仅用于二进制文件虽不添冲突但要多维护一套写回；v2 中 `.scn` 有 79 个（7 项目）、`.res` 有 544 个（40 项目），挂脚本比例未查 | §13.3 |
