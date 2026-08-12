# Luau 代码规范:测量结果与提纲

> Archived 2026-08-12: the survey measured a fifteen-file runtime that no
> longer exists. The current re-survey and executable-gate decision live in
> [the consolidated outstanding plan](../../plans/2026-08-12-outstanding-work.md)
> `T-008`; the old symbol-by-symbol edits are superseded rather than owed.

2026-08-02。**本文只是记录,代码一行未改。**下面每一项改动都是一次独立的批准决定,
不打包。

> **计数与行号的时效(2026-08-03 复核)**。测量做在 2026-08-02。此后
> `333b114`(注释只留约束、故事移入文档)从 `modules/task/runtime/` 净删了约 1200 行,
> `cef4886`(矩形三选一)与 `0b5c1e5`(长按 + 空读裁决)又加了一批,所以
> **本文写下的行号一处也不作数**——按符号名去找(`requireCtx`、`renderValue`、
> `k_sectionFields`),别按行号。**聚合计数同样没有重测**,仍是 08-02 那次普查的
> 数字:`table.freeze` 当时 37 处、现在 41 处;`error()` 当时 248 处、现在 272 处;
> 文件仍是 15 个,行数 8907 → 9088。第 2 节那个「全删掉仍然全绿」的证伪实验是在
> 37 处的状态下跑的,没有重跑。

## 为什么要有这一份

仓库有一份 C++ 规范(`.claude/skills/cpp-coding/references/coding-standard.md`),
**没有任何 Luau 规范**。`modules/task/runtime/*.luau` 那 15 个文件、8907 行、
368 KB 是可信 framework,它跟 C++ 共用一套概念、常量成对出现、失败要在两边之间传递,
却全靠习惯维持写法。

出发点是一个具体问题:`.luau` 里为什么用 `k_` 前缀。答案是它从 C++ 规范第 47 行漏过来的,
仓库里没有任何文档裁决过。顺着这个问题把 15 个文件按六个维度量了一遍(命名、不可变性、
失败处理、模块结构、类型标注、注释),再对照 C++ 规范和现有门禁的可执行性。

## 先说测出来的三个真问题

这三个不是风格,是缺陷。**三条都已在 2026-08-02 落地**——1 是 `10b8a7f`,2 是
`804b398`,3 是 `d49082b`——下面保留的是当时的测量和判断依据,不是待办。

### 1. `requireCtx` 的错误层级是错的(当时的活 bug,`10b8a7f` 已改抛 level 3)

`requireCtx` 用 level 2 抛错,但它被这个文件里 **7 个公开 verb** 调用。level 2 指的是
调用者的那一帧,而在这条链上那一帧是 `observe.luau` 内部的 `requireCtx(ctx, "observe.find")`
那一行——于是报错指向 framework 自己的源码,而不是工程脚本里真正传错 ctx 的那行。

同一文件里四十余行之后的兄弟 `readTarget`(`observe.luau:330`)用的是 level 3,
注释(`observe.luau:327-329`)写着的正是前者违反的那条规则。**全仓库没有任何测试断言过
error level**,所以 CI 看不见。

### 2. 整套不可变约定从未被证伪(`804b398` 已补上对抗用例)

`table.freeze` 在 runtime 里用了 37 次。把 `model.luau`、`navigation.luau`、`oracle.luau`
里**每一个 freeze 全部删掉,测试套件仍然全绿**。`grep -rn "isfrozen" tests/` 的 14 处命中
全是关于 Luau 标准库、`uf` 根和 `ctx` 的,没有一处针对 Element / Page / Reference / Hit /
Receipt / Edge / Graph / Claims。

按仓库自己的纪律,一个删掉被守护属性之后仍然绿的测试等于没有守护任何东西。补一组
「写入必须抛错」的对抗用例约 20 行,能让这 37 处 freeze 一次性变成可依赖的。

### 3. `mint.frozen_extra` 是浅冻,且通向存盘时的静默删字段(`d49082b` 已递归冻)

`mint.luau:106-115`,6 个调用点。`Element.new{ extra = { tags = {...} } }` 冻出来的元素,
它的 `extra.tags` 仍然是调用方那张可写的表——头部注释声称的「快照」在这里是假的。

