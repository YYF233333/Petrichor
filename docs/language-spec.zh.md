# Petrichor 语言规范

**v0.2 草案 · 2026-10-25** · English: [language-spec.en.md](language-spec.en.md)

Petrichor 是一门面向中大型 Godot 4 项目的全静态类型脚本语言，在项目代码中替代 GDScript，与 GDScript 插件共存，以 GDExtension 的形式交付。本文是它的语言规范草案，发布出来征求意见；实现按[实现架构](architecture.zh.md)进行，尚未覆盖全部规则。上一版 v0.1 保存在 git 标签 `spec-v0.1` 中。

## 1. 概述

### 1.1 问题与目标

- 用动态语言写的大型 Godot 项目难以管理。GDScript 受历史兼容性约束，摆脱不了动态类型；即使脚本全部写成静态类型，节点路径、编辑器中连接的信号、动画轨道和资源的属性名仍然是动态的，因为场景中的静态信息到不了脚本里。改用 C# 则语言支持降为二等，跨语言桥接有暗坑，部分引擎能力受限，工具链与 Godot 的结合也有坑。
- 本语言是全静态类型的，只在引擎或第三方强加的地方出现动态值（§3.13）。静态检查延伸到场景：节点路径、场景中连接的信号、动画轨道、导出属性和场景资源都有类型，改名时连同场景一起修改（§7、§13）。
- 目标用户是会读、会改代码的开发者，项目规模中等以上，部分代码可以由 AI 生成。小项目用 GDScript 更合适。
- 以开新项目为主。第三方插件保持 GDScript，不要求迁移（§11）。
- 性能明显快于 GDScript 即可，不以 C# 为目标（§12.2）。

### 1.2 与 GDScript 的关系

- 表层语法沿用 GDScript：缩进、`func`、`var`、`extends`、`signal`、`@export`、`$X` 等。新增的构造按同一风格扩展（§2）。
- **语法硬规则**：与 GDScript 写法相同的构造，行为要么与 GDScript 一致，要么是编译错误并给出改法，不能悄悄不同。例外是 GDScript 在运行时出错、或悄悄得到错误结果（默认值、截断、永远挂起）的地方：本语言在这里改为程序错误（§8.1），或者给出代码本来想要的结果，例如就绪前的 setter（§6.5）。本文中每一处与 GDScript 不同的规则都满足这一条；规则说明中提到“与 GDScript 相同”时，指的是这一条所要求的一致。
- 本语言不承诺能直接编译 GDScript 源码。迁移由编译错误和快速修复引导（§13.4）。

### 1.3 交付形态

- 以 GDExtension 交付，实现 Godot 的脚本语言扩展接口（`core/object/script_language_extension.h:304, 391, 441, 497, 655`）。访问引擎的代码集中在一层薄的适配层中，保留以后改编为引擎模块的可能。
- GDExtension 可以装进官方编辑器，使用官方导出模板。代价是 Web 导出须启用“Extensions Support”，使用带动态链接的导出模板（`platform/web/export/export_plugin.cpp:374`，`platform/web/export/export_plugin.h:59-73`）；跨域隔离响应头只在同时开启线程支持时才需要（`platform/web/doc_classes/EditorExportPlatformWeb.xml:93-95`）。
- 理由：[暗坑清单](surveys/pitfalls.zh.md)中没有只有模块才能修复的条目；GDExtension 修不了的部分需要修改引擎，模块同样修不了。
- 源码只在编辑器和无界面模式下编译。导出的游戏只含字节码，导出模板使用的库不含编译器。理由：`$X`、`@parent` 和 `PackedScene[T]` 的类型取决于场景（§7），在加载时编译就要在游戏中重新分析场景，并把分析器带到每个导出平台。GDScript 导出时至多做到词法化，加载时才解析和编译（`modules/gdscript/register_types.cpp:99-116`，`modules/gdscript/gdscript_cache.cpp:373-379, 392`）。

### 1.4 文档约定

- 定义合法程序和所需行为的规则具有规范性。理由、依据、例子、§15 和附录 B 不具有规范性。
- “依据”指向 `surveys/` 中的调查，每份都附有复现它的脚本：[场景结构](surveys/scene-structures.zh.md)、[跨分支重复](surveys/cross-branch-duplication.zh.md)、[暗坑清单](surveys/pitfalls.zh.md)、[属性访问器](surveys/property-accessors.zh.md)、[await 信号结果](surveys/await-signal-results.zh.md)、[成员可见性](surveys/member-visibility.zh.md)、[纸面迁移](surveys/paper-migration.zh.md)（按 v0.1 迁移）、[第二次纸面迁移](surveys/paper-migration-2.zh.md)（把两个真实项目在纸面上改写为本语言）、[语料计数](surveys/corpus-counts.zh.md)。
- 状态标记：
  - **【待定】**：尚未决定，汇总在 §14.3；
  - **【已知限制】**：第一版不支持，限制程序合法性的规则仍具有规范性；
  - **【后续】**：以后可以兼容地加入，不属于第一版。
- **编译错误**指分析服务在运行之前报告、使程序不合法的错误。唯一的例外是标在场景上的缺值错误，它不阻止在编辑器中运行（§7.5）。**警告**和**提示**不影响合法性。**程序错误**指 §8.1 定义的运行时错误。
- 文中的引擎行为都对照 Godot 4.7.2-stable（提交 `ed1daf0`）核对过，出处列在附录 A。这是核对基线，不是支持版本声明。
- Petrichor 是暂定名。源文件扩展名暂定为 `.pet`，在语言首次发布之前与正式名称一起确定，因为发布后再改，场景中指向脚本的路径都要跟着改。扩展名须短且全小写，不与 Godot 已用的扩展名（如 `.gd`、`.tscn`）重复，也不与 GitHub Linguist 和常用编辑器中的其他语言重复。脚本语言通过 `get_extension` 自报扩展名（`core/object/script_language_extension.h:236`）。

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
  - 新增后缀运算符 `?`（§8.2）。GDScript 中 `?` 是语法错误（`modules/gdscript/gdscript_parser.cpp:3860-3861`）。
  - 前缀 `!` 仍是逻辑非（`gdscript_parser.cpp:2963-2965`）。
  - `null` 是 None 的别名，在所有位置的含义与 GDScript 一致：`x = null` 给 `T?` 赋空值，`x != null` 与 `x != None` 相同并带来收窄，传给 Variant 时引擎得到 NIL。把它赋给非空的 `T` 是编译错误，快速修复把类型改为 `T?`。提示建议新代码写 None。理由：[语料计数](surveys/corpus-counts.zh.md)中 `null` 每千行 22.2 处，改成编译错误只会增加机械的迁移工作。
- 字面量：整数、浮点数、字符串（含三引号和 `r` 前缀）、`&"…"`（StringName）、`^"…"`（NodePath）、数组 `[…]`、字典 `{k: v}` 和 `{k = v}`、`true`、`false`、`None`，与 GDScript 相同；`None` 是 Option 的空值（§3.4）。字面量的类型见 §3.2 和 §10.7。

### 2.3 注解

- 沿用 GDScript 的注解，含义相同：`@export` 系列、`@onready`、`@tool`、`@icon`、`@warning_ignore`、`@rpc`、`@static_unload`、`@abstract`（§4.1）。
- 新增的类级注解：`@parent(T)`、`@owner(T)`（§7.3）。
- 新增的成员注解：`@readonly`，只用于接口属性（§3.9）。
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

- **interface**：缩进块，可以声明 `var`、`signal` 和 `func`。没有函数体的方法不写冒号，与 GDScript 的 `@abstract func` 相同（`gdscript_parser.cpp:1773-1774`）；有函数体的是默认方法。属性要求可读写，只要求可读的标 `@readonly`，例如 `@readonly var max_hp: int`。接口的成员都是公开的：

  ```gdscript
  pub interface Damageable:
      var hp: int
      signal died
      func take_damage(amount: int)
      func is_dead() -> bool:
          return hp <= 0
  ```

- **实现接口**：类头写 `implements A, B`（§2.1），不并入 `extends`，使基类与接口一眼可分。接口继承接口写作 `interface Combatant extends Damageable, Healable:`（§3.9）。
- **pub**：关键字，写在注解之后、`static` 之前，例如 `@export pub var max_hp: int`、`pub static func make() -> Player`、`pub signal died`。struct、enum、interface 和内部类同样用 `pub` 公开，例如 `pub interface Damageable:`（§4.2）。用关键字而不用注解，因为它写得频繁：[成员可见性调查](surveys/member-visibility.zh.md)中每千行至少 22 处。
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

  调用处通常由实参推断，推断不出时显式写出，例如 `first[int]([])`、`Pair[int, String](first = 1, second = "a")`。`名字[...]` 与下标同形，由名字解析区分：名字是泛型函数或泛型类型时，方括号中是类型实参，否则是下标。GDScript 在内部类名、`class_name` 的名字、enum 名和函数名之后遇到 `[` 都是语法错误（`gdscript_parser.cpp:964, 1005-1010, 1602, 1805`），所以这些声明写法不违反语法硬规则。
- **函数类型**：沿用函数声明的形状，去掉函数名和参数名（§3.11）。GDScript 的类型只能以标识符或 `void` 开头（`gdscript_parser.cpp:3874-3886`），`func(...)` 作类型是语法错误。

  ```gdscript
  var on_hit: func(int, Node3D)                 # 没有返回值，也可以写 -> void
  var score: func(Enemy) -> float
  var load_level: async func(String) -> Level   # 调用得到 Task[Level]
  var filter: (func(Enemy) -> bool)?            # 可空须加括号，否则 ? 属于返回类型
  ```

### 2.5 语句

- `var`、`const`、`if`/`elif`/`else`、`while`、`for`、`match`、`break`、`continue`、`pass`、`return` 与 GDScript 相同，规则见 §5。
- `assert(条件, 消息)` 与 GDScript 相同，release 构建中去掉（§8.3）。
- 没有以 if/else 块为值的写法；条件表达式只有 `a if c else b`（§5.5）。

## 3. 类型

### 3.1 一览

| 类别 | 类型 |
|---|---|
| 数值与布尔 | `int`（64 位）、`float`（64 位）、`bool` |
| 字符串 | `String`、`StringName`、`NodePath` |
| core 值类型 | `Vector2`、`Vector2i`、`Vector3`、`Vector3i`、`Vector4`、`Vector4i`、`Color`、`Rect2`、`Rect2i`、`Transform2D`、`Transform3D`、`Basis`、`Quaternion`、`Plane`、`AABB`、`Projection`、`RID`，以及 `Packed*Array` |
| 容器 | `Array[T]`、`Dictionary[K, V]`（§3.8） |
| 对象 | 引擎类、本语言的类、GDScript 和 C# 的全局类；非空 `T` 与可空 `T?`（§3.3）；场景资源 `PackedScene[T]`（§7.4） |
| 本语言的值类型 | struct（§3.6）、enum（§3.7） |
| 接口 | §3.9 |
| 标准类型 | `T?`（Option，§3.4）、`Result[T, E]`（§3.5）、`Task[T]` 与 `Settled[T]`（§9）、`weak[T]`（§3.12）、`ConversionError`（§5.3） |
| 函数 | 函数类型（§3.11）、`Callable`、`Signal` |
| 动态 | `Variant`（§3.13） |

### 3.2 数值

- 数值类型只有 `int` 和 `float`，都是 64 位，与 Variant 一致（`core/variant/variant.h:252-253`）。没有供用户使用的窄类型；固定位宽的运算使用 `int` 上的回绕运算方法和位运算。
- 整数字面量的类型是 `int`，浮点字面量的类型是 `float`。
- `int` 在赋值、传参和返回时隐式转为 `float`。`float` 转为 `int` 必须写 `int(x)`，否则是编译错误。理由：`var hp = 10` 在本语言中推断为 int（§4.3），在 GDScript 中却没有类型，`hp = 2.5` 之后为 2.5；隐式截断会让结果悄悄不同。
- 运算的结果类型按 §5.1。
- 与引擎之间的数值边界按 §10.6。

### 3.3 对象与可空

- 对象类型 `T` 的值总是指向一个对象；`T?` 的值是一个对象或 None。
- 对象被释放后：
  - 读取 `T?` 类型的引用时，已释放的对象读作 None，所以 `!= None` 检查和由它带来的收窄（§5.4）同时就是存活检查；
  - `T` 类型的引用仍可以持有已释放的对象，读取、比较和传参不算访问，访问其成员是程序错误；`==` 比较 ObjectID，所以两个不同的已释放对象不相等（§5.2）。debug 构建中，若对象是本语言的实例或由本语言代码释放，诊断写明它在哪里被释放；
  - `is` 对已释放的对象为假，`as` 得到 None，对所有静态类型都如此，所以类型检查同时也是存活检查。GDScript 在这里运行时出错（`modules/gdscript/gdscript_vm.cpp:942-947, 1665-1669`）；
  - `queue_free()` 之后、帧末真正删除之前，对象仍算存活：引擎把它放入删除队列，在每个物理步和处理帧的末尾删除（`scene/main/node.cpp:3461-3471`，`scene/main/scene_tree.cpp:666, 735, 1626-1643`）。`is_instance_valid` 的行为与 GDScript 相同。
