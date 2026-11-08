# 调查：语料中的零散计数

**2026-10-25** · 关联：[语言规范](../language-spec.zh.md)

规范中的个别规则只需要一个计数作依据。本文收集这类计数，语料与[跨分支重复调查](cross-branch-duplication.md)相同：20 个仓库，固定提交，只计第一方 `.gd` 文件，共 203,720 行代码（不计空行和注释行）。“用于”一列是规范中以该计数为依据的章节。

| 计数 | 结果 | 用于 |
|---|---|---|
| `null` 字面量 | 4,520 处，每千行 22.2 处；其中比较（`== null`、`!= null`）2,541 处，赋值、返回和传参 1,979 处 | §2.2：`null` 是 None 的别名 |
| 前缀 `!` 与 `not` | 前缀 `!` 449 处，`not` 4,500 处（含 `not in`） | 附录 B：不采用后缀 `!` 的背景；前缀 `!` 仍是逻辑非（§2.2） |
| 在 `_ready` 中把节点登记到 autoload 成员上（`Autoload.x = self`） | 6 处，其中 5 处在 Ring | §6.2：autoload 引用场景节点的成员写作 `T?`（脚本见 `paper-migration-2/autoload_self.py`） |
| `@abstract` 类和方法 | 28 个类：gdUnit4 25 个，Pixelorama 2 个，godot-open-rpg 1 个；250 个抽象方法；以文件为单位的抽象类中，17 个的方法全部是抽象的 | §4.1：`@abstract` |
| 剩余参数 `...args` | 78 个：gdUnit4 76 个，Pixelorama 2 个 | §4.6：剩余参数 |
| 含 await 的 lambda | 979 个 lambda 中 35 个，其中 33 个在 gdUnit4 的测试中 | §9.1：lambda 也写 `async func` |
| 命名的、带默认值的、给捕获赋值的 lambda | 979 个 lambda 中命名的 18 个，带默认值的 0 个；方法中的 961 个里，给方法的参数或局部变量赋值的 0 个，经其成员或下标赋值的 15 个，经阅读都是对象或容器，例如 godot-statecharts 用信号连接设置的 `var holder: Array[bool] = [false]` | §3.11：给捕获的变量赋值是编译错误；命名的 lambda；lambda 的参数不带默认值 |
| 作为值使用的协程方法（同一文件中按名字作为实参传递） | 91 处，其中 60 处是 `connect(名字)`，分布在 12 个仓库 | §3.11：由引擎调用的位置接受 async 函数 |
| `assert(...)` 语句 | 595 个，其中 406 个在 godot_dialogue_manager；实参中含 await 的 0 个；带消息的 539 个：字符串字面量 496 个，含调用的 19 个，都是查询或格式化（`translate` 15 次、`format` 6 次、`size` 3 次、`get_error_message` 2 次、`get_error_text` 2 次、`get` 1 次、`str` 1 次），其他表达式 24 个，例如用 `%` 格式化 | §8.3：`assert` 在 release 构建中去掉；消息只在失败时求值 |
| `is_instance_valid(...)` | 678 处，每千行 3.3 处；phantom-camera 中每千行 12.8 处 | §3.3：经 `T?` 读到已释放的对象时为 None |
| `static var` 与 `_static_init` | `static var` 188 处，有初始化式的 134 处；没有初始化式的：内置类型 24 处，其他类型 29 处，经阅读都是类，如 RegEx、Shader 和单例 `static var instance: DMPlugin`，无类型 1 处；`_static_init` 4 个 | §4.8：静态成员沿用 GDScript 的写法和时机；没有默认值的类型的静态变量须有初始化式或写作 `T?` |
| `NOTIFICATION_PREDELETE` | 40 处，其中 32 处在 gdUnit4 | §6.6：析构不新增语法 |
| `Thread.new` 与 `WorkerThreadPool` | `Thread.new` 5 处，`WorkerThreadPool` 2 处 | §12.3：第一版不新增线程 API |
| `create_timer(...)` 的调用 | 260 处，只传时间的 251 处；经 `get_tree()` 252 处，经 `Engine.get_main_loop()` 6 处 | §9.5：`Task.delay` 按引擎的默认值计时，不需要节点在树中 |
| 自身的 `get_parent()` 与 `owner` | `get_parent()` 174 处，其中直接访问成员 62 处，分布在 46 个文件；判断为空 3 处，经阅读都在处理没有父节点的情况，其中 beehave 的 `beehave_tree.gd:91` 在节点挂到父节点之前执行的 setter 中调用 `get_parent()`。`owner` 直接访问成员 49 处，分布在 21 个文件；判断为空 0 处 | §7.3：未声明 `@parent`、`@owner` 时，`get_parent()` 和 `owner` 的类型为 `Node?` |
| 在自身上的 `free()` 与 `set_script(...)`，以及对照的 `queue_free()` | `free()` 0 处；`set_script` 1 处，即 gdUnit4 `GdUnitMockImpl.gd:19` 的 `super.set_script(__script)`，经阅读，调用之前已设为同一个脚本，引擎遇到同一个脚本直接返回；在 `_on_` 开头的方法中对其他对象调用 `.free()` 6 处，经阅读释放的都是子节点或 TreeItem，没有释放信号的发射者；`queue_free()` 532 处 | 附录 B：不把在自身上的 `free()`、`set_script()` 定为编译错误；§3.3：方法运行中实例被删除后访问成员是程序错误 |
| `pop_at`、`pop_back` 与 `pop_front` | `pop_at` 14 处：Pixelorama 9 处，tanks-of-freedom-ii 5 处，经阅读都按程序算出的下标移出元素，结果直接使用或丢弃，没有一处判断为空；`pop_back` 28 处，`pop_front` 48 处 | §3.8：`pop_at(i)` 返回 T，越界是程序错误 |
| 名为 `_get`、`_set` 的方法 | `_get` 3 个，`_set` 5 个：godot-tiny-mmo 5 个，beehave 2 个，Godot-Game-Template 1 个。经阅读都只为本类自己的动态名字而写，没有一个针对引擎属性。不计入的第三方代码中，godot-open-rpg 附带的 Dialogic 有 7 个，其中 `node_name_label.gd:18` 的 `_set` 拦截 Label 的 `text`，同时切换名字框的显隐 | §10.5：读写引擎属性不经 `_get`、`_set` |
| 单引号或双引号（非三引号）中含换行的字符串 | 97 个：godot_dialogue_manager 95 个，都在测试中，是多行的对话脚本输入；phantom-camera 1 个，`print_rich` 打印的多行字符画；godot-open-rpg 1 个，`field.gd:30` 的 assert 消息折行，换行和缩进成了消息的一部分 | §2.2：单引号和双引号字符串不能跨行 |
| 含 `@tool` 的文件 | 326 个：插件仓库 211 个，示例 51 个，游戏仓库 58 个（godot-open-rpg 31 个，godot-tiny-mmo 18 个，godot-open-rts 8 个，godot_open_star_fighter 1 个），工具 6 个；其中 33 个用到编辑器插件类（EditorPlugin、EditorInspectorPlugin、EditorImportPlugin、EditorExportPlugin、EditorScript） | §2.3：第一版没有 `@tool` |
| 以字面量路径 `preload` 的 `.gd` 文件，以及以它初始化的常量在同一文件中的用法 | 351 处：游戏仓库 199 处，插件仓库 137 处，示例 13 处，工具 2 处；最多的是 tanks-of-freedom-ii 115 处、godot-open-rts 76 处、gdUnit4 67 处。初始化常量 207 处，直接 `.new()` 69 处，直接访问其他成员 1 处，作为值 74 处（经阅读多是 tanks-of-freedom-ii 按名字查脚本的字典，以及编辑器插件的 `add_custom_type`）。经这些常量：`new()` 54 处，分布在 36 个文件；其他调用 124 处，分布在 42 个文件；其他成员（常量、enum、内部类）134 处，分布在 28 个文件；`is` 69 处；作为类型 46 处；作为值 17 处 | §2.1：preload 脚本得到它的类 |
| 不带载荷的 enum | 258 个：具名 228 个，匿名 30 个；没有值 0 的 8 个，其中 4 个在 gdUnit4 的测试中。用 `as` 把 int 转为 enum 10 处，`@warning_ignore("int_as_enum_without_cast")` 4 处，都在 Pixelorama。把 enum 名当 Dictionary 用 38 处：`keys()` 14 处，`values()` 8 处，`size()` 6 处，`get()` 4 处，`find_key()` 2 处，`Name[...]` 2 处，`for x in Name` 2 处；以常量作 enum 的别名 3 处，在 gdUnit4。对引擎 enum 的 match 不带 `_` 的 5 处，都是 Pixelorama 的 `match mouse_button` | §3.7：int 只能显式转为 enum；enum 名作为带类型的 Dictionary；没有值 0 的 enum 必须赋值 |
| 内部类 | 121 个文件中 192 个，其中 71 个在测试文件中；游戏仓库中 19 个文件 37 个，全在 godot-open-rts（25 个）和 godot-tiny-mmo（12 个）。127 个不写 extends，9 个继承同文件的内部类，3 个继承其他类的内部类（`Other.Inner`）；10 个把 `extends` 写在类体第一行；7 个嵌套在另一个内部类中。17 个以裸名使用外层类的声明：其他内部类 9 个，具名 enum 5 个，常量 4 个；没有一个使用外层的静态函数或静态变量。其他文件经 autoload 使用 175 处，其中 168 处在 godot-open-rts，如 `Constants.Match.Units.PROJECTILES`；经 class_name 117 处；经 preload 的常量 11 处 | §2.1、§4.1：内部类（脚本见 `corpus-counts/inner_classes.py`） |
| 排版习惯 | 2,800 个文件中用制表符缩进的 2,643 个，用空格的 65 个。括号内的续行比开括号所在的行多缩进：圆括号中一层 5,837 行、两层 892 行，方括号和花括号中一层 9,520 行、两层 369 行；独占一行的闭括号回到开括号所在行的缩进的 4,240 个，其他 358 个。单行的字典花括号内侧不留空格的 1,077 个，留空格的 289 个。行尾注释前两个空格的 1,207 处，一个空格的 775 处。顶层函数和类之前空两行的 15,430 处，空一行的 3,687 处，以空一行为主的是 Godot-Game-Template、tanks-of-freedom-ii 和 Kenney 的 4 个 Starter Kit；内部类中不是第一个成员的函数之前空一行的 375 处，不空的 260 处，空两行的 59 处；缩进的代码中单个空行 15,175 处，连续空行 134 处。超过 100 列的代码行 4,305 行（2.1%），其中 gdUnit4 1,708 行，它的 gdstyle 配置把行宽设为 180。以 `\` 续行的 2,150 行，其中 gdUnit4 1,784 行；与头部写在同一行的块 1,830 个；语句之间的 `;` 6 处。经阅读 CI 配置，3 个仓库运行格式化器：Pixelorama 和 godot-open-rts 用 gdformat，gdUnit4 用 gdstyle | §13.2：格式化的规则（脚本见 `corpus-counts/layout.py`） |
| 节点的脚本给它下方节点的脚本成员赋值（`$X.m = v`，或经以 `$X` 初始化的成员、局部变量） | 222 处，都赋给本场景的节点，其中 151 处的目标是放进本场景的实例根。赋给实例化的子场景内部节点的 0 处。目标成员没有初始化式的 85 处，同样都在本场景。伸进子场景内部的只有 `godot-demo-projects/3d/graphics_settings/settings.gd` 一个文件，赋的都是引擎属性 | §6.4：由祖先赋值的必须赋值成员在 owner 发出 `ready` 时检查（脚本见 `corpus-counts/ancestor_assignments.py`） |
| 跨文件的类级访问中，GDScript 对本语言的类做不到的。包括调用静态方法，以及把 enum、常量或内部类用作类型或用在常量表达式中 | 经 class_name、autoload 和 preload 的常量，静态方法 3,287 处，用作类型 367 处，常量表达式 59 处。游戏仓库的 514 个具名类和 autoload 中，97 个被其他文件这样访问，其中 82 个在 godot-tiny-mmo。除它之外是 247 个中的 15 个，tanks-of-freedom-ii 和 Ring 为 0。可以做到的访问：读常量 1,722 处，读 enum 1,122 处，经 autoload 实例访问成员 4,724 处 | §11.3：GDScript 访问本语言类的已知限制（脚本见 `corpus-counts/class_level_access.py`） |
| 文件自身的 `_init` 带参数的脚本，按原生祖先分 | `Node` 28 个：有必填参数的 26 个，都有默认值的 2 个。`Resource` 2 个，都有必填参数。其他 150 个。`Node` 的 28 个中插件 15 个；游戏和示例 13 个，其中 godot-open-rts 的动作节点 11 个（如 `Moving.gd`），另有示例的 `chunk.gd` 和 Ring 的 `BranchOption.gd`。以 class_name 写出的 `Name.new(...)`：`Node` 6 处，`Resource` 8 处 | §6.2：`Node` 和 `Resource` 的 `_init` 可带参数（脚本见 `corpus-counts/init_params.py`） |
| 本语言新增的关键字（`pub`、`struct`、`interface`、`implements`、`async`、`None`）用作名字 | 0 处，点号之后和其他位置都没有。不计入的第三方代码中，Ring 附带的 dialogue_manager 有 `enum TranslationSource { None, ... }` | §2.2：新增的是上下文关键字 |
| 加载成环：场景和资源经 ext_resource，脚本经 preload 和基类，各自加载的文件连成的环 | 2,678 个场景和资源、2,800 个脚本中 1 个，是 gdUnit4 的 `CallableDoubler.gd` preload 它自己；脚本 preload 挂着它的场景 0 处。375 个脚本有 preload，指向场景、资源和脚本的 1,004 处都找到了文件；按 uid 找不到的 18 处 | §7.6：preload 成环是编译错误（脚本见 `corpus-counts/preload_cycles.py`） |
| 紧贴在声明上方、以 `##` 开头的注释，GDScript 把它当作该声明的文档 | 755 个文件中 5,084 段；其中只由 `###` 开头的行组成的 41 段，godot_open_star_fighter 38 段，经阅读都是 `### Signals ###` 这样的分节标题或由 `#` 组成的分隔线；没有一段把这样的行和其他行混写 | §2.2：以 `###` 开头的注释不是文档 |

