# Translating the specification

`docs/language-spec.md` is the English edition of `docs/language-spec.zh.md`. It is generated, never edited by hand. The Chinese edition is the only source of rules, and this procedure never changes it.

## Inputs

- `docs/language-spec.zh.md`: the source.
- `docs/language-spec.md`: the previous output, with the blob id of the source it was made from.
- The glossary below: the fixed English term for each recurring Chinese term.
- `README.md`: its English terms are authoritative for the terms it uses.
- The diagnostics in `src/compiler/*.cpp`: where they use a term, the specification uses the same term.

## Rules

- Keep the structure: every heading, list item, table row, code block and example, in the same order. Headings keep their section numbers.
  - One line of the Chinese edition is one line of the English edition.
  - After the title block, line n of the Chinese is line n + 2 of the English.
- Add nothing, drop nothing, and keep the normative force.
  - A definition (即, 指) keeps an explicit "that is" or "means".
  - A reason (因为, 因此, 所以) keeps an explicit "because" or "so", also after a split.
  - A condition stays next to what it limits: write "Before ready, reading X is an error".
  - A split-off condition starts with "Suppose" or "When", never with "may" or "can", which read as permission.
  - 须 and 必须 are "must". 可 is "may" or "can". 应 is "should". 不能 is "cannot".
  - 报编译错误 is "is a compile error". 报程序错误 is "is a program error".
  - 给出警告 is "gives a warning". 给出提示 is "gives a hint".
  - An error without a kind in Chinese (报错) stays without a kind: "is an error" or "reports an error".
- 【待定】 becomes `**[Open]**`. 【已知限制】 becomes `**[Known limitation]**`. Each stays at the same place in its item.
- Copy code blocks byte for byte, with two exceptions inside them.
  - Translate Chinese comments, and keep the column where each comment starts.
  - Leave identifiers and string literals as they are, Chinese string literals included.
- Translate Chinese placeholders in inline code, such as `expect(消息)` to `expect(message)`.
- Follow the house style of `AGENTS.md`, "Documentation".
  - Write one idea per sentence. The style check flags sentences over 30 words.
  - Where one Chinese sentence needs more, split it into two sentences without changing the meaning.
  - A parenthesis that makes a sentence too long becomes the next sentence.
  - The words of a status marker count toward the sentence that follows it.
  - Do not turn running text into new list items: the two editions keep the same lines.
  - Put the condition before the result.
  - Put links at the end of the sentence.
  - Refer to another rule by its concept or its section title, never by section number.
- Keep the links to `design-notes.md`, `engine-facts.md` and `architecture.md` as they are.
- The opening paragraph adds three statements that the Chinese edition lacks.
  - This edition is generated from the Chinese one.
  - The Chinese edition prevails where the two differ.
  - The linked documents are in Chinese only.
- The line after the title block records the source, in exactly this form:

  ```
  <!-- Generated from language-spec.zh.md, git blob <40-hex blob id>. Do not edit by hand; see tools/spec_translation.md. -->
  ```

- Where the Chinese has two readings that behave differently, translate literally and report the place. Do not fix the specification.

## Procedure

1. Read `AGENTS.md`, `README.md` and this file.
2. Read the recorded blob id from the comment in `docs/language-spec.md`.
3. Run `git diff <recorded blob id> docs/language-spec.zh.md` to see what changed in the Chinese edition.
4. For each changed hunk, find the same item in the English edition: same heading, same position in its list.
5. When a hunk brings a new recurring term, add it to the glossary first.
   - Look for the term in `README.md` and in the compiler diagnostics before choosing one.
6. Translate only the changed items again, with the glossary. Leave the other items untouched.
7. Run `git hash-object docs/language-spec.zh.md` and write the result into the comment.
8. Run the checks below until they report nothing.

With no previous output, translate every section in order, in one session, and build the glossary first.

## Checks

