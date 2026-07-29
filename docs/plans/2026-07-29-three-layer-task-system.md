# 三层 Task System — 目标架构与实施计划

> 状态:**已定方向,2026-07-29 开发者确认**。允许大范围重构与删除。
>
> 权威性:本文取代
> [`2026-07-28-luau-first-task-system-design-draft.md`](2026-07-28-luau-first-task-system-design-draft.md)
> 与 [`2026-07-27-p0b-script-layer.md`](2026-07-27-p0b-script-layer.md) 的脚本层
> 裁决部分。产品方向仍看
> [roadmap](2026-07-21-product-form-and-roadmap.md);S0 标注契约仍看
> [annotation-design](2026-07-22-annotation-design.md),但其 §4 的脚本拼写按本文
> 修正。评审依据见
> [`docs/reviews/2026-07-28-luau-first-draft-review.md`](../reviews/2026-07-28-luau-first-draft-review.md)。
>
> 口径:**「完美」= 恰好贴合目标,没有历史包袱,也没有为想象需求预留的机械。**
> 本项目拒绝投机性泛化的规则不因为放开重构而失效。

## 一、一条规则

> **C++ 拥有所有「保证」。Luau 拥有所有「策略」。**
> 保证 = 即使 Luau 层有 bug 也必须成立的东西。

所有边界问题按这条机械裁决,不再一事一议。

| 问题 | 判定 |
|---|---|
| 沙箱、配额、指令预算、运行上限 | 保证 → C++ |
| 取消的终局性 | 保证 → C++ |
| 帧/证据的所有权与寿命 | 保证 → C++ |
| 动作授权四要件、目标兼容性、租约 | 保证 → C++ |
| 严格后台投递、投递前 target 复验 | 保证 → C++ |
| trace 的写入、run 身份、hash 与 seed 盖章 | 保证 → C++ |
| 等多久、轮询多密、什么时候重试 | 策略 → Luau |
| 弹窗注册表、first-match、max_hits、不重入 | 策略 → Luau |
| step 嵌套与命名、subtask 组合 | 策略 → Luau |
| 一个游戏的页面流转与成功条件 | 策略 → Luau(project) |

## 二、目标与非目标

### 目标

1. 新增或调整一个游戏流程只改 project 的 `.luau` 与 assets,不改也不重编 C++。
2. P0 的一次一 run 升级到 P2 常驻托盘 App 时,project task API 与宿主 API 面都不换。
3. 每一次动作都能从 trace 里解释:看到了什么帧、识别到什么、为什么被授权、投在哪。
4. 同 seed 同观察序列可复现。
5. 硬取消在预算内停机,且脚本无法恢复执行。

### 非目标

- 不把识别、坐标变换、授权或投递改写成 Luau。
- 不做多 task 并发、运行中状态迁移、跨进程 worker(roadmap 的重评估触发条件未满足)。
- 不做纯声明式 workflow DSL 取代 Luau。
- 不做 effect 的 coroutine yield/request driver(理由见 §11)。
- 不保证硬取消时 Luau 侧清理一定运行;资源清理由 C++ 拥有。

## 三、三层

```text
C++ 保证层
    modules/script   Luau 底座:VM、沙箱、配额、指令与时间预算、中断取消
    modules/engine   原子能力:observe / resolvePage / findAction / act,以及授权
    modules/task     TaskHost、票据账本、私有能力面、trace sink、generation

Trusted Luau Framework            modules/task/runtime/*.luau,随二进制发布
    task 生命周期、step、观察周期编排、wait、retry、interrupt、错误翻译

Project Task                      <project>/tasks/*.luau
    一个游戏的页面流转、条件分支、操作顺序、弹窗处理
```

依赖方向:

```text
entry -> task -> script
              -> engine -> annotation
                        -> controller ports at composition

engine -X-> task        script -X-> task        script -X-> engine
```

### 为什么 framework 必须是 Luau

不是为了热加载。是因为 framework 的 API 形状要求它:

```lua
ctx:step("claim", function() ... end)   -- project 函数
ctx:retry(policy, function() ... end)   -- project 函数
handle = function(ctx, cycle) ... end   -- project 函数
```

framework 若用 C++ 写,上面每一个都是「C++ 回调进 Lua」:project 代码执行期间 C 栈
是深的;取消落在 project 回调里就撞上「break 跨 C 帧退化成可捕获普通错误」的窄例外,
而且从罕见变成常态;错误要反复穿越 C 帧。Luau 调 Luau 一个都没有。

## 四、核心协议:观察周期

这是相对现状最大的改动。今天 8 MiB 的帧寿命绑在 **Lua 的 GC 析构器**上,靠「撞上限
就强制回收一次再重试」兜底。宿主内存的释放时机由宿主不控制的回收器决定,设计上不对。

改为**显式周期 + 票据**:

```text
cycle_open(deadline)              -> ticket        一次 capture
cycle_close(ticket)               -> ()            确定性释放,幂等
cycle_page(ticket)                -> page | nil    同帧页面解析
cycle_find(ticket, recognizer)    -> hit | nil     同帧查找
cycle_click(ticket, hit)          -> receipt       消费周期
```

Lua 侧句柄只是一张**票**。C++ 持有票据账本,每次使用拿票对账。周期一关,票即死。
**GC 完全不参与。**

**硬规则:每个 generation 同时只允许存在一个打开的周期。** 账本至多一条。
只要账本能装两条,「page 与 hit 同源」就仍然只是一次**检查**,而 C++ 不能依赖
framework 的自觉;收窄到一条,第二帧根本不存在,跨帧混用才真的不可能发生。这也正是
Model B 的语义,而且没有任何已知场景需要同时持有两个周期:wait 循环每轮开一个关一个,
interrupt handler 拿到的是**当前**周期,handler 消费掉观察后由外层循环重开。

连锁简化三处:

1. **跨帧校验变成结构性的。** 今天每个句柄要记「源自哪次观察」,`click` 里比对两个
   seq。有了周期,那套 seq 机械整体删除;命中句柄只带 ordinal,与当前周期不符即
   StaleObservation——在「至多一个周期」下这就是陈旧性检查本身。
2. **`cycle_click` 不收 page 参数。** C++ 要求该 ticket 已成功解析出页面,并用它作为
   授权证据。脚本无法提供不匹配的页面证据——四要件从「校验」变成「构造上不可能违反」。
   页面身份判断(`page:is(uf.pages.home)`)是脚本分支,与授权证据是两件事,互不影响。
3. **`maxLiveObservations` 这个旋钮整个删掉。** 上限恒为 1,「护宿主内存」的保证由
   结构满足,不再需要一个可配置的数,也不再需要 `guardObservationBudget` 那套
   「先强制 GC 再拒绝」。

> **修订 2026-07-29(orchestrator)**:本节初稿写的是「票据账本」,隐含账本可装多条。
> 那样「跨帧结构性不可能」这句话不成立。上面的硬规则是收紧后的版本。

**嵌套 wait 的行为**:点击已消费周期,所以「等首页 → 点按钮 → 等下一页」这种顺序
嵌套天然可行,而且是最常见的写法。真正的嵌套(外层周期仍开着就再等一个页面)会撞上
「已有打开周期」,**它该失败**,但必须由 framework 在 Luau 侧抛可读的话
(「不能在一个观察周期打开时再开一个;先消费或关闭外层周期」);C++ 的
InternalInvariant 只作为 framework 有 bug 时的兜底,正常路径下作者永远看不到。
分层原则:framework 负责好消息,C++ 负责保证。

生命周期责任:framework 负责关(纯 Luau,`pcall` 清理路径可靠);generation 拆除时
C++ 兜底释放账本里的一切。

**红线**:没有任何 native 原语回调进 Lua。周期不是跨越 Luau 回调的 C++ RAII 对象,
而是一对 open/close 原语。这条红线同时保证了 §11 的可逆性。

## 五、私有能力面(12 个原语)

只有 framework 能拿到,以闭包 upvalue 形式持有,**永不作为任何 project 脚本能命名的
表的键**。

> **收紧 2026-07-29(orchestrator)**:初稿写的是「永不作为任何表的键」,太绝对了。
> 这 12 个原语**正是**那张私有表的键——宿主构造它、把它作为 chunk 实参交给 framework
> bundle(`local native = ...`),§6 的 `wait_for_page` 草图里那一串 `native.cycle_open`
> 就是在读这些键。承重的不变量是**那张表不在任何环境里有名字**:宿主交出去之后就丢掉
> 自己的引用,于是唯一能拿到它的办法是当初被交到手里的那个闭包。按字面读初稿会得出
> 「代码违反了自己的设计」,所以在此改成实际成立的那一句。

```text
-- 观察周期
cycle_open()                      -> ticket | raise        捕获期限由宿主铸,不经脚本
cycle_close(ticket)               -> ()
cycle_page(ticket)                -> page | nil          nil = Unknown/Ambiguous(已入 trace)
cycle_find(ticket, recognizer)    -> hit | nil           nil = Tier A 未命中
cycle_click(ticket, hit)          -> receipt | raise

-- 时间与等待:全部有界、感知 stop token
deadline(ms)                      -> deadline
wait(deadline, interval_ms)       -> bool                false = deadline 到期
settle(ms)                        -> ()                  声明式有界停顿,入 trace

-- 控制与可观测
raise(kind, message)              -> never               抛宿主 mint 的错误 userdata
emit(event)                       -> ()                  语义 trace 事件,C++ 校验
terminal()                        -> bool                generation 是否已终局
random(...)                       -> number
```

> **补记 2026-07-29(阶段 2c `c37ee5b`)**:这张表上还有**一个不是原语的字段**:
> `error_tag`,值就是 Tier B 载体 metatable 上那个受保护的标签串 `"uf.error"`
> (`uf-tables.cpp` 的 `k_errorTagField` / `k_errorType`)。它**不是能力**——同一个串
> 任何脚本对捕获到的错误调 `getmetatable` 都能读到。它住在这里只因为这张表是
> framework 唯一拿得到、而任何 project 都无法命名的东西,于是 `ctx:try` 能拿到那个标签
> 而不必在 `.luau` 里把串再写一遍(写第二遍就有两份要保持相等的真相,而没有任何东西
> 检查它们相等)。所以本节标题的「12 个原语」描述的是能力,不是表的全部键。
>
> 同一批还要说清今天树上的实际形状:表上是**八个**原语——五个周期原语、
> `wait_for_page`、`now`、`random`——外加 `error_tag`。`deadline` / `wait` / `settle` /
> `raise` / `emit` / `terminal` 尚未存在(阶段 3),而 `wait_for_page` 与 `now` 是要在
> 阶段 3 分别搬进 framework Luau 与删除的过渡项。上面那 12 个是**目标**表面。
> (这段形状快照停在阶段 2c,已被下一条取代。)

