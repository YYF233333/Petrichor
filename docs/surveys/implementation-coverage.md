# 调查：真实代码用到而实现尚不支持的写法

**2026-11-01** · 关联：[语言规范](../language-spec.zh.md)、[实现架构](../architecture.md)

## 问题

两个里程碑让 FPS 和 Ring 的脚本以本语言运行（架构 §5 第 4、6 步），但只覆盖了这两个项目用到的写法。本文统计：在真实的 GDScript 项目中，有多少文件用到了规范第一版包含、而提交 `812aaa2` 的实现尚不支持的写法。

## 语料

与[跨分支重复调查](cross-branch-duplication.md)相同：20 个仓库，固定提交，第一方代码的判定规则也相同（`cross-branch-duplication/gdcorpus.py`）。第一方 `.gd` 文件 2,800 个，共 203,720 行代码（不计空行和注释行）；其中 12 个游戏仓库有 1,223 个文件、70,646 行。

## 方法

- 按词法识别下表中的 16 种写法，每种都是规范第一版的内容，而实现会报“尚不支持”或语法错误。
- `Type.m()` 只在 Type 是引擎类、内置类型或本仓库 `class_name` 的类，且 m 是它的静态方法时计入；引擎一侧的静态方法取自 godot-cpp 的 `extension_api-4-7.json`。`Type.new()` 只在 Type 是本仓库 `class_name` 的类时计入；引擎类的 `new()` 已经支持。
- 计入的注解：类注解 `@tool`、`@icon`、`@static_unload`、`@abstract`；成员注解中除 `@export`、`@export_range`、`@export_enum`、`@export_multiline`、`@onready` 和分组注解以外的全部，例如 `@export_node_path`、`@export_flags`、`@warning_ignore`、`@rpc`。
- 行首为 `for` 的一行中，第一个 `in` 算作循环的，其余的 `in` 算作运算符。
- 一个文件用到其中任何一种，就计为“用到”。

## 结果

| 写法 | 规范 | 全部：文件 | 全部：每千行 | 游戏：文件 | 游戏：每千行 |
|---|---|---:|---:|---:|---:|
| 一种都没用到的文件 | | 29.6% | | 40.6% | |
| `const` | §4.5 | 29.8% | 15.2 | 28.6% | 20.9 |
| `Type.m()`，本仓库类的静态方法 | §4.8 | 22.9% | 16.2 | 20.4% | 12.1 |
| `&"…"`、`^"…"` 字面量 | §2.2 | 20.3% | 19.5 | 20.4% | 27.5 |
| 其他成员注解 | §2.3 | 13.4% | 6.2 | 5.2% | 2.4 |
| `@tool`、`@icon`、`@static_unload`、`@abstract` | §2.3 | 13.4% | 3.4 | 5.8% | 1.1 |
| `Type.new()`，本仓库的类 | §4.6 | 13.2% | 6.8 | 6.3% | 1.5 |
| `Type.m()`，引擎类或内置类型的静态方法 | §10 | 9.9% | 4.9 | 3.6% | 1.8 |
| `preload()` 场景以外的资源 | §7.4 | 9.7% | 3.3 | 8.7% | 4.6 |
| lambda | §3.11 | 8.5% | 4.9 | 7.4% | 3.3 |
| `%Name`、`$".."` | §7.1、§7.3 | 7.8% | 4.8 | 3.4% | 2.1 |
| `in`、`not in` | §5.1 | 7.4% | 2.8 | 4.8% | 1.4 |
| `enum` | §3.7 | 6.7% | 1.3 | 4.7% | 1.0 |
| `assert` | §8.3 | 4.4% | 2.9 | 5.2% | 1.7 |
| 内部类 | §2.1 | 4.3% | 0.9 | 1.6% | 0.5 |
| `static var` | §4.8 | 2.7% | 0.9 | 2.9% | 1.2 |
| 位运算 | §3.2、§5.1 | 2.6% | 1.3 | 1.2% | 1.0 |
| `**` | §5.1 | 0.1% | 0.0 | 0 | 0 |

各仓库中一种都没用到的文件：

