# Luau-First Task System 设计草案评审

> **2026-07-29**:本文结论已被开发者采纳,并并入
> [`docs/plans/2026-07-29-three-layer-task-system.md`](../plans/2026-07-29-three-layer-task-system.md)。
> 以下正文保留为该日期的评审记录,不再更新。
>
> 评审对象:[`docs/plans/2026-07-28-luau-first-task-system-design-draft.md`](../plans/2026-07-28-luau-first-task-system-design-draft.md)(提交 `45f91cb`)。
>
> 日期:2026-07-28。方法:六个维度独立评审 + 对抗验证,一个维度对 vendored
> Luau 0.730 编译并运行了实测 spike。结论中标注「实测」的均为跑出来的,标注
> 「已复核」的为评审者在本仓库源码中直接确认。
>
> 本文是评审意见,不改变任何决定。裁决权在开发者。

## 结论

草案的**分层判断是对的**:C++ 拥有证据、授权、取消和 trace 盖章,Luau 拥有
「接下来做什么」。ADR 0001 和 ADR 0002 在草案下都成立。第 1–6 节的职责划分可以
按原样接受。

但**执行计划的排序是错的**,而且草案对自己方案的论证在几处被落地代码直接证伪。
按证据,建议采纳约四成:

| 草案阶段 | 建议 | 理由 |
|---|---|---|
| A(framework facade) | **现在做**,但收窄:只做 `TaskHost` 抽出 + 一层 Luau framework 盖在现有同步 verbs 上,**不改根命名** | 唯一「现有代码违反最终形态原则」的地方;不丢弃任何东西 |
| D(把 task policy 移入 framework) | **紧接 A 做** | 这里才是真正的能力缺口(见下) |
| F(迁第一个真实游戏) | **现在做**,并让它决定后面还要不要做 | 先跑通一个真日常,再谈模块系统 |
| B(private native surface) | **推 P1**,收窄为「project 环境不再暴露 raw verb 形态」 | 有价值的一半与命名无关 |
| E(project modules + manifest) | **推 P1**,且不要 manifest | 已有决定就是 P1;三分之二的功能 task-loader 已有 |
| C(yield/request 协议) | **不做**,留触发条件 | §7.1 的三条论证被落地代码逐条证伪 |

## 一、真正的能力缺口,以及草案把它排在了错误的位置

草案想解决的问题里,**今天的设计确实做不到的只有一件事:长等待期间处理弹窗**。

脚本调 `umbra:wait_for_page` 会在 C++ 里一直阻塞到超时(默认 10 分钟,
`task-context.hpp:51-55`)。每轮观察周期在
`engine::EngineSession::waitForPage`(`session.cpp:541-590`)里面,它每轮调一次
`sweepKnownPopups()`(`:558`),而那是个永久 no-op(`:592-595`)。所以今天要处理
「等待中弹出的网络错误框」,只能在 `modules/engine` 里写一个 C++ 弹窗注册表——
这会直接违反草案验收标准 1 和 2,也违反「第二个游戏只改 `.luau`」的目标。

这就是整个迁移最硬的论据。草案把它埋在 Phase D,前面挡着两阶段 API 翻修和一阶段
协议重写。

**关键判断:Phase D 不依赖 Phase C。** 把 `wait_for_page` 搬进 Luau 需要的所有
东西里,C++ 今天只缺一个:有界的、感知 stop token 的等待原语。而它已经以
`pollSleep` 的形态存在(`session.cpp:56-79`)。framework 的循环是

```text
capture -> 跑 interrupt -> 解析页面 -> 不匹配就 release -> poll(deadline, interval)
```

每一步都是一次普通的 C 绑定调用,没有一步需要 yield。

### §7.1 的三条论证都被落地代码证伪

1. **「C++ host call 成为取消的长临界区」** — `EngineSession::waitForPage` 每轮
   重查 stop token(`session.cpp:580`),`pollSleep` 已把睡眠切片。上了 yield
   之后,driver 仍然在 `executeRequest` 里同步执行每次 capture/识别,临界区一样长。
   唯一真实的不可抢占缺口是 `IFrameSource::capture()` 端口没有 deadline/stop_token
   ——那是 P0-B 已承诺、至今未做的事(`2026-07-27-p0b-script-layer.md:54-56`),
   和 yield 无关。
2. **「统一 trace 边界」** — 每个 verb 今天已经在成功和失败两条路上都发
   `HostCall`(`umbra-tables.cpp:470` 及六处调用点)。
