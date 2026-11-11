# Petrichor 语言规范

**v0.2 草案 · 2026-10-25**

Petrichor 是一门 Godot 4 的全静态类型脚本语言，以 GDExtension 的形式交付。它在项目代码中替代 GDScript，与 GDScript 插件共存。本文是它的语言规范草案，[实现](architecture.md)尚未覆盖全部规则。设计取舍与语料依据见[设计说明](design-notes.md)，引擎核对基线见[引擎事实](engine-facts.md)。上一版规范 v0.1 保存在 git 标签 `spec-v0.1` 中。

## 1. 概述

### 1.1 问题与目标

- GDScript 的类型是渐进的。场景中的静态信息到不了脚本：节点路径、信号连接、动画轨道和资源属性。C# 有静态类型，但跨语言桥接与工具链有不少暗坑。
- 本语言是全静态类型的，只在引擎或第三方强加的地方出现动态值。静态检查延伸到场景。节点路径、场景信号连接、动画轨道、导出属性和场景资源都有类型。重命名时连同场景一起修改。
- 项目越大、维护越久，收益越大。代码由人写还是由 AI 生成都适用，无人审读的代码更依赖静态检查。
- 新项目直接使用。已有项目可整体或分批迁入，也可退回 GDScript。第三方插件保持 GDScript。

### 1.2 与 GDScript 的关系

- 表层语法沿用 GDScript：缩进、`func`、`var`、`extends`、`signal`、`@export`、`$X` 等。新增构造按同一风格扩展。
- **语法硬规则**：与 GDScript 写法相同的构造，行为要么与 GDScript 一致，要么报编译错误并给出改法，绝不悄悄不同。唯一的例外是 GDScript 在运行时出错或悄悄给出错误结果之处（如默认值、截断、永远挂起）。本语言在那里改为程序错误，或给出代码本来的意图。
- **可还原规则**：GDScript 代码的含义，在本语言中都能只用两类写法写出：
  - GDScript 也有、且含义相同的写法；
  - 可还原的写法，即去掉或按固定规则替换后得到同义 GDScript 的写法，如 `pub`、`async`、`.detach()`、类型中的 `?`。
- 可还原规则的例外是语法硬规则的例外和各处标出的【已知限制】。
- 不承诺直接编译 GDScript 源码。迁移工具按可还原规则双向转换。

### 1.3 交付形态

- 以 GDExtension 交付，装入官方编辑器并使用官方导出模板。Web 导出须开启“Extensions Support”（跨域隔离响应头仅在开启线程支持时需要）。
- 源码只在编辑器和无界面模式下编译。导出的游戏只含字节码，导出模板库不含编译器。

### 1.4 文档约定

- 定义合法程序和所需行为的规则具有规范性；例子、[设计说明](design-notes.md)和[引擎事实](engine-facts.md)不具规范性。
- 第一版指 1.0 版。1.0 之前的版本仍可能改变语义。
- 状态标记：**【待定】**表示尚未决定。**【已知限制】**表示第一版不支持，但其合法性限制仍具规范性。待定问题和第一版之后可兼容加入的特性，汇总在“第一版范围与待定问题”一章。
- **编译错误**由分析服务在运行前报告，并使程序不合法。唯一的例外是标在场景上的缺值错误，它不阻止编辑器内运行。**警告**和**提示**不影响合法性。**程序错误**是运行时错误，其处理见错误一章。
- 文中的 GDScript 指 Godot 4 的 GDScript。语法硬规则和可还原规则都以它为准。
- 文中依赖的引擎行为均对照 Godot 4.7.2-stable（提交 `ed1daf0`）核对，出处列于[引擎事实](engine-facts.md)。
- 源文件扩展名为 `.pet`。

### 1.5 保证

除语法硬规则外，本语言自身的运算处处保证以下三点。引擎 API 的行为不变。例外只有各处标出的【已知限制】。

- **类型可信**：值不符合静态类型时，在被当作该类型使用前报程序错误。非空 `T` 不会是 null，但可能是已释放对象。
- **失败不沉默**：失败要么是编译错误，要么得到 `Option` 或 `Result`，要么是程序错误。不悄悄给出默认值或错误结果，也不永远挂起。除 `assert` 外，debug 与 release 构建的检测和处理完全一致。
- **不崩溃**：合法程序不会使进程崩溃或破坏内存，共享对象的数据竞争除外。

## 2. 词法与表层语法

### 2.1 文件与类

- `preload` 任何脚本都得到它的类。以它初始化的常量在用裸名处指这个类。这个名字像 `class_name` 一样可作类型，并可访问 `new()`、静态方法和常量。`preload(...)` 后直接访问 `.new()` 或其他成员同理。在需要值的位置，它是脚本资源本身。GDScript 文件的类型为 `GDScript`，其他脚本为其脚本资源类：

  ```gdscript
  const Unit = preload("res://units/unit.gd")

  var squad: Array[Unit] = []

  func spawn(at: Vector2) -> Unit:
      var unit := Unit.new()
      unit.position = at * Unit.TILE_SIZE
      return unit

  func adopt(body: Node2D):
      if body is Unit:
          squad.append(body)
      else:
          body.set_script(Unit)
  ```

- 类头可含类级注解、`class_name`、`extends` 和 `implements`，都写在成员之前。`class_name` 与 `extends` 先后不限，同 GDScript。后两者可与 `class_name` 同行，也可各占一行：

  ```gdscript
  class_name Player extends CharacterBody3D implements Combat.Damageable
  ```

- 文件中可声明内部类、struct、enum 和 interface，都属于该文件的类。内部类的写法与 GDScript 相同，`implements` 可写在 `extends` 能写的位置。

### 2.2 词法

- 缩进、注释、标识符、字面量和运算符沿用 GDScript，增减如下：
  - 新增上下文关键字：`pub`、`struct`、`interface`、`implements`、`async`、`None`。它们只在各自的语法位置上是关键字，在点号之后和其他位置是普通名字。
  - 单独出现的 `None` 总是空的 Option。【已知限制】以 `None` 为名、可用裸名访问的声明报编译错误，如变量、函数和匿名 enum 的值。
  - 新增短路返回的后缀运算符 `?`。
  - `null` 是 `None` 的别名，语义与 `None` 相同。提示建议新代码写 `None`。
  - 单引号和双引号字符串不能跨行，跨行须用三引号。
  - `##` 文档注释也用于 struct 及其字段、interface 及其成员。以 `###` 开头的注释是普通注释。

### 2.3 注解

- 【待定】`@tool` 和 `@export_tool_button` 让脚本在编辑器中运行。它们属于第一版，规则尚未定下。定下之前，它们报编译错误。
- `@warning_ignore` 接受本语言警告和提示的 snake_case 名字，提示与警告一样各有名字。不抑制任何诊断的名字给出提示 `unused_ignore`，包括迁移留下的 GDScript 警告名。
- 新增类级注解：`@parent(T)`、`@owner(T)`，用于向上访问；新增成员注解：`@readonly`，仅用于接口属性。
- `pub` 写在注解之后、`static` 之前。

### 2.4 声明的写法

- **struct**：`struct 名字:` 下用 `var` 声明字段，可带默认值；构造只接受具名实参，有默认值的字段可省略：

  ```gdscript
  struct Hit:
      var damage: int
      var point: Vector3
      var crit: bool = false

  var h = Hit(damage = 3, point = p)
  ```

- **enum**：分支可带命名的载荷字段，按名字传参构造。无载荷 enum 与 GDScript 相同：

  ```gdscript
  enum Command { IDLE, MOVE(target: Vector3), ATTACK(target: Node3D, power: int) }

  var c = Command.MOVE(target = Vector3.ZERO)
  ```

- **interface**：缩进块，可声明 `var`、`signal` 和无函数体的 `func`，方法不写冒号。属性默认要求可读写，只要求可读的标 `@readonly`；接口成员均为公开：

  ```gdscript
  pub interface Damageable:
      var hp: int
      signal died
      func take_damage(amount: int)
      func is_dead() -> bool
  ```

- **实现与继承接口**：类头写 `implements A, B`；接口继承接口写作 `interface Combatant extends Damageable, Healable:`。
- **pub**：关键字，用于成员以及 struct、enum、interface 和内部类，如 `@export pub var max_hp: int`、`pub static func make() -> Player`。
- **放置与命名**：struct、enum、interface 和内部类属于所在文件的类；本文件内直接写名字，其他文件写 `类名.名字`（如 `Combat.Hit`）。
- **函数类型**：沿用函数声明的形状，去掉函数名和参数名：

  ```gdscript
  var on_hit: func(int, Node3D)                 # 无返回值，也可写 -> void
  var score: func(Enemy) -> float
  var load_level: async func(String) -> Level   # 调用得到 Task[Level]
  var filter: (func(Enemy) -> bool)?            # 可空须加括号，否则 ? 属于返回类型
  ```

### 2.5 语句

- 语句的写法与 GDScript 相同。

### 2.6 GDScript 自有的常量和函数

- GDScript 自有的常量和工具函数沿用 GDScript，如 `PI`、`range()`、`len()`。同名的声明遮蔽它们。
- GDScript 已弃用的 `Color8`、`convert`、`type_exists`、`inst_to_dict` 和 `dict_to_inst` 报编译错误。诊断给出 Godot 文档建议的替代写法。
- `is_instance_of(v, T)` 的 `T` 有两种：
  - 写出的类名或 `TYPE_*` 常量，含义与 `v is T` 相同；
  - 静态类型为 `Script` 的值，按 `is` 的规则检测，但不收窄 `v` 的类型。
- 【已知限制】存在变量中的引擎类不能这样检测。

## 3. 类型

### 3.1 一览

| 类别 | 本语言新增或改变的类型 |
|---|---|
| 对象 | 非空 `T` 与可空 `T?`；`PackedScene[T]` |
| 本语言值类型 | struct、enum |
| 接口 | interface |
| 标准类型 | `T?`（Option）、`Result[T, E]`、`Task[T]` 与 `Settled[T]`、`ConversionError` |
| 函数 | 函数类型 |

数值、字符串、core 值类型、容器、`Callable`、`Signal` 和 `Variant` 沿用 GDScript 的类型，下面只写不同之处。

### 3.2 数值

- 没有窄整数类型，固定位宽运算用 `int` 上的回绕运算方法和位运算。

### 3.3 对象与可空