| 仓库 | 类别 | 文件 | 一种都没用到 |
|---|---|---:|---:|
| Starter-Kit-FPS | game | 7 | 7 |
| Starter-Kit-3D-Platformer | game | 9 | 9 |
| Starter-Kit-Racing | game | 3 | 3 |
| Starter-Kit-City-Builder | game | 6 | 5 |
| Ring | game | 66 | 55 |
| tanks-of-freedom-ii | game | 292 | 213 |
| godot_open_star_fighter | game | 40 | 25 |
| deck_builder_tutorial | game | 122 | 44 |
| godot-open-rts | game | 118 | 39 |
| tps-demo | game | 15 | 3 |
| godot-open-rpg | game | 78 | 15 |
| godot-tiny-mmo | game | 467 | 79 |
| godot-demo-projects | demos | 461 | 147 |
| Pixelorama | tool | 210 | 27 |
| godot-statecharts | plugin | 89 | 41 |
| gdUnit4 | plugin | 482 | 92 |
| beehave | plugin | 95 | 9 |
| phantom-camera | plugin | 36 | 3 |
| Godot-Game-Template | plugin | 125 | 9 |
| godot_dialogue_manager | plugin | 79 | 4 |

每种写法的分布和抽样位置见 `implementation-coverage/results/coverage.txt`，加 `--samples N` 运行脚本可列出每种的前 N 处。

## 局限

- 按词法识别，不做类型解析。经常量或变量间接调用的静态方法不计，例如 `const Utils = preload("utils.gd")` 之后的 `Utils.f()`；这类文件已经因为 `const` 和 `preload` 计入。
- 一种都没用到，不等于文件能编译。迁移还要补类型，[第二次纸面迁移](paper-migration-2.md)中每百行约 28 处修改；尚未实现、本文也不统计的还有无类型的值（规范 §3.13）、match 的数组和字典模式、字面量形式的属性名（§10.4）、不导出的必须赋值成员（§6.2）等。
- 规范中本语言新增的构造，如 Result、泛型、带载荷的 enum，不在 GDScript 中出现，本文无法统计。

## 对实现的意义

- 在全部仓库中，七成文件至少用到一种尚未实现的写法，游戏仓库中约六成。最常见的几种都是小特性：`const`、经类名调用静态方法和 `new()`、`&"…"` 字面量、注解。
- 两个里程碑的项目几乎不用这些写法：FPS 的 7 个文件全部、Ring 的 66 个文件中 55 个一种都没用到，而全部仓库中只有 29.6%。所以两次迁移都没有暴露这些缺口。

## 重跑（`a203ae1`，2026-11-04）

上面表中的写法在 `a203ae1` 都已实现，只有 `@tool` 和 `@export_tool_button` 例外，它们此后移出了第一版（规范 §2.3）。其中三种还有未实现的部分，计入下表：`Type.new()` 传参所需的带参数 `_init()`、含 await 的 lambda，以及常量的容器中的 `preload()`。

重跑用 `implementation-coverage/remaining.py` 在同一语料上统计：真实的 GDScript 代码用到、属于第一版而 `a203ae1` 的实现尚不支持的写法。名字只在文件写出其类型时才有类型，所以对象之间的 `==` 和返回 Variant 的引擎方法是下限；各写法的定义见脚本的文档字符串。

| 写法 | 规范 | 全部：文件 | 全部：每千行 | 游戏：文件 | 游戏：每千行 |
|---|---|---:|---:|---:|---:|
| 一种都没用到的文件 | | 60.0% | | 63.4% | |
| Variant 作类型：无类型的 `Array`、`Dictionary`，以及 `Variant` | §3.13 | 28.2% | 22.1 | 26.7% | 28.7 |
| 返回 Variant 的引擎方法，如 `get_meta()`、`get_setting()` | §3.13 | 9.4% | 3.5 | 6.3% | 2.4 |
| 带参数的 `_init()` | §6.2 | 8.1% | 1.2 | 4.1% | 0.7 |
| 对象之间的 `==`、`!=` | §5.2 | 5.1% | 1.3 | 4.3% | 1.3 |
| 以裸名使用引擎类的常量，如 `NOTIFICATION_PREDELETE` | | 4.9% | 1.1 | 2.9% | 0.8 |
| 不是常量的参数默认值 | §4.6 | 0.8% | 0.2 | 0.7% | 0.2 |
| 含 await 的 lambda | §9.1 | 0.3% | 0.2 | 0.1% | 0.0 |
| 常量的容器中的 `preload()` | §4.5 | 0.2% | 0.0 | 0.4% | 0.1 |
| 静态函数作为值，如 `Battler.sort` | §3.11 | 0.2% | 0.1 | 0.3% | 0.1 |
| 在自身上以字面量指名本文件的成员，如 `set("x", v)` | §10.4、§10.5 | 0.2% | 0.0 | 0.1% | 0.0 |
| match 的数组和字典模式 | §5.6 | 0.1% | 0.0 | 0.2% | 0.1 |
| 遍历 String 的 `for` | §5.7 | 0 | 0 | 0 | 0 |