3. **「取消不可被吞」** — 见下面第二节,窄例外在 yield 设计里同样存在,因为
   request executor 仍然是一个 C 帧。

而 §7.2 明说 `ResolvePage` 和 `FindAction`「即使当前实现可以同步返回,仍建议纳入
统一 effect 协议」——这正是本项目在 `task-context.cpp:34-50` 拒绝加空弹窗钩子时
定下的标准所反对的:为零当下收益增加未测试分支。

成本也是所有阶段里最大的:script substrate 要加 load/resume/inspect-yield API,
`umbra-tables.cpp`(1223 行)从 verb 表面重构成 request executor,
`test-task-binding.cpp`(882 行)要按 request 序列重写,还要在第一个真日常之前
先建一整套 Luau fake-driver 测试。

**重开条件**(写进文档,不靠记忆):① 直接绑定下实测 500ms 取消预算失败;
② P2 pause 需求被证明无法用「阻塞式、感知 stop 的 checkpoint」满足。
注意 pause 本身不构成 Phase C 的理由:一个感知 stop 的阻塞 checkpoint 就能把线程
停在安全点并保住 generation,这正是 §9.2 承诺的行为。

## 二、必须先改的硬问题

### 1. §7.4 的循环会在硬取消之后继续执行请求(blocker,若做 Phase C)

`lua_break` 在 `nCcalls > baseCcalls` 时不会 break,而是抛一个**普通的、可被
pcall 捕获的**运行时错误(`ldo.cpp:766-772`,已复核)。这条在本仓库已经记录过两次
(`sandbox.cpp:240-256` 的注释、`p0b-script-layer.md:182-190` 的 2026-07-27 实测)。

实测 spike 显示:取消发生在 `table.sort` 比较器里 → 项目级 `pcall` 捕获 → 脚本继续
→ yield 出一个**全新的、由宿主合法 mint 的** DriverRequest。§7.4 的伪代码只区分
completed / failed / yielded,于是它会通过 §5.2 的每一项校验(对的 generation、
对的 kind、对的 handle)并**执行**它 —— 也就是在 run 已取消之后投递一次点击。
另一个 spike 显示返回 `LUA_BREAK` 的线程可以被直接再 resume 并继续跑,所以
「不再 resume」只有在宿主真的 latch 了才成立。`LUA_BREAK` 还是 §7.4 三分法没有
点名的第四种 `lua_resume` 状态。

今天的代码没有这个洞:`InterruptState::broken` + `markFatal`/`guardFatal`
(`umbra-tables.cpp:370-404`)在 C++ 侧闩住。**草案等于删掉一个现有保证。**

修法:resume 返回后、分类之前查一次 latch,`executeRequest` 之前再查一次;
`LUA_BREAK` 单独映射 Cancelled。

### 2. `umbra:try` 是一个没有 continuation 的 C 闭包,任何 yield 落在它下面都会坏(blocker,若做 Phase C)

`tryFn` 用 `lua_pushcclosure` 注册(`umbra-tables.cpp:1020-1026`),内部调
`lua_pcall`(`:845`)。`luaD_callint` 只在调用帧是**带 continuation 的** C 闭包时
才保持可 yield 不变量(`ldo.cpp:307-324`)。所以它下面的任何 yield 都会失败,
而且实测中这个失败被 `tryFn` 自己的 `lua_pcall` 吞掉,表现为一个普通的
`false` 返回。

草案 Phase A 让 framework 适配现有 `umbra:try`,Phase C 才引入 yield —— 中间必然
有一个窗口,所有做 Capture/Click 的 `ctx:try`/`ctx:retry` 体都是坏的。

好消息(已复核):Luau 的 `pcall`/`xpcall` 本身**是可 yield 的**,两者都用
`lua_pushcclosurek` 带 continuation 注册(`lbaselib.cpp:509,512`)。所以纯 Luau 版
`ctx:try` 没有这个问题。约束是:第一次 yield 落地时,调用路径上不能残留任何
调 `lua_pcall`/`lua_call` 的宿主 C 函数。

### 3. C 边界的可 yield 矩阵会反向约束 §8.3/§8.5/§10.3(major)

实测结果:

- **可以 yield**:`__index` 是表时经它到达的方法、`__call`、generic-for 的循环体、
  深层 Lua 递归、`pcall` 内、`xpcall` 的被保护函数内。
