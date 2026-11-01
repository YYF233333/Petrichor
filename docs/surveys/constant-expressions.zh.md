# 调查：常量表达式

**2026-11-01** · 关联：[语言规范](../language-spec.zh.md) §4.5 · English: [constant-expressions.en.md](constant-expressions.en.md)

## 问题

GDScript 借引擎在编译期求出常量的值，本语言的编译器不依赖引擎。本文统计真实代码中常量的初始化式需要怎样的求值，以及哪些位置在编译期就要用到常量的值，供规范 §4.5 决定哪部分由编译器求值、哪部分在类加载时由引擎求值。

## 语料

与[跨分支重复调查](cross-branch-duplication.zh.md)相同：20 个仓库，固定提交，第一方代码的判定规则也相同。第一方 `.gd` 文件 2,800 个，其中 12 个游戏仓库 1,223 个。

## 方法

- 按词法找出 `const` 声明，按初始化式的形式分类，每个声明归入第一个符合的类别。
- 再按求值所需分类：
  - 不用计算：字面量、名字、`preload`，只存放分量的构造（如 `Vector2(1, 2)`、`Plane(Vector3.UP, 0)`），以及由它们组成的数组和字典；
  - 标量计算：int、float、bool、String 的运算，`int()`、`float()`，返回 int 或 bool 的数学函数；
  - 引擎类型的运算：运算的一边出现内置类型的构造或常量，或同一文件中以它们初始化的常量；
  - 返回 float 的数学函数；
  - 由字符串构造 Color，并检查字符串是否是十六进制写法。
- 在编译期要用到值的位置：注解参数和 enum 的值；另计 match 模式和默认参数值中的常量。
- 其他用法：preload 脚本的常量怎样使用，从其他文件访问常量，写入常量中的容器。

## 结果

3,094 个常量，分布在 835 个文件中（30%）；游戏仓库中 1,474 个，350 个文件。

### 初始化式的形式

| 形式 | 全部 | 游戏 |
|---|---:|---:|
| 字面量（int 510、float 362、String 1,101、bool 24） | 1,997 | 987 |
| `&"…"` | 130 | 50 |
| `preload`：脚本 208、场景 109、其他资源 92 | 409 | 179 |
| 数组字面量 | 106 | 49 |
| 字典字面量 | 84 | 49 |
| 内置类型的构造（Color 117、Vector2 21、Vector2i 6、Vector3 4、Plane 3 等） | 156 | 101 |
| 内置类型的常量，如 `Color.RED` | 8 | 7 |
| 名字或带运算的表达式 | 198 | 48 |
| 工具函数（`deg_to_rad` 4、`tan(deg_to_rad(…))` 1、`int()` 1） | 6 | 4 |

### 求值所需

| 求值所需 | 全部 | 游戏 |
|---|---:|---:|
| 不用计算 | 2,920 | 1,434 |
| 标量计算 | 141 | 35 |
| 由十六进制字符串构造 Color | 24 | 2 |
| 返回 float 的数学函数 | 5 | 3 |
| 引擎类型的运算 | 4 | 0 |

- 标量计算如 `24 * 60 * 60 * 1000`、`1 << 14`、`1.0 / 60.0 * 30.0`、`MOUSE_BUTTON_MASK_LEFT | MOUSE_BUTTON_MASK_RIGHT`、`GROUP_TEST + "/flaky_check_enable"`（gdUnit4 一个文件中有 60 处这样的拼接）、`int(1.10 * 60 * 1000)`。
- 引擎类型的运算只有四处：`Vector3.ONE / 2.0`、`Vector3(0.5, 0.5, 0.5) * Chunk.CHUNK_SIZE`、`Color.WHITE * Color(1, 1, 1, 0.5)`（均在 godot-demo-projects），以及 Pixelorama `Drawers.gd:39` 数组中的 `-Vector2i.ONE`。
- 返回 float 的数学函数是 `deg_to_rad` 4 处、`tan(deg_to_rad(30))` 1 处。
- 由字符串构造的 Color 全部是十六进制写法，没有颜色名。
- 没有出现的写法：`%` 格式化、`is`、`as`、`in`、静态方法、GDScript 自己的工具函数（`Color8`、`len`、`char`、`ord` 等），以及 Basis、Quaternion 等的构造。

### 在编译期要用到值的位置

- 1,622 个带参数的注解中，7 个用到本仓库的常量：6 个 `@export_placeholder`、1 个 `@export_enum`。这些常量都以字面量初始化，例如 beehave `leaf.gd:6` 的 `const EXPRESSION_PLACEHOLDER: String = "Insert an expression..."`。另有 11 个注解用到引擎常量。
- 140 个写出值的 enum 成员中，136 个是字面量，4 个是字面量的移位，没有一个用到常量。
- match 模式中写作常量名的 339 处（单个名字 181、带点 158），默认参数值中用到常量的 56 处。

### 其他用法

- 写入常量中的容器：0 处。
- preload 脚本的 208 个常量中，在同一文件里用作类型 140 处，`X.new()` 55 处，调用静态方法 111 处。
- 从其他文件以 `ClassName.CONST` 访问常量 1,722 处，在 263 个文件中；`self.CONST` 721 处，全在 tanks-of-freedom-ii；经实例访问约 35 处。

各类别的抽样位置见 `constant-expressions/results/constants.txt`，需要引擎求值的初始化式在其中全部列出。

## 局限

- 按词法识别，不做类型解析。引擎类型只凭同一表达式中的内置类型名和同一文件中以构造初始化的常量判断，经其他文件的常量算出的引擎类型值会被算作标量计算。
- 继承来的常量在内部类中漏计；经实例的访问是粗略的估计。
- 只统计 GDScript 中已有的写法。

## 所评估的设计

规范 §4.5：编译器求出标量，引擎类型的运算和返回 float 的数学函数在类加载时由引擎求值，加载时可能因值出错的情形都在编译期检查。按这个设计，3,094 个常量中 3,061 个在编译期得到值，需要引擎求值的 33 个；编译期要用到值的位置用到的常量都是以字面量初始化的标量；规范不支持的写法在语料中都没有出现。

## 复现

```
python docs/surveys/cross-branch-duplication/gdcorpus.py corpus
python docs/surveys/constant-expressions/constants.py corpus thirdparty/godot-cpp/gdextension/extension_api-4-7.json > docs/surveys/constant-expressions/results/constants.txt
```