- 以字面量指名的成员能够编译，但名字不受检查，不带 `pub` 的成员也不暴露给引擎（§10.5）；其余各种写法，实现都报告尚不支持。Node 和 Resource 脚本用必须赋值的成员代替 `_init()` 的参数（§6.2），这也尚未实现。
- FPS、Starter-Kit-3D-Platformer 和 Starter-Kit-Racing 的全部文件，以及 Ring 的 66 个文件中的 58 个，一种都没用到；比例最低的是 godot_dialogue_manager（31.6%）、godot-tiny-mmo（32.5%）和 Pixelorama（38.6%）。各仓库的计数见 `implementation-coverage/results/remaining.txt`。
- 未计入：进入实例化场景的路径和实例化它的场景中的唯一名称（§7.1），这要读场景；对 `T?` 的 match；把内部类外层类的静态函数作为值；迁移时必须补上类型的无类型值。
- 剩下的主要缺口是 Variant（§3.13）：28.2% 的文件把无类型的容器或 Variant 写作类型，9.4% 调用返回 Variant 的引擎方法。其次是带参数的 `_init()`、对象之间的 `==` 和以裸名使用引擎的常量，各占 5–8% 的文件；其余每种不到 1%。

## 第二次重跑（`602ee60`，2026-11-07）

上一次重跑表中最常见的五种写法，此后都已实现：
- Variant 和无类型的容器；
- 返回 Variant 的引擎方法；
- 带参数的 `_init()`；
- 对象之间的 `==`；
- 以裸名使用引擎类的常量。

这次用 `implementation-coverage/remaining2.py` 统计其余七种，并加入四种：
- 剩余参数 `...args`（§4.6）；
- `breakpoint` 语句；
- 定义 `_iter_init` 的类，供 `for` 遍历它的对象（§5.7）；
- 本文件的类的另一个实例的成员，如 `parent.children`（§4.2）。

最后一种按文件写出的类型识别：名字标注为本文件的 `class_name`，成员由本文件的 `var` 或 `func` 声明。本语言中这样的成员不带 `pub`，在本文件中可见，而实现报告尚不支持。

| 写法 | 规范 | 全部：文件 | 全部：每千行 | 游戏：文件 | 游戏：每千行 |
|---|---|---:|---:|---:|---:|
| 一种都没用到的文件 | | 96.1% | | 97.5% | |
| 剩余参数 `...args` | §4.6 | 1.1% | 0.4 | 0 | 0 |
| 同一个类的另一个实例的成员 | §4.2 | 1.0% | 1.2 | 0.7% | 0.5 |
| 不是常量的参数默认值 | §4.6 | 0.8% | 0.2 | 0.7% | 0.2 |
| 含 await 的 lambda | §9.1 | 0.3% | 0.2 | 0.1% | 0.0 |
| 常量的容器中的 `preload()` | §4.5 | 0.2% | 0.0 | 0.4% | 0.1 |
| 静态函数作为值 | §3.11 | 0.2% | 0.1 | 0.3% | 0.1 |
| 在自身上以字面量指名本文件的成员 | §10.4、§10.5 | 0.2% | 0.0 | 0.1% | 0.0 |
| match 的数组和字典模式 | §5.6 | 0.1% | 0.0 | 0.2% | 0.1 |
| 定义 `_iter_init` 的类 | §5.7 | 0 | 0 | 0 | 0 |
| `breakpoint` | | 0 | 0 | 0 | 0 |
| 遍历 String 的 `for` | §5.7 | 0 | 0 | 0 | 0 |

- 剩余参数只出现在 gdUnit4 中，它是测试框架。
- 每种写法都不超过 1.1% 的文件，各仓库中一种都没用到的文件占 89.4%–100%。各仓库的计数见 `implementation-coverage/results/remaining2.txt`。
- 未计入的写法：
  - 进入实例化场景的路径，这要读场景；
  - 对 `T?` 的 match 和不导出的必须赋值成员，迁移时才写出；
  - 内部类以裸名把外层类的静态函数作为值。