计数基于词法分析，不做类型解析；`!` 只计不紧跟 `=` 的。`...` 在 GDScript 中只出现在函数签名里（Godot `469659e` 的 `modules/gdscript/gdscript_parser.cpp:1714, 4336`），所以紧跟名字的 `...` 都是剩余参数。协程指函数体中有 await 的方法，只在同一文件中查找对它的引用，不计场景中的信号连接。lambda 的捕获指所在方法在它之前声明的参数和局部变量，lambda 自己的参数和局部变量除外。`get_parent()` 和 `owner` 只计在自身上的使用，即前面没有 `.` 的；判断为空包括与 null 比较、用作条件和传给 `is_instance_valid`，以及在其后 25 行内对赋得其值的变量做这些判断。`free()` 和 `set_script(...)` 在自身上，指前面没有 `.`，或写作 `self.`、`super.`。字符串由扫描注释以外的文本找出，反斜杠转义和 `r` 前缀按 GDScript 的读法处理。preload 的常量的用法只在声明它的文件中查找：作为类型指出现在 `:`、`->`、`as` 之后或 `Array[...]` 中，不计节点路径 `$名字`、`%名字`。enum 的用法只计单独写出它的名字或写在同一仓库的 class_name 之后的，经 preload 的常量的用法不计；每个值都是 int 字面量或从字面量顺延时，才算知道它的值。内部类指行首（注解之后）的 `class`，类体是其后缩进更深的行；在内部类中以裸名使用外层类声明的名字才计入，内部类自己、其局部变量或其引擎基类声明了同名的除外；其他文件中的 `名字.Inner` 经 class_name、preload 的常量和 autoload（`project.godot`）解析。没有初始化式的 `static var` 按类型的写法归类：内置类型即 Variant 的类型名。排版的计数中制表符按 4 列计，即编辑器默认的缩进宽度（Godot `ed1daf0` 的 `editor/settings/editor_settings.cpp:831`）；续行的缩进相对于最内层未闭合的开括号所在的行；函数或类之前的空行不计紧贴其上的注解和注释行。文档注释指连续若干只含以 `##` 开头的注释的行，其下一行（跳过只有注解的行）以 `var`、`const`、`func`、`signal`、`enum`、`class`、`static`、`class_name` 或 `extends` 开头。

