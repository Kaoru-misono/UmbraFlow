# P0-B 脚本层 — 动工前裁决与执行切片

> **词汇重定向(2026-07-31)。** 本文是有日期的记录,不改写。下文的
> `recognizer` / `RecognizerId` / `uf.recognizers` / `recognizerId` 一律读作
> **element** / `ElementId` / `uf.elements` / `elementId`;`RecognizerDefinition`
> 与 `RecognizerVariant` 读作 `CompiledElement` 与 `CompiledAppearance`;
> `Variant` / `variant` 读作 `Appearance` / `appearance`。`RecognitionCatalog` 与
> `RecognitionRuntime` 名字不变——它们指的是「识别」这个动作。schema id 随改名一起动了:
> `umbraflow-authoring/v4`、`umbraflow-annotations/v3`、`umbraflow-trace/v2`。
> 权威词汇见 `CONTEXT.md` 的「Annotation model」一节。

> **2026-07-29 取代注记**:本文的**脚本层裁决(§一)与阶段 2 API 草图(§四)已被
> [`2026-07-29-three-layer-task-system.md`](2026-07-29-three-layer-task-system.md)
> 取代**,不再是实现依据。仍然有效的是**已落地工作的记录**——§三各阶段的「已完成」
> 注记(阶段 1/2/3 的提交、实测结论、对抗审查发现)是历史事实,照读。
>
> 读法要点:
>
> - capability 根 `umbra` 已改名为 **`uf`**(新文 §6/§18)。本文正文一律不改写,
>   凡作为脚本根出现的 `umbra` 读作 `uf`;`umbra-flow` 是 CLI 名字,不改。
> - 裸动词(`umbra:capture` / `umbra:click` / `umbra:wait_for_page` / `umbra:try` /
>   `umbra:now`)不再是 project 可见表面:新文 §5 改为 framework 私有的 12 个原语 +
>   §6 的 `ctx`,project 环境无裸动词(新文 §7、§16)。
> - 观察寿命不再绑 Lua GC:改为显式观察周期 + 票据(新文 §4),跨帧 seq 校验机械
>   整体删除。
> - `task-trace/v1` 与 `engine-trace/v1` 合并为 `umbraflow-trace/v1`(新文 §12)。
> - §六的五个待裁决项已在 2026-07-29 全部有结论,就地标注于该节。
> - **(2026-07-30 补注)** `RecognitionFailed` 已改名为 **`RecognitionIncomplete`**,
>   wire 名由 `recognition_failed` 改为 `recognition_incomplete`,`FailureResponse`
>   由 `StepFailed` 改为 **`Retry`**(依据:`modules/domain/source/domain/error.hpp`
>   与 `error.cpp` 的 `failureResponse`/`automationErrorWireName`)。本文 §三阶段 2 与
>   §四 Tier 映射里的 `RecognitionFailed` 一律读作 `RecognitionIncomplete`,并且**不能
>   读成「识别失败/没匹配上」**:识别跑完却没匹配上根本不是错误(那是 `UnknownPage`
>   或空命中,不带 error kind);这个 kind 只表示比较预算在搜索结束前耗尽、调用方对
>   屏幕一无所知,所以它的 retryable 默认值就是重试——重新观察,而不是把该步当已排除。
> - **(2026-07-30 补注)** 上面「framework 私有的 12 个原语」现在是 **13 个**:`ed38124`
>   加了 `key(ticket, name)`,形状与理由见新文 §5 的 2026-07-30 形状快照。同一笔还给宿主
>   加了**第二个前端** `umbra-flow drive`(`TaskHost::startOperatorSession` 与
>   `task::OperatorSession`),它是同一张私有能力面的同级消费者而不是通往 Luau 的口子,
>   一个 generation 只接受两者之一。本文 §一与 §四只描述脚本这一条路,读时不要据此推断
>   「只有脚本能驱动」;权威见新文 §5 与 §13。
>
> 状态:**已裁决,2026-07-27 grill 完成**。本文补全
> [`2026-07-21-lua-task-model-grill-decisions.md`](2026-07-21-lua-task-model-grill-decisions.md)
> 中 D1/D4/D10 的「留待」项,并给出 P0-B 的执行切片。上游权威不变:产品方向看
> [roadmap](2026-07-21-product-form-and-roadmap.md),S0 契约看
> [annotation-design](2026-07-22-annotation-design.md),实现配方看
> [integration-plan](2026-07-21-luau-integration-plan.md) 与
> [hardening-ledger](2026-07-21-p0b-luau-hardening-ledger.md)。
>
> 指导原则(开发者 2026-07-27 明确):**宿主架构、模块边界、schema 与脚本可见
> API 必须从第一天就服务最终形态(P2 托盘常驻 App、P3 第二游戏),不做
> 「先随意跑通再重构」的版本。**「P0 允许笨但能跑通」只适用于 Luau 任务脚本的
> 内容(复制粘贴写每日,P1 按 D7 重构),不适用于宿主侧。