- 对象类型 `T` 非空；`T?` 为对象或 `None`。
- 对象被释放后：
  - 每次读取 `T?` 引用都做存活检查（与 `is_instance_valid` 一致），已释放对象读作 `None`。因此 `!= None` 收窄同时也是存活检查；
  - `T` 引用仍可持有已释放对象。读取、按 ObjectID 的 `==` 比较和普通传参不算访问。访问其成员是程序错误。对本语言实例或由本语言释放的对象，debug 构建在诊断中写明释放位置；
  - 把已释放对象传给声明为对象类型的引擎参数是程序错误。写入对象类型的引擎属性也是。传给对象元素 `Array[T]` 的方法（如 `append`、`erase`、`has`）也是。声明为 `Variant` 的参数不受此限（如 `is_instance_valid` 返回 `false`）；
  - 对任何静态类型，已释放对象的 `is` 为 `false`、`as` 为 `None`；
  - 方法运行中自身被释放或被卸下本语言脚本时，可继续运行至返回。此后访问成员或 `self` 是程序错误。

### 3.4 Option

- `T?` 即 Option，值为 `Some(v)` 或 `None`。显式包装写 `Some(v)`（如 `T??` 中的 `Some(None)`）。
- 操作均为方法，名字同 Rust：`unwrap`、`expect(消息)`（失败均为程序错误）、`unwrap_or`、`unwrap_or_else`、`map`、`and_then`、`or`、`or_else`、`filter`、`ok_or`、`ok_or_else`。没有 `!`、`??`、`?.` 运算符；唯一特殊语法是短路返回的后缀 `?`。判空用 `!= None` 与收窄。
- 嵌套 Option 保留多层（如 `T??`）：
  - 字面量 `None` 总是最外层的 `None`；
  - `?`、`unwrap`、`match` 和收窄每次剥一层，`.flatten()` 把 `T??` 合为 `T?`；
  - `Variant?` 同理：`None` 表示无值，NIL 是一个值。

  ```gdscript
  var r = enemies.pop_back()   # enemies: Array[Enemy?]，r: Enemy??
  match r:
      None: print("空数组")
      var e: print(e)          # e: Enemy?，可能是 None
  ```

### 3.5 Result

- `Result[T, E]` 用 `Ok(v)` 和 `Err(e)` 构造，成功值不隐式包装。
- 方法名同 Rust：`unwrap`、`expect`、`unwrap_or`、`unwrap_or_else`、`map`、`map_err`、`and_then`、`or_else`、`ok`、`err`；后缀 `?` 遇 `Err(e)` 返回 `Err(e)`。
- 丢弃 `Result` 报编译错误；有意忽略写 `.ok()`。

### 3.6 struct

- 值语义：赋值、传参、返回均浅复制，对象与容器字段复制引用。只存在于本语言内部。构造只接受具名实参。
- 跨引擎边界时按引擎边界的规则编码。解码按 `T.try_from` 的规则递归检查各字段及嵌套容器/struct，多余的键忽略。缺少的键取字段的默认值。字段没有默认值时解码失败。
- 可声明 `func` 和 `static func`。实例方法中 `self` 只读，给 `self` 字段赋值报编译错误。
- 方法中的名字先查 struct 自身，再按内部类的规则查外层类的无实例声明。
- 构造省略字段时，每次都求一次该字段的默认值。默认值中的名字按静态方法的规则查找，不能读字段。
- 字段和方法在 struct 可见处均可见。
- 修改 `s.x = 1` 作用于 `s` 所在存储位置：
  - 链式赋值沿链读出、修改并写回（`player.last_hit.damage = 3` 触发 `last_hit` 的 setter，`hits[0].damage = 3` 更新数组元素）；
  - 赋值先从左到右求出位置中的对象和下标，最后一个下标除外。再求右边的值和最后一个下标，与 GDScript 相同。然后连续完成读出、修改和写回，中间不挂起。引擎值类型（如 `position.x`）和复合赋值同样如此。修改接收者的方法调用（如 `points.append(x)`）先求接收者的位置，再求实参，然后读出、调用并写回；
  - 给临时值的字段赋值（如 `get_hit().damage = 3`）报编译错误；
  - 循环变量和 lambda 捕获是副本。修改 struct 循环变量的字段且后续不再读取该变量时，给出 `modified_copy` 警告。经容器或对象字段修改共享值不算。
- struct 默认提供逐字段 `==` 和包含全部字段的哈希，因此都可作 `Dictionary` 的键。哈希中的容器按内容计算。所有字段都有 `<` 时，它还按声明顺序提供词典序 `<`。对象字段没有 `<`，含对象字段的 struct 因此也没有。
- struct 和带载荷 enum 不经容器（`Array`、`Dictionary`）直接或间接包含自己时，报编译错误。诊断列出成环字段。经容器递归合法。
- struct 和带载荷 enum 的值经容器嵌套超过 100 层时，比较、哈希、转为字符串、编码、解码和深复制报程序错误。经共享容器包含自己的值因此也报错。

  ```gdscript
  struct Hit:
      var damage: int
      var crit: bool = false

      func total() -> int:
          return damage * 2 if crit else damage

  player.last_hit.damage = 3   # 读出、修改，经 last_hit 的 setter 写回
  get_hit().damage = 3         # 编译错误：修改临时值
  for h in hits:
      h.damage = 0             # 警告：modified_copy
  ```

### 3.7 enum

- enum 的显式值须是编译器求出的 `int`。匿名 `enum { A, B }` 声明类的 `int` 常量。
- 本语言的 enum 封闭，其值在入口检查和从容器读出时校验。引擎的 enum 开放：引擎会产生枚举外的值，后续版本也可能新增。因此 `MouseButton(x)` 不检查范围，对它的 `match` 必须带 `_`。
- `int` 转本语言 enum 须显式写 `State(x)`（非枚举值时报程序错误）或 `State.try_from(x)`：

  ```gdscript
  enum SpeedUnit { KMH, MPH }
  var unit := SpeedUnit.KMH
  unit = SpeedUnit((unit + 1) % SpeedUnit.size())
  unit = 1                                          # 编译错误；快速修复改为 SpeedUnit(1)
  ```

- enum 名作为值时，各方法带类型：`keys() -> Array[String]`、`values() -> Array[State]`、`find_key(v) -> String?`、`get(name) -> State?`。`State[name]` 的类型是 `State`，名字不存在时报程序错误。以 enum 名初始化的常量指同一 enum。
- 没有值 0 的 enum 没有默认值，其变量必须赋值。
- 带载荷的 enum：
  - 载荷字段必须命名（如 `MOVE(target: Vector3)`），只能经 `match` 解构，写 `c.target` 报编译错误；
  - `==`、`<` 和哈希按 struct 的规则，先按分支（`<` 按声明顺序），同分支再按字段；
  - 名字不能作为值使用，如 `Command.keys()`。以它初始化的常量照常指同一 enum；
  - 【已知限制】第一版不能声明方法。

### 3.8 容器

- **与无类型容器转换**：容器运行时类型恰为 `Array[int]` 时，`a is Array[int]` 为真并收窄（不复制）；`assign()` 则复制元素。经无类型视图向带类型容器写入不能赋给元素/键类型的值，是程序错误。经无类型视图用 `has`、`find`、`erase` 等查找这样的值，或 `assign()` 遇到此类元素，也是程序错误。写入 `Array[Node]` 的 `None` 在读出时也是程序错误。
- **表示与嵌套**：
  - 元素可由引擎表示时直接共享引擎的带类型容器；
  - 元素为本语言独有类型（struct、带载荷 enum 等）时使用自有表示。`Array(hits)`、`Dictionary(d)` 生成按引擎边界规则编码的无类型副本。反向用 `Array[Hit].try_from(v)`；
  - 嵌套容器（如 `Array[Array[int]]`）保留内层类型，在入口检查和读出内层容器时校验。
- **取值与方法**：
  - `pop_front()`、`pop_back()` 返回 `T?`（`T` 本身可空时返回 `T??`，空数组得外层 `None`）；
  - `pop_at(i)`、`front()`、`back()`、`min()`、`max()` 返回 `T`，空数组或越界下标均为程序错误；普通下标越界也是程序错误；
  - `Dictionary[K, V].get(k)` 返回 `V?`（缺键为 `None`），`get(k, default)` 返回 `V`，`d[k]` 缺键是程序错误；
  - 所有方法按元素自身的 `==` 和 `<` 工作。`resize` 填入元素默认值，内层容器填入空的带类型容器。`duplicate(true)` 深复制元素内部容器；
  - 排序方法在元素无 `<` 时报编译错误（如 `Array[Node]` 上提示 `sort_custom`）。【已知限制】`Variant` 元素排序时不可比较的一对算作不小于，与 GDScript 相同（`min`/`max` 返回 `null`）。`resize` 在元素无默认值时报编译错误（如 `Array[Node]` 上提示 `Array[Node?]`）；
  - 标准库在原生容器上提供接受带类型 lambda 的 `map`、`filter`、`sort` 等。
- **Dictionary 的键**：插入新键时存入一份副本，后续修改原值不影响键。副本中的容器逐层只读。副本内容器的共享与原值相同，包括容器引用自身。`keys()`、`for k in d`、`find_key()` 交出该只读副本，修改它是程序错误。诊断建议插入修改后的 `duplicate()`。查找操作不复制也不冻结参数。外部传入的 `Dictionary` 在本语言取得其容器键时，就地设为只读。

### 3.9 接口

- 名义接口：类须以 `implements` 显式声明，且只有本语言的类能实现接口。实现接口的成员须标 `pub`，方法不能是 `static`。
- 可声明方法、属性和信号，都是对实现类的要求。继承来的引擎属性可满足同类型的属性要求。未标 `@readonly` 的属性要求可读写。可读写的成员可满足 `@readonly`，而无 setter 的引擎属性只能满足 `@readonly`。在接口外使用 `@readonly` 报编译错误。
- 接口的信号是要求，与属性相同。类自己声明的、继承来的和引擎类的同名信号都能满足它。它须带 `pub`，参数类型须完全一致。
- 接口可 `extends` 多个父接口。子接口可以协变返回类型重声明父接口方法。父接口同名成员签名不兼容，或继承成环，报编译错误。
- 【已知限制】第一版的接口没有默认方法和带状态的 mixin，也不能是泛型。声明 `interface Source[T]:` 报编译错误。

### 3.10 泛型

- 第一版只有内置泛型类型（`Array[T]`、`Dictionary[K, V]`、`Result[T, E]`、`Task[T]`、`PackedScene[T]`），用户代码不能声明类型参数。

### 3.11 函数类型与 Callable

- 函数类型包含参数类型、返回类型（省略为 `void`）及是否 `async`。调用写作 `f.call(x)` 并静态检查；写 `f(x)` 报编译错误并给出 `f.call(x)`。
- 引擎调用的位置接受 `async` 函数，并作为根任务运行。这些位置是代码和场景中的信号连接，以及 `tween_callback` 等引擎 `Callable` 参数。
- 方法值和 lambda 交给引擎时成为直接指向目标方法的 `Callable`，不按名字查找。`f.bind(a, b)` 的类型为去掉末尾两个参数后的函数类型。
- 引擎传入函数类型位置的 `Callable` 签名未知，每次调用检查实参和返回值。【已知限制】第一版不能把静态类型为 `Callable` 的值显式转为函数类型，可用 `call()` 调用它。
- 方法值不使接收者保持存活，与引擎的 `Callable` 相同。接收者释放后，调用该方法值是程序错误。
  - 接收者为 `T.new()` 新建的 RefCounted 对象时，报编译错误；
  - 接收者只由临时值持有时，至少存活到所在语句结束，如 `g(make().hit, h())`。