更糟的是第二段:`project.encode` 的 `renderValue`(`project.luau:247`)把任何表都按数组渲染,
所以 `extra` 下面的嵌套 map 存盘时变成 `[]`。这正是 extra/residual 这套设计要防的
静默删除。一行能修——要么递归冻,要么 `check_extra` 直接拒绝嵌套表。

## 规范的主干

### 第一条规则不是命名,是线格式

**数据字段是 `snake_case`,Luau 变量绑定永远不是。** 测量:672 处字段/键名,0 处驼峰;
547 个 `local` 声明,唯一带下划线的 45 个是 `k_` 常量。

理由不是统一。`project.luau:77-169` 的 `k_sectionFields` 就是工程文件的合法键名表,
对 `[[reference]]` / `[[edge]]` / `[[screen]]` / `[[expect]]` 而言,它与导出类型的字段
一一对应。同一个名字同时是 TOML 键、脚本可见的 API、和类型字段。

失败方式是**静默丢数据**:`project.parse` 把不在表里的键路由进 `residual`
(`project.luau:791-795`),原始行照样往返写回磁盘,文件看着没问题,但 `project.build`
从不读它。在 reference 上写成 `pinnedAppearance` 的后果是点击不再遵守那个 pin、
改为搜索所有 appearance,全链路无人报错。`project.luau:22-27` 自己把这类 bug 称作
「持久层最糟的失败」。

**这一行相对 C++ 规范是反的**(C++ 的公开数据成员是驼峰),规范开篇必须点明,
否则从 C++ 过来的人一定搞反。局部变量不许用 `snake_case` 是这条的执行半边:
局部一旦允许下划线,检查器就再也无法把一个 `snake_case` 名字读成「放错位置的字段」。

### 其余各节的组织方式:C++ 的哪条不成立,拿什么补

| C++ 那节 | Luau 这边 |
|---|---|
| `## Ownership`(`unique_ptr`/`shared_ptr`/`span`) | 无法表达,`...Ref`/`...Ptr`/`...Handle` 一律禁止(现为 0 处)。替代物是「交出去的值是冻结快照」`[17 处]` 与「交出去的集合是 `table.clone` 的可变副本」`[6 处,四处注释用近乎相同的话说了同一件事]` |
| RAII | verb 收一个 body 回调、在 `pcall` 下释放,资源永不以打开的句柄交给调用方(`ctx:step`、`ctx:cycle`)。`ctx:cycle` 的头注已经写出了这个形式:「cycle 逃不出这个块,所以『总是关闭』和『至多开一个』是 API 的性质,不是作者自觉的性质」 |
| `## Errors and invariants` / `Result<T>` | 抛错取代返回值。**每个 `error()` 都显式传 level**`[248/248]`;公开 verb 传 2、深一帧的辅助函数传 3`[207 处]`;level 0 专指「这个值不该获得源码位置」`[39 处,三类]` |
| `## Enums` | `k_<domain>Known: { [string]: boolean }` 集合加一个校验构造器`[6 处]`。这是 Luau 的 scoped enum |
| `## Arithmetic and conversions` | 换成一段说明限制的话:Luau 只有一种数字类型,`mint.whole_number` 对 `2^60` 返回真,`inf`/`nan` 和字符串强制转换是 C++ 没有的风险 |
| `StrongValue` / `StrongId` | **拿不到,也不能靠命名假装拿到**。element / page / screen / capability 名全是裸字符串;部分买回来的是成员校验(`requireDeclaredElement` 等),它只在两个命名空间不相交时有效,而没有任何东西保证这一点 |
| `## Type aliases` / `## Type placement` / `## Namespaces` | 全部删除:这个 VM 里没有模块解析器,只有全局绑定(`project.luau:173-176`、`navigation.luau:75-81`),一个 chunk 根本无法命名另一个 chunk 的导出类型。取代它的是**两层可见性**——`local function lowerCamelCase` 是私有,`module.snake_case` 是公开,不能被够到的东西就不能挂在模块表上 |
| `## Naming` 的 `g_`/`m_`/`p_`/`s_` | **不带过来**,规范要明写而不是沉默(现为 0 处)。模块级 `local` 是生存期等于一个 VM 的闭包上值,不是 `g_` 标记的程序级存储,借用前缀等于断言一件假事 |