- **不能 yield**:`__index` 是函数、`__tostring`、`table.sort` 比较器、
  `string.gsub` 回调、generic-for 的迭代器函数(`LuauYieldIter2` 默认关,本仓库
  未设 fflag)、**`xpcall` 的错误处理函数**。

最后一条最危险:它不报错,run 返回 OK 且值为 `"error in error handling"`,原始
错误和请求一起静默丢失。

推论:资源与观察句柄的方法必须挂在**表形态的 `__index`** 上,不能用函数形态
(今天 `umbra-tables.cpp:992-993` 正是表形态,不能退化);framework 的
cooperative cleanup 不能从 `xpcall` 错误处理函数里发请求。

## 三、悄悄推翻的已锁定决定

这几条草案都没有引用被推翻的文档,也没有给论证。

### 1. `umbra` 命名空间(你亲自定的)

`CONTEXT.md:11-17` 把 `umbra` 定为「脚本触达全部宿主能力的唯一只读全局根」;
`p0b-script-layer.md:17-21` 记录这是 2026-07-27 你在 grill 里的裁决,绑定 AST
校验器、trace、错误消息和全部示例;`annotation-design.md:376-411` 是 **S0 LOCKED**
的 `umbra.recognizers.NAME` / `umbra.pages.NAME`。

草案 §8.3 改成 `resources.pages.NAME` / `resources.actions.NAME`,§22 推荐
`task` + `resources` 双根。这是**三重推翻**:根名、单根性质、以及
`recognizers` 被改名成 `actions`。

草案给的唯一理由是「raw C++ verbs 不该挂在 public global 上」——那论证的是
**把动词搬走**,不是把资源根改名。`umbra.pages` / `umbra.recognizers` 挂在
framework 拥有的 `umbra` 根下,§8.3 的每一条目标都满足。

代价:438 行的 AST 校验器 + 354 行测试 + `_G` 逃逸后补的 7 条绕过回归 +
CONTEXT.md + annotation-design §4 + 所有错误字符串和文档示例。买到的新安全性
为零。

还有一个**技术性后果**:`rootsAtNamespace`(`script-validator.cpp:110-119`)只能
锚定在 `AstExprGlobal` 上。如果 `resources` 像 `Context` 那样通过 framework 闭包
/参数/upvalue 交给 project,就没有全局可锚,S0 的资源闭包**在静态上不可执行**,
只剩运行时 nil —— 这是对验收标准 23.10 的实质削弱。

**建议**:保留 `umbra.recognizers.NAME` / `umbra.pages.NAME` 为永久拼写。
校验器只做加法(承认 framework 根),Phase B 时删掉 `umbra:VERB(...)` 冒号动词
形态 —— 是 `script-validator.cpp:121-140`、`252-290` 的一次编辑,不是重写。

### 2. 项目磁盘布局(S0 LOCKED,且真机验证过)

`annotation-design.md:33-70` 是 S0 LOCKED 的磁盘契约:手写 `project.toml`、
GUI 拥有的 `annotations.toml`、`assets/sources/HASH.png`、
`assets/templates/HASH.png`、生成的 `generated/annotations.runtime.toml`(schema id
`umbraflow-annotations/v2`)。全是 TOML,全按内容 hash 寻址,这条路径已落地并
端到端真机验证(`TODO.md:86-105`)。

草案 §15 换成 `project/runtime/manifest.json` + `runtime/templates/` —— 不同目录、
不同文件名、不同序列化格式,零论证。§8.9 又加了第二个 `tasks/manifest.json`,
和 annotation 生成的那个并列,没有说谁优先。

### 3. D6/D7 的 P0/P1 切分

roadmap `:110-115` 写得很明确,而且是当作范围否决写的:P0 只要**最小** D6 清扫,
「重机制推 P1(注册 API、first-match、max_hits、不重入)」;D7 跨文件复用推迟,
P0 接受单文件复制粘贴。`TODO.md:130-131` 重复了一遍。

草案 §8.8 把那四项延后机制**逐字**指定为第一版 framework 语义,§8.9 + Phase E
交付 manifest 声明的跨文件模块、`task.import`、`ctx:call`,而 Phase F(跑第一个
真实日常)排在两者之后。等于在产品跑通一次真日常之前先上两个 P1 特性。

需要说清的是:「framework 随产品发布,所以要按最终形态设计」是对的,但那要求的是
**接缝**对,不是要求在第一个日常跑起来之前就实现 `max_hits` 和模块加载器。

### 4. ADR 0002 明确拒绝的「盲设计 task manifest」

