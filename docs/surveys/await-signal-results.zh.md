# 调查：等待多参数信号的结果有多常被使用

**2026-10-22** · 关联：[语言规范](../language-spec.zh.md) §9.6 · English: [await-signal-results.en.md](await-signal-results.en.md)

本文是一次小规模的实证调查，对象是规范 v0.1（git 标签 `spec-v0.1`）中一个尚未确定的选择：await 一个有多个参数的信号，得到的是什么。做法是先在真实代码上统计这种情况出现的频率，再逐条比较各个候选规则在这些代码上分别会产生什么结果。现行规范采用了规则 A：多参数信号的 await 只能作为语句（§9.6）。

## 问题

在 GDScript 中，await 一个信号时，信号没有实参则得到 null，有一个实参则得到这个实参，有多个实参则得到由这些实参组成的 Array（Godot `469659e` 的 `modules/gdscript/gdscript_function.cpp:237-255`；规范附录 A）。本语言则给结果定型：await 一个无参数的静态信号不产生值，await 一个单参数的静态信号得到该参数类型的值（§9.6）。对于有多个参数的静态信号，候选规则有三条：

| 规则 | `s` 有多个参数时，`await s` 得到什么 | 来源与现状 |
|---|---|---|
| A 只能作语句 | 不产生值，这个 await 只能作为语句 | v0.1 §9 的写法；已采用（§9.6） |
| B 得到 Array | 由实参组成的 `Array[Variant]` | GDScript；未采用（附录 B） |
| C 按参数绑定 | 一种只用于 await 的形式：`var (result, code, headers, body) = await http.request_completed`，每个变量按信号的参数定型 | v0.1 设计说明中的提案；未采用，以后可以加入（附录 B） |

A 和 C 只在使用结果的代码上有区别。按 A，这类代码必须改写，通常改成连接一个处理函数；按 C，代码形状不变，但语言要多出一种只为 await 服务的绑定形式。B 保留 GDScript 的形状，但丢掉了参数类型。要回答的问题有三个：真实的 Godot 4 代码有多常 await 有两个及以上参数的信号？有多常使用其结果？拿结果做什么？

## 语料

我们选了 14 个 Godot 4 项目：官方示例集、一个 RPG 示例、四个 Kenney 入门模板、官方第三人称射击示例、图像编辑器 Pixelorama，以及 6 个常用的插件和模板。仓库和提交与[属性访问器调查](property-accessors.zh.md)相同。它们都是 MIT 许可。tps-demo 的 LICENSE.md 规定其素材采用 CC-BY 3.0、代码采用 MIT，我们只读它的代码。检出时只取了 `.gd` 文件和许可证文件。