- 所以 GDScript 代码用到的写法已经基本实现。语言剩下的工作主要是本语言新增的构造，如 Result、带载荷的 enum 和结构体的方法。GDScript 中没有它们，本调查无法为其排序。

## 补充：GDScript 自有的常量和工具函数（`c92c643`，2026-11-09）

前三次统计只数了引擎 API 描述中的全局符号，漏掉了 GDScript 自己定义的一层：
- 常量 `PI`、`TAU`、`INF`、`NAN`，由分词器给出（`modules/gdscript/gdscript_tokenizer.cpp:540-546`）；
- `range()`、`len()` 等工具函数（`modules/gdscript/gdscript_utility_functions.cpp:572-590`）。

`c92c643` 的实现只支持其中的 `load()`、`preload()` 和 `print_debug()`。`implementation-coverage/gdscript_builtins.py` 统计其余各个：

| 写法 | 全部：文件 | 全部：每千行 | 游戏：文件 | 游戏：每千行 |
|---|---:|---:|---:|---:|
| 用到其中任何一个的文件 | 10.5% | | 7.0% | |
| `range()` | 7.4% | 2.3 | 4.7% | 1.5 |
| `TAU` | 1.3% | 0.4 | 1.5% | 0.4 |
| `PI` | 1.1% | 0.4 | 0.9% | 0.4 |
| `INF` | 0.6% | 0.2 | 0.4% | 0.1 |
| `is_instance_of()` | 0.4% | 0.1 | 0.1% | 0.0 |
| `len()`、`Color8()` | 各 0.2% | 0.1 以下 | 0.1% 以下 | 0.0 |
| `char()`、`ord()`、`get_stack()`、`print_stack()`、`inst_to_dict()`、`dict_to_inst()` | 各 0.1% 以下 | 0.0 | 0 | 0 |
| `NAN`、`type_exists()`、`convert()` | 0 | 0 | 0 | 0 |

- 用到其中任何一个的文件占 10.5%，多于第二次重跑中其余写法的总和（3.9%）。所以那里“已经基本实现”的结论不成立。
- `range()` 占了大半。它和 `PI`、`TAU` 一样，是 GDScript 代码中的日常写法。
- 一种都没用到的文件，在 Pixelorama 中最少（64.8%），在 godot_dialogue_manager 中其次（74.7%）。各仓库的计数见 `implementation-coverage/results/gdscript_builtins.txt`。

## 第三次重跑（`3f61379`，2026-11-11）

第二次重跑表中的写法，此后都已实现。补充表中的函数除 5 个已弃用的外也都已实现。这 5 个按规范报编译错误，诊断给出替代写法（§2.6）。

此外实现的还有：
- Node 和 Resource 脚本的 `_init()` 参数（§6.2）；
- 覆盖 Object 的属性方法，如 `_get()`、`_set()`、`_get_property_list()`；
- 作为值的 `Class.new`，以及对 Script 值的 `is_instance_of()`（§2.6）；
- enum 名作为值，如 `State.keys()`（§3.7）；
- 名字在运行时给出的 `Callable(o, name)`；
- 进入实例化场景和走出根节点的路径，以及字面量路径的 `get_node()`（§7.1）。

这次先从编译器的诊断中，找出实现报告尚不支持的写法。再为每种写几行的 `.pet` 片段，用无界面构建（`--petrichor-build`）编译。上面已实现的写法也这样确认。实现拒绝的写法由 `implementation-coverage/remaining3.py` 在同一语料上统计，它的文档字符串列出各自的诊断：
- `class_name` 作为值，如 `mock(Player)`（§2.1）；
- 引擎类作为值，如 `[InputEventMouseButton, InputEventScreenTouch]`；
- 直接传给 Array 方法的 lambda 中无类型的参数，如 `filter(func(x): ...)`（§3.8、§4.6）；
- `instance_from_id()`，以及 Callable 和 Signal 的 `get_object()`，它们返回 `Object?`；
- 不经 autoload 的绝对路径 `$"/root/..."`（§7.1）；
- 常量初始化式中的 `%` 格式化（§4.5）；
- 带类型容器的构造，如 `Array[String](a)`。