`create_timer` 只计写作 `.create_timer(` 的方法调用。参数个数按括号内顶层的逗号计。

## 复现

```
python docs/surveys/cross-branch-duplication/gdcorpus.py corpus
python docs/surveys/corpus-counts/counts.py corpus
python docs/surveys/paper-migration-2/autoload_self.py corpus
python docs/surveys/corpus-counts/inner_classes.py corpus thirdparty/godot-cpp/gdextension/extension_api-4-7.json --samples 0 > docs/surveys/corpus-counts/results/inner_classes.txt
python docs/surveys/corpus-counts/layout.py corpus > docs/surveys/corpus-counts/results/layout.txt
python docs/surveys/corpus-counts/ancestor_assignments.py corpus > docs/surveys/corpus-counts/results/ancestor_assignments.txt
python docs/surveys/corpus-counts/init_params.py corpus thirdparty/godot-cpp/gdextension/extension_api-4-7.json > docs/surveys/corpus-counts/results/init_params.txt
python docs/surveys/corpus-counts/class_level_access.py corpus thirdparty/godot-cpp/gdextension/extension_api-4-7.json > docs/surveys/corpus-counts/results/class_level_access.txt
python docs/surveys/corpus-counts/preload_cycles.py corpus > docs/surveys/corpus-counts/results/preload_cycles.txt
```
