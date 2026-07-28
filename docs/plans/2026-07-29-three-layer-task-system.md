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

连锁简化三处:

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

只有 framework 能拿到,以闭包 upvalue 形式持有,**永不作为任何表的键**。

```text
-- 观察周期
cycle_open(deadline)              -> ticket | raise
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

四条不变量(它们同时是 §11 可逆性的条件):

1. 纯粹:只有效果,不回调进 Lua。
2. 入参只有宿主 mint 的句柄和标量。
3. 返回只有宿主 mint 的句柄、标量、或错误 userdata。
4. 每个原语有界且感知 stop token。

`wait` 由现有 `engine::pollSleep`(`session.cpp:56-79`)的切片实现搬来。
`cycle_open` 的 deadline 要求 `IFrameSource::capture()` 补 deadline/stop_token
——这是 P0-B 已承诺、至今未做的项,在此正式排进阶段 3。

## 六、脚本表面

根为 `uf`,单一全局根。校验器锚定 `AstExprGlobal`,所以根必须是全局。

```lua
uf.pages.<name>          -- 冻结句柄
uf.recognizers.<name>    -- 冻结句柄
uf.task                  -- define / interrupt / import(P1) / backoff
uf.errors.<kind>         -- 错误 kind 常量,由 AutomationErrorKind 生成
```

project 环境**没有**裸动词。`ctx` 作为参数传入 `run`。

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

## 七、环境与信任分层

**环境隔离按闭包,不按线程。** `luau_load` 收 env 索引(`lvmload.cpp:787`);新线程的
`gt` 是从父线程复制的(`lstate.cpp:121`),所以 `luaL_sandboxthread` 那套代理形状
**不能**用来做 project 环境——那正是 `_G` 逃逸的形状。

boot 顺序:

```text
建配额 VM
-> 开放准入的基础库
-> C++ 构造 framework env 表,把私有能力面装进 framework 闭包的 upvalue
-> luau_load(framework bundle, env = framework env),执行,冻结 exports
-> nil 掉危险全局,luaL_sandbox
-> C++ 构造 project env 表:显式白名单,【没有】__index 链指向 framework env 或主 globals
-> 把 uf.pages / uf.recognizers / uf.task / uf.errors 装进 project env 并冻结
-> luau_load(project module, env = project env)
-> 把 descriptor 交给 framework runner
```

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

删掉的是 `uf:try` 的 C 绑定,不是 `markFatal`/`guardFatal` 的语义。

verto 第 6 条(人为阻塞每个长耗时 binding,验证总退出仍在预算内)进 CI:12 个原语
逐个注入阻塞。这条 roadmap 一票否决至今没跑过。

## 九、错误

**载体是宿主 mint 的 userdata,不是表。** project 伪造不了、改不了;C++ 按 tag 解码而
不是鸭子类型。`table.clone` 伪造问题被结构性解决,不依赖 `__metatable` 兜底。

**kind 表只有一份真相。** `AutomationErrorKind`(C++)是真相:

- C++ 侧的 kind→wire 名映射**合并为一个函数,住在 `modules/domain`**。domain 拥有
  这个 enum,而 task 与 trace 都依赖它,所以那是唯一一个两边都不必互相依赖的家。
  合并前有两份(`umbra-tables.cpp` 的 `snakeName` 与 `trace/event.cpp` 的
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

**retry 语义**(修掉草案的自相矛盾):`on` 列表对它点名的 kind **覆盖** `retryable`;
`on` 缺省时 `retryable` 是默认。

必须这样,因为 `retryable` 直接由 `FailureResponse` 推导,只有 `CaptureStalled` 和
`StaleObservation` 是 `Retry`,而 `Timeout` 和 `TargetUnavailable` 都是 `Abort`
(`domain/error.cpp:80-88`)。若 `retryable` 是硬过滤,「等页面,游戏慢了就整步重来」
——日常脚本最常见的写法——永远不可能重试。

不去改 `domain/error.cpp` 里 `Timeout` 的分类:那会改变整个 engine 的
`failureResponse`,为脚本层方便动它不划算。

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

**二、它引入一个静默失败模式。** 实测的不可 yield 位置:`__index` 是函数、
`__tostring`、`table.sort` 比较器、`string.gsub` 回调、generic-for 迭代器函数、
**`xpcall` 错误处理函数**。最后一种不报错——run 返回 OK,值是
`"error in error handling"`,原始错误和请求一起消失。直接绑定可以从任何地方调用,
没有这一类。

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
task.native_call         序号 / 原语 / 入参身份 / outcome / error kind
```

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
4. `scripts/check_luau.py`:用 vendored 的 Luau 编译器对 `modules/task/runtime/` 和
   `tests/task/runtime/` 下所有 `.luau` 做语法门,并入最小验证门。

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

### 真机验收

完整一轮日常;全程严格后台;click 后旧票必死;**长等待中弹窗被处理**;Ctrl-C 达
SLA;一条 trace 足以解释每一步。

## 十六、删除清单

放开重构后明确要删的东西。列出来是为了实施时不犹豫。

**modules/engine**