| 写法 | 规范 | 全部：文件 | 全部：每千行 | 游戏：文件 | 游戏：每千行 |
|---|---|---:|---:|---:|---:|
| 一种都没用到的文件 | | 96.0% | | 97.4% | |
| `class_name` 作为值 | §2.1 | 1.6% | 1.4 | 0 | 0 |
| 传给 Array 方法的 lambda 的无类型参数 | §3.8、§4.6 | 1.5% | 0.5 | 2.1% | 1.0 |
| 引擎类作为值 | §2.6 | 0.7% | 0.8 | 0.1% | 0.0 |
| `instance_from_id()`、`get_object()` | | 0.3% | 0.1 | 0.2% | 0.1 |
| 不经 autoload 的绝对路径 `$` | §7.1 | 0.1% | 0.1 | 0.2% | 0.1 |
| 常量中的 `%` 格式化 | §4.5 | 0 | 0 | 0 | 0 |
| `Array[T](a)`、`Dictionary[K, V](d)` | | 0 | 0 | 0 | 0 |

- 两种作为值的写法几乎都在 gdUnit4 中。用到 `class_name` 作为值的 45 个文件中它占 43 个，用到引擎类作为值的 20 个中占 18 个。它们多是测试把类传给 `mock()` 和 `is_instanceof()`。
- 引擎类作为值还没有设计。规范只把存在变量中的引擎类记为 `is_instance_of()` 的已知限制（§2.6）。[设计说明](../design-notes.md)把它留给类作为值一并设计。
- `class_name` 作为值则是实现的缺口：内部类和 preload 的常量已能作为值，它却被报为未声明的名字。
- 游戏仓库中最常见的是 lambda 的参数，26 个文件中 20 个在 godot-open-rts。规范的标准库让 `map`、`filter`、`sort` 等接受带类型的 lambda（§3.8）。有期望类型时，lambda 的参数取自它（§4.6）。实现还没给出这个期望类型，所以迁移时要写出参数的类型。
- 绝对路径的 `$` 有 3 个文件在 Ring 中，如 `$/root/Main`，另一个是 gdUnit4 的测试。
- 常量中的 `%` 格式化和带类型容器的构造，语料中都没有用到。后者只出现在 Godot-Game-Template 写在字符串中的配置文本里。规范没有提到 `Array[T](a)`，实现把它当作语法错误。
- 各仓库中一种都没用到的文件占 83.1%–100%，最低的是 godot-open-rts。各仓库的计数见 `implementation-coverage/results/remaining3.txt`。
- 按词法识别，有遗漏也有多计：
  - 名字只在 `=`、`(`、`,`、`[`、`return` 或 `in` 之后，且在 `)`、`,`、`]`、`}` 或行尾之前，才算作值。所以 `x == Node and y` 这类不计，两种作为值的写法是下限；
  - lambda 只计直接写在实参中的。方法与 Array 的同名时，接收者不是 Array 也计入；
  - `get_object()` 按方法名计，gdUnit4 测试中对 mock 的调用也计入。
- 按规范报编译错误的写法不是实现的缺口，未计入。例如：
  - 给 String 的字符赋值（§5.1）；
  - 非 bool 的条件（§5.4）；
  - 已弃用的工具函数（§2.6）；
  - 经 `/root/X` 访问 autoload（§7.3）。
- 表中只有找到的缺口，它们来自诊断和逐个编译的片段。诊断没有说明、片段也没有试到的写法，本文无法统计。
- 所以在找到的缺口中，每种都不超过 1.6% 的文件。超过 1% 的只有两种：`class_name` 作为值集中在一个测试框架中；lambda 的参数写出类型即可编译。

## 复现

```
python docs/surveys/cross-branch-duplication/gdcorpus.py corpus
python docs/surveys/implementation-coverage/coverage.py corpus thirdparty/godot-cpp/gdextension/extension_api-4-7.json > docs/surveys/implementation-coverage/results/coverage.txt
python docs/surveys/implementation-coverage/remaining.py corpus thirdparty/godot-cpp/gdextension/extension_api-4-7.json > docs/surveys/implementation-coverage/results/remaining.txt
python docs/surveys/implementation-coverage/remaining2.py corpus thirdparty/godot-cpp/gdextension/extension_api-4-7.json > docs/surveys/implementation-coverage/results/remaining2.txt
python docs/surveys/implementation-coverage/gdscript_builtins.py corpus > docs/surveys/implementation-coverage/results/gdscript_builtins.txt
python docs/surveys/implementation-coverage/remaining3.py corpus thirdparty/godot-cpp/gdextension/extension_api-4-7.json > docs/surveys/implementation-coverage/results/remaining3.txt
```
