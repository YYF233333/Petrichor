# Petrichor 语言规范（草案）

**v0.2 草案 · 2026-10-25** · English: [language-spec-draft.en.md](language-spec-draft.en.md)

本文按[设计决定](decisions.zh.md)重写语言规范，完成讨论后将取代 v0.1（git 标签 `spec-v0.1`）。本文与设计决定不一致时，以设计决定为准并修改本文。设计决定中尚未决定的问题，本文标为【待定】。

## 1. 概述

### 1.1 问题与目标

- 用动态语言写的大型 Godot 项目难以管理。GDScript 受历史兼容性约束，摆脱不了动态类型；即使脚本全部写成静态类型，节点路径、编辑器中连接的信号、动画轨道和资源的属性名仍然是动态的，因为场景中的静态信息到不了脚本里。改用 C# 则语言支持降为二等，跨语言桥接和工具链都有暗坑。
- 本语言是全静态类型的，只在引擎或第三方强加的地方出现动态值（§3.13）。静态检查延伸到场景：节点路径、场景中连接的信号、动画轨道、导出属性和场景资源都有类型，改名时连同场景一起修改（§7、§13）。
- 目标用户是会读、会改代码的开发者，项目规模中等以上，部分代码可以由 AI 生成。小项目用 GDScript 更合适。
- 以开新项目为主。第三方插件保持 GDScript，不要求迁移（§11）。
- 性能明显快于 GDScript 即可，不以 C# 为目标（§12.2）。

### 1.2 与 GDScript 的关系

- 表层语法沿用 GDScript：缩进、`func`、`var`、`extends`、`signal`、`@export`、`$X` 等。新增的构造按同一风格扩展（§2）。
- **语法硬规则**：与 GDScript 写法相同的构造，行为要么与 GDScript 一致，要么是编译错误并给出改法，不能悄悄不同。本文中每一处与 GDScript 不同的规则都满足这一条；规则说明中提到“与 GDScript 相同”时，指的是这一条所要求的一致。
- 本语言不承诺能直接编译 GDScript 源码。迁移由编译错误和快速修复引导（§13.4）。

### 1.3 交付形态

- 以 GDExtension 交付，实现 Godot 的脚本语言扩展接口（`core/object/script_language_extension.h:393, 493, 533, 596, 754`）。访问引擎的代码集中在一层薄的适配层中，保留以后改编为引擎模块的可能。
- GDExtension 可以装进官方编辑器，使用官方导出模板。代价是 Web 导出须启用“Extensions Support”，并配置跨域隔离响应头。

### 1.4 文档约定

- 定义合法程序和所需行为的规则具有规范性。理由、例子、§15 和附录 B 不具有规范性。
- 状态标记：
  - **【待定】**：尚未决定，对应[设计决定](decisions.zh.md) §10 中的一项；
  - **【已知限制】**：第一版不支持，限制程序合法性的规则仍具有规范性；
  - **【后续】**：以后可以兼容地加入，不属于第一版。
- **编译错误**指分析服务在运行之前报告、使程序不合法的错误。**警告**和**提示**不影响合法性。**程序错误**指 §8.1 定义的运行时错误。
- 文中的引擎行为都对照 Godot master（4.8-beta，提交 `469659e`）核对过，出处列在附录 A。这是核对基线，不是支持版本声明。
- 例子中的 `.pet` 扩展名是暂定的【待定】。

## 2. 词法与表层语法

### 2.1 文件与类

- 一个源文件就是一个类，与 GDScript 相同。文件可以以 `class_name 名字` 声明全局类名；没有 `class_name` 的文件经 `preload` 引用。全局名只来自 `class_name`（§4.1）。
- 类头依次可以有：类级注解、`class_name`、`extends`、`implements`。`extends`、`implements` 可以写在 `class_name` 的同一行，也可以各占一行：

  ```gdscript
  class_name Player extends CharacterBody3D implements Combat.Damageable
  ```

- 文件中可以声明内部类（`class 名字:`）、struct、enum 和 interface，它们都属于这个文件的类（§2.4）。

### 2.2 词法

- 缩进、注释、标识符、字面量和运算符与 GDScript 相同，另有以下增减：
  - 新增关键字：`pub`、`struct`、`interface`、`implements`、`async`、`None`。
  - 新增后缀运算符 `?`（§8.2）。GDScript 中 `?` 是语法错误（`modules/gdscript/gdscript_parser.cpp:3834-3835`）。
  - 前缀 `!` 仍是逻辑非（`gdscript_parser.cpp:2965-2967`）。
  - `null` 是 None 的别名，在所有位置的含义与 GDScript 一致：`x = null` 给 `T?` 赋空值，`x != null` 与 `x != None` 相同并带来收窄，传给 Variant 时引擎得到 NIL。把它赋给非空的 `T` 是编译错误，快速修复把类型改为 `T?`。提示建议新代码写 None。依据：[语料计数](surveys/corpus-counts.zh.md)中 `null` 每千行 22.2 处。
- 字面量：整数、浮点数、字符串（含三引号和 `r` 前缀）、`&"…"`（StringName）、`^"…"`（NodePath）、数组 `[…]`、字典 `{k: v}` 和 `{k = v}`、`true`、`false`、`None`，与 GDScript 相同；`None` 是 Option 的空值（§3.4）。字面量的类型见 §3.2 和 §10.7。

### 2.3 注解

- 沿用 GDScript 的注解，含义相同：`@export` 系列、`@onready`、`@tool`、`@icon`、`@warning_ignore`、`@rpc`、`@static_unload`、`@abstract`（§4.1）。
- 新增的类级注解：`@parent(T)`、`@owner(T)`（§7.3）。
- 注解写在声明之前，可以与声明同一行；`pub` 写在注解之后、`static` 之前（§2.4）。

### 2.4 声明的写法

- **struct**：`struct 名字:` 之下用 `var` 声明字段，字段可以有默认值。构造时只能按名字传参，有默认值的字段可以省略：

  ```gdscript
  struct Hit:
      var damage: int
      var point: Vector3
      var crit: bool = false

  var h = Hit(damage = 3, point = p)
  ```

- **enum**：GDScript 的花括号写法，分支可以带命名的载荷字段。无载荷的 enum 与 GDScript 相同。构造带载荷的分支时按名字传参：

  ```gdscript
  enum Command { IDLE, MOVE(target: Vector3), ATTACK(target: Node3D, power: int) }

  var c = Command.MOVE(target = Vector3.ZERO)
  ```

- **interface**：缩进块，可以声明 `var`、`signal` 和 `func`。没有函数体的方法不写冒号，与 GDScript 的 `@abstract func` 相同（`gdscript_parser.cpp:1790-1791`）；有函数体的是默认方法：

  ```gdscript
  interface Damageable:
      var hp: int
      signal died
      func take_damage(amount: int)
      func is_dead() -> bool:
          return hp <= 0
  ```

- **实现接口**：类头写 `implements A, B`（§2.1）。
- **pub**：关键字，写在注解之后、`static` 之前，例如 `@export pub var max_hp: int`、`pub static func make() -> Player`、`pub signal died`。
- **放置与命名**：struct、enum、interface 和内部类属于声明它们的文件的类。在本文件中直接写名字；在其他文件中写 `类名.名字`，例如 `Combat.Hit`、`Player.Command.MOVE(target = p)`。几个类共用的类型放进一个专门承载它们的类。【后续】别名或 `import`。
- **泛型**：类型参数写在名字之后的方括号中，与 `Array[int]` 一致，写法同 Python 3.12（PEP 695）；约束写作 `T: 接口`（§3.10）。文件的类在 `class_name` 上声明类型参数，例如 `class_name Cache[K, V] extends RefCounted`；内部类写作 `class Cache[K, V] extends RefCounted:`。

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

  调用处通常由实参推断，推断不出时显式写出，例如 `first[int]([])`、`Pair[int, String](first = 1, second = "a")`。`名字[...]` 与下标同形，由名字解析区分：名字是泛型函数或泛型类型时，方括号中是类型实参，否则是下标。GDScript 在这些位置遇到 `[` 都是语法错误（`gdscript_parser.cpp:964, 1005-1010, 1616, 1822`）。
- **函数类型**：沿用函数声明的形状，去掉函数名和参数名（§3.11）。GDScript 的类型只能以标识符或 `void` 开头（`gdscript_parser.cpp:3849-3861`），`func(...)` 作类型是语法错误。

  ```gdscript
  var on_hit: func(int, Node3D)                 # 没有返回值，也可以写 -> void
  var score: func(Enemy) -> float
  var load_level: async func(String) -> Level   # 调用得到 Task[Level]
  var filter: (func(Enemy) -> bool)?            # 可空须加括号，否则 ? 属于返回类型
  ```

### 2.5 语句

- `var`、`const`、`if`/`elif`/`else`、`while`、`for`、`match`、`break`、`continue`、`pass`、`return` 与 GDScript 相同，规则见 §5。
- `assert(条件, 消息)` 失败时是程序错误；release 构建中是否去掉【待定】。
- 没有以 if/else 块为值的写法；条件表达式只有 `a if c else b`（§5.5）。

## 3. 类型

### 3.1 一览

| 类别 | 类型 |
|---|---|
| 数值与布尔 | `int`（64 位）、`float`（64 位）、`bool` |
| 字符串 | `String`、`StringName`、`NodePath` |
| core 值类型 | `Vector2`、`Vector2i`、`Vector3`、`Vector3i`、`Vector4`、`Vector4i`、`Color`、`Rect2`、`Rect2i`、`Transform2D`、`Transform3D`、`Basis`、`Quaternion`、`Plane`、`AABB`、`Projection`、`RID`，以及 `Packed*Array` |
| 容器 | `Array[T]`、`Dictionary[K, V]`（§3.8） |
| 对象 | 引擎类、本语言的类、GDScript 和 C# 的全局类；非空 `T` 与可空 `T?`（§3.3） |
| 本语言的值类型 | struct（§3.6）、enum（§3.7） |
| 接口 | §3.9 |
| 标准类型 | `T?`（Option，§3.4）、`Result[T, E]`（§3.5）、`Task[T]` 与 `Settled[T]`（§9）、`weak[T]`（§3.12）、`ConversionError`（§5.3） |
| 函数 | 函数类型（§3.11）、`Callable`、`Signal` |
| 动态 | `Variant`（§3.13） |

### 3.2 数值

- 第一版只有 `int` 和 `float`，都是 64 位，与 Variant 一致（`core/variant/variant.h:257-258`）。没有供用户使用的窄类型；固定位宽的运算使用 `int` 上的回绕运算方法和位运算。
- 整数字面量的类型是 `int`，浮点字面量的类型是 `float`。
- `int` 在赋值、传参和返回时隐式转为 `float`。`float` 转为 `int` 必须写 `int(x)`，否则是编译错误；隐式截断会让 `hp = 2.5` 与 GDScript 悄悄不同。
- 运算的结果类型按 §5.1。
- 与引擎之间的数值边界按 §10.6。

### 3.3 对象与可空

- 对象类型 `T` 的值总是指向一个对象；`T?` 的值是一个对象或 None。
- 对象被释放后：
  - 读取 `T?` 类型的引用时，已释放的对象读作 None，所以 `!= None` 检查和由它带来的收窄（§5.4）同时就是存活检查；
  - `T` 类型的引用仍可以持有已释放的对象，读取、比较和传参不算访问，访问其成员是程序错误。debug 构建中，若对象是本语言的实例或由本语言代码释放，诊断写明它在哪里被释放；
  - `queue_free()` 之后、帧末真正删除之前，对象仍算存活。`is_instance_valid` 的行为与 GDScript 相同。