- `EngineSession::waitForPage`、`PageWait`、`sweepKnownPopups`——能力层里不该有 policy
  循环,那个循环还捎带一个永久 no-op 的弹窗接缝。
- `pollSleep` 迁移到 task 的 `wait` 原语。
- 相应测试。

**modules/task**

- frame-box 的 GC 析构释放路径、`guardObservationBudget`、seq 跨帧校验机械
  (`umbra-tables.cpp` 里约 300 行),以及 `TaskContextConfig::maxLiveObservations`
  这个旋钮本身(见 §4 的修订:上限恒为 1)。
- `DeterministicClock` 及 `now()` 的全部绑定。
- `task-trace/v1` 作为独立 schema。
- `uf:try` 的 C 绑定(语义由纯 Luau 承接,`markFatal`/`guardFatal` 保留)。
- `TaskContext` 作为单个类:拆成票据账本 + 私有能力面。
- 两份 kind→wire 映射中的一份。
- project 环境里的全部裸动词。

**entry/cli**

- `runScriptFlow` 的生命周期部分(搬进 `TaskHost`)。
- `runSmokeFlow` 与 `--page`/`--action`(**2026-07-29 开发者裁决:删**)。`--task` 已
  覆盖其用途。连带:`ExitCode::ActionAbsent` 失去唯一来源,随之删除;`args` 的互斥
  校验简化为只认 `--task`;B1 smoke 的真机验证路径改由一个最小 `.luau` 任务承担,
  在阶段 4 一并落地。

**docs**

- `2026-07-28-luau-first-task-system-design-draft.md` 标记为已被本文取代。
- `2026-07-27-p0b-script-layer.md` 的脚本层裁决部分标记为已被本文取代。

预计 `modules/task` 现有 3443 行第一方代码约六成原样存活;
`tests/task/test-task-binding.cpp`(882 行)大半按周期语义重写。

## 十七、实施阶段

排序依据不再是「不丢弃任何东西」,而是:先做后面全部依赖的地基,再尽早关闭真正的
能力缺口,再尽早上真机。

### 阶段 1 — 地基

- 合并为单条 `umbraflow-trace/v1`(先做,因为之后每个阶段都往里发事件)。
- `.luau` 构建管线:`embed_luau.py` + manifest key + `check_luau.py`。
- 合并 kind→wire 映射为一份;生成 `uf.errors`;加覆盖性测试。
- `TaskHost` 立起 D10 动词形;CLI 收缩;注入每 run 种子。

出口:全门绿,现有任务仍按老表面跑通。

### 阶段 2 — 周期协议 + 两个环境 + `uf` 根

- 票据账本 + 5 个周期原语;删 GC 释放路径、`guardObservationBudget`、seq 机械。
- `HostTableInstaller` 改返回 `Status`。
- C++ 白名单 project env + `luau_load` env 分离 + C 侧 `lua_setfenv`;私有能力面进
  upvalue;裸动词从 project 环境移除。
- 错误改宿主 mint 的 userdata;删 `uf:try` 的 C 绑定。
- 根 `umbra` → `uf`:校验器、能力面、错误消息、S0 §4、CONTEXT.md、全部示例。
- 运行期冻结规则(构造时冻结、`__metatable`、`__index` 是表)。
- 对抗套件扩充。

出口:全门绿 + 对抗套件绿。

### 阶段 3 — framework 承接 task policy

- `deadline` / `wait` / `settle` 原语;`IFrameSource::capture()` 补 deadline/stop_token。
- framework Luau:`ctx:step` / `ctx:cycle` / `ctx:wait_for_page` / `ctx:retry` /
  `ctx:try` / interrupt 注册表。
- 删 engine 的 `waitForPage` / `PageWait` / `sweepKnownPopups`。
- 语义事件 + C++ 校验状态机 + 载荷上限。
- framework Luau 单测(fake 能力面)。
- 一票否决第 6 条进 CI。

出口:**弹窗-长等待缺口关闭**;全门绿。

### 阶段 4 — 第一个真日常(P0-C)

- 卡厄斯梦境日常写成单个 `.luau`,允许复制粘贴。
- 真机验收全项。

出口:真机通过。**这一阶段决定阶段 5 要不要做。**

### 阶段 5 — 跨文件复用(P1,仅当阶段 4 证明需要)

- `uf.task.import("name")` 走 `tasks/modules/` 下**同一套** allowlist;每源 hash 入
  `run.started`;module cache 与环检测在 framework 的 Luau 里。
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

**待你裁决:**

1. 常数标定:内存 64 MiB、指令预算 1e8、maxRuntime 30 分钟、`wait` 默认轮询节奏、
   `settle` 上限、`max_action_frame_age` 生产值——目前全是保守占位,首个真日常 +
   真机 soak 标定。
2. `ctx:settle` 的时长上限,以及超限是 Tier B 还是 framework 不变量失败。
3. workbench 共享元素展开名(`back_<page>`)直接成为 `uf.recognizers.back_main`
   这类 member key,可读性是否接受(p0b 遗留项)。
4. **最小验证门是否补一道 clang 检查。** 2026-07-29 实测发现一个结构性盲区:
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