- `python tools/check_doc_style.py --all docs/language-spec.md tools/spec_translation.md` reports nothing.
- `python tools/check_spec_translation.py` exits with zero.
- These counts are equal in the two editions:
  - lines, apart from the two lines of the recorded source;
  - headings, per level;
  - list items;
  - fenced code blocks;
  - table rows, per table;
  - 【待定】 against `**[Open]**`, and 【已知限制】 against `**[Known limitation]**`.
- Each line has the same inline code as its Chinese line, apart from translated placeholders.
- Each line has the same numbers and the same link targets as its Chinese line.
- Outside code blocks, the English edition has no Chinese character.
- Inside code blocks, only comments differ between the two editions.
- Reread the changed sections against the Chinese, sentence by sentence.

## Glossary

One Chinese term has one English term. A term from Godot or GDScript keeps its own English name.

### Rules and diagnostics

| Chinese | English | Note |
|---|---|---|
| 本语言 | this language | Never "Petrichor" inside a rule. The compiler says "a class of this language". |
| 其他语言的类 | class of another language | Compiler term. |
| 无静默差异 | no silent differences; the no-silent-differences rule | |
| 可还原规则 | the reversibility rule | |
| 可还原的写法 | reversible form | |
| 写法 | form; "written as" | "Spelling" only for the text of a literal. |
| 规范性 | normative | |
| 合法 | legal | Not "valid". |
| 第一版 | the first version | It means 1.0. The README's "first public release" is 0.1. |
| 编译错误 | compile error | |
| 程序错误 | program error | |
| 警告 | warning | |
| 提示 (noun) | hint | The diagnostic level below a warning. |
| 提示 (verb, inside an error) | suggests | |
| 诊断 | diagnostic | |
| 写明 | states | For what a diagnostic says. |
| 快速修复 | quick fix | |
| 改法 | fix; suggested fix | README: "with a suggested fix". |
| 分析服务 | analysis service | |
| 悄悄 | silently | |
| 永远挂起 | hang forever | README term. |
| 类型可信 | types can be trusted | The previous edition said "sound types". |
| 失败不沉默 | no silent failure | |
| 不崩溃 | no crashes | |
| 语料 | corpus | |
| 设计说明 | design notes | |
| 引擎事实 | engine facts | |
| 交付 | ship | README: "ships as a GDExtension". The section title is "Delivery". |
| 无界面模式 | headless mode | |
| 运行时 (adverb) | at run time | README spelling. |
| 运行时 (adjective, component) | runtime | As in "runtime type" and the chapter "Runtime". |
| 编译期 | at compile time; compile-time | |
| 类加载时, 加载期 | when the class loads; load-time | |

### Types and values

| Chinese | English | Note |
|---|---|---|
| 可空 | nullable | README term. The compiler also says "may be None". |
| 非空 | non-null | README term. |
| 已释放对象 | freed object | |
| 存活检查 | liveness check | |
| 保持存活 | keep alive | |
| 读作 | reads as | README: "reads as `None`". |
| 收窄 | narrow; narrowing | Compiler: "narrow it with `if v is`". |
| 短路返回 | short-circuit return | The postfix `?`. The README calls it "early return". |
| 提前返回 | early return | A `return` before the end of a function. |
| 包装 | wrap | |
| 层 (of Option) | level | |
| 值类型 | value type | |
| 值语义 | value semantics | |
| 载荷 | payload | |
| 带载荷 enum | enum with payloads | Compiler term. |
| 无载荷 enum | enum without payloads | |
| 分支 (of an enum) | variant | Compiler term. |
| 分支 (of `match`, `if`) | branch | |
| 封闭, 开放 (enum) | closed, open | |
| 容器 | container | |
| 带类型容器 | typed container | |
| 无类型容器 | untyped container | |
| 无类型视图 | untyped view | |
| 元素可由引擎表示 | elements the engine can represent | |
| 本语言独有类型 | type that exists only in this language | Compiler: "exist only in this language". |
| 自有表示 | a representation of its own | |
| 下标 | index | Runtime: "Index %d is out of bounds". |
| 越界 (index) | out of bounds | |
| 越界 (number) | out of range | Runtime: "is out of range for the engine's". |
| 只读 | read-only | |
| 副本 | copy | |
| 浅复制, 深复制 | shallow copy, deep copy | |
| 同一性 | identity | |
| 词典序, 字典序 | lexicographic order | |
| 存储位置 | storage location | |
| 临时值 | temporary value | Compiler term. |
| 写回 | write back | |
| 可赋给 | assignable to | |
| 隐式转换, 显式转换 | implicit conversion, explicit conversion | Compiler: "Convert explicitly". |
| 期望类型 | expected type | |
| 协变, 逆变, 不变 | covariant, contravariant, invariant | |
| 函数类型 | function type | |
| 函数值 | function value | Compiler term. |
| 方法值 | method value | Compiler term. |
| 接收者 | receiver | |
| 捕获 | capture | |
| 弱引用, 强引用 | weak reference, strong reference | |
| 泛型 | generics; generic | |
| 类型参数, 类型实参 | type parameter, type argument | |
| 名义接口 | nominal interface | |
| 父接口, 子接口 | parent interface, child interface | |
| 回绕 | wrap; wrapping | For integer overflow. |
| 截断 | truncate | |
| 窄整数 | narrow integer | |
| 真值判断 | truthiness test | |