- 每次读取 `T?` 类型的对象引用都要做一次存活查询。
- 理由：[语料计数](surveys/corpus-counts.zh.md)中 `is_instance_valid` 每千行 3.3 处，phantom-camera 中每千行 12.8 处；引擎从 4.4 起也让已释放的对象在条件中为假（PR [#93885](https://github.com/godotengine/godot/pull/93885)，`core/variant/variant.cpp:952-953`）。依据：[暗坑清单](surveys/pitfalls.zh.md) G-07、G-08。

### 3.4 Option

- `T?` 就是 Option，值为 `Some(v)` 或 None，v 是 T 的值。T 的值隐式包装为 Some；需要显式写出时用 `Some(v)`，例如 `T??` 中的 `Some(None)`。S 是 T 的子类型或实现了接口 T 时，`S?` 可以用在期望 `T?` 的位置。
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

  理由：合并为一层时，泛型代码的行为随 T 改变，例如 `first[T]` 在 T 为 `Enemy?` 时分不清空数组和首个元素为空，通用缓存会把存入的 None 当作没有缓存；JSON 中缺少的键与 null 也无法区分。本语言没有 `?.`，嵌套基本只出现在泛型和容器取值上。

### 3.5 Result

- `Result[T, E]` 表示预期之内可能失败的操作。用 `Ok(v)` 和 `Err(e)` 构造；成功值不隐式包装。
- 操作是方法，名字与 Rust 相同：`unwrap`、`expect`、`unwrap_or`、`unwrap_or_else`、`map`、`map_err`、`and_then`、`or_else`、`ok`、`err`。
- 后缀 `?` 遇到 `Err(e)` 时当前函数返回 `Err(e)`（§8.2）。
- 丢弃 Result 是编译错误；确实要忽略时写 `.ok()`。

### 3.6 struct

- struct 具有值语义：赋值、传参和返回都复制。只存在于本语言内部。
- 构造时只能按名字传参，有默认值的字段可以省略（§2.4）。【后续】位置参数。
- 跨越引擎边界（信号参数、Variant、跨语言消息）时，自动编码为按字段名的 Dictionary，键为 String，使编码经过 JSON 往返后不变；解码时检查每个字段的存在和类型，并深入容器和嵌套的 struct，例如 `Dictionary[String, ConsumableState]`。字段的解码规则与 `T.try_from` 相同（§5.3）。
- 【已知限制】第一版 struct 不能 `@export`，也不能存入 `.tres`；需要在编辑器中编辑的数据使用 Resource。
- 方法：struct 可以声明 `func` 和 `static func`。方法中的 self 只读，给 self 的字段赋值是编译错误；需要修改时返回修改后的副本。理由：修改 self 的方法要求调用处是能写回的位置，在临时值、经 getter 读出的属性和循环变量上调用时，要么报错，要么悄悄改了副本。【后续】修改 self 的方法。
- 字段和方法在 struct 可见的地方都可见（§4.2）。【后续】更严的 `private`。
- 修改：`s.x = 1` 修改 s 所在位置的那一份（变量、成员或容器元素）。
  - 链式赋值沿链读出、修改、写回，与 GDScript 对 `node.position.x = 1` 的处理相同（`modules/gdscript/gdscript_compiler.cpp:1172-1190`）：`player.last_hit.damage = 3` 经 `last_hit` 的 setter 写回，`hits[0].damage = 3` 修改数组中的元素；
  - 给临时值的字段赋值是编译错误，例如 `get_hit().damage = 3`；
  - 循环变量和 lambda 捕获的是副本，与 GDScript 中的 Vector2 相同。修改 struct 类型循环变量的字段、之后又不读它时，给出“修改的是副本”的警告。
- 复制是浅复制：对象和容器字段复制引用，与这些值在别处赋值时一样。需要独立的容器时，对该字段调用 `duplicate()`。
- `==` 逐字段比较，各字段按自己类型的 `==`（§5.2）。哈希与 `==` 一致，struct 可以作 Dictionary 的键；键是副本，之后修改原值不影响键。直接或经嵌套间接含有 Array 或 Dictionary 字段的 struct 不能作键，否则是编译错误，因为复制是浅复制，修改共享的容器会改变键的哈希；Packed 数组是写时复制的值，对象按同一性哈希，都不受此限。GDScript 允许以 Array 作键，本语言照旧。
- struct 和带载荷的 enum 不经容器（Array、Dictionary）直接或间接包含自己，是编译错误，诊断列出成环的字段，因为这样的值没有有限的大小。经容器的递归可以，例如 §2.4 的 `Tree[T]`；需要链表一类的结构时用 RefCounted 类。【后续】类似 Swift `indirect` 的写法。

  ```gdscript
  struct Hit:
      var damage: int
      var crit: bool = false

      func total() -> int:
          return damage * 2 if crit else damage

  player.last_hit.damage = 3   # 读出、修改，经 last_hit 的 setter 写回
  get_hit().damage = 3         # 编译错误：修改的是临时值
  for h in hits:
      h.damage = 0             # 警告：修改的是副本
  ```

### 3.7 enum

- 无载荷的 enum 与 GDScript 相同。
- 带载荷的分支，载荷字段必须命名，例如 `MOVE(target: Vector3)`。命名使编码不随字段顺序改变。
- 对 enum 的 match 必须穷尽或带 `_`（§5.6）。新增分支时，编译器列出所有没有处理它的位置。
- 引擎看到的形式见 §10.1：无载荷的 enum 为 int，导出时显示为下拉框；带载荷的 enum 编码为按字段名的 Dictionary，保留键 `"$variant"` 记录分支名的字符串，例如 `Command.MOVE(target = p)` 编码为 `{"$variant": "MOVE", "target": p}`，`Command.IDLE` 编码为 `{"$variant": "IDLE"}`。`$variant` 不是合法的标识符，所以不会与字段名冲突；记录分支名而不是序号，使编码不随分支顺序改变。
- 带载荷的 enum 的 `==` 先比较分支，再逐字段比较；哈希与 `==` 一致。作 Dictionary 的键的限制与 struct 相同（§3.6）。
- 载荷字段只能经 match 取出（§5.6），不能写 `c.target`；所以载荷不能原地修改，只能构造新值。
- 【已知限制】第一版带载荷的 enum 不能有方法，因为花括号写法中没有放函数体的位置，相关函数写在所属的类中；也不能 `@export`，理由与 struct 相同（§3.6）。

### 3.8 容器

- 无论内部如何表示（见下条），用户看到的只有一种 `Array[T]` 和一种 `Dictionary[K, V]`，API 与 GDScript 相同。没有类型参数的 `Array`、`Dictionary` 的元素类型为 Variant，与 GDScript 相同。`Array[T]` 和 `Dictionary[K, V]` 不变，例如 `Array[Node2D]` 不能用在期望 `Array[Node]` 的位置，与 GDScript 的静态检查相同（`modules/gdscript/gdscript_analyzer.cpp:6379-6393`）。
- 元素可以由引擎表示时，容器就是引擎的带类型容器，与引擎共享。元素是本语言独有的类型（struct、带载荷的 enum 等）时，使用本语言自己的表示；交给引擎须显式转换，写作 GDScript 的构造转换 `Array(hits)`、`Dictionary(d)`，结果是引擎的无类型容器，元素按 §10.1 编码，是一份副本；反方向用 `Array[Hit].try_from(v)`（§5.3）。理由：隐式复制会造出“修改的其实是副本”一类的坑。
- 取值：
  - `pop_front()`、`pop_back()` 返回 `T?`：引擎在空数组上不报错而返回 null（`core/variant/array.cpp:781-800`）；
  - `front()`、`back()` 返回 T，空数组时是程序错误，引擎在这里同样报错（`array.cpp:354-362`）；
  - 越界的下标是程序错误；
  - T 本身可空时，`pop_front()`、`pop_back()` 返回 `T??`，由绑定层先检查数组是否为空，因为引擎在两种情况下都返回 null；
  - `Dictionary[K, V].get(k)` 返回 `V?`，键不存在时为 None，对应引擎在缺键时返回默认值（`core/variant/dictionary.cpp:178-187`）；`get(k, default)` 返回 V；`d[k]` 在键不存在时是程序错误，GDScript 在这里同样报错。
- 标准库在原生容器上提供接受带类型 lambda 的 `map`、`filter`、`sort` 等（§3.11）。

### 3.9 接口

- 名义接口：类必须用 `implements` 显式声明实现了哪些接口。
- 接口可以声明方法、属性和信号，可以有默认方法；默认方法通过接口属性读写状态。
- 属性要求可以由继承来的引擎属性满足，例如 `var global_position: Vector2` 由 Node2D 的同名属性满足；类型必须相同。属性要求可读写，标 `@readonly` 的只要求可读：可读写的实现满足只读的要求，反过来不行；本语言的 `var` 总是可读写的，所以没有 setter 的引擎属性只能满足 `@readonly` 的要求。默认方法给 `@readonly` 属性赋值是编译错误。`@readonly` 只用于接口属性，用在其他位置是编译错误。
- 实现接口的成员必须是 `pub`（§4.2）。
- 只有本语言的类能实现接口；外部对象须显式包装。
- 【已知限制】第一版没有带状态的 mixin。依据：[跨分支重复调查](surveys/cross-branch-duplication.zh.md)中，原生基类不同造成的重复在多数游戏仓库中每千行约 4–10 行；经阅读判断，游戏中这类重复的 353 行里有 185 行可以用组合或静态函数消除，适合带状态 mixin 的 102 行中 72 行来自 Ring 一个项目。继承本可消除却没有消除的重复更多，分别是它的 4.9 倍（同一原生基类的无关脚本之间）和 8.2 倍（同一继承链上），但主要来自 tanks-of-freedom-ii 一个仓库。
- 接口可以继承多个接口：
  - 实现子接口的类也实现了各个父接口，子接口的值可以用在期望父接口的位置；
  - 子接口可以用协变的返回类型重新声明父接口的方法；
  - 两个父接口有同名而签名不兼容的成员时，在子接口上报编译错误；
  - 两个父接口为同名方法都提供了默认实现时，子接口或实现类必须覆盖它；
  - 继承成环是编译错误。

  依据：gdUnit4 中 16 个只有抽象方法的类里，14 个继承同样只有抽象方法的 GdUnitAssert，并以更具体的返回类型重新声明它的方法，例如 `GdUnitAssert.gd:7` 的 `is_null() -> GdUnitAssert` 在 `GdUnitIntAssert.gd:7` 中写作 `is_null() -> GdUnitIntAssert`。
- 【已知限制】第一版接口不能是泛型。理由：泛型接口须规定型变（`Source[Enemy]` 是否是 `Source[Node]`）和 `Comparable[T]` 一类引用自身的约束，而调查的代码中没有发现需求；排序和比较用 lambda（§3.8）。【后续】泛型接口，写作 `interface Source[T]:`。

### 3.10 泛型

- 函数、struct、enum，以及不继承 Resource 的 RefCounted 类可以是泛型。Node、Resource 脚本和 autoload 不能，因为引擎实例化它们时不提供类型参数。泛型类不登记进引擎的全局类表，GDScript 看不到它，因为 GDScript 给不出类型实参。
- 约束用接口表达，第一版每个类型参数至多一个约束。类型实参在运行时保留。
- 写法见 §2.4。
- 泛型方法不经反射暴露（§10.5），因为引擎和 GDScript 按名字调用时给不出类型实参。作为 Callable 使用时须确定类型实参，例如 `sig.connect(handle[int])`。
- 【后续】多个约束，以及以类作约束；写法不变。

### 3.11 函数类型与 Callable

- 函数类型记录参数类型、返回类型以及是否为 async，写法见 §2.4。没有返回类型的函数类型返回 void。
- 调用照 GDScript 写 `f.call(x)`，按函数类型静态检查。直接写 `f(x)` 报 GDScript 现有的错误并给出改法（`modules/gdscript/gdscript_analyzer.cpp:3741-3745`）。
- 函数值能否用在某个位置只看类型，lambda 与方法值相同：
  - 参数逆变、返回协变，例如 `func(Node)` 可以用在期望 `func(Node3D)` 的位置；
  - async 与普通的函数类型之间没有隐式转换；
  - 由引擎调用的位置接受 async 函数：代码中和场景中的信号连接，以及引擎 API 的 Callable 参数（如 `tween_callback`）。任务作为根任务运行，与 async 的引擎虚函数相同（§9.1、§9.2）。依据：[语料计数](surveys/corpus-counts.zh.md)中作为值使用的协程方法有 91 处，其中 60 处是 `connect(名字)`。
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

- 动态值只在三处由外部强加：以 Variant 声明的引擎 API；GDScript 插件中没有标注类型的参数、返回值和成员（§11.1）；跨语言传来的消息。本语言代码中写出的 Variant，以及没有类型参数的 Array 和 Dictionary 的元素（§3.8），同样是动态值。
- 动态值进入静态代码时必须显式转换（§5.3）；不做隐式检查。
- 能定型的从源头定型：引擎实际有约束但没有导出的，在本语言的绑定元数据中补上（§10.3）。
- 静态值进入 Variant 是隐式的：赋值、传参和返回都可以。struct 和带载荷的 enum 按 §10.1 编码。例外：元素为本语言独有类型的容器须显式转换，写作 `Array(hits)`（§3.8）。
- Variant 进入静态代码只有显式的途径：
  - `v as T`，只用于对象，得到 `T?`（§5.3）；
  - `T(v)`，GDScript 的构造转换；
  - `T.try_from(v)`，严格转换，返回 Result；
  - `if v is T:`，分支中的局部变量和参数收窄为 T（§5.4）。
- `==` 和 `!=` 按引擎的 Variant 比较（§5.2）。
- 其余操作都是编译错误，快速修复插入转换或 `if v is T:`：运算符、成员访问 `v.x`、方法调用 `v.f()`、下标 `v[i]`、`for x in v`。GDScript 在这些位置动态执行。确实要按名字动态访问时，先转为 Object，再用引擎的 `get`、`set`、`call`（§10.4）。
- Variant 包含 NIL：`v == None` 对 NIL 和已释放的对象为真，与引擎的运算表相同（`core/variant/variant_op.h:484-489`），`v is int` 对 NIL 为假，`v as Node` 对 NIL 得到 None。`Variant?` 与 Variant 不同（§3.4），例如 `Dictionary[String, Variant]` 的 `get("hp")` 在缺键时为 None，在 `{"hp": null}` 中得到包含 NIL 的 Some。

  ```gdscript
  var data = JSON.parse_string(text)   # Variant
  data.hp                              # 编译错误：先转换
  if data is Dictionary:               # data 收窄为 Dictionary
      var hp = int.try_from(data.get("hp", 100)).unwrap_or(100)
  var n = data as Node                 # Node?
  ```

## 4. 声明

### 4.1 类与继承

- 类名使用引擎的扁平全局类表。编译器检查重名，包括与 GDScript 插件和 C# 的类名重复。【已知限制】第一版没有命名空间。
- 一个类可以继承引擎类或本语言的类。
- 【已知限制】跨语言继承两个方向都不支持：本语言的类不能继承 GDScript 类，GDScript 脚本也不能继承本语言的类（§11.2）。
- 覆盖方法时签名的规则见 §4.6，可见性的规则见 §4.2。
- `@abstract` 与 GDScript 相同（`modules/gdscript/gdscript_analyzer.cpp:1533-1552, 3630-3643`，`modules/gdscript/gdscript_parser.cpp:4500-4524`）：
  - 抽象方法不写冒号、没有函数体；
  - 含抽象方法的类必须标 `@abstract`；
  - 具体子类必须实现全部抽象方法；
  - 抽象类不能构造；
  - `super` 调用抽象方法，以及给 static 方法标 `@abstract`，都是编译错误。
- 场景中的节点使用抽象类是编译错误，标在该场景上。引擎本身也不给对象设置抽象脚本（`core/object/object.cpp:976`）；本语言经 `_is_abstract` 和全局类表把抽象性报告给引擎（`core/object/script_language_extension.h:132-136, 657-670`）。
- 带状态的共同基类用 `@abstract`；不同引擎基类的类共有的能力用接口（§3.9）。例如 godot-open-rpg 的 BattlerAction 继承 Resource，带导出字段和抽象的协程 `execute`：

  ```gdscript
  @abstract
  class_name BattlerAction extends Resource

  @export var label: String

  @abstract pub async func execute()
  ```

  依据：[语料计数](surveys/corpus-counts.zh.md)中 28 个抽象类有 25 个在 gdUnit4，以文件为单位的抽象类中 17 个的方法全部是抽象的，可以改写为接口。

### 4.2 可见性

- 不带 `pub` 的成员，包括 struct、enum、interface 和内部类，在声明它的文件内可见，文件中的内部类也能看到；子类也可以读写、调用和覆盖它们；其他类看不到。
- 标为 `pub` 的成员对所有代码可见。
- 覆盖时可见性不变：覆盖 `pub` 成员须写 `pub`，覆盖不带 `pub` 的成员不能加 `pub`。
- 实现接口的成员必须是 `pub`。
- `pub` 成员的签名中出现非 `pub` 的类型是编译错误。
- 不带 `pub` 的必须赋值成员（§6.2）只能由场景（导出时）、本文件或子类中的代码赋值；都没有时是编译错误，标在成员的声明处。
- 引擎、GDScript 和 C# 按名字能访问哪些成员，见 §10.5。
- 依据：[成员可见性调查](surveys/member-visibility.zh.md)中，名字可判定的成员约 25% 在其他文件中被访问。[第二次纸面迁移](surveys/paper-migration-2.zh.md)中，如果子类只能看到 `pub` 成员，102 处 `pub` 中有 29 处只是为了子类。
- 【后续】更严的 `private`。

### 4.3 成员变量

- `var` 声明成员。有初始化式时，类型可以由它推断，规则与局部变量相同：`var speed = 5.0` 为 float，`@onready var label = $Label` 按场景推断为 Label（§7.1）。推断出的类型出现在悬停、补全和引擎看到的属性信息中。
- 以下成员必须标注类型：没有初始化式的；初始化式推断不出类型的，例如空容器和 None。
- GDScript 中 `var hp = 10` 是无类型成员；在本语言中推断为 int，此后赋入其他类型是编译错误，符合语法硬规则。
- 没有初始化式的成员：
  - 数值、bool、String、StringName、NodePath、core 值类型、容器、`Callable`、`Signal` 和无载荷的 enum 取默认值，与 GDScript 相同；
  - `T?` 取 None，Variant 取 NIL，`weak[T]` 取不指向对象的弱引用；
  - struct 在所有字段都有默认值时取默认值，否则与对象引用一样必须赋值；
  - 带载荷的 enum 和函数类型必须赋值；
  - 必须赋值的成员的规则见 §6.2。
- `@export` 的规则与 GDScript 相同，另有：struct 和带载荷的 enum 不能导出（§3.6、§3.7）；导出的场景写作 `PackedScene[T]`（§7.4）；非空的对象类型的导出必须赋值（§7.5）。
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

- 函数签名的类型手写，不推断。参数必须标注类型，例外是覆盖（下一条）和有期望类型的 lambda（本节倒数第二条）。
- 覆盖引擎虚函数或基类方法时，可以省略参数类型和返回类型；省略的类型取自被覆盖的声明，悬停可见。写出的类型按 GDScript 的覆盖检查：返回类型可以协变，参数类型可以逆变，可以多出带默认值的参数（`modules/gdscript/gdscript_analyzer.cpp:1895-1936`）；参数名可以不同。例如 `func _process(delta):` 中 delta 为 float（`scene/main/node.h:433-434`）。这不是推断：类型由被覆盖的声明规定，与函数体无关。依据：[第二次纸面迁移](surveys/paper-migration-2.zh.md)中，引擎虚函数覆盖的参数标注有 6 处。
- 不是覆盖的函数，省略返回类型表示 void；在其中返回值是编译错误，快速修复补上返回类型。
- 参数可以有默认值，与 GDScript 相同。
- 剩余参数：
  - `...args: Array` 与 GDScript 相同，只能是最后一个参数，不能有默认值（`gdscript_parser.cpp:1709-1716`）；
  - 另外允许 `...args: Array[T]`，调用处逐个检查实参；GDScript 对带类型的剩余参数报编译错误（`gdscript_analyzer.cpp:1803-1810`）；
  - 只写 `...args` 是编译错误，快速修复补上 `: Array`；
  - 引擎看到的方法信息带 `METHOD_FLAG_VARARG`，GDScript 据此接受任意个额外的实参（`gdscript_analyzer.cpp:6052-6055`），元素类型由入口检查（§10.1）；
  - 调用处没有展开写法，GDScript 也没有（`gdscript_parser.cpp:4344`）；已有的数组用 `callv` 传入。没有展开写法时容易写错，例如 Pixelorama 的 `_debug_msg` 把自己的 args 整个当作一个实参传给 `_prefix_print`，后者再 `prints(args)`，打印出的是数组而不是以空格分隔的值（`src/Classes/ObjParse.gd:59-66`）。【后续】展开写法；
  - 依据：[语料计数](surveys/corpus-counts.zh.md)中 78 个剩余参数有 76 个在 gdUnit4 的断言接口中，元素都是 Variant。
- 调用按位置传参，与 GDScript 相同。按名字传参只用于 struct 和带载荷分支的构造（§2.4），以及 `instantiate(...)`、`new(...)` 传入必须赋值的成员（§6.2）。
- 函数内部的局部变量可以由初始化式推断类型。lambda 是函数内部的代码，按局部规则推断：有期望的函数类型时，参数类型取自它，没有时须写出；返回类型由函数体推断，例如 `weapons.map(func(w): return w.name)`。
- 协程在签名上声明为 `async func`，lambda 也是如此（§9.1）。

### 4.7 信号

- 信号的参数必须标注类型，例如 `pub signal died(cause: String)`。
- `emit` 和 `connect` 都做静态检查：发射时的实参、连接的函数的参数个数和类型。
- 场景中的信号连接由场景模型检查（§7.7）。
- 引擎类的信号按 ClassDB 的信息定型；未审核的对象参数按非空处理（§10.2）。
- await 信号的结果见 §9.6。

### 4.8 静态成员

- `static var`、`static func`、`_static_init()` 的写法和时机与 GDScript 相同：类加载时，先基类后派生类，按声明顺序初始化静态成员，最后调用 `_static_init`（`modules/gdscript/gdscript_compiler.cpp:2629-2632`）。静态成员的初始化式同样不经过 setter（`gdscript_compiler.cpp:2603-2626`）。
- 一个类在静态初始化中读取另一个类的静态成员时，先完成那个类的初始化。
- 静态初始化之间形成环是程序错误，诊断列出依赖链；能静态判断的是编译错误。不读取默认值或未完成的值。GDScript 的相关问题见 [godotengine/godot#104161](https://github.com/godotengine/godot/issues/104161)、[#82141](https://github.com/godotengine/godot/issues/82141)、[#104588](https://github.com/godotengine/godot/issues/104588)。
- 静态初始化不能 await。
- 修改静态成员的声明属于结构变化，热重载时要求重新运行（§12.4）。
- 理由：[语料计数](surveys/corpus-counts.zh.md)中 `static var` 只有 188 处、`_static_init` 4 个，接触频率低。

## 5. 表达式与语句

### 5.1 运算

- 运算的结果类型按引擎的运算符表确定，与 GDScript 的静态检查相同（`core/variant/variant_op.cpp:212-1033`）。例如 `Vector2 * float` 得到 Vector2，`Vector2i * float` 得到 Vector2，`int / int` 是整除。表中没有的组合，例如 `Vector2 + Vector2i`，是编译错误。
- `int` 与 `float` 混合运算时提升为 `float`。
- 整数除以 0 是程序错误。
- 以 Variant 绑定的工具函数（lerp、clamp、min、max、abs、wrap 等）的定型见 §10.3。

### 5.2 比较

- `==` 按引擎的运算表定型，与其他运算符相同（§5.1）；表中没有的组合是编译错误，例如 `hp == "3"`、`Vector2() == Vector2i()`。
- int 与 float 按数值比较，`1 == 1.0` 为真（`core/variant/variant_op.cpp:490-491`）。String 与 StringName 按内容比较（§10.7）。
- 对象按同一性比较，比较的是 ObjectID（`variant_op.cpp:514`），所以两个不同的已释放对象不相等。引擎比较前先把已释放的对象取为 null，两个已释放对象在 GDScript 中相等（`core/variant/variant_op.h:471-477`），属于 §1.2 的例外。两个类型都是类、且互不为子类时是编译错误，因为结果总为假；涉及接口时可以比较。
- Option：None 等于 None；两个 Some 在内部的值相等时相等；层数较少的一侧先包装到相同的层数再比较，例如 hp 为 `int?` 时的 `hp == 3`，以及 `T?` 与 `T??` 的比较。既不是 Option 也不是 Variant 的值与 None 比较是编译错误，因为结果总为假；快速修复把类型改为 `T?`，或在检查存活时改用 `is_instance_valid(x)`。
- 容器照引擎逐元素比较，Dictionary 按键逐项比较，与顺序无关。引擎比较元素时要求类型相同（`core/variant/array.cpp:155-158`，`core/variant/dictionary.cpp:255-285`，`core/variant/variant.cpp:3176-3179`），所以 `[1] == [1.0]` 为假，而 `1 == 1.0` 为真。元素类型不同的容器比较是编译错误，例如 `Array[int] == Array[float]`，因为只有两边都为空时才相等。
- 一侧是 Variant 时，运行时按引擎的运算表求值；组合不在表中是程序错误（`variant_op.cpp:1049-1054`）。GDScript 在这里报错；运算数的类型与这条指令第一次执行时不同的情况，只在 debug 构建中检查，release 构建得到 null 并继续执行（`modules/gdscript/gdscript_vm.cpp:793-798, 821-843`）。`v == None` 对 NIL 和已释放的对象为真（§3.13）。
- `!=` 在所有情况下都是 `==` 的否定。
- struct 和带载荷的 enum 逐字段比较（§3.6、§3.7）。

### 5.3 转换

- `x as T` 只用于对象类型，结果为 `T?`：类型不符时为 None，与 GDScript 相同（`modules/gdscript/gdscript_vm.cpp:1677-1678`）。x 的静态类型已经是 T 时，结果为 T，并提示这个 `as` 多余。已释放的对象 `as` 任何类型都得到 None（§3.3）。
- `as` 转为内置类型是编译错误。GDScript 在这里按构造函数转换，转换失败只在 debug 构建中报错（`gdscript_vm.cpp:1642-1650`），快速修复给出两种改法：
  - `T(x)`：保留这种转换，例如 `int("5")`、`int(2.7)`；
  - `T.try_from(x)`：严格的转换。
- `T.try_from(v)` 返回 `Result[T, ConversionError]`。规则与 struct 字段的解码相同：值的类型必须与 T 相符；转为 `int` 时，接受恰为整数且在 int64 范围内的浮点数（§10.8）。用 `?` 传播失败，或用 `.unwrap()`、`.expect(…)` 把失败变为程序错误。
- 必须成功的向下转型写作 `(x as T).unwrap()`。依据：[第二次纸面迁移](surveys/paper-migration-2.zh.md)中有 16 处。
- 先用 `is_in_group` 判断分组、再向下转型的写法，快速修复建议改用 `is` 收窄。

### 5.4 收窄

- 收窄与 Kotlin 的智能转换类似，但只用于局部变量和参数：它们在 `!= None`、`is` 和提前返回之后收窄；条件表达式 `a if c else b` 的两个分支，以及 `and`、`or` 的右侧，同样按条件收窄。给局部变量重新赋值后，收窄按新值重新计算。
- 成员不收窄，须先读入局部变量。依据：[第二次纸面迁移](surveys/paper-migration-2.zh.md)中，因此新增的局部变量每千行 4.4 个。【后续】条件绑定（例如 `if var t = _tween:`），或在检查与使用之间没有调用、await 和赋值时让没有 getter 的字段收窄。
- 没有真值判断：条件，以及 `not`、`!`、`and`、`or` 的操作数，必须是 bool。x 不是 bool 时，GDScript 的 `if x:`、`if not x:` 是编译错误，快速修复按 x 的类型改为 `x != None`、`x != 0`、`x != ""` 等，`not x` 相应改为 `x == None` 等。
- 非空不代表对象仍然存活；访问时照常检查（§3.3）。

### 5.5 条件表达式

- 只有 GDScript 的 `a if c else b`，c 必须是 bool。

### 5.6 match

- 保留 GDScript 的全部模式，含义不变（`modules/gdscript/gdscript_parser.cpp:2487-2711`）：字面量和常量表达式按相等比较；`var x` 绑定；`_` 通配；数组和字典模式，以及其中的 `..`；逗号分隔的多个模式，其中不能绑定；`when` 守卫。
- 新增的模式：
  - enum 分支：`类型.分支(字段 = 模式, ...)`。`var 字段` 是 `字段 = var 字段` 的简写。未列出的字段须由 `..` 覆盖，与字典模式相同。模式可以嵌套。
  - struct：同样的写法，例如 `Hit(damage = var d, ..)`。
  - 内置的构造：`Ok(var v)`、`Err(var e)`（Result），`Some(var x)`（Option），`Finished(var v)`、`Cancelled(var r)`（Settled，§9.2），不带类型前缀，按位置书写，与构造一致。
  - Option 另有 `None` 和 `var x`；后者绑定的值已收窄为 T。

  ```gdscript
  match cmd:
      Command.IDLE:
          stop()
      Command.MOVE(var target):
          walk_to(target)
      Command.ATTACK(target = var enemy, power = 0):
          taunt(enemy)
      Command.ATTACK(var target, ..) when target.is_in_group("bosses"):
          flee()
      Command.ATTACK(..):
          hit()
  ```

- 对 enum（无论有无载荷）、Result 和 Option 的 match 必须穷尽或带 `_`；带 `when` 的分支不计入覆盖。对其他类型不要求穷尽，与 GDScript 相同。
- 【已知限制】第一版没有类型模式（用 `if x is T`）、没有 match 表达式，也不能在逗号分隔的多个模式中绑定。【后续】这三项都可以兼容地加入。

### 5.7 循环

- `for` 与 GDScript 相同：`for i in n` 遍历 0 到 n-1，i 为 int；可以遍历 `range(...)`、容器、String 和 Dictionary 的键。
- 循环变量可以标注类型，如 `for i: int in ...`；标注的类型须是元素类型或其父类型，不做隐式的向下转换。
- 遍历 float 和向量是编译错误，快速修复改用 `range(...)`。GDScript 允许这样写，并把向量当作范围（`gdscript_vm.cpp:3092-3229`）；在静态类型下这几乎总是误用。
- `while`、`break`、`continue` 与 GDScript 相同。

## 6. 初始化与生命周期

### 6.1 成员的初始化

- 成员的初始化式直接写字段，不经过 setter，实例成员和静态成员都是如此，与 GDScript 相同（`modules/gdscript/gdscript_compiler.cpp:2414-2430, 2603-2626`）。初始化式给出第一个值，setter 处理值的变化；初始化时没有观察者，后声明的成员也尚未初始化。
- 派生状态在 `_ready` 或 `_static_init` 中计算。成员同时有初始化式和 setter 时，编译器提示初始化不经过 setter。
- 没有初始化式的成员取默认值或必须赋值（§4.3）。

### 6.2 必须赋值的成员

- Node 和 Resource 脚本的 `_init` 不能有参数，因为引擎总以零个参数构造它们（`core/object/script_language.h:152`；[暗坑清单](surveys/pitfalls.zh.md) G-02）。构造参数表达为必须赋值的成员：没有默认值的非空成员。
- 必须赋值的成员要在所在场景就绪完毕之前赋值，来源可以是：
  - 场景：导出的成员由场景提供（§7.5）；
  - 创建者：`instantiate(...)` 和 `new(...)` 接受按名字传入的成员值，在返回前完成赋值，例如 `BulletScene.instantiate(speed = 300.0, target = enemy)`。对 `PackedScene[T]`，编译器在调用处检查必须传入的成员（§7.4）都已传入，漏写是编译错误；`new(...)` 漏传不是编译错误，按下文的时机检查；
  - 祖先：同一场景中的祖先节点在自己的 `_ready` 中赋值，只要场景模型看得见这次赋值，即写成 `$Child.target = v`，或经 `@onready` 引用赋值。
- 检查时机：
  - 由场景或创建者提供的，在节点就绪时检查，早于 `@onready` 的初始化；
  - 由祖先赋值的，在整个场景就绪之后检查。就绪在节点入树时同步传播完毕（`scene/main/node.cpp:3368-3372`），每帧回调由主循环发起，所以正常逻辑读不到空值；
  - 子节点在自己的 `_ready` 中读取由祖先赋值的成员，是编译错误；
  - 就绪前读取尚未赋值的成员是程序错误，诊断写明成员名；
  - 场景模型看不见的配置方式（例如遍历 `get_children()` 赋值），成员须写作 `T?`。
- 运行时才确定路径的场景，只有就绪时的检查。
- Resource 没有就绪阶段：`.tres` 中的赋值静态检查；代码中 `new()` 出来的，在读取时检查。
- 不继承 Resource 的 RefCounted 类，照常使用带参数的 `_init`；GDScript 经 `new` 按位置传参（§10.1）。
- autoload 比场景活得久，引用场景节点的成员只能写作 `T?`。需要非空时，把管理器作为场景中的节点，用必须赋值的导出成员引用其他节点。
- 【已知限制】第一版没有“就绪后才赋值的非空成员”（如 Kotlin 的 `lateinit`）。依据：[语料计数](surveys/corpus-counts.zh.md)中，在 `_ready` 里把节点登记到 autoload 成员上的写法只有 6 处，其中 5 处在同一个项目（Ring，[第二次纸面迁移](surveys/paper-migration-2.zh.md)的素材之一）中。【后续】就绪后才赋值的非空成员。

### 6.3 `@onready`

- 沿用 GDScript 的时机：`@onready` 的初始化式在 `_ready` 之前执行，先基类后派生类，各自按声明顺序（`modules/gdscript/gdscript.cpp:1937-1953`）。
- 以下情况是编译错误：`@onready` 与 `@export` 同时使用；初始化式中使用 await；在 `_init` 或 `_enter_tree` 中直接读取 `@onready` 成员。
- 经调用间接读到尚未初始化的 `@onready` 成员是程序错误，诊断写明成员名。依据：[暗坑清单](surveys/pitfalls.zh.md) G-04。

### 6.4 就绪顺序

- 子节点先于父节点就绪，沿用引擎的顺序（`scene/main/node.cpp:326-337`），不新增生命周期钩子。
- 子节点在 `_ready` 中读到父节点尚未初始化的成员时，是程序错误，诊断说明原因并给出两种改法：由父节点在自己的 `_ready` 中配置子节点（§6.2）；或把子节点的 `_ready` 声明为 async，声明 `@parent(T)`，并 `await get_parent().ready`。
- 子节点在运行时加到已就绪的父节点下时，这样的读取是合法的，因此不在编译期一概报错。
- 节点重新入树时不再执行 `_ready`，与引擎一致（`node.cpp:334-336, 454`）。依据：[暗坑清单](surveys/pitfalls.zh.md) G-03、G-05。

### 6.5 就绪前的 setter

- 场景实例化时，引擎先设置节点保存在场景中的属性，再把节点加到父节点下（`scene/resources/packed_scene.cpp:494, 543`），这时节点的子节点和 `@onready` 成员都还不可用。
- setter 照常立即执行。执行到第一处需要节点就绪的语句时，若节点尚未就绪，就在此挂起，就绪后从这里继续：
  - 需要就绪的语句由静态分析确定：访问 `@onready` 成员、`$X` 或子节点，或者调用会访问它们的本类方法，包括子类的覆盖；
  - 只有在 `is_node_ready()` 为真时才能执行到的访问不算，判断规则与收窄相同（§5.4）：`if is_node_ready():` 分支内、`if not is_node_ready(): return` 之后、`is_node_ready() and …` 的右侧。方法中的这类访问全部不算时，调用它也不算需要就绪。只认 `is_node_ready()`，因为 `is_inside_tree()` 为真时 `@onready` 成员仍可能尚未初始化；
  - 挂起点的位置在编辑器中悬停可见。
- 同一属性在就绪前再次被写入时，之前挂起的调用立即取消，不论这次写入是否挂起。挂起的调用在 `@onready` 初始化之后、`_ready` 之前，按写入顺序继续。节点在就绪前被释放时，挂起的调用随之丢弃。就绪以后 setter 不再挂起。
- 挂起点之前的语句照常执行。setter 通常先变换并存储值，再同步节点，所以就绪前读到的是变换后的值。只推迟整个 setter、记下原始输入的做法，会让就绪前读到未经变换的输入，例如 Ring 的 `Runtime/AVGRuntime/UI/UI.cs:16-35` 给角色名加【】。
- 这就是 GDScript 中手写 `if not is_node_ready(): await ready` 的写法，由编译器自动完成，用户不需要新的概念；setter 中手写的这一行是编译错误（§4.4），快速修复把它删去。依据：[属性访问器调查](surveys/property-accessors.zh.md)中有 25 个 setter 用 `await ready` 等待就绪。
- 值只存在节点中的属性（没有字段、getter 读取子节点的），在就绪前读取是程序错误，诊断写明读取了哪个尚未就绪的节点。需要就绪前读取时，用字段保存。
- 从未被写入的属性，就绪时不执行 setter，与 GDScript 一致。导出属性的 setter 会同步节点、而默认值从不经过 setter 时，给出提示。

### 6.6 析构

- 不新增语法：继续用 `_notification(what)` 处理 `NOTIFICATION_PREDELETE`，与场景树相关的清理放在 `_exit_tree`，写法和时机与 GDScript 相同；析构顺序沿用引擎。理由：[语料计数](surveys/corpus-counts.zh.md)中 `NOTIFICATION_PREDELETE` 只出现 40 次，其中 32 次在 gdUnit4。
- 运行时保证 RefCounted 在 PREDELETE 期间可以读写自己的字段、调用自己的方法，不重新给 self 加引用。引擎在 PREDELETE 时引用计数已经归零，再加引用会失败（`core/object/ref_counted.cpp:94-95`，`core/templates/safe_refcount.h:136-146`，`core/variant/variant.cpp:1104-1108, 2509-2510`）。
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

- 编译器收集脚本所在的全部场景结构，包括继承场景和实例上的修改，为 `$X`、`%X` 和字面量路径的 `get_node` 定类型。
- 不同的场景结构给出不同类型时是编译错误，须把它赋给标注了共同基类或接口的变量，例如 `@onready var weapon: Node3D = $Weapon`。依据：[场景结构调查](surveys/scene-structures.zh.md)的游戏仓库中，2,023 处字面量路径没有一处在不同结构中解析为不同类型。
- 找不到的路径是编译错误，标在缺少该节点的场景上；哪些场景需要有这个节点，见 §7.2。

### 7.2 场景之间结构不同的情况

- 以下三种情况都用现有的写法，不需要新的声明。
- **基类要求、由继承场景提供的子节点**：按场景的使用方式检查，规则与必须赋值的导出相同（§7.5）。
  - 只被当作继承基础的场景可以缺这个节点；作为根使用或被实例化的场景必须有；没有被任何地方引用的场景缺节点时只给警告。
  - `$X` 的类型取自有这个节点的场景。
  - 例如 tanks-of-freedom-ii 的 `base_ground.tscn` 缺少 `tile.gd` 用到的 `$"mesh"`，它被 preload 作为占位地块，属于作为根使用，错误标在这个场景上。
  - 依据：[场景结构调查](surveys/scene-structures.zh.md)中，游戏代码在部分结构中缺失的路径有 7 处是模板或占位场景缺少的节点。
- **可选的子节点**：字面量路径的 `get_node_or_null(...)` 就是声明，类型为 `T?`，缺这个节点的场景不算错误。

  ```gdscript
  @onready var trail_right = get_node_or_null("Container/TrailRight")   # GPUParticles3D?
  ```

  `$X` 在某个被使用的场景中缺失时，快速修复把访问改为 `get_node_or_null`。依据：[场景结构调查](surveys/scene-structures.zh.md)中，游戏代码在部分结构中缺失的路径有 8 处用 `get_node_or_null`，例如 Starter-Kit-Racing 中两种车的尾迹。
- **运行时添加的子节点**：不按路径定型。
  - 任何场景中都没有的 `$X` 是编译错误，即使这些场景没有被引用；快速修复建议在 `add_child` 时把节点存入有类型的成员，例如 `var tetromino: Tetromino?`。这条规则也能发现改名后没有更新的路径，例如 tps-demo 的 `door.gd:6` 用 `$DoorModel/AnimationPlayer`，而 `door.tscn` 中这个节点叫 `DoorModel2`。
  - 确实要按名字查找时，用 `get_node_or_null("X")`，得到 `Node?`，再用 `as` 转换（§5.3）。
  - `get_node_or_null` 的字面量路径在任何场景中都不存在时给出警告；确实是运行时添加的节点，用 `@warning_ignore` 关闭。

### 7.3 向上访问

- autoload 用其全局名访问，类型为其脚本的类（§11.1）。以 `/root/X` 形式的字面量路径访问 autoload 是编译错误，快速修复改为全局名 X。
- 父节点和 owner 的要求写作类级注解 `@parent(T)`、`@owner(T)`，与 `@tool` 放在同一位置。声明后，`get_parent()`、`$".."` 和 `owner` 具有类型 T。未声明时，`get_parent()` 和 `owner` 的类型为 `Node?`，与引擎一致：没有父节点或 owner 是节点的正常状态，引擎这时返回 null 而不报错（`scene/main/node.cpp:2100-2102, 2296-2298`）；`$".."` 和 `get_node("..")` 的类型为 Node，因为 GDScript 在没有父节点时同样报错。未声明时写 `get_parent().x` 或 `owner.x` 是编译错误，快速修复按场景模型推断出父节点或 owner 的公共类型，在类上补 `@parent(T)` 或 `@owner(T)`，也可以在调用处改为 `.unwrap()`。依据：[语料计数](surveys/corpus-counts.zh.md)中，自身的 `get_parent()` 有 174 处，其中 62 处直接访问成员，分布在 46 个文件中；`owner` 直接访问成员 49 处，分布在 21 个文件中。另有 3 处处理了没有父节点的情况，其中 beehave 的 setter 在节点挂到父节点之前执行，`get_parent()` 在那里得到 null。

  ```gdscript
  @owner(Combat.Combatant)
  class_name HealthComponent extends Node

  func _ready():
      current_health = owner.init_health
  ```

- 静态检查在场景中放置该脚本的位置进行。父节点要求检查节点在场景中的父节点，以及场景的根被实例化到另一个场景中时外层的父节点，不符合时是编译错误，标在该场景上；场景作为主场景时，父节点是 Window 类型的根视口，同样检查；其他作为根使用的场景，父节点要到运行时才知道，只在运行时检查。owner 是包含该节点的场景的根节点；场景的根被实例化到另一个场景中时，owner 是外层场景的根（`scene/resources/packed_scene.cpp:567-570`）。脚本挂在作为根使用（§7.5）的场景的根上时没有 owner，这样的场景使用声明了 `@owner` 的脚本是编译错误，标在该场景上。
- 运行时：
  - 父节点要求在节点挂到父节点时检查（`NOTIFICATION_PARENTED`，`scene/main/node.h:465`）；声明了 `@parent(T)` 的节点在没有父节点时调用 `get_parent()`，是程序错误；
  - 声明了 `@owner(T)` 时，`owner` 在每次读取时检查不为 null 且符合要求；失败是程序错误，诊断写明要求和实际的 owner。不在移动节点时检查 owner：节点被移出 owner 的子树时，引擎把 owner 置为 null（`scene/main/node.cpp:391-396, 1781-1783`），这是合法的操作，例如 Ring 的 DeathComponent 先读 owner，再把自己移到前景层；owner 被释放时同样置为 null（`node.cpp:299-302`）。
- `get_owner()` 和 `get_node_or_null("..")` 的类型总是 `Node?`，声明了要求的代码需要判断有没有 owner 或父节点时使用它们。

### 7.4 场景资源

- `preload` 和字面量路径的 `load` 按资源的实际内容定型；场景得到 `PackedScene[T]`，T 为场景根节点的类型，根节点有脚本时为脚本的类。
- 导出的场景写作 `PackedScene[T]`，由编辑器检查所填的场景。`instantiate()` 的结果为 T。
- `PackedScene[T]` 对 T 协变，例如 `PackedScene[Boss]` 可以用在期望 `PackedScene[Enemy]` 的位置。
- `instantiate(...)` 须传入哪些成员，取决于是否静态知道具体的场景：
  - 静态知道时，即 `preload`、字面量路径的 `load`，以及以它们初始化的常量，须传入该场景没有提供的必须赋值成员；
  - 其余的 `PackedScene[T]`，即导出、变量、参数和返回值，须传入 T 中不导出的必须赋值成员；导出的必须赋值成员由场景自己提供；
  - 场景转为后一种 `PackedScene[T]` 时，即填入导出、赋给变量或传参时，场景根的实际类中没有由场景提供的必须赋值成员只能是 T 中不导出的必须赋值成员，否则是编译错误，标在填入导出的场景上，或报在赋值处。例如 `Boss extends Enemy` 新增了不导出的必须赋值成员 `arena`，boss.tscn 填入 `PackedScene[Enemy]` 时报错，因为调用处只知道 Enemy。
- 运行时才确定路径的 `load` 按引擎的签名得到 Resource，可空性按 §10.2 的审核；这样加载的场景，`instantiate()` 得到 Node。转为具体类型须显式转换（§5.3）。

### 7.5 必须赋值的导出

- 非空的对象类型的导出必须赋值。确实可能缺值的写作 `T?`。
- 按场景的使用方式检查：
  - 场景被其他场景实例化或继承时，沿实例和继承链在任何一层赋值都算数；每个实例分别检查，缺值标在该实例所在的场景上。例如 player.tscn 的 `crosshair` 指向只存在于 main.tscn 的节点，由 main.tscn 在实例上赋值，player.tscn 本身不报错；
  - 场景作为根使用时，即主场景、以字面量路径 `load` 或 `preload` 的场景、被填入导出的 `PackedScene[T]` 属性的场景，检查场景本身；场景没有提供的值，按 §7.4 由代码中 `instantiate(...)` 的调用处传入也算数，漏传是调用处的编译错误；
  - 没有被任何地方引用的场景只给警告，因为运行时才确定路径的 `load` 看不到。
- 检查分三个时机：
  - 编辑时允许清空。Inspector 中的字段和场景树中的节点立即标出编译错误，但不阻止任何操作，重新赋值后标记消失；
  - 运行时允许运行：这类标在场景上的编译错误不阻止在编辑器中运行；缺值的节点就绪时报程序错误，检查早于 `@onready` 的初始化，诊断写明成员名，例如在编辑器中单独运行 player.tscn 时；
  - 导出时，确定会缺值的场景经导出插件报告为错误，导出窗口显示失败，消息写明场景和成员名；只有警告的照常导出。CI 用无界面的分析命令阻断（§13.1）；
  - 【已知限制】引擎的导出器不因插件报告的错误中止：导出插件的钩子都不能使导出失败（`editor/export/editor_export_platform.cpp:2568-2576`，`editor/export/editor_export_plugin.h:120-122`），报告的错误只让导出窗口显示失败（`editor_export_platform.cpp:131, 146-148`），产物照样写出，命令行导出的退出码仍为 0（`editor/editor_node.cpp:1408-1424`）。
- 远程检查器向非空成员写入 null 时，入口检查失败，即程序错误（§10.1）：写入被拒绝，原值不变。
- 理由：编辑过程中清空再替换是正常操作，非空约束的是编辑完成的场景。替换也不需要先清空，因为节点引用在 `.tscn` 中以路径保存，实例化时才解析（`scene/resources/packed_scene.cpp:376-395`）。

### 7.6 资源共享

- 资源共享的语义不变，由场景模型发现问题。编译器知道哪些资源是共享的：`load`、`preload` 和导出属性得到的资源来自缓存（`core/io/resource_loader.cpp:800-810`）；场景中没有设为“局部于场景”的内嵌子资源，由该场景的所有实例共用（`scene/resources/packed_scene.cpp:705-706`）。
- 对已知共享的资源写属性，或调用会修改它的方法时，给出警告，列出共享它的位置，并提供两个快速修复：在场景中把它设为局部于场景，或在修改前 `duplicate()`。有意修改共享资源时用 `@warning_ignore` 关闭。运行时才替换的资源追踪不到，不警告。例如 godot-open-rpg 的 `interaction_popup.tscn` 内嵌的 CircleShape2D 没有设为局部于场景，`interaction_popup.gd:30` 的 setter 修改它的半径，而 `main.tscn:484-486` 中只有一个实例把 radius 设为 64；按引擎的共享规则推断，十几个实例共用同一个形状，半径取决于最后执行的 setter（未运行验证）。
- `duplicate()` 返回接收者的静态类型；复制深度用 `duplicate_deep(模式)` 写明。
- `preload`、导出的 PackedScene 和资源之间的引用环是编译错误，快速修复把其中一条边改为运行时 `load`；引擎在这里返回 null（`core/io/resource_loader.cpp:1049-1056`）。依据：[暗坑清单](surveys/pitfalls.zh.md) G-19、G-20、G-34。

### 7.7 场景中的连接和路径

- 场景中保存的信号连接，由场景模型检查方法是否存在、参数是否与信号相符。
- 动画轨道中的属性路径、方法轨道中的方法名，以及 `"position:x"` 一类属性路径，按静态引用解析（§10.4）。
- 这些引用参与重命名和查找引用（§13.3）。

## 8. 错误

### 8.1 程序错误

- 程序错误中止本语言的整条调用链，退回到引擎调用进来的入口。退出时释放持有的引用，记录错误和调用栈；在编辑器中运行时进入调试器。游戏继续运行。
- 程序错误的检测和处理在 debug 和 release 构建中一致；唯一的例外是 `assert`（§8.3）。debug 构建只补充诊断信息，并在退出时报告泄漏（§6.7、§12.5）。
- 若引擎调用的入口需要返回值，引擎得到该类型的默认值。
- 本文中称为“程序错误”的情况都按这一条处理，包括：访问已释放的对象（§3.3）、`unwrap` 和 `expect` 失败、越界下标、整数除以 0、就绪前读取尚未赋值的成员、跨引擎边界的入口检查失败（§10.1）、等待被取消的任务（§9.2）。
- 同一线程上本语言的调用嵌套超过 1024 层是程序错误，而不是让进程因栈溢出崩溃；1024 是 GDScript 的默认上限（`modules/gdscript/gdscript.cpp:2870`，`modules/gdscript/gdscript.h:489-490`）。
- 理由：GDScript 出错后，debug 构建按返回类型构造默认值返回（`modules/gdscript/gdscript_vm.cpp:3963-3993`），release 构建返回 null（`:535, 3995`），调用方都继续执行；在静态类型下，这会让代码拿到假值。

### 8.2 预期之内的失败

- 预期之内的失败用 Option（§3.4）或 Result（§3.5）表达。采用 Rust 的模型：Option 是表示可能没有值的普通值，可以不处理；Result 表示失败，不能丢弃（§3.5）。
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

- `assert(条件, 消息)` 与 GDScript 相同，只在 debug 构建（在编辑器中运行和 debug 导出）中求值，失败时是程序错误；release 构建中去掉，条件和消息都不求值（`modules/gdscript/gdscript_compiler.cpp:2181-2207`；GDScript 的失败处理见 `modules/gdscript/gdscript_vm.cpp:3876-3902`）。
- 条件或消息中出现 await 或后缀 `?` 是编译错误，因为它们会随断言在 release 构建中消失。
- 条件必须是 bool（§5.4），例如 `assert(save_data, "...")` 是编译错误，快速修复改为 `save_data != None`。
- release 中也要检查的，用 `expect`（§3.4）或显式的 `if`。【后续】始终生效的断言。
- 理由：GDScript 在 release 构建中去掉 assert，语法硬规则要求一致；[语料计数](surveys/corpus-counts.zh.md)中 595 个 assert 的条件基本都是查询，例如 `peers.has(peer_id)`，实参中含 await 的为 0 个。

### 8.4 诊断

- 编译错误、警告和提示由分析服务报告，标在源码、Inspector 和场景树中（§13）。
- 程序错误的诊断写明出错的位置、相关的成员或对象，以及调用栈。本文对个别程序错误规定了诊断必须包含的内容，例如必须赋值的成员名、已释放对象的释放位置、owner 要求与实际的 owner。

## 9. 协程

### 9.1 async 与 Task

- 协程在签名上声明，写作 `async func`。函数体中有 await 而没有声明，是编译错误。引擎虚函数可以声明为 async。
- lambda 同样如此：含 await 的 lambda 写作 `async func(...)`，例如 `button.pressed.connect(async func(): await fade_out())`。GDScript 中这样的 lambda 不加标记（`modules/gdscript/gdscript_parser.cpp:3249-3251, 3752-3753`），快速修复补上 `async`。理由：判断函数是否为 async 只有一条规则，挂起点旁边一定看得到 async；[语料计数](surveys/corpus-counts.zh.md)中 979 个 lambda 只有 35 个含 await，其中 33 个在 gdUnit4 的测试中。
- 由引擎调用的位置接受 async 函数，任务作为根任务运行（§3.11）。
- 调用 async 函数得到 `Task[T]`。任务立即开始，运行到第一次挂起为止；`await f()` 和 `await t` 得到 T。返回 void 的协程得到 `Task[void]`；`void` 作类型实参只能用于 Task，没有类型实参的 `Task` 是编译错误。
- 丢弃 async 调用的结果是编译错误；确实不等待时写 `f().detach()`。GDScript 中把协程调用的结果当值使用本来就是编译错误（`modules/gdscript/gdscript_analyzer.cpp:3776-3784`），所以 `var t = f()` 获得新含义不违反语法硬规则。
- setter、getter、`@onready` 初始化式和静态初始化不能 await。

### 9.2 结束状态

- 任务的结束状态有三种：
  - **完成**：得到返回值；
  - **取消**：由 `t.cancel()`、所属对象被释放，或 `Task.of(信号)` 的信号源被释放引起（§9.7）；
  - **失败**：任务内部发生程序错误，包括 await 到被取消的任务或已释放的信号源（§9.7）。
- 等待被取消的任务是程序错误。失败沿 await 链向上传播到根任务，即 detach 的任务或被引擎调用的任务，不能拦截。
- `await t.settled()` 等待任务完成或被取消，把取消当作值而不是错误，返回 `Settled[T]`：`Finished(v)` 或 `Cancelled(r)`，r 的类型是 enum `CancelReason`，分支为 `REQUESTED`、`OWNER_FREED` 和 `AWAITED_FREED`。任务失败时，失败照常传给等待方，与 `await t` 相同。不用 Result 表达，因为被取消不是错误。

### 9.3 取消与归属

- 对挂起中的任务，`cancel()` 立即断开它等待的信号或任务，任务结束为取消。
- 对正在执行的任务（只在它自己的同步代码间接取消自己时出现），执行到下一个 await 或返回时结束为取消，返回值丢弃。
- 对已结束的任务，`cancel()` 没有效果。调用 `cancel()` 之后，结果一定是取消。
- 取消沿归属传播：任务以 `await f(...)` 亲自启动并直接等待的子任务归它所有，随它一起取消；`await` 一个从别处取得的任务值时，取消只是停止等待，那个任务照常运行。两种写法在编译期即可区分。例如过场 `cutscene` 停在 `await walk_to(door)` 时被取消，`walk_to` 随之取消，角色停下。

### 9.4 释放

- 协程帧，即局部变量和挂起点，在任务结束时立即释放；持有的引用按作用域退出处理，await 之后的代码不再执行。【已知限制】第一版没有 finally 或 defer。
- Task 对象是只保存结果的 RefCounted 句柄。变量、等待方，以及等待信号期间的运行时都不再引用它时，它被释放。
- 挂起的协程不使 self 保持存活，与 GDScript 相同（`modules/gdscript/gdscript_vm.cpp:2609-2625`）。

### 9.5 组合与重入

- `Task.of(信号)` 把信号包装为任务；`Task.delay(秒)` 计时；`Task.any(列表)` 返回第一个完成的任务的下标，并按归属规则（§9.3）取消其余任务；`Task.all(列表)` 等待全部完成。在 `Task.any(...)`、`Task.all(...)` 的实参中直接启动的任务归它们所有；从别处取得的任务，只是停止等待。
- 重入不做自动策略，需要时用句柄取消旧的调用。【后续】`@restart` 一类的注解。依据：[暗坑清单](surveys/pitfalls.zh.md) G-12、G-13；提案 [godot-proposals#8838](https://github.com/godotengine/godot-proposals/issues/8838)、[#6243](https://github.com/godotengine/godot-proposals/issues/6243)。

### 9.6 await 信号

- 可以直接 await 一个信号。
- 无参数的信号不产生值；单参数的信号得到该参数类型的值；多参数信号的 await 只能作为语句。依据：[await 信号结果调查](surveys/await-signal-results.zh.md)中，1,356 个 await 只有 1 个使用了多参数信号的结果。GDScript 在这里得到由实参组成的 Array（`modules/gdscript/gdscript_function.cpp:256-274`），所以使用这种结果的 GDScript 写法是编译错误。【后续】按参数解构多参数信号的 await 结果。
- 信号表达式的静态类型只是 `Signal` 时，签名未知，例如 `Signal` 类型的变量或参数、返回 Signal 的方法、Variant 或 Object 上取到的信号。经已知的类访问的信号，签名都已知：引擎类按 ClassDB，本语言的类按声明，GDScript 类经反射。
- await 签名未知的信号，结果是 Variant，运行时的形状与 GDScript 相同：无参数时为 NIL，一个参数时为该实参，多个参数时为由实参组成的 Array。用作语句时没有限制。理由：签名未知本来就是动态来源，结果按 Variant 的规则显式转换后才能使用（§3.13）。依据：[await 信号结果调查](surveys/await-signal-results.zh.md)中，这类 await 的结果只被使用过 1 次，即 `godot_dialogue_manager/addons/dialogue_manager/dialogue_manager.gd:1956` 经类型为 RefCounted 的接收者等待一个在 C# 中声明的两参数信号。【后续】带参数类型的 Signal 类型。

  ```gdscript
  var r = await DialogueManager.Resolved   # 接收者是 RefCounted，签名未知：r 为 Variant
  if r is Array and r[0] == id:            # 按 Variant 的规则收窄后使用
      return r[1]
  ```

### 9.7 对象释放与挂起的协程

- 协程所属的对象被释放时，协程静默取消，挂起帧持有的引用随之释放，与 GDScript 相同（`modules/gdscript/gdscript.cpp:2069-2079`）。运行时在对象收到 PREDELETE 时就取消它的协程。PREDELETE 先通知脚本实例，再由 Node 删除子节点（`core/object/object.cpp:173, 928-931`；`scene/main/node.cpp:308-312`），所以等待自己子节点的协程在子节点被删除之前已经取消。
- 被等待的对象（另一个对象上的协程，或另一个对象的信号源）在完成之前被释放时，等待方不会像在 GDScript 中那样永远挂起（`modules/gdscript/gdscript.cpp:2069-2079`，`core/object/object.cpp:2327-2342`）。判断推迟到本帧待删除的对象都删除之后，即场景树处理完删除队列、引擎调用脚本语言的每帧回调时（`scene/main/scene_tree.cpp:735`，`main/main.cpp:5106-5109`）：
  - 等待方的所属对象在这期间也被释放时，等待方静默取消（`OWNER_FREED`）。例如切换场景时，引擎从后往前逐个删除兄弟节点（`node.cpp:308-312`），等待兄弟节点信号的协程不报错；
  - 否则等待方在 await 处失败，即程序错误，诊断写明被释放的对象及释放位置，并照常沿 await 链传播（§9.2）。
- 用 `settled()` 等待时不报错，得到被等待者自己的取消原因：`Task.of(信号)` 在信号源被释放时得到 `Cancelled(CancelReason.AWAITED_FREED)`，另一个对象上的协程在其所属对象被释放时得到 `Cancelled(CancelReason.OWNER_FREED)`。

  ```gdscript
  # hud.pet：HUD 和 player 都是 Main 的子节点
  async func _ready():
      await player.died         # 切换场景：player 先被删除，HUD 随后被删除，静默取消
      show_game_over()          # player 被单独释放却没有发出 died：本帧结束后报程序错误
  ```
- 任何对象的释放都可以通过 GDExtension 公开的实例绑定释放回调感知（`core/object/object.cpp:2360-2365`）。运行时使用自己的绑定标识，一个对象可以同时持有多个标识不同的绑定（`core/object/object.cpp:2167-2201`），所以不影响 godot-cpp 的绑定；这已在运行的引擎上验证（[探针](../tests/engine/project/probes/probes.gd)）。依据：[暗坑清单](surveys/pitfalls.zh.md) G-11。

### 9.8 跨语言 await

- 对 GDScript 方法的调用可以写 await。运行时得到 `GDScriptFunctionState` 时，等待它的 `completed` 信号（`modules/gdscript/gdscript_function.cpp:369`），再按声明的返回类型检查结果。协程性不在方法的反射信息中（`core/object/method_info.h:37-44`），因此无法在编译期判断。没有写 await 时，按调用的写法区分，与 GDScript 相同（`modules/gdscript/gdscript_analyzer.cpp:3776-3784`）：
  - 作为语句调用、结果没有被使用时，运行时发现方法是协程也不报错：协程运行到第一次挂起，之后自行继续，相当于 detach；
  - 结果被当作值使用时，运行时得到 `GDScriptFunctionState` 是程序错误，诊断写明该方法是协程、须写 await。
  - 【后续】为 GDScript 协程调用取得 `Task` 句柄，以便稍后等待。
- 等待 `GDScriptFunctionState` 时，运行时持有它直到 `completed` 发出，同时监听发起调用的对象，即接收者，或经 Callable 调用时的目标对象。这个对象被释放，或它的脚本被替换（`script_changed`，`core/object/object.cpp:995`）时，按被等待的对象被释放处理（§9.7），`settled()` 得到 `Cancelled(CancelReason.OWNER_FREED)`。理由：协程再次挂起后，内层状态只以 ObjectID 记住第一个状态的 `completed` 信号（`modules/gdscript/gdscript_vm.cpp:2634-2639`），第一个状态在恢复执行时就离开了实例的待处理列表（`modules/gdscript/gdscript_function.cpp:329-330`），不持有它就会被提前释放；实例析构时，引擎只清除挂起的状态，不发出 `completed`（`modules/gdscript/gdscript.cpp:2066-2084`，`gdscript_function.cpp:343-362`）；`GDScriptFunctionState` 是内部类，不暴露它所属的实例，它的 `is_valid(true)` 在恢复执行之后总是为假（`gdscript_function.cpp:288-307, 337, 364-370`，`modules/gdscript/register_types.cpp:140`），所以协程再次挂起后，无法从持有的状态得知宿主是否已释放。
- 【已知限制】GDScript 脚本被重载时，引擎清除它挂起的函数而不发出任何信号（`gdscript.cpp:1487-1506`，`modules/gdscript/gdscript_compiler.cpp:2700`），等待这些函数的协程会永远挂起。这只在编辑器中修改脚本时发生。
- GDScript await 本语言的协程时，协程在第一次挂起时返回其 Task 的 `completed` 信号；GDScript 的 await 接受任意 Signal 值（`modules/gdscript/gdscript_vm.cpp:2586-2600`）。Task 完成时发出 `completed(值)`；取消或失败时发出 `completed(null)`，使 GDScript 的等待方不会永远挂起；另有 `cancelled` 信号供需要区分的代码使用。
- 本语言的 async 方法交给引擎的返回类型一律是无类型的 Variant（`PROPERTY_USAGE_NIL_IS_VARIANT`），与声明的返回类型无关，返回 void 时也是如此；声明的类型只出现在文档和悬停中。理由：方法的反射信息中没有协程性（`core/object/method_info.h:37-44`），GDScript 只能按报告的返回类型检查。报告为 T 时，GDScript 中每处 await 都有“多余的 await”警告（`modules/gdscript/gdscript_analyzer.cpp:3102-3107`）；取消时的 `completed(null)` 不经检查写入类型为 T 的临时变量（`modules/gdscript/gdscript_vm.cpp:2667-2668`，`modules/gdscript/gdscript_byte_codegen.cpp:998-1001`），T 为 Transform3D 一类的值类型时，之后按类型访问它的指令解引用空指针，进程崩溃。报告为 Variant 时，debug 构建在把结果赋给有类型的变量时检查类型（`gdscript_vm.cpp:1433-1444`）；release 构建不检查，与 GDScript 中其他返回 Variant 的函数相同。
- 两个方向都已在运行的引擎上验证（[探针](../tests/engine/project/probes/probes.gd)）：持有第一个状态时，协程再次挂起后仍得到结果，不持有时结果不会到达；接收者被释放时协程不再完成；GDScript 等待本语言的协程得到结果，取消时得到 null，把它赋给 Transform3D 变量只中止该函数而不崩溃。

## 10. 引擎边界

### 10.1 引擎和 GDScript 看到的类型

| 本语言 | 引擎和 GDScript 看到的 |
|---|---|
| `int`、`float`、`String`、Node 等类 | 同名类型 |
| 对象 `T` 和 `T?` | 都是该类；非空由入口检查保证 |
| `int?` 等可空值 | 无类型的 Variant |
| 元素可由引擎表示的 `Array[T]`、`Dictionary[K, V]` | 引擎的带类型容器 |
| 无载荷的 enum | int；导出时显示为下拉框 |
| struct | 按字段名编码的 Dictionary，键为 String |
| 带载荷的 enum | 按字段名编码的 Dictionary，保留键 `"$variant"` 记录分支名（§3.7） |
| async 方法的返回值 | 无类型的 Variant（§9.8） |

- GDScript 通过 Script 的反射接口为其他语言的全局类定型（`modules/gdscript/gdscript_analyzer.cpp:789-804, 4278-4319, 6005-6014`），所以 GDScript 对本语言类的实例成员（属性、方法、信号）做与原生类相同的静态检查：已知成员的类型和实参受检查，未知的方法名与在原生类上一样只是不安全访问的警告（`gdscript_analyzer.cpp:3750-3757`）。`Player.new(...)` 同样受检查：Script 基类没有绑定 `new`，GDScript 是自己绑定的（`modules/gdscript/gdscript.cpp:1078`），所以本语言的 Script 类绑定 `new`，并把 `_init` 报告为返回该类的静态方法，GDScript 把 `new` 当作静态的 `_init` 检查（`gdscript_analyzer.cpp:5958-5961`）。实参按位置传入；按名字传入必须赋值的成员（§6.2）只在本语言中可用。这些已在运行的引擎上验证（[探针](../tests/engine/project/probes/probes.gd)）。其余的类级访问见 §11.3。
- **入口检查**：值从引擎、GDScript 或 C# 进入本语言的位置（参数、`set` 写入成员、信号的实参、await 的结果、解码），按声明的类型检查，失败是程序错误。远程检查器的写入也经过入口检查（§7.5）。
- Option 交给引擎时 None 编码为 NIL。所以只有 X 的值不会编码为 NIL 时，`X?` 才能隐式交给引擎；X 是 Option 或 Variant 时是编译错误，快速修复给出 `.flatten()` 或 `.unwrap_or(null)`。否则 Some(None) 经过引擎往返后会悄悄变成 None（§3.4）。

### 10.2 引擎 API 中对象的可空性

- 绑定元数据（§10.3）尚未审核的引擎 API，按值的流向确定对象类型的可空性：
  - 引擎交给本语言的值可能为空：方法的返回值和读出的属性，类型为 `T?`；
  - 引擎调用本语言时传入的值按非空处理：被覆盖的虚函数的参数和引擎信号的参数，类型为 T，为空时由入口检查（§10.1）报告程序错误；处理函数可以把参数写成 `T?` 来接受空值，不必等审核（§4.6）；
  - 本语言传给引擎的值可以为空：方法的参数和写入的属性接受 None，空值是否合法由引擎判断，例如 `set_owner(null)` 清除 owner（`scene/main/node.cpp:2273-2281`）。
- 审核从调用最频繁的 API 开始。返回值的结果分三种：
  - 非空，类型为 T；
  - 只在误用时为空，即引擎在返回 null 的同时报错，例如 `get_tree()`（`scene/main/node.h:558-561`）：按非空处理，为空时是程序错误；
  - 正常情况下可能为空，例如 `get_collider()`、`get_first_node_in_group()`、`get_parent()`：类型为 `T?`。
  参数同样审核：不接受空值的参数标为非空；引擎可能传入空值的虚函数参数和信号参数标为可空，例如 Area3D 的 `body_shape_entered` 在节点不在树中时传入 null（`scene/3d/physics/area_3d.cpp:233-234`）。
- 理由：引擎用 `RequiredResult` 标注“只在出错时为 null”的返回值（`core/object/object.h:942-944`），这样的 API 声明只有约 50 处，其中 29 处在 Tween。引擎传入的对象几乎总是非空，GDScript 代码也不对它们判空；若一律可空，每个 `_input(event)` 和每个信号处理函数都要先检查，而少数为空的情况由入口检查在边界上报告，不会悄悄传下去。

### 10.3 绑定元数据

- 引擎实际有约束但没有导出的，在本语言的绑定元数据中补上；引擎已导出的元数据直接使用。
- 以 Variant 绑定的工具函数（lerp、clamp、min、max、abs、wrap 等）按实参定型：实参类型相同时返回该类型。int 与 float 混用是编译错误，快速修复把整数实参改为 float，例如 `min(ratio, 1.0)`。理由：引擎的 min、max、clamp 原样返回其中一个实参（`core/variant/variant_utility.cpp:642-759`），GDScript 中 `min(1.5, 1)` 得到 int，统一提升为 float 会使结果悄悄不同；lerp 本身要求两个实参类型相同（`variant_utility.cpp:465-469`）。
- 以字面量类名过滤的引擎 API 按该类定型，例如 `find_children("*", "MeshInstance3D")` 返回 `Array[MeshInstance3D]`。
- 把“失败返回 null、错误另取”的引擎 API（如 `FileAccess.open`）封装为返回 Result 的标准库函数。

### 10.4 字符串形式的成员名

- `connect`、`emit_signal`、`call`、`call_deferred`、`set_deferred`、`has_method`、`Callable(obj, "m")`、`tween_property`，以及 `"position:x"` 一类属性路径，在目标类型已知时，字面量按静态引用解析：检查名字是否存在以及值的类型，参与重命名和查找引用。
- 哪些参数是名字，由绑定元数据标注。
- 非字面量的名字是显式的动态访问。

### 10.5 反射暴露的成员

- 引擎、GDScript 和 C# 通过反射（`get_property_list`、`has_method`，以及按名字的 `call`、`get`、`set`）只能看到：
  - `pub` 成员；
  - 引擎必须按名字找到的成员：导出属性、信号、引擎虚函数的覆盖、场景中连接的方法，以及本语言代码以字面量名字交给引擎的成员，例如 `call_deferred("reset")`、`tween_property(self, "current_speed")` 和场景动画轨道中的属性路径。后一类由编译器收集。
- 其余成员对按名字的访问不可见：GDScript 引用它们是 GDScript 的编译错误；C# 经 `Call` 的访问，以及任何语言以非字面量名字的访问，在运行时报告找不到成员。
- 泛型方法不经反射暴露，因为按名字调用时给不出类型实参；引擎必须按名字找到的方法不能是泛型（§3.10）。
- 由方法值形成的 Callable（例如 `tween_callback(change_weapon)`、`sig.connect(_on_x)`）直接指向方法，不按名字查找，不受此限制。
- 调试器可以看到全部成员。
- 理由：不带 `pub` 的成员只会被本文件、子类和编译器收集到的字面量名字引用，改名时工具能找全。

### 10.6 数值边界

- 写入引擎的 32 位浮点位置时隐式收窄，例如标准构建中为 float 的 `real_t` 向量分量（`core/math/math_defs.h:143-146`）。
- 传给引擎 int32 参数时做范围检查，越界是程序错误；引擎自身在这里会直接截断（`core/variant/variant.h:343-344`）。

### 10.7 String 与 StringName

- 两个方向都在需要时隐式转换，与 GDScript 相同；`==` 按内容比较（`core/variant/variant_op.cpp:196-203, 493, 615`）。
- 期望类型为 StringName 或 NodePath 时，字符串字面量在编译期构造为该类型，运行时不再转换。从 String 构造 StringName 需要在全局表的互斥锁下查找（`core/string/string_name.cpp:263-285`）。
- `&"x"` 和 `^"x"` 前缀照旧。热路径上的运行时转换给出性能提示。

### 10.8 JSON

- 引擎把 JSON 中的所有数字解析为浮点数（`core/io/json.cpp:390-396`）。解码到 `int` 时，接受恰为整数且在 int64 范围内的浮点数，否则报错，不截断。
- 标准库提供 struct 和 enum 与 JSON、`var_to_str`、ConfigFile 之间的编码和解码。元素为 struct 或带载荷的 enum 的容器，先用 `Array(hits)`、`Dictionary(d)` 转为引擎的容器（§3.8）。

## 11. 与 GDScript 和 C# 共存

### 11.1 GDScript 类的静态视图

- 本语言通过反射看到 GDScript 类和 autoload：
  - 对象类型的成员、参数和返回值视为 `T?`；
  - 值类型照声明；
  - 没有标注类型的是动态值（§3.13）。
- autoload 名是全局标识符，类型为其脚本的类。

### 11.2 继承

- 两个方向的跨语言继承都不支持：
  - 本语言的类不能继承 GDScript 类。插件要求继承它的类时（例如 Beehave 的 ActionLeaf），由用户手写一层薄的 GDScript 子类，在其中调用本语言的代码；GDScript 通过反射看到本语言的类型，对实例成员的调用仍有静态检查（§10.1）。依据：[暗坑清单](surveys/pitfalls.zh.md) C-17。
  - GDScript 脚本不能继承本语言的类：GDScript 解析 `extends 全局类名` 时，把基类文件当作 GDScript 解析（`modules/gdscript/gdscript_analyzer.cpp:469-485`，`modules/gdscript/gdscript_cache.cpp:83-92`），失败时只报“Could not resolve super class”（`:483, 489`）。分析服务在这样的 GDScript 文件上报错并说明原因。
- 因此迁移一个类时，它仍为 GDScript 的子类必须一起迁移（§13.4）。依据：[第二次纸面迁移](surveys/paper-migration-2.zh.md)中，Ring 的 Brick 和 Enemy 因此连带迁移了 5 个子类。

### 11.3 跨语言调用与消息

- GDScript 和 C# 只能按名字访问 §10.5 所列的成员。
- 【已知限制】GDScript 只能经实例使用本语言的类，以及用 `new` 构造（§10.1）。以下类级访问不受支持，替代写法是经实例或 autoload 的实例方法转一层：
  - 静态方法：GDScript 的分析器接受 `Player.make()`，运行时却报找不到方法，因为 GDScript 和 C# 靠覆盖引擎内部的 `callp` 分派静态方法，GDExtension 没有对应的钩子（`core/object/object.cpp:800-826`）；
  - 类中的 enum 和常量：不能用在 GDScript 的类型标注中（`modules/gdscript/gdscript_analyzer.cpp:935-937`），也不能用作 `const` 的值或写成 `Player.State.IDLE`，因为 GDScript 不把其他语言的常量当作常量表达式（`gdscript_analyzer.cpp:4321-4330`）；读取常量的值可以；
  - 本语言写的 autoload：GDScript 只为 GDScript 写的 autoload 定型（`gdscript_analyzer.cpp:806-826, 4579-4616`），所以它在表达式中的类型是 Node，写在类型标注中时悄悄变成无类型。
- 【后续】经 Script 资源上的脚本实例支持 GDScript 调用静态方法；需要先在运行的引擎上验证。
- 与 C# 之间的消息用 struct 表达，编码为按字段名的 Dictionary（§3.6），入口处解码并检查。
- 跨语言 await 见 §9.8。

## 12. 运行时

### 12.1 适配层

- 访问引擎的代码集中在一层薄的适配层中。实例的创建、释放、属性读写和方法调用都经过 GDExtension 的脚本实例接口。

### 12.2 性能

- 明显快于 GDScript 即可，不以 C# 为目标。多数脚本碰不到解释器的性能瓶颈。
- 解释器须独自达到性能目标：iOS 和多数主机平台不允许运行时生成机器码；copy-and-patch 一类的 JIT 只是桌面端的额外收益。

### 12.3 线程

- 虚拟机必须能在引擎选定的任意线程上安全运行，例如后台加载资源的线程和物理线程。
- 共享对象上的数据竞争由用户负责。
- 【已知限制】第一版不新增线程 API，使用引擎的 Thread、WorkerThreadPool 和 Mutex。依据：[语料计数](surveys/corpus-counts.zh.md)中，`Thread.new` 只有 5 处，`WorkerThreadPool` 2 处。

### 12.4 热重载

- 只修改函数体时，运行中直接替换：下一次调用使用新代码，正在挂起的协程用旧版本执行完。
- 修改成员、签名或类结构时不迁移状态，提示需要重新运行；编辑器中的 tool 脚本重建实例。

### 12.5 调试支持

- 接入引擎的调试界面：断点、单步、局部变量、调用栈。
- debug 构建记录本语言实例的释放位置，以及本语言代码释放任何对象的位置，用于已释放对象的诊断（§3.3），并在退出时列出泄漏的节点和对象（§6.7）。

### 12.6 标准库

- 标准库不重写引擎已有的功能，只补三类：
  - 绑定审核得到的带类型封装，例如把 `FileAccess.open` 封装为返回 Result（§10.3）；
  - 本语言类型的配套：struct 和 enum 的编码和解码（§10.8），Option 和 Result 上的方法（§3.4、§3.5），原生容器上接受带类型 lambda 的 `map`、`filter`、`sort`（§3.8）；
  - 测试运行器（§13.2）。
- 不提供包管理器。

## 13. 工具链

### 13.1 分析服务

- 分析服务运行在编辑器进程中。外部 IDE 通过 LSP 连接；命令行和 CI 以无界面模式运行同一套分析。无界面的分析命令在存在编译错误时以非零退出码结束，包括标在场景上的缺值错误（§7.5）。
- 理由：跨场景的重构要修改编辑器正打开着的场景，只有编辑器能安全地做到。GDScript 的 LSP 同样是编辑器插件（`modules/gdscript/language_server/gdscript_language_server.h:35`，`modules/gdscript/register_types.cpp:167`）。

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
- 理由：否则导出成员改名后，场景中保存的值会静默丢失：加载时不检查 `node->set` 是否成功（`scene/resources/packed_scene.cpp:494`），保存时只写节点当前属性列表中的属性（`packed_scene.cpp:860-869`）。
- 在场景编辑器中给节点改名或移动节点时：
  - 本语言脚本中指向它的 `$X`、`%X` 和字面量 `get_node` 路径自动更新，与编辑器对场景中 NodePath 属性和动画轨道的更新（`editor/docks/scene_tree_dock.cpp:2143-2248, 2250-2401`）放在同一个撤销步骤中；
  - 做法是在编辑器提交动作后发出的 `history_changed` 信号中，创建同名、合并方式为 `MERGE_ALL` 的动作，它并入刚提交的动作，并保留其原有的操作（`editor/editor_undo_redo_manager.cpp:298`，`core/object/undo_redo.cpp:97, 116-117`）。不能在编辑器的动作执行中追加操作：移动节点时编辑器不在动作内部发出信号（`scene_tree_dock.cpp:2535-2677`），提交时又先退出动作再执行（`undo_redo.cpp:313-332`）。新动作以被编辑场景的根节点为上下文，从而进入同一个撤销历史（`editor/editor_undo_redo_manager.cpp:63-105, 149-156`）；信号处理是同步的，满足合并要求的 800 毫秒时限；动作名经过翻译，从历史中读出。这一做法已在运行的引擎上验证（[编辑器检查](../tests/engine/project/addons/petrichor_checks/plugin.gd)）；
  - 不预览，完成后在输出面板列出改动的位置；
  - 非字面量的路径，以及脚本挂在其他场景中、同一路径在那里仍指向别的节点的情况，只报告不修改，并提供更新路径的快速修复。

### 13.4 迁移

- 迁移由编译错误和快速修复引导。机械的修改，例如补 `pub`、补参数类型、改写真值判断、补 `.detach()`，可以由快速修复完成。依据：[第二次纸面迁移](surveys/paper-migration-2.zh.md)中，230 处修改有 197 处是机械的。
- 迁移一个类之前，列出它仍为 GDScript 的子类，并提供一起迁移的选项（§11.2）。

### 13.5 以后再做

- 【后续】格式化；提取方法、修改签名、把成员提到基类等重构；性能分析的深度集成；文档生成；为要求继承的 GDScript 插件类生成转发用的 GDScript 外壳。

## 14. 第一版范围与待定问题

### 14.1 已知限制

- 导出的游戏不能在运行时从源码加载脚本（§1.3）。
- struct 不能 `@export`，也不能存入 `.tres`（§3.6）；带载荷的 enum 不能有方法，也不能 `@export`（§3.7）。
- 没有带状态的 mixin，接口不能是泛型（§3.9）。
- 没有命名空间（§4.1）。
- 不支持跨语言继承，两个方向都不支持（§4.1、§11.2）。
- GDScript 不能调用本语言类的静态方法，不能把其中的 enum 和常量用作类型或常量表达式，本语言写的 autoload 在 GDScript 中没有类型（§11.3）。
- match 没有类型模式、没有 match 表达式，也不能在逗号分隔的多个模式中绑定（§5.6）。
- 没有“就绪后才赋值的非空成员”（§6.2）。
- 协程没有 finally 或 defer（§9.4）。
- GDScript 脚本被重载时，等待它挂起的函数的协程会永远挂起（§9.8）。
- 引擎的导出器不因确定缺值的错误中止导出（§7.5）。
- 不新增线程 API（§12.3）。

### 14.2 以后可以兼容地加入

- 含编译器的运行时库，供需要在运行时从源码加载脚本的项目选用，代价是加载时编译（§1.3）；
- 别名或 `import`（§2.4）；
- struct 构造的位置参数、修改 self 的 struct 方法、struct 成员的 `private`、不经容器的值类型递归（§3.6）；
- 泛型接口，写作 `interface Source[T]:`（§3.9）；
- 泛型的多个约束，以及以类作约束（§3.10）；
- 函数类型中的参数名、直接调用 `f(x)`、把普通函数传给期望 async 的位置（§3.11）；
- 更严的 `private` 可见性（§4.2）；
- 调用处的展开写法（§4.6）；
- 条件绑定，或检查与使用之间没有调用、await 和赋值时让没有 getter 的字段收窄（§5.4）；
- match 的类型模式、match 表达式，以及在逗号分隔的多个模式中绑定（§5.6）；
- 就绪后才赋值的非空成员（§6.2）；
- 始终生效的断言（§8.3）；
- 重入策略的注解，例如 `@restart`（§9.5）；
- 按参数解构多参数信号的 await 结果，带参数类型的 Signal 类型（§9.6）；
- 为 GDScript 协程调用取得 `Task` 句柄（§9.8）；
- GDScript 调用本语言类的静态方法（§11.3）；
- §13.5 所列的工具。

### 14.3 待定问题

目前没有。

## 15. 一致性检查清单（非规范性）

本清单列举前述规则的检查实例，不增加语言要求。

| 领域 | 检查内容 |
|---|---|
| 语法硬规则 | 本文中每一处与 GDScript 不同的规则，对应的 GDScript 写法都得到编译错误和改法；运行时行为不同的，只限 §1.2 所列的例外，即 GDScript 在那里运行时出错或悄悄得到错误结果 |
| 数值 | `var hp: int = 10; hp = 2.5` 报编译错误，`hp = int(2.5)` 得 2；int 实参传给 float 参数无需转换；`7 / 2` 得 3；`Vector2i(1, 2) * 0.5` 为 Vector2；`Vector2() + Vector2i()` 报编译错误；整数除以 0 是程序错误；超出 int32 范围的值传给引擎的 int32 参数是程序错误 |
| 可空与释放 | 已释放对象经 `T?` 读出为 None；经 `T` 访问成员是程序错误，debug 构建中，本语言实例的诊断写明释放位置；`queue_free()` 之后、帧末之前仍算存活；两个不同的 Node 释放后，以 Node 比较为假，以 `Node?` 比较为真；已释放的 Node 上 `x is Sprite2D` 为假，`x as Sprite2D` 为 None |
| Option 与 Result | `?` 在 None 或 `Err` 时从当前函数返回；在返回 void 的函数中使用 `?` 报编译错误；Option 传播到返回 Result 的函数报编译错误；`await f()?` 等于 `(await f())?`；丢弃 Result 报编译错误；`unwrap` 失败是程序错误 |
| 嵌套的 Option | `Array[Enemy?]` 为空时 `pop_back()` 得 None，末元素为 None 时得包含 None 的 Some；`var x: Enemy?? = maybe_enemy` 得 Some；`Dictionary[String, Variant]` 中缺键得 None，值为 null 得包含 NIL 的 Some；把 `int??` 或 `Variant?` 传给 Variant 参数报编译错误并给出 `.flatten()` 或 `.unwrap_or(null)`；`var x: Enemy?? = Some(None)` 得包含 None 的 Some；对 `Enemy?` 的 match 有 `Some(var e)` 和 `None` 两个分支即为穷尽 |
| struct 与带载荷的 enum | 在方法中给 self 的字段赋值报编译错误；`player.last_hit.damage = 3` 调用 `last_hit` 的 setter；`hits[0].damage = 3` 之后 `hits[0].damage` 为 3；`get_hit().damage = 3` 报编译错误；`var b = a; b.damage = 1` 不改变 a；struct 的 Array 字段在复制后仍共享；字段相同的两个 struct 相等，作为 Dictionary 键时命中同一项；`c.target` 报编译错误；`@export` 带载荷的 enum 报编译错误；`struct ListNode` 中声明 `var next: ListNode?` 报编译错误，`Tree[T]` 经 Array 递归合法；含 Array 字段的 struct 作 Dictionary 的键报编译错误 |
| 容器 | `pop_front()` 在空数组上得 None；`front()` 在空数组上是程序错误；越界下标是程序错误；`d[k]` 在缺键时是程序错误；元素为 struct 的数组直接传给 Variant 参数报编译错误，快速修复改为 `Array(hits)`，得到元素为 Dictionary 的副本，之后修改原数组不影响副本；`Array[Hit].try_from(Array(hits))` 得 `Ok` |
| 接口 | 实现 `Combatant extends Damageable` 的类可以传给期望 Damageable 的参数；两个父接口的同名方法签名不兼容时报编译错误；两个父接口都有默认实现而未覆盖时报编译错误；`interface A extends B` 与 `interface B extends A` 报编译错误；声明 `interface Source[T]:` 报编译错误；`@readonly var hp: int` 的要求可以由可读写的成员满足；`var hp: int` 的要求遇到没有 setter 的引擎属性报编译错误；默认方法给 `@readonly` 属性赋值报编译错误；在接口以外使用 `@readonly` 报编译错误 |
| 泛型 | `first([1, 2])` 中 T 推断为 int；`var x = first([])` 推断不出 T 报编译错误，`var x = first[int]([])` 合法；不满足约束的类型实参报编译错误；Node 脚本声明类型参数报编译错误；GDScript 引用泛型方法是 GDScript 的编译错误；`sig.connect(handle)` 在 handle 为泛型且类型实参确定不了时报编译错误；不继承 Resource 的泛型 RefCounted 类合法，不出现在引擎的全局类表中 |
| 函数类型 | `func(int)` 与 `func(int) -> void` 相同；`(func(Enemy) -> bool)?` 可空，`func(Enemy) -> bool?` 返回 `bool?`；`f(x)` 报编译错误并给出 `f.call(x)`；`func(Node)` 可以用于期望 `func(Node3D)` 的位置，反之报编译错误；含 await 而未写 async 的 lambda 报编译错误；把 async 函数传给期望普通函数类型的本语言参数报编译错误；把 async 方法连接到信号合法，任务作为根任务运行 |
| Variant | `v.hp`、`v + 1`、`v[0]`、`for x in v` 报编译错误；`if v is Dictionary:` 之后 `v.get("hp", 0)` 合法；`v as Node` 为 `Node?`；v 为 NIL 时 `v == None` 为真、`v is int` 为假；v 中是已释放的对象时 `v == None` 为真、`v is Node` 为假、`v as Node` 为 None；int 赋给 Variant 无需转换 |
| 抽象类与剩余参数 | 构造抽象类报编译错误；具体子类缺少某个抽象方法的实现报编译错误；场景中的节点使用抽象类报编译错误；向 `...args: Array[int]` 传入 String 报编译错误；只写 `...args` 报编译错误；GDScript 调用 `...args: Array[int]` 时传入 String 是程序错误 |
| 可见性 | 不带 `pub` 的成员在其他类中不可见，在子类中可以读写和覆盖；在子类以外的其他类中使用不带 `pub` 的 struct 报编译错误；覆盖时改变可见性报编译错误；`pub` 函数的签名中出现非 `pub` 的类型报编译错误；GDScript 引用不带 `pub` 的成员报 GDScript 编译错误；`call_deferred("reset")` 使 `reset` 可以被引擎按名字调用 |
| 成员 | `var speed = 5.0` 为 float；空容器成员要求标注；没有初始化式的 int 成员为 0，`T?` 成员为 None；所有字段都有默认值的 struct 成员取默认值，否则必须赋值 |
| 签名 | `func _process(delta):` 中 delta 为 float；覆盖时把返回类型写成子类型合法，写成父类型报编译错误；覆盖时多出带默认值的参数合法；非覆盖函数省略返回类型后返回值报编译错误；有期望类型的 lambda 参数不必标注 |
| 信号 | emit 实参不符和 connect 参数不符报编译错误；场景中的连接与信号不符时标在场景中；单参数信号的 await 结果有该参数类型；使用多参数信号的 await 结果报编译错误；await `Signal` 类型的变量得到 Variant，信号有两个参数时运行时为 Array |
| 相等 | `1 == 1.0` 为真；`hp == "3"` 报编译错误；`Node2D` 与 `Node3D` 比较报编译错误；hp 为 `int?` 时 `hp == 3` 合法；非空的 `Node` 与 None 比较报编译错误；`Array[int] == Array[float]` 报编译错误；Variant 中是 String 时与 int 比较是程序错误；`Enemy?` 的值可以传给 `Node?` 参数，`Array[Enemy]` 传给 `Array[Node]` 参数报编译错误；`Some(3) == Some(Some(3))` 先把左侧包装为两层再比较 |
| 转换 | `node as Sprite2D` 为 `Sprite2D?`；`x as int` 报编译错误并给出 `int(x)` 与 `int.try_from(x)`；`int.try_from(2.0)` 得 `Ok(2)`，`int.try_from(2.5)` 和 `int.try_from("2")` 得 `Err` |
| 收窄 | `!= None`、`is` 和提前返回之后局部变量收窄；条件表达式的分支和 `and` 右侧收窄；成员不收窄；`if x:`、`if not x:` 在 x 不是 bool 时报编译错误并按类型给出改法 |
| match | enum 的 match 不穷尽报编译错误；带 `when` 的分支不计入覆盖；未列出字段而没有 `..` 报编译错误；Option 的 `var x` 分支中 x 为 T；对 int 的 match 不要求穷尽 |
| 循环 | `for i in 5` 中 i 为 int；`for x in 2.5` 和遍历向量报编译错误；循环变量标注为子类报编译错误 |
| 初始化 | 成员初始化式不调用 setter；`@onready` 与 `@export` 同用报编译错误；`_init` 中读取 `@onready` 成员报编译错误；静态初始化的环是程序错误并列出依赖链 |
| 必须赋值 | `BulletScene.instantiate(speed = 300.0)` 漏写 `target` 报编译错误；祖先在 `_ready` 中经 `$Child.target = v` 赋值时不报错；子节点在自己的 `_ready` 中读取该成员报编译错误；就绪前读取未赋值的成员是程序错误；`new()` 漏传必须赋值的成员不是编译错误，Node 在就绪时检查 |
| 就绪前的 setter | 场景中保存的值使 setter 在访问 `@onready` 成员处挂起，就绪后继续；就绪前先写入 `"Hi"` 而挂起、再写入 None 而不挂起时，`"Hi"` 那次调用被取消；setter 调用以 `if not is_node_ready(): return` 开头的方法时不挂起；先变换再存储的 setter 就绪前读到变换后的值；节点就绪前被释放时挂起的调用被丢弃 |
| 泄漏 | 移出后未保存也未释放的节点给出警告；捕获 self 的闭包存入 self 的成员时给出警告和 `weak` 的快速修复；debug 构建退出时列出泄漏的对象和创建位置 |
| 场景定型 | `$Head/Camera` 按场景定为 Camera3D；两个场景给出不同类型时报编译错误；找不到的路径标在缺少该节点的场景上；只被继承的基础场景缺少继承它的场景都有的节点时不报错，被 preload 的占位场景缺少时报错；`get_node_or_null("X")` 在部分场景中缺失时类型为 `T?` 且不报错；任何场景中都没有的 `$X` 报编译错误，`get_node_or_null("X")` 得 `Node?` 并给出警告；`/root/X` 报编译错误并给出 autoload 全局名；声明 `@owner(T)` 的组件场景被实例化到别的场景中时合法，作为根使用时报编译错误；声明 `@owner(T)` 的节点移出 owner 的子树后不读 owner 不报错，读取时是程序错误；未声明 `@parent` 时 `get_parent().add_child(x)` 报编译错误，快速修复补上 `@parent(T)`；游离节点上 `var p = get_parent()` 得 None；声明 `@parent(Node2D)` 的节点在场景中挂在 Control 下时报编译错误 |
| 导出检查 | player.tscn 的 `crosshair` 由 main.tscn 的实例赋值时不报错；某个实例没有赋值时标在该实例所在的场景上；没有被引用的场景只给警告，导出时不报错；运行中缺值时就绪报程序错误并写明成员名；有确定缺值的场景时，导出窗口显示失败并写明场景和成员名，无界面分析命令返回非零；`PackedScene[Boss]` 可以赋给 `PackedScene[Enemy]`；新增了不导出的必须赋值成员的 Boss 场景填入 `PackedScene[Enemy]` 时，标在填入它的场景上报错；Enemy 有不导出的必须赋值成员 `target` 时，导出的 `PackedScene[Enemy]` 调用 `instantiate()` 漏传报编译错误 |
| 资源 | 修改共享资源给出警告和两个快速修复；`duplicate()` 保持静态类型；`preload` 环报编译错误 |
| 程序错误 | `_process` 往下两层调用中的程序错误中止这两层和 `_process`，下一帧照常调用 `_process`；需要返回值的入口，引擎得到该类型的默认值；除 `assert` 外，程序在 debug 和 release 构建中行为相同；无限递归在第 1025 层调用处成为程序错误，进程不崩溃 |
| 断言 | debug 构建中 `assert(false)` 是程序错误；release 构建中 `assert(f())` 不调用 f；`assert(await t)` 报编译错误；`assert(save_data)` 在 save_data 为 `Resource?` 时报编译错误并给出 `save_data != None` |
| 协程 | 含 await 而未声明 async 报编译错误；丢弃 async 调用的结果报编译错误，`f().detach()` 合法；`cancel()` 之后结果一定是取消；`await f(...)` 启动的子任务随父任务取消，`await t` 只停止等待；被等待的兄弟节点与等待方在同一次场景释放中被删除时不报错；被等待的对象被单独释放时，等待方在本帧结束后报程序错误，`Task.of(信号).settled()` 得 `Cancelled(CancelReason.AWAITED_FREED)`；失败的任务经 `settled()` 等待时，失败照常传给等待方；`var t: Task[void] = f()` 合法，没有类型实参的 `Task` 报编译错误；所属对象被释放时协程静默取消；GDScript 对象在其协程第二次挂起期间被释放时，await 这个协程的等待方在本帧结束后报程序错误，经 Callable 调用时相同 |
| 引擎边界 | 未审核的、返回对象的引擎方法类型为 `T?`；`get_tree()` 非空，`get_collider()` 可空；未审核的虚函数参数和信号参数为 T，引擎传入 null 是程序错误；未审核的参数接受 None；GDScript 向非空参数传入 null 是程序错误；远程检查器向非空成员写入 null 时原值不变；经信号传出的 struct 是按字段名编码的 Dictionary，`Command.MOVE(target = p)` 是 `{"$variant": "MOVE", "target": p}` |
| 工具函数 | `min(1.5, 1)` 报编译错误并给出 `min(1.5, 1.0)`；`lerp(a, b, t)` 的结果与 a 同类型；`find_children("*", "MeshInstance3D")` 为 `Array[MeshInstance3D]` |
| 字符串 | 期望 StringName 的字面量在编译期构造；`&"a" == "a"` 为真；字面量成员名不存在时报编译错误，并参与重命名 |
| 跨语言 | GDScript 文件继承本语言的类时，分析服务在该文件上报错并说明原因；await GDScript 协程得到按声明类型检查的结果；作为语句调用 GDScript 协程不报错，把它的结果当作值使用而不写 await 是程序错误；GDScript await 本语言的协程，在取消时得到 null 而不挂起；GDScript await 本语言中返回 Transform3D 的协程时没有警告，取消时把结果赋给 Transform3D 变量，在 debug 构建中报赋值错误而不崩溃；GDScript 持有 `p: Player` 时，属性、方法和信号的类型错误报 GDScript 编译错误；`Player.new()` 的类型为 Player，多传实参报 GDScript 编译错误 |
| 重构 | 导出成员改名时场景中的键同时修改；场景编辑器中给节点改名或移动节点时脚本中的 `$X` 同时修改，并在同一撤销步骤中，一次撤销同时还原场景和脚本，一次重做同时恢复；非字面量路径只报告 |

## 16. 反馈

欢迎通过 [GitHub issues](https://github.com/YYF233333/Petrichor/issues) 提出反馈。可采用以下格式：**问题 → 最小反例 → 对应章节 → 严重程度 → 最小修改 → 测试**。

## 附录 A：引擎事实

以下事实基于 Godot 4.7.2-stable（提交 `ed1daf0`），路径相对于 Godot 仓库的根目录。

| 事实 | 出处 |
|---|---|
| Variant 的整数是 int64_t，浮点数是 double | `core/variant/variant.h:252-253` |
| Variant 转为 int32 时直接截断 | `core/variant/variant.h:343-344` |
| 标准构建中 `real_t` 是 float | `core/math/math_defs.h:143-146` |
| 引擎的运算符表 | `core/variant/variant_op.cpp:212-1033` |
| int 与 float 的 `==` 按数值比较；对象的 `==` 比较同一性，但先把已释放的对象取为 null | `core/variant/variant_op.cpp:490-491, 514`；`core/variant/variant_op.h:471-477` |
| 已释放的对象与 NIL 比较为真 | `core/variant/variant_op.h:484-489` |
| String 与 StringName 的 `==` 按内容比较 | `core/variant/variant_op.cpp:196-203, 493, 615` |
| 运算表中没有的组合求值无效 | `core/variant/variant_op.cpp:1049-1054` |
| Array 的 `==` 逐元素用 `hash_compare`，它要求元素类型相同 | `core/variant/array.cpp:155-158`；`core/variant/variant.cpp:3176-3179` |
| Dictionary 的 `==` 按键查找后逐项比较值，与顺序无关 | `core/variant/dictionary.cpp:255-285` |
| 已释放的对象在条件中为假：对象的 `is_zero` 检查对象是否仍然有效 | `core/variant/variant.cpp:952-953`；`core/variant/variant_op.cpp:1120-1122` |
| min、max、clamp 原样返回其中一个实参 | `core/variant/variant_utility.cpp:642-759` |
| lerp 要求两个实参类型相同 | `core/variant/variant_utility.cpp:465-469` |
| `pop_front`、`pop_back` 在空数组上返回 null 而不报错 | `core/variant/array.cpp:781-800` |
| `front`、`back` 在空数组上报错 | `core/variant/array.cpp:354-362` |
| Dictionary 的 `get` 在缺键时返回默认值 | `core/variant/dictionary.cpp:178-187` |
| `bind` 把绑定的实参接在调用实参之后 | `core/variant/callable_bind.cpp:141-150` |
| 从 String 构造 StringName 需要在全局表的互斥锁下查找 | `core/string/string_name.cpp:263-285` |
| JSON 中的数字都解析为浮点数 | `core/io/json.cpp:390-396` |
| 资源按路径缓存，`load` 默认复用缓存 | `core/io/resource_loader.cpp:800-810` |
| 资源的循环加载返回 null | `core/io/resource_loader.cpp:1049-1056` |
| 方法的反射信息中没有协程性 | `core/object/method_info.h:37-44` |
| 脚本语言所需的钩子对 GDExtension 公开 | `core/object/script_language_extension.h:304, 391, 441, 497, 655` |
| 扩展实现的脚本语言经 `_is_abstract` 和全局类表报告抽象性 | `core/object/script_language_extension.h:132-136, 657-670` |
| 脚本语言通过 `get_extension` 自报扩展名 | `core/object/script_language_extension.h:236` |
| 脚本实例创建时不接受构造参数 | `core/object/script_language.h:152` |
| `RequiredResult` 标注只在出错时为 null 的返回值 | `core/object/object.h:942-944` |
| 对象释放时调用每个实例绑定的释放回调 | `core/object/object.cpp:2360-2365` |
| 一个对象可以同时持有多个标识不同的实例绑定 | `core/object/object.cpp:2167-2201` |
| 对象析构时静默断开它的所有信号连接 | `core/object/object.cpp:2327-2342` |
| 对象设置脚本后发出 `script_changed` | `core/object/object.cpp:995` |
| 引擎不给对象设置抽象脚本 | `core/object/object.cpp:976` |
| PREDELETE 按逆序发出，先通知脚本实例，再通知原生类 | `core/object/object.cpp:173, 928-931` |
| RefCounted 在 PREDELETE 时引用计数已归零，再加引用会失败 | `core/object/ref_counted.cpp:94-95`；`core/templates/safe_refcount.h:136-146`；`core/variant/variant.cpp:1104-1108, 2509-2510` |
| 脚本语言的每帧回调在场景树处理完删除队列之后调用 | `main/main.cpp:5106-5109`；`core/object/script_language_extension.h:651`；`scene/main/scene_tree.cpp:735` |
| `_process` 和 `_physics_process` 的参数是 double | `scene/main/node.h:433-434` |
| `NOTIFICATION_PARENTED` | `scene/main/node.h:465` |
| `get_tree()` 在节点不在树中时报错并返回 null；`get_parent()`、`get_owner()` 没有父节点或 owner 时返回 null 而不报错 | `scene/main/node.h:558-561`；`scene/main/node.cpp:2100-2102, 2296-2298` |
| `set_owner(null)` 清除 owner，不报错 | `scene/main/node.cpp:2273-2281` |
| 节点移出 owner 的子树时 owner 被置为 null | `scene/main/node.cpp:391-396, 1781-1783` |
| owner 被释放时，它拥有的节点的 owner 被置为 null | `scene/main/node.cpp:299-302` |
| 节点在 PREDELETE 中从最后一个子节点起逐个删除子节点 | `scene/main/node.cpp:308-312` |
| 节点入树时，就绪同步传播完毕 | `scene/main/node.cpp:3368-3372` |
| 子节点先于父节点就绪；节点重新入树时不再执行 `_ready` | `scene/main/node.cpp:326-337, 454` |
| `queue_free` 把节点放入删除队列，在每个物理步和处理帧的末尾删除 | `scene/main/node.cpp:3461-3471`；`scene/main/scene_tree.cpp:666, 735, 1626-1643` |
| Area3D 的 `body_shape_entered` 在物体的节点不在树中时以 null 作为节点参数 | `scene/3d/physics/area_3d.cpp:233-234` |
| 没有设为局部于场景的子资源，由场景的所有实例共用 | `scene/resources/packed_scene.cpp:705-706` |
| 实例化场景时，先设置保存的属性，再把节点加到父节点下 | `scene/resources/packed_scene.cpp:494, 543` |
| 加载场景时不检查属性写入是否成功 | `scene/resources/packed_scene.cpp:494` |
| 节点引用以路径保存，实例化时解析 | `scene/resources/packed_scene.cpp:376-395` |
| 保存场景时只写节点当前属性列表中的属性 | `scene/resources/packed_scene.cpp:860-869` |
| 场景的根被实例化到另一个场景中时，owner 是外层场景的根 | `scene/resources/packed_scene.cpp:567-570` |
| 编辑器给节点改名或移动节点时，更新场景中的 NodePath 属性和动画轨道 | `editor/docks/scene_tree_dock.cpp:2143-2248, 2250-2401`（调用在 `:2426, 2667`） |
| 移动节点时，编辑器在动作内部不发出信号 | `editor/docks/scene_tree_dock.cpp:2535-2677` |
| 提交动作时先退出动作再执行，执行中不能再追加操作 | `core/object/undo_redo.cpp:313-332` |
| 编辑器的撤销管理器在提交后发出 `history_changed` | `editor/editor_undo_redo_manager.cpp:298` |
| 同名、合并方式为 `MERGE_ALL`、在前一个动作之后 800 毫秒内创建的动作并入前一个动作，保留其原有的操作 | `core/object/undo_redo.cpp:97, 116-117` |
| 撤销管理器按动作的上下文对象选择撤销历史，被编辑场景中的节点进入该场景的历史 | `editor/editor_undo_redo_manager.cpp:63-105, 149-156` |
| Web 导出开启 Extensions Support 时使用带动态链接的模板；跨域隔离响应头只在开启线程支持时需要 | `platform/web/export/export_plugin.cpp:374`；`platform/web/export/export_plugin.h:59-73`；`platform/web/doc_classes/EditorExportPlatformWeb.xml:93-95` |
| 导出插件的钩子都不能使导出失败 | `editor/export/editor_export_platform.cpp:2568-2576`；`editor/export/editor_export_plugin.h:120-122` |
| 导出插件报告的错误只让导出窗口显示失败；命令行导出只在导出函数返回错误时以非零退出码结束 | `editor/export/editor_export_platform.cpp:131, 146-148`；`editor/editor_node.cpp:1408-1424` |
| GDScript 通过 Script 的反射接口为其他语言的全局类定型 | `modules/gdscript/gdscript_analyzer.cpp:789-804, 4278-4319, 6005-6014` |
| GDScript 对 self 和内置类型以外的对象调用未知方法，只给不安全访问的警告 | `modules/gdscript/gdscript_analyzer.cpp:3750-3757` |
| GDScript 解析 `extends 全局类名` 时把基类文件当作 GDScript 解析，失败时只报“Could not resolve super class” | `modules/gdscript/gdscript_analyzer.cpp:469-485`（报错在 `:477, 483`）；`modules/gdscript/gdscript_cache.cpp:83-92` |
| GDScript 要求具体子类实现抽象方法，禁止构造抽象类和经 `super` 调用抽象方法 | `modules/gdscript/gdscript_analyzer.cpp:1533-1552, 3630-3643` |
| GDScript 的覆盖检查允许返回类型协变、参数类型逆变，以及多出带默认值的参数 | `modules/gdscript/gdscript_analyzer.cpp:1895-1936` |
| GDScript 的静态检查要求带类型的 Array、Dictionary 的元素类型完全相同 | `modules/gdscript/gdscript_analyzer.cpp:6379-6393` |
| GDScript 的剩余参数只能是元素为 Variant 的 Array | `modules/gdscript/gdscript_analyzer.cpp:1803-1810` |
| GDScript 从其他语言的方法信息中读取 `METHOD_FLAG_VARARG` | `modules/gdscript/gdscript_analyzer.cpp:6052-6055` |
| GDScript 把 `new` 当作静态的 `_init` 检查；`new` 由 GDScript 自己绑定，Script 基类没有 | `modules/gdscript/gdscript_analyzer.cpp:5958-5961`；`modules/gdscript/gdscript.cpp:1078` |
| 对象没有脚本实例时，`callp` 只查找引擎注册的方法，所以 Script 资源上的静态方法找不到 | `core/object/object.cpp:800-826` |
| GDScript 不能在类型标注中引用其他语言的类中的类型 | `modules/gdscript/gdscript_analyzer.cpp:935-937` |
| GDScript 不把其他语言的类的常量当作常量表达式 | `modules/gdscript/gdscript_analyzer.cpp:4321-4330` |
| GDScript 只为 GDScript 写的 autoload 定型 | `modules/gdscript/gdscript_analyzer.cpp:806-826, 4579-4616` |
| GDScript 中以调用函数的写法调用 Callable 变量是编译错误，提示改用 `.call()` | `modules/gdscript/gdscript_analyzer.cpp:3741-3745` |
| GDScript 中不加 await 把协程调用当值使用是编译错误，作为语句时只是警告 | `modules/gdscript/gdscript_analyzer.cpp:3776-3784` |
| GDScript 中 `?` 是语法错误 | `modules/gdscript/gdscript_parser.cpp:3860-3861` |
| GDScript 的前缀 `!` 是逻辑非 | `modules/gdscript/gdscript_parser.cpp:2963-2965` |
| GDScript 中内部类名、`class_name` 的名字、enum 名和函数名之后不能接 `[` | `modules/gdscript/gdscript_parser.cpp:964, 1005-1010, 1602, 1805` |
| 抽象方法的声明不写冒号 | `modules/gdscript/gdscript_parser.cpp:1773-1774` |
| `@abstract` 注解，不能用于 static 方法 | `modules/gdscript/gdscript_parser.cpp:4500-4524` |
| 剩余参数只能是最后一个参数，不能有默认值 | `modules/gdscript/gdscript_parser.cpp:1709-1716` |
| GDScript 的表达式中不能出现 `...` | `modules/gdscript/gdscript_parser.cpp:4344` |
| GDScript 的类型只能以标识符或 `void` 开头 | `modules/gdscript/gdscript_parser.cpp:3874-3886` |
| GDScript 的 lambda 含 await 时即为协程，不加标记 | `modules/gdscript/gdscript_parser.cpp:3249-3251, 3752-3753` |
| match 的模式 | `modules/gdscript/gdscript_parser.cpp:2487-2711` |
| `as` 转为对象类型失败时得到 null | `modules/gdscript/gdscript_vm.cpp:1677-1678` |
| GDScript 对已释放的对象做 `is` 时报错并中止，release 构建也一样；做 `as` 时只在 debug 构建中报错 | `modules/gdscript/gdscript_vm.cpp:942-947, 1665-1669` |
| `as` 转为内置类型时按构造函数转换，转换失败只在 debug 构建中报错 | `modules/gdscript/gdscript_vm.cpp:1642-1650` |
| GDScript 在运行时遇到运算表中没有的组合时报错；运算数类型与首次执行时不同的情况只在 debug 构建中检查，release 构建得到 null | `modules/gdscript/gdscript_vm.cpp:793-798, 821-843` |
| `for` 可以遍历 float 和向量 | `modules/gdscript/gdscript_vm.cpp:3092-3229` |
| GDScript 的 `assert` 失败时报“Assertion failed” | `modules/gdscript/gdscript_vm.cpp:3876-3902` |
| GDScript 的 await 接受任意 Signal 值 | `modules/gdscript/gdscript_vm.cpp:2586-2600` |
| GDScript 对返回类型既不是协程、也不是 Variant 或 Signal 的调用的 await 给出“多余的 await”警告 | `modules/gdscript/gdscript_analyzer.cpp:3102-3107` |
| await 的结果不经检查写入按被等待者返回类型定型的临时变量 | `modules/gdscript/gdscript_vm.cpp:2667-2668`；`modules/gdscript/gdscript_byte_codegen.cpp:998-1001` |
| Variant 赋给有类型的变量时，只在 debug 构建中检查类型 | `modules/gdscript/gdscript_vm.cpp:1433-1444` |
| 挂起的 GDScript 函数状态不复制 self，只以裸指针指向实例 | `modules/gdscript/gdscript_vm.cpp:2609-2625` |
| GDScript 出错后，debug 构建按返回类型构造默认值返回，release 构建返回 null | `modules/gdscript/gdscript_vm.cpp:3963-3993`（debug）；`:535, 3995`（release） |
| GDScript 的调用栈深度上限由 `debug/settings/gdscript/max_call_stack` 设置，默认 1024，超过时报告栈溢出 | `modules/gdscript/gdscript.cpp:2870`；`modules/gdscript/gdscript.h:489-490` |
| GDScript 的 `assert` 只在 debug 构建中编译 | `modules/gdscript/gdscript_compiler.cpp:2181-2207` |
| GDScript 的链式赋值沿链读出、修改，再把值类型写回，共享类型原地修改 | `modules/gdscript/gdscript_compiler.cpp:1172-1190` |
| 成员的初始化式不经过 setter | `modules/gdscript/gdscript_compiler.cpp:2414-2430, 2603-2626` |
| 静态成员初始化之后调用 `_static_init` | `modules/gdscript/gdscript_compiler.cpp:2629-2632` |
| `@onready` 初始化式在用户的 `_ready` 之前执行，先基类后派生类 | `modules/gdscript/gdscript.cpp:1937-1953` |
| GDScript 实例释放时清除其挂起的函数状态，不恢复执行 | `modules/gdscript/gdscript.cpp:2069-2079` |
| `GDScriptFunctionState` 有 `completed` 信号 | `modules/gdscript/gdscript_function.cpp:369` |
| GDScript 协程再次挂起时，内层状态只以 ObjectID 记住第一个状态的 `completed` 信号 | `modules/gdscript/gdscript_vm.cpp:2634-2639` |
| GDScript 函数状态恢复执行时离开实例的待处理列表 | `modules/gdscript/gdscript_function.cpp:329-330` |
| `GDScriptFunctionState` 是内部类，不暴露它所属的实例；`is_valid` 在恢复执行之后总是为假 | `modules/gdscript/gdscript_function.cpp:288-307, 337, 364-370`；`modules/gdscript/register_types.cpp:140` |
| GDScript 脚本重载时清除挂起的函数，不发出信号 | `modules/gdscript/gdscript.cpp:1487-1506`；`modules/gdscript/gdscript_compiler.cpp:2700` |
| await 多参数信号得到由实参组成的 Array | `modules/gdscript/gdscript_function.cpp:256-274` |
| 被等待的对象被释放后，GDScript 的等待方永远挂起 | `modules/gdscript/gdscript.cpp:2069-2079` |
| RefCounted 类中的 lambda 强引用 self | `modules/gdscript/gdscript_lambda_callable.cpp:285-291` |
| GDScript 的 LSP 是编辑器插件 | `modules/gdscript/language_server/gdscript_language_server.h:35`；`modules/gdscript/register_types.cpp:167` |
| GDScript 导出时至多把源码转为词法单元，加载时才解析和编译 | `modules/gdscript/register_types.cpp:99-116`；`modules/gdscript/gdscript_cache.cpp:373-379, 392` |

## 附录 B：考虑过但没有采用的方案（非规范性）

| 方案 | 不采用的原因 | 现行规则 |
|---|---|---|
| 以引擎模块交付（v0.1） | 暗坑清单中没有只有模块才能修复的条目；GDExtension 可以用官方编辑器和导出模板 | §1.3 |
| 窄整数类型 i32、u8 等（v0.1） | 迁移成本最大的一项（[纸面迁移](surveys/paper-migration.zh.md)）；脚本很少在意数值类型的性能 | §3.2 |
| 推断函数签名（v0.1） | 签名是对外的约定，不应随函数体变化 | §4.6 |
| 语言层面的跨语言继承 | 依赖 GDScript 的内部实现 | §11.2 |
| 可空链 `?.`、默认值 `??`、后缀 `!` | 与 Result 的短路返回合用时，`x?.y` 的含义要看 x 的类型；Option 的操作用方法就够了 | §3.4、§8.2 |
| Swift 式的 `try` 覆盖整条表达式 | 要按类型决定哪些子表达式被解开；Rust 的后缀 `?` 更直接 | §8.2 |
| Result 也支持链式的 `?.` | 两种类型嵌套时，一个运算符无法说明处理的是哪一层 | §8.2 |
| 在属性中另设 `apply:` 块，存放须等到节点就绪才执行的代码 | 让用户思考逻辑在何时执行、执行几次；自动挂起可以由编译器完成 | §6.5 |
| 就绪前只记下 setter 的原始输入 | 先变换再存储的 setter（如给角色名加【】）就绪前会读到未变换的值 | §6.5 |
| `@late`（类似 `lateinit`） | 需要它的写法很少见；父节点配置子节点已由祖先赋值的规则覆盖 | §6.2 |
| 子类只能看到 `pub` 成员 | [第二次纸面迁移](surveys/paper-migration-2.zh.md)中 102 处 `pub` 有 29 处只是为了子类 | §4.2 |
| 公开标记用 `@public` 注解 | 写得频繁，[成员可见性调查](surveys/member-visibility.zh.md)中每千行至少 22 处 | §2.4 |
| 文件顶层的类型直接成为全局名 | 打破“文件就是类”，需要另一张全局名表 | §2.4 |
| 接口的交集类型 `@owner(Node2D & Combatant)` | 新概念；允许引擎属性满足接口的属性要求即可 | §3.9 |
| 追踪式 GC，或闭包自动弱捕获 self | 与 GDScript 的行为悄悄不同 | §3.11、§6.7 |
| 在移动节点时检查 owner | 节点移出 owner 的子树是合法操作 | §7.3 |
| 没有被引用的场景缺值时也算导出错误 | 运行时才确定路径的 `load` 看不到，无法断定场景不被使用 | §7.5 |
| `as` 转为内置类型时严格检查 | 与 GDScript 的构造转换悄悄不同 | §5.3 |
| 未审核的引擎 API 中对象类型一律可空 | 引擎传入的虚函数参数和信号参数几乎总是非空，一律可空会让每个处理函数都先判空；少数为空的情况由入口检查报告 | §10.2 |
| int 与 float 混用的工具函数统一提升为 float | GDScript 原样返回实参，结果会悄悄不同 | §10.3 |
| `pop_front()` 在空数组上是程序错误 | 引擎在这里返回 null 而不报错，会悄悄不同 | §3.8 |
| await 多参数信号得到 Array，或按参数解构 | 真实代码几乎不使用这种结果；解构可以以后加入 | §9.6 |
| 以 if/else 块为值 | 与条件表达式重复 | §5.5 |
| 为基类要求的、可选的和运行时添加的子节点各设一种声明 | 按使用方式的检查和 `get_node_or_null` 已经覆盖这三种情况，新的声明只会增加概念 | §7.2 |
| 覆盖时写出的类型必须与被覆盖的完全一致 | 比 GDScript 严，拒绝 gdUnit4 一类流式接口所用的协变返回类型 | §4.6 |
| 嵌套的 Option 合并为一层（Kotlin 的做法） | 泛型代码的行为随 T 改变；容器中存入的 None 与缺少的值无法区分 | §3.4 |
| lambda 不写 async，按函数体或期望类型推断 | 判断函数是否为 async 要用多条规则，lambda 和方法值能传到哪里也会不同 | §9.1 |
| async 方法向引擎报告声明的返回类型 | GDScript 中每处 await 都有警告；取消时的 null 写入值类型的临时变量会使进程崩溃 | §9.8 |