- 每次读取 `T?` 类型的对象引用都要做一次存活查询。

### 3.4 Option

- `T?` 就是 Option，值为 T 的值或 None。T 的值隐式包装为 `T?`。
- Option 是值，对它的操作都是方法，名字与 Rust 相同：`unwrap`、`expect(消息)`（失败时都是程序错误）、`unwrap_or`、`unwrap_or_else`、`map`、`and_then`、`or`、`or_else`、`filter`、`ok_or`、`ok_or_else`。没有 `!`、`??`、`?.` 运算符。
- 唯一的特殊语法是后缀 `?`：遇到 None 时当前函数返回 None（§8.2）。
- 判空用 `!= None` 和收窄（§5.4）；没有真值判断。
- 嵌套的 Option 保留两层，与 Rust 相同。`T??` 是合法的类型，多数由泛型和容器产生（§3.8）：
  - 隐式包装按目标类型补层：`T?` 的值赋给 `T??` 得到包含该值的 Some；字面量 None 总是最外层的 None；
  - `?`、`unwrap`、match 和收窄每次剥去一层；`.flatten()` 把 `T??` 合为 `T?`；
  - `Variant?` 同理：None 表示没有值，NIL 是一个值（§3.13）；
  - 交给引擎的规则见 §10.1。

  ```gdscript
  var r = enemies.pop_back()   # enemies: Array[Enemy?]，r: Enemy??
  match r:
      None: print("空数组")
      var e: print(e)          # e: Enemy?，可能是 None
  ```

  理由：合并为一层时，泛型代码的行为随 T 改变，例如 `first[T]` 在 T 为 `Enemy?` 时分不清空数组和首个元素为空，通用缓存会把存入的 None 当作没有缓存；JSON 中缺少的键与 null 也无法区分。这里没有 `?.`，嵌套基本只出现在泛型和容器取值上。

### 3.5 Result

- `Result[T, E]` 表示预期之内可能失败的操作。用 `Ok(v)` 和 `Err(e)` 构造；成功值不隐式包装。
- 操作是方法，名字与 Rust 相同：`unwrap`、`expect`、`unwrap_or`、`unwrap_or_else`、`map`、`map_err`、`and_then`、`or_else`、`ok`、`err`。
- 后缀 `?` 遇到 `Err(e)` 时当前函数返回 `Err(e)`（§8.2）。
- 丢弃 Result 是编译错误；确实要忽略时写 `.ok()`。

### 3.6 struct

- struct 具有值语义：赋值、传参和返回都复制。只存在于本语言内部。
- 构造时只能按名字传参，有默认值的字段可以省略（§2.4）。【后续】位置参数。
- 跨越引擎边界（信号参数、Variant、跨语言消息）时，自动编码为按字段名的 Dictionary；解码时检查每个字段的存在和类型，并深入容器和嵌套的 struct，例如 `Dictionary[String, ConsumableState]`。字段的解码规则与 `T.try_from` 相同（§5.3）。
- 【已知限制】第一版 struct 不能 `@export`，也不能存入 `.tres`；需要在编辑器中编辑的数据使用 Resource。
- struct 能否有方法、`s.x = 1` 作用于哪一份、`==` 以及作为 Dictionary 键时的比较和哈希【待定】。

### 3.7 enum

- 无载荷的 enum 与 GDScript 相同。
- 带载荷的分支，载荷字段必须命名，例如 `MOVE(target: Vector3)`。命名使编码不随字段顺序改变。
- 对 enum 的 match 必须穷尽（§5.6）。新增分支时，编译器列出所有没有处理它的位置。
- 引擎看到的形式见 §10.1：无载荷的 enum 为 int，导出时显示为下拉框；带载荷的 enum 编码为按字段名的 Dictionary，用一个保留键记录分支名。
- 带载荷的 enum 能否 `@export`，以及 `==` 和哈希【待定】，与 struct 的同一项一起决定。

### 3.8 容器

- 对外只有 `Array[T]` 和 `Dictionary[K, V]`，API 相同。没有类型参数的 `Array`、`Dictionary` 的元素类型为 Variant，与 GDScript 相同。
- 元素可以由引擎表示时，容器就是引擎的带类型容器，与引擎共享。元素是本语言独有的类型（struct、带载荷的 enum 等）时，使用本语言自己的表示；交给引擎须显式转换，转换生成副本。理由：隐式复制会造出“修改的其实是副本”一类的坑。
- 取值：
  - `pop_front()`、`pop_back()` 返回 `T?`：引擎在空数组上不报错而返回 null（`core/variant/array.cpp:798-817`）；
  - `front()`、`back()` 返回 T，空数组时是程序错误，引擎在这里同样报错（`array.cpp:364-372`）；
  - 越界的下标是程序错误；
  - T 本身可空时，`pop_front()`、`pop_back()` 返回 `T??`，由绑定层先检查数组是否为空，因为引擎在两种情况下都返回 null；
  - `Dictionary[K, V].get(k)` 返回 `V?`，键不存在时为 None，对应引擎在缺键时返回默认值（`core/variant/dictionary.cpp:177-188`）；`get(k, default)` 返回 V。
- 标准库在原生容器上提供接受带类型 lambda 的 `map`、`filter`、`sort` 等（§3.11）。

### 3.9 接口

- 名义接口：类必须用 `implements` 显式声明实现了哪些接口。
- 接口可以声明方法、属性和信号，可以有默认方法；默认方法通过接口属性读写状态。
- 属性要求可以由继承来的引擎属性满足，例如 `var global_position: Vector2` 由 Node2D 的同名属性满足；类型必须相同，读写能力相符。
- 实现接口的成员必须是 `pub`（§4.2）。
- 只有本语言的类能实现接口；外部对象须显式包装。
- 【已知限制】第一版没有带状态的 mixin。
- 接口能否继承接口、能否是泛型【待定】。

### 3.10 泛型

- 函数、struct、enum，以及只由本语言代码创建的 RefCounted 类可以是泛型。Node、Resource 脚本和 autoload 不能，因为引擎实例化它们时不提供类型参数。
- 约束用接口表达，第一版每个类型参数至多一个约束。类型参数在运行时保留。
- 写法见 §2.4。
- 泛型方法不经反射暴露（§10.5），因为引擎和 GDScript 按名字调用时给不出类型实参。作为 Callable 使用时须确定类型实参，例如 `sig.connect(handle[int])`。
- 【后续】多个约束，以及以类作约束；写法不变。

### 3.11 函数类型与 Callable

- 函数类型记录参数类型、返回类型以及是否为 async，写法见 §2.4。没有返回类型的函数类型返回 void。
- 调用照 GDScript 写 `f.call(x)`，按函数类型静态检查。直接写 `f(x)` 报 GDScript 现有的错误并给出改法（`modules/gdscript/gdscript_analyzer.cpp:3785-3789`）。
- 函数值能否用在某个位置只看类型，lambda 与方法值相同：
  - 参数逆变、返回协变，例如 `func(Node)` 可以用在期望 `func(Node3D)` 的位置；
  - async 与普通的函数类型之间没有隐式转换；
  - 由引擎调用的位置接受 async 函数：代码中和场景中的信号连接，以及引擎接口的 Callable 参数（如 `tween_callback`）。任务作为根任务运行，与 async 的引擎虚函数相同（§9.1）。依据：[语料计数](surveys/corpus-counts.zh.md)中作为值使用的协程方法有 91 处，其中 60 处是 `connect(名字)`。
- 【后续】函数类型中的参数名；直接调用 `f(x)`；把普通函数传给期望 async 的位置。
- 方法值（例如 `change_weapon`、`_on_hit`）和 lambda 具有函数类型，交给引擎时成为直接指向该方法的 Callable，不按名字查找（§10.5）。
- `f.bind(a, b)` 的类型是在 f 的函数类型中去掉最后两个参数，因为引擎把绑定的实参接在调用实参之后（`core/variant/callable_bind.cpp:141-150`）。
- 签名未知的 `Callable` 须显式转换为函数类型，每次调用时检查参数和返回值。
- lambda 按值捕获，与 GDScript 相同。在 RefCounted 类中捕获 self 是强引用（`modules/gdscript/gdscript_lambda_callable.cpp:285-291`），不悄悄改为弱引用；需要弱捕获时用 `weak`（§3.12）。
- lambda 的类型按局部规则推断（§4.6）。

### 3.12 弱引用

- `weak[T]` 是带类型的弱引用：`weak(x)` 创建，`.get()` 返回 `T?`。
- 需要在闭包中弱捕获 self 时，在闭包外写 `var me = weak(self)`，在闭包内用 `me.get()`。

### 3.13 Variant

- 动态值只有三个来源：以 Variant 声明的引擎接口；GDScript 插件中没有标注类型的参数、返回值和成员（§11.1）；跨语言传来的消息。
- 动态值进入静态代码时必须显式转换（§5.3），失败时报错；不做隐式检查。
- 能定型的从源头定型：引擎实际有约束但没有导出的，在本语言的绑定元数据中补上（§10.3）。
- 静态值进入 Variant 是隐式的：赋值、传参和返回都可以。struct 和带载荷的 enum 按 §10.1 编码。
- Variant 进入静态代码只有显式的途径：
  - `v as T`，只用于对象，得到 `T?`（§5.3）；
  - `T(v)`，GDScript 的构造转换；
  - `T.try_from(v)`，严格转换，返回 Result；
  - `if v is T:`，分支中的局部变量和参数收窄为 T（§5.4）。
- `==` 和 `!=` 按引擎的 Variant 比较（§5.2）。
- 其余操作都是编译错误，快速修复插入转换或 `if v is T:`：运算符、成员访问 `v.x`、方法调用 `v.f()`、下标 `v[i]`、`for x in v`。GDScript 在这些位置动态执行。确实要按名字动态访问时，先转为 Object，再用引擎的 `get`、`set`、`call`（§10.4）。
- Variant 包含 NIL：`v == None` 判断 NIL，`v is int` 对 NIL 为假，`v as Node` 对 NIL 得到 None。`Variant?` 与 Variant 不同（§3.4），例如 `Dictionary[String, Variant]` 的 `get("hp")` 在缺键时为 None，在 `{"hp": null}` 中得到包含 NIL 的 Some。

  ```gdscript
  var data = JSON.parse_string(text)   # Variant
  data.hp                              # 编译错误：先转换
  if data is Dictionary:               # data 收窄为 Dictionary
      var hp = int.try_from(data.get("hp")).unwrap_or(100)
  var n = data as Node                 # Node?
  ```

## 4. 声明

### 4.1 类与继承