ADR 0002 写着:承重的是**寻址模型**,P0 故意用目录约定(任务名 = 文件名),
P1 的 task manifest 「在 D7 真实需求已知后加法式扩展,而不是今天盲设计那个格式」。
草案 §8.9 现在就把它设计出来了(entry modules、allowed modules、每源 content hash、
framework 兼容区间),而且没说 hash 由谁算、谁维护、过期了怎么办。

### 5. p0b §6 的待裁决项被「以断言方式」回答

- 第 1 项 `umbra:now()` 语义:草案 §11 直接采用「逻辑序数」并顺手把动词改名成
  `ctx:logical_time()`,没有说明这里本来有个待决问题,也没提改名。
- 第 2 项 D6 落点:§17 判给 Luau interrupt registry,与实现方记录的判断相反,但
  没有解决「task 侧钩子看不见 engine 内层轮询」这个原始理由(答案其实是:内层
  轮询整个搬走了,所以理由消失 —— 但草案没写)。
- 第 3–5 项完全没提。

另外,p0b §2 把 `IFrameSource::capture()` 补 deadline/stop_token 升级成 P0-B 承诺
(理由是否则 500ms 硬取消永远有一个不可抢占缺口),`p0b:135` 记录它仍未做。
Phase A–F 一个阶段都没有排它。

## 四、草案自相矛盾或规格缺失

### 1. retry 的例子被它自己的规则否决(major)

§8.7 说 framework 默认只重试 `retryable=true` 的错误,紧接着的例子在 `on` 列表里
写了 `errors.target_unavailable`。

已复核:`retryable` 直接由 domain 的 `FailureResponse` 推导
(`umbra-tables.cpp:98-102`),只有 `CaptureStalled` 和 `StaleObservation` 是
`Retry`;`TargetUnavailable` 和 `Timeout` 都是 `Abort`
(`domain/error.cpp:80-88`)。所以例子被自己的规则拒绝。

更要命的是这打中 §8.6 推荐的主流程:`wait_for_page` 超时抛 `Timeout`,而
`Timeout` 是 Abort,所以**把 wait 包进 `ctx:retry` 永远不会重试** —— 而「等页面,
游戏慢了就整步重来」是日常脚本最常见的一种写法。

三选一,必须写死:①`on` 列表对它点名的 kind 覆盖 `retryable`;②`retryable` 是硬
过滤器,那就改例子,并接受 wait 超时只能靠 project 自己的控制流;③在
`domain/error.cpp` 里重新分类 `Timeout` —— 这会改变整个 engine 的
`failureResponse`,要单独论证。

### 2. 私有 native capability 的家没定死,而且否定名单漏了 `getfenv`(major)

§5.1 boot 步骤 3 说「install private native capability into framework-only
environment」(一个表),两段之后的散文说「framework 通过闭包 upvalue 持有它」。
两者威胁模型不同。如果 capability 是任何环境表里的一个键,它就能被恰恰产生了
已修复 `_G` 逃逸的那个机制读到:`luaL_sandboxthread`(`linit.cpp:94-107`)造的是
一个**新的可写表,其 metatable `__index` 链向父 globals**,project 环境这么造就
能按名字读到 framework 环境的每一个键。

§5.1 给 project 环境的否定名单只写了 `_G`、debug、coroutine、filesystem、network。
漏掉了 `installSandbox` 今天真的会移除的:`getfenv`、`setfenv`、`newproxy`、
`gcinfo`、`os.time/clock/date`、`math.random/randomseed`(`sandbox.cpp:140-160`)。

`getfenv` 是 `_G` 在双环境下的精确对应物:对一个 Lua 闭包,`luaB_getfenv` 返回
**那个闭包的** env 表(`lbaselib.cpp:126-135`)。所以对任意 framework export 调
`getfenv` 就把 framework 环境交给了 project;`setfenv` 则可以替换它。

另外还有一条 Luau 特有的:`luau_load` 接受 env 索引(`lvmload.cpp:787`),而
新线程的 `gt` 是从父线程复制的(`lstate.cpp:121`)。**环境隔离是按闭包的
(`luau_load` 的 env 参数),不是按线程 `gt` 的** —— 草案把它写成了后者。

**建议锁定**:私有 capability 只作为 framework 闭包的 upvalue 存在,永不作为任何
全局表或环境表的键;project 环境是 C++ 构造的显式白名单表,**没有** `__index` 链
指向 framework 环境或主 globals。

### 3. 冻结只覆盖 boot 期的表,而 project 能看到的每个对象都是 boot 之后造的(major)