## 一、裁决汇总

1. **capability 命名空间 = `umbra.*`**。脚本全局根对象唯一,进 AST 校验器、
   trace、错误消息与全部示例;C++ 命名空间保持 `uf::` 不受影响。旧文档中的
   `bot.*` 均已加注读作 `umbra.*`(annotation-design §4 已改写)。词条见根目录
   `CONTEXT.md`。
2. **P0 脚本句柄 = 进程内 opaque userdata**(原 ADR 0001 已删除,论证保留于
   [`2026-07-29-three-layer-task-system.md`](2026-07-29-three-layer-task-system.md)
   §11「为什么句柄不是可序列化 DTO」及 §5 的四条不变量)。
   `frame`/`page`/`detection` 是宿主拥有、脚本不可窥视的 userdata,一比一包装
   engine 冻结句柄;「`IScriptRuntime` 只传可序列化 DTO」改判为未来跨进程
   worker 接缝的要求(触发 C# worker 重评估条件时兑现)。
3. **绑定层 = 新模块 `modules/task`**,依赖 `script + engine + annotation`。
   `modules/script` 保持纯 Luau 底座(沙箱/取消/配额住这里,不沾 engine);
   `modules/task` 承载 host-api 绑定、AST 字面量校验器、Tier 错误映射、任务
   运行循环,并且是 D10 Engine API 语义(`load_project/start_task/cancel/...`)
   的长期归宿;entry 组合根只做装配。engine 永不依赖 script/task。
4. **动作动词:click 为 P0-B 核心,keyPress 为 P0-B 内次优先扩展,拖拽/滑动
   明确推迟**(开发者确认每日大部分为点击,部分可按键,拖动暂不考虑)。
   controller 层按键机械(keyPress/keyDown/keyUp/longPress + 按住账本)已存在,
   增量在 engine `IActionSink` 端口与授权语义。**硬约束:端口与授权模型按
   action kind 参数化塑形,新增动词是加法,不改造既有链路。**
   非坐标动作(按键)要求的授权证据模型是 key 扩展动工前的前置设计项——
   底线:仍需页面证据 + fingerprint/generation 校验 + 租约,只是不绑定像素。
   若 P0-C 逐页分解发现全部按键步骤有点击等价物,key 扩展可后滑、不阻塞 B3。
5. **generation 热加载 P0 只做加载边界语义**:新 generation 编译+自检成功才
   原子安装,失败不影响已装载 generation;每 generation 全新 VM,新旧 closure
   混用在构造上不可能。一票否决 #5 按此口径验收(roadmap 已加注)。运行中途
   活体热切推 P2——generation 机械(校验→原子安装)作为独立接缝住在
   `modules/task`,P2 只是增加「任务安全点触发同一套安装」。
6. **错误暴露 = `umbra:try(fn)` 受控捕获入口 + 保留原生 `pcall`**。`try` 只捕
   Tier B 自动化结构化错误,脚本自身 bug 穿透;`pcall` 不删(语言习惯,且
   Tier C 经 `lua_break` 对其免疫)。`wait` 超时归 Tier B 独立 kind,与 Tier C
   的预算耗尽分开,不合并。
7. **任务归属项目、名字寻址**(原 ADR 0002 已删除,论证保留于
   [`2026-07-29-three-layer-task-system.md`](2026-07-29-three-layer-task-system.md)
   §6「任务寻址」):
   任务住在项目 `tasks/` 目录,`umbra-flow run --project <dir> --task <name>`,
   宿主载入时算脚本内容 hash、连同 compiler 版本写入 trace。CLI 永不执行游离
   路径脚本。P1 D7 的任务清单与 P2 App 的任务枚举都是这一结构的加法。

## 二、被「最终形态」原则升级为 P0-B 承诺的项

- **`IFrameSource::capture()` 端口补 deadline/stop_token**。「宿主调用必须有界」
  是终态契约(integration-plan §5 Risk #1);不补则 500ms 硬取消永远有一个
  不可抢占的 capture 缺口,且拖得越晚返工面越大。
- **脚本级 trace 从第一天就是正式版本化 schema**(`task-trace/v1`:seed、脚本
  hash、compiler 版本、host-API 返回、generation 事件),不往 `engine-trace/v1`
  塞临时字段;两条 trace 流并存,各自 golden 测试。
- **每任务全新 VM 在阶段 1 就纠正**。当前 `modules/script` 的持久 Engine 复用
  主 `lua_State`(globals 跨 `runNumber` 泄漏)与 D9「每 run 全新 lua_State」、
  D10「每任务全新 VM generation」相悖,属阶段 1 返工项,不留到以后。
- **单一取消源**:Ctrl-C(及未来 `Engine.cancel`)驱动同一 `std::stop_source`;
  经 `std::stop_callback` 桥接置 Luau interrupt 读的原子标志,engine 侧继续
  消费同一 token——不做两套取消。

## 三、执行切片

### 阶段 1 — 沙箱 + 硬取消 + veto 套件(integration-plan §6 step 3+4,同批 TDD)

> **已完成(2026-07-27 夜,提交 `e4e1e23` + `29ac46b`)**。全门绿:格式/模块/
> 安全脚本 + x64-debug 构建 + 14/14 CI。落地范围与下列清单一致,另加两处对抗
> 审查暴露的加固:deepFreeze 连 metatable 一起冻(否则改写 `__index` 即可绕过
> 冻结表,已用「撤掉修复→测试失败」验证可证伪),以及 `runNumberOnThread`
> 统一按 `InterruptState::broken` 判定取消(不可 yield C 帧里的取消会以普通
> 运行时错误浮现,否则会被误判成 Tier B 脚本错误)。取消的可捕获性边界见 §四。
> 常数仍是占位:内存 64 MiB、指令预算 1e8 ticks、maxRuntime 30 分钟。
> 已知残留(记账,不阻塞):500ms 计时断言在超载 CI 机上有抖动风险;
> 200 代创建/销毁用例只断言脚本结果、未断言内存指标(memory-probe 的 residual
> 可用于加固);host binding 内 hang 的 veto 子用例待阶段 2 有真 binding 后补。

全部在 `modules/script` 内,无未决依赖,可立刻动工:

- `sandbox.*`:建序 openlibs → 注册+递归 deepFreeze 宿主表 → nil 掉
  `getfenv/setfenv/newproxy/coroutine/debug` → `luaL_sandbox` → 每任务
  `lua_newthread` + `luaL_sandboxthread`;os.time 处理按 D9(宿主替代)。
- `cancellation.*`:atomic cancel 标志 + interrupt 回调(先 `if (gc >= 0) return;`)
  + `lua_break` yield-and-abandon;绝不 `luaL_error`(硬红线,spike 已实测)。
- 记账 allocator + 每任务内存硬配额;指令预算 + `max_runtime` 时间预算
  (常数先保守占位,首个真实任务标定)。
- per-task VM 生命周期纠正(见 §二)。
- veto 套件进 CI:6 条一票否决 + 不可 yield C 边界用例(`table.sort` 比较器、
  `string.gsub` 回调、宿主 C 函数内 hang)+ 嵌套宿主表不可写 + 危险全局缺席
  断言。veto 测试与其守护的实现同批提交,Luau 升级纪律自此可执行。

### 阶段 2 — `modules/task` 绑定层(step 5)

> **已完成(2026-07-27 夜,提交 `6e78353`..`ab0e3a0` 共 6 笔)**。全门绿:4 个静态
> 门 + 15/15 CI(新增 `test-task`)。下列清单全部落地,另加实现期定的两处:
> ①**跨帧校验**——每个句柄记录它源自哪次观察,`click` 混用两帧的 page 与 hit 在
> 绑定层即拒(不让错配证据走到 engine 授权);②**观察寿命绑定 Lua 句柄**——frame
> 句柄的 GC 析构释放它 pin 住的观察,并加「达上限先强制回收再重试、仍超才报错」
> 的护栏,否则最普通的轮询循环每轮泄漏一整帧(~8MiB,在 C++ 侧、Luau 配额看不到)。
> **对抗审查抓到一个真沙箱逃逸**:`_G` 未被 nil(`luaL_sandbox` 只冻结不移除),
> 于是 `_G.umbra:capture()` 既过 AST 校验(校验器只认字面 `umbra` 根)又在运行时
> 可用 —— 已两层修复(运行时 nil `_G` + 校验器拒绝 `_G` 起头的链),7 种绕过形态
> 各配一条回归。

- 新模块 `modules/task`(manifest:public = core domain annotation engine script)。
- runtime manifest → `umbra.recognizers/pages` 递归只读句柄表;Luau AST 校验器
  在 VM 创建前 100% 枚举字面量引用(annotation-design §4)。
- userdata 包装:`umbra:capture()`/`frame:resolve_page()`/`frame:find()`/
  `umbra:click()`/`umbra:wait_for_page()` 一比一映射 EngineSession 语义,
  act 后作废、四要件授权、fail-closed 全部保全;`__metatable` 保护。
- Tier A/B/C 映射:空 optional→`nil`;StaleObservation/Timeout/ActionRejected/
  RecognitionFailed→可捕获结构化错误(retryable 默认值在此定表);
  Cancelled/预算耗尽→特判走 Tier C 不可吞路径。`umbra:try` 落地。
- 任务 sourcing:`tasks/` 目录 + 名字寻址 + hash 入 trace(原 ADR 0002,现见
  [`2026-07-29-three-layer-task-system.md`](2026-07-29-three-layer-task-system.md) §6);
  `umbra-flow run` 改为 `--project --task` 形态。
- API 草图先行:动工前出一版 `umbra.*` 表面签名清单(含 raw find 暴露与命名
  细节)供开发者过目,再写绑定。

### 阶段 3 — B2 收尾

> **已完成(2026-07-27 夜,提交 `82e870c`..`acffe04` 共 4 笔)**。全门绿。
> **`umbra:now()` 是逻辑序数,不是墙钟** —— 对抗审查发现初版直接返回真实经过
> 毫秒(类名叫 Deterministic 却 run-to-run 漂移,脚本一旦基于它分支即破 veto #4),
> 已改为「每次读取推进一 tick」的纯逻辑量。**语义待开发者复核**,见 §六。
> veto #4 harness 跑 **300 轮**(非 1000,CI 预算所限;常数一行可放大,测试注释
> 如实写明跑的是哪个数),并配**反向对照**(两个 seed 断言输出不同)防止空洞。
> **D6 清扫钩子按实现方判断未加**:理由是 engine 的 `waitForPage` 内层轮询才是
> 「每观察周期」的正确落点,task 侧钩子只会在一次 task 级 wait 起点触发、看不到
> 内层轮询;故 D6 归宿仍是 engine 既有 no-op seam。**此判断待复核**(见 §六)。
> `capture()` 端口的 deadline/stop_token 未补,顺延。

- `umbra:now()` 逻辑时钟 / `umbra:random()` 固定算法 RNG(seed 入 trace);
  有序遍历纪律(禁把无序迭代结果发出到 trace/state)。
- `task-trace/v1` schema + golden 测试;确定性回归 harness(可复用
  Fake Controller + 静态截图 golden——目前不存在,新建)。
- veto #4 跑通:同 observation trace + seed 1000× 同机复现。
- `sweepKnownPopups` 从 no-op 变最小 D6 清扫(观察周期边界 + 长 wait 内)。
- key 动作扩展(若 P0-C 分解确认需要):IActionSink 新 kind + 非坐标授权证据
  设计 + 授权链/trace 词汇扩展。
- `capture()` deadline/stop_token 端口补齐(可与阶段 1/2 并行,谁先方便谁做)。

之后进入 B3/P0-C(整套每日、遮挡/最小化/CaptureStalled、长程),不在本文范围。

## 四、阶段 2 API 草图(2026-07-27 夜间定稿,待开发者复核)

拼写与 S0 annotation-design §4(已修订为 `umbra`)完全一致,新增部分标注:

```lua
local frame = umbra:capture()                 -- Tier B 失败抛结构化错误
local outcome = frame:resolve_page()          -- PageOutcome userdata
local page = outcome:resolved()               -- ResolvedPage | nil(Unknown/Ambiguous 已入 trace)

if page ~= nil and page:is(umbra.pages.main) then
    local hit = frame:find(umbra.recognizers.battle)  -- ActionFound | nil(Tier A)
    if hit ~= nil then
        umbra:click(page, hit)                -- 消费 frame 的观察,之后 frame 上任何操作 = StaleObservation
    end
end

-- 新增:证据等待(engine waitForPage 的一比一暴露;返回值携带命中该页的观察)
local wait = umbra:wait_for_page(umbra.pages.main, { timeout_ms = 600000 })
-- wait.page / wait.frame,超时抛 Tier B TimedOut(独立 kind)

-- 新增:受控捕获(只捕 Tier B 自动化错误,脚本 bug 穿透)
local ok, err = umbra:try(function()
    umbra:click(page, hit)
end)
-- err 为 frozen table: { kind = "stale_observation", message = ..., retryable = ... }

-- 阶段 3 追加:umbra:now()(单调逻辑毫秒)、umbra:random()(种子入 trace)
```

Tier 映射:`find` 未命中 → `nil`;StaleObservation / TimedOut / ActionRejected /
RecognitionFailed → 可捕获结构化错误(retryable 默认值实现时定表);
Cancelled / 预算耗尽 → `lua_break` 路径。

> **取消可捕获性的精确边界**(2026-07-27 阶段 1 实测,勿再写成「任何捕获手段
> 不可见」):纯 Luau 代码里的 `lua_break` 不是 Lua 错误,`pcall` 结构上无法
> 捕获,脚本也绝不会执行到被中断点之后(有 `mark()` 判别器测试守着)。**窄例
> 外**:中断发生在不可 yield 的宿主 C 帧内(`table.sort` 比较器、`string.gsub`
> 回调)时,Luau 抛的是可被 `pcall` 捕获的 "break across C-call boundary" 普通
> 错误。宿主对此的兜底是 `InterruptState::broken`:`runNumberOnThread` 一律据
> 它判定为 `Cancelled`,VM generation 就此作废——**脚本能捕获那一次错误,但
> 拿不回控制权,也无法让任务继续**。阶段 2 每个 host binding 必须及时返回,
> 才能让这条窄例外始终停在 500ms 预算内。

**留给开发者复核的三点**:①workbench 共享元素展开名(`back_<page>`)直接成为
`umbra.recognizers.back_main` 这类 member key,可读性是否接受;②**不提供**
`umbra:sleep_ms`——推荐一切等待走 `wait_for_page` 证据等待(裸 sleep 破坏
可复现性且掩盖页面状态假设),若真实每日流程出现"必须无证据等待"的场景再议;
③`wait_for_page` 返回 `{ page, frame }` 双字段形态是否顺手。

## 六、夜间实现后新增的待裁决项(2026-07-27)

> **五项已于 2026-07-29 全部裁决**,结论就地标注在每项之下,依据是
> [`2026-07-29-three-layer-task-system.md`](2026-07-29-three-layer-task-system.md)。
> 原问题文字保留为历史。

1. **`umbra:now()` 的语义**。现在是「每次读取 +1ms」的逻辑序数,完全可复现,但
   与真实时间无关:作者写 `until umbra:now() - t0 > 5000` 得到的是「循环 5000 次」
   而非「等 5 秒」。三条路可选:①保持现状 + 文档强调它不是墙钟(推荐:真正的等待
   一律走 `wait_for_page` 的宿主超时,脚本本就不该自己数时间);②改为随观察事件
   推进(有语义但仍非真实时间);③暴露真实墙钟并显式排除在确定性契约外。
   决定前 `now()` 只适合做单调守卫,不适合做超时。
   > **裁决(2026-07-29):三条路都不选,动词从脚本表面整体删除。** `now()` 及
   > `DeterministicClock` 的全部绑定删除,脚本永远读不到任何形式的时钟。替代是
   > 基于证据的等待 `ctx:wait_for_page(page, { timeout_ms })` 与声明式有界停顿
   > `ctx:settle(ms)`(后者进 trace、时长是重放输入的一部分)。见新文 §10,
   > 删除清单见 §16。
2. **D6 最小清扫的落点**。实现方判断 task 侧钩子位置错误、应留在 engine 的
   `sweepKnownPopups`(但那属 `modules/engine`,本轮禁改)。若认可,P0-C 动工时
   直接改 engine;若认为 task 侧也需要一层,需说明它要看见哪些观察周期。
   > **裁决(2026-07-29):问题消失——那个接缝整体删除。** 开发者已准许改动
   > `modules/engine`:`EngineSession::waitForPage`、`PageWait` 与
   > `sweepKnownPopups` 全部删除(能力层里不该有 policy 循环)。弹窗 interrupt
   > 注册表迁入 Luau framework,由 framework 在每个观察周期边界跑。见新文 §3、
   > §16、§17 阶段 3(该阶段的出口即「弹窗-长等待缺口关闭」)。
3. **veto #4 的轮数**。CI 跑 300;是否需要一个 nightly/手动的 1000 轮门。
   > **状态(2026-07-29):仍未裁决**,已并入新文 §18「待你裁决」的常数标定项,
   > 与内存/指令预算/`max_runtime`/轮询节奏/`max_action_frame_age` 一起,由首个
   > 真日常 + 真机 soak 标定。
4. **`installer()` 的两个重载**。无参版只装资源表、不装动词,目前只有测试用;
   生产误用会得到一个没有动词的 `umbra` 表。是否收窄为单一重载。
   > **裁决(2026-07-29):问题消失——能力面整体重构。** 表面拆成 framework 私有
   > 能力面(12 个原语,只以闭包 upvalue 持有)+ 冻结的 project 环境(`uf.pages` /
   > `uf.recognizers` / `uf.task` / `uf.errors`),两个环境按 `luau_load` 的 env
   > 索引隔离,不存在「装不装动词」这个选择。连带:`HostTableInstaller` 必须改成
   > 返回 `Status`(今天是 `void`,坏 bundle 无法让 generation 失败)。见新文
   > §5、§7。
5. **`snakeName` 与 `errorKindWireName` 两处重复的 kind→wire 名映射**(绑定层与
   trace 层各一份,注释要求二者恒等但无共享真相、无一致性测试)。建议补一条断言
   两表相等的测试,或合并到一处。
   > **裁决(2026-07-29):合并,不是加测试。** 两份映射合并为**一个** C++ 函数,
   > `AutomationErrorKind` 是唯一真相;Python 生成器解析该 enum 产出 Luau 的
   > `uf.errors` 表,并加一条断言生成表覆盖 enum 每个取值的测试。不生成 C++ enum。
   > 见新文 §9,落地在 §17 阶段 1。

## 五、遗留待定(记账,不阻塞动工)

- 常数标定:interrupt 回调频率/指令预算、`max_runtime` 默认、allocator 配额、
  `max_action_frame_age` 生产值——保守占位,首个真实任务 + 真机 soak 标定。
- D9 确定性地板逐项(`setmetatable`/`raw*`/`next`/`tonumber`、integer/float
  边界、地址型 `tostring` 消毒):阶段 1 按「能禁则禁」的保守默认落,逐项在
  hardening-ledger 记录。
- OCR/`info_region`:P0-C 分解时按 annotation-design §7 契约裁决,默认全
  `gray_template`。
- 卡厄斯梦境每日流程分解、接管起点、运行分辨率/DPI(roadmap §四待开发者输入)。
- workbench「共享 action target」在 Save+Generate 时展开为每页 recognizer
  (形如 `back_<page>`),对 `umbra.recognizers.<name>` 脚本表面的可读性影响
  ——2026-07-25 UI 重设计时已标注「P0-B 落地时复核」,归阶段 2 API 草图评审。