### Declarations and expressions

| Chinese | English | Note |
|---|---|---|
| 类头 | class header | |
| 文件类 | file class | |
| 内部类 | inner class | |
| 外层类, 外层实例 | outer class, outer instance | As `Outer.Inner` in the examples. |
| 外层块 | enclosing block | Compiler term. |
| 无实例声明 | non-instance declaration | |
| 裸名 | bare name | |
| 注解 | annotation | The `@` kind. |
| 类级注解 | class-level annotation | |
| 类型标注 | type annotation | |
| 上下文关键字 | contextual keyword | |
| 初始化式 | initializer | |
| 访问器 | accessor | |
| 存储字段 | backing field | |
| 底层字段 | underlying field | |
| 可见性 | visibility | |
| 不带 `pub` | without `pub` | Compiler: "is not pub". |
| 覆盖 | override | |
| 继承 (class, interface) | extend; inheritance | |
| 虚函数 | virtual function | |
| 剩余参数 | rest parameter | Compiler term. |
| 具名实参 | named argument | |
| 按名字 | by name | Compiler: "take members by name". |
| 按位置 | by position; positional | |
| 必须赋值的成员 | member that must be assigned | Compiler term. Not "must-assign member". |
| 必须赋值的导出 | export that must be assigned | Compiler term. |
| 每条路径 | every path | |
| 常量表达式 | constant expression | |
| 已知 (in constants) | known | Compiler: "known at compile time". |
| 工具函数 | utility function | Godot term. |
| 内置类型 | built-in type | |
| 标准库 | standard library | |
| 静态初始化 | static initialization | |
| 模式, 值模式 | pattern, value pattern | |
| 被匹配值 | matched value | |
| 守卫 | guard | |
| 绑定 | binding | |
| 穷尽 | exhaustive | README term. |
| 循环变量 | loop variable | |
| 遍历 | iterate over | |
| 复合赋值 | compound assignment | |
| 链式赋值 | chained assignment | |
| 复合语句 | compound statement | |

### Scenes and lifecycle