- lambda 按值捕获，与 GDScript 相同。不同之处有两点：
  - 在 lambda 内给捕获的变量或其值类型字段赋值，报编译错误。共享可变状态须放在成员或容器中。因捕获不可重赋值，外层已收窄的类型在 lambda 内保持收窄；

  ```gdscript
  var hits := 0
  area.body_entered.connect(func(body): hits += 1)        # 编译错误：给捕获变量赋值
  var counter: Array[int] = [0]
  area.body_entered.connect(func(body): counter[0] += 1)  # 合法：容器共享
  ```

  - lambda 参数不能写默认值，写了报编译错误。

### 3.12 弱引用

- 【已知限制】第一版没有带类型的弱引用。需要时用引擎的 `weakref()` 和 `WeakRef`，与 GDScript 相同。

### 3.13 Variant

- **字面量**：进入 `Variant` 位置（含无类型容器元素）的数组和字典字面量保持无类型。无目标类型的数组和字典字面量由元素定型，如 `var a = [1, 2]` 得 `Array[int]`。
- 只能直接做 `==` 和 `!=`。其余操作（算术、`v.x`、`v.f()`、`v[i]`、`for x in v`）均报编译错误，并给出转换或 `is` 的快速修复。按名字动态访问须先转为 `Object`，再调 `get`/`set`/`call`。
- `Variant` 含 NIL：`v == None` 对 NIL 和已释放对象为真，`v is int` 对 NIL 为假，`v as Node` 对 NIL 得 `None`。`Variant?` 多一层 `Option`（如 `Dictionary[String, Variant].get("hp")` 缺键得 `None`，值为 `null` 得 `Some(NIL)`）。

  ```gdscript
  var data = JSON.parse_string(text)   # Variant
  data.hp                              # 编译错误：须先转换
  if data is Dictionary:
      var hp = int.try_from(data.get("hp", 100)).unwrap_or(100)
  var n = data as Node                 # Node?
  ```

- 结构固定的数据解码为 struct。整棵树在入口处一次检查，出错时写明字段：

  ```gdscript
  struct Consumable:
      var rest_times: int
      var transformed: bool

  struct InitMessage:
      var consumables: Dictionary[String, Consumable]
      var level: int = 1

  var message = InitMessage.try_from(JSON.parse_string(text))   # Result[InitMessage, ConversionError]
  ```

### 3.14 可赋值

- 期望类型 `T` 的位置只接受可赋给 `T` 的值。这些位置包括赋值、传参、返回值和容器元素。
- 可赋给 `T` 的值只有以下几类：
  - 同一类型的值；
  - 对象：子类赋给基类，实现接口的类和子接口赋给接口。非空可赋给可空，反之不行；
  - `None` 赋给 `T?` 或 `Variant`，后者得 NIL。`null` 赋给非空 `T` 时，快速修复把类型改为 `T?`；
  - 可赋给 `T` 的值也可赋给 `T?`，隐式包装为 `Some`，并按目标类型补层（`T?` 赋给 `T??` 得 `Some`）；
  - 隐式转换，只保留 GDScript 中不丢信息、含义不变的几种：
    - `int` 转 `float`，无载荷 enum 的值转 `int` 或 `float`；
    - `Vector2i`、`Vector3i`、`Vector4i`、`Rect2i` 转对应的浮点类型；
    - `String` 与 `StringName`、`NodePath` 互转；
  - 函数值：同为 `async` 或同为普通函数，参数逆变、返回协变。函数值也可赋给 `Callable`；
  - `PackedScene[S]` 赋给 `PackedScene[T]`，其中 `S` 可赋给 `T`；
  - 元素可由引擎表示的带类型容器，赋给同种无类型容器，得到同一容器的无类型视图；
  - 任何值赋给 `Variant`，struct 和带载荷 enum 按引擎边界的规则编码。例外有两种：
    - 元素为本语言独有类型的容器，须显式写 `Array(hits)`、`Dictionary(d)`；
    - `T??` 和 `Variant?`，因为 `None` 和内层的 NIL 无法区分。快速修复提供 `.flatten()` 或 `.unwrap_or(null)`。
- 其余情况须显式转换，否则报编译错误，快速修复补上转换。例如：
  - 带类型容器、`Result` 和 `Task` 对类型参数不变，`Array[Node2D]` 不能赋给 `Array[Node]`；
  - `float` 转 `int` 写 `int(x)`，还有 `Vector2i(v)`、`bool(n)`、`Color(s)`；
  - 无类型容器和 `Variant` 赋给带类型的位置，须显式转换或经 `is` 收窄。

## 4. 声明

### 4.1 类与继承

- 类名使用引擎的扁平全局类表，编译器检查与本语言、GDScript 插件及 C# 类名的重名。【已知限制】第一版没有命名空间。
- 类可以继承引擎类或本语言的类。【已知限制】两个方向的跨语言继承均不支持。
- **裸名解析**：类中的裸名先依次查找局部变量、本类与本语言基类的声明。再查找所继承引擎类及其祖先的成员：
  - 属性、方法、信号和常量；
  - enum 值，类型为该引擎 enum；
  - enum 名，可用于类型标注；bitfield 值与其余整数常量为 `int`。
- 本类声明的变量、常量、信号、匿名 enum 值或内部类，与引擎基类的常量、enum 值或 enum 名同名时报编译错误。内部类中，裸写的名字若既是外层类的，又是引擎基类的上述名字，报编译错误。须写 `Outer.X` 或 `Area3D.X`。
- `new` 是编译器为类提供的构造函数名。类和接口中声明名为 `new` 的函数，报编译错误。
- **`@abstract`**：抽象方法无冒号、无函数体。含抽象方法的类须标 `@abstract`。具体子类须实现全部抽象方法。抽象类不能构造，也不能挂在场景节点上（标在场景上报错）。经 `super` 调用抽象方法或给 `static` 方法标 `@abstract` 报编译错误。

  ```gdscript
  @abstract
  class_name BattlerAction extends Resource

  @export pub var label: String
  @abstract pub async func execute()
  ```

- **内部类**：
  - 可声明文件类能声明的一切（含嵌套内部类），但 `class_name` 及 `@abstract` 以外的类级注解只属于文件类；
  - 可继承同文件内部类（写裸名）、其他类的内部类（`Other.Inner`）或外层类，省略 `extends` 时默认继承 `RefCounted`；
  - 【已知限制】两个文件不能互相继承对方的类，经其他文件间接也不行，否则报编译错误。例如 `A.X extends B.Y` 时，B 中的类不能再继承 A 的类；
  - 名字先查自身与继承链，再由近及远查外层类的无实例声明。无实例声明指常量、类型、静态函数和静态变量。访问外层实例成员报编译错误，并说明没有外层实例；
  - 可经 `Outer.Inner` 或外层实例（如 autoload）访问；
  - 运行时是无资源路径、无全局名的独立脚本（`get_script()` 返回它），不能直接挂在场景节点上，可经 `set_script()` 设置。

### 4.2 可见性

- 不带 `pub` 的成员在声明文件内和子类中可见，其他类不可见。struct、enum、interface 和内部类同样适用。声明文件包括其中的内部类。子类可以读写、调用和覆盖这些成员。带 `pub` 的成员对所有代码可见。
- 覆盖须保持可见性不变；实现接口的成员须为 `pub`；`pub` 成员签名中出现非 `pub` 类型报编译错误。
- `pub` 接口的父接口须为 `pub`，否则报编译错误。
- `new(...)` 的可见性取继承链上最近的 `_init`。无 `_init` 时随处可 `new()`。`_init` 不带 `pub` 时，仅本文件和子类可调用 `new(...)`。
- 不带 `pub` 的必须赋值成员只能由本文件和子类赋值。这些文件中都没有给它赋值的代码时，在声明处报编译错误。按名字反射能看到哪些成员，见“反射暴露的成员”一节。

### 4.3 成员变量

- 有初始化式的 `var` 可按局部变量规则推断类型。如 `var speed = 5.0` 为 `float`，`@onready var label = $Label` 按场景推断。推断结果用于悬停、补全和反射。无初始化式，或初始化式为空容器、`None` 等无法确定类型时，必须标注类型。
- 无初始化式的成员：
  - 数值、`bool`、字符串、core 值类型、容器、`Callable`、`Signal` 和含值 0 的无载荷 enum 取各自默认值。`T?` 取 `None`，`Variant` 取 NIL；
  - struct 在所有字段均有默认值时取默认值，否则属于必须赋值的成员。非空对象引用、带载荷 enum、不含 0 的 enum 和函数类型也是必须赋值的成员。
- `@export` 沿用 GDScript，但有三点不同。导出的变量须带 `pub`。导出场景写 `PackedScene[T]`。非空对象导出必须赋值。
- struct 和带载荷 enum 可以导出，存储时按引擎边界的规则编码。场景和 `.tres` 中存的值在编译时按类型检查。
  - 字段中任何一层含 `Node` 时，导出报编译错误。
  - 【已知限制】检查器暂以通用的 Dictionary 编辑器编辑它们。

### 4.4 属性访问器

- 内联 `set(value):`、`get:` 与具名 `set = f`、`get = g` 沿用 GDScript。访问器内部，属性名指存储字段。类内其他位置的读写均经访问器。初始化式不经 setter。setter 和 getter 不能是协程。

### 4.5 常量

- `const` 可声明在类或函数中。类常量在本类、子类和静态函数中用裸名。其他类写 `Player.MAX`，也可经实例写 `self.MAX`、`p.MAX`。类型省略时按成员变量的规则推断，如 `const SPEEDS = [1.0, 2.0]` 为 `Array[float]`。值为 `None` 时须写 `T?`。
- 初始化式须为常量表达式，不含方法调用、lambda、`self` 和变量。常量表达式由以下部分组成：
  - 字面量、常量、enum 值、引擎类与内置类型常量；
  - `preload`、运算符与条件表达式；
  - 数组与字典字面量、内置类型构造与转换；
  - 引擎 math 类工具函数；
  - 常量的下标和字段。
- **求值划分**：
  - 编译器在编译期求出 `int`、`float`、`bool`、三种字符串及其运算转换；
  - 编译器也求出返回 `int`/`bool` 的数学函数和容器字面量结构；
  - 引擎值类型（向量、`Color` 等）的构造与运算在类加载时求值一次；
  - 返回 `float` 的数学函数及含 `float` 的 `**` 也在类加载时求值一次。