`deepFreeze` 在安装时跑一次。但 `Context`、`Observation`、`PageObservation` 和
interrupt 的 `cycle` 对象都是运行期由 Luau 构造的。草案说「`Context` 的 methods 是
frozen closures」—— Luau 里闭包不可冻结,能冻的只有装它们的表和 metatable,而
草案没把这个责任派给任何人。

两条 Luau 具体事实决定它可不可利用:`table.freeze` 是**浅**的,且会拒绝
metatable 带 `__metatable` 的表;`table.clone` 同样拒绝受保护的 metatable,否则
返回一个**可变副本、带同一个 metatable** —— 所以任何缺 `__metatable` 的
「身份证明表」都可被 clone 伪造。今天 Tier B 错误的 metatable 正好设了
`__metatable`(`umbra-tables.cpp:1042-1049`),这才使伪造自动化错误不可能;草案
的整个错误模型依赖 C++ 和 framework 能分辨真 `TaskError` 和 project 自造的表,
但它没有重述这条规则。

### 4. §7.2 缺一个 Release 请求,而 §8.5/§8.6/§20 都依赖它(major)

第一版 request kinds 里没有 Release/CloseCycle,但 §8.6 的 `wait_for_page` 每轮
不匹配都调 `cycle:close()`,§8.5 和 §20 都断言 framework 主动释放、GC 只兜底。

今天的释放路径**完全**是 GC 驱动的:`destroyFrameBox`(`umbra-tables.cpp:161-172`)
从 userdata finaliser 调 `TaskContext::release`,`maxLiveObservations` 上限靠
capture verb 里强制一次完整回收才活得下去(`guardObservationBudget`,`:431-452`)。
而在草案模型下,framework 把每个 cycle 的 wrapper 存在一个**挂起的** coroutine 的
local 里,那是活的 GC 根 —— 强制回收能收回的比今天更少,默认上限 8
(`task-context.hpp:62-72`)会从「病态保留的兜底」变成「普通 framework 代码会撞到
的限制」。

### 5. 时间预算的执行点会在新模型下失效(minor,但会静默)

指令预算和 wall-clock deadline 都只在 interrupt 回调里判断,而 interrupt 只在
`luau_execute` 运行时触发(`cancellation.cpp:63-70`)。在新模型下绝大部分时间花在
挂起态(宿主 Poll、capture)里,interrupt 根本不触发 —— run 可以任意超出
`maxRuntime`,直到下一次 resume 才被发现。而 deadline 又是在 VM 构造时锚定的
(`engine.cpp:100-104`),没有办法扣除 pause 时间,和 §9.2 的推荐默认矛盾。

### 6. trace 既不能重放,也回答不了「为什么点错了」(major)

§12 把 `NativeCall` 列为权威宿主事件,但从未定义它的 payload;它取代的
`HostCall` 只记录 verb、outcome、errorKind(`trace.hpp:73-75`)。草案没有任何地方
要求记录请求点名了哪个页面/识别器、返回了哪个帧身份、匹配分数、点击点。

engine trace 里有这些证据(`engine/trace.hpp:47-63`),但**两条流没有 join key**:
task 流不带 frame 身份,engine 流不带 run/generation id,两个 sink 都不写时间戳。
验收标准 23.7 断言三层可以串成一个有序 run,草案里没有任何机制产生它。

另外 §12 让 C++ 加「host timestamp(若 schema 需要)」,而 §19.3 要求同 seed 同
DriverResult 序列下 task trace golden **逐字节相同** —— 这两节自相矛盾。

### 7. `.luau` 在本仓库根本没有构建管线(major)

§13 要求 framework 源码放 `modules/task/runtime/`,产物带版本和 SHA-256,release
build 只认二进制自带的 bundle。仓库没有任何机械可以做这件事:模块自动加载器只 glob
`source/*.cpp` 和 `source/*.hpp`(`build.cmake:57-62`),模块不允许有自己的
CMakeLists,整个构建只有一处 `configure_file`、零 `add_custom_command`。
`check_safety.py` 只认 C/C++ 扩展名,所以 framework Luau 会**完全无门**发布。
§19.1 的 framework 单测更没着落:`tests/CMakeLists.txt` 手写枚举源文件,今天唯一的
Luau 以 C++ raw string 形态住在 `test-task-binding.cpp` 里。

