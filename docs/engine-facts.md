# 引擎事实

[语言规范](language-spec.zh.md)所依据的引擎和 GDScript 行为核对表，基于 Godot 4.7.2-stable（提交 `ed1daf0`），路径相对于 Godot 仓库的根目录。升级引擎核对基线时逐条复核行号和行为。

| 事实 | 出处 | 规范章节 |
|---|---|---|
| Variant 的整数是 int64_t，浮点数是 double | `core/variant/variant.h:252-253` | §3.2 |
| Variant 转为比 int64 窄或无符号的整数时直接截断或回绕 | `core/variant/variant.h:336-353, 411-424` | §10.6 |
| RefCounted 对象的实例 ID 最高位为 1，按 int64 读出为负数 | `core/object/object.h:876-880`；`core/object/object.cpp:2485` | §10.6 |
| `real_t` 在标准构建中是 float，在双精度构建中是 double | `core/math/math_defs.h:143-147` | §4.5、§10.6 |
| 引擎的运算符表 | `core/variant/variant_op.cpp:212-1033` | §5.1、§5.2 |
| int 与 float 的 `==` 按数值比较；对象的 `==` 比较同一性，但先把已释放的对象取为 null | `core/variant/variant_op.cpp:490-491, 514`；`core/variant/variant_op.h:471-477` | §5.1、§5.2 |
| 已释放的对象与 NIL 比较为真 | `core/variant/variant_op.h:484-489` | §3.13 |
| String 与 StringName 的 `==` 按内容比较 | `core/variant/variant_op.cpp:196-203, 493, 615` | §5.1、§10.7 |
| 运算表中没有的组合求值无效 | `core/variant/variant_op.cpp:1049-1054` | §5.2 |
| int 和整数向量除以 0、对 0 取模时求值无效 | `core/variant/variant_op.h:134-146, 241-250` | §4.5 |
| 移位只在 debug 构建中拒绝负的运算数 | `core/variant/variant_op.h:360-372, 386-398` | §4.5、§5.1 |
| 两个 int 的 `**` 用 double 计算再转回 int，超过 2^53 不精确，超出范围或为无穷时是 C++ 的未定义行为 | `core/variant/variant_op.h:86-95` | §4.5、§5.1 |
| Packed 数组的 `in` 先把左运算数转为元素类型，float 因此截断为 int | `core/variant/variant_op.h:1142-1153` | §5.1 |
| `name in obj` 在对象上查找该名字的成员，与 `obj.get(name)` 相同 | `core/variant/variant_op.h:1217-1252` | §5.1 |
| String 或 StringName 的 `%` 格式化任何类型的值，Array 视为其中各个值。格式出错时，经验证的求值打印错误并给出格式串，直接求值给出错误文本 | `core/variant/variant_op.cpp:205-210, 408-449`；`core/variant/variant_op.h:720-837` | §5.1 |
| 引擎归为 math 类的工具函数。它们的调用错误都是实参类型不符，`posmod` 的除数为 0 时打印错误并返回 0 | `core/variant/variant_utility.cpp:383-402, 459-470, 587-606, 653-668, 697-712, 737-751, 1632-1732`；`core/math/math_funcs.h:312-313` | §4.5、§10.3 |
| 由字符串构造 Color 时，不是十六进制写法就按颜色名查找，找不到时打印错误并返回黑色。颜色名就是 Color 的常量名 | `core/math/color.h:274-280`；`core/math/color.cpp:396-402`；`core/variant/variant_call.cpp:3086-3089` | §4.5 |
| debug 构建中有四项检查：由轴和角构造 Basis、Quaternion，要求轴已归一化；由 Basis 构造 Quaternion，要求它是旋转。由两个向量构造 Quaternion，要求它们不为零；Quaternion 乘向量，要求四元数已归一化 | `core/math/basis.cpp:716-717, 843-844`；`core/math/quaternion.cpp:289-290`；`core/math/quaternion.h:91-94, 139-140`；`core/math/math_defs.h:56-58` | §4.5 |
| Array 的 `==` 逐元素用 `hash_compare`，它要求元素类型相同 | `core/variant/array.cpp:155-158`；`core/variant/variant.cpp:3176-3179` | §5.2 |
| `hash_compare` 只在语义比较时认为 float 的 NaN 与 NaN 匹配。Array 的 `==` 不做语义比较，float 元素的 NaN 因此不匹配；Dictionary 查键做语义比较。向量、Color、Quaternion 按 `is_same` 比较，NaN 总是匹配 | `core/variant/variant.cpp:3138-3155, 3186-3187, 3399-3401`；`core/variant/variant.h:829`；`core/variant/array.cpp:155` | §5.2 |
| Array 的 `<` 逐元素比较，比不出大小的元素当作相等，接着比下一个。元素按 `Variant::operator<` 比较：类型不同时按类型编号排序，查不到运算时为假 | `core/variant/array.cpp:164-179`；`core/variant/variant.cpp:868-876` | §5.2 |
| Array 的 `<=` 是 `not (b < a)`，`>=` 是 `not (a < b)` | `core/variant/array.cpp:181-188` | §5.2 |
| Dictionary 的 `==` 按键查找后逐项比较值，与顺序无关 | `core/variant/dictionary.cpp:255-285` | §5.2 |
| 已释放的对象在条件中为假：对象的 `is_zero` 检查对象是否仍然有效 | `core/variant/variant.cpp:952-953`；`core/variant/variant_op.cpp:1120-1122` | §3.3 |
| Variant 转为对象指针时不检查对象是否存活。引擎方法的对象参数经它取出：debug 构建按类检查时就使用这个指针，release 构建直接交给方法 | `core/variant/variant.cpp:2024-2026`；`core/variant/binder_common.h:58-61`；`core/variant/variant_caster.h:139-140` | §3.3 |
| 带类型的 Array 收到对象时，debug 构建先查 ObjectDB，拒绝已释放的对象。release 构建直接使用指针。`append`、`erase`、`has` 等都经过这一检查 | `core/variant/container_type_validate.h:89-112`；`core/variant/array.cpp:285, 350, 499` | §3.3、§3.8 |
| `is_instance_valid` 先检查对象是否存活 | `core/variant/variant_utility.cpp:1133-1137` | §3.3 |
| min、max、clamp 原样返回其中一个实参 | `core/variant/variant_utility.cpp:642-759` | §4.5、§10.3 |
| `find_children` 只收入 `is_class` 给定类名的节点，或脚本是该全局类的节点 | `scene/main/node.cpp:2031-2042` | §10.3 |
| lerp 要求两个实参类型相同 | `core/variant/variant_utility.cpp:465-469` | §4.5、§10.3 |
| `pop_front`、`pop_back` 在空数组上返回 null 而不报错 | `core/variant/array.cpp:781-800` | §3.8 |
| `pop_at` 在空数组上返回 null 而不报错，非空数组越界时报错并返回 null，负下标从末尾数起 | `core/variant/array.cpp:802-826` | §3.8 |
| `front`、`back` 在空数组上报错 | `core/variant/array.cpp:354-362` | §3.8 |
| Dictionary 的 `get` 在缺键时返回默认值 | `core/variant/dictionary.cpp:178-187` | §3.8 |
| 大小比较运算只有 bool、数值、String、StringName、向量、RID 和 Array 有，对象、Dictionary、Callable 没有；Array 逐元素比较 | `core/variant/variant_op.cpp:731-791`；`core/variant/array.cpp:164-179` | §3.6、§5.2 |
| Array 的 `sort`、`bsearch`、`min`、`max` 和 Dictionary 的 `sort` 按运算表比较。`sort` 把做不了的比较当作假；`min`、`max` 返回 null，在空数组上也是；都不报错 | `core/variant/array.cpp:721-736, 756-760, 828-868`；`core/variant/dictionary.cpp:315-330` | §3.8 |
| 带类型的 Array 的 `resize` 以该类型的默认值填充新元素，对象为 null | `core/variant/array.cpp:306-318` | §3.8 |
| 带类型的容器收到不合元素类型的值时，能按 `can_convert_strict` 转换的先转换，float 因此截断为 int。不能转换的报错，不写入也不查找 | `core/variant/container_type_validate.h:55-75`；`core/variant/variant.cpp:557-566`；`core/variant/array.cpp:285, 374, 499, 520`；`core/variant/dictionary.cpp:205-207, 222` | §3.8、§5.1 |
| 带类型的 Array 的 `assign` 同样按 `can_convert_strict` 转换元素，转换不了时报错，数组不变 | `core/variant/array.cpp:252-266, 276` | §3.8 |
| 引擎为容器的元素以及 Dictionary 的键和值只记录一层类型：一个内置类型、一个类和一个脚本 | `core/variant/container_type_validate.h:37-40` | §3.8 |
| Dictionary 保存每个键插入时的哈希，扩容时也不重算，查找时先比较哈希再比较键 | `core/variant/dictionary.cpp:46`；`core/templates/hash_map.h:130, 149-151, 193-198` | §3.8 |
| `make_read_only` 只作用于调用它的那个容器，没有办法让容器重新可写 | `core/variant/array.cpp:938-942`；`core/variant/dictionary.cpp:599-603` | §3.8 |
| `bind` 把绑定的实参接在调用实参之后 | `core/variant/callable_bind.cpp:141-150` | §3.11 |
| 从 String 构造 StringName 需要在全局表的互斥锁下查找 | `core/string/string_name.cpp:263-285` | §10.7 |
| JSON 中的数字都解析为浮点数 | `core/io/json.cpp:390-396` | §10.8 |
| 资源按路径缓存，`load` 默认复用缓存 | `core/io/resource_loader.cpp:800-810` | §7.6 |
| 资源的循环加载返回 null | `core/io/resource_loader.cpp:1049-1056` | §7.6 |
| 方法的反射信息中没有协程性 | `core/object/method_info.h:37-44` | §9.8 |
| 脚本语言所需的钩子对 GDExtension 公开 | `core/object/script_language_extension.h:304, 391, 441, 497, 655` | §1.3 |
| 扩展实现的脚本语言经 `_is_abstract` 和全局类表报告抽象性 | `core/object/script_language_extension.h:132-136, 657-670` | §4.1 |
| 脚本语言通过 `get_extension` 自报扩展名 | `core/object/script_language_extension.h:236` | §1.4 |
| 脚本实例创建时不接受构造参数 | `core/object/script_language.h:152` | §6.2 |
| `RequiredResult` 标注只在出错时为 null 的返回值 | `core/object/object.h:942-944` | §10.2 |
| 对象释放时调用每个实例绑定的释放回调 | `core/object/object.cpp:2360-2365` | §9.7 |
| 一个对象可以同时持有多个标识不同的实例绑定 | `core/object/object.cpp:2167-2201` | §9.7 |
| 对象析构时静默断开它的所有信号连接 | `core/object/object.cpp:2327-2342` | §9.7 |
| 连接信号和 debug 构建发出信号时，引擎按名字向脚本核对信号是否存在 | `core/object/object.cpp:1197-1201, 1533-1537` | §10.5 |
| 按实例列出的属性族名字带 `/`，如元数据、主题覆盖和着色器参数。ClassDB 注册的属性名都不带 `/`（由 `extension_api.json` 核对） | `core/object/object.cpp:525`；`scene/gui/control.cpp:444-479`；`scene/resources/material.cpp:340` | §10.4 |
| 按名读写对象时，先交给脚本实例，再交给扩展类，最后才按 ClassDB 调 getter 和 setter。这个顺序出自开源时的第一个提交（`0b806ee0fc9`） | `core/object/object.cpp:233-259, 316-338` | §10.5 |
| GDScript 实例对不是自身成员的名字都先调 `_set`、`_get`，引擎类的属性名也是。Object 的文档却说它们不处理内置属性（`29a3f56701`） | `modules/gdscript/gdscript.cpp:1589-1600, 1676-1685`；`doc/classes/Object.xml:39, 303` | §10.5 |
| GDScript 的 `obj.prop` 按名读写，只对内置类型的成员用 validated 访问。对已知类的方法则直接调用 MethodBind，所以 `label.set_text(s)` 不经 `_set` | `modules/gdscript/gdscript_byte_codegen.cpp:848-883`；`modules/gdscript/gdscript_vm.cpp:1220, 1277, 2273-2313` | §10.5 |
| C# 绑定的引擎属性调用 getter 和 setter 方法 | `modules/mono/editor/bindings_generator.cpp:2816, 2834` | §10.5 |
| 对象设置脚本后发出 `script_changed` | `core/object/object.cpp:995` | §9.8 |
| 引擎不给对象设置抽象脚本 | `core/object/object.cpp:976` | §4.1 |
| PREDELETE 按逆序发出，先通知脚本实例，再通知原生类 | `core/object/object.cpp:173, 928-931` | §9.7 |
| RefCounted 在 PREDELETE 时引用计数已归零，再加引用会失败 | `core/object/ref_counted.cpp:94-95`；`core/templates/safe_refcount.h:136-146`；`core/variant/variant.cpp:1104-1108, 2509-2510` | §6.6 |
| 脚本语言的每帧回调在场景树处理完删除队列之后调用 | `main/main.cpp:5106-5109`；`core/object/script_language_extension.h:651`；`scene/main/scene_tree.cpp:735` | §3.3、§9.7 |
| `_process` 和 `_physics_process` 的参数是 double | `scene/main/node.h:433-434` | §4.6 |
| `NOTIFICATION_PARENTED` | `scene/main/node.h:465` | §7.3 |
| `get_tree()` 在节点不在树中时报错并返回 null；`get_parent()`、`get_owner()` 没有父节点或 owner 时返回 null 而不报错 | `scene/main/node.h:558-561`；`scene/main/node.cpp:2100-2102, 2296-2298` | §7.3、§10.2 |
| `set_owner(null)` 清除 owner，不报错 | `scene/main/node.cpp:2273-2281` | §7.3、§10.2 |
| `set_owner()` 可把任一祖先设为 owner；设为不是祖先的节点时报错，owner 随后为 null | `scene/main/node.cpp:2271-2294` | §7.3、§10.2 |
| 节点移出 owner 的子树时 owner 被置为 null | `scene/main/node.cpp:391-396, 1781-1783` | §7.3 |
| owner 被释放时，它拥有的节点的 owner 被置为 null | `scene/main/node.cpp:299-302` | §7.3 |
| 节点在 PREDELETE 中从最后一个子节点起逐个删除子节点 | `scene/main/node.cpp:308-312` | §9.7 |
| 节点入树时，就绪同步传播完毕 | `scene/main/node.cpp:3368-3372` | §6.2 |
| 子节点先于父节点就绪；节点重新入树时不再执行 `_ready` | `scene/main/node.cpp:326-337, 454` | §6.4 |
| `queue_free` 把节点放入删除队列，在每个物理步和处理帧的末尾删除 | `scene/main/node.cpp:3461-3471`；`scene/main/scene_tree.cpp:666, 735, 1626-1643` | §3.3、§9.7 |
| Area3D 的 `body_shape_entered` 在物体的节点不在树中时以 null 作为节点参数 | `scene/3d/physics/area_3d.cpp:233-234` | §10.2 |
| 没有设为局部于场景的子资源，由场景的所有实例共用 | `scene/resources/packed_scene.cpp:705-706` | §7.6 |
| 实例化场景时，先设置保存的属性，再把节点加到父节点下 | `scene/resources/packed_scene.cpp:494, 543` | §6.5、§13.3 |
| 加载场景时不检查属性写入是否成功 | `scene/resources/packed_scene.cpp:494` | §6.5、§13.3 |
| 节点引用以路径保存，实例化时解析 | `scene/resources/packed_scene.cpp:376-395` | §7.5 |
| 保存场景时只写节点当前属性列表中的属性 | `scene/resources/packed_scene.cpp:860-869` | §13.3 |
| 场景的根被实例化到另一个场景中时，owner 是外层场景的根 | `scene/resources/packed_scene.cpp:567-570` | §7.3 |
| 编辑器给节点改名或移动节点时，更新场景中的 NodePath 属性和动画轨道 | `editor/docks/scene_tree_dock.cpp:2143-2248, 2250-2401`（调用在 `:2426, 2667`） | §13.3 |
| 移动节点时，编辑器在动作内部不发出信号 | `editor/docks/scene_tree_dock.cpp:2535-2677` | §13.3 |
| 提交动作时先退出动作再执行，执行中不能再追加操作 | `core/object/undo_redo.cpp:313-332` | §13.3 |
| 编辑器的撤销管理器在提交后发出 `history_changed` | `editor/editor_undo_redo_manager.cpp:298` | §13.3 |
| 同名、合并方式为 `MERGE_ALL`、在前一个动作之后 800 毫秒内创建的动作并入前一个动作，保留其原有的操作 | `core/object/undo_redo.cpp:97, 116-117` | §13.3 |
| 撤销管理器按动作的上下文对象选择撤销历史，被编辑场景中的节点进入该场景的历史 | `editor/editor_undo_redo_manager.cpp:63-105, 149-156` | §13.3 |
| Web 导出开启 Extensions Support 时使用带动态链接的模板；跨域隔离响应头只在开启线程支持时需要 | `platform/web/export/export_plugin.cpp:374`；`platform/web/export/export_plugin.h:59-73`；`platform/web/doc_classes/EditorExportPlatformWeb.xml:93-95` | §1.3 |
| 导出插件的钩子都不能使导出失败 | `editor/export/editor_export_platform.cpp:2568-2576`；`editor/export/editor_export_plugin.h:120-122` | §7.5 |
| 导出插件报告的错误只让导出窗口显示失败。命令行导出只在导出函数返回错误时以非零退出码结束 | `editor/export/editor_export_platform.cpp:131, 146-148`；`editor/editor_node.cpp:1408-1424` | §7.5 |
| GDScript 通过 Script 的反射接口为其他语言的全局类定型 | `modules/gdscript/gdscript_analyzer.cpp:789-804, 4278-4319, 6005-6014` | §10.1 |
| GDScript 对 self 和内置类型以外的对象调用未知方法，只给不安全访问的警告 | `modules/gdscript/gdscript_analyzer.cpp:3750-3757` | §10.1 |
| GDScript 解析 `extends 全局类名` 时把基类文件当作 GDScript 解析，失败时只报“Could not resolve super class” | `modules/gdscript/gdscript_analyzer.cpp:469-485`（报错在 `:477, 483`）；`modules/gdscript/gdscript_cache.cpp:83-92` | §11.2 |
| GDScript 要求具体子类实现抽象方法，禁止构造抽象类和经 `super` 调用抽象方法 | `modules/gdscript/gdscript_analyzer.cpp:1533-1552, 3630-3643` | §4.1 |
| GDScript 的覆盖检查允许返回类型协变、参数类型逆变。覆盖接受的实参个数须包含被覆盖方法接受的，因此可多出带默认值的参数或剩余参数 | `modules/gdscript/gdscript_analyzer.cpp:1895-1936` | §4.6 |
| GDScript 的静态检查要求带类型的 Array、Dictionary 的元素类型完全相同，一边没有类型时不检查 | `modules/gdscript/gdscript_analyzer.cpp:6379-6393` | §3.8 |
| GDScript 的单引号和双引号字符串可以跨行 | `modules/gdscript/gdscript_tokenizer.cpp:1086-1098` | §2.2 |
| GDScript 把以 `##` 开头的注释行（含 `###`）当作文档。紧贴在声明或其注解上方的一段，或写在声明同一行之后的一条，说明该声明。局部变量和常量也是如此。第一个成员之前的第一段说明类 | `modules/gdscript/gdscript_parser.cpp:842-862, 1068-1101, 2195-2228, 4066-4075` | §2.2 |
| GDScript 的 `PI`、`TAU`、`INF`、`NAN` 是可被声明遮蔽的全局常量 | `modules/gdscript/gdscript.cpp:2154-2157`；`modules/gdscript/gdscript_tokenizer.cpp:202-207` | §2.6 |
| GDScript 的 `Color8`、`convert`、`type_exists`、`inst_to_dict`、`dict_to_inst` 在禁用弃用功能的构建中不存在，文档给出替代写法 | `modules/gdscript/gdscript_utility_functions.cpp:572-585`；`modules/gdscript/doc_classes/@GDScript.xml:14, 61, 77, 106, 269` | §2.6 |
| GDScript 的 `is_instance_of` 的第二个参数可以是 `TYPE_*` 常量、原生类或脚本，三者都可以是变量中的值 | `modules/gdscript/gdscript_utility_functions.cpp:454-506` | §2.6 |
| GDScript 的 `is_instance_of` 对已释放的值报错。值为 null 或不是对象时得到假，此前不查第二个参数是不是类或脚本 | `modules/gdscript/gdscript_utility_functions.cpp:471-477, 479-505` | §2.6 |
| GDScript 中作为值的原生类是 `GDScriptNativeClass` 对象，这个类不登记也不暴露给扩展 | `modules/gdscript/gdscript_compiler.cpp:109-113` | §2.6 |
| GDScript 的 `@warning_ignore` 遇到不认识的警告名时报错 | `modules/gdscript/gdscript_parser.cpp:5126-5128` | §2.3 |
| GDScript 的 `@warning_ignore` 按行忽略警告，从注解所在行到所注语句或声明的开头部分结束；`@warning_ignore_start` 忽略到同名的 `@warning_ignore_restore` | `modules/gdscript/gdscript_parser.cpp:291-294, 5131-5189, 5199-5228` | §2.3 |
| GDScript 的静态检查允许 String 与 StringName、NodePath 双向隐式转换，不允许 StringName 与 NodePath 之间转换 | `modules/gdscript/gdscript_analyzer.cpp:6370-6374`；`core/variant/variant.cpp:579-587, 735-750` | §10.7 |
| GDScript 不允许元素类型本身是带类型容器的带类型容器 | `modules/gdscript/gdscript_parser.cpp:3903-3904` | §3.8 |
| GDScript 的剩余参数只能是元素为 Variant 的 Array | `modules/gdscript/gdscript_analyzer.cpp:1803-1810` | §4.6 |
| GDScript 从其他语言的方法信息中读取 `METHOD_FLAG_VARARG` | `modules/gdscript/gdscript_analyzer.cpp:6052-6055` | §4.6 |
| GDScript 的方法信息不列剩余参数，只带 `METHOD_FLAG_VARARG`。函数入口把多出的实参装进新的 Array | `modules/gdscript/gdscript_compiler.cpp:2352-2355`；`modules/gdscript/gdscript_vm.cpp:634-643` | §4.6 |
| GDScript 的函数和 lambda 可以有剩余参数，信号不能 | `modules/gdscript/gdscript_parser.cpp:1555-1578, 1690-1737, 3765` | §4.6 |
| GDScript 把默认值编进被调函数的开头，每次调用从第一个省略的参数处求值。默认值可读前面的参数。方法信息中，不是常量的默认值记为 null | `modules/gdscript/gdscript_compiler.cpp:2438-2456`；`modules/gdscript/gdscript_vm.cpp:2781-2784`；`modules/gdscript/gdscript_parser.cpp:1704-1735`；`modules/gdscript/gdscript_analyzer.cpp:1795-1798` | §4.6 |
| GDScript 把 `new` 当作静态的 `_init` 检查；`new` 由 GDScript 自己绑定，Script 基类没有 | `modules/gdscript/gdscript_analyzer.cpp:5958-5961`；`modules/gdscript/gdscript.cpp:1078` | §10.1 |
| GDScript 的 `new` 构造原生对象，运行各类的成员初始化式，再把实参交给继承链上最近的 `_init` | `modules/gdscript/gdscript.cpp:125-152, 176-194, 210-254` | §6.1、§11.1 |
| GDScript 不按覆盖规则检查 `_init` 的签名 | `modules/gdscript/gdscript_analyzer.cpp:1830-1875` | §4.8、§6.1 |
| 引擎附加 GDScript 脚本时不带实参调用 `_init`，调用失败则不创建脚本实例 | `modules/gdscript/gdscript.cpp:192-203, 406-424` | §6.2 |
| GDScript 把没有报告 `_init` 的其他语言的脚本当作只有无参的构造 | `modules/gdscript/gdscript_analyzer.cpp:6005-6031` | §10.1、§11.1 |
| C# 脚本的方法列表只有普通方法，不含构造器 | `modules/mono/editor/Godot.NET.Sdk/Godot.SourceGenerators/ScriptMethodsGenerator.cs:129-131`；`modules/mono/csharp_script.cpp:2509-2522` | §11.1 |
| C# 脚本不提供常量：`get_constants` 用基类的空实现，`_get` 只认 `script/source` | `core/object/script_language.h:192`；`modules/mono/csharp_script.cpp:2197-2203` | §11.1 |
| GDScript 的脚本对象按名字调用静态函数，读取常量、静态变量和静态函数 | `modules/gdscript/gdscript.cpp:928-940, 957-997` | §4.8、§11.1 |
| GDScript 实例从自己的脚本往上按名字查找方法，包括静态函数 | `modules/gdscript/gdscript.cpp:1948-1966` | §4.8、§6.3 |
| 在实例上调用静态方法，GDScript 默认只给警告 | `modules/gdscript/gdscript_analyzer.cpp:3699-3703`；`modules/gdscript/gdscript_warning.h:131` | §4.8 |
| GDScript 把 enum 的值转为 int 不提示，把 int 转为 enum 给出默认开启的警告。它不把一个 enum 转为另一个。`as` 把 int 转为 enum 时不做检查 | `modules/gdscript/gdscript_analyzer.cpp:3826-3829, 6326-6336, 6375-6378, 6397-6406`；`modules/gdscript/gdscript_warning.h:140` | §3.7、§5.3 |
| GDScript 把 enum 名作为从各值的名字（String）到值的只读 Dictionary，没有值 0 的 enum 的变量从 0 开始并给出警告 | `modules/gdscript/gdscript_analyzer.cpp:1159-1197, 2207-2217` | §3.7 |
| `get_keycode_with_modifiers()` 给出带修饰键位的 Key，不在 enum 的值之中 | `core/input/input_event.cpp:394-396` | §3.7 |
| GDScript 把作为值的脚本定型为它的类，以 preload 脚本初始化的常量可作类型 | `modules/gdscript/gdscript_analyzer.cpp:873-892, 5776-5786` | §2.1 |
| GDScript 中作为值使用的 GDScript 类是 GDScript 对象 | `modules/gdscript/gdscript_analyzer.cpp:6447-6449` | §2.1 |
| 对象没有脚本实例时，`callp` 只查找引擎注册的方法，所以 Script 资源上的静态方法找不到 | `core/object/object.cpp:800-826` | §11.3 |
| GDScript 不能在类型标注中引用其他语言的类中的类型 | `modules/gdscript/gdscript_analyzer.cpp:935-937` | §11.3 |
| GDScript 不把其他语言的类的常量当作常量表达式 | `modules/gdscript/gdscript_analyzer.cpp:4321-4330` | §11.3 |
| GDScript 的常量初始化式必须是常量表达式 | `modules/gdscript/gdscript_analyzer.cpp:2124-2131` | §4.5 |
| GDScript 在编译期求值两边都是常量的运算，求值无效时报错 | `modules/gdscript/gdscript_analyzer.cpp:3139-3157` | §4.5 |
| GDScript 在编译期对常量取下标，取不到时报错；字典字面量中重复的常量键是编译错误 | `modules/gdscript/gdscript_analyzer.cpp:3854-3856, 4920-4927` | §4.5 |
| GDScript 把常量中的数组和字典逐层设为只读 | `modules/gdscript/gdscript_analyzer.cpp:5318-5367` | §4.5 |
| GDScript 只为 GDScript 写的 autoload 定型 | `modules/gdscript/gdscript_analyzer.cpp:806-826, 4579-4616` | §11.3 |
| GDScript 中以调用函数的写法调用 Callable 变量是编译错误，提示改用 `.call()` | `modules/gdscript/gdscript_analyzer.cpp:3741-3745` | §3.11 |
| GDScript 中不加 await 把协程调用当值使用是编译错误，作为语句时只是警告 | `modules/gdscript/gdscript_analyzer.cpp:3776-3784` | §9.1、§9.8 |
| GDScript 中 `?` 是语法错误 | `modules/gdscript/gdscript_parser.cpp:3860-3861` | §2.2 |
| GDScript 的前缀 `!` 是逻辑非 | `modules/gdscript/gdscript_parser.cpp:2963-2965` | §2.2 |
| GDScript 的 `**` 左结合，优先级高于一元 `-` | `modules/gdscript/gdscript_parser.h:1448-1450`；`modules/gdscript/gdscript_parser.cpp:2942, 3001` | §5.1 |
| GDScript 中内部类名、`class_name` 的名字、enum 名和函数名之后不能接 `[` | `modules/gdscript/gdscript_parser.cpp:964, 1005-1010, 1602, 1805` | §2.4 |
| GDScript 中不写 `extends` 的内部类继承 RefCounted，`extends` 可以写在类体第一行 | `modules/gdscript/gdscript_analyzer.cpp:425-429`；`modules/gdscript/gdscript_parser.cpp:974-980` | §2.1、§4.1 |
| GDScript 的内部类按裸名解析外层类的常量、enum 和内部类，不解析其静态函数和静态变量 | `modules/gdscript/gdscript_analyzer.cpp:4229, 4249, 4270-4271` | §4.1、§4.8 |
| GDScript 中的裸名先在本类、脚本基类和外层类中找。然后在原生基类的属性、方法、信号、enum 和整数常量中找。类型中的裸名也可以是原生基类的 enum | `modules/gdscript/gdscript_analyzer.cpp:332-343, 837-839, 4185-4275, 4333-4385` | §4.1、§4.8 |
| GDScript 中类的变量、常量、信号、enum 和内部类与原生基类的信号、属性或整数常量同名是编译错误 | `modules/gdscript/gdscript_analyzer.cpp:255-276, 1050, 1110, 1121, 1151, 1216, 1232, 1250` | §4.1 |
| GDScript 的内部类是单独的脚本，没有资源路径和全局名，列在外层类的常量中 | `modules/gdscript/gdscript.cpp:1082-1084`；`modules/gdscript/gdscript_parser.h:778-780`；`modules/gdscript/gdscript_compiler.cpp:3003` | §4.1、§10.5 |
| GDScript 的内部类中不能写 `class_name`、`@tool`、`@icon` 和 `@static_unload` | `modules/gdscript/gdscript_parser.cpp:146-149, 1215, 1859-1861` | §4.1 |
| 抽象方法的声明不写冒号 | `modules/gdscript/gdscript_parser.cpp:1773-1774` | §2.4 |
| `@abstract` 注解，不能用于 static 方法 | `modules/gdscript/gdscript_parser.cpp:4500-4524` | §4.1 |
| 剩余参数只能是最后一个参数，不能有默认值 | `modules/gdscript/gdscript_parser.cpp:1709-1716` | §4.6 |
| GDScript 的表达式中不能出现 `...` | `modules/gdscript/gdscript_parser.cpp:4344` | §4.6 |
| GDScript 的类型只能以标识符或 `void` 开头 | `modules/gdscript/gdscript_parser.cpp:3874-3886` | §2.4 |
| GDScript 的 lambda 含 await 时即为协程，不加标记 | `modules/gdscript/gdscript_parser.cpp:3249-3251, 3752-3753` | §9.1 |
| match 的模式 | `modules/gdscript/gdscript_parser.cpp:2487-2711` | §4.6、§5.6 |
| GDScript 的 match 中，字面量和表达式模式先比较类型，String 与 StringName 视为相同，再比较值 | `modules/gdscript/gdscript_compiler.cpp:1451-1477, 1550-1556` | §5.6 |
| `as` 转为对象类型失败时得到 null | `modules/gdscript/gdscript_vm.cpp:1677-1678` | §5.3 |
| GDScript 把不是对象的值 `as` 转为对象类型时，debug 构建报错，release 构建原样交出这个值 | `modules/gdscript/gdscript_vm.cpp:1670-1681` | §5.3 |
| GDScript 的 `is Array[T]` 要求数组的类型完全一致；把数组赋给带类型的变量时也这样要求，否则报错 | `modules/gdscript/gdscript_vm.cpp:879-895, 1454-1481` | §3.8 |
| GDScript 只在去处为带类型容器时为数组和字典字面量定型，否则它们是无类型的 | `modules/gdscript/gdscript_analyzer.cpp:2112-2121, 3647-3658` | §3.13 |
| GDScript 对已释放的对象做 `is` 时报错并中止，release 构建也一样；做 `as` 时只在 debug 构建中报错 | `modules/gdscript/gdscript_vm.cpp:942-947, 1665-1669` | §3.3 |
| GDScript 调用引擎方法时，debug 构建只检查基对象是否已释放，实参原样交出 | `modules/gdscript/gdscript_vm.cpp:2046-2055, 2072` | §3.3 |
| GDScript 中，局部变量、常量、`for` 的循环变量和 match 的绑定不能与外层块的局部变量或参数同名。参数是函数体的局部变量 | `modules/gdscript/gdscript_parser.cpp:1731, 1735, 1983-1987, 1993-2002, 2345-2349, 2599-2600, 4384-4392` | §4.6、§5.6 |
| GDScript lambda 的函数体以外面的块为父块，声明其参数时不检查外面的名字 | `modules/gdscript/gdscript_parser.cpp:1728-1735, 3758-3760` | §4.6 |
| `as` 转为内置类型时按构造函数转换，转换失败只在 debug 构建中报错 | `modules/gdscript/gdscript_vm.cpp:1642-1650` | §5.3 |
| GDScript 在运行时遇到运算表中没有的组合时报错。运算数类型与首次执行时不同的情况只在 debug 构建中检查，release 构建得到 null | `modules/gdscript/gdscript_vm.cpp:793-798, 821-843` | §5.2 |
| `for` 可以遍历 float 和向量 | `modules/gdscript/gdscript_vm.cpp:3092-3229` | §5.7 |
| 遍历对象时，`_iter_init` 和 `_iter_next` 收到装着状态的单元素 Array，返回真时继续。`_iter_get` 收到状态本身。调用后 Array 的长度不为 1 即为出错 | `core/variant/variant_setget.cpp:1368-1396, 1602-1631, 1798-1823`；`modules/gdscript/gdscript_vm.cpp:3355-3403, 3720-3770` | §5.7 |
| Object 在编辑器构建中把 `_iter_init`、`_iter_next`、`_iter_get` 登记为虚函数 | `core/object/object.cpp:1947, 1974-1985` | §5.7 |
| GDScript 编译遍历对象的 `for` 时只查找 `_iter_get`，以它的返回类型为循环变量的类型。编辑器构建中 Object 的虚函数也算找到 | `modules/gdscript/gdscript_analyzer.cpp:2326-2338, 6034-6047`；`core/object/class_db.cpp:1105-1109` | §5.7 |
| GDScript 的 `range()` 步长为 0 时报错，`for` 中的 `range()` 则一次也不循环。两处都把 float 参数转为 int | `modules/gdscript/gdscript_utility_functions.cpp:140-142, 192`；`modules/gdscript/gdscript_vm.cpp:3421`；`modules/gdscript/gdscript_byte_codegen.cpp:1590-1604` | §5.7 |
| PackedDataContainer 是引擎中唯一定义 `_iter_*` 的类，已标为弃用 | `core/io/packed_data_container.cpp:359-361, 386-388`；`doc/classes/PackedDataContainer.xml:2` | §5.7 |
| GDScript 的 `assert` 失败时报“Assertion failed” | `modules/gdscript/gdscript_vm.cpp:3876-3902` | §8.3 |
| GDScript 的 await 接受任意 Signal 值 | `modules/gdscript/gdscript_vm.cpp:2586-2600` | §9.8 |
| GDScript 对返回类型既不是协程、也不是 Variant 或 Signal 的调用的 await 给出“多余的 await”警告 | `modules/gdscript/gdscript_analyzer.cpp:3102-3107` | §9.8 |
| await 的结果不经检查写入按被等待者返回类型定型的临时变量 | `modules/gdscript/gdscript_vm.cpp:2667-2668`；`modules/gdscript/gdscript_byte_codegen.cpp:998-1001` | §9.8 |
| Variant 赋给有类型的变量时，只在 debug 构建中检查类型 | `modules/gdscript/gdscript_vm.cpp:1433-1444` | §9.8 |
| 挂起的 GDScript 函数状态不复制 self，只以裸指针指向实例 | `modules/gdscript/gdscript_vm.cpp:2609-2625` | §9.4 |
| GDScript 每次调用把实例装进 Variant 作为 self，RefCounted 在 PREDELETE 中因此得到 null；调用自己的方法以它为接收者 | `modules/gdscript/gdscript_vm.cpp:656-657`；`modules/gdscript/gdscript_byte_codegen.cpp:1345-1355` | §6.6 |
| GDScript 在每条语句结束和每个块开始时，清空可能含对象的临时值 | `modules/gdscript/gdscript_byte_codegen.cpp:135-147, 1911-1920`；`modules/gdscript/gdscript_compiler.cpp:1893, 2276` | §6.8 |
| GDScript 离开块时清空其中可能含对象的局部变量；循环体的局部变量到循环结束才清空 | `modules/gdscript/gdscript_compiler.cpp:1879-1886, 2103-2115` | §6.8 |
| GDScript 在循环内逐轮清空局部变量的代码被注释掉，标为 TODO | `modules/gdscript/gdscript_compiler.cpp:2106, 2140` | §6.8 |
| GDScript 把 `match` 的被匹配值和 `for` 遍历的值存入局部变量，在分支和循环执行期间一直持有 | `modules/gdscript/gdscript_compiler.cpp:1909-1924`；`modules/gdscript/gdscript_byte_codegen.cpp:1568, 1575-1582` | §6.8 |
| GDScript 出错后，debug 构建按返回类型构造默认值返回，release 构建返回 null | `modules/gdscript/gdscript_vm.cpp:3963-3993`（debug）；`:535, 3995`（release） | §8.1 |
| GDScript 的调用栈深度上限由 `debug/settings/gdscript/max_call_stack` 设置，默认 1024，超过时报告栈溢出 | `modules/gdscript/gdscript.cpp:2870`；`modules/gdscript/gdscript.h:489-490` | §8.1 |
| GDScript 的 `assert` 只在 debug 构建中编译 | `modules/gdscript/gdscript_compiler.cpp:2181-2207` | §8.3 |
| GDScript 在判断 `assert` 的条件之前求值其消息，并要求消息是 String | `modules/gdscript/gdscript_compiler.cpp:2185-2198`；`modules/gdscript/gdscript_analyzer.cpp:2399-2405` | §8.3 |
| GDScript 的链式赋值沿链读出、修改，再把值类型写回，共享类型原地修改 | `modules/gdscript/gdscript_compiler.cpp:1172-1190` | §3.6 |
| GDScript 的链式赋值先求基对象并沿链读出中间值，再求右边，然后求最后一个下标，最后写入并写回 | `modules/gdscript/gdscript_compiler.cpp:1069, 1097-1121, 1124, 1134, 1157-1162` | §3.6 |
| 成员的初始化式不经过 setter | `modules/gdscript/gdscript_compiler.cpp:2414-2430, 2603-2626` | §4.8、§6.1 |
| 创建实例时先从基类到本类运行成员初始化式，再调用 `_init` | `modules/gdscript/gdscript.cpp:140-148, 176, 192-194` | §6.4 |
| 构造调用不等待 `_init` 的返回值；带返回值的引擎虚函数同步读取并转换脚本返回值 | `modules/gdscript/gdscript.cpp:194`；`core/object/make_virtuals.py:1-10, 173-174` | §9.1 |
| 静态成员初始化之后调用 `_static_init` | `modules/gdscript/gdscript_compiler.cpp:2629-2632` | §4.8 |
| GDScript 先初始化外层类的静态成员，再依次初始化其内部类 | `modules/gdscript/gdscript.cpp:667-683` | §4.8 |
| GDScript 只在启用了脚本的进程中或 tool 脚本中运行静态初始化 | `modules/gdscript/gdscript.cpp:803, 882-887` | §4.8 |
| `_static_init` 必须是静态的，没有参数，不返回值 | `modules/gdscript/gdscript_parser.cpp:1751-1757`；`modules/gdscript/gdscript_analyzer.cpp:1841-1850` | §4.8、§6.1 |
| 静态变量不能 `@export`、`@onready` | `modules/gdscript/gdscript_parser.cpp:4536-4537, 4665-4666` | §4.8 |
| GDScript 中对象类型的静态变量从 null 开始 | `modules/gdscript/gdscript_compiler.cpp:2555-2590` | §4.8 |
| 不带 `@static_unload` 时，有静态变量的 GDScript 脚本一直留在缓存中 | `modules/gdscript/gdscript_compiler.cpp:3315-3316`；`modules/gdscript/gdscript_cache.cpp:449-453` | §4.8 |
| GDScript 允许经实例访问静态变量 | `modules/gdscript/gdscript_analyzer.cpp:4229` | §4.1、§4.8 |
| GDScript 的脚本和实例按名字读写静态变量 | `modules/gdscript/gdscript.cpp:973-985, 1022-1043, 1562-1587, 1634-1646` | §4.8 |
| GDScript 保留状态的重载重新运行静态初始化，再按名字搬回旧的静态变量值 | `modules/gdscript/gdscript.cpp:714-731, 803-807, 882-892` | §4.8 |
| `@onready` 初始化式在用户的 `_ready` 之前执行，先基类后派生类 | `modules/gdscript/gdscript.cpp:1937-1953` | §6.3 |
| GDScript 实例释放时清除其挂起的函数状态，不恢复执行 | `modules/gdscript/gdscript.cpp:2069-2079`；`modules/gdscript/gdscript_function.cpp:343-362` | §9.7、§9.8 |
| GDScript 函数开始时取得实例成员的地址，实例在函数运行中释放时，成员随之释放 | `modules/gdscript/gdscript_vm.cpp:751`；`modules/gdscript/gdscript.cpp:2066-2084` | §3.3 |
| `GDScriptFunctionState` 有 `completed` 信号 | `modules/gdscript/gdscript_function.cpp:369` | §9.8 |
| GDScript 协程再次挂起时，内层状态只以 ObjectID 记住第一个状态的 `completed` 信号 | `modules/gdscript/gdscript_vm.cpp:2634-2639` | §9.8 |
| GDScript 函数状态恢复执行时离开实例的待处理列表 | `modules/gdscript/gdscript_function.cpp:329-330` | §9.8 |
| `GDScriptFunctionState` 是内部类，不暴露它所属的实例；`is_valid` 在恢复执行之后总是为假 | `modules/gdscript/gdscript_function.cpp:288-307, 337, 364-370`；`modules/gdscript/register_types.cpp:140` | §9.8 |
| GDScript 脚本重载时清除挂起的函数，不发出信号 | `modules/gdscript/gdscript.cpp:1487-1506`；`modules/gdscript/gdscript_compiler.cpp:2700` | §9.8 |
| GDScript 保留状态的重载让所有实例换用新代码，按成员名搬移旧值，不检查类型 | `modules/gdscript/gdscript_compiler.cpp:3070-3074`；`modules/gdscript/gdscript.cpp:2038-2064` | §12.4 |
| await 多参数信号得到由实参组成的 Array | `modules/gdscript/gdscript_function.cpp:256-274` | §9.6 |
| 被等待的对象被释放后，GDScript 的等待方永远挂起 | `modules/gdscript/gdscript.cpp:2069-2079` | §9.7 |
| RefCounted 类中的 lambda 强引用 self | `modules/gdscript/gdscript_lambda_callable.cpp:285-291` | §3.11 |
| GDScript 不用 self 的 lambda 和静态函数值以脚本为对象，lambda 还持有脚本。4.7 的 UndoRedo 跳过对象已不存在的 Callable | `modules/gdscript/gdscript_lambda_callable.h:45`；`modules/gdscript/gdscript_lambda_callable.cpp:75-77`；`modules/gdscript/gdscript.cpp:993`；`core/object/undo_redo.cpp:357-360` | §3.11 |
| GDScript 的 lambda 每次调用都把捕获作为实参接收。给捕获赋值既不改变外面的变量，也影响不到下一次调用。GDScript 对此给出警告 | `modules/gdscript/gdscript_lambda_callable.cpp:101-120`；`modules/gdscript/gdscript_analyzer.cpp:2895-2917` | §3.11 |
| GDScript 的 lambda 可以命名，名字用于标识它的 Callable，不在其函数体中声明 | `modules/gdscript/gdscript_parser.cpp:3731-3733`；`modules/gdscript/gdscript_lambda_callable.cpp:61-62` | §3.11 |
| GDScript 重新加载时把 lambda 换成新版本中同一位置的 lambda，lambda 的个数、捕获或参数不同时使它失效 | `modules/gdscript/gdscript_compiler.cpp:3217-3254`；`modules/gdscript/gdscript.cpp:1411-1421` | §12.4 |
| GDScript 的 LSP 是编辑器插件 | `modules/gdscript/language_server/gdscript_language_server.h:35`；`modules/gdscript/register_types.cpp:167` | §13.1 |
| 编辑器默认用制表符缩进。在开括号后换行时多缩进一层；闭括号紧随光标时移到下一行，回到原来的缩进 | `editor/settings/editor_settings.cpp:830`；`scene/gui/code_edit.cpp:1349-1385, 4128-4131` | §13.2 |
| 编辑器注释选中的行时，把 `#` 插在每行的缩进之后，空行的插在行首 | `editor/gui/code_editor.cpp:1356-1371`；`scene/gui/text_edit.cpp:4194-4202` | §13.2 |
| GDScript 导出时至多把源码转为词法单元，加载时才解析和编译 | `modules/gdscript/register_types.cpp:99-116`；`modules/gdscript/gdscript_cache.cpp:373-379, 392` | §1.3、§4.5 |