> **形状快照 2026-07-29(阶段 3c `8b16f2d`)**:私有表上是**十个**原语加 `error_tag`
> 那一个数据字段,共十一个键(`uf-tables.cpp` 的 `buildPrivateSurface`):
>
> ```text
> cycle_open  cycle_close  cycle_page  cycle_find  cycle_click
> deadline    wait         settle      raise       random
> error_tag                                            -- 非能力,Tier B 标签串
> ```
>
> 与上面那张目标表的差是两项,方向相反:
>
> - **`wait_for_page` 不再是原语**。阶段 3b(`d1a0685`)把它整个搬进
>   `modules/task/runtime/ctx.luau`,成为 `cycle_open` / `cycle_page` / `wait` 之上
>   的一段 Luau 循环——这正是 §1 那条规则要的结果,不是过渡。C++ 侧的
>   `wait_for_page` 原语与 `TaskContext::waitForPage` 都已删除。
> - **`emit` 与 `terminal` 仍不存在**,排在阶段 3d(语义事件 + 校验状态机)。今天
>   framework 的 step / retry / interrupt 都不写语义事件,唯一落在 trace 上的脚本侧
>   证据是 `task.native_call`。
>
> `now` 也不在表上:阶段 3a(`f146329`)随 `deadline` / `wait` / `settle` 一起删掉了
> 它与 `DeterministicClock` 的全部绑定(§10)。

四条不变量(它们同时是 §11 可逆性的条件):

1. 纯粹:只有效果,不回调进 Lua。
2. 入参只有宿主 mint 的句柄和标量。
3. 返回只有宿主 mint 的句柄、标量、或错误 userdata。
4. 每个原语有界且感知 stop token。

`wait` 由现有 `engine::pollSleep`(`session.cpp:56-79`)的切片实现搬来。
`cycle_open` 的 deadline 要求 `IFrameSource::capture()` 补 deadline/stop_token
——这是 P0-B 已承诺、至今未做的项,在此正式排进阶段 3。

> **落地更正 2026-07-29(阶段 3a `f146329`)——这两句都要改写**:
>
> - `pollSleep` **没有搬进 task**,它升进了 `modules/core/source/core/time/poll-sleep.hpp`。
>   一个按 `k_maxPollSleepSlice = 100ms` 切片、感知 stop token 的睡眠是通用时间设施,
>   不是任何一层的 policy;它今天的生产调用方就是 task 的 `wait` 与 `settle`
>   (`task-context.cpp`),engine 一个都没有。放 core 是为了下一个要「对着 deadline 停一下」
>   的模块不必长出第二份切片逻辑。§16 那条「迁移到 task 的 `wait` 原语」按此更正。
> - `IFrameSource::capture()` **已补上**,签名是 `capture(CaptureBudget const&)`,
>   `CaptureBudget` 是 `{ deadline, cancellation }`(`engine/ports.hpp`)。两个成员都承重,
>   适配器必须都兑现。
> - 但**期限不经过脚本**。`cycle_open` 至今不收参数:deadline 由
>   `EngineSession::observe()` 从 `EngineSessionConfig::captureTimeout`
>   (默认 `k_defaultCaptureTimeout = 2s`)当场铸出,溢出单调时钟就 fail closed。
>   这比原设计更严——「一次捕获能阻塞多久」是宿主的资源边界,不是脚本的 policy,
>   所以脚本连一个可以放大的旋钮都没有。脚本能拿到的期限只有 `deadline(ms)` 那一个,
>   它约束的是**等待循环**,不是单次捕获。

## 六、脚本表面

根为 `uf`,单一全局根。校验器锚定 `AstExprGlobal`,所以根必须是全局。

```lua
uf.pages.<name>          -- 冻结句柄
uf.recognizers.<name>    -- 冻结句柄
uf.task                  -- define / interrupt / import(P1) / backoff
uf.errors.<kind>         -- 错误 kind 常量,由 AutomationErrorKind 生成
```

> **`uf.task` 今天不存在,而且结构上还做不到 2026-07-29(阶段 3b `d1a0685`)**:
> 落地的拼写是 project 全局 **`task`**——`task.define{...}` / `task.interrupt{...}`
> (`modules/task/runtime/task.luau`),由 `ctx` 用的同一个接缝
> (`framework-bundle.cpp` 的 `frameworkProjectGlobals()`)发布。
>
> 原因值得记下来,免得以后当成疏忽去「顺手修掉」:`uf` 是宿主在 C++ 里建的,
> 建完就 `deepFreeze` 并设为全局(`uf-tables.cpp` 的 `buildUfData`),而**这一步跑在
> framework bundle 加载之前**。framework 的模块导出是 Luau 值,晚于冻结才存在,所以它
> 没有任何办法成为 `uf` 的成员。要让 `uf.task` 成真,得给 `modules/script` 加**第四道
> 接缝**——一条「framework 模块的导出回填进宿主表、然后才冻结」的通道——那是一次真
> 改动,不是改个名。
>
> 在那之前两个拼写的差别只有一个词:`task.define` 与 `uf.task.define` 的调用点形状
> 完全一样,`task.luau` 里没有任何东西依赖它挂在哪。

project 环境**没有**裸动词。`ctx` 作为参数传入 `run`。

> **过渡状态 2026-07-29(阶段 2b-2,`e89bc53`)**:`run(ctx)` 是**目标**形状,今天还
> 没有 task runner 来传这个参数。所以 2b-2 先把 `ctx` 当作 **project 全局**发布——
> `modules/task/source/task/framework-bundle.cpp` 的 `frameworkProjectGlobals()` 这个
> 接缝把 framework 模块 `ctx` 的冻结 exports 按同名拷进 project 环境。阶段 3 建起
> `uf.task.define{ run = function(ctx) … }` 之后,这个全局就退掉。**不要把它读成最终
> 形态**:它存在只是为了在 `run(ctx)` 出现之前顶替被删掉的裸动词。

### Task

```lua
return uf.task.define {
    interrupts = { popups.network_error },

    run = function(ctx)
        ctx:step("daily", function()
            ctx:wait_for_page(uf.pages.home, { timeout_ms = 60000 }, function(home)
                local hit = home:find(uf.recognizers.daily_button)
                if hit then
                    home:click(hit)
                end
            end)
        end)
    end,
}
```

task 名来自 `(project, taskName)` 地址,不在源码里重复声明。descriptor 冻结,只能由
`uf.task.define` 构造。

**任务寻址**(承接已删除的 ADR 0002,论证保留于此):任务永远住在它的项目里
(`<project>/tasks/`),按 `(project, taskName)` 寻址,**CLI 永不执行游离路径的脚本**。
宿主在加载时算脚本内容 hash,连同 Luau compiler 版本写进 `run.started`。

理由:最终形态(P2 托盘 App)是打开一个项目、列出它的任务;P1 跨文件复用需要按内容
hash 寻址的源。两者都建在项目所有权上,游离路径的 CLI 会逼着以后迁移所有工作流。
**承重的是寻址模型**——P0 用目录约定(任务名 = 文件名),跨文件复用在 §17 阶段 5
加法式扩展,而不是今天盲设计 manifest 格式。

### Interrupt

```lua
return uf.task.interrupt {
    id       = "network_error",
    when     = uf.pages.network_error,
    max_hits = 3,

    handle = function(ctx, cycle)
        local close = cycle:find(uf.recognizers.close_dialog)
        if close then
            cycle:click(close)
        end
    end,
}
```

### Context

```lua
ctx:step(name, fn)
ctx:cycle(fn)                            -- fn(cycle);退出必关
ctx:wait_for_page(page, options, fn)     -- fn(cycle),该 cycle 已解析出目标页
ctx:retry(policy, fn)
ctx:try(fn)                              -- 纯 Luau pcall + 错误解码
ctx:settle(ms)
ctx:random(...)
ctx:call(subtask, ...)                   -- P1
```

**周期一律回调形态**,不返回打开的周期。这样「必关」是结构保证,「一次只持有一个
周期」也是结构保证。

> **补记 2026-07-29(阶段 3b `d1a0685`)——落地的 `ctx` 比上表宽**,多出来的都在
> `modules/task/runtime/ctx.luau` 里:
>
> - `ctx:step_path()` — 当前打开的 step 名,由外向内的一份拷贝。它存在是因为「step
>   严格良嵌套」这条性质得可观察才值钱;§12 那台校验状态机落地前,它是唯一能从外面
>   检查这件事的东西。
> - `ctx:deadline(ms)` / `ctx:wait(deadline, interval_ms)` — 原语的直接转发。
> - `ctx:cycle_open` / `cycle_close` / `cycle_page` / `cycle_find` / `cycle_click` —
>   同上,五个周期原语的直接转发。
>
> 后面这些**不是给日常脚本用的**:它们留在表面上是为了宿主的原语契约能被直接驱动
> (`tests/task/` 大半靠它们)。混着用是合法的但没有意义——用 `ctx:cycle_open` 开的
> 周期在 framework 自己的记账之外,随后的 `ctx:cycle` 会被宿主的 `InternalInvariant`
> 拒绝,而不是被 framework 那句写给作者看的话拒绝。等到 task 变成宿主执行的
> descriptor 而不是宿主执行的 chunk,它们和 `ctx` 这个 project 全局一起退掉。
>
> `ctx:call` 仍是 P1,未落地。

### `wait_for_page` 的 framework 实现

```lua
function Context:wait_for_page(page, options, fn)
    local deadline = native.deadline(options and options.timeout_ms)
    while true do
        local ticket = native.cycle_open(deadline)
        local consumed = self:_run_interrupts(ticket)
        if not consumed then
            local resolved = native.cycle_page(ticket)
            if resolved and resolved:is(page) then
                return self:_with_cycle(ticket, fn)     -- fn 内使用,退出必关
            end
        end
        native.cycle_close(ticket)
        if not native.wait(deadline, options and options.poll_ms) then
            native.raise("timeout", "page did not resolve")
        end
    end
end
```

页面选择、轮询节奏、interrupt 时机、超时含义都在这里。真正的睡眠、stop token 和
steady clock 在 C++。project 看不到墙钟。

> **落地形状 2026-07-29(阶段 3b `d1a0685`)**:上面的草图是对的方向,细节有三处不同,
> 都在 `modules/task/runtime/ctx.luau` 的 `observeCycle` / `ctx:wait_for_page` 里。
>
> 1. **先解析页面,再喂给 interrupt 注册表**,而不是草图那样先跑 interrupt。原因是
>    interrupt 按**页面**寻址(`when = uf.pages.<name>`,一个弹窗对识别运行时来说就是
>    一个页面),所以「这一帧是哪一页」必须先有答案才谈得上匹配。页面解析在周期视图上
>    memoize:它是一次带 `engine.page_resolved` 的引擎调用,一帧解析两遍会在线上留两行。
>    interrupt 跑过之后**立刻重新观察**,不拿目标页去试一张刚被点击作废的证据。
> 2. **`cycle_open` 不收 deadline**(见 §5 的落地更正)。
> 3. **两个默认值住在 framework**,不是「nil 就透传给宿主」:
>    `k_defaultTimeoutMillis = 600000`、`k_defaultPollMillis = 500`,选项名是
>    `timeout_ms` 与 `poll_ms`。宿主背后已经没有任何回退值——`TaskRunConfig` 与
>    `TaskContextConfig` 里的等待预算字段都随 3b 删了,理由写在 `task-host.hpp`:
>    「framework 读不到的默认值,就是 framework 拥有不了的 policy」。
>
> 另外草图没有、而落地必须有的是**收尾**:每一轮的 observe 与 close 各自过一次
> `pcall`,块自己的失败优先于关闭的失败;`cycle_close` 前先丢掉 framework 的记账,
> 否则一次抛错会让 framework 永远认为有个周期开着。