- 类名使用引擎的扁平全局类表。编译器检查重名，包括与 GDScript 插件和 C# 的类名重复。【已知限制】第一版没有命名空间。
- 一个类可以继承引擎类或本语言的类。
- 【已知限制】不能继承 GDScript 类。插件要求继承它的 GDScript 类时（例如 Beehave 的 ActionLeaf），由用户手写一层薄的 GDScript 子类，在其中调用本语言的代码（§11.2）。
- GDScript 脚本不能继承本语言的类：GDScript 解析 `extends 全局类名` 时，把基类文件当作 GDScript 解析（`modules/gdscript/gdscript_analyzer.cpp:471-487`）。分析服务在这样的 GDScript 文件上报错并说明原因（§11.2）。
- 覆盖方法时签名的规则见 §4.6，可见性的规则见 §4.2。
- `@abstract` 与 GDScript 相同（`gdscript_analyzer.cpp:1543-1562, 3674-3687`，`modules/gdscript/gdscript_parser.cpp:4492-4516`）：
  - 抽象方法不写冒号、没有函数体；
  - 含抽象方法的类必须标 `@abstract`；
  - 具体子类必须实现全部抽象方法；
  - 抽象类不能构造；
  - `super` 调用抽象方法，以及给 static 方法标 `@abstract`，都是编译错误。
- 场景中的节点使用抽象类是编译错误，标在该场景上。引擎本身也不实例化抽象脚本（`modules/gdscript/gdscript.cpp:265-270`）；本语言经 `_is_abstract` 和全局类表把抽象性报告给引擎（`core/object/script_language_extension.h:143-147, 756-769`）。
- 带状态的共同基类用 `@abstract`；不同引擎基类的类共有的能力用接口（§3.9）。例如 BattlerAction 继承 Resource，带导出字段：

  ```gdscript
  @abstract
  class_name BattlerAction extends Resource

  @export var label: String

  @abstract pub async func execute()
  ```

  依据：[语料计数](surveys/corpus-counts.zh.md)中 28 个抽象类有 25 个在 gdUnit4，以文件为单位的抽象类中 17 个的方法全部是抽象的，可以改写为接口。

### 4.2 可见性

- 不带 `pub` 的成员在声明它的文件内可见，包括文件中的内部类；子类也可以读写、调用和覆盖它们；其他类看不到。
- 标为 `pub` 的成员对所有代码可见。
- 覆盖时可见性不变：覆盖 `pub` 成员须写 `pub`，覆盖不带 `pub` 的成员不能加 `pub`。
- 实现接口的成员必须是 `pub`。
- `pub` 成员的签名中出现非 `pub` 的类型是编译错误。
- 必须赋值的成员（§6.2）若不带 `pub`、又没有场景或祖先为它赋值，在类的定义处报错。
- 引擎、GDScript 和 C# 按名字能访问哪些成员，见 §10.5。
- 【后续】更严的 `private`。

### 4.3 成员变量

- `var` 声明成员。有初始化式时，类型可以由它推断，规则与局部变量相同：`var speed = 5.0` 为 float，`@onready var label = $Label` 按场景推断为 Label（§7.1）。推断出的类型出现在悬停、补全和引擎看到的属性信息中。
- 以下成员必须标注类型：没有初始化式的；初始化式推断不出类型的，例如空容器和 None。
- GDScript 中 `var hp = 10` 是无类型成员；在本语言中推断为 int，此后赋入其他类型是编译错误，符合语法硬规则。
- 没有初始化式的成员：
  - 数值、bool、String、StringName、NodePath、core 值类型、容器和无载荷的 enum 取默认值，与 GDScript 相同；
  - struct 在所有字段都有默认值时取默认值，否则与对象引用一样必须赋值；
  - 带载荷的 enum 必须赋值；
  - 必须赋值的成员的规则见 §6.2。
- `@export` 的规则与 GDScript 相同，另有：struct 不能导出（§3.6）；导出的场景写作 `PackedScene[T]`（§7.4）；非空的对象类型的导出必须赋值（§7.5）。
- `@onready` 见 §6.3。

### 4.4 属性访问器

- setter 和 getter 的写法与 GDScript 相同：内联的 `set(value):`、`get:`，或具名的 `set = f`、`get = g`。
- 在属性自己的访问器内，属性名指向它的存储字段；在类的其他地方，读写都经过访问器，与 GDScript 相同。
- 初始化式直接写字段，不经过 setter（§6.1）。
- setter 和 getter 不能是协程。
- 就绪前执行的 setter 的规则见 §6.5。

### 4.5 常量

- `const` 与 GDScript 相同。

### 4.6 函数

- 函数签名的类型手写，不推断。参数必须标注类型。
- 覆盖引擎虚函数或基类方法时，可以省略参数类型和返回类型；省略的类型取自被覆盖的声明，悬停可见。写出的类型必须与之一致，参数名可以不同。例如 `func _process(delta):` 中 delta 为 float（`scene/main/node.h:433-434`）。这不是推断：类型由被覆盖的声明规定，与函数体无关。
- 不是覆盖的函数，省略返回类型表示 void；在其中返回值是编译错误，快速修复补上返回类型。
- 参数可以有默认值，与 GDScript 相同。
- 剩余参数：
  - `...args: Array` 与 GDScript 相同，只能是最后一个参数，不能有默认值（`gdscript_parser.cpp:1723-1730`）；
  - 另外允许 `...args: Array[T]`，调用处逐个检查实参；GDScript 对带类型的剩余参数报编译错误（`gdscript_analyzer.cpp:1802-1809`）；
  - 只写 `...args` 是编译错误，快速修复补上 `: Array`；
  - 引擎看到的方法信息带 `METHOD_FLAG_VARARG`，GDScript 据此接受任意个额外的实参（`gdscript_analyzer.cpp:6101-6104`），元素类型由入口检查（§10.1）；
  - 调用处没有展开写法，GDScript 也没有（`gdscript_parser.cpp:4336`）；已有的数组用 `callv` 传入。【后续】展开写法。
- 调用按位置传参，与 GDScript 相同。按名字传参只用于 struct 和带载荷分支的构造（§2.4），以及 `instantiate(...)`、`new(...)` 传入必须赋值的成员（§6.2）。
- 函数内部的局部变量可以由初始化式推断类型。lambda 是函数内部的代码，按局部规则推断：有期望的函数类型时，参数类型取自它，没有时须写出；返回类型由函数体推断，例如 `weapon.map(func(w): return w.name)`。
- 协程在签名上声明为 `async func`，lambda 也是如此（§9.1）。

### 4.7 信号

- 信号的参数必须标注类型，例如 `pub signal died(cause: String)`。
- `emit` 和 `connect` 都做静态检查：发射时的实参、连接的函数的参数个数和类型。
- 场景中的信号连接由场景模型检查（§7.7）。
- 引擎类的信号按 ClassDB 的信息定型；未审核的对象参数可空（§10.2）。
- await 信号的结果见 §9.6。

### 4.8 静态成员

- `static var`、`static func`、`_static_init()` 的写法和时机与 GDScript 相同：类加载时，先基类后派生类，按声明顺序初始化静态成员，最后调用 `_static_init`（`modules/gdscript/gdscript_compiler.cpp:2646-2649`）。静态成员的初始化式同样不经过 setter（`gdscript_compiler.cpp:2620-2643`）。
- 一个类在静态初始化中读取另一个类的静态成员时，先完成那个类的初始化。
- 静态初始化之间形成环是程序错误，诊断列出依赖链；能静态判断的在编译期报错。不读取默认值或未完成的值。GDScript 的相关问题见 #104161、#82141、#104588。
- 静态初始化不能 await。
- 修改静态成员属于结构变化，热重载时提示重新运行（§12.4）。

## 5. 表达式与语句

### 5.1 运算

- 运算的结果类型按引擎的运算符表确定，与 GDScript 的静态检查相同（`core/variant/variant_op.cpp:256-368`）。例如 `Vector2 * float` 得到 Vector2，`Vector2i * float` 得到 Vector2，`int / int` 是整除。表中没有的组合，例如 `Vector2 + Vector2i`，是编译错误。
- `int` 与 `float` 混合运算时提升为 `float`。
- 整数除以 0 是程序错误。
- 以 Variant 绑定的工具函数（lerp、clamp、min、max、abs、wrap 等）的定型见 §10.3。

### 5.2 比较

- `==` 按引擎的运算表定型，与其他运算符相同（§5.1）；表中没有的组合是编译错误，例如 `hp == "3"`、`Vector2() == Vector2i()`。
- int 与 float 按数值比较，`1 == 1.0` 为真（`core/variant/variant_op.cpp:490-491`）。String 与 StringName 按内容比较（§10.7）。
- 对象按同一性比较（`variant_op.cpp:514`）。两个类型都是类、且互不为子类时是编译错误，因为结果总为假；涉及接口时可以比较。
- Option：None 等于 None；两个 Some 在内部的值相等时相等；非 Option 的一侧先包装再比较，例如 hp 为 `int?` 时的 `hp == 3`。既不是 Option 也不是 Variant 的值与 None 比较是编译错误，因为结果总为假；快速修复把类型改为 `T?`，或在检查存活时改用 `is_instance_valid(x)`。
- 容器照引擎逐元素比较。引擎比较元素时要求类型相同（`core/variant/array.cpp:153-156`，`core/variant/variant.cpp:3213-3216`），所以 `[1] == [1.0]` 为假，而 `1 == 1.0` 为真。元素类型不同的容器比较是编译错误，例如 `Array[int] == Array[float]`，因为只有两边都为空时才相等。
- 一侧是 Variant 时，运行时按引擎的运算表求值；组合不在表中是程序错误，与 GDScript 相同（`variant_op.cpp:1051-1056`，`modules/gdscript/gdscript_vm.cpp:756-761, 794-802`）。`v == None` 判断 NIL（§3.13）。
- `!=` 在所有情况下都是 `==` 的否定。
- struct 和带载荷 enum 的 `==`【待定】（§3.6）。

### 5.3 转换

- `x as T` 只用于对象类型，结果为 `T?`：类型不符时为 None，与 GDScript 相同（`modules/gdscript/gdscript_vm.cpp:1665-1666`）。x 的静态类型已经是 T 时，结果为 T，并提示这个 `as` 多余。
- `as` 转为内置类型是编译错误。GDScript 在这里按构造函数转换（`gdscript_vm.cpp:1630-1637`），快速修复给出两种改法：
  - `T(x)`：保留这种转换，例如 `int("5")`、`int(2.7)`；
  - `T.try_from(x)`：严格的转换。
- `T.try_from(v)` 返回 `Result[T, ConversionError]`。规则与 struct 字段的解码相同：值的类型必须与 T 相符；转为 `int` 时，接受恰为整数且在 int64 范围内的浮点数（§10.8）。用 `?` 传播失败，或用 `.unwrap()`、`.expect(…)` 把失败变为程序错误。
- 必须成功的向下转型写作 `(x as T).unwrap()`。
- 先判断分组、再向下转型的写法，快速修复建议改用 `is` 收窄。以字面量类名过滤的接口按该类定型（§10.3）。

### 5.4 收窄

- 采用 Kotlin 的规则：局部变量和参数在 `!= None`、`is` 和提前返回之后收窄；条件表达式 `a if c else b` 的两个分支，以及 `and`、`or` 的右侧，同样按条件收窄。
- 成员不收窄，须先读入局部变量。依据：[第二次纸面迁移](surveys/paper-migration-2.zh.md)中，因此新增的局部变量每千行 4.4 个。【后续】条件绑定（例如 `if var t = _tween:`），或在检查与使用之间没有调用、await 和赋值时让没有 getter 的字段收窄。
- 没有真值判断：条件必须是 bool。GDScript 的 `if x:` 是编译错误，快速修复按 x 的类型改为 `x != None`、`x != 0`、`x != ""` 等。
- 非空不代表对象仍然存活；访问时照常检查（§3.3）。

