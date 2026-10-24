# 调查：语料中的零散计数

**2026-10-25** · 关联：[设计决定](../decisions.zh.md) · English: [corpus-counts.en.md](corpus-counts.en.md)

个别设计决定只需要一个计数作依据。本文收集这类计数，语料与[跨分支重复调查](cross-branch-duplication.zh.md)相同：20 个仓库，固定提交，只计第一方 `.gd` 文件，共 203,720 行代码（不计空行和注释行）。

| 计数 | 结果 | 用于 |
|---|---|---|
| `null` 字面量 | 4,521 处，每千行 22.2 处；其中比较（`== null`、`!= null`）2,541 处，赋值、返回和传参 1,980 处 | 设计决定 §9：`null` 是 None 的别名 |
| 前缀 `!` 与 `not` | 前缀 `!` 460 处，`not` 4,504 处（含 `not in`） | 讨论后缀 `!` 时的参考 |
| 在 `_ready` 中把节点登记到 autoload 成员上（`Autoload.x = self`） | 6 处，其中 5 处在 Ring | 设计决定 §3：autoload 引用场景节点的成员写作 `T?`（脚本见 `paper-migration-2/autoload_self.py`） |

计数基于词法分析，不做类型解析；`!` 只计不紧跟 `=` 的。

## 复现

```
python docs/surveys/cross-branch-duplication/gdcorpus.py corpus
python docs/surveys/corpus-counts/counts.py corpus
python docs/surveys/paper-migration-2/autoload_self.py corpus
```