## 七、环境与信任分层

**环境隔离按闭包,不按线程。** `luau_load` 收 env 索引(`lvmload.cpp:787`);新线程的
`gt` 是从父线程复制的(`lstate.cpp:121`),所以 `luaL_sandboxthread` 那套代理形状
**不能**用来做 project 环境——那正是 `_G` 逃逸的形状。

boot 顺序:

```text
建配额 VM
-> 开放准入的基础库
-> nil 掉危险全局(含 os/math 上的残余时钟与 RNG 字段)
-> C++ 构造 framework env 表(冻结的 metatable 把 __index 链到主 globals)
-> C++ 构造私有能力面,留在栈上
-> luau_load(framework bundle, env = framework env, arg = 私有能力面),执行,
   冻结每个模块的 exports 并按模块名绑进 framework env;随后丢掉宿主自己那份引用
-> 装宿主数据表(uf.recognizers / uf.pages / uf.errors)为普通全局并递归冻结
-> luaL_sandbox
-> C++ 构造 project env 原型:显式白名单,【没有】metatable,因而没有 __index 链
   指向 framework env 或主 globals;白名单含 projectGlobals(`uf`)与
   frameworkProjectGlobals(framework 模块导出,今天是 `ctx`)
-> 每次 run 从原型浅拷一份可写的 project env
-> luau_load(project module, env = 该 project env)
-> 把 descriptor 交给 framework runner
```

> **修订 2026-07-29(orchestrator,阶段 2b-1/2b-2 落地时)**:本节初稿把「nil 掉危险
> 全局」排在 framework 加载**之后**,并且没有「构造私有能力面」这一步。两处都改了,
> 上面是代码实际实现的顺序(`modules/script/source/script/ffi/sandbox.{hpp,cpp}`)。
>
> 顺序必须换,理由是一个捕获窗口:framework env 的 `__index` 链到主 globals,所以
> 先加载 framework 就允许某个模块在**加载期**写下 `local getfenv = getfenv`,把那个
> 引用握住整个 generation,nil 掉之后依然有效。只要 framework 什么都不导出给 project,
> 这个窗口是无害的;而它导出 `ctx` 的那一刻起就不是了。bundle 里没有任何东西正当地需要
> 这些名字——时间与随机都走私有能力面——所以关掉窗口不花任何代价,却把「framework 代码
> 不会去拿它们」这句承诺换成了一条结构性事实。`tests/script/test-environments.cpp` 有一条
> 「A framework module cannot capture a dangerous global at load time」,**从 framework
> 环境内部**断言模块运行时这些名字已经不在了。
>
> 私有能力面那一步不是遗漏了细节,而是缺了一个真实步骤:它必须在 framework 加载**之前**
> 构造完成,因为它就是每个 framework 模块的 chunk 实参(`local native = ...`)。

project 环境的否定名单(**完整**,不是示例):

```text
_G  getfenv  setfenv  newproxy  gcinfo  coroutine  debug
os.time  os.clock  os.date  math.random  math.randomseed
```

`getfenv` 是 `_G` 在双环境下的精确对应物:对 Lua 闭包,`luaB_getfenv` 返回**那个闭包
的** env 表(`lbaselib.cpp:126-135`),所以对任意 framework export 调它就拿到 framework
环境。因为 Lua 侧的 `setfenv` 被移除,C++ 侧需要一个 `lua_setfenv` 来装环境。

**冻结规则**(现状只覆盖 boot 期,新设计必须覆盖运行期):

- 每个 project 可见的 framework 对象在**构造时**冻结,顺序是先 metatable 后表。
- 每个这样的 metatable 都带 `__metatable` 字段。理由:`table.clone` 拒绝受保护的
  metatable,否则返回**可变副本、带同一个 metatable**——缺 `__metatable` 的身份证明表
  可被 clone 伪造。
- 身份判断一律比较 userdata tag 或 registry 里的 metatable 身份,不做鸭子类型。
- `__index` 一律是**表**,不是函数(见 §11 的可 yield 矩阵,这条同时保证未来转 yield
  不会踩坑)。

> **补记 2026-07-29(阶段 2c `c37ee5b`)——「构造时」也包括运行中构造的对象**:
> 句柄的 metatable 在 boot 期建一次并存进 registry,而 **Tier B 错误载体的 metatable
> 是每抛一次错建一份**。原因是每个错误的 `kind` / `message` / `retryable` 不同,而
> `__index` 又被上面那条规则限定为表,于是 per-error 数据没有别处可住;代价是每个抛出
> 的错误多两张小表,而错误本来就是例外路径。承重的是它走的是同一道门:
> `script::deepFreezeMetatable` 在挂到载体之前检查并冻结它,所以「构造时冻结 + 两条形状
> 规则」对**运行中新铸的对象**同样成立,而不只对 boot 期建的那六张。冻结失败时宿主抛
> 一个纯字符串,绝不把半冻结的载体交给脚本。
>
> **同时要说清一件本节读起来像不存在的事**:有一张宿主相邻的 metatable 是 project
> 脚本**读得到**的。`luaL_sandbox`(`linit.cpp:65-91`)把字符串 metatable 设为只读,
> 却**没有**给它加 `__metatable`,所以 `getmetatable("")` 在 project 环境里返回真表。
> 这是无害的:它已冻结、改不动、`table.clone` 出来的副本也挂不到任何值上,而它的
> `__index` 就是白名单里那张 `string` 表——脚本从它那里到不了任何新东西。写下来是因为
> 「没有任何 metatable 可读」这句话若被当成不变量,以后会有人按它推理。
> 对抗断言在 `tests/task/test-adversarial-surface.cpp` 的
> 「The standard library a project shares with the framework is immutable」。

`HostTableInstaller` 必须改成返回 `Status`。今天是 `void`(`engine.hpp:26`),
坏 bundle 只能 longjmp 或被静默忽略,generation 无法因此失败。

## 八、取消

强度不低于今天,机制不变,只是收口:

- 中断回调照旧在预算/deadline/stop token 命中时 `lua_break`,负责停住不调用任何原语的
  纯 Luau 死循环。
- `lua_break` 在 `nCcalls > baseCcalls` 时抛的是**普通可捕获错误**
  (`ldo.cpp:766-772`)。所以 `InterruptState::broken` 照旧是判定终局的真相。
- **每个 framework→native 调用先过同一个 C++ 守卫入口检查终局闩。** 即使 project 的
  `pcall` 捕获了那个普通错误、脚本继续跑,下一次原语调用即拒绝。
- `ctx:try` 是**纯 Luau** 的 `pcall`,错误不是可解码的错误 userdata 就重抛。
  调用路径上不存在做 `lua_pcall` 的 C 闭包。

> **补记 2026-07-29(阶段 2e `2ebcf0c`)——闩不是唯一那道拦截,而且不是更强的那道**:
> 上面那条写的是「下一次原语调用即拒绝」,而对**取消**这条路,Luau 的源码给的保证更强:
> `InterruptState::broken` 一旦置上,**下一次原语调用根本发生不了**。
> 三个触发条件(stop token、指令预算、deadline)全部单调:token 请求过就不会撤回,
> `ticks` 只增,过了的 deadline 不会退回去。所以中断回调下一次进来仍然命中,仍然
> `lua_break`。而 Luau 的中断点是 call、return 与回边三类指令
> (`lvmexecute.cpp` 的 `VM_INTERRUPT()`,`LOP_CALL` 那一处在 `VM_CASE(LOP_CALL)` 的
> **第一行**,指令都还没取),命中即 `savedpc--` 后 `goto exit`——那次调用没有发生过。
> 于是脚本连「再进一个宿主 C 函数」都做不到,更不用说在里面铸一个新载体;手上已经握着
> 的载体也只能靠 `error()` 抛,而 `error()` 本身就是一次 call。闩是保险带,中断是那道
> 结构性的拦阻,**而且它先到**。取消保证不建立在闩上,这一点值得写进文档而不是靠记忆。
>
> 反过来也要说清,免得读成「闩是多余的」:闩覆盖的是**中断根本没有触发**的那一类终局。
> 一次被取消的 capture 会让原语走 Tier C 并 `markFatal`,而此时可以完全没有 armed 的
> stop token——VM 中断从头到尾没响过,能拒绝下一次原语的**只有**闩。
> `tests/task/test-adversarial-surface.cpp` 的「The terminal latch refuses a primitive
> called from any context」正是按这个条件构造的。两道拦截各管一类终局来源,不是同一件事
> 的两层。

> **语义变更 2026-07-29(阶段 3b `d1a0685`)——「同时超时又被取消」的那一轮,现在报
> 取消,不再报超时**。这是一次真的行为变化,没有任何测试断言过旧顺序,所以记在这里而
> 不是留给以后重新发现。
>
> 旧的 engine 侧 `waitForPage` 每轮的检查顺序是**先 deadline、后 stop token**
> (`8b16f2d^` 的 `session.cpp:676` 与 `:684`):一轮里两件事都成立时,返回的是
> `Timeout`。搬进 Luau 之后,轮询这一步是 `wait` 原语,而它在睡眠的**两侧**各查一次
> 取消(`uf-tables.cpp` 的 `waitFn`:`guardCancelled` → `waitUntil` → `guardCancelled`),
> 取消命中就直接走 Tier C 的 `markFatal` + 哨兵,根本没机会把「预算用完了」这个布尔值
> 交回 Luau。所以同一轮里取消赢。
>
> 方向是对的:§1 把取消的终局性算作 C++ 的保证,而超时是 framework 的 policy——一次
> 被取消的 run 报成超时,会让「取消一定终局」这句话在报告层面留一个例外。真正的看点是
> 取消**根本不必**赢在这一轮:stop token 同时也是 VM 中断的触发条件,`broken` 一置上
> 下一次原语调用就发生不了(见上一条补记)。`wait` 两侧的检查覆盖的是中断没赶上的那个
> 窗口——睡眠发生在一个 C 帧里,中间没有任何中断点。

删掉的是 `uf:try` 的 C 绑定,不是 `markFatal`/`guardFatal` 的语义。

verto 第 6 条(人为阻塞每个长耗时 binding,验证总退出仍在预算内)进 CI:12 个原语
逐个注入阻塞。这条 roadmap 一票否决至今没跑过。