### 5.5 条件表达式

- 只有 GDScript 的 `a if c else b`，c 必须是 bool。

### 5.6 match

- 保留 GDScript 的全部模式，含义不变（`modules/gdscript/gdscript_parser.cpp:2499-2723`）：字面量和常量表达式按相等比较；`var x` 绑定；`_` 通配；数组和字典模式，以及其中的 `..`；逗号分隔的多个模式，其中不能绑定；`when` 守卫。
- 新增的模式：
  - enum 分支：`类型.分支(字段 = 模式, ...)`。`var 字段` 是 `字段 = var 字段` 的简写。未列出的字段须由 `..` 覆盖，与字典模式相同。模式可以嵌套。
  - struct：同样的写法，例如 `Hit(damage = var d, ..)`。
  - Result：`Ok(var v)`、`Err(var e)`，按位置书写，与构造一致。
  - Option：`None` 和 `var x`；后者绑定的值已收窄为 T。

  ```gdscript
  match cmd:
      Command.IDLE:
          stop()
      Command.MOVE(var target):
          walk_to(target)
      Command.ATTACK(target = var enemy, power = 0):
          taunt(enemy)
      Command.ATTACK(var target, ..) when target.is_boss:
          flee()
      Command.ATTACK(..):
          hit()
  ```

- 对 enum（无论有无载荷）、Result 和 Option 的 match 必须穷尽或带 `_`；带 `when` 的分支不计入覆盖。对其他类型不要求穷尽，与 GDScript 相同。
- 【已知限制】第一版没有类型模式（用 `if x is T`）、没有 match 表达式，也不能在逗号分隔的多个模式中绑定。

### 5.7 循环

- `for` 与 GDScript 相同：`for i in n` 遍历 0 到 n-1，i 为 int；可以遍历 `range(...)`、容器、String 和 Dictionary 的键。
- 循环变量可以标注类型，如 `for i: int in ...`；标注的类型须是元素类型或其父类型，不做隐式的向下转换。
- 遍历 float 和向量是编译错误，快速修复改用 `range(...)`。GDScript 允许这样写，并把向量当作范围（`gdscript_vm.cpp:3093-3230`）；在静态类型下这几乎总是误用。
- `while`、`break`、`continue` 与 GDScript 相同。

## 6. 初始化与生命周期

### 6.1 成员的初始化

- 成员的初始化式直接写字段，不经过 setter，实例成员和静态成员都是如此，与 GDScript 相同（`modules/gdscript/gdscript_compiler.cpp:2435-2451, 2620-2643`）。初始化式给出第一个值，setter 处理值的变化；初始化时没有观察者，后声明的成员也尚未初始化。
- 派生状态在 `_ready` 或 `_static_init` 中计算。成员同时有初始化式和 setter 时，编译器提示初始化不经过 setter。
- 没有初始化式的成员取默认值或必须赋值（§4.3）。

### 6.2 必须赋值的成员

- Node 和 Resource 脚本的 `_init` 不能有参数，因为引擎总以零个参数构造它们（[暗坑清单](surveys/pitfalls.zh.md) G-02）。构造参数表达为必须赋值的成员：没有默认值的非空成员。
- 必须赋值的成员要在所在场景就绪完毕之前赋值，来源可以是：
  - 场景：导出的成员由场景提供（§7.5）；
  - 创建者：`instantiate(...)` 和 `new(...)` 接受按名字传入的成员值，在返回前完成赋值，例如 `BulletScene.instantiate(speed = 300.0, target = enemy)`。对 `PackedScene[T]`，编译器在调用处检查场景没有提供的必须赋值成员都已传入，漏写是编译错误；
  - 祖先：同一场景中的祖先节点在自己的 `_ready` 中赋值，只要场景模型看得见这次赋值，即写成 `$Child.target = v`，或经 `@onready` 引用赋值。
- 检查时机：
  - 由场景或创建者提供的，在节点就绪时检查，早于 `@onready` 的初始化；
  - 由祖先赋值的，在整个场景就绪之后检查。就绪在节点入树时同步传播完毕（`scene/main/node.cpp:3425-3429`），每帧回调由主循环发起，所以正常逻辑读不到空值；
  - 子节点在自己的 `_ready` 中读取由祖先赋值的成员，是编译错误；
  - 就绪前读取尚未赋值的成员是程序错误，诊断写明成员名；
  - 场景模型看不见的配置方式（例如遍历 `get_children()` 赋值），成员须写作 `T?`。
- 运行时才确定路径的场景，只有就绪时的检查。
- Resource 没有就绪阶段：`.tres` 中的赋值静态检查；代码中 `new()` 出来的，在读取时检查。
- 只由本语言代码创建的 RefCounted 类，照常使用带参数的 `_init`。
- autoload 比场景活得久，引用场景节点的成员只能写作 `T?`。需要非空时，把管理器作为场景中的节点，用必须赋值的导出成员引用其他节点。【已知限制】第一版没有“就绪后才赋值的非空成员”（如 Kotlin 的 `lateinit`）。依据：20 个仓库中，在 `_ready` 里把节点登记到 autoload 成员上的写法只有 6 处，其中 5 处在 Ring（[第二次纸面迁移](surveys/paper-migration-2.zh.md)）。

### 6.3 `@onready`

- 沿用 GDScript 的时机：`@onready` 的初始化式在 `_ready` 之前执行，先基类后派生类，各自按声明顺序（`modules/gdscript/gdscript.cpp:1956-1972`）。
- 以下情况是编译错误：`@onready` 与 `@export` 同时使用；初始化式中使用 await；在 `_init` 或 `_enter_tree` 中直接读取 `@onready` 成员。
- 经调用间接读到尚未初始化的 `@onready` 成员是程序错误，诊断写明成员名。依据：[暗坑清单](surveys/pitfalls.zh.md) G-04。

### 6.4 就绪顺序

- 子节点先于父节点就绪，沿用引擎的顺序，不新增生命周期钩子。
- 子节点在 `_ready` 中读到父节点尚未初始化的成员时，报程序错误并说明原因。快速修复给出两种改法：由父节点在自己的 `_ready` 中配置子节点（§6.2）；或把子节点的 `_ready` 声明为 async，并 `await get_parent().ready`。
- 子节点在运行时加到已就绪的父节点下时，这样的读取是合法的，因此不在编译期一概报错。
- 节点重新入树时不再执行 `_ready`，与引擎一致。依据：[暗坑清单](surveys/pitfalls.zh.md) G-03、G-05。

### 6.5 就绪前的 setter

- 场景实例化时，引擎先设置节点保存在场景中的属性，再把节点加到父节点下（`scene/resources/packed_scene.cpp:567, 616`），这时节点的子节点和 `@onready` 成员都还不可用。
- setter 照常立即执行。执行到第一处需要节点就绪的语句时，若节点尚未就绪，就在此挂起，就绪后从这里继续：
  - 需要就绪的语句由静态分析确定：访问 `@onready` 成员、`$X` 或子节点，或者调用会访问它们的本类方法，包括子类的覆盖；
  - 写在 `if is_node_ready():` 分支里的访问不算；
  - 挂起点的位置在编辑器中悬停可见。
- 就绪前多次写入时，只有最后一次挂起的调用继续执行，之前的全部取消。挂起的调用在 `@onready` 初始化之后、`_ready` 之前，按写入顺序继续。节点在就绪前被释放时，挂起的调用随之丢弃。就绪以后 setter 不再挂起。
- 变换和存储在挂起点之前完成，所以就绪前读到的是变换后的值。
- 这就是 GDScript 中手写 `if not is_node_ready(): await ready` 的写法，由编译器自动完成。依据：[属性访问器调查](surveys/property-accessors.zh.md)中，6 处 `await ready` 的 setter 恢复后都读取字段的当前值。
- 值只存在节点中的属性（没有字段、getter 读取子节点的），在就绪前读取是程序错误，诊断写明读取了哪个尚未就绪的节点。需要就绪前读取时，用字段保存。
- 从未被写入的属性，就绪时不执行 setter，与 GDScript 一致。导出属性的 setter 会同步节点、而默认值从不经过 setter 时，给出提示。

### 6.6 析构

- 不新增语法：继续用 `_notification(what)` 处理 `NOTIFICATION_PREDELETE`，与场景树相关的清理放在 `_exit_tree`，写法和时机与 GDScript 相同；析构顺序沿用引擎。
- 运行时保证 RefCounted 在 PREDELETE 期间可以读写自己的字段、调用自己的方法，不重新给 self 加引用。
- 处理 PREDELETE 的分支中，把 self 传出、存入成员或容器，以及使用 await，都是编译错误。这期间指向 self 的 `weak` 返回 None。依据：[暗坑清单](surveys/pitfalls.zh.md) G-09。

### 6.7 引用环与泄漏

- 不做追踪式 GC。RefCounted 使用引擎的引用计数，环不会被释放。
- 编译器对明显的环给出警告和改写为 `weak` 的快速修复：
  - 捕获 self 的闭包存入 self 的成员、self 自身的信号连接或 self 持有的容器；
  - 两个 RefCounted 类以强引用类型的字段互相指向。
- Node 不做引用计数。以下情况给出警告：
  - `new()` 或 `instantiate()` 得到的节点，在某条路径上既没有加入场景树，也没有被返回、保存或释放；
  - `remove_child` 之后既没有保存也没有释放。
- debug 构建退出时，列出泄漏的节点和对象，以及它们的创建位置。
- 经引擎容器或其他对象间接形成的环，静态分析发现不了。依据：[暗坑清单](surveys/pitfalls.zh.md) G-10。

## 7. 场景模型

### 7.1 节点定型

- 编译器收集脚本能看到的全部场景结构，包括继承场景和实例上的修改，为 `$X`、`%X` 和字面量路径的 `get_node` 定类型。
- 不同的场景结构给出不同类型时报错，要求标注。依据：[场景结构调查](surveys/scene-structures.zh.md)的游戏仓库中，2,023 处字面量路径没有一处在不同结构中解析为不同类型。
- 找不到的路径是编译错误，标在缺少该节点的场景上；§7.2 的三种情况除外。

### 7.2 由脚本声明的结构

- 以下三种情况由脚本声明，写法【待定】：
  - 基类要求、由继承场景提供的子节点；
  - 运行时添加的子节点；
  - 可选的子节点，类型为 `T?`。

### 7.3 向上访问

- `/root/X` 形式的 autoload 访问改用有类型的 autoload 全局名。
- 父节点和 owner 的要求写作类级注解 `@parent(T)`、`@owner(T)`，与 `@tool` 放在同一位置。`get_parent()`、`..` 和 `owner` 因此具有类型 T；未声明时为 Node，转为具体类型须显式转换（§5.3）。

  ```gdscript
  extends Node
  class_name HealthComponent
  @owner(Combat.Combatant)

  func _ready():
      current_health = owner.init_health
  ```

- 静态检查在场景中放置该脚本的位置进行。owner 是包含该节点的场景的根节点。挂在场景根上的脚本没有 owner，声明 `@owner` 是编译错误。
- 运行时：
  - 父节点要求在节点挂到父节点时检查（`NOTIFICATION_PARENTED`，`scene/main/node.h:466`）；
  - `owner` 在每次读取时检查不为 null 且符合要求，失败是程序错误，诊断写明要求和实际的 owner。不在移动节点时检查 owner：节点被移出 owner 的子树时，引擎把 owner 置为 null（`scene/main/node.cpp:391-396, 1799-1801`），这是合法的操作；owner 被释放时同样置为 null（`node.cpp:299-302`）。