| Chinese | English | Note |
|---|---|---|
| 场景模型 | scene model | |
| 节点定型 | typing nodes | |
| 定型 | type (verb); typed | |
| 字面量路径 | literal path | |
| 向上访问 | upward access | |
| 就绪 | ready | |
| 就绪顺序 | ready order | |
| 就绪前, 就绪后 | before ready, after ready | |
| 创建者 | creator | Compiler term. |
| 祖先 | ancestor | Compiler term. |
| 挂 (script on a node) | attach | |
| 挂 (node under a parent) | add under; parenting | |
| 实例化 (a scene in a scene) | instance (verb); instanced | Compiler: "no scene instances it". |
| 继承场景 | inherited scene | Godot term. |
| 作为根使用 | used as a root | |
| 场景根 | scene root | |
| 缺值错误 | missing-value error | |
| 导出 (`@export`) | export; exported | |
| 导出 (a build) | export | "At export", "export template", "export plugin", "export dialog". |
| 检查器 | Inspector | The editor dock. |
| 远程检查器 | remote inspector | |
| 场景树 | scene tree | |
| 局部于场景 | Local to Scene | Godot's label. |
| 内嵌子资源 | embedded sub-resource | |
| 资源共享 | resource sharing | |
| 引用环 | reference cycle | |
| 泄漏 | leak | |
| 析构 | destruction | |
| 释放 (an object) | free | |
| 释放 (a reference, a frame) | release | |
| 释放位置 | free site | |
| 创建位置 | creation site | |
| 热重载 | hot reload | |
| 重新运行 | run again | |

### Errors and coroutines

| Chinese | English | Note |
|---|---|---|
| 调用链 | call chain | |
| 调用栈 | call stack | |
| 中止 | abort | |
| 引擎入口 | engine entry point | |
| 预期之内的失败 | expected failure | README term. |
| 丢弃 | discard | Compiler: "cannot be discarded". |
| 断言 | assertion | |
| 协程 | coroutine | |
| 任务 | task | |
| 根任务 | root task | |
| 挂起 | suspend; suspension | |
| 完成, 取消, 失败 | finished, cancelled, failed | The three end states. |
| 归属 | ownership | |
| 所属对象 | owning object | Not "owner", which is the node property. |
| 等待方 | waiter | |
| 被等待对象 | awaited object | |
| 信号源 | signal source | |
| 协程宿主 | coroutine host | |
| 静默取消 | cancelled silently | |
| 重入 | reentrancy | |
| 句柄 | handle | |
| 拦截 | intercept | |

### Engine boundary and tools

| Chinese | English | Note |
|---|---|---|
| 引擎边界 | engine boundary | |
| 入口检查 | entry check | |
| 校验 | validate | |
| 编码, 解码 | encode, decode | |
| 绑定元数据 | binding metadata | |
| 审核 | audit | |
| 反射 | reflection | |
| 按名反射 | reflection by name | |
| 反射暴露的成员 | members exposed through reflection | |
| 改写的名字 | rewritten name | |
| 全局类 | global class | |
| 类级访问 | class-level access | |
| 跨语言 | cross-language | |
| 静态视图 | static view | |
| 转发 | forward | |
| 带下标的属性 | indexed property | Godot term. |
| 按位互转 | convert bit for bit | |
| 原生代码 | native code | |
| 虚拟机 | virtual machine | |
| 数据竞争 | data race | |
| 补全 | completion | README term. |
| 悬停类型 | hover types | README term. |
| 跳转定义 | go to definition | README term. |
| 查找引用 | find references | README term. |
| 重命名 | rename | README term. |
| 格式化 | formatting | README term. |
| 重构 | refactoring | |
| 迁移工具 | migration tool | README term. |
| 迁移报告 | migration report | |
| 保义 | meaning-preserving | |
| 输出面板 | Output panel | Godot's label. |

### Recurring phrases

| Chinese | English | Note |
|---|---|---|
| 沿用 GDScript | follows GDScript | |
| 与 GDScript 相同, 同 GDScript | as in GDScript | |
| 按……的规则 | by the rules of | |
| 得 | gives | As in "`instantiate()` gives `T`". |
| 标在场景上 | marked on the scene | |
| 须写, 写作 | must be written; is written | |
| 视为 | treated as | |
| 照常 | as usual | |
| 途径 | route | |
| 由……提供 | provided by | |
| 所在语句, 所在场景 | its statement, its scene | |
| 如, 例如 | such as; as in; for example | "Examples are" when the list becomes a sentence. |
| 等 | and so on | |
| 同样如此, 同理 | the same holds for | |
| 除外 | is excepted | |
| 算数 | counts | |
| 漏传 | leave out | |
| 不受此限 | not restricted this way | |