- **编译期值检查**（表中“已知”指由编译器求出，保证加载期求值绝不失败）：

  | 情形 | 编译期要求 |
  |---|---|
  | `int` 和整数向量的 `/`、`%`，`posmod` | 除数（向量逐分量）已知且不为 0 |
  | `<<`、`>>` | 两边已知且非负，移位量小于 64 |
  | 两个 `int` 的 `**` | 两边已知；指数为负时底数不为 0 |
  | `float` 转 `int`（`int(x)`、`floori(x)` 等） | `x` 已知，非 NaN/无穷，且在 `int64` 范围内 |
  | 对常量取下标或键 | 下标或键已知，不越界且键存在 |
  | 字典字面量的键 | 已知且不重复 |
  | `Color(s)` | `s` 为十六进制字面量（颜色名报编译错误，快速修复改为 `Color.RED` 等常量） |

- 注解参数、enum 值和上表要求“已知”的位置只接受编译器求出的值。【已知限制】由加载期 `float` 取整的 `int`（如 `floori(sin(X) * 100)`）不能用于这些位置。常量表达式中也不能使用：
  - `%` 格式化、`is`、`as`、`in`；
  - GDScript 自有工具函数（`len`、`char`、`ord`）；
  - debug 构建检查实参值的 `Basis`/`Quaternion` 构造与乘法。
- 常量中的数组和字典逐层只读。直接赋值或调用修改方法报编译错误，经其他变量修改报程序错误。修改常量在热重载时按结构变化处理。

### 4.6 函数

- 函数签名不推断。除覆盖和有期望类型的 lambda 外，参数必须标注类型。非覆盖函数省略返回类型表示 `void`，此时返回值报编译错误。
- **默认值**：参数可带默认值。省略实参时，被调函数每次在开头求出默认值，可读 `self` 和前面的参数。
  - 经基类调用覆盖方法时，用覆盖方法自己的默认值。
- **覆盖**：覆盖引擎虚函数或基类方法可省略参数和返回类型。省略的类型取自被覆盖声明，如 `func _process(delta):` 中 `delta` 为 `float`。写出的类型允许返回协变和参数逆变，参数名可不同。
  - 覆盖须接受被覆盖方法接受的每种实参个数。因此可多出带默认值的参数或剩余参数，不能去掉剩余参数。
- **剩余参数**：须为末尾无默认值的 `...args: Array` 或 `...args: Array[T]`。调用处逐个检查实参，从外部调用由入口检查校验。
  - 覆盖之外，裸写 `...args` 报编译错误。
  - 调用处无展开写法，已有数组用 `callv`。
  - 带剩余参数的方法值和 lambda 是签名未知的 `Callable`。
- 普通调用按位置传参。具名传参只用于 struct、带载荷 enum 构造和 `instantiate(...)`、`new(...)` 的必须赋值成员。
- 局部变量可由初始化式推断类型。lambda 参数在有期望函数类型时取自期望类型，否则须标注。lambda 返回类型由函数体推断。
- 同一函数中，局部变量、`for` 变量和 `match` 的 `var x` 绑定不能与外层块的局部变量或参数同名。并列块可同名。lambda 函数体视为外层函数的内层块。但 lambda 参数可与外层局部变量同名。
- 协程（含 lambda）须在签名写 `async func`。

### 4.7 信号

- 信号参数必须标注类型，如 `pub signal died(cause: String)`。
- 代码中的 `emit`、`connect` 和场景中的信号连接均静态检查参数个数与类型。引擎信号按 ClassDB 定型，未审核对象参数按非空。

### 4.8 静态成员

- 编辑器内不做静态初始化。类加载时先基类后派生类，按声明顺序初始化 `static var`，不经 setter。然后调用无参无返回的 `static func _static_init()`，随后依次初始化内部类。
- `static var` 由声明它的类与子类共享，可带访问器。它不能标 `@export` 或 `@onready`。无初始化式时按成员变量的规则取默认值，无默认值的类型（如非空对象类型）报编译错误。未标 `@static_unload` 时，含静态变量的类在运行期间保持加载。
- 静态变量和静态方法在本类、子类和内部类中用裸名。其他类写 `Registry.count`、`Base.label()`。它们也可经实例或值访问：
  - `pub` 静态变量可由 GDScript 经类和对象按名字读写；
  - 本语言与 GDScript 类的对象按动态类型自下而上查找同名静态方法；
  - 引擎类与内置类型直接调用该类型的方法。

  ```gdscript
  var b: Base = Sub.new()
  b.label()                             # Sub.label()
  Base.label()                          # Base.label()
  var hue := Color.RED.from_hsv(0.5, 1.0, 1.0)
  ```

- 静态初始化不能 `await`。读取其他类静态成员时先完成该类初始化。直接读取本类后声明的静态变量报编译错误。通过调用读写本类未初始化完的静态变量，报程序错误。跨类静态初始化成环也报程序错误，诊断列出依赖链。
- 热重载时增删或改静态成员声明需重新运行。只改函数体、初始化式或 `_static_init` 时保留现有静态变量值，不重跑静态初始化。

## 5. 表达式与语句

### 5.1 运算

- 移位左操作数为负、移位量为负或达到 64 是程序错误。常量中为编译错误。
- `x in c` 的 `x` 须可赋给元素或键类型。如 `1 in scores` 对 `Array[float]` 合法，`1.5 in ids` 对 `Array[int]` 报编译错误。`name in obj` 在已释放对象上是程序错误。
- 两个 `int` 的 `**` 精确计算，溢出同 `*` 回绕。负指数按 `1 / (a ** -b)` 向零截断（`2 ** -1` 得 0，`(-1) ** -3` 得 -1）。`0` 的负指数次方是程序错误。
- `%` 格式化时，格式串与值不符（如 `"%d %d" % [1]`）是程序错误。
- 给 String 的字符赋值（如 `s[i] = c`）是编译错误。删除和替换字符改用 `erase` 和 `insert`。
- 值转成文本时（如 `str`、`print` 和 `%`），先按引擎边界的规则编码，再由引擎转换。因此 struct 显示为字典，`None` 显示为 `<null>`。程序错误中的值按本语言写法显示，如 `Hit(damage = 3)`。

### 5.2 比较

- `!=` 恒为 `==` 的否定。`==` 与 GDScript 有以下不同：
  - 对象按 ObjectID 比较同一性，两个不同的已释放对象不相等。互不为子类的两个类类型之间比较报编译错误，涉及接口时允许；
  - `Task[T]` 也按同一性比较，同一个任务才相等，可作 `Dictionary` 的键；
  - `Option`：`None == None` 为真，两个 `Some` 比较内部值。层数少的一侧先包装到同层再比较，如 `int?` 与 `3`、`T?` 与 `T??`。非 `Option` 且非 `Variant` 的值与 `None` 比较报编译错误，快速修复改为 `T?` 或 `is_instance_valid(x)`；
  - 容器逐元素/逐键比较，元素类型不同则不匹配，如 `[1] == [1.0]` 为假。【已知限制】含 `NaN` 的容器在 `==`、查找和字典键中如何匹配，第一版未统一，结果随元素类型而不同。静态元素类型不同的容器比较（如 `Array[int] == Array[float]`）报编译错误。struct 与带载荷 enum 逐字段匹配；
  - 对 `Variant`，`v == None` 在 NIL 和已释放对象上为真。
- 大小比较：
  - `Array` 在元素有 `<` 时按字典序比较。在第一个按容器 `==` 不匹配的位置，结果取该处元素的 `<`。各处都匹配时，短的小；
  - `T?` 在 `T` 有 `<` 时令 `None` 小于一切 `Some`，且**不**隐式包装：`hp: int?` 时 `hp < 3` 报编译错误；
  - struct 和带载荷 enum 按 struct 与 enum 两节的规则定序；
  - 以上三类的 `>`、`<=`、`>=` 由 `<` 和 `==` 导出：`a > b` 即 `b < a`，`a <= b` 即 `a < b or a == b`。其余类型的四个运算都按运算表。

### 5.3 转换

- `x as T` 只用于对象类型，返回 `T?`。不匹配、已释放或 `Variant` 内非对象时均得 `None`。`x` 静态类型已是 `T` 时返回 `T` 并提示多余。向下转型断言写 `(x as T).unwrap()`。先 `is_in_group` 再转型的代码，快速修复建议改用 `is`。
- `as` 转为内置类型或 enum 均报编译错误。快速修复提供两种改法：保留构造/显式转换的 `T(x)`（如 `int("5")`、`int(2.7)`、`State(x)`），以及严格转换 `T.try_from(x)`。
- `T.try_from(v)` 返回 `Result[T, ConversionError]`，规则与 struct 字段解码一致。运行时类型须与 `T` 相符。转 `int` 时接受恰为整数且在 `int64` 范围内的 `float`。转本语言 enum 时在值域外返回 `Err`。
- `ConversionError` 是内置 struct，只有字段 `message: String`。内容与该值未通过入口检查时的报错相同，如 `Hit.damage: expected int, found String`。

### 5.4 收窄

- 局部变量和参数在 `!= None`、`is` 和提前返回后收窄，也在 `a if c else b` 的分支及 `and`/`or` 右侧按条件收窄。重新赋值后按新值重算。lambda 捕获保留创建处的收窄。收窄不免除运行时的对象存活检查。
- `Callable` 类型的局部变量和参数以函数值初始化或赋值后，收窄为该函数类型。此后调用它按函数类型静态检查，出错时与调用者属于同一条调用链。
- 成员变量不收窄，须先读入局部变量。
- 没有真值判断：条件及 `not`、`!`、`and`、`or` 的操作数须为 `bool`。非 `bool` 的 `if x:` / `if not x:` 报编译错误，快速修复按类型改为 `x != None`、`x != 0`、`x != ""` 等。

### 5.5 条件表达式

- 只有 `a if c else b`，其中 `c` 须为 `bool`。
- 有期望类型时，两支都按它检查。没有时，结果类型依次取：
  1. 一支能赋给另一支时，取较宽的一支；
  2. 一支为 `None`、另一支为 `T` 时，取 `T?`；
  3. 两支都是类时，取最近的共同基类。
- 都不成立时报编译错误，提示写出类型。实现同一接口而基类不同的两个类得共同基类，要得到接口须写出类型。

### 5.6 match

- 值模式是字面量、常量表达式、标识符或属性访问（`A.B`），同 GDScript。非常量的模式在运行时比较，不计入穷尽。
- 值模式的类型须与被匹配值的静态类型相同，否则报编译错误。以下情况除外，与 GDScript 能匹配的一致：
  - 被匹配值为 `Variant` 时，不限类型；
  - `String` 与 `StringName` 互相匹配；
  - 被匹配值为对象时，模式的类型可以是其子类；
  - 被匹配值为 `int` 时，模式可以是不带载荷的 enum 值。