- 需要判断有没有 owner 的代码使用 `get_owner()`，其类型为 `Node?`。

### 7.4 场景资源

- `preload` 和字面量路径的 `load` 按资源的实际内容定型；场景得到 `PackedScene[T]`，T 为场景根节点的类型，根节点有脚本时为脚本的类。
- 导出的场景写作 `PackedScene[T]`，由编辑器检查所填的场景。`instantiate()` 的结果为 T。
- 运行时才确定路径的 `load` 按引擎的签名得到 Resource，可空性按 §10.2 的审核；这样加载的场景，`instantiate()` 得到 Node。转为具体类型须显式转换（§5.3）。

### 7.5 必须赋值的导出

- 非空的对象类型的导出必须赋值。确实可能缺值的写作 `T?`。
- 按场景的使用方式检查：
  - 场景被其他场景实例化或继承时，沿实例和继承链在任何一层赋值都算数；每个实例分别检查，缺值标在该实例所在的场景上。例如 player.tscn 的 `crosshair` 指向只存在于 main.tscn 的节点，由 main.tscn 在实例上赋值，player.tscn 本身不报错；
  - 场景作为根使用时，即主场景、以字面量路径 `load` 或 `preload` 的场景、作为 `PackedScene[T]` 导出的场景，检查场景本身；
  - 代码中 `instantiate(...)` 的场景，按调用处传入的值检查（§6.2）；
  - 没有被任何地方引用的场景只给警告，因为运行时才确定路径的 `load` 看不到。
- 检查分三个时机：
  - 编辑时允许清空。Inspector 中的字段和场景树中的节点立即标出错误，但不阻止任何操作，重新赋值后标记消失；
  - 运行时允许运行，缺值的节点就绪时报程序错误，检查早于 `@onready` 的初始化，诊断写明成员名，例如在编辑器中单独运行 player.tscn 时；
  - 导出发布版本时，确定会缺值的不允许导出，只有警告的不阻止。
- 远程检查器向非空成员写入 null 时，入口检查拒绝写入，原值不变。
- 理由：编辑过程中清空再替换是正常操作，非空约束的是编辑完成的场景。替换也不需要先清空，因为节点引用在 `.tscn` 中以路径保存，实例化时才解析（`scene/resources/packed_scene.cpp:449-468`）。

### 7.6 资源共享

- 资源共享的语义不变，由场景模型发现问题。编译器知道哪些资源是共享的：`load`、`preload` 和导出属性得到的资源来自缓存；场景中没有设为“局部于场景”的内嵌子资源，由该场景的所有实例共用。
- 对已知共享的资源写属性，或调用会修改它的方法时，给出警告，列出共享它的位置，并提供两个快速修复：在场景中把它设为局部于场景，或在修改前 `duplicate()`。有意修改共享资源时用 `@warning_ignore` 关闭。运行时才替换的资源追踪不到，不警告。
- `duplicate()` 返回接收者的静态类型；复制深度用 `duplicate_deep(模式)` 写明。
- `preload`、导出的 PackedScene 和资源之间的引用环在运行前报告，并建议把其中一条边改为运行时 `load`。依据：[暗坑清单](surveys/pitfalls.zh.md) G-19、G-20、G-34。

### 7.7 场景中的连接和路径

- 场景中保存的信号连接，由场景模型检查方法是否存在、参数是否与信号相符。
- 动画轨道中的属性路径、方法轨道中的方法名，以及 `"position:x"` 一类属性路径，按静态引用解析（§10.4）。
- 这些引用参与重命名和查找引用（§13.3）。

## 8. 错误

### 8.1 程序错误

- 程序错误中止本语言的整条调用链，退回到引擎调用进来的入口。退出时释放持有的引用，记录错误和调用栈；在编辑器中运行时进入调试器。游戏继续运行。
- debug 和 release 构建的行为一致。
- 若引擎调用的入口需要返回值，引擎得到该类型的默认值。
- 本文中称为“程序错误”的情况都按这一条处理，包括：访问已释放的对象（§3.3）、`unwrap` 和 `expect` 失败、越界下标、整数除以 0、就绪前读取尚未赋值的成员、跨引擎边界的入口检查失败（§10.1）、等待被取消的任务（§9.2）。
- 理由：GDScript 出错后按返回类型构造默认值返回，调用方继续执行（`modules/gdscript/gdscript_vm.cpp:3975-3979`）；在静态类型下，这会让代码拿到假值。

### 8.2 预期之内的失败

- 预期之内的失败用 Option（§3.4）或 Result（§3.5）表达。采用 Rust 的模型：Option 是值，Result 用于控制流。
- 唯一的特殊语法是后缀 `?`，表示短路返回：
  - 作用于 `Result[T, E]`：遇到 `Err(e)` 时当前函数返回 `Err(e)`，否则得到 T。当前函数须返回 `Result[_, E]`，E 相同；不同时先用 `map_err` 转换。
  - 作用于 `T?`：遇到 None 时当前函数返回 None，否则得到 T。当前函数须返回 `_?`。
  - 在返回 void 的函数中使用 `?`，或在 Option 与 Result 之间传播，都是编译错误；两者之间用 `ok()`、`ok_or()` 转换。
  - `?` 的优先级使 `await f()?` 解析为 `(await f())?`。

  ```gdscript
  func load_profile(path: String) -> Result[Profile, Error]:
      var text = read_text(path)?
      return Ok(parse_profile(text)?)
  ```

- 理由：短路返回改变控制流，需要语法；其余都是对值的变换，用方法就够了，也可以直接参照 Rust 的文档。若同时提供可空链 `?.`，`x?.y` 是从函数返回还是得到 None，就要看 x 的类型。

### 8.3 断言

- `assert(条件, 消息)` 失败时是程序错误。release 构建中是否像 GDScript 一样去掉【待定】。

### 8.4 诊断

- 编译错误、警告和提示由分析服务报告，标在源码、Inspector 和场景树中（§13）。
- 程序错误的诊断写明出错的位置、相关的成员或对象，以及调用栈。本文对个别程序错误规定了诊断必须包含的内容，例如必须赋值的成员名、已释放对象的释放位置、owner 要求与实际的 owner。

## 9. 协程

### 9.1 async 与 Task

- 协程在签名上声明，写作 `async func`。函数体中有 await 而没有声明，是编译错误。引擎虚函数可以声明为 async。
- lambda 同样如此：含 await 的 lambda 写作 `async func(...)`，例如 `button.pressed.connect(async func(): await fade_out())`。GDScript 中这样的 lambda 不加标记（`modules/gdscript/gdscript_parser.cpp:3223-3225, 3726-3727`），快速修复补上 `async`。理由：判断函数是否为 async 只有一条规则，挂起点旁边一定看得到 async；[语料计数](surveys/corpus-counts.zh.md)中 979 个 lambda 只有 35 个含 await，其中 33 个在 gdUnit4 的测试中。
- 由引擎调用的位置接受 async 函数，任务作为根任务运行（§3.11）。
- 调用 async 函数得到 `Task[T]`。任务立即开始，运行到第一次挂起为止；`await f()` 和 `await t` 得到 T。
- 丢弃 async 调用的结果是编译错误；确实不等待时写 `f().detach()`。GDScript 中把协程调用的结果当值使用本来就是编译错误，所以 `var t = f()` 获得新含义不违反语法硬规则。
- setter、getter、`@onready` 初始化式和静态初始化不能 await。

### 9.2 结束状态

- 任务的结束状态有三种：
  - **完成**：得到返回值；
  - **取消**：由 `t.cancel()`、所属对象被释放或所等待的对象被释放引起；
  - **失败**：任务内部发生程序错误。
- 等待被取消的任务是程序错误。失败沿 await 链向上传播到根任务，即 detach 的任务或被引擎调用的任务，不能拦截。
- `await t.settled()` 等待任务以任何方式结束而不报错，返回 `Settled[T]`：`Finished(value: T)` 或 `Cancelled(reason: CancelReason)`，`CancelReason` 为 `Requested`、`OwnerFreed` 或 `AwaitedFreed`。不用 Result 表达，因为被取消不是错误。

### 9.3 取消与归属

- 对挂起中的任务，`cancel()` 立即断开它等待的信号或任务，任务结束为取消。
- 对正在执行的任务（只在它自己的同步代码间接取消自己时出现），执行到下一个 await 或返回时结束为取消，返回值丢弃。
- 对已结束的任务，`cancel()` 没有效果。调用 `cancel()` 之后，结果一定是取消。
- 取消沿归属传播：任务以 `await f(...)` 亲自启动并直接等待的子任务归它所有，随它一起取消；`await` 一个从别处取得的任务值时，取消只是停止等待，那个任务照常运行。两种写法在编译期即可区分。例如过场 `cutscene` 停在 `await walk_to(door)` 时被取消，`walk_to` 随之取消，角色停下。

### 9.4 释放

- 协程帧，即局部变量和挂起点，在任务结束时立即释放；持有的引用按作用域退出处理，await 之后的代码不再执行。【已知限制】第一版没有 finally 或 defer。
- Task 对象是只保存结果的 RefCounted 句柄。变量、等待方，以及等待信号期间的运行时都不再引用它时，它被释放。
- 挂起的协程不使 self 保持存活，与 GDScript 相同。

### 9.5 组合与重入

- `Task.of(信号)` 把信号包装为任务；`Task.delay(秒)` 计时；`Task.any(列表)` 返回第一个完成的任务的下标，并按归属规则取消其余任务；`Task.all(列表)` 等待全部完成。
- 重入不做自动策略，需要时用句柄取消旧的调用。【后续】`@restart` 一类的注解。依据：[暗坑清单](surveys/pitfalls.zh.md) G-12、G-13；提案 #8838、#6243。

### 9.6 await 信号

- 可以直接 await 一个信号。
- 无参数的信号不产生值；单参数的信号得到该参数类型的值；多参数信号的 await 只能作为语句。依据：[await 信号结果调查](surveys/await-signal-results.zh.md)中，1,356 个 await 只有 1 个使用了多参数信号的结果。GDScript 在这里得到由实参组成的 Array（`modules/gdscript/gdscript_function.cpp:237-255`），所以使用这种结果的 GDScript 写法是编译错误。

### 9.7 对象释放与挂起的协程

- 协程所属的对象被释放时，协程静默取消，挂起帧持有的引用随之释放，与 GDScript 相同。
- 被等待的对象（另一个对象上的协程，或另一个对象的信号源）在完成之前被释放时，等待方在 await 处报程序错误，并中止到引擎边界，诊断写明被释放的对象及释放位置；用 `settled()` 等待时得到 `Cancelled(AwaitedFreed)`。GDScript 在这种情况下让等待方永远挂起（`modules/gdscript/gdscript_function.cpp:335-349`）。
- 任何对象的释放都可以通过 GDExtension 公开的实例绑定释放回调感知。依据：[暗坑清单](surveys/pitfalls.zh.md) G-11。

### 9.8 跨语言 await