## 九、错误

**载体是宿主 mint 的 userdata,不是表。** project 伪造不了、改不了;C++ 按 tag 解码而
不是鸭子类型。`table.clone` 伪造问题被结构性解决,不依赖 `__metatable` 兜底。

> **精确化 2026-07-29(阶段 2c `c37ee5b` 落地后)**:「不依赖 `__metatable` 兜底」说的是
> **身份不再落在它身上**,不是它没了。载体的 per-raise metatable 上 `__metatable` 仍在,
> 而且是两条独立要求同时要的:§7 的 deepFreeze 形状规则**强制**每张 metatable 都带它
> (缺了 `deepFreezeMetatable` 直接失败),而它的值 `"uf.error"` 仍然是脚本可见的那个
> 标签——`ctx:try` 现在的判定是 `type(err) == "userdata" and getmetatable(err) == errorTag`,
> 两半都在做事。变的是**承重点**:userdata tag 是 VM 自己存在对象上的属性,
> `table.clone` 只吃表、`setmetatable` 只吃表、`newproxy` 两个环境都没有,所以 project
> 拿不出任何带 tag 的值;C++ 侧(`tierBErrorKind`)**只看 tag**,不看标签、不看字段。
> 标签今天回答的是「宿主的这些 userdata 里,哪一个是错误」——把错误和 page / hit 之类
> 句柄分开——而不是「这是不是宿主造的」。

**kind 表只有一份真相。** `AutomationErrorKind`(C++)是真相:

- C++ 侧的 kind→wire 名映射**合并为一个函数,住在 `modules/domain`**。domain 拥有
  这个 enum,而 task 与 trace 都依赖它,所以那是唯一一个两边都不必互相依赖的家。
  合并前有两份(`uf-tables.cpp`——当时还叫 `umbra-tables.cpp`——的 `snakeName` 与
  `trace/event.cpp` 的
  `errorKindWireName`),注释要求二者恒等但无共享真相、无一致性测试(p0b §6 第 5 项)。
- `uf.errors` 由**宿主在装能力面时用 C++ 直接构建**,与 `uf.pages` / `uf.recognizers`
  并列,递归只读。
- 两条测试:表覆盖 enum 的每个取值(不多不少);同一 kind 在 trace 里的拼写与脚本
  可见的拼写是同一个字符串。

> **修订 2026-07-29(orchestrator)**:本节原写「Python 生成器解析该 enum 产出
> Luau 表」。不需要。那张表只是一堆字符串,宿主直接从 domain 那个函数构建,单一真相
> **由构造保证**,不用解析、不用构建期 codegen、不多一件需要同步的产物;覆盖性检查
> 也从构建期解析变成运行期断言,更实。

不生成 C++ enum——那会伤 IDE 导航,收益不抵。

**三层语义保留:**

| Tier | 含义 | project 看到 |
|---|---|---|
| A | 正常缺席 | `nil` |
| B | 可恢复的自动化失败 | 错误 userdata,可被 `ctx:try` / `pcall` 捕获 |
| C | 宿主终局控制 | 捕获得到那一次错误,但拿不回控制权:下一次原语调用即拒绝 |

> **补记 2026-07-29(阶段 3b `d1a0685`)——`ctx:try` 只接 Tier B,这一点表里读不出来**:
> `ctx:try` 捕获的**只有**宿主铸的 Tier B 载体;别的一律 `error(err, 0)` 原样重抛。
> step body 里一句普通的 `error("boom")` 不会变成 `(false, err)`,它会穿过 `try` 继续
> 上抛,要接就得用裸 `pcall`。
>
> 这是有意的,而且是 `try` 存在的理由:project 自己的 bug 不是自动化失败,把两者塞进
> 同一个返回值会让一个拼错的字段名看起来像一次可重试的超时。Tier C 那个哨兵是纯字符串,
> 走的也是这条重抛路径,所以 `try` 既吞不掉 bug 也吞不掉取消。
>
> 对作者来说这是个真陷阱(「我 try 了怎么还是炸了」),所以它同时写进了
> `docs/pitfalls/page-modeling-and-multi-step.md`。回归由
> `tests/task/test-framework-context.cpp` 的 "ctx:step nests strictly..." 钉住。

**retry 语义**(修掉草案的自相矛盾):`on` 列表对它点名的 kind **覆盖** `retryable`;
`on` 缺省时 `retryable` 是默认。

必须这样,因为 `retryable` 直接由 `FailureResponse` 推导,只有 `CaptureStalled` 和
`StaleObservation` 是 `Retry`,而 `Timeout` 和 `TargetUnavailable` 都是 `Abort`
(`domain/error.cpp:80-88`)。若 `retryable` 是硬过滤,「等页面,游戏慢了就整步重来」
——日常脚本最常见的写法——永远不可能重试。

不去改 `domain/error.cpp` 里 `Timeout` 的分类:那会改变整个 engine 的
`failureResponse`,为脚本层方便动它不划算。

> **落地读法 2026-07-29(阶段 3b `d1a0685`)——「覆盖」是严格的**:`on` 在场时
> `retryable` **完全不被读**。`ctx.luau` 的 `retryAllowed` 就是这个形状——`on == nil`
> 才回落到 `err.retryable`,否则只在 `on` 里线性找 `err.kind`。所以 `on` 没点名的 kind
> **不重试**,哪怕它的 `retryable` 是 true。
>
> 这是有意的:`on` 是作者写下的「这一段我认这几种失败」,一份白名单读成白名单,而不是
> 读成「白名单再并上默认集」。否则 `on = { uf.errors.timeout }` 会连带把
> `CaptureStalled` 和 `StaleObservation` 一起重试进来,而作者从字面上看不出这件事。
>
> 另外两条边界同批落地:只有 Tier B 载体会被重试(判定是
> `type(result) == "userdata" and getmetatable(result) == errorTag`),其余原样重抛——
> 所以 Tier C 那个纯字符串哨兵**不可能**被 retry 变成三次新尝试;`policy.attempts`
> 是总次数,缺省 3,小于 1 直接是作者错误。

**失败优先级:**

1. 取消在动作投递前优先,fail closed。
2. 原语自身的失败优先于同时发生的 trace sink 失败。
3. 动作已成功投递后 trace 失败:run 失败,但不得重投动作。
4. project 脚本错误不伪装成自动化错误。
5. framework 内部不变量失败使用独立 kind,并且**先在 C++ 侧闩住终局再抛**——否则
   project 的 `pcall` 能吞掉它。

## 十、确定性

`now()` 从脚本表面**删除**。

现在的 `DeterministicClock` 是诚实的(逻辑序数、可复现),但几乎没用:作者写
`until uf:now() - t0 > 5000` 得到的是「循环 5000 次」。它唯一正当的用途是单调守卫,
而单调守卫脚本不该自己写。

替代:

- **基于证据的等待** — `ctx:wait_for_page(page, { timeout_ms })`。
- **声明式停顿** — `ctx:settle(ms)`,有界、进 trace、时长是重放输入的一部分。

脚本永远读不到任何形式的时钟。这一刀砍掉的是「不可复现脚本」这一整类。

其余不变:固定算法 seeded RNG(PCG32),seed 由宿主每 run 注入并入 trace;禁止用无序
遍历结果决定行为;公开集合用有序数组。framework 侧另加:interrupt 按注册顺序、
step path 稳定、语义事件顺序稳定。

## 十一、为什么不用 driver

草案的 yield/request/result driver 不采用。三条论证被落地代码证伪(取消临界区、统一
trace 边界、取消不可吞),细节见评审文档。这里只记结论性的两条:

**一、它不消除 C 帧,它增加一个。** yield 唯一的独有性质是「挂起时 C 栈是空的」。
它换来四件事:不等 C 调用返回就能销毁 VM(但你照样卡在 `executeRequest` 里等 engine)、
序列化续体(不要)、把 effect 扔到别的线程(明确非目标)、挂起态不占线程(P2 是两三个
任务,不构成理由)。

**二、它引入一个静默失败模式。** 实测的不可 yield 位置(七种,即 §15 对抗套件里的那张
矩阵):`__index` 是函数、`__newindex` 是函数、`__tostring`、`table.sort` 比较器、
`string.gsub` 回调、generic-for 迭代器函数、**`xpcall` 错误处理函数**。在 driver 设计里
最后一种不报错:Luau 把错误处理函数内部的失败折成 `LUA_ERRERR`,交给**调用方**一个平平
无奇的 `(false, "error in error handling")`,原始错误和那个请求一起消失,run 一路跑完
返回 OK。直接绑定可以从任何地方调用,没有这一类。

> **更正 + 补记 2026-07-29(阶段 2e `2ebcf0c`)**:上面「run 返回 OK」这句**只对被否决的
> driver 成立**,不要读成对本设计成立的事实——照字面搬用会得出「取消能在 `xpcall` 处理
> 函数里被静默吃掉」的错误结论。同一个位置在**本设计**下的实测结果是:
>
> - `__newindex` 原来漏在这张表外,现已补上。它和 `__index` 同一类,而且它才是宿主句柄
>   实际用的那个(`uf-tables.cpp` 的 `denyWrite` 挂在每张句柄与错误 metatable 上)。
> - `xpcall` 的静默确实发生:一次落在错误处理函数里的 `lua_break` 退化成普通错误,又被
>   `LUA_ERRERR` 折掉,调用方拿到的就是那句 `(false, "error in error handling")`。
> - **但它被关住了。** run **不**返回 OK:脚本继续跑,下一个 VM 安全点干净地 break,
>   engine 边界报 `Cancelled`。买下这份关押的正是那一条
>   **`runStatus == LUA_BREAK || control->broken`** 的析取
>   (`modules/script/source/script/ffi/environment.cpp`):`lua_break` 在 C 帧里退化时
>   `lua_resume` 返回的是 `LUA_ERRRUN` 而不是 `LUA_BREAK`,只认状态码会把宿主的控制信号
>   报成一次可恢复的脚本失败。删掉 `broken` 这一半,七种形态里有六种当场从 `Cancelled`
>   变成 `InvalidResource`。
>
> 结论对 §11 的论证方向没有影响:driver 会把这一类从罕见变成常态,而直接绑定加
> `broken` 闩把它压成「一次被吞掉的错误值」,而不是「一次被吞掉的取消」。整张矩阵的正反
> 两侧都在 `tests/script/test-adversarial-substrate.cpp`(含全绿控制组),
> 原语侧在 `tests/task/test-adversarial-surface.cpp`。

**可逆性**:§5 的四条不变量满足时,这 12 个签名**已经是**一份 request/response 协议。
以后转 yield 是把 12 个 `lua_CFunction` 拆成 mint + executor 分支,framework 原语层
以上和 project 脚本一行不动。第 1 条(不回调进 Lua)是关键:一旦有原语接受 Lua 回调,
转换就从机械操作变成重写。