而且 §7.4 的循环把 `executeRequest(request, context)` 写死,§7.2/§19.4 又坚持只有
当前 generation 能 mint request —— 除非 request executor 是可注入端口,否则
「fake driver result sequence」不可能。

### 8. Luau 抛的错误怎么回到 C++,没有规定(major)

§7.3 只定义了 C++ → Luau 方向。反方向是空白:§8.1 说未捕获的结构化自动化错误变成
`Failed(kind)`,§8.6 让 framework 自己抛 `errors.timeout(page)`,但没说一个 Luau
错误值怎么变成带 `AutomationErrorKind` 的 `uf::Error`。这不是外观问题:
`entry/cli/run.cpp:49-88` 把 `AutomationErrorKind` 映射到冻结的 `ExitCode` 契约,
一个解不出来的 framework 错误会把每次 run 静默降级成 `ExitCode::Failure`,丢掉
Timeout 和 Cancelled。

同时 §10.1 的 `TaskError` 和 §8.7 的 `errors.*` 要求 kind 字符串在 Luau 里也存在,
于是 C++ 里已经重复两份的映射(`umbra-tables.cpp:75-96` 和 `trace.cpp:113-135`,
p0b §6 第 5 项已经把这个重复列为待决)变成**三份且跨语言**,switch 必须穷尽的
纪律够不到那里。

### 9. 其它较小项

- **`task-trace/vNext` 不是版本号**。§12 把 `HostCall` 改名 `NativeCall`、加
  `GenerationBuilt`、加十种 framework 语义事件、给 TaskStarted 加六个字段,但没说
  schema 字符串变成什么、v1 是迁移还是废弃、落地的 golden 怎么办。
- **framework 内部不变量失败无法执行**。§10.2 第 5 条要求它「必须终止 generation」,
  但纯 Luau 抛错会被 project 的 `pcall` 吞掉。今天的等价保证靠 C++ 先 latch
  (`markFatal`)+ 每个 verb 经 `guardFatal` 重抛。需要一个终止性 driver request。
- **§21 说「C++ validation state machine」,§12 没写任何规则**。没有规则集时 C++
  实际只校验了 kind 名字。
- **project 回调会重入 framework 状态**。`ctx:step(name, fn)`、`ctx:retry(policy, fn)`、
  `handle(ctx, cycle)` 里的 project 代码都持有 `ctx`,可以回调任何 Context 方法;
  一个吞掉重入错误的 project `pcall` 会让 framework 的标志位卡住,静默关掉整轮
  interrupt。
- **§9 的 pause 承诺了三件在实现之前不可测的事**。按 `task-context.cpp:34-50` 的
  标准,这些今天不该以代码存在。真正的约束只有一句:framework 的观察周期边界是
  未来的 pause 点。

## 五、建议的裁决清单

必须由你拍板的:

1. **根命名**:保留 `umbra` 单根(建议),还是正式撤销 2026-07-27 的裁决。撤销的话
   草案要补上迁移项:CONTEXT.md、annotation-design §4、校验器、7 条 `_G` 绕过回归、
   所有错误字符串 —— Phase A–F 里一个都没有。
2. **项目磁盘布局**:草案 §15/§8.9 的 `.json` 布局撤回、照抄 S0 的 TOML 契约,还是
   你明确批准一次格式迁移(那要连带重新生成所有已发布产物和 workbench 发布路径)。
3. **阶段与 P0/P1 的对应**:草案完全没写。建议 A(收窄)+ D + F 是 P0-B/P0-C;
   interrupt 的 `max_hits`/不重入和整个 Phase E 是 P1,P0 只落 framework 侧的接缝。
4. **Phase C 做不做**。建议不做,写死两条重开触发条件。
5. **retry 语义**:`on` 覆盖 `retryable`,还是 `retryable` 硬过滤,还是重新分类
   `Timeout`。
6. **`capture()` 的 deadline/stop_token 排进哪个阶段**(P0-B 已承诺,一直没排)。
7. **`ctx:logical_time()` 的改名**,以及是否确认「每次读取 +1」的逻辑序数语义。
8. **schema 版本**:`task-trace/v2`?`HostCall` 值不值得改名?§12 的 host timestamp
   和 §19.3 的逐字节 golden 怎么调和。

若草案要成为权威,还需要:新增 ADR(framework 属于产品、project 环境隔离)、修订
ADR 0001(userdata 框架现在跨两个 Luau 信任层)、并在同一提交里修正 CONTEXT.md、
annotation-design §4、p0b-script-layer 的裁决 1 与 §6、TODO.md、INDEX.md。