- 对 GDScript 方法的调用可以写 await。运行时得到 `GDScriptFunctionState` 时，等待它的 `completed` 信号（`modules/gdscript/gdscript_function.cpp:327`），再按声明的返回类型检查结果；没有写 await 却得到它时，是程序错误。协程性不在方法的反射信息中（`core/object/method_info.h:37-44`），因此无法在编译期判断。
- GDScript await 本语言的协程时，协程在第一次挂起时返回其 Task 的 `completed` 信号；GDScript 的 await 接受任意 Signal 值（`modules/gdscript/gdscript_vm.cpp:2585-2599`）。Task 完成时发出 `completed(值)`；取消或失败时发出 `completed(null)`，使 GDScript 的等待方不会永远挂起；另有 `cancelled` 信号供需要区分的代码使用。
- 两个方向都须在能编译引擎的环境中实测。

## 10. 引擎边界

### 10.1 引擎和 GDScript 看到的类型

| 本语言 | 引擎和 GDScript 看到的 |
|---|---|
| `int`、`float`、`String`、Node 等类 | 同名类型 |
| 对象 `T` 和 `T?` | 都是该类；非空由入口检查保证 |
| `int?` 等可空值 | 无类型的 Variant |
| 元素可由引擎表示的 `Array[T]`、`Dictionary[K, V]` | 引擎的带类型容器 |
| 无载荷的 enum | int；导出时显示为下拉框 |
| struct | 按字段名编码的 Dictionary |
| 带载荷的 enum | 按字段名编码的 Dictionary，用一个保留键记录分支名 |

- GDScript 通过 Script 的反射接口为其他语言的全局类定型（`modules/gdscript/gdscript_analyzer.cpp:804-819, 4329-4370, 6054-6063`），所以 GDScript 调用本语言的类时同样有静态检查。
- **入口检查**：值从引擎、GDScript 或 C# 进入本语言的位置（参数、`set` 写入成员、信号的实参、await 的结果、解码），按声明的类型检查，失败是程序错误。远程检查器的写入也经过入口检查（§7.5）。
- Option 交给引擎时 None 编码为 NIL。所以只有 X 的值不会编码为 NIL 时，`X?` 才能隐式交给引擎；X 是 Option 或 Variant 时是编译错误，快速修复给出 `.flatten()` 或 `.unwrap_or(null)`。否则 Some(None) 经过引擎往返后会悄悄变成 None（§3.4）。

### 10.2 引擎返回对象的可空性

- 未审核的接口一律可空。
- 审核按语料中的调用频率由高到低进行，结果分三种：
  - 非空，类型为 T；
  - 只在误用时为空，例如 `get_tree()`：按非空处理，为空时是程序错误；
  - 正常情况下可能为空，例如 `get_collider()`、`get_first_node_in_group()`：类型为 `T?`。
- 理由：引擎用 `RequiredResult` 标注的接口声明只有约 50 处，其中 29 处在 Tween。

### 10.3 绑定元数据

- 引擎实际有约束但没有导出的，在本语言的绑定元数据中补上；引擎已导出的元数据直接使用。
- 以 Variant 绑定的工具函数（lerp、clamp、min、max、abs、wrap 等）按实参定型：实参类型相同时返回该类型。int 与 float 混用是编译错误，快速修复把整数实参改为 float，例如 `min(ratio, 1.0)`。理由：引擎的 min、max、clamp 原样返回其中一个实参（`core/variant/variant_utility.cpp:643-760`），GDScript 中 `min(1.5, 1)` 得到 int，统一提升为 float 会使结果悄悄不同；lerp 本身要求两个实参类型相同（`variant_utility.cpp:466-470`）。
- 以字面量类名过滤的接口按该类定型，例如 `find_children("*", "MeshInstance3D")` 返回 `Array[MeshInstance3D]`。
- 把“失败返回 null、错误另取”的接口（如 `FileAccess.open`）封装为返回 Result 的标准库函数。

### 10.4 字符串形式的成员名

- `connect`、`emit_signal`、`call`、`call_deferred`、`set_deferred`、`has_method`、`Callable(obj, "m")`、`tween_property`，以及 `"position:x"` 一类属性路径，在目标类型已知时，字面量按静态引用解析：检查名字是否存在以及值的类型，参与重命名和查找引用。
- 哪些参数是名字，由绑定元数据标注。
- 非字面量的名字是显式的动态访问。

### 10.5 反射暴露的成员

- 引擎、GDScript 和 C# 通过反射（`get_property_list`、`has_method`，以及按名字的 `call`、`get`、`set`）只能看到：
  - `pub` 成员；
  - 引擎必须按名字找到的成员：导出属性、信号、引擎虚函数的覆盖、场景中连接的方法，以及本语言代码以字面量名字交给引擎的成员，例如 `call_deferred("reset")`、`tween_property(self, "current_speed")` 和场景动画轨道中的属性路径。后一类由编译器收集。
- 其余成员对按名字的访问不可见：GDScript 引用它们是 GDScript 的编译错误；C# 的 `Call` 和非字面量的动态访问在运行时报告找不到成员。
- 泛型方法不经反射暴露，因为按名字调用时给不出类型实参；引擎必须按名字找到的方法不能是泛型（§3.10）。
- 由方法值形成的 Callable（例如 `tween_callback(change_weapon)`、`sig.connect(_on_x)`）直接指向方法，不按名字查找，不受此限制。
- 调试器可以看到全部成员。
- 理由：不带 `pub` 的成员只会被本文件、子类和编译器收集到的字面量名字引用，改名时工具能找全。

### 10.6 数值边界

- 写入引擎的 32 位浮点位置时隐式收窄，例如标准构建中为 float 的 `real_t` 向量分量（`core/math/math_defs.h:143-146`）。
- 传给引擎 int32 参数时做范围检查，越界是程序错误；引擎自身在这里会直接截断（`core/variant/variant.h:348-349`）。

### 10.7 String 与 StringName

- 两个方向都在需要时隐式转换，与 GDScript 相同；`==` 按内容比较。
- 期望类型为 StringName 或 NodePath 时，字符串字面量在编译期构造为该类型，运行时不再转换。从 String 构造 StringName 需要在全局表的互斥锁下查找（`core/string/string_name.cpp:280-302`）。
- `&"x"` 和 `^"x"` 前缀照旧。热路径上的运行时转换给出性能提示。

### 10.8 JSON

- 引擎把 JSON 中的所有数字解析为浮点数（`core/io/json.cpp:390-396`）。解码到 `int` 时，接受恰为整数且在 int64 范围内的浮点数，否则报错，不截断。
- 标准库提供 struct 和 enum 与 JSON、`var_to_str`、ConfigFile 之间的编码和解码。

## 11. 与 GDScript 和 C# 共存

### 11.1 GDScript 类的静态视图

- 本语言通过反射看到 GDScript 类和 autoload：
  - 对象类型的成员、参数和返回值视为 `T?`；
  - 值类型照声明；
  - 没有标注类型的是动态值（§3.13）。
- autoload 名是全局标识符，类型为其脚本的类。

### 11.2 继承

- 两个方向的跨语言继承都不支持：
  - 本语言的类不能继承 GDScript 类。插件要求继承它的类时，由用户手写一层薄的 GDScript 子类，在其中调用本语言的代码；GDScript 通过反射看到本语言的类型，这些调用仍有静态检查。依据：[暗坑清单](surveys/pitfalls.zh.md) C-17。
  - GDScript 脚本不能继承本语言的类（§4.1），GDScript 此时只报“Could not resolve super class”（`modules/gdscript/gdscript_analyzer.cpp:479, 485`）。分析服务在这样的 GDScript 文件上报错并说明原因。
- 因此迁移一个类时，它仍为 GDScript 的子类必须一起迁移。迁移之前，分析服务列出受影响的子类，并提供一起迁移的选项（§13.4）。

### 11.3 跨语言调用与消息

- GDScript 和 C# 只能按名字访问 §10.5 所列的成员。
- 与 C# 之间的消息用 struct 表达，编码为按字段名的 Dictionary（§3.6），入口处解码并检查。
- 跨语言 await 见 §9.8。

## 12. 运行时

### 12.1 适配层

- 访问引擎的代码集中在一层薄的适配层中。实例的创建、释放、属性读写和方法调用都经过 GDExtension 的脚本实例接口。

### 12.2 性能

- 明显快于 GDScript 即可，不以 C# 为目标。
- 解释器须独自达到性能目标：iOS 和多数主机平台不允许运行时生成机器码；copy-and-patch 一类的 JIT 只是桌面端的额外收益。

### 12.3 线程

- 虚拟机必须能在引擎选定的任意线程上安全运行，例如后台加载资源的线程和物理线程。
- 共享对象上的数据竞争由用户负责。
- 【已知限制】第一版不新增线程 API，使用引擎的 Thread、WorkerThreadPool 和 Mutex。依据：调查所用的 20 个仓库中，`Thread.new` 共 11 处，`WorkerThreadPool` 2 处。

### 12.4 热重载

- 只修改函数体时，运行中直接替换：下一次调用使用新代码，正在挂起的协程用旧版本执行完。
- 修改成员、签名或类结构时不迁移状态，提示需要重新运行；编辑器中的 tool 脚本重建实例。

### 12.5 调试支持

- 接入引擎的调试界面：断点、单步、局部变量、调用栈。
- debug 构建记录本语言实例的释放位置，用于已释放对象的诊断（§3.3），并在退出时列出泄漏的节点和对象（§6.7）。

## 13. 工具链

### 13.1 分析服务

- 分析服务运行在编辑器进程中。外部 IDE 通过 LSP 连接；命令行和 CI 以无界面模式运行同一套分析。
- 理由：跨场景的重构要修改编辑器正打开着的场景，只有编辑器能安全地做到。GDScript 的 LSP 同样是编辑器插件（`modules/gdscript/language_server/gdscript_language_server.h:35`，`modules/gdscript/register_types.cpp:170`）。

### 13.2 第一版的工具

- 实时诊断，编辑器、LSP 和命令行使用同一套分析；
- 补全、悬停显示类型、跳转到定义、查找引用，包括场景中的引用；
- 带场景部分的重命名（§13.3）；
- 场景检查：`$X`、信号连接、`PackedScene[T]`、父节点和 owner 要求、必须赋值的导出，错误标在场景编辑器中；
- 快速修复：插入推断出的类型、补全 match 分支、为动态值加显式转换，以及本文各处提到的改写；
- 调试器（§12.5）；
- 自带的测试运行器：发现、运行、报告，可以无界面运行。理由：gdUnit4 和 GUT 要求测试类继承它们的 GDScript 基类，而跨语言继承不受支持。

### 13.3 重构

- 重构包含场景中的那一半。机械且不改变含义的修改随重构自动完成，事先预览，可以撤销，包括改名所涉及的导出属性键、信号连接的方法名、动画轨道路径，以及无损的值转换。
- 需要判断的修改只报告并给出选项，不自动执行，例如改变节点类型、现有资源不再符合新类型。
- 理由：目前导出成员改名后，场景中保存的值会静默丢失：加载时不检查 `node->set` 是否成功（`scene/resources/packed_scene.cpp:567`），保存时只写节点当前属性列表中的属性（`packed_scene.cpp:941-950`）。
- 在场景编辑器中给节点改名或移动节点时，本语言脚本中指向它的 `$X`、`%X` 和字面量 `get_node` 路径自动更新，与编辑器对场景中 NodePath 属性和动画轨道的更新（`editor/docks/scene_tree_dock.cpp:2173-2260, 2280-2400`）放在同一个撤销步骤中，不预览，完成后在输出面板列出改动的位置。非字面量的路径，以及脚本挂在其他场景中、同一路径在那里仍指向别的节点的情况，只报告不修改，并提供更新路径的快速修复。