`k_` 保留,但它的**唯一真实职责是跨语言**:`grep -r k_maximumColour` 能同时找到
`model.luau:85-89` 的常量和它在 `modules/vision/.../frame-analysis.hpp` 里的原件,
这是让 765 这个数不漂的全部机制。在 Luau 内部它不产生任何保证。

### 两个读者

- **[framework]** —— `modules/task/runtime/*.luau`,15 个文件。可信、在仓库内、经评审、
  很少改动,承担昂贵的规则。
- **[project]** —— `E:\umbraflow-projects\<项目>\tasks\<名字>.luau`。在仓库外、不在任何构建里、
  由 agent 在时间压力下写,一个任务一个文件。只给两三条。

判据:一条规则若要读 framework 源码才能理解,它就不是给工程脚本的。另外,**工程环境
已经在机制上不可违反的事不是规则,是注解**——`framework-bundle.cpp:70-79`
「工程环境是一张没有元表的白名单,没有发布的名字任何路径都够不到」,别把工程脚本的
规则额度花在可达性上。

### 开篇必须先承认的事

**今天没有任何机制检查 `.luau`。** `scripts/check_safety.py:15` 只认 C/C++ 扩展名;
`scripts/fix_format.py:12-31` 列了 18 种文本扩展名,`.luau` 不在其中;构建期
`framework-bundle-syntax.cpp:14` 只解析、随即丢掉 AST;**没有任何类型检查跑过**,
所以 8907 行里每一个 `--!strict`、每一处类型标注,目前都只是带高亮的注释。

C++ 规范自己警告过「一条只是*看起来*被强制的规则,比一条公开承认交给读者的规则更糟」。
新文档如果不先说这句,就是在重犯它。

## 故意不裁决的事

写下这些和写下规则一样重要——每一条要么数数会数出一个代码有书面理由去违反的「规则」,
要么它是一个本文不该越权替代的设计决定。

- **`navigation.stack_*` 该不该改成 `Stack` 命名空间表**。21 处用命名空间表、8 处用
  `stack_` 前缀,其中 3 处连前缀都不带——现状不是两种约定,是零种。不能靠数数解决:
  stack 是唯一一个**故意可变**的对象(`navigation.luau:128-131`「它是信念,没有东西
  压在它上面」),而每个 `.new` 都冻结,这个关联是真的但**从未被写成理由**。
  答案该落在 `navigation.luau:612`,无论选哪个。`stack_new` 用 `options` 而不是 `spec`
  是同一条缝,要么一起改要么都不改。
- **用局部变量遮蔽已发布模块名**。1 处注释声称这是规则,4 处在做。一比四不是约定。
  而且载入顺序检查器的设计依赖「`ctx` 作为参数名合法」。留待:要么把
  `regress.luau:545` 里遮蔽了 `hits` 的那处改名,要么删掉 `observe.luau:919-921`
  那段声称规则的注释。
- **`check` 作为返回句子的校验器前缀**。4 处用、3 处不用,太弱不足以成规则。
- **`Millis` / `Milliseconds` / `milliseconds`**。5 比 1 比 2。数据键那侧已经统一
  (`timeout_ms` / `interval_ms` / `backoff_ms`),那才是要紧的一侧。
- **报告类对象要不要冻**。`regress` 的 `Verdict`(`regress.luau:1054`)是贯穿整个遍历的
  累加器。若决定不冻,文件头必须说出来——这里其他每个集合都是冻的,读者会默认它也是。
- **`project.is_model`**。载入后的 Model 正确地保持可变(scribe 要往里写),但四个消费方
  各自按形状重新推断可信度。加一张身份注册表是真实的改进,也是真实的设计变更。

还有五条明确不做成门禁,各带一句理由:freeze 覆盖率(需要逃逸分析,白名单只是换了张皮的
抑制列表)、「不许写入收到的参数」(需要作用域追踪,外加六处有据可依的例外)、
每张注册表配一个 `is_` 谓词、禁止重名导出类型、要求写全每个字段。