### 为什么句柄不是可序列化 DTO

承接已删除的 ADR 0001,论证保留于此。roadmap §5 约束 8(「`IScriptRuntime` 边界只传
可序列化 DTO,脚本永不持有 C++ 指针」)读起来像 P0 规则,但它**绑定的是未来跨进程
worker 接缝**,不是进程内绑定。P0 里票和句柄都是宿主拥有的 opaque userdata。

四条理由,以后有人再提 DTO 边界时直接引用:

1. 证据对象只有留在宿主保管下才可信——私有构造就是来源证明,序列化副本可被篡改或重放。
2. 动作后作废必须一次命中所有脚本引用,只有单一宿主对象做得到(在本设计里是关掉票)。
3. 脚本从不窥视句柄,只把它传回来;拷 8 MiB 什么也没买到。
4. 脚本可见的 API 形状与表示无关,以后换成 ID + IPC 留在绑定层内,project 脚本不改。

跨进程 worker 那个未来接缝拿到的是 §5 那 12 个签名,不是 Luau 的 yield;它的重评估
触发条件在 roadmap 里已枚举,当前一条都不满足。

**会重开这个决定的四个触发条件**(写进文档,不靠记忆):

1. 直接绑定下实测 500ms 硬取消预算失败。
2. P2 实际要同时挂起的任务数上到两位数。
3. roadmap 的跨进程 worker 重评估触发条件真的触发。
4. 出现「任务状态要跨进程重启存活」的需求。

## 十二、Trace:一条流

`engine-trace/v1` 与 `task-trace/v1` 合并为 **`umbraflow-trace/v1`**。

两条流的存在纯粹因为 engine 先写完。今天它们**没有 join key**:task 流不带帧身份,
engine 流不带 run/generation id,两个 sink 都不写时间戳。所以「三层串成一个有序 run」
这个验收标准今天做不到。合并之后排序免费、join 免费、一份 golden、一个 sink 寿命。

### 事件

**宿主权威**(C++ 写,Luau 无法请求):

```text
run.started              project id / task name / source hash / framework version+hash /
                         luau compiler version / resource snapshot hash / seed /
                         config digest / generation id / run id
run.resources_validated
run.finished             outcome / error kind
engine.observed          capture session id / target generation / frame id
engine.page_resolved     page id | unknown | ambiguous,分数
engine.action_found      recognizer id / sad score / matched rect
engine.action_authorized
engine.action_rejected   原因
engine.action_delivered  client 坐标 / receipt
engine.observation_invalidated
task.native_call         序号 / 原语 / 入参身份 / outcome / error kind /
                         durationMillis(仅 settle)
```

> **补记 2026-07-29(阶段 2c `c37ee5b`)——`run.finished` 的 error kind 是真 kind 了**:
> 在此之前,一个**没人捕获**的 Tier B 错误穿出脚本时,`script` 只知道「栈顶是个非字符串
> 的值」,于是整类失败一律报 `InvalidResource`——一个超时没被 catch 的任务会在报告和
> `run.finished` 里被记成「脚本格式不对」。修法是给 `script::EngineConfig` 加第三个接缝
> `RaisedErrorClassifier`(`classifyRaisedError`),由 `modules/task` 的
> `CapabilitySurface::raisedErrorClassifier()` 提供,**只按载体的 userdata tag 判定**,
> 绝不读字段——鸭子类型会让 project 自己挑一个 kind 来背自己的锅。
> 硬取消在它之前分类且永不进它,所以 classifier 无法把一次硬取消降级成可捕获的 kind。
>
> 因此凡是枚举 script 模块接缝的叙述都要数三个,不是两个:`installHostTables`、
> `installPrivateCapabilities`、`classifyRaisedError`。

> **补记 2026-07-29(阶段 3a `f146329`)——`task.native_call` 多一个
> `durationMillis`**:`std::optional<uint64>`,**只有 `settle` 带**,其余动词一律缺席
> (`trace/event.hpp` 的 `NativeCall`)。理由是 `settle` 是唯一一个碰不到任何 engine 动词
> 的原语:它整个内容就是那段时长,不写下来这一行就等于没说什么,而 §10 要求停顿本身
> 是重放输入——停两秒的 run 和停十秒的 run 是两个 run。
>
> `umbraflow-trace/v1` **没有升版**:加一个可缺席字段是加法式扩展,旧读者读到的仍是
> 一条合法的 v1 行。取消掉的 settle 也会先把这一行写完再走终局路径,所以线上看到的是
> 「一次被打断的停顿」,而不是一个什么都没报的动词。

**framework 语义**(经 `emit` 请求,C++ 校验并盖章):

```text
framework.step_started / step_finished
framework.retry_attempt / retry_backoff
framework.interrupt_matched / interrupt_handled / interrupt_exhausted
framework.settled
framework.subtask_entered / subtask_exited        -- P1
```

C++ 在每条事件上盖:`seq`(单调)、`runId`、`generationId`,以及 `wallClock`。
**`wallClock` 属于一个文档化的非 golden 字段集**,确定性断言比较前剥掉——这解决了
草案 §12 与 §19.3 的自相矛盾。

### C++ 侧的校验状态机

`emit` 不是透传。最小规则集:

- 恰好一次 `run.started` 和一次 `run.finished`;`run.finished` 之后不接受任何语义事件。
- `step_started`/`step_finished` 严格良嵌套,finish 必须指名最内层未闭 step;
  硬性嵌套深度上限;`run.finished` 时仍有未闭 step ⇒ `Failed(InternalInvariant)`。
- `subtask_entered`/`exited` 良嵌套,且与 step 的交错一致。
- `retry_attempt` 在其 retry 作用域内单调,且不超过该作用域声明的 attempts。
- `interrupt_handled`/`interrupt_exhausted` 必须跟在同 id 的 `interrupt_matched` 之后,
  且该 id 无嵌套 match。
- 每条 `task.native_call` 必须落在当时打开的 step 作用域内。
- 每事件字段长度、字符集、总载荷有上限,**在请求边界拒绝,不静默截断**。
  step 名来自 project 字符串字面量,长度/字符集/同父唯一性是 C++ 的责任。

### 定位

**task 与 framework 事件是审计日志,不是重放日志。** 重放靠 seed + 测试里记录的观察
序列,不靠生产 trace。写清楚,避免以后有人按重放的期望去读它。

## 十三、TaskHost

从第一天就是 D10 锁定的动词集,P0 实现子集:

```cpp
class TaskHost
{
public:
    auto loadProject(ProjectPath path, TaskHostConfig config) -> Result<GenerationId>;
    auto startTask(GenerationId generation, TaskName task, TaskRunConfig config)
        -> Result<TaskRunReport>;
    auto cancel(GenerationId generation) -> Status;
    auto queryTask(GenerationId generation) -> Result<TaskStatus>;

    // P2:P0 返回 UnsupportedCapability
    auto pause(GenerationId generation) -> Status;
    auto resume(GenerationId generation) -> Status;
    auto subscribeEvents(ITaskEventSink& sink) -> Status;
};
```

未实现的动词返回一个**已经存在的真实错误 kind**,没有未测试分支。这是签名承诺,不是
投机性泛化——两者的区别就在这里。D10 存在的意义就是 P0 到 P2 不换 API 面。

`entry/cli` 只剩:解析参数、构造 controller adapters 与端口、调 `TaskHost`、打印报告。
`runScriptFlow` 那 130 行运行生命周期(建能力面、载入校验任务、开 trace sink、选 seed、
发 `run.started`、分类退出原因、发 `run.finished`)全部搬进 `TaskHost`,并顺手注入
每 run 种子,干掉固定默认值占位。

`TaskRunReport` 的字段与到 `ExitCode` 的映射在本文定义,不留给实现方。

## 十四、构建管线

framework 是 `.luau`,而本仓库今天对 `.luau` 没有任何管线:模块自动加载器只 glob
`source/*.cpp` 和 `source/*.hpp`,模块不允许有自己的 CMakeLists,全仓一处
`configure_file`、零 `add_custom_command`,`check_safety.py` 只认 C/C++ 扩展名。

方案:

1. framework 源码放 `modules/task/runtime/*.luau`。
2. `scripts/embed_luau.py` 生成 build tree 里的一个 `.cpp`:字节 + 每文件 SHA-256 +
   bundle hash + framework 语义版本。**生成物不提交**,保证单一真相。
   hash 复用 `annotation::sha256`(`modules/task` 已依赖 annotation),不引第二套。
3. `modules/task/manifest.txt` 加一个 `embed` 段,`cmake/manifest.cmake` 与
   `scripts/check_modules.py` 认它。
4. `.luau` 语法门:`modules/task/source/task/ffi/framework-bundle-syntax.cpp` 里的
   `checkFrameworkModuleSyntax`(声明在 `framework-bundle.hpp`)直接调 vendored
   Luau 的 `Parser`,由 doctest `tests/task/test-framework-bundle.cpp` 逐个解析
   bundle 里的每个模块。它随 `test-task` 带 `CI` 标签,所以已经在最小验证门的
   `ctest -L CI` 里。

> **修订 2026-07-29(orchestrator,阶段 1a 落地时)**:本项原写「`scripts/check_luau.py`
> 用 vendored 的 Luau 编译器做语法门」。**这个脚本不会存在。**
> `modules/script/external/CMakeLists.txt:6` 强制 `LUAU_BUILD_CLI OFF`,构建里根本没有
> 可供 Python 调用的 `luau` 二进制;打开它等于为一道语法门多编一个 CLI、多一条进程边界。
> 换成 C++ doctest 直接调 `Luau::Parser`,语法门与 bundle 的 hash/排序断言住在同一个
> 测试文件里,门的语言也和被门的东西一致。`tests/task/runtime/` 目前不存在,阶段 3 加
> framework 单测时再一并纳入这个测试的解析范围。
> 这条写下来是因为「补一个 Python 检查器」否则会被后来的会话当成未完成的工作反复重开。

**关于 SHA-256 要说实话**:它和被它证明的字节编译进同一个二进制,对能改二进制的人
证明不了任何东西。它的真实作用是给 trace 盖章(一次 run 可归因到确切的 framework
构建)和抓意外过期。不写成安全性质。

## 十五、测试

### framework 单测:在私有能力面注入 fake

doctest fixture 建 VM → 装一个**脚本化的假能力面**(「`cycle_open` 返回票 1、
`cycle_page` 返回 home、`cycle_find` 返回 nil、`wait` 第三次返回 false」)→ 加载
bundle + 测试 `.luau` → 断言原语调用序列与语义 trace。

不需要 WGC、engine、controller。测试 `.luau` 放 `tests/task/runtime/`,同一个生成器
嵌入,`cpp_add_test` 不用改(它没有数据文件机制)。

覆盖:step 嵌套与稳定 path、wait 成功/超时、可重试/不可重试、backoff attempts、
interrupt first-match/不重入/max_hits、周期开关配对、确定性顺序、framework 内部错误
分类。