- `var x`、`_`、带 `..` 的数组和字典模式、逗号多模式和 `when` 守卫同 GDScript。数组和字典模式另有以下规则：
  - 数组模式只用于 `Array[T]` 和 `Variant`，字典模式只用于 `Dictionary[K, V]` 和 `Variant`。用于其他类型报编译错误；
  - 其中元素和值的模式以元素类型和值类型为被匹配值的类型。如 `Array[int]` 上 `[var x]` 的 `x` 为 `int`；
  - 字典模式的键须为常量表达式，且同 `in` 一样须可赋给键类型。
- 新增模式：
  - enum 分支与 struct：`Command.MOVE(target = 模式, ..)`、`Hit(damage = var d, ..)`；`var 字段` 为 `字段 = var 字段` 的简写，未列出的字段须写 `..`，可嵌套；
  - 内置构造：`Ok(var v)`、`Err(var e)`、`Some(var x)`、`Finished(var v)`、`Cancelled(var r)`，按位置书写，不带类型前缀；
  - `Option` 另有 `None` 和收窄为 `T` 的 `var x`（`None` 配合 `Some(var x)` 或 `var x` 即为穷尽）。

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

- 对本语言 enum、`Result` 和 `Option` 的 `match` 必须穷尽或带 `_`。对开放的引擎 enum 必须带 `_`。带 `when` 的分支不计入覆盖。其他类型不要求穷尽。本语言 enum 的穷尽 `match` 计为覆盖所有返回路径。
- 【已知限制】第一版无类型模式（用 `if x is T`）、无 `match` 表达式、逗号多模式不可绑定。

### 5.7 循环

- 循环变量标注的类型须为元素类型或其父类型。遍历 `float` 或向量报编译错误（快速修复改用 `range(...)`）。
- `range(...)` 的参数须为 `int`，结果是 `Array[int]`。`for` 遍历它时只计数，不建数组。
- `range(...)` 的步长为 0 是程序错误，在 `for` 中也是。步长已知为 0 时报编译错误。
- 本语言或其他语言的类定义了 `_iter_init`、`_iter_next` 和 `_iter_get` 时，可以遍历它的对象。元素类型是 `_iter_get` 的返回类型。三者缺一报编译错误。
- 这三个方法是 Object 的虚函数，按引擎的协议调用：
  - `_iter_init` 和 `_iter_next` 每次收到一个新的单元素 `Array`，元素是遍历状态，开始时为 `null`；
  - 二者返回 `true` 时，以当时的状态调用 `_iter_get` 得到下一个元素，返回 `false` 时遍历结束；
  - 二者返回时，这个 `Array` 的长度不为 1 是程序错误。

## 6. 初始化与生命周期

### 6.1 成员的初始化

- 子类 `_init` 签名不受覆盖规则约束。
- 实例和静态成员的初始化式均按声明顺序直接写底层字段，不经过 setter。同时有初始化式和 setter 时给出提示。直接读取本类后声明的成员报编译错误。派生状态在 `_ready` 或 `_static_init` 中计算。

### 6.2 必须赋值的成员

- `_init` 可带参数，`new(...)` 按位置传给它，与 GDScript 相同。引擎创建对象时不传参数，如场景、`instantiate(...)` 和 `duplicate()`。这些途径的构造参数表达为无默认值的非空必须赋值成员。
- `Node` 的必须赋值成员须在所在场景就绪完毕前由以下之一提供：
  - **`_init`**：在每条路径上给它赋值，如由参数赋值；
  - **场景**：导出的成员由场景提供；
  - **创建者**：`instantiate(...)` 和 `new(...)` 按名字接收成员并在返回前赋值，如 `BulletScene.instantiate(shooter = self, target = enemy)`。`PackedScene[T].instantiate(...)` 漏传场景资源规则要求的成员报编译错误。`new(...)` 漏传在运行时检查；
  - **祖先**：同一场景中的祖先节点在自身 `_ready` 中赋值。赋值经场景模型可见的 `$Child.target = v` 或 `@onready` 引用进行。不可见的动态配置须标为 `T?`。
- 编译器检查以下写法给出的值，其余赋值在就绪时检查：
  - `_init` 中每条路径上的赋值；
  - 场景存的值；
  - `new(...)` 和 `instantiate(...)` 的具名实参；
  - 祖先在 `_ready` 中经 `$Child.target = v` 或 `@onready` 引用的赋值。
- 就绪时仍未赋值的成员报程序错误，并提示改用上述写法，以便在编译时检查。
- 检查的时机见就绪顺序。子节点在自身 `@onready` 初始化式和 `_ready` 中读取由祖先赋值的成员，报编译错误。就绪前读取尚未赋值的成员报程序错误并写明成员名。
- `_init` 直接读取必须赋值的成员，而此前 `_init` 未在每条路径上给它赋值时，报编译错误。经调用间接读取报程序错误。
- `Resource` 无就绪阶段：`.tres` 静态检查，代码中 `new()` 的在读取时检查。
- `_init` 有必填参数时，经 `set_script` 等无参途径附加该脚本报程序错误。此时不运行 `_init`。场景或 `.tres` 使用这样的脚本报编译错误。
- autoload 比场景活得久，引用场景节点的成员须写 `T?`。【已知限制】第一版没有就绪后赋值的非空成员。

### 6.3 `@onready`

- `@onready` 与 `@export` 同用报编译错误。初始化式中含 `await`，或在 `_init`、`_enter_tree` 中直接读取 `@onready` 成员，也报编译错误。经调用间接读取尚未初始化的 `@onready` 成员报程序错误，并写明成员名。

### 6.4 就绪顺序

- 本语言的 `Node` 依次经过以下阶段，各项检查在所列阶段进行：
  1. 创建：初始化式按声明顺序赋值，然后运行 `_init`。`new(...)` 和 `instantiate(...)` 的具名实参随后按书写顺序经 setter 赋值，与场景给导出赋值相同；
  2. 挂到父节点（`NOTIFICATION_PARENTED`）：检查 `@parent(T)`。脚本挂到已有父节点的节点上时，在挂上时检查；
  3. 就绪：先检查由场景或创建者提供的必须赋值成员，再初始化 `@onready` 成员，然后运行 `_ready`；
  4. 整个场景就绪后，即 owner 发出 `ready` 时：检查由祖先赋值的必须赋值成员。
- 场景中有 owner 的节点，其未导出的必须赋值成员由祖先赋值，其余的由场景或创建者提供。祖先在 `_ready` 中 `await` 之后才赋值时，第 4 步的检查报程序错误。
- 子节点在 `_ready` 中读到父节点尚未初始化的成员时，报程序错误并说明原因。运行时挂到已就绪父节点下时读取合法，因此不在编译期一概禁止。诊断给出两种改法：
  - 改由父节点 `_ready` 配置子节点；
  - 子节点声明 `async func _ready()` 与 `@parent(T)`，并 `await get_parent().ready`。

### 6.5 就绪前的 setter

- setter 立即执行。就绪前访问 `@onready` 成员或子节点报程序错误，并写明成员或节点名。需要同步节点的 setter 应先存值、在 `is_node_ready()` 为真时同步，并在 `_ready` 中同步一次。setter 内写 `await ready` 报编译错误。
- 无存储字段、getter 直接读子节点的属性在就绪前读取报程序错误并写明未就绪节点。从未写入的属性在就绪时不跑 setter。导出属性默认值不经同步节点的 setter 时给出提示。

### 6.6 析构

- `RefCounted` 在 `PREDELETE` 期间可正常读写自身字段、调用自身方法，不重加引用。处理 `PREDELETE` 的分支中，把 `self` 当作值使用报编译错误。当作值使用指传参、存储、返回、比较等，`self.x` 和 `self.f()` 除外。在该分支中使用 `await` 也报编译错误。

### 6.7 引用环与泄漏

- 编译器对明显引用环给出警告：
  - 捕获 `self` 的闭包存入 `self` 的成员、信号连接或容器；
  - 两个 `RefCounted` 类以强引用字段互指。
- `Node` 由 `new()` / `instantiate()` 创建后，在某路径上未入树、未返回、未保存也未释放时，给出警告。`remove_child` 的结果未保存也未释放时，也给出警告。
- debug 构建退出时，列出本语言创建而仍存活的节点和对象。
  - 互相引用成环的 RefCounted 也算在内。
  - 按创建位置分组，每组写明类和个数，最多列 5 处。

### 6.8 释放时机

- 临时值和局部变量按作用域释放所持的引用，不在最后一次使用后提前释放。所以析构时解锁一类的守卫写法成立：

  ```gdscript
  func save():
      var guard := FileLock.new(path)   # 函数返回时才释放
      write_all()
      if load_config().verbose:         # load_config() 的结果在进入块之前释放
          log_all()
  ```

- 临时值在所在语句结束时释放。复合语句的头部例外：
  - `if`、`elif`、`while` 的条件和 `match` 的被匹配值，其临时值在判定后即释放，不带入块中；
  - `for` 遍历的值保留到循环结束，头部的其他临时值在进入循环体之前释放。
- 局部变量在离开所在的块时释放。循环体每轮结束也算离开。
- 语句中的 `await` 挂起时，该语句的临时值仍被持有。`self` 除外，见协程的释放规则。
- GDScript 对代码已访问不到的值释放得更晚，如上一轮的循环局部变量和 `match` 的被匹配值。这属于语法硬规则的例外。

## 7. 场景模型

### 7.1 节点定型

- 编译器收集脚本所在的全部场景结构（含继承场景与实例修改），为 `$X`、`%X` 和字面量路径 `get_node` 定型。
- 不同场景结构给出不同类型时报编译错误，须赋给标注了共同基类或接口的变量（如 `@onready var weapon: Node3D = $Weapon`）。
- 找不到的路径报编译错误并标在缺少该节点的场景上；哪些场景必须有该节点见下一节。

### 7.2 场景之间结构不同的情况

- **基类要求、由继承场景提供的子节点**：按场景使用方式检查，与必须赋值的导出相同。仅作继承基础的场景可缺该节点。作为根使用或被实例化的场景必须有该节点。未被任何地方引用的场景缺节点只给警告。`$X` 类型取自含该节点的场景。
- **可选的子节点**：字面量路径 `get_node_or_null(...)` 类型为 `T?`，缺该节点的场景不报错。`$X` 在某个被使用的场景中缺失时，快速修复改为 `get_node_or_null`：

  ```gdscript
  @onready var trail_right = get_node_or_null("Container/TrailRight")   # GPUParticles3D?
  ```