## 改动清单(按价值排序,每行一次批准)

**1–4 已在 2026-08-02 落地,不再列在下面**:1 `requireCtx` 改抛 level 3
(`10b8a7f`);2 对抗性 freeze 测试(`804b398`,`tests/task/test-script-owned-model.cpp`);
3 `mint.frozen_extra` 递归冻(`d49082b`,`mint.luau` 的 `frozenCopy`);4
`scripts/fix_format.py` 认 `.luau`(`4489f33`,`SPACE_INDENTED_EXTENSIONS`)。编号
不重排,后文的 `#1`、「1–7」仍指原来的那一项。

| # | 改动 | 规模 | 为什么在这个位置 |
|---|---|---|---|
| 5 | 删掉 `ctx.luau` 的两个 wait 默认值 | 2 行 | 已死(`modules/`、`entry/`、`tests/` 中零引用),且与 `observe.wait_until` 明确拒绝提供默认值的立场矛盾。留着早晚有人伸手去拿 |
| 6 | 冻 `scribe.measure` 的 Measurement 及其嵌套 `rect`/`key` | 1 个返回 + 2 处嵌套 | 堵上调用方在 `measure` 和 `author_element` 之间改写 `m.key.tolerance` 的洞;`scribe.luau:363-372` 已经写出了这条要保护的性质 |
| 7 | 冻 `reading.measure` 的两个返回 | 2 处 | 只被消费一次、从不被改,零成本,消掉与 `observe.read_element` 之间唯一没有解释的不对称 |
| 8 | 写 `scripts/check_luau.py` Tier 1(5 条)+ `tests/test-check-luau.py` + 注册进 CMake / CLAUDE.md / post-change-validation | 新增约 230 行 | 把第 2、3、4 节和第 9 节第一条从散文变成门禁。**今天全绿,所以必须随附证伪 fixture** |
| 9 | 给 `ctx.luau`(27 个函数)和 `task.luau`(1 个)补类型标注,然后加 `--!strict` | 2 个文件 | 顺序要紧:先加 pragma 只会产生噪音。做完才谈得上那条 3 行的检查 |
| 10 | 检查器扩展 level 2/3 规则 | 约 70 行 | #1 已落地,所以这一条现在才值得做;需要按头部自身缩进列做 `end` 配对,否则报 4 个假阳性 |
| 11 | `model.luau` 去掉 7 个 `mint` 别名,改为全限定调用 | 7 处声明 + 调用点 | 可读性,不是洞:今天 `checkExtra(...)` 与私有辅助函数形状完全相同,却跨了模块边界 |
| 12 | 冻那 21 张 `k_` 常量表 | 21 处一行改动 | framework 内部,运行时零成本(`table.clone` 一张冻表返回可变副本,`model.capability_order()` 不受影响)。防的是将来一次误写永久放宽校验。**顺手做,不专门做** |
| 13 | 裁决 `navigation.stack_*` | 8 处,或 1 段注释 | **需要先做设计决定,不是一次编辑** |
| 14 | `regress.luau:545` 的 `hits` 改名,或删掉 `observe.luau:919-921` | 1 处 | 二选一:今天一段注释声称的规则,代码五处只守一处 |
| 15 | 本地门禁和 CI 加一步 `luau-analyze` 覆盖 `modules/task/runtime/` | 新工具链 | 唯一能让第 6 节——8907 行里每一处标注和每一个 `--!strict`——真正成立的东西。也是文档里最大的一笔借来的承诺,所以排在最后而不是最前 |

1–7 合计约 40 行源码改动,关掉了找到的每一个具体失败;1–4 已落地,剩下 5–7。
8–12 是门禁建设。13–15 需要一次决定或一套新工具,不能并进前面几项。

## 规范文档本身

写的时候是 `.claude/skills/` 下 C++ 那份的兄弟篇,**英文**(仓库规则:提交进仓库的
规则一律英文,哪怕讨论全程中文)。十七节,每节标注 [framework] / [project] / [both],
并标明由检查器、测试还是评审强制——C++ 那份的「一个标记要精确说出它的工具覆盖什么」
同样适用。