### 13.4 迁移

- 迁移由编译错误和快速修复引导。机械的修改，例如补 `pub`、补参数类型、改写真值判断、补 `.detach()`，可以由快速修复完成。依据：[第二次纸面迁移](surveys/paper-migration-2.zh.md)中，230 处修改有 197 处是机械的。
- 迁移一个类之前，列出它仍为 GDScript 的子类，并提供一起迁移的选项（§11.2）。

### 13.5 以后再做

- 【后续】格式化；提取方法、修改签名、把成员提到基类等重构；性能分析的深度集成；文档生成；为要求继承的 GDScript 插件类生成转发用的 GDScript 外壳。

## 14. 第一版范围与待定问题

### 14.1 已知限制

- 不支持跨语言继承，两个方向都不支持（§11.2）。
- 没有命名空间、别名和 `import`（§2.4、§4.1）。
- struct 不能 `@export`，也不能存入 `.tres`（§3.6）。
- 没有带状态的 mixin（§3.9）。
- match 没有类型模式、没有 match 表达式，也不能在逗号分隔的多个模式中绑定（§5.6）。
- 协程没有 finally 或 defer（§9.4）。
- 没有“就绪后才赋值的非空成员”（§6.2）。
- 不新增线程 API（§12.3）。

### 14.2 以后可以兼容地加入

- struct 构造的位置参数（§3.6）；
- 更严的 `private` 可见性（§4.2）；
- 条件绑定，或检查与使用之间没有调用时让字段收窄（§5.4）；
- 重入策略的注解，例如 `@restart`（§9.5）；
- 别名或 `import`（§2.4）；
- 调用处的展开写法（§4.6）；
- 泛型的多个约束，以及以类作约束（§3.10）；
- 函数类型中的参数名、直接调用 `f(x)`、把普通函数传给期望 async 的位置（§3.11）；
- §13.5 所列的工具。

### 14.3 待定问题

以下各项对应[设计决定](decisions.zh.md) §10：

| 问题 | 章节 |
|---|---|
| struct 和带载荷 enum 的方法、修改、`==`、哈希和导出 | §3.6、§3.7、§5.2 |
| 接口能否继承接口、能否是泛型 | §3.9 |
| `assert` 在 release 构建中是否去掉 | §8.3 |
| 由脚本声明的三种场景结构的写法 | §7.2 |
| 源文件的扩展名 | §1.4 |

## 15. 一致性检查清单（非规范性）

本清单列举前述规则的检查实例，不增加语言要求。

| 领域 | 检查内容 |
|---|---|
| 语法硬规则 | 本文中每一处与 GDScript 不同的规则，对应的 GDScript 写法都得到编译错误和改法，而不是另一种运行时行为 |
| 数值 | `var hp: int = 10; hp = 2.5` 报编译错误，`hp = int(2.5)` 得 2；int 实参传给 float 参数无需转换；`7 / 2` 得 3；`Vector2i(1, 2) * 0.5` 为 Vector2；`Vector2() + Vector2i()` 报编译错误；整数除以 0 是程序错误；超出 int32 范围的值传给引擎的 int32 参数是程序错误 |
| 可空与释放 | 已释放对象经 `T?` 读出为 None；经 `T` 访问成员是程序错误，诊断写明释放位置；`queue_free()` 之后、帧末之前仍算存活 |
| Option 与 Result | `?` 在 None 或 `Err` 时从当前函数返回；在返回 void 的函数中使用 `?` 报编译错误；Option 传播到返回 Result 的函数报编译错误；`await f()?` 等于 `(await f())?`；丢弃 Result 报编译错误；`unwrap` 失败是程序错误 |
| 相等 | `1 == 1.0` 为真；`hp == "3"` 报编译错误；`Node2D` 与 `Node3D` 比较报编译错误；hp 为 `int?` 时 `hp == 3` 合法；非空的 `Node` 与 None 比较报编译错误；`Array[int] == Array[float]` 报编译错误；Variant 中是 String 时与 int 比较是程序错误 |
| 嵌套的 Option | `Array[Enemy?]` 为空时 `pop_back()` 得 None，末元素为 None 时得包含 None 的 Some；`var x: Enemy?? = maybe_enemy` 得 Some；`Dictionary[String, Variant]` 中缺键得 None，值为 null 得包含 NIL 的 Some；把 `int??` 或 `Variant?` 传给 Variant 参数报编译错误并给出 `.flatten()` 或 `.unwrap_or(null)` |
| 转换 | `node as Sprite2D` 为 `Sprite2D?`；`x as int` 报编译错误并给出 `int(x)` 与 `int.try_from(x)`；`int.try_from(2.0)` 得 `Ok(2)`，`int.try_from(2.5)` 和 `int.try_from("2")` 得 `Err` |
| 收窄 | `!= None`、`is` 和提前返回之后局部变量收窄；条件表达式的分支和 `and` 右侧收窄；成员不收窄；`if x:` 报编译错误并按类型给出改法 |
| 可见性 | 不带 `pub` 的成员在其他类中不可见，在子类中可以读写和覆盖；覆盖时改变可见性报编译错误；`pub` 函数的签名中出现非 `pub` 的类型报编译错误；GDScript 引用不带 `pub` 的成员报 GDScript 编译错误；`call_deferred("reset")` 使 `reset` 可以被引擎按名字调用 |
| 签名 | `func _process(delta):` 中 delta 为 float；覆盖时写出不一致的类型报编译错误；非覆盖函数省略返回类型后返回值报编译错误；有期望类型的 lambda 参数不必标注 |
| 泛型 | `first([1, 2])` 中 T 推断为 int；`var x = first([])` 推断不出 T 报编译错误，`var x = first[int]([])` 合法；不满足约束的类型实参报编译错误；Node 脚本声明类型参数报编译错误；GDScript 引用泛型方法是 GDScript 的编译错误；`sig.connect(handle)` 在 handle 为泛型且类型实参确定不了时报编译错误 |
| Variant | `v.hp`、`v + 1`、`v[0]`、`for x in v` 报编译错误；`if v is Dictionary:` 之后 `v.get("hp")` 合法；`v as Node` 为 `Node?`；v 为 NIL 时 `v == None` 为真、`v is int` 为假；int 赋给 Variant 无需转换 |
| 函数类型 | `func(int)` 与 `func(int) -> void` 相同；`(func(Enemy) -> bool)?` 可空，`func(Enemy) -> bool?` 返回 `bool?`；`f(x)` 报编译错误并给出 `f.call(x)`；`func(Node)` 可以用于期望 `func(Node3D)` 的位置，反之报编译错误；含 await 而未写 async 的 lambda 报编译错误；把 async 函数传给期望普通函数类型的本语言参数报编译错误；把 async 方法连接到信号合法，任务作为根任务运行 |
| 抽象类与剩余参数 | 构造抽象类报编译错误；具体子类缺少某个抽象方法的实现报编译错误；场景中的节点使用抽象类报编译错误；向 `...args: Array[int]` 传入 String 报编译错误；只写 `...args` 报编译错误；GDScript 调用 `...args: Array[int]` 时传入 String 是程序错误 |
| 成员 | `var speed = 5.0` 为 float；空容器成员要求标注；没有初始化式的 int 成员为 0；所有字段都有默认值的 struct 成员取默认值，否则必须赋值 |
| 必须赋值 | `BulletScene.instantiate(speed = 300.0)` 漏写 `target` 报编译错误；祖先在 `_ready` 中经 `$Child.target = v` 赋值时不报错；子节点在自己的 `_ready` 中读取该成员报编译错误；就绪前读取未赋值的成员是程序错误 |
| 导出检查 | player.tscn 的 `crosshair` 由 main.tscn 的实例赋值时不报错；某个实例没有赋值时标在该实例所在的场景上；没有被引用的场景只给警告且不阻止导出；运行中缺值时就绪报程序错误并写明成员名 |
| 初始化 | 成员初始化式不调用 setter；`@onready` 与 `@export` 同用报编译错误；`_init` 中读取 `@onready` 成员报编译错误；静态初始化的环报错并列出依赖链 |
| 就绪前的 setter | 场景中保存的值使 setter 在访问 `@onready` 成员处挂起，就绪后继续；就绪前多次写入只有最后一次继续；变换型 setter 就绪前读到变换后的值；节点就绪前被释放时挂起的调用被丢弃 |
| 场景定型 | `$Head/Camera` 按场景定为 Camera3D；两个场景给出不同类型时报错；找不到的路径标在缺少该节点的场景上；`@owner(T)` 放在场景根上报编译错误；节点移出 owner 的子树后不读 owner 不报错，读取时是程序错误 |
| 资源 | 修改共享资源给出警告和两个快速修复；`duplicate()` 保持静态类型；`preload` 环在运行前报告 |
| match | enum 的 match 不穷尽报编译错误；带 `when` 的分支不计入覆盖；未列出字段而没有 `..` 报编译错误；Option 的 `var x` 分支中 x 为 T；对 int 的 match 不要求穷尽 |
| 循环 | `for i in 5` 中 i 为 int；`for x in 2.5` 和遍历向量报编译错误；循环变量标注为子类报编译错误 |
| 容器 | `pop_front()` 在空数组上得 None；`front()` 在空数组上是程序错误；越界下标是程序错误；元素为 struct 的数组交给引擎须显式转换且得到副本 |
| 协程 | 含 await 而未声明 async 报编译错误；丢弃 Task 报编译错误；`cancel()` 之后结果一定是取消；`await f(...)` 启动的子任务随父任务取消，`await t` 只停止等待；被等待的对象被释放时等待方报程序错误，`settled()` 得 `Cancelled(AwaitedFreed)`；所属对象被释放时协程静默取消 |
| 信号 | emit 实参不符和 connect 参数不符报编译错误；场景中的连接与信号不符时标在场景中；单参数信号的 await 结果有该参数类型；使用多参数信号的 await 结果报编译错误 |
| 跨语言 | GDScript 文件继承本语言的类时，分析服务在该文件上报错并说明原因；await GDScript 协程得到按声明类型检查的结果；GDScript await 本语言的协程，在取消时得到 null 而不挂起 |
| 工具函数 | `min(1.5, 1)` 报编译错误并给出 `min(1.5, 1.0)`；`lerp(a, b, t)` 的结果与 a 同类型；`find_children("*", "MeshInstance3D")` 为 `Array[MeshInstance3D]` |
| 字符串 | 期望 StringName 的字面量在编译期构造；`&"a" == "a"` 为真；字面量成员名不存在时报编译错误，并参与重命名 |
| 重构 | 导出成员改名时场景中的键同时修改；场景编辑器中给节点改名时脚本中的 `$X` 同时修改，并在同一撤销步骤中；非字面量路径只报告 |
| 泄漏 | 移出后未保存也未释放的节点给出警告；捕获 self 的闭包存入 self 的成员时给出警告和 `weak` 的快速修复；debug 构建退出时列出泄漏的对象和创建位置 |

## 16. 反馈