- **运行时添加的子节点**：不按路径定型。任何场景（含未引用场景）中都没有的 `$X` 报编译错误。快速修复建议在 `add_child` 时存入带类型成员，如 `var tetromino: Tetromino?`。确需按名查找时，用 `get_node_or_null("X")` 得 `Node?`，再以 `as` 转换。在任何场景中都不存在的字面量 `get_node_or_null` 路径给出警告。确为运行时节点时，用 `@warning_ignore` 关闭警告。

### 7.3 向上访问

- autoload 用全局名访问，类型为其脚本的类。字面量路径 `/root/X` 报编译错误，快速修复改为 `X`。
- `X` 不是 autoload 时，`$"/root/X"` 与 `get_node("/root/X")` 相同，类型为 `Node`，转具体类型须显式转换。这类节点由代码在运行时挂到根下。
- 父节点和 owner 要求写作类级注解 `@parent(T)`、`@owner(T)`。声明后 `get_parent()`、`$".."` 和 `owner` 类型为 `T`。未声明时 `get_parent()` 和 `owner` 为 `Node?`，`$".."` 和 `get_node("..")` 为 `Node`。此时写 `get_parent().x` 或 `owner.x` 报编译错误。快速修复按场景推断公共类型，补上 `@parent(T)` / `@owner(T)`，或在调用处加 `.unwrap()`：

  ```gdscript
  @owner(Combat.Combatant)
  class_name HealthComponent extends Node

  func _ready():
      current_health = owner.init_health
  ```

- **静态检查**：在放置该脚本的场景上进行。`@parent(T)` 检查场景内父节点。场景根被实例化到外层场景时，检查外层父节点。在主场景上，检查 `Window` 根视口。不符时标在该场景报错。其他作为根使用的场景只在运行时检查。`owner` 为包含该节点的场景根节点；场景根被实例化到外层场景时，为外层场景根。作为根使用的场景其根节点无 `owner`，挂有 `@owner` 脚本时在该场景报错。
- **运行时**：`@parent(T)` 按就绪顺序检查。无父节点时调用 `get_parent()` 报程序错误。`@owner(T)` 在每次读取 `owner` 时检查非空且符合 `T`。失败报程序错误，并写明要求与实际 `owner`。移动节点或 `set_owner()` 时不检查。
- `get_owner()` 和 `get_node_or_null("..")` 类型恒为 `Node?`，供声明了要求的代码判断父节点或 `owner` 是否存在。

### 7.4 场景资源

- `preload` 和字面量路径 `load` 按资源实际内容定型。场景得 `PackedScene[T]`，`T` 为根节点类型或根节点脚本的类。`preload` 脚本得其类。
- 导出场景写作 `PackedScene[T]`，由编辑器检查所填场景，`instantiate()` 得 `T`。
- `instantiate(...)` 必须传入的成员取决于场景是否静态已知：
  - 静态已知（`preload`、字面量路径 `load` 及以它们初始化的常量）：须传入该场景未提供的必须赋值成员；
  - 其余 `PackedScene[T]`（导出、变量、参数、返回值）：须传入 `T` 中未导出的必须赋值成员（导出的由场景自身提供）；
  - 静态已知的场景可隐式转为后一种 `PackedScene[T]`，如填入导出、赋值、传参、返回和作容器元素。转换时检查根节点实际类中未由场景提供的必须赋值成员。它们只能是 `T` 中未导出的必须赋值成员，否则在转换处报编译错误。
- 运行时路径的 `load` 得 `Resource`，可空性按引擎 API 的规则。其场景 `instantiate()` 得 `Node`，转具体类型须显式转换。

### 7.5 必须赋值的导出

- 非空对象类型的导出必须赋值；可能缺值的写 `T?`。
- 按场景使用方式检查：
  - 被其他场景实例化或继承时，沿实例和继承链任一层赋值均算数。每个实例单独检查，并标在所在场景上；
  - 作为根使用时（主场景、字面量路径 `load`/`preload`、填入导出的 `PackedScene[T]`），检查场景本身。代码中 `instantiate(...)` 传入的值也算数，漏传在调用处报编译错误；
  - 未被任何地方引用的场景只给警告。
- 三个检查时机：
  - **编辑时**：允许清空，Inspector 字段和场景树节点立即标编译错误但不阻止操作，重赋值后消失；
  - **运行时**：标在场景上的缺值错误不阻止编辑器内运行。仍缺值的节点按就绪顺序检查，报程序错误并写明成员名；
  - **导出时**：确定缺值的场景经导出插件报错，导出窗口显示失败并写明场景与成员名。仅警告不报错。CI 用无界面分析命令阻断。【已知限制】引擎导出器不会因插件报错中止写出产物，命令行导出退出码仍为 0。
- 远程检查器向非空成员写 `null` 触发入口检查失败，即程序错误，写入被拒绝且原值不变。

### 7.6 资源共享

- 资源共享语义不变。编译器识别来自缓存（`load`、`preload`、导出属性）及未设为“局部于场景”的内嵌子资源。
- 对已知共享的资源写属性或调用修改方法时给出警告。警告列出共享位置，并提供两个快速修复：设为局部于场景，或修改前 `duplicate()`。有意共享时用 `@warning_ignore` 关闭。运行时替换的资源不追踪。
- `duplicate()` 返回接收者的静态类型；复制深度用 `duplicate_deep(模式)` 写明。
- `preload`、导出的 `PackedScene` 与资源之间的引用环报编译错误，快速修复将其中一条边改为运行时 `load`。

### 7.7 场景中的连接和路径

- 场景中保存的信号连接检查目标方法是否存在、参数是否与信号相符。
- 动画属性轨道、方法轨道及 `"position:x"` 类属性路径按静态引用解析，参与重命名和查找引用。
- 轨道和同步属性解析失败时：
  - 节点的类是本语言的类，却没有所指的成员，报编译错误；
  - 路径找不到节点，给出警告，因为共用的动画库可以刻意缺节点。

## 8. 错误

### 8.1 程序错误

- 程序错误中止本语言的整条调用链，退回到引擎调用进来的入口。调用本语言类声明的方法属于同一条链，无论接收者是哪个类，即使经接口调用也是。`f` 的静态类型为函数类型时，用 `f.call(x)` / `f.callv(args)` 调用本语言函数值也属于同一条链。函数值指 lambda 或方法值，含 `bind()` 之后。只有引擎入口开始新链，包括信号、引擎 `Callable`、引擎虚函数，以及静态类型为 `Callable` 的值的 `call()` 和 `callv()`。退出时释放持有的引用，记录错误和调用栈。编辑器内运行时进入调试器；游戏继续运行。
- debug 构建仅补充诊断细节，并在退出时报告泄漏。
- 需要返回值的引擎入口在中止后得到该返回类型的默认值。
- 本文称为“程序错误”的情形均按此处理。例如访问已释放对象、`unwrap`/`expect` 失败、越界下标和整数除以 0。还有就绪前读未赋值成员、入口检查失败、修改 `Dictionary` 键和等待被取消任务等。
- 同一线程上本语言调用嵌套超过 1024 层报程序错误，不因栈溢出崩溃。

### 8.2 预期之内的失败

- 预期之内的失败用 `Option`（可不处理）或 `Result`（不可丢弃）表达。
- 唯一特殊语法是短路返回的后缀 `?`：
  - 作用于 `Result[T, E]`：遇 `Err(e)` 从当前函数返回 `Err(e)`，否则得 `T`。当前函数须返回同 `E` 的 `Result[_, E]`，不同 `E` 先用 `map_err` 转换；
  - 作用于 `T?`：遇 `None` 从当前函数返回 `None`，否则得 `T`；当前函数须返回 `_?`；
  - 在返回 `void` 的函数中使用 `?`，或在 `Option` 与 `Result` 之间直接传播，报编译错误。用 `ok()`、`ok_or()` 转换；
  - 优先级使 `await f()?` 解析为 `(await f())?`。

  ```gdscript
  func load_profile(path: String) -> Result[Profile, Error]:
      var text = read_text(path)?
      return Ok(parse_profile(text)?)
  ```

### 8.3 断言

- `assert(条件, 消息)` 只在 debug 构建（编辑器内运行与 debug 导出）中求值，失败报程序错误。release 构建中移除，条件和消息均不求值。
- 消息为 `String`，仅在条件为假时求值。这属于语法硬规则的例外：GDScript 在 debug 构建中总是求值，release 构建中不求值。条件或消息中含 `await` 或后缀 `?` 报编译错误。条件须为 `bool`。例如 `assert(save_data, "...")` 报编译错误，并提示 `save_data != None`。release 构建也需检查的用 `expect` 或显式 `if`。

### 8.4 诊断

- 编译错误、警告和提示由分析服务报告。它们标在源码、Inspector 和场景树中。
- 程序错误诊断写明出错位置、相关成员或对象及调用栈。诊断还包含各节规定的具体信息，如必须赋值成员名、释放位置、`owner` 要求与实际 `owner`。
- 编译期和运行时的诊断都自带理由，不引用本规范。

## 9. 协程

### 9.1 async 与 Task

- 函数和 lambda 含 `await` 时须在签名写 `async func`，如 `button.pressed.connect(async func(): await fade_out())`。未写时报编译错误，并提供补 `async` 的快速修复。
- 只有返回 `void` 的引擎虚函数可声明为 `async`。`async func _init()` 和非 `void` 引擎虚函数的 `async` 覆盖均报编译错误。省略返回类型时相同。引擎调用的位置接受 `async` 函数并作为根任务运行。
- 调用 `async` 函数立即启动并运行到首次挂起，返回 `Task[T]`。无返回值时为 `Task[void]`；`void` 仅在 `Task[void]` 中可作类型实参。裸写 `Task` 报编译错误。`await f()` 和 `await t` 得 `T`。
- 丢弃 `async` 调用结果报编译错误；有意不等待写 `f().detach()`。
- setter、getter、`@onready` 初始化式和静态初始化不能 `await`。

### 9.2 结束状态

- 任务有三种结束状态。**完成**指得到返回值。**取消**指调用了 `t.cancel()`、所属对象释放，或 `Task.of(信号)` 的信号源释放。**失败**指内部发生程序错误，含 `await` 到已取消任务或已释放信号源。
- `await` 被取消的任务是程序错误。失败沿 `await` 链传播到根任务，即 `detach` 或引擎调用的任务。失败不可拦截。
- `await t.settled()` 把取消作为值返回 `Settled[T]`：`Finished(v)` 或 `Cancelled(r)`。`r` 为 `enum CancelReason`：`REQUESTED`、`OWNER_FREED`、`AWAITED_FREED`。任务失败时照常向等待方传播。
- `Task[void]` 的 `Finished` 不带值，模式写作 `Finished`。

### 9.3 取消与归属

