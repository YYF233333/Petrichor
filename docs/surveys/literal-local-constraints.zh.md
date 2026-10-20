# 调查：只用字面量初始化的局部变量会遇到哪些数值类型

**2026-10-21** · 关联规范：[语言规范](../language-spec.zh.md) §3 R4 · English: [literal-local-constraints.en.md](literal-local-constraints.en.md)

本文记录一次小规模的实证调查。我们用它来确定规范中局部变量类型推断的一条规则。之所以单独写成文档，是因为这套方法适用于很多类似的设计问题：先在真实代码上统计问题出现的频率，再逐条比较各个候选规则在这些代码上分别会产生什么结果，最后才做决定。

## 问题

本语言有多种定宽数值类型（i8…i64、u8…u64、f32、f64），字面量是柔性的：它会跟随另一个操作数或期望类型来定型。隐式的有损转换允许，但会给出警告，分为两类：精度损失（默认关闭）和值域损失（默认开启）。

只用字面量初始化的局部变量，例如 `var a = 1`，后面可能同时遇到几个类型不同的约束：

```
var a = 1
takes_i32(a)
takes_i64(a)
```

`a` 无论取 i32 还是 i64，现有的转换规则都能放行，区别只在警告不同，所以规范必须给出一个唯一的答案。候选规则有三条：

| 规则 | 做法 | 采用者 |
|---|---|---|
| A 冲突即报错 | 在整个函数范围内收集约束，全部一致才采用，否则要求标注 | Rust、Haskell |
| B 只看初始化式 | 类型由初始化式单独决定（字面量取默认类型），之后的使用都按隐式转换处理 | Swift、Kotlin、C#、Go |
| C 先看流入 | 类型取初始化式和所有赋给该变量的值的无损公共类型；变量往外流出时按隐式转换处理 | 思路接近 Pyright、Crystal：变量的类型由所有赋给它的值决定 |

要回答的问题有三个：真实的 Godot 代码里多约束出现得有多频繁？是哪些类型组合？每条规则分别会产生什么结果？

## 语料

我们选了 6 个 MIT 许可的 Godot 4 项目，涵盖官方示例、小型游戏模板和一个 RPG 框架。行数中的“第一方”指不计随项目附带的 `addons/` 目录。