### C++ 侧

票据校验(死票、跨 generation 票、错误 kind 的票)、周期消费后一切操作失败、
终局闩之后所有原语拒绝、trace 失败优先级、deadline 溢出与时长边界、
framework/project 环境隔离、载荷上限。

### 对抗套件

在现有 veto 套件之上补:对 framework export 调 `getfenv`、`table.clone` 伪造身份表、
project env 的 `__index` 链遍历、`_G` 的 7 种绕过形态(平移到 `uf` 根)、
project 取不到私有能力面、`pcall`/`ctx:try` 吞不掉终局、无限循环在 SLA 内停、
**每个原语人为阻塞后总退出仍在预算内**(roadmap 一票否决第 6 条)。

> **状态 2026-07-29(阶段 2e `2ebcf0c`)**:除最后一条外**已落地**,分两个文件,
> 按被攻击的层分:
>
> - `tests/script/test-adversarial-substrate.cpp` — 底座侧。§11 的整张七形态不可 yield
>   矩阵(含全绿控制组)、主动扛住指令预算与墙钟上限的脚本、跨 run 的种植路径
>   (库表与字符串 metatable)。
> - `tests/task/test-adversarial-surface.cpp` — 原语与脚本表面侧。`ctx` 的可达面封闭性、
>   从 project 出发到 framework-only 值的路径扫描(带控制组证明扫描器真的会找)、
>   共享标准库不可变、Tier B 载体伪造、冒名者被归为脚本自身失败、宿主对象在每条路径上
>   拒绝改写、原语从不可 yield 上下文调用仍守协议、终局闩在任意上下文拒绝原语、
>   任何嵌套的捕获都换不回控制权、硬取消换不到 Tier B、不调原语的死循环仍被停。
>
> **未落地的是「每个原语人为阻塞」这一条(一票否决第 6 条本体)**,仍排在阶段 3。
> `tests/script/test-veto-suite.cpp` 今天只覆盖它的底座切片(不可 yield C 帧里的死循环),
> 真正「阻塞一个注册的宿主 C binding」要等阶段 3 的 `deadline` / `wait` 原语与
> `IFrameSource::capture()` 的 deadline/stop_token 一起做。

### 真机验收

完整一轮日常;全程严格后台;click 后旧票必死;**长等待中弹窗被处理**;Ctrl-C 达
SLA;一条 trace 足以解释每一步。

## 十六、删除清单

放开重构后明确要删的东西。列出来是为了实施时不犹豫。

状态标记(**核对至 `8b16f2d`,2026-07-29**):**已删**的每一项都在当前工作树上 grep
验证过确实没有残留;未标记的项尚未动。

> **文件改名注记(阶段 2d `2f4af93`)**:下面提到的 `umbra-tables.cpp` 已随根改名成为
> `modules/task/source/task/ffi/uf-tables.cpp`,本节与全文按新名书写;引用旧名的历史
> 文档(评审记录、被取代的草案)不回改。行号也随之整体位移,不再逐条引用。

**modules/engine**

- `EngineSession::waitForPage`、`PageWait`、`sweepKnownPopups`——能力层里不该有 policy
  循环,那个循环还捎带一个永久 no-op 的弹窗接缝。(**已删,阶段 3c `8b16f2d`**:三个
  符号在整棵树上都 grep 不到。**engine 现在完全不轮询**——每个动词都是单次的
  observe / resolvePage / findAction / act,循环只存在于 `ctx.luau`。)
- ~~`pollSleep` 迁移到 task 的 `wait` 原语。~~(**更正,阶段 3a `f146329`**:它**升进了
  `modules/core/source/core/time/poll-sleep.hpp`,并留在那里**,不是搬进 task。切片睡眠
  是通用时间设施;今天的生产调用方是 task 的 `wait` 与 `settle`,engine 一个都没有。
  详见 §5 的落地更正。)
- 相应测试。(**已改,阶段 3a/3c**:`tests/engine/test-session.cpp` 的等待用例改成钉
  `CaptureBudget`——`DeadlineHonouringFrameSource` 是套件里唯一真的对着 budget 阻塞的
  帧源,用来证明 session 铸的是一个真期限,而不是一个适配器可以无视的时刻。)

**modules/task**

- frame-box 的 GC 析构释放路径、`guardObservationBudget`、seq 跨帧校验机械
  (`uf-tables.cpp` 里约 300 行),以及 `TaskContextConfig::maxLiveObservations`
  这个旋钮本身(见 §4 的修订:上限恒为 1)。
  (**已删,阶段 2a `01d0e9a`**:`ObservationSeq`、`maxLiveObservations`、
  `liveObservationCount()`、`guardObservationBudget`、句柄析构器的释放路径全部不再存在;
  帧的释放时机改由 `cycle_close`、消费周期的 click,或 `CycleLedger` 的析构决定。)
- `DeterministicClock` 及 `now()` 的全部绑定。(**已删,阶段 3a `f146329`**:
  `DeterministicClock`、`TaskContext::nowMillis`、`ctx:now` 与 `now` 原语都不存在了;
  `modules/task/source/task/deterministic.hpp` 只留一段解释为什么删的注释,以及那个仍然
  需要的 seeded RNG。)
- `wait_for_page` 原语、`TaskContext::waitForPage`、`CycleWait`。(**已删,阶段 3b
  `d1a0685`**。这一项**不在本清单原稿里**——原稿只写了 engine 那半边的循环,没写 task
  这半边也有一个。等待循环整体成为 `ctx.luau` 里的 Luau,C++ 侧不再有任何一个动词
  「等到某页出现」。)
- `task-trace/v1` 作为独立 schema。(**已删,阶段 1b `408dc90`**:与 `engine-trace/v1`
  合并为 `modules/trace` 下的 `umbraflow-trace/v1`,源码里只剩解释合并的注释。)
- `uf:try` 的 C 绑定(语义由纯 Luau 承接,`markFatal`/`guardFatal` 保留)。(**已删,
  阶段 2b-2 `e89bc53`**:`tryFn` 这个 C 闭包不再存在,`ctx:try` 是
  `modules/task/runtime/ctx.luau` 里的纯 Luau `pcall`,其余原样重抛;
  `markFatal`/`guardFatal` 按计划保留在 `uf-tables.cpp` 里,每个原语入口仍先过
  `guardFatal`。**判定形状已随阶段 2c `c37ee5b` 收紧**:2b-2 时是「受保护的
  `__metatable == 'uf.error'`」,今天是
  `type(err) == "userdata" and getmetatable(err) == errorTag`——多出的 `userdata` 那一半
  才是 project 造不出来的东西,标签只负责把错误和 page / hit 之类宿主句柄分开;
  `errorTag` 由私有能力面的 `error_tag` 字段交进来,`.luau` 里不重写那个串。)
- `TaskContext::cancelled()`。(**已删,阶段 2b-2 `e89bc53`**。这一项**不在本清单原稿
  里**,补记于此:它唯一的消费者就是那个 C `try` 绑定——`try` 需要在捕获后问一句
  「是不是已经终局」。绑定删掉之后它没有调用者,而它想表达的保证已经由「每个原语入口
  过 `guardFatal` 检查终局闩」结构性承担,所以是随 `try` 一起走的,不是独立裁决。)
- `TaskContext` 作为单个类:拆成票据账本 + 私有能力面。(**已完成**:票据账本析出为
  `cycle-ledger.hpp`/`.cpp`(阶段 2a `01d0e9a`);能力面进 framework 闭包 upvalue
  (阶段 2b-2 `e89bc53`),`CapabilitySurface` 由此分成两个接缝——`installer()` 装
  project 可见的数据表,`privateCapabilities()` 建那张只交给 framework 的私有表。
  `TaskContext` 本身作为宿主侧 session 对象保留:它是那些原语闭包背后的持有者,
  不是要被解散的东西。)
- 两份 kind→wire 映射(整体上移到 `modules/domain`)。(**已删,阶段 1c `31ea3af`**:
  `snakeName` 与 `errorKindWireName` 都不存在了,唯一真相是
  `domain::automationErrorWireName`,错误 kind 表由宿主从它构建。根改名已于阶段 2d
  `2f4af93` 落地,这张表今天挂在 `uf.errors` 上。)
- project 环境里的全部裸动词。(**已删,阶段 2b-2 `e89bc53`**:project 全局 `uf` 只剩
  数据——`uf.recognizers` / `uf.pages` / `uf.errors`,`buildUfData` 里没有任何能观察或
  动作的东西;原来的动词由 `ctx` 顶替。)

**entry/cli**

- `runScriptFlow` 的生命周期部分(搬进 `TaskHost`)。(**已搬,阶段 1d `e387453`**:
  `runScriptFlow` 这个符号在 `entry/` 里已不存在。)
- `runSmokeFlow` 与 `--page`/`--action`(**2026-07-29 开发者裁决:删**)。`--task` 已
  覆盖其用途。连带:`ExitCode::ActionAbsent` 失去唯一来源,随之删除;`args` 的互斥
  校验简化为只认 `--task`;B1 smoke 的真机验证路径改由一个最小 `.luau` 任务承担,
  在阶段 4 一并落地。(**已删,阶段 1d `e387453`**:退出码 3 在 `entry/cli/run.hpp`
  里留了一条「此位刻意空着」的注记,不再复用。)
- `--timeout` 与 `--poll`。(**已删,阶段 3b `d1a0685`**。这一项**不在本清单原稿里**:
  两个 flag 原本是页面等待的宿主默认值,经 `TaskRunConfig` 转发进 `TaskContextConfig`。
  等待循环成为 framework 的 policy 之后,这两个值 framework 读不到,宿主也没有任何地方
  读它们,于是它们连同两个 config 字段一起走。今天 `umbra-flow run` 把它们当**未知参数
  拒绝**,`runUsageText()` 也不再列。剩下的 `--recognition-timeout` 是每次识别的期限,
  与页面等待无关,保留。)

**docs**

- `2026-07-28-luau-first-task-system-design-draft.md` 标记为已被本文取代。
- `2026-07-27-p0b-script-layer.md` 的脚本层裁决部分标记为已被本文取代。

预计 `modules/task` 现有 3443 行第一方代码约六成原样存活;
`tests/task/test-task-binding.cpp`(882 行)大半按周期语义重写。

## 十七、实施阶段

排序依据不再是「不丢弃任何东西」,而是:先做后面全部依赖的地基,再尽早关闭真正的
能力缺口,再尽早上真机。

### 阶段 1 — 地基(**已完成 2026-07-29**)

- **1b(`408dc90`)** 合并为单条 `umbraflow-trace/v1`(先做,因为之后每个阶段都往里发
  事件)。落地时连 `modules/trace` 这个模块一起立起来了,两个 sink 变一个。