- 对挂起任务，`cancel()` 立即断开所等待的信号或任务并取消它。对正在执行的任务，`cancel()` 只能来自它同步调用到的代码，如它发出的信号的处理函数。这时任务在下一次 `await` 或返回时取消，返回值被丢弃。对已结束任务无效果。调用 `cancel()` 后结果必为取消。
- 取消沿归属传播。以 `await f(...)` 直接启动并等待的子任务归父任务所有，随父任务一起取消。`await` 已有的 `Task` 值时，取消只停止等待，原任务继续运行。
- 因所属对象释放而取消时，不沿归属传播。另一个仍存活对象上的子任务像 `detach` 一样继续运行，与 GDScript 相同。

### 9.4 释放

- 任务结束时立即释放协程帧及所持引用，`await` 之后的代码不再执行。【已知限制】第一版没有 `finally` 或 `defer`。
- `Task` 是只保存结果的 `RefCounted` 句柄；挂起的协程不使 `self` 保持存活。

### 9.5 组合与重入

- `Task.of(信号)` 将信号包装为任务。`Task.delay(秒)` 计时。`Task.any(列表)` 返回首个完成任务的下标，并按归属规则取消其余任务。`Task.all(列表)` 等待全部完成。在 `any`/`all` 实参中直接启动的任务归其所有，已有任务仅停止等待。
- `await Task.all(列表)` 不产生值。需要结果时，先把任务存入变量，完成后逐个 `await`。
- 重入无自动策略，需要时经句柄取消旧调用。

### 9.6 await 信号

- 无参信号 `await` 不产生值。单参信号得该参数类型的值。多参信号只能作语句 `await`，使用结果报编译错误。
- 经已知类访问的信号签名已知。已知类指引擎 ClassDB 中的类、本语言声明的类和接口，以及经反射看到的 GDScript 类。静态类型仅为 `Signal` 时签名未知。作语句 `await` 时不受限制。作值 `await` 得 `Variant`：运行时无参为 NIL，单参为该值，多参为 `Array`：

  ```gdscript
  var r = await DialogueManager.Resolved   # 签名未知：r 为 Variant
  if r is Array and r[0] == id:
      return r[1]
  ```

### 9.7 对象释放与挂起的协程

- 协程所属对象收到 `NOTIFICATION_PREDELETE` 时（早于 `Node` 删除子节点），其协程立即静默取消并释放挂起帧引用。
- 被等待对象指另一对象上的协程宿主或信号源。它在完成前被释放时，推迟到本帧删除队列清空后的每帧回调判定：
  - 若等待方所属对象在此期间也已释放，等待方静默取消（`OWNER_FREED`）；
  - 否则等待方在 `await` 处报程序错误（写明被释放对象及释放位置）并沿 `await` 链传播。
- 经 `settled()` 等待不报错：`Task.of(信号)` 得 `Cancelled(CancelReason.AWAITED_FREED)`，另一对象的协程得 `Cancelled(CancelReason.OWNER_FREED)`：

  ```gdscript
  # hud.pet：HUD 和 player 都是 Main 的子节点
  async func _ready():
      await player.died         # 切换场景一起删除：静默取消
      show_game_over()          # player 单独释放且未发出 died：本帧后报程序错误
  ```

### 9.8 跨语言 await

- 调用 GDScript 方法可写 `await`（等待 `GDScriptFunctionState.completed` 并按声明返回类型做入口检查）。未写 `await` 时，作语句调用不报错，运行到首次挂起后如 `detach` 继续。未写 `await` 时，作值使用而运行时得到 `GDScriptFunctionState`，报程序错误并提示补 `await`。
- 等待 `GDScriptFunctionState` 期间，调用目标对象指接收者或 `Callable` 目标对象。它被释放或替换脚本（`script_changed`）时，按上一节被等待对象释放的规则处理，`settled()` 得 `Cancelled(CancelReason.OWNER_FREED)`。【已知限制】编辑器中重载 GDScript 脚本时，引擎静默清除挂起函数，等待方会一直挂起。
- GDScript `await` 本语言协程时，首次挂起返回 `Task` 的 `completed` 信号。本语言 `async` 方法向引擎报告无类型 `Variant` 返回值，声明类型仅用于文档和悬停。任务结束时：
  - 完成发出 `completed(值)`；
  - 取消先发出 `cancelled(原因)`，再发出 `completed(null)`，原因是 `CancelReason` 的值；
  - 失败只发出 `completed(null)`。失败已报告为程序错误，与本语言中一样不可拦截。

## 10. 引擎边界

### 10.1 引擎和 GDScript 看到的类型

| 本语言 | 引擎和 GDScript 看到的 |
|---|---|
| 对象 `T` 和 `T?` | 均为该类；非空由入口检查保证 |
| `int?` 等可空值类型 | 无类型 `Variant`，`None` 为 NIL |
| 元素可由引擎表示的 `Array[T]`、`Dictionary[K, V]` | 引擎带类型容器（嵌套容器只带外层一层类型） |
| struct | `String` 字段名键的 `Dictionary` |
| 带载荷 enum | `String` 字段名键的 `Dictionary`，保留键 `"$variant"` 存分支名：`{"$variant": "MOVE", "target": p}`，无载荷分支 `{"$variant": "IDLE"}` |
| `Result[T, E]` | 同带载荷 enum：`{"$variant": "Ok", "value": v}` 或 `{"$variant": "Err", "error": e}` |
| `async` 方法返回值 | 无类型 `Variant` |

- `T??` 和 `Variant?` 不能编码，因为引擎中外层的 `None` 和内层的空值都是 null。
  - 值须经引擎边界编码时报编译错误。字段中含它们的 struct、带载荷 enum 和 `Result` 同样如此。
  - 快速修复为 `T??` 加 `.flatten()`，为 `Variant?` 加 `.unwrap_or(null)`。
- GDScript 通过脚本反射对本语言全局类做静态检查。检查的是实例成员（属性、方法、信号）及 `Player.new(...)` 的位置实参。`_init` 不带 `pub` 时，GDScript 调带参 `new` 报编译错误，无参 `new` 在运行时报错。具名必须赋值实参仅限本语言使用。其余类级访问见“跨语言调用与消息”一节。
- **入口检查**：值从引擎、GDScript、C# 或远程检查器进入本语言时，按声明类型校验。进入的途径包括参数、`set` 写成员、信号实参、`await` 结果和解码。校验失败报程序错误。

### 10.2 引擎 API 中对象的可空性

- 未经绑定元数据审核的引擎 API 按流向定可空性：
  - 引擎返回的方法值与读出的属性为 `T?`；
  - 引擎调用本语言的虚函数参数和引擎信号参数为非空 `T`，传入 `null` 由入口检查报程序错误。覆盖或处理函数可自行标为 `T?` 接受空值；
  - 本语言传给引擎的方法参数和属性写入接受 `None`（如 `set_owner(null)`）。
- 审核按高频 API 推进。返回值分为三类：非空 `T`、仅误用报错时为空的 `T`、正常可空的 `T?`。第二类如 `get_tree()`，为空报程序错误。第三类如 `get_collider()`、`get_first_node_in_group()`、`get_parent()`。参数同理审核，如 `Area3D.body_shape_entered` 标可空。

### 10.3 绑定元数据

- 引擎未导出的既有约束在绑定元数据中补齐，已导出的直接使用。
- `Variant` 数学工具函数（`lerp`、`clamp`、`min`、`max`、`abs`、`wrap` 等）按同类型实参返回该类型。混用 `int` 与 `float` 报编译错误，快速修复把整数实参改为 `float`（如 `min(1.5, 1.0)`）。
- 以字面量类名过滤的引擎 API 按该类定型（如 `find_children("*", "MeshInstance3D")` 返回 `Array[MeshInstance3D]`）。
- 有些引擎 API 失败时返回空值，错误可在事后读取，如 `FileAccess.open` 与 `FileAccess.get_open_error()`。
  - 标准库为它们各加一个返回 `Result[T, Error]` 的版本，名字是原名加前缀 `try_`，如 `FileAccess.try_open`。
  - 原 API 的类型不变。
  - 错误无从读取的 API 仍返回 `T?`，如 `load()`。

### 10.4 字符串形式的成员名

- 以下位置的字面量名字按静态引用解析，与写成 `目标.名字` 相同。位置包括 `connect`、`emit_signal`、`call`、`call_deferred`、`set_deferred`、`has_method`、`Callable(obj, "m")`、`tween_property` 及 `"position:x"` 类属性路径，由绑定元数据标注参数位置。这些名字检查存在性与值类型，并参与重命名和查找引用。非字面量名字为显式动态访问。
- 目标上没有该名字时报编译错误，以下三种情况除外：
  - 目标的静态类型是 `Object` 或 `Variant`，即动态访问；
  - `has_method`、`has_signal`、`has_user_signal` 等运行时查询；
  - 名字含 `/`，如 `"theme_override_colors/font_color"`，引擎按实例列出这类属性。
- 名字指向不带 `pub` 的本语言成员时报编译错误，因为引擎按名找不到它。方法名的快速修复改用方法值，如 `call_deferred("reset")` 改为 `reset.call_deferred()`。
- 以 `@` 开头的名字报编译错误，目标为 `Object` 时也是。这种名字只能是改写后的私有信号名。

### 10.5 反射暴露的成员

- 引擎、GDScript 和 C# 经按名反射（`get_property_list`、`has_method`、`call`、`get`、`set`、`connect`）只能访问 `pub` 成员。引擎虚函数的覆盖除外，引擎按约定的名字调用它们。其余成员对按名访问不可见：GDScript 报编译错误，动态访问在运行时报找不到成员。
- 引擎按名字访问的以下成员须带 `pub`，否则报编译错误，快速修复补上 `pub`：
  - 导出的变量和 `@rpc` 函数；
  - 场景中信号连接的信号和方法；
  - 动画的属性轨道和方法轨道所指的成员，`MultiplayerSynchronizer` 同步的属性。
- 不带 `pub` 的信号以改写的名字 `@名字` 注册到引擎。本语言中照常写原名，`Signal` 值的 `get_name()` 返回改写的名字。【已知限制】其他语言拼出改写的名字，仍能按名访问这类信号。
- 元素为本语言独有类型的容器即便是 `pub` 也不经反射暴露。`struct` 成员照常按复制编码/解码暴露。
- 方法值形成的 `Callable`（如 `tween_callback(change_weapon)`）直接绑定目标方法，不按名查找，所以可以指向不带 `pub` 的方法。`pub` 内部类作为值为其脚本的类常量暴露。调试器可见全部成员。
- 读写引擎类的属性即调用它的 getter 和 setter，不经按名访问。`label.text = s` 与 `label.set_text(s)` 相同。
- 所以对象脚本的 `_get`、`_set` 只拦截按名字的访问，如 `get`、`set`、检查器和动画。
- `_get_property_list()` 加入的属性也经过 `_validate_property()`，与 GDScript 相同。
- 【已知限制】引擎不向脚本提供带下标的属性的下标，如 `BaseMaterial3D` 的各张贴图共用一对方法。其他扩展的类中的这类属性因此仍按名读写。