欢迎通过 [GitHub issues](https://github.com/YYF233333/Petrichor/issues) 提出反馈。可采用以下格式：**问题 → 最小反例 → 对应章节 → 严重程度 → 最小修改 → 测试**。

## 附录 A：引擎事实

以下事实基于 Godot master（4.8-beta，提交 `469659e`），路径相对于 Godot 仓库的根目录。

| 事实 | 出处 |
|---|---|
| Variant 的整数是 int64_t，浮点数是 double | `core/variant/variant.h:257-258` |
| Variant 转为 int32 时直接截断 | `core/variant/variant.h:348-349` |
| 标准构建中 `real_t` 是 float | `core/math/math_defs.h:143-146` |
| 引擎的运算符表 | `core/variant/variant_op.cpp:256-368` |
| int 与 float 的 `==` 按数值比较；对象的 `==` 比较同一性 | `core/variant/variant_op.cpp:490-491, 514` |
| 运算表中没有的组合求值无效 | `core/variant/variant_op.cpp:1051-1056` |
| Array 的 `==` 逐元素用 `hash_compare`，它要求元素类型相同 | `core/variant/array.cpp:153-156`；`core/variant/variant.cpp:3213-3216` |
| min、max、clamp 原样返回其中一个实参 | `core/variant/variant_utility.cpp:643-760` |
| lerp 要求两个实参类型相同 | `core/variant/variant_utility.cpp:466-470` |
| `pop_front`、`pop_back` 在空数组上返回 null 而不报错 | `core/variant/array.cpp:798-817` |
| `front`、`back` 在空数组上报错 | `core/variant/array.cpp:364-372` |
| Dictionary 的 `get` 在缺键时返回默认值 | `core/variant/dictionary.cpp:177-188` |
| `bind` 把绑定的实参接在调用实参之后 | `core/variant/callable_bind.cpp:141-150` |
| 从 String 构造 StringName 需要在全局表的互斥锁下查找 | `core/string/string_name.cpp:280-302` |
| JSON 中的数字都解析为浮点数 | `core/io/json.cpp:390-396` |
| 方法的反射信息中没有协程性 | `core/object/method_info.h:37-44` |
| 脚本语言所需的钩子对 GDExtension 公开 | `core/object/script_language_extension.h:393, 493, 533, 596, 754` |
| 扩展实现的脚本语言经 `_is_abstract` 和全局类表报告抽象性 | `core/object/script_language_extension.h:143-147, 756-769` |
| `_process` 和 `_physics_process` 的参数是 double | `scene/main/node.h:433-434` |
| `NOTIFICATION_PARENTED` | `scene/main/node.h:466` |
| 节点移出 owner 的子树时 owner 被置为 null | `scene/main/node.cpp:391-396, 1799-1801` |
| owner 被释放时，它拥有的节点的 owner 被置为 null | `scene/main/node.cpp:299-302` |
| 节点入树时，就绪同步传播完毕 | `scene/main/node.cpp:3425-3429` |
| 实例化场景时，先设置保存的属性，再把节点加到父节点下 | `scene/resources/packed_scene.cpp:567, 616` |
| 加载场景时不检查属性写入是否成功 | `scene/resources/packed_scene.cpp:567` |
| 节点引用以路径保存，实例化时解析 | `scene/resources/packed_scene.cpp:449-468` |
| 保存场景时只写节点当前属性列表中的属性 | `scene/resources/packed_scene.cpp:941-950` |
| 编辑器给节点改名时，更新场景中的 NodePath 属性和动画轨道 | `editor/docks/scene_tree_dock.cpp:2173-2260, 2280-2400` |
| GDScript 通过 Script 的反射接口为其他语言的全局类定型 | `modules/gdscript/gdscript_analyzer.cpp:804-819, 4329-4370, 6054-6063` |
| GDScript 解析 `extends 全局类名` 时把基类文件当作 GDScript 解析，失败时只报“Could not resolve super class” | `modules/gdscript/gdscript_analyzer.cpp:471-487`（报错在 `:479, 485`） |
| GDScript 要求具体子类实现抽象方法，禁止构造抽象类和经 `super` 调用抽象方法 | `modules/gdscript/gdscript_analyzer.cpp:1543-1562, 3674-3687` |
| GDScript 的剩余参数只能是元素为 Variant 的 Array | `modules/gdscript/gdscript_analyzer.cpp:1802-1809` |
| GDScript 从其他语言的方法信息中读取 `METHOD_FLAG_VARARG` | `modules/gdscript/gdscript_analyzer.cpp:6101-6104` |
| GDScript 中以调用函数的写法调用 Callable 变量是编译错误，提示改用 `.call()` | `modules/gdscript/gdscript_analyzer.cpp:3785-3789` |
| GDScript 中 `?` 是语法错误 | `modules/gdscript/gdscript_parser.cpp:3834-3835` |
| GDScript 的前缀 `!` 是逻辑非 | `modules/gdscript/gdscript_parser.cpp:2965-2967` |
| GDScript 中内部类名、`class_name` 的名字、enum 名和函数名之后不能接 `[` | `modules/gdscript/gdscript_parser.cpp:964, 1005-1010, 1616, 1822` |
| 抽象方法的声明不写冒号 | `modules/gdscript/gdscript_parser.cpp:1790-1791` |
| `@abstract` 注解，不能用于 static 方法 | `modules/gdscript/gdscript_parser.cpp:149, 4492-4516` |
| 剩余参数只能是最后一个参数，不能有默认值 | `modules/gdscript/gdscript_parser.cpp:1723-1730` |
| GDScript 的表达式中不能出现 `...` | `modules/gdscript/gdscript_parser.cpp:4336` |
| GDScript 的类型只能以标识符或 `void` 开头 | `modules/gdscript/gdscript_parser.cpp:3849-3861` |
| GDScript 的 lambda 含 await 时即为协程，不加标记 | `modules/gdscript/gdscript_parser.cpp:3223-3225, 3726-3727` |
| match 的模式 | `modules/gdscript/gdscript_parser.cpp:2499-2723` |
| `as` 转为对象类型失败时得到 null | `modules/gdscript/gdscript_vm.cpp:1665-1666` |
| `as` 转为内置类型时按构造函数转换 | `modules/gdscript/gdscript_vm.cpp:1630-1637` |
| GDScript 在运行时遇到运算表中没有的组合时报错 | `modules/gdscript/gdscript_vm.cpp:756-761, 794-802` |
| `for` 可以遍历 float 和向量 | `modules/gdscript/gdscript_vm.cpp:3093-3230` |
| GDScript 的 await 接受任意 Signal 值 | `modules/gdscript/gdscript_vm.cpp:2585-2599` |
| GDScript 出错后按返回类型构造默认值返回 | `modules/gdscript/gdscript_vm.cpp:3975-3979` |
| 成员的初始化式不经过 setter | `modules/gdscript/gdscript_compiler.cpp:2435-2451, 2620-2643` |
| 静态成员初始化之后调用 `_static_init` | `modules/gdscript/gdscript_compiler.cpp:2646-2649` |
| 引擎不实例化抽象的 GDScript 脚本 | `modules/gdscript/gdscript.cpp:265-270` |
| `@onready` 初始化式在用户的 `_ready` 之前执行，先基类后派生类 | `modules/gdscript/gdscript.cpp:1956-1972` |
| `GDScriptFunctionState` 有 `completed` 信号 | `modules/gdscript/gdscript_function.cpp:327` |
| await 多参数信号得到由实参组成的 Array | `modules/gdscript/gdscript_function.cpp:237-255` |
| 被等待的对象被释放后，GDScript 的等待方永远挂起 | `modules/gdscript/gdscript_function.cpp:335-349` |
| RefCounted 类中的 lambda 强引用 self | `modules/gdscript/gdscript_lambda_callable.cpp:285-291` |
| GDScript 的 LSP 是编辑器插件 | `modules/gdscript/language_server/gdscript_language_server.h:35`；`modules/gdscript/register_types.cpp:170` |

## 附录 B：考虑过但没有采用的方案（非规范性）

| 方案 | 不采用的原因 | 现行规则 |
|---|---|---|
| 以引擎模块交付（v0.1） | 暗坑清单中没有只有模块才能修复的条目；GDExtension 可以用官方编辑器和导出模板 | §1.3 |
| 窄整数类型 i32、u8 等（v0.1） | 迁移成本最大的一项；脚本不在意数值类型的性能 | §3.2 |
| 推断函数签名（v0.1） | 签名是对外的约定，不应随函数体变化 | §4.6 |
| 语言层面的跨语言继承 | 依赖 GDScript 的内部实现 | §11.2 |
| 可空链 `?.`、默认值 `??`、后缀 `!` | 与 Result 的短路返回合用时，`x?.y` 的含义要看 x 的类型；Option 的操作用方法就够了 | §3.4、§8.2 |
| Swift 式的 `try` 覆盖整条表达式 | 要按类型决定哪些子表达式被解开；Rust 的后缀 `?` 更直接 | §8.2 |
| Result 也支持链式的 `?.` | 两种类型嵌套时，一个运算符无法说明处理的是哪一层 | §8.2 |
| setter 的 `apply:` 块 | 让用户思考逻辑在何时执行、执行几次；自动挂起可以由编译器完成 | §6.5 |
| 就绪前只记下 setter 的原始输入 | 变换型 setter（如给角色名加【】）就绪前会读到未变换的值 | §6.5 |
| `@late`（类似 `lateinit`） | 需要它的写法很少见；父节点配置子节点改由祖先赋值的规则覆盖 | §6.2 |
| 子类只能看到 `pub` 成员 | 迁移中 102 处 `pub` 有 29 处只是为了子类 | §4.2 |
| 公开标记用 `@public` 注解 | 写得频繁，每千行至少 22 处 | §2.4 |
| 文件顶层的类型直接成为全局名 | 打破“文件就是类”，需要另一张全局名表 | §2.4 |
| 接口的交集类型 `@owner(Node2D & Combatant)` | 新概念；改为允许引擎属性满足接口的属性要求 | §3.9 |
| 追踪式 GC，或闭包自动弱捕获 self | 与 GDScript 的行为悄悄不同 | §6.7 |
| 在移动节点时检查 owner | 节点移出 owner 的子树是合法操作 | §7.3 |
| 没有被引用的场景缺值时也阻止导出 | 运行时才确定路径的 `load` 看不到，无法断定场景不被使用 | §7.5 |
| `as` 转为内置类型时严格检查 | 与 GDScript 的构造转换悄悄不同 | §5.3 |
| int 与 float 混用的工具函数统一提升为 float | GDScript 原样返回实参，结果会悄悄不同 | §10.3 |
| `pop_front()` 在空数组上是程序错误 | 引擎在这里返回 null 而不报错，会悄悄不同 | §3.8 |
| await 多参数信号得到 Array，或按参数解构 | 真实代码几乎不使用这种结果；解构可以以后加入 | §9.6 |
| 以 if/else 块为值 | 与条件表达式重复 | §5.5 |
| 嵌套的 Option 合并为一层（Kotlin 的做法） | 泛型代码的行为随 T 改变；容器中存入的 None 与缺少的值无法区分 | §3.4 |
| lambda 不写 async，按函数体或期望类型推断 | 判断函数是否为 async 要用多条规则，lambda 和方法值能传到哪里也会不同 | §9.1 |