| 仓库 | 提交 | `.gd` 文件 | 行数 | 第一方行数 | 候选变量（第一方） |
|---|---|---|---|---|---|
| [godotengine/godot-demo-projects](https://github.com/godotengine/godot-demo-projects) | `b761b4c` | 461 | 30,819 | 28,577 | 27（24） |
| [gdquest-demos/godot-open-rpg](https://github.com/gdquest-demos/godot-open-rpg) | `d59f2f7` | 342 | 40,662 | 5,422 | 80（3） |
| [KenneyNL/Starter-Kit-3D-Platformer](https://github.com/KenneyNL/Starter-Kit-3D-Platformer) | `3fa8a04` | 9 | 378 | 378 | 0 |
| [KenneyNL/Starter-Kit-City-Builder](https://github.com/KenneyNL/Starter-Kit-City-Builder) | `4535092` | 6 | 311 | 311 | 0 |
| [KenneyNL/Starter-Kit-FPS](https://github.com/KenneyNL/Starter-Kit-FPS) | `185fd23` | 7 | 451 | 451 | 0 |
| [KenneyNL/Starter-Kit-Racing](https://github.com/KenneyNL/Starter-Kit-Racing) | `2f2e5f2` | 3 | 258 | 258 | 1（1） |
| **合计** | | **828** | **72,879** | **35,397** | **108（28）** |

godot-open-rpg 的 80 个候选中有 77 个来自它附带的 Dialogic 插件。原计划还包括 Cogito，但调查时克隆失败（仓库不存在），没有计入。

## 方法

1. **提取候选。** 用脚本找出函数体内形如 `var 名字 = 字面量` 或 `var 名字 := 字面量` 的声明，字面量包括带符号的整数、浮点、十六进制和二进制形式。类成员不计入，因为规范 R6 已经规定成员的类型只看标注或初始化式。
2. **逐个阅读，不抽样。** 对每个候选，打印它所在函数中所有提到该变量的行，然后逐个判断它受到哪些约束。108 个全部读过。
3. **约束的定义。**
   - 流入：赋给该变量的值、复合赋值右侧的值。
   - 流出：作为实参传给类型已知的参数，赋给类型已知的属性、成员或变量，作为比较或二元运算的一侧（另一侧类型已知），作为有标注的返回值。
4. **类型映射。** 按引擎 C++ 签名换算：`double` 为 f64；`float` 为 f32；`real_t` 为 f32（标准构建）；`int` 为 i32；`int64_t` 为 i64；`uint64_t` 为 u64；`uint32_t` 为 u32。几个典型位置：
   - `delta` 是 f64。
   - Node2D、Node3D、Control 的变换类属性，以及 Color 分量，都是 f32。
   - `lerpf`、`clampf`、`move_toward`、`randf` 等全局函数是 f64；`wrapi`、`clampi`、`randi_range` 等是 i64。
   - `Array.size()`、`String.length()`、`get_child_count()` 是 i32；Packed 数组的 `size()` 是 i64。

   以下情况不算约束：数组下标、`range()`、枚举常量。用户代码中标注为 `int` 和 `float` 的，分别按 i32 和 f64 计；没有标注的用户参数和成员，类型视为未知。
5. **计算结果。** 对每个变量，记下约束类型的集合。对多约束的变量，分别按 A、B、C 三条规则算出结果；对全部变量，统计每条规则产生的编译错误数和警告数。

逐个分类的工作由 AI 代理按上述映射完成，引用的代码片段和统计数字经过人工复核。

## 结果

### 约束的分布

| 约束的类型种数 | 变量数 |
|---|---|
| 0（没有已知类型的约束） | 42 |
| 1 种 | 61（i32 42、f64 8、f32 5、i64 3、u32 3） |
| 2 种及以上 | **5** |

多约束变量占全部候选的 4.6%，约合每千行 0.07 个；只看第一方代码是每千行 0.14 个，5 个全部在第一方代码中。其中 2 个之所以成为多约束，是因为我们把 `const ACCELERATION = 4` 这类没有标注类型的常量按字面量的默认类型 i32 计算。如果这类常量像字面量一样是柔性的，多约束只剩 3 个。

### 多约束的类型组合

| 组合 | 个数 | A | B | C |
|---|---|---|---|---|
| f64 流入，流向 f32 的引擎 API | 2 | 报错 | f64，精度损失（默认不报） | 同 B |
| i32 流入（`int()`），与 f64 运算，流向 f32（`rotation`） | 1 | 报错 | i32，i32→f32 精度损失（默认不报） | 同 B |
| 从 `const X = 4` 流入 i32，与 f64（`delta`）运算 | 1 | 报错 | f64，无警告 | f64，无警告 |
| 多处 f64 流入，与 `const BPM = 116`（i32）运算 | 1 | 报错 | f64，无警告 | f64，无警告 |

### 各规则在全部 108 个变量上的结果

| 规则 | 编译错误 | 带默认开启的值域损失警告的变量 | 只有默认关闭的精度损失的变量 |
|---|---|---|---|
| A 冲突即报错 | 6（若常量是柔性的则为 4） | 0 | 0 |
| B 只看初始化式 | 0 | 4（3 个 i32→u32，1 个 f64→i32） | 8 |
| C 先看流入 | 0 | 3（都是 i32→u32） | 8 |
| **最终采用：C 加上“看流向”的回退** | **0** | **0 或 2**（见下文） | ≤ 8 |

A 的 6 个错误中，5 个来自上表的多约束变量，另 1 个是浮点字面量和 i32 约束的类别冲突（`var wall_side := 0.0` 与一个 i32 比较）。**这 6 个错误没有一个对应真正的 bug。**

B 和 C 只在一个变量上结果不同，而那里 B 是错的（见下面的例子 2）。

C 剩下的 3 条值域损失警告，都是一个整数变量只是被传给 u32 参数：

- 1 个是从未被重新赋值的 `var z_groups := 1`。
- 2 个是 Dialogic 中的 `var flags := 0`，之后只会与引擎枚举常量做按位或。

这些值在实际中不会出问题，于是我们在 C 的基础上加了一条回退：**如果除初始化式之外没有类型已知的值流入，就看变量流向哪里；流向的类型全部一致就取那个类型，否则取默认类型。**

- 按这条回退，`z_groups` 直接取 u32，不再警告。
- 两个 `flags` 的结果取决于引擎枚举常量算作什么类型，规范目前把这一点列为【待定】：
  - 如果枚举常量像字面量一样是柔性的，`flags | SEARCH_MATCH_CASE` 就不提供类型信息，`flags` 取 u32，不警告。
  - 如果枚举常量的类型是 i64，`flags` 就会取 i64，仍然产生 2 条警告。
- 加上这条回退后，警告不会比 C 更多：流向一致时，所有使用处都不需要转换；流向不一致时，结果与 C 相同。表中的“≤ 8”就是由此推出的上限，没有逐个重新统计。

## 典型例子

**1. 最常见的形态：f64 流入，流向 f32。**
`godot-demo-projects/audio/spectrum/show_spectrum.gd:52-60`

```
var prev_hz := 0.0
for i in range(1, VU_COUNT + 1):
    var hz := i * FREQ_MAX / VU_COUNT                                      # f64
    var magnitude := spectrum.get_magnitude_for_frequency_range(prev_hz, hz).length()   # 参数是 float，即 f32
    ...
    prev_hz = hz
```

A 会要求在这里写标注；C 取 f64，传参时产生默认不报的精度损失。GDScript 在引擎边界上也是这样悄悄转换的。

**2. B 会得出错误的结果。**
`godot-open-rpg/src/field/gamepieces/animation/gamepiece_animation.gd:98-101`

```
var current_position_ratio = 0
if keep_position:
    current_position_ratio = \
        _anim.current_animation_position / _anim.current_animation_length   # double
```

按 B，变量被定为 i32，这个比值会被截断成 0，而作者本意显然是要一个比值。A 和 C 都得到 f64。

**3. 只流向 u32。**
`godot-demo-projects/compute/post_shader/post_process_grayscale.gd:58, 87`

```
var z_groups := 1
...
rd.compute_list_dispatch(compute_list, x_groups, y_groups, z_groups)   # 参数是 uint32_t
```

C 产生 i32→u32 的值域损失警告；加上回退后取 u32，不再警告。

**4. 流入只来自枚举常量。**
`godot-open-rpg/addons/dialogic/Editor/TimelineEditor/TextEditor/timeline_editor_text.gd:249-261`

```
var flags := 0
if match_case:
    flags = flags | SEARCH_MATCH_CASE
...
set_search_flags(flags)                                  # uint32_t
var result := search(search_text, flags, ...)            # uint32_t
```

结果取决于引擎枚举常量算作什么类型，见上文。

## 结论

- **多约束很少见**，约占字面量初始化局部变量的 3% 到 5%，不到每千行 0.1 个。最典型的形态是 f64 流入、流向 f32 的引擎 API。
- **A 带来的是没有收益的标注负担**：它在真实代码中报出的错误，没有一个对应实际的 bug。
- **B 会改变程序的含义**：先用整数字面量初始化、再赋一个浮点值的变量会被截断。
- **C 最贴合真实代码**，再加上“看流向”的回退，消除了剩下那些无害的警告。规范的 R4 就是这样写的。

## 局限

- **样本较小，且偏向写了类型标注的代码。** 语料大多是写了类型标注的现代 GDScript：写成 `var x: float = 0.0` 这类的局部变量有 37 个，不在统计之内。在不写类型的老项目里，多约束的比例可能更高。不过迁移到本语言时，AI 辅助迁移本来就会补上标注。
- **第一方样本偏少。** 108 个候选中有 77 个来自同一个插件 Dialogic。
- **类型映射靠人工。** 引擎 API 的类型是按 C++ 签名人工对照的，个别地方可能把 `float` 和 `double` 判断反了。
- **部分统计依赖尚未确定的规则。** 无标注常量和引擎枚举常量如何定型，规范中尚未确定，这会影响部分计数。上文已经分别给出两种假设下的结果。
- **动态取节点的类型未知。** 通过 `$` 或 `%` 取到的节点，类型按未知处理。如果它们能被静态定型，Dialogic 中还会多出 1 个多约束变量（f32 流入、经由 `max` 流入 i32、流向 f32）：B 会对它给出默认开启的浮点→整数警告，C 则取 f64。
- **B 与 C 的差别只有一个数据点。** 但这个差别涉及程序含义是否被改变，而不只是多一条警告。

## 复现

脚本放在 [`literal-local-constraints/`](literal-local-constraints/) 目录：`extract.py` 负责提取候选，`dump.py` 打印每个候选在函数中的上下文。分类需要人工按“方法”一节中的映射完成。

```
mkdir corpus && cd corpus
git clone https://github.com/godotengine/godot-demo-projects
git -C godot-demo-projects checkout b761b4c
# 对其余仓库重复以上两步，提交见“语料”一节的表格
cd ..
python docs/surveys/literal-local-constraints/extract.py corpus           # 输出各仓库统计，写入 corpus/locals.json
python docs/surveys/literal-local-constraints/dump.py corpus > dump.txt   # 每个候选及其上下文
```