### 10.6 数值边界

- 传给引擎窄整数或无符号整数参数/属性（`int8`、`int16`、`int32`、`uint8`、`uint16`、`uint32`、`char32`）时做范围检查，越界报程序错误。
- `uint64` 与 `int` 按位互转，不做范围检查。两者的值一一对应，如 RefCounted 的实例 ID 读出为负数，原样传回。【已知限制】其他扩展的类不在引擎 API 描述中，其窄整数参数不检查。
- 写入引擎 32 位浮点位置（如标准构建 `real_t` 向量分量）时隐式收窄。

### 10.7 String 与 StringName

- 热路径上 `String` 与 `StringName`、`NodePath` 之间的运行时转换给出性能提示。

### 10.8 JSON

- JSON 数字解码到 `int` 或无载荷的 enum 时，只接受恰为整数且在 `int64` 范围内的浮点数，否则报错而不截断。
- struct 和 enum 赋给 `Variant` 时已按引擎边界的规则编码。所以 `JSON.stringify`、`var_to_str` 和 `ConfigFile` 直接存取它们，读回时用 `T.try_from`。它们的容器先以 `Array(hits)`、`Dictionary(d)` 转换。
- 【已知限制】JSON 把 `Vector2`、`Color` 等引擎值写成字符串，读回时类型不符。绝对值超过 2^53 的整数也会失去精度。存这些值有三种办法：
  - `var_to_str`；
  - `ConfigFile`；
  - 经 `JSON.from_native` 和 `JSON.to_native` 转换，仍存为 JSON。

## 11. 与 GDScript 和 C# 共存

### 11.1 其他语言的类的静态视图

- 本语言经反射看到其他语言的类（GDScript、C# 等）和 autoload，含它们的继承子类型关系，适用于参数、`is` 与 `as`。对象类型的成员、参数和返回值视为 `T?`。值类型照声明，未标注类型的为 `Variant`。
- autoload 名是全局标识符，类型为其脚本的类（无 `class_name` 时同样如此）。
- autoload 名也可写在类型中，指同一个类，如 `func get_export() -> Export` 和 `f: Export.FileFormat`。
  - 场景 autoload 指根节点脚本的类。根节点没有脚本时，指它的引擎类。
  - 作用域中更近的同名声明优先。
- 经全局类名或 `preload` 指称其他语言的类时，以 `new(...)` 按其 `_init` 参数构造。还可经类访问静态方法与带值类型的常量（`Unit.make()`、`Unit.TILE_SIZE`）。
- 【已知限制】引擎不提供 C# 类的构造器参数和常量。因此 C# 类只能以无参 `new()` 构造，其常量不可访问。

### 11.2 继承

- 两个方向的跨语言继承均不支持：
  - 本语言类不能继承 GDScript 类。插件要求继承时，手写薄 GDScript 转发子类。其中调用本语言实例成员仍受 GDScript 静态检查；
  - GDScript 脚本继承本语言类时，分析服务在该 GDScript 文件上报错并说明原因；
  - 迁移一个类时须连带迁移其 GDScript 子类。

### 11.3 跨语言调用与消息

- GDScript 和 C# 按名字只能访问反射暴露的成员。
- 【已知限制】GDScript 除使用实例、调用 `new` 外，还能按名读写 `pub` 静态变量，并读取常量值和 `Player.Inner` 脚本。此外不支持以下类级访问，替代写法是经实例或 autoload 实例方法转发：
  - 调用本语言静态方法；
  - 把本语言 enum、常量或内部类用于 GDScript 类型标注或常量表达式；
  - 为本语言编写的 autoload 定型（GDScript 中视为 `Node` 或无类型）。
- 与 C# 之间的消息用 struct 表达，按字段名 `Dictionary` 编解码并做入口检查。

## 12. 运行时

### 12.2 性能

- 目标是明显快于 GDScript，不以 C# 为目标。
- 【待定】原生代码的生成时机：运行时、导出时，或两者都有。

### 12.3 线程

- 虚拟机须在引擎调度的任意线程（如后台资源加载线程、物理线程）上安全运行。共享对象的数据竞争由用户负责。
- 【已知限制】第一版不新增线程 API，直接使用引擎的 `Thread`、`WorkerThreadPool` 和 `Mutex`。

### 12.4 热重载

- 仅修改函数体时在运行中直接替换。下一次调用走新代码。挂起中的协程和此前创建的 lambda 继续执行创建它们的旧版本代码。
- 修改成员、签名、常量或类结构时不迁移状态，并提示重新运行。当前运行拒绝本次及后续一切热重载，保证单次运行中每个类只有一个版本。

### 12.5 调试支持

- 接入引擎调试界面（断点、单步、局部变量、调用栈）。
- debug 构建记录本语言实例及由本语言释放对象的释放位置。退出时按创建位置列出泄漏的节点与对象。
- 【已知限制】释放位置只为最近的释放保留，更早的释放在诊断中不写位置。全部保留会使内存随释放次数一直增长。

## 13. 工具链

### 13.1 分析服务

- 外部 IDE 经 LSP 连接分析服务，命令行与 CI 以无界面模式运行同一套分析。存在编译错误（含场景缺值错误）时，无界面分析命令返回非零退出码。

### 13.2 第一版的工具

- **诊断与导航**：编辑器、LSP 与命令行共享实时诊断。提供补全、悬停类型、跳转定义与含场景在内的查找引用。
- **场景与重构**：带场景部分的重命名。`$X`、信号连接、`PackedScene[T]`、`@parent`/`@owner` 与必须赋值导出的场景检查。插入推断类型、补全 `match` 分支、加显式转换及本文各处改写的快速修复。
- **格式化**：唯一一套无选项规则。它保持语法树、全部注释和字面量写法不变，幂等，语法错误文件不格式化：
  - 缩进每层一个制表符。注释按所属块缩进，并保留多出的缩进层数。块首行前的注释归该块。多块同时结束处的注释按原列归属，但不浅于其后注释所属的块；
  - 括号内续行比开括号行多缩进一层，独占一行的闭括号回到开括号行缩进。`\` 续行比语句首行多缩进一层。括号内 lambda 块比其头部结束行多缩进一层；
  - 二元运算符两侧各一空格，含赋值、具名实参 `=`、`:=` 和 `->`。逗号、分号、冒号后一空格，前无空格。括号内侧、一元运算符后、调用与下标括号前、`.` 两侧无空格。`?` 前、`$` 路径内及单行字典花括号内侧也无空格。行尾注释前两空格。去掉多余空格与行尾空白；
  - 顶层函数与类（含 struct、interface）前后各空两行，内部类中前后各空一行。计算时连同紧贴其上的注释与注解。块首成员前及无函数体方法除外。其余位置连续空行压为一行，文件首无空行、尾留单个换行；
  - 换行位置不变：不按列宽拆合行，保留 `;`、同行块与 `\`。字面量、引号、括号、注释文字、尾随逗号和原文件换行符不变。
- **迁移**：GDScript 与本语言之间的双向迁移工具。
- **lint、调试与测试**：lint 作为分析服务警告报告，并用 `@warning_ignore` 抑制；某个 `@warning_ignore` 不再抑制任何诊断时给出提示。接入引擎调试器。提供可无界面运行的自带测试运行器。不提供包管理器。

### 13.3 重构

- 重构包含场景中的一半。机械且保义的修改预览后随重构自动完成，并可撤销。这类修改包括导出属性键、信号连接方法名、动画轨道路径和无损值转换。需要判断的修改只报告并给出选项，如改节点类型、现有资源不符新类型。
- 在场景编辑器中改名或移动节点时，脚本中的 `$X`、`%X` 和字面量 `get_node` 路径自动更新。更新并入同一撤销步骤，不预览，完成后列于输出面板。非字面量路径及在其他场景中仍指向别处的路径只报告并提供快速修复。

### 13.4 迁移

- 迁移工具把 GDScript 文件转为本语言文件，范围可以是文件、目录或整个项目。场景、资源和项目设置中引用它的路径一并修改。
- 迁移只做可还原规则允许的修改，如补 `pub`、补类型、改真值判断、补 `.detach()`。它不引入 GDScript 没有的特性。
- 需要判断之处保留为编译错误，由快速修复给出选项。不报编译错误的列进迁移报告，如以非字面量名字按名访问。迁移类前列出其 GDScript 子类和对它做类级访问的 GDScript 文件，并提供一并迁移选项。
- 反向转换把本语言文件转回 GDScript。它去掉或替换可还原的写法，并写出 GDScript 能表达的类型。文件用到 GDScript 没有的特性时不转换，只列出位置。

## 14. 第一版范围与待定问题

### 14.2 以后可以兼容地加入

以下特性可在第一版之后兼容加入，泛型与就绪前自动挂起 setter 的完整设计见[设计说明](design-notes.md)：

- **语言特性**：
  - 用户定义泛型与泛型接口；
  - 接口的默认方法；
  - 带类型的弱引用 `weak[T]`，及引用环警告中改用它的快速修复；
  - 别名/`import` 与跨类访问 `preload` 类；
  - 两个文件互相继承对方的类；
  - 更严的 `private`；
  - 导出变量和 `@rpc` 函数隐含 `pub`；
  - struct 位置构造、可变方法、运算符重定义与间接递归；
  - 导出的 struct 和带载荷 enum 中的 `Node` 字段；
  - 函数类型参数名、直接调用 `f(x)` 和共享捕获；
  - 未知签名的 `Callable` 显式转为函数类型；
  - 普通函数隐式用于期望 `async` 处；
  - 调用处展开剩余参数、函数类型中的剩余参数；
  - 字段收窄/条件绑定；
  - `match` 的类型模式、`match` 表达式与多模式绑定；
  - 就绪后赋值的非空成员；
  - 就绪前自动挂起 setter；
  - 常驻断言；
  - 按本语言写法显示值的调试文本；
  - 协程重入注解、多参信号解构与带参 `Signal`。
- **跨语言与工具**：
  - 可选带编译器的运行时库；
  - GDScript 协程 `Task` 句柄；
  - GDScript enum/内部类/静态变量视图及静态方法调用；
  - 检查 GDScript 代码以改写的名字访问私有信号；
  - 进阶重构、性能分析深度集成、文档生成与转发外壳生成。

### 14.3 待定问题

- 原生代码的生成时机：运行时、导出时，或两者都有。
- `@tool` 脚本在编辑器中运行的规则。
