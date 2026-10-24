# 调查：语料中的零散计数

**2026-10-25** · 关联：[语言规范](../language-spec.zh.md) · English: [corpus-counts.en.md](corpus-counts.en.md)

规范中的个别规则只需要一个计数作依据。本文收集这类计数，语料与[跨分支重复调查](cross-branch-duplication.zh.md)相同：20 个仓库，固定提交，只计第一方 `.gd` 文件，共 203,720 行代码（不计空行和注释行）。“用于”一列是规范中以该计数为依据的章节。

| 计数 | 结果 | 用于 |
|---|---|---|
| `null` 字面量 | 4,521 处，每千行 22.2 处；其中比较（`== null`、`!= null`）2,541 处，赋值、返回和传参 1,980 处 | §2.2：`null` 是 None 的别名 |
| 前缀 `!` 与 `not` | 前缀 `!` 460 处，`not` 4,504 处（含 `not in`） | 附录 B：不采用后缀 `!` 的背景；前缀 `!` 仍是逻辑非（§2.2） |
| 在 `_ready` 中把节点登记到 autoload 成员上（`Autoload.x = self`） | 6 处，其中 5 处在 Ring | §6.2：autoload 引用场景节点的成员写作 `T?`（脚本见 `paper-migration-2/autoload_self.py`） |
| `@abstract` 类和方法 | 28 个类：gdUnit4 25 个，Pixelorama 2 个，godot-open-rpg 1 个；250 个抽象方法；以文件为单位的抽象类中，17 个的方法全部是抽象的 | §4.1：`@abstract` |
| 剩余参数 `...args` | 78 个：gdUnit4 76 个，Pixelorama 2 个 | §4.6：剩余参数 |
| 含 await 的 lambda | 979 个 lambda 中 35 个，其中 33 个在 gdUnit4 的测试中 | §9.1：lambda 也写 `async func` |
| 作为值使用的协程方法（同一文件中按名字作为实参传递） | 91 处，其中 60 处是 `connect(名字)`，分布在 12 个仓库 | §3.11：由引擎调用的位置接受 async 函数 |
| `assert(...)` 语句 | 595 个，其中 406 个在 godot_dialogue_manager；实参中含 await 的 0 个 | §8.3：`assert` 在 release 构建中去掉 |
| `is_instance_valid(...)` | 678 处，每千行 3.3 处；phantom-camera 中每千行 12.8 处 | §3.3：经 `T?` 读到已释放的对象时为 None |
| `static var` 与 `_static_init` | `static var` 188 处，`_static_init` 4 个 | §4.8：静态成员沿用 GDScript 的写法和时机 |
| `NOTIFICATION_PREDELETE` | 40 处，其中 32 处在 gdUnit4 | §6.6：析构不新增语法 |
| `Thread.new` 与 `WorkerThreadPool` | `Thread.new` 5 处，`WorkerThreadPool` 2 处 | §12.3：第一版不新增线程 API |
| 自身的 `get_parent()` 与 `owner` | `get_parent()` 174 处，其中直接访问成员 62 处，分布在 46 个文件；判断为空 3 处，经阅读都在处理没有父节点的情况，其中 beehave 的 `beehave_tree.gd:91` 在节点挂到父节点之前执行的 setter 中调用 `get_parent()`。`owner` 直接访问成员 49 处，分布在 21 个文件；判断为空 0 处 | §7.3：未声明 `@parent`、`@owner` 时，`get_parent()` 和 `owner` 的类型为 `Node?` |

计数基于词法分析，不做类型解析；`!` 只计不紧跟 `=` 的。`...` 在 GDScript 中只出现在函数签名里（`modules/gdscript/gdscript_parser.cpp:1714, 4336`），所以紧跟名字的 `...` 都是剩余参数。协程指函数体中有 await 的方法，只在同一文件中查找对它的引用，不计场景中的信号连接。`get_parent()` 和 `owner` 只计在自身上的使用，即前面没有 `.` 的；判断为空包括与 null 比较、用作条件和传给 `is_instance_valid`，以及在其后 25 行内对赋得其值的变量做这些判断。

## 复现

```
python docs/surveys/cross-branch-duplication/gdcorpus.py corpus
python docs/surveys/corpus-counts/counts.py corpus
python docs/surveys/paper-migration-2/autoload_self.py corpus
```