- **1a(`9d9d164`)** `.luau` 构建管线:`embed_luau.py` + `manifest.txt` 的 `[embed]` 段
  + 语法门。语法门的形态与本文原稿不同,见 §14 的修订——不是 `check_luau.py`,
  是 `tests/task/test-framework-bundle.cpp` 里的 doctest。
- **1c(`31ea3af`)** kind→wire 映射合并为 `modules/domain` 的一份
  (`automationErrorWireName`);宿主在能力面安装时构建错误 kind 表;加覆盖性测试与
  trace/脚本拼写一致性测试。
- **1d(`e387453`)** `TaskHost` 立起 D10 动词形;CLI 收缩(`runScriptFlow` 与
  `runSmokeFlow` 双双消失);注入每 run 种子。

出口:全门绿,现有任务仍按老表面跑通。**已达成。**

### 阶段 2 — 周期协议 + 两个环境 + `uf` 根(**已完成 2026-07-29,`2ebcf0c`**)

> **落地顺序**:`2a` `01d0e9a` → `2b-1` `67e7e63` → `2b-2` `e89bc53` → `2d` `2f4af93`
> → `2c` `c37ee5b` → `2e` `2ebcf0c`。**2d 先于 2c 落地**,下面的小节按落地顺序排,
> 不按字母序——2c 的错误载体改造要在 `uf` 根改名之后做才只改一遍标签串。
> 出口两条(全门绿 + 对抗套件绿)均已达成。

> **拆分 2026-07-29(orchestrator)**:本阶段原稿把「私有能力面进 upvalue;裸动词从
> project 环境移除」和环境机械写在同一条里。做不到:**裸动词不能在有 `ctx` 顶替它们
> 之前离开 project 环境**,而 `ctx` 要由 framework 模块提供,而拆分写下时
> `modules/task/runtime/` 下只有一个不声明任何 API 的 placeholder(2b-2 已把它换成
> `ctx.luau`)。硬拆会留下一个既没有裸动词也没有 `ctx` 的 project 环境,门是红的,
> 没法收工。
> 因此阶段 2 的后半按下面的 2b-1 / 2b-2 两步走,顺序不可交换。

#### 2a — 观察周期(**已完成,`01d0e9a`**)

- 票据账本(`CycleLedger`)+ 5 个周期原语;删 GC 释放路径、`guardObservationBudget`、
  seq 机械(详见 §16 的状态标记)。
- 顺带:trace 的 `observationSeq` / `hitObservationSeq` 改名 `cycleOrdinal` /
  `hitCycleOrdinal`——同一次提交删掉了 `ObservationSeq` 这个类型,让线上字段继续背着一个
  已退休概念的名字,会把它写进后面要建的校验状态机;`umbraflow-trace/v1` 目前在仓库外
  没有消费者,现在改代价为零。

#### 2b-1 — 环境机械就位,能力面暂不搬家(**已完成,`67e7e63`**)

- `HostTableInstaller` 改返回 `Status`(原为 `script/engine.hpp` 的 `void`)。
- C++ 侧环境分离:`luau_load` 的 env 索引 + C 侧 `lua_setfenv`。
- project 环境的白名单与 §7 那份**完整**否定名单。
- 运行期冻结规则(构造时冻结、`__metatable`、`__index` 是表)。
- **能力面仍然装在 project 环境里**,不动裸动词。这一步不改脚本可见的表面,所以门始终
  是绿的,环境隔离可以单独被对抗套件打。
- 落地形态:`modules/script/source/script/ffi/environment.{hpp,cpp}` 新立,两张 env 表
  住在 VM registry 里(两个环境都不从任何全局表可达);project env 是**冻结的原型 +
  每次 run 一份浅拷贝**,所以脚本写的全局随那份拷贝一起死。

#### 2b-2 — 能力面搬进 framework 闭包,裸动词下线(**已完成,`e89bc53`**)

- 私有能力面从 project 环境移进 framework 闭包的 upvalue。
- `modules/task/runtime/placeholder.luau` 长成一个最小 framework,导出一个薄 `ctx`
  ——只要够顶替被删掉的裸动词,`ctx:step` / `ctx:wait_for_page` / `ctx:retry` 那一整套
  仍归阶段 3。(落地时该文件**改名为 `modules/task/runtime/ctx.luau`**,`placeholder`
  这个名字在树上已不存在。)
- 裸动词从 project 环境移除。
- **删 `uf:try` 的 C 绑定**,`ctx:try` 改为纯 Luau 的 `pcall`。
  *裁决 2026-07-29(orchestrator)*:这一条不必等 §9 的错误 userdata。纯 Luau 的 `try`
  只需要能认出「这是自动化错误」,而今天的错误表已经带受保护的
  `__metatable`(阶段 2d 起是 `'uf.error'`),Luau 侧一句 `getmetatable(err)` 就够;
  2c 换成 userdata 之后,`try` 的判定从比较那个字符串改成比较 tag,是一处局部替换。
  反过来若拖到 2c,`ctx` 已经存在却还留着一个做 `lua_pcall` 的 C 闭包,正是 §8 要消灭
  的形状。
  *落地更正(2c `c37ee5b`)*:上面这句预判只对了一半。**Luau 侧比不了 tag**——
  `lua_userdatatag` 没有脚本入口——所以 `ctx:try` 今天是
  `type(err) == "userdata" and getmetatable(err) == errorTag`:仍然比那个标签串,
  只是前面多了一道 `userdata` 类型闸。改成比 tag 的是 **C++ 侧**的 `tierBErrorKind`。
  「一处局部替换」这个判断本身成立(改的只有 `ctx.luau` 里那一行),
  换的东西不是原话说的那样。
- 顺带删掉 `TaskContext::cancelled()`——见 §16 的补记,它唯一的消费者就是那个 C 绑定。
- 落地时把 boot 顺序改了:危险全局的剥除提前到 framework 加载**之前**。理由与那条测试
  见 §7 的修订注。

#### 2d — 根 `umbra` → `uf`(**已完成,`2f4af93`**)

- `lua_setglobal` 的根名与校验器的 `k_namespace` 都改成 `uf`;六张句柄 metatable 的标签
  改成 `uf.recognizer` / `uf.page` / `uf.cycle` / `uf.resolved_page` / `uf.hit` /
  `uf.error`;Tier C 哨兵串改成 `"uf: task cancelled"`;错误消息与示例一并改。
  `umbra-tables.cpp` 随之改名为 `uf-tables.cpp`。
- **刻意不动的边界**:产品名 `UmbraFlow` / `umbra-flow` / `umbra-workbench`,以及
  schema id `umbraflow-authoring/v2` / `umbraflow-annotations/v1` /
  `umbraflow-trace/v1`,全部保持原样。改的是**脚本能力根**这一个词,不是产品的名字,
  也不是任何线上契约的 id。文档若把这两件事混在一起,以此条为准。

#### 2c — 错误改宿主 mint 的 userdata(**已完成,`c37ee5b`**)

- Tier B 载体从冻结表换成 `lua_newuserdatatagged` 铸的 tagged userdata(§9)。C++ 侧
  `tierBErrorKind` **只读 tag**;`ctx:try` 改成先验 `type(err) == "userdata"` 再比标签,
  标签由私有能力面新增的 `error_tag` 字段交给 framework(§5 的补记)。
- 每抛一次错现建一份 per-raise metatable,走 `script::deepFreezeMetatable` 同一道形状门
  (§7 的补记)。
- `script::EngineConfig` 加第三个接缝 `RaisedErrorClassifier`,于是没人捕获的 Tier B
  错误在报告和 `run.finished` 里报**真 kind**,不再一律 `InvalidResource`(§12 的补记)。

#### 2e — 对抗套件扩充(**已完成,`2ebcf0c`**)

- 两个新文件按被攻击的层分:`tests/script/test-adversarial-substrate.cpp` 与
  `tests/task/test-adversarial-surface.cpp`。覆盖清单与仍未落地的那一条见 §15 的状态注。
- 副产物是三处文档更正,都已就地写回:§11 的不可 yield 矩阵漏了 `__newindex`、
  它的「run 返回 OK」只对 driver 成立(§11 的更正注),以及 §8 的取消保证并不建立在
  终局闩上——VM 中断先于 call 指令完成就已经拦住了(§8 的补记)。

出口:全门绿 + 对抗套件绿。**已达成。**

### 阶段 3 — framework 承接 task policy

- **3a(`f146329`,已完成)** `deadline` / `wait` / `settle` 原语;
  `IFrameSource::capture()` 补 deadline/stop_token(落地为 `CaptureBudget`,期限由
  `EngineSession::observe()` 铸,不经脚本——见 §5)。同批**删掉 `DeterministicClock` 与
  `now()` 的全部绑定**,`pollSleep` 升进 `modules/core`。
  *裁决 2026-07-29(orchestrator)*:§16 列了这一条却没排进任何阶段,补在这里。不能更早,
  因为 `now()` 今天是脚本唯一的时间设施,而 §10 之所以敢删它,前提是 `settle` 与 `wait`
  已经把「等一等」这件事从脚本手里接管过去;两者不同批,中间会出现一个既不能读时间也不能
  等待的脚本表面。
- **3b(`d1a0685`,已完成)** framework Luau:`ctx:step` / `ctx:cycle` /
  `ctx:wait_for_page` / `ctx:retry` / `ctx:try` / interrupt 注册表,外加
  `modules/task/runtime/task.luau` 这个声明表面(`task.define` / `task.interrupt`,
  拼写见 §6 的注)与 `raise` 原语。同批删掉 C++ 的 `wait_for_page` 原语、
  `TaskContext::waitForPage`、`CycleWait`,以及 CLI 的 `--timeout` / `--poll`。
  **弹窗-长等待缺口在此关闭**:interrupt 在等待循环的每一轮都有机会匹配,
  而不是像旧的 no-op 接缝那样只在一次等待的开头响一次。
- **3c(`8b16f2d`,已完成)** 删 engine 的 `waitForPage` / `PageWait` /
  `sweepKnownPopups`。**engine 从此完全不轮询**,每个动词都是单次的。
- **3d(进行中)** 语义事件 + C++ 校验状态机 + 载荷上限;`emit` 与 `terminal` 两个原语
  随这一批落地(§5 的形状快照按 `8b16f2d` 写,不含它们)。
- **未做** framework Luau 单测的 fake 能力面形态。§15 要的是「装一个脚本化的假能力面」;
  今天 `tests/task/test-framework-context.cpp` 覆盖的是同一张清单
  (step 嵌套、wait 成功/超时、on 覆盖 retryable、interrupt first-match / max_hits、
  周期开关配对、等待中取消),但驱动方式是**假 engine 端口**而不是假能力面。差别在于
  它仍然要过真的识别与 `CycleLedger`,所以「framework 恰好调了哪几个原语、什么顺序」
  这件事今天断言不到——要断言它,那张假能力面还是得建。
- **未做** 一票否决第 6 条进 CI(人为阻塞每个原语)。§15 已注明它等 3a 的 `deadline` /
  `wait` 与 `capture()` 的 budget,这两样现在都有了,阻塞点齐了。