| 仓库 | 提交 | `.gd` 文件 | 行数 | 第一方行数 | await（第一方） |
|---|---|---|---|---|---|
| [godotengine/godot-demo-projects](https://github.com/godotengine/godot-demo-projects) | `b761b4c` | 461 | 30,819 | 30,819 | 171（171） |
| [gdquest-demos/godot-open-rpg](https://github.com/gdquest-demos/godot-open-rpg) | `d59f2f7` | 342 | 40,662 | 5,422 | 236（111） |
| [KenneyNL/Starter-Kit-3D-Platformer](https://github.com/KenneyNL/Starter-Kit-3D-Platformer) | `3fa8a04` | 9 | 378 | 378 | 1（1） |
| [KenneyNL/Starter-Kit-City-Builder](https://github.com/KenneyNL/Starter-Kit-City-Builder) | `4535092` | 6 | 311 | 311 | 0 |
| [KenneyNL/Starter-Kit-FPS](https://github.com/KenneyNL/Starter-Kit-FPS) | `185fd23` | 7 | 451 | 451 | 0 |
| [KenneyNL/Starter-Kit-Racing](https://github.com/KenneyNL/Starter-Kit-Racing) | `2f2e5f2` | 3 | 258 | 258 | 0 |
| [godotengine/tps-demo](https://github.com/godotengine/tps-demo) | `a82f154` | 15 | 1,619 | 1,619 | 8（8） |
| [Orama-Interactive/Pixelorama](https://github.com/Orama-Interactive/Pixelorama) | `38a8b03` | 249 | 60,025 | 56,024 | 92（83） |
| [ramokz/phantom-camera](https://github.com/ramokz/phantom-camera) | `21d3762` | 36 | 10,410 | 10,410 | 7（7） |
| [nathanhoad/godot_dialogue_manager](https://github.com/nathanhoad/godot_dialogue_manager) | `7a72dcf` | 79 | 14,619 | 14,619 | 240（240） |
| [bitbrain/beehave](https://github.com/bitbrain/beehave) | `fe58915` | 322 | 35,193 | 8,566 | 240（28） |
| [derkork/godot-statecharts](https://github.com/derkork/godot-statecharts) | `76d226a` | 141 | 18,768 | 6,852 | 87（70） |
| [MikeSchulze/gdUnit4](https://github.com/MikeSchulze/gdUnit4) | `9bc1a45` | 482 | 54,133 | 54,133 | 586（586） |
| [Maaack/Godot-Game-Template](https://github.com/Maaack/Godot-Game-Template) | `6849d6c` | 125 | 8,553 | 8,553 | 51（51） |
| **合计** | | **2,277** | **276,199** | **198,415** | **1,719（1,356）** |

“第一方”指仓库作者自己写的代码。如果仓库发布的产品本身就是一个插件（beehave、gdUnit4、Dialogue Manager、Godot State Charts、Phantom Camera，以及游戏模板里 Maaack 自己的插件），这个插件算第一方；godot-demo-projects 作为示例内容附带的插件也算第一方。随项目附带、由其他作者编写的插件不计入：beehave 中的 gdUnit4，godot-open-rpg 中的 Dialogic，godot-statecharts 中的 GUT，以及 Pixelorama 六个插件中作者无法从仓库判断的四个（aimg_io、dockable_container、gdgifexporter、SmartSlicer）。Pixelorama 的 keychain（同一组织）和 applinks（为 Pixelorama 编写）算第一方。属性访问器调查对第一方的界定更严，所以行数不同。

有四个仓库带测试套件：`beehave/test`、`gdUnit4/addons/gdUnit4/test`、`godot_dialogue_manager/tests` 和 `godot-statecharts/tests`，合计 35,226 行、625 个 await，接近第一方 await 的一半。因此第一方的结果分别给出含测试和不含测试两种。godot-demo-projects 中的 `physics_tests` 目录是示例，不是测试套件。

## 方法

1. **提取所有 await。** 脚本先把注释和字符串字面量的内容替换为空白，再找出所有 `await`：源码中共 1,858 个，其中 1,719 个在代码中。操作数按 GDScript 的方式解析。await 的优先级高于除调用、属性访问和下标之外的所有运算符（`469659e` 的 `modules/gdscript/gdscript_parser.h:1490`），所以操作数是一个基本表达式加上后面连着的调用、属性和下标。带括号、跨行和用反斜杠续行的写法都能处理。
2. **判断操作数的种类。** 以调用结尾的操作数，如果被调函数的函数体含有 await，就算协程调用；通过 `call()`、`callv()` 或 `Expression.execute()` 的动态调用、基类实现不含 await 的可重写方法的调用，以及对不含 await 的函数的调用，都算其他调用。以名字结尾的操作数，在接收者的类型中按信号查找。接收者的类型来自 self、类型标注、`as` 转换、`.new()`，以及引擎方法的返回类型和属性类型。用户信号取自语料中的 `signal` 声明，引擎信号取自类参考 XML（`469659e` 的 `doc/classes` 以及 `modules/*/doc_classes`、`platform/*/doc_classes`）。接收者类型未知时，例如 `$AnimationPlayer` 或自动加载的单例，如果同名的所有引擎信号和同一个 Godot 项目中声明的所有同名信号参数个数都相同，就凭名字确定这个信号。
3. **判断结果是否被使用。** 如果一个 await 开始一条语句（在行首、`;` 之后，或者代码块和单行 lambda 的 `:` 之后），而且它的操作数结束这条语句，它就是语句；否则它的结果被使用，脚本记下用法：变量初始化、赋值、返回、实参、条件等。
4. **需要判断的都逐个阅读，不抽样。** 全部 632 个信号 await 按每处一行列出后逐个检查参数个数以及语句还是被使用；结果被使用的 21 个信号 await 连同上下文和后面使用结果的行一起阅读；操作数既不是已确定的信号、也不是协程调用的 await 全部阅读。由此得到 27 条人工判断，记录在 `classification.json` 中：4 个有歧义的 `animation_finished`（在 AnimatedSprite2D/3D 上无参数，在 AnimationMixer 上有一个参数），根据场景文件确定；6 个在语料之外声明的信号（3 个在 C# 中，3 个在一个 Android 插件中）；15 个由 GUT 的 `wait_seconds()` 和 `wait_frames()` 返回的 Signal 值；2 个 GDScriptFunctionState 值。为确定这些情况，读了检出中没有的 3 个场景文件和 1 个 C# 文件。
5. **背景数据。** 为了看多参数信号本身是否少见，脚本还用同样的方法，按参数个数统计 `<信号>.connect(...)` 的调用处和 `signal` 声明。这些数字是自动得出的，没有逐个复核。

以上的阅读和分类工作由 AI 代理完成。

## 结果

### await 的操作数

| 操作数 | 全部 | 第一方 | 第一方，不含测试 |
|---|---|---|---|
| 信号，引擎类 | 508 | 361 | 331 |
| 信号，用户声明 | 82 | 58 | 54 |
| 信号，引擎或用户，凭名字确定 | 21 | 8 | 8 |
| 调用返回的 Signal 值 | 15 | 15 | 0 |
| 在语料之外声明的信号 | 6 | 5 | 5 |
| 协程调用 | 1,025 | 876 | 308 |
| 其他调用 | 59 | 31 | 24 |
| 其他 | 3 | 2 | 1 |
| **合计** | **1,719** | **1,356** | **731** |

折合每千行 6.22 个 await；第一方代码中是 6.83 个，不含测试是 4.48 个。测试代码以对测试框架的协程调用为主（625 个 await 中占 568 个），所以去掉测试后信号 await 的数量变化不大（447 → 398）。调用返回的 Signal 值全部来自 godot-statecharts 测试中 GUT 的 `wait_seconds()` 和 `wait_frames()`，它们返回一个无参数的信号。“其他”是 2 个 GDScriptFunctionState 值（其中 1 个在随附的第三方插件中）和 1 个被 await 的 lambda。

### 信号 await 按参数个数

每格为“语句 / 结果被使用”。

| 参数个数 | 全部 | 第一方 | 第一方，不含测试 |
|---|---|---|---|
| 0 | 590 / 2 | 415 / 1 | 366 / 1 |
| 1 | 19 / 18 | 17 / 12 | 17 / 12 |
| 2 个及以上 | 0 / 1 | 0 / 1 | 0 / 1 |
| 未知 | 2 / 0 | 1 / 0 | 1 / 0 |
| **合计** | **611 / 21** | **433 / 14** | **384 / 14** |

**整个语料中只有 1 个 await 的对象是有两个及以上参数的信号**，而且它使用了结果，约合第一方代码每千行 0.005 个。它的信号是在 C# 中声明的（例子 1）。有两个及以上参数的引擎信号和 GDScript 信号一次也没有被 await 过，作为语句也没有。“未知”一行是 gdUnit4 .NET API 的 C# 信号 `ExecutionCompleted`，以语句形式 await。

第一方代码中被使用的结果：

- **无参数，1 个**：一个 `return await`，在返回 Variant 的函数里返回 null（例子 5）。
- **一个参数，29 个中的 12 个**：7 个变量初始化（其中 5 个有类型标注）、4 个返回、1 个赋值。
- **两个及以上参数，1 个**：

| 位置 | 信号 | 参数 | 结果的用法 |
|---|---|---|---|
| `godot_dialogue_manager/addons/dialogue_manager/dialogue_manager.gd:1956` | `DialogueManager.Resolved`，在 C# 中声明（`addons/dialogue_manager/DialogueManager.cs:78`） | 2 个（`double id, Variant value`） | 存为 `Array`；`[0]` 与请求 id 比较，`[1]` 作为返回值 |

作为对照，第一方代码中 876 个协程调用 await 有 328 个使用了结果。

有一处不在上面的表中，因为它 await 的是协程调用：gdUnit4 为“按名字等待信号”的辅助函数写的测试，读取了一个三参数信号的 Array（`gdUnit4/addons/gdUnit4/test/core/GdUnitSignalAwaiterTest.gd:19`，例子 4）。

### 背景：多参数信号是被连接，而不是被 await

统计范围是第一方代码；引擎一列统计 `469659e` 类参考中的信号。

| 参数个数 | `.connect()` 调用处 | 其中在测试之外 | GDScript 信号声明 | 引擎信号 |
|---|---|---|---|---|
| 0 | 542 | 530 | 254 | 246 |
| 1 | 397 | 384 | 198 | 194 |
| 2 个及以上 | 94 | 87 | 64 | 78 |
| 未知 | 60 | 60 | – | – |

在参数个数已知的 `.connect()` 调用处中，两个及以上参数的信号占 9%（1,033 处中的 94 处）；在 GDScript 信号声明中占 12%（516 个中的 64 个）；而在引擎信号和 GDScript 信号的 await 中占 0。规则 C 的例子所用的 `HTTPRequest.request_completed`，在代码中有 4 处 `.connect()`，没有一处 await。

## 典型例子

**1. 唯一一个 await 多参数信号的地方。**
`godot_dialogue_manager/addons/dialogue_manager/dialogue_manager.gd:1950-1961`

```
func callv_dotnet(thing: Variant, method: String, args: Array) -> Variant:
    var dotnet_dialogue_manager: RefCounted = _get_dotnet_dialogue_manager()
    var id: float = _rng.randf()
    dotnet_dialogue_manager.ResolveThingMethod(id, thing, method, args)
    var x: int = 0
    while x < 1000:
        var result: Array = await dotnet_dialogue_manager.Resolved   # C#：ResolvedEventHandler(double id, Variant value)
        if result[0] == id:
            return result[1]
        x += 1

    return null
```

这个信号在 C# 中声明，接收者的类型标注是 RefCounted，所以在 await 处看不到静态签名；规则 A 到 C 针对的是参数静态已知的信号，决定不了这种情况。v0.1 让参数未知的信号保持 GDScript 的结果形状，类型为 `Variant?`；现行规范没有为它们另设规则。这个循环也说明了这里为什么需要实参：多个并发请求的响应共用一个信号，靠第一个实参来区分。

**2. 结果被使用时，通常是一个有类型的值。**
`godot-open-rpg/src/field/cutscenes/templates/combat/combat_trigger.gd:7-11`

```
func _execute() -> void:
    FieldEvents.combat_triggered.emit(combat_arena)

    var did_player_win: bool = await CombatEvents.combat_finished   # 单参数信号
```

§9.6 按信号的参数为这个结果定型。

**3. 多参数信号先被包装，再被 await。**
`gdUnit4/addons/gdUnit4/src/update/GdUnitUpdateClient.gd:4-69`

```
signal request_completed(response: HttpResponse)

var _http_request := HTTPRequest.new()

func _ready() -> void:
    add_child(_http_request)
    _http_request.request_completed.connect(_on_request_completed)

func request_latest_version() -> HttpResponse:
    var error := _http_request.request("https://api.github.com/repos/MikeSchulze/gdUnit4/tags")
    ...
    return await self.request_completed

func _on_request_completed(_result: int, response_http_status: int, _headers: PackedStringArray, body: PackedByteArray) -> void:
    ...
    request_completed.emit(HttpResponse.new(response_http_status, body))
```

`HTTPRequest.request_completed` 的四个实参进入一个有类型的处理函数，处理函数发出一个带对象的单参数信号，被 await 的是这个信号。这正是规则 A 要求的写法。尽管 GDScript 允许直接写 `await _http_request.request_completed`，作者还是选择了这种写法。

**4. Array 形状出现在测试框架按名字等待信号的地方。**
`gdUnit4/addons/gdUnit4/src/core/GdUnitSignalAwaiter.gd:19-60` 和 `gdUnit4/addons/gdUnit4/test/core/GdUnitSignalAwaiterTest.gd:19-20`

```
func _on_signal_emmited(
    arg0: Variant = NO_ARG,
    ...
    arg9: Variant = NO_ARG) -> void:
    var signal_args: Variant = GdArrayTools.filter_value([arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9], NO_ARG)
    signal_emitted.emit(signal_args)

func on_signal(source: Object, signal_name: String, expected_signal_args: Array) -> Variant:
    source.connect(signal_name, _on_signal_emmited)
    ...
        value = await signal_emitted

# 这个辅助函数的测试中
var signal_args: Variant = await await_signal_on(monster, "slide", [1.0, 1, 2])
assert_array(signal_args).is_equal([1.0, 1, 2])
```

gdUnit4 连接一个有十个可选参数的回调，自己把实参打包成数组。信号是以字符串形式按名字传入的，所以无论选哪条规则，它的参数在静态时都是未知的。gdUnit4 按名字等待信号的辅助函数在第一方代码中被 await 了 93 次；除了辅助函数内部把值往外传，只有这个测试的调用方读取了结果（第 11 行和第 19 行），其中只有第 19 行是多参数信号。

**5. §9.6 会拒绝的无参数结果。**
`gdUnit4/addons/gdUnit4/src/GdUnitAwaiter.gd:10-18`

```
func await_signal_on(source: Object, signal_name: String, args: Array = [], timeout: int = 2000) -> Variant:
    ...
    if not is_instance_valid(source):
        ...
        return await (Engine.get_main_loop() as SceneTree).process_frame
```

这个函数等一帧之后返回 null。按 §9.6，无参数的信号不产生值，结果不能使用；改写成一条 `await` 语句加上 `return null` 即可。这是第一方代码中唯一的这类情况；beehave 附带的 gdUnit4 中有一份副本。

## 结论

- **使用多参数信号的 await 结果的代码几乎不存在。** 第一方代码的 1,356 个 await 中只有 1 个，约合每千行 0.005 个。它的信号在 C# 中声明，并且是通过类型为 RefCounted 的接收者访问的，所以参数在静态时未知，选哪条规则都不影响它。有两个及以上参数的引擎信号和 GDScript 信号从未被 await，作为语句也没有。
- **多参数信号很常见，但它们是被连接的。** 需要其实参的代码会连接一个处理函数（94 处）；如果之后还想 await，就再发出一个带对象的单参数信号。
- **规则 C 在这批语料中没有服务对象。** 规则 A 在这里没有代价，而且 C 以后可以在不破坏已有代码的前提下加入，因为它只是给 A 拒绝的代码赋予含义。数据支持第一版保留 A，把 C 留待以后；B 仍然否决。现行规范就是这样规定的（§9.6，附录 B）。
- **信号的 await 结果本来就很少被使用**：第一方代码 447 个信号 await 中只有 14 个，而协程调用 await 是 876 个中的 328 个。这 14 个中有 12 个是单参数信号。
- **关于无参数规则的一个附带发现**：第一方代码中有 1 个 await 使用了无参数信号的结果，用来返回 null。§9.6 会拒绝这种写法，改写很简单。

## 局限

- **语料以工具、插件和示例为主。** 与 Web 服务或游戏服务器通信的代码偏少，而这类代码里 await `request_completed` 会很自然：语料中的 HTTP 代码只出现在插件更新器和一个调色板下载器中。这类代码 await 多参数信号的频率可能更高。
- **只分析了 GDScript。** 场景文件中建立的连接和用 C# 编写的代码都没有分析；为确定个别情况，读了 1 个 C# 文件和 3 个场景文件。
- **接收者类型来自一个简单的解析器。** 解析失败时，只有当所有同名信号的参数个数一致时，才凭名字确定信号；不一致的 4 个已人工确定。
- **结果是否被使用按语法判断。** 全部 632 个信号 await 都在每处一行的列表中检查过，被使用的 21 个还结合上下文读过。
- **协程的判断是近似的。** 函数体含有 await 就算协程。这只影响“协程调用”和“其他调用”两行。
- **有些代码是重复的。** Godot-Game-Template 把它的示例场景复制到了 `scenes/`，godot-demo-projects 的 2.5D 示例在 GDScript 和 C# 两个版本中都带有同样的编辑器脚本。重复部分中的 await 都没有使用结果。
- **`.connect()` 调用处和声明的统计是自动得出的**，没有逐个复核，个别凭名字确定的信号可能有误。
- **分类工作由 AI 代理完成**，“方法”一节列出的情况它都逐个读过。

## 复现

脚本放在 [`await-signal-results/`](await-signal-results/) 目录：`extract.py` 找出并分类所有 await，输出上面的各个表格；`dump.py` 打印选定的 await 及其上下文；`classification.json` 记录人工判断，每条都附有依据。引擎信号取自 `469659e` 的 Godot 检出。

```
mkdir corpus && cd corpus
git clone https://github.com/godotengine/godot-demo-projects
git -C godot-demo-projects checkout b761b4c
# 对其余仓库重复以上两步，提交见“语料”一节的表格
cd ..
python docs/surveys/await-signal-results/extract.py corpus ../godot          # 输出表格，写入 corpus/awaits.json 和 corpus/connects.json
python docs/surveys/await-signal-results/dump.py corpus used-signals > used.txt   # 结果被使用的信号 await 及其上下文
```

`dump.py` 还接受 `all`、`signals`、`used`、`multi`、`unknown`、`byname`、`nonsignal` 和 `manual`。只检出 `.gd` 文件的稀疏检出得到的统计相同，因为文件不存在时，脚本会从 git 树中读取 `project.godot` 的位置。