出口:**弹窗-长等待缺口关闭(3b 已达成)**;全门绿。

### 阶段 4 — 第一个真日常(P0-C)

- 卡厄斯梦境日常写成单个 `.luau`,允许复制粘贴。
- 真机验收全项。

出口:真机通过。**这一阶段决定阶段 5 要不要做。**

### 阶段 5 — 跨文件复用(P1,仅当阶段 4 证明需要)

- `uf.task.import("name")` 走 `tasks/modules/` 下**同一套** allowlist;每源 hash 入
  `run.started`;module cache 与环检测在 framework 的 Luau 里。(拼写按 §6 的注读作
  `task.import`,直到那道回填接缝存在。)
- `ctx:call` 与 subtask 语义事件。
- **不做 manifest**,直到出现第二个 framework major 版本。

### 明确不做

yield/request driver(触发条件见 §11);`resources` 根改名;`tasks/manifest.json`;
pause 的实现(只留 §13 的签名,以及「framework 的观察周期边界是未来的 pause 点」
这一句约束)。

## 十八、待裁决与风险

**已裁决(2026-07-29):**

- **脚本根 = `uf`**,单一全局根。`uf::`(C++)与 `uf.`(Luau)是同一个产品缩写在两种
  语言里的一致用法。词条见 `CONTEXT.md`。
- **`runSmokeFlow` 与 `--page`/`--action` 删除**(§16)。
- **允许改动 `modules/engine`**:合并单条 trace 与删除 `waitForPage` / `PageWait` /
  `sweepKnownPopups` 均已获准,engine 的 golden 测试随之重写。
- **`ctx:settle` 的上限 = 30 秒,超限是 Tier B**(原「待裁决」第 2 项,阶段 3a
  `f146329` 落地)。走 Tier B 而不是 framework 不变量失败,是因为 §9 把不变量那个 kind
  留给「project 造不出来的失败」;一个项目要求 settle 十分钟是**项目自己的错误**,
  作者应当能 catch、能改。常数是 `task-context.hpp` 的 `k_maxSettleDuration`,
  仍是待标定占位(见下面第 1 项)。

**待你裁决:**

1. 常数标定:内存 64 MiB、指令预算 1e8、maxRuntime 30 分钟、`max_action_frame_age`
   生产值——目前全是保守占位,首个真日常 + 真机 soak 标定。阶段 3a/3b 之后这份清单
   多了七个,分住两边,**边界就是 §1 那条规则**:

   | 常数 | 值 | 住在哪 | 为什么在那一侧 |
   |---|---|---|---|
   | 单次捕获期限 | 2 s | `engine/session.hpp` `k_defaultCaptureTimeout` | 一次捕获能阻塞多久是宿主的资源边界 |
   | `wait` 轮询下限 | 10 ms | `task-context.hpp` `k_minWaitPollInterval` | 防 framework bug 把观察周期变忙等,是保证 |
   | `settle` 上限 | 30 s | `task-context.hpp` `k_maxSettleDuration` | 同上,宿主对一次声明式停顿的封顶 |
   | 默认等待超时 | 600000 ms | `runtime/ctx.luau` `k_defaultTimeoutMillis` | 等多久是 policy |
   | 默认轮询间隔 | 500 ms | `runtime/ctx.luau` `k_defaultPollMillis` | 多久重看一次是 policy |
   | 默认 retry 次数 | 3 | `runtime/ctx.luau` `k_defaultRetryAttempts` | 值不值得再试是 policy |
   | 默认 `max_hits` | 3 | `runtime/ctx.luau` `k_defaultMaxHits` | 一个弹窗处理几次是 policy |

   前两个 Luau 常数原本在宿主 config 里(`--timeout` / `--poll` 一路转发进
   `TaskContextConfig`),随 3b 一起搬;后两个是 3b 新增的,一开始就在 Luau。搬的理由
   不是就近,是 3b 之后**宿主已经没有任何代码读它们**:等待循环在 Luau,framework
   读不到的默认值就是 framework 拥有不了的 policy。标定时也按这张表分两批——宿主那三个
   要真机 soak 的分布数据,framework 那四个靠第一个真日常的手感。

2. workbench 共享元素展开名(`back_<page>`)直接成为 `uf.recognizers.back_main`
   这类 member key,可读性是否接受(p0b 遗留项)。
3. **最小验证门是否补一道 clang 检查。** 2026-07-29 实测发现一个结构性盲区:
   `modules/core/source/core/safety/annotations.hpp` 里的 `UF_LIFETIME_BOUND` 等
   注解按 `defined(__clang__)` 分支,MSVC 下展开为空。于是把 `UF_LIFETIME_BOUND`
   写在非成员函数的声明符位置(只有成员函数合法,它标注的是隐式对象参数)在本地
   **四个静态门 + 16/16 CI 全绿**,而 clang 下是硬错误,必挂 `linux-analysis`
   那道必需 CI 门。CLAUDE.md 的最小门是纯 MSVC,结构上看不见这一整类问题。
   两条路:①把 `cmake --preset x64-analysis`(在 MSVC 构建上开 clang-tidy,会展开
   这些注解)加进最小门;②加一个轻量脚本,用 VS 自带的 clang 对改动过的 TU 跑
   `-fsyntax-only`。我已验证第二条可行且很快(用 `compile_commands.json` 的 include
   集,单 TU 秒级),并且双向可证伪:错位置报错、对位置通过。

**风险:**

- **framework 权限隔离不彻底。** 缓解:独立 env 表无 `__index` 链、能力面只在 upvalue、
  运行期冻结规则、对抗套件、C++ 每次调用重验。
- **framework 变成过重 DSL。** 缓解:只结构化 task/step/cycle/retry/interrupt/subtask;
  step body 保持普通 Luau。
- **C++ 重新吸收 task policy。** 缓解:§1 的规则 + 原语准入规则——一个原语必须是不可
  再拆的 effect 或安全原语;若它包含页面选择、循环、重试或游戏决策,就不该存在。
- **单条 trace 的写入量。** 每观察周期若干条事件,长程日常可能很大。缓解:载荷上限 +
  按 run 分文件;真机 soak 时测量,必要时按事件类别分级。

## 十九、文档漂移修正清单

> **已完成 2026-07-29**,与本文同批提交。另发现两处清单外的问题:
> `docs/plans/README.md` 仍把 07-28 草案列为「待评审」(已改),以及
> **一处事实更正** —— S0 运行期清单的 schema id 是
> `umbraflow-annotations/v1`(`modules/annotation/source/annotation/runtime-manifest.hpp:17`),
> 升到 v2 的是**授权文档** `umbraflow-authoring/v2`(`authoring-document.hpp:27`);
> 两者是不同的 schema,此前被混为一谈。
>
> 未处理、留给开发者的治理问题:`docs/adr/` 已空,而
> `.claude/skills/improve-codebase-architecture` 仍教「读 `docs/adr/`、按需写
> `docs/adr/NNNN-*.md`」。ADR 在本仓库是否还作为决策记录格式,是治理裁决而非漂移。

修正范围:

- `CONTEXT.md` — capability 根改 `uf`;新增词条「观察周期(Observation cycle)」、
  「票据(Ticket)」、「私有能力面」;`umbra` 移入 _Avoid_。
- `docs/plans/2026-07-22-annotation-design.md` §4 — S0 拼写改
  `uf.recognizers.NAME` / `uf.pages.NAME`,加日期注记。
- `docs/plans/2026-07-21-product-form-and-roadmap.md` — 落地约束「脚本只看到
  `umbra.*`」改 `uf.*`;`:244` 指向已删 ADR 0001 的注记改为指向本文 §11 的可逆性论证。
- `docs/plans/2026-07-27-p0b-script-layer.md` — 顶部加「脚本层裁决已被本文取代」;
  §6 五个待裁决项就地标注结论(1 删 `now()`;2 engine 接缝整体删除;3 veto 轮数并入
  §18;4 `installer` 重载随能力面重构消失;5 kind 映射合并为一份)。
- `docs/plans/2026-07-21-lua-task-model-grill-decisions.md:13` — 删去指向已删 ADR 的引用。
- `docs/plans/2026-07-28-luau-first-task-system-design-draft.md` — 顶部加取代注记。
- `docs/TODO.md:114` — 删去指向已删 ADR 0001 的引用,改述为本文 §11。
- `docs/INDEX.md` — 新增本文;移除 `docs/adr/` 条目。
- `docs/reviews/2026-07-28-luau-first-draft-review.md` — 加一行「结论已并入本文」。

### 第二批(阶段 3a–3c 落地后,2026-07-29,核对至 `8b16f2d`)

阶段 3 前三批把等待循环整个搬出 C++,漂移面比根改名那次更大:知识库两个镜像都在讲一个
已经不存在的 engine 轮询循环。

- 本文 §5 / §6 / §8 / §9 / §12 / §16 / §17 / §18 — 见各节的落地更正与形状快照。
- `docs/knowledge/{cn,en}/module-engine.md` — 删 `waitForPage` / `PageWait` /
  `sweepKnownPopups` 的全部叙述;`k_maxPollSleepSlice` 的归属改 `modules/core`;
  `IFrameSource::capture()` 改 `CaptureBudget` 形态;D6 那节改写为「弹窗由 framework 的
  interrupt 注册表处理」。
- `docs/knowledge/{cn,en}/entry-cli.md` — 删 `--timeout` / `--poll`(今天是未知参数);
  证据缺口那条与 D6 接缝那条重述。
- `docs/knowledge/{cn,en}/module-script.md` — 时钟删除从「阶段 3 将做」改为已做。
- `docs/knowledge/{cn,en}/00-overview.md` — `umbra-flow run` 那一行仍写「等页面、找一个
  动作、点一次」,那是已删的 smoke flow;改为「运行 `--task NAME` 指名的 Luau 任务」。
- `docs/pitfalls/page-modeling-and-multi-step.md` — 机制从 `waitForPage` + `--timeout`
  改为 `ctx:wait_for_page`,教训保留;新增 `ctx:try` 只接 Tier B 这一条。
- `docs/TODO.md` — B1 那条流水线里的 `waitForPage` 加时点注记。
- `docs/knowledge/{cn,en}/README.md` — 新增「待补的两页」,写明 `module-task` 与
  `module-trace` 的建议范围。

未做、留给后续:那两页本身(等阶段 3d 落地再写,免得写完立刻过期);
`scripts/generate_code_atlas/data/{engine,entry-cli}.json` 与 `tour_content.py` 仍在讲
被删的 `waitForPage` 循环,需要一次 `generate-code-atlas` 重跑;
`.claude/skills/correct-doc-drift/SKILL.md` 拿「`waitForPage` 的内层轮询循环」当
doc-vs-code 矛盾的范例,而今天矛盾的方向反过来了(代码没有循环、文档还写着),
那是技能文件,属治理范围。
