# modules/engine 架构知识

本文说明 `modules/engine` 已经实现的运行时编排。设计背景见
`docs/plans/2026-07-23-engine-architecture.md`；帧语义、租约和错误追踪的相关决策见
`docs/plans/2026-07-21-lua-task-model-grill-decisions.md` 的 D0、D1、D4。尚未落地的
能力集中列在文末。

## 模块职责

`modules/engine` 是产品运行时的编排层。它把已发布的标注项目变成可执行的识别运行时，从已绑定到目标实例的帧源取得画面，在同一帧上解析页面和动作目标，调用 annotation 的授权策略，最后把已授权的 client-space 点击交给输入端口。

它拥有四类产品语义：

- 发布物读取：`loadRuntimeProject` 读取 runtime manifest 与其引用的模板资产，构造 `annotation::RecognitionRuntime`。
- 单帧决策：`EngineSession::observe` 产生 `Observation`；页面解析与动作查找都读取该句柄持有的同一 `Frame`，不会隐式再截图。
- 坐标动作交付：`EngineSession::act` 把页面证据、动作证据、帧 identity、lease、live fingerprint 和目标实例复核串成一条 fail-closed 路径。
- 运行证据：`TraceEvent` 与 `serializeTraceEvent` 定义稳定、带版本的 JSON 记录；`ITraceSink` 把持久化策略留给组合根。

engine 不负责以下工作：

- 不发现窗口、不创建 WGC、不调用 Win32 输入 API。`modules/engine/manifest.txt` 只依赖 `core`、`domain`、`annotation`，没有 `controller`，因此 engine 可以在非 Windows 主机和离线 CI 中构建、测试。
- 不决定具体平台如何严格后台投递。它通过 `IActionSink` 写下契约；Windows 实现位于 `entry/cli/platform/controller-action-sink.hpp`，平台能力仍由 controller 所有。
- 不拥有目标选择、DPI awareness、窗口 geometry 或 Ctrl-C 注册。这些组合职责位于 `entry/cli/run-windows.cpp`。
- 不编辑或发布 annotation 项目，也不读取 authoring document。它只读 `generated/annotations.runtime.toml` 与该 manifest 闭包内的 runtime 模板。
- 不实现 Luau 宿主、任务队列、pause/resume、事件订阅或常驻进程生命周期。`modules/script/source/script/engine.hpp` 中现有 `uf::script::Engine` 仍是独立的最小 Luau 执行器，当前没有依赖或绑定 `uf::engine::EngineSession`。
- 不把识别算法和授权规则复制进自身。页面/动作识别由 annotation runtime 执行，页面允许关系、fingerprint 与 frame identity 的授权由 annotation 校验。
- 不定义 trace 文件格式之外的存储策略。engine 的 serializer 不做 I/O，也不追加换行；JSONL 的打开、逐条写入和 flush 位于 `modules/trace/source/trace/file-sink.cpp`。

严格后台输入只能由平台层实现，识别和授权则必须能在没有桌面、没有 HWND 的 CI
中复现。为此，engine 只依赖几个窄端口；测试可以提供替身实现，平台代码也能保持
简短、便于审查。

## 运行时流程

### 端口

公开端口集中在 `modules/engine/source/engine/ports.hpp`。

- `IFrameSource::capture(CaptureBudget const&) -> Result<Frame>` 从一个已经绑定的目标取得帧。`CaptureBudget` 是嵌套在该端口里的 `{ deadline, cancellation }`：capture 是 engine 唯一可能阻塞在外部生产者上的操作，没有这个界限，一个等合成器的适配器就自己决定了调用方要等多久，而一次被取消的 run 会卡在帧池里。两个成员都承重，适配器必须都兑现。deadline 是**绝对时刻**而不是时长，所以一个已经花掉一部分预算的调用方无法悄悄续期；它没有默认值，每个构造点都得说出自己施加的界限。
- `IFrameSource::validateTargetInstance() -> Status` 复核绑定的仍是同一个目标实例。engine 在 capture 前调用一次，在 delivery 前再调用一次，后者用于关闭“观察后 HWND 被复用或目标被替换”的窗口。
- `IActionSink::click(Point<ClientSpace>, ObservationLease const&) -> Status` 接收 client 坐标和原始 lease。lease pass-through 是接口的一部分，适配器不能只传坐标，否则 controller 层无法执行第二层 session/generation/age fencing。
- `IActionSink::pressKey(KeyName, TargetGeneration) -> Status` 投递一次按下并释放。它收的是 `TargetGeneration` 而 `click` 收的是 lease，**这个差别就是两个动词之间授权上的全部差别**：lease 围栏的是*坐标*，它的 `frameId` 与年龄之所以存在，是因为布局一动，同一个点击点就悄悄变成了别的意思；而按键不指名任何点，没有任何矩形的位置会因此过时，也没有东西要靠帧年龄去保护。仍然必须成立的是这次按键落到 observation 来自的那个目标实例上，而 generation 携带的正是这件事。实现**必须**把这个 generation 转发下去，让 controller 的复验仍在 post 时发生；**必须**严格后台投递；**绝不**抢焦点或激活目标窗口。2026-07-30（`ed38124`）落地。
- `ITraceSink::emit(TraceEvent const&) -> Status` 是同步、可失败的证据端口。失败不是 best-effort warning，而会中止当前 engine 操作。

三个端口都不可复制、不可移动，由虚析构支持组合根提供实现。`EngineSession` 通过 `std::unique_ptr` 独占它们，因而端口实现和其中的平台资源与 session 同寿命。

Windows 产品入口使用两个薄适配器：

- `entry/cli/platform/wgc-frame-source.hpp` 的 `WgcFrameSource` 拥有 `WgcCaptureSession`，两个方法分别直接转发 `capture` 和 `validateTargetInstance`。
- `entry/cli/platform/controller-action-sink.cpp` 的 `ControllerActionSink` 拥有 `DeliveryTarget`、`HeldInputs`、`AuditLog`，把 lease 原样传给 controller 的 `uf::click`。失败后调用 `releaseHeld` 补偿可能残留的按下状态，并保留原始错误。

### 加载运行时项目

入口是 `modules/engine/source/engine/runtime-loader.hpp` 中的 `loadRuntimeProject(projectRoot) -> Result<LoadedRuntime>`。真实路径如下：

1. 拼出 `generated/annotations.runtime.toml`。
2. `readCappedFile` 先用 `file_size` 快速拒绝明显超限文件，再以 64 KiB chunk 实际读取；实际字节数仍受限，因此文件在 stat 后增长也不能绕过上限。
3. manifest 上限由 `k_maximumRuntimeManifestBytes` 固定为 16 MiB。
4. 文本交给 `annotation::parseRuntimeManifest`；engine 不另写 TOML parser。
5. 按 manifest 的 `assets()` 顺序读取引用文件。同一 `ContentHash` 只加载一次；未被 manifest 引用的磁盘文件被忽略，允许 content-addressed store 保留历史。
6. 模板读取上限为 64 MiB。该常数在 `modules/engine/source/engine/runtime-loader.cpp` 中有意镜像 image 模块的 PNG 上限，因为 image 是 annotation 的 private dependency，engine 不能越过模块边界 include 它的 header。
7. `annotation::RecognitionRuntime::create` 接收 manifest 与 `EncodedRuntimeTemplate`，负责 hash closure、内容 hash 与 PNG 解码校验。
8. 成功后返回仅含有效 `RecognitionRuntime` 的 `LoadedRuntime`；该类型没有可观察的半初始化状态。

因此 manifest 是 runtime 的读取权威。当前路径不读取 `project.toml`，也不扫描目录猜测资产；这使“发布 commit point 指向什么”与“运行时实际加载什么”保持一致。

### 会话与 Observation

公开运行表面位于 `modules/engine/source/engine/session.hpp`：

- `EngineSessionConfig` 固定 live `ProjectFingerprint`、每次识别的 pixel comparison budget、recognition timeout、最大动作帧龄和共享 `std::stop_token`。
- `EngineSession::create` 要求三个端口都非空，保存 `LoadedRuntime` 与配置，并首先 emit `SessionStarted`。首条 trace 写入失败时 session 不会创建成功。
- `EngineSession::observe() -> Result<Observation>` 先检查 cancellation，再复核目标实例、capture、由 `ObservationLease::forFrame` 建 lease、提取 `annotation::FrameIdentity`，emit `Observed` 后才把句柄交给调用者。**capture 的 deadline 在这里铸**：由配置里那一个 `captureTimeout`（`k_defaultCaptureTimeout = 2s`）加上当前时刻算出，连同 session 自己的 cancel 源一起装进 `CaptureBudget`。所以没有任何适配器自行决定一次观察能阻塞多久，也没有任何脚本能放大它——溢出单调时钟是配置错误，当场 fail closed。
- `EngineSession::resolvePage(Observation const&)` 对传入 observation 持有的 frame
  调用自身 `RecognitionRuntime::evaluatePage`，返回由 `ResolvedPage`、
  `UnknownPage` 或 `AmbiguousPages` 组成的 `PageOutcome`。
- `EngineSession::findAction(Observation const&, RecognizerId)` 对同一 frame 调用
  `evaluateActionTarget`。未命中是 `Result<std::optional<ActionFound>>` 的成功空值，
  对应 D4 Tier A，不是错误。
- `ActionFound` 保存原始 `AnchorEvidence`、绑定 recognizer identity 的 `ActionDetection` 和确定性的 `PixelPoint`。点击点由 annotation 的 `resolveClickPixel` 决定；match rect 经 `pixelRectToFrameRect` 变成 authorization-ready `Detection`。
- `EngineSession::act(Observation&&, ResolvedPage const&, ActionFound const&)` 消费 observation，成功返回记录被授权 `FrameId` 与 client-space 坐标的 `ActReceipt`。
- `EngineSession::pressKey(Observation&&, KeyName)` 同样消费 observation，返回 `KeyReceipt`——
  `{ frameId, key }`，没有点，因为按键本来就没有点；这也正是它是一个独立回执、而不是一个带着
  杜撰坐标的 `ActReceipt` 的原因。

  它与 `act` **共有**这些：请求过停止时，任何 sink 调用之前就拒绝；来自另一个 session 的
  observation 是 `InternalInvariant`，已作废的是 `StaleObservation`；post 之前立刻复验绑定的
  目标实例；observation 被花掉，因此一个 observation 至多投出一次输入——按键与点击一样会改变
  屏幕，活过它的那一帧描述的是一个已经不存在的目标。

  它**有意不共有**两件事，因为按键不指名任何屏幕位置。一是没有 page 授权、没有同帧 detection：
  那两样回答的是「我要点的东西还在我看到的位置吗、这一页允许点它吗」，虚拟键根本不提这个问题，
  在这里要兑现它只能杜撰一个 detection。二是不强制 observation 的 lease：lease 约束的是坐标的
  保质期，而按键的含义不随布局衰减；强制它等于用一条根本不适用的理由拒绝按键，还会逼操作者为
  整次 run 放宽 `--max-frame-age`——为了一个按键削弱其中的每一次点击。

**engine 没有任何循环。** 上面六个动词全是单次的：观察一帧、在那一帧上解析、在那一帧上找、点一次或按一次键。
2026-07-29(`8b16f2d`)删掉了 `EngineSession::waitForPage`、它的成对返回类型 `PageWait` 和那个永远
no-op 的 `sweepKnownPopups`；「等到某页出现」现在是受信任 Luau framework 里的
`ctx:wait_for_page`（`modules/task/runtime/ctx.luau`）。理由见
[`docs/plans/2026-07-29-three-layer-task-system.md`](../../plans/2026-07-29-three-layer-task-system.md)
§1 与 §16：能力层里不该有 policy 循环，而那个循环还捎带一个弹窗接缝——接缝在循环里，
弹窗策略在 Luau 里，两边永远接不上。

一次产品数据流现在从 `modules/task` 那侧读起，`entry/cli/run-windows.cpp` 只负责装配：

```text
loadRuntimeProject
  -> resolve page/action names
  -> bind WgcCaptureSession + DeliveryTarget
  -> create IFrameSource/IActionSink/ITraceSink adapters
  -> task::TaskHost::loadProject / startTask
  -> EngineSession::create
  -> (ctx.luau 的等待循环，每轮一次)
       EngineSession::observe
       -> EngineSession::resolvePage
       -> EngineSession::findAction(observation, recognizerId)
       -> EngineSession::act
       -> IActionSink::click
```

`act` 内部的关键顺序是：

1. cancellation gate；
2. session provenance 与 stale-handle guard；
3. `annotation::authorizeCoordinateAction`；
4. emit `ActionAuthorized`；
5. pixel → frame → client 坐标变换；
6. delivery-edge `IFrameSource::validateTargetInstance`；
7. `IActionSink::click(clientPoint, observation.m_lease)`；
8. 立即设置 `observation.m_invalidated = true`；
9. emit `ClickDelivered` 与 `ObservationInvalidated`。

第 8 步必须早于两个可能失败的 post-click trace。若点击已落地而 trace 随后失败，
调用者仍可能保留传入的命名右值别名；先作废可保证重试得到 `StaleObservation`，
不会因为记录失败而双击。

### 追踪事件

schema 由 `modules/trace/source/trace/event.hpp` 拥有，id 是
`umbraflow-trace/v1`；engine 与 task 写同一条流。engine 发出的事件是：

- `engine.observed`、`engine.observation_invalidated`。
- `engine.page_resolved`，outcome 为 `Resolved` / `Unknown` / `Ambiguous` /
  `Stopped` / `Failed`。跑完的一次尝试还带 `pageScores`：每个被评估的 page 一条，
  记录它是否仍是候选，以及它最差的 required anchor 相对自身上限的分数，因此没解析
  出来时能读出差了多少，而不是只知道没解析出来。
- `engine.action_found`，outcome 为 `Found` / `Absent` / `Stopped` / `Failed`。
- `engine.action_authorized`、`engine.action_rejected`、`engine.action_delivered`。
- `engine.key_delivered`，每投出一次按键一条，记录它花掉的 observation 来自哪一帧，以及按了哪个键。
  它没有坐标，理由与 `KeyReceipt` 没有坐标是同一条。

`engine-trace/v1` 的 `PageResolved` / `PageUnknown` / `PageAmbiguous` 与
`ActionFound` / `ActionAbsent` 折叠成上面两个 kind 的 outcome；原先与阶段无关的
`RecognitionStopped` 与 `Failure` 也成为对应阶段的 outcome，因此现在还能读出停止或
失败发生在哪一步。`SessionStarted` 没有后继：`task::TaskHost` 的 `run.started`
记录同一时刻，并带上 project、task、source hash、framework 版本与 bundle hash、
Luau 编译器版本、seed 与 run 身份。`entry/cli` 的 smoke 路径不写任何 run 级事件、
trace 从第一条 `engine.observed` 开始，它已于 2026-07-29 随运行生命周期迁入
`TaskHost` 一并删除。

`trace::TraceRecorder` 在每条事件上盖 `seq`、`runId`、`generationId`、`frontEnd`，以及 `meta`
里的 `wallClock`。`meta` 是文档化的非 golden 字段集，golden 比较前用
`trace::stripNonGoldenFields` 剥掉。engine 不拥有 sink：它借用 run 的 recorder，
文件的打开、逐条写入与 flush 由 `modules/trace` 的 `FileTraceSink` 负责。

**`frontEnd` 属于「盖章」而不属于事件本身**（2026-07-30，`ed38124`）。取值是 `"task"` 或
`"operator"`，来自 `trace::FrontEnd`。它存在是因为能力面现在有两个同级消费者——task 所跑的
受信任 Luau framework，和从进程外送命令的操作者——没有这条归属，读 trace 的人就回答不了
「这件事是 task 做的还是操作者做的」，而这个问题对每一行都要问一次；于是一个 recorder 为整次
run 持有一个值并盖到每一行上，没有任何发射方能忘掉它，也没有谁能冒领另一个前端的活。
`TaskHost` 交给 recorder 的就是它闩住的那个值，所以一条流的归属与产生它的互斥是同一件事，
而不是两件必须彼此吻合的事。

它同时是一条协议规则而不只是标签：`TraceStreamValidator` 在 operator 流上**拒绝
`framework.*` 事件**，报 `InternalInvariant`。那些事件描述的是受信任 Luau framework 自己的
结构——哪个 step 开着、这是第几次重试、哪个 interrupt 命中了——而 operator 流上根本没有那个
framework，所以这样一行只可能是宿主 bug 把 task 的结构安到了操作者头上。拒绝它，才使这个字段
是权威而不是装饰。

## 必须保持的约束

### Fail-closed

所有会导致“对哪里做什么”不确定的条件都沿拒绝方向失败：

- session 缺任一端口时，`EngineSession::create` 返回 `InvalidResource`。
- live fingerprint 与 catalog fingerprint 不同，识别或授权拒绝。
- page evidence、action detection 与 delivery 的 `CaptureSessionId`、
  `TargetGeneration`、`FrameId` 必须相同；action recognizer 还必须属于 active
  catalog、类型为 `ActionTarget` 且允许 resolved page。
- `ObservationLease::validate` 校验 session、generation、frame 和 expiration；
  任一不符返回 `StaleObservation`。
- recognition budget、deadline 或 cancellation 产生明确 stop reason，而不是把
  半完成搜索当 miss。page/action stop 会先 emit `RecognitionStopped`，再映射为
  `RecognitionIncomplete`、`Timeout` 或 `Cancelled`。`RecognitionIncomplete` 说的是
  搜索没看完，不是看完了没匹配上；它的 `FailureResponse` 是 `Retry`，调用方要重新
  观察，而不是把该页当已排除。
- `UnknownPage`、`AmbiguousPages` 不可伪装成 `ResolvedPage`；`act` 的参数类型本身
  要求真实 `ResolvedPage`。action miss 则是空 optional，保持 Tier A 正常控制流。
- delivery 前再次调用 `validateTargetInstance`。目标被替换时 emit
  `ActionRejected`，且不会调用 sink。

这里有两层 lease 防线，但职责不同。annotation 授权层比较完整 frame identity 和
过期时间；当前 controller 的 `checkPointerPreconditions` 在 post 前再次比较
session、target generation、age，并校验有限且落在 client area 内的坐标。
controller 当前没有“最新 FrameId”输入，不能描述成在投递层再次比较当前 frame。

### Model B 的所有权与生命周期

D1 的 Model B 被编码为句柄，而非仅靠调用约定：

- `Observation` 独占一份 `Frame`、对应 `ObservationLease` 和 `FrameIdentity`。
- 它不可复制，只可移动；move constructor 和 move assignment 都立即把 source
  标为 invalidated，所以 moved-from 句柄与已消费句柄行为一致。
- `EngineSession::resolvePage` 与 `EngineSession::findAction` 首先检查 observation
  的 invalidated flag；失效后任一查询都返回 `StaleObservation`。
- `act` 接受 `Observation&&`，成功投递后作废整个 observation。调用者必须重新
  `observe`，从结构上维持“一次观察、同帧多查询、一次坐标动作、重新观察”。
- observation 不保存指向 `EngineSession` 的指针或 borrow，而是与产生它的 session
  共享一个私有、不可变的 identity token。token 会随 session move 到新对象，既有
  observation 因而仍然有效；把它交给其他 session 会返回 `InternalInvariant`，过程中
  不会解引用 moved-from 对象。
- session 独占 runtime 与三个端口；`ActionFound` 与 `ActReceipt` 都是明确拥有其结果的
  值，不返回悬空的临时 view。

### 确定性与有界执行

同一 observation 上的所有查询都读取同一 frame，避免两个隐式 capture 之间游戏状态
变化。识别 policy 每次由固定 comparison budget、monotonic deadline 和 stop token
构造；超预算/超时是可区分的停止，不把部分搜索结果当完整结果。

manifest 顺序驱动资产读取，重复 hash 用线性 `loadedHashes` 去重，不依赖 unordered
container 的迭代顺序。点击 pixel 由 annotation 规则确定，坐标变换来自 captured
frame 自带的 `CoordinateTransform`。trace 字段顺序和 wire names 也固定，便于 golden
比较与下游拒绝未知 schema。

墙钟只参与 recognition deadline、lease 陈旧保险丝和 capture deadline。这些路径的
偏差只会缩短可操作窗口或返回停止/超时，不会把未知状态转成允许动作。

engine 自己不睡。切片睡眠 `core::pollSleep`（`modules/core/source/core/time/poll-sleep.hpp`，
按 `k_maxPollSleepSlice = 100ms` 切片，每片之间重查 cancellation 与 deadline）今天的生产
调用方是 task 的 `wait` 与 `settle` 原语，engine 一个都没有——它随 2026-07-29 的等待循环
重构从 engine 升进 core，理由是「对着 deadline 停一下」是通用时间设施，下一个需要它的模块
不该再长出第二份切片逻辑。engine 侧唯一还会阻塞的地方是 `IFrameSource::capture`，
而它被 `CaptureBudget` 界住：套件里的 `DeadlineHonouringFrameSource`
（`tests/engine/test-session.cpp`）就是靠真的阻塞到 deadline 来证明这一点。

### 在失败发生时记录追踪

D4 要求错误在向上层传播的瞬间记录，而不是期待未来脚本自觉记录。当前具体机制不是
全局 exception hook，而是 engine failure site 的显式 emit：

- `evaluatePage` / `evaluateActionTarget` 返回错误时，先组装带 frame identity、
  `errorKind` 和 message 的 `Failure`，emit 成功后才返回原错误。
- recognition stop 先 emit `RecognitionStopped`，再返回映射后的结构化错误。
- authorization 或 delivery-edge revalidation 拒绝时，先 emit `ActionRejected`，
  再传播原错误。

emit 自身可失败；`UF_TRY` 会立即传播该失败，因此 trace 基础设施故障不会被降级成
无声运行。还应注意当前覆盖范围：session 创建前的 loader 错误、observe/act 的前置
cancellation、capture 自身的 deadline/cancellation，以及 `IActionSink::click`
直接失败，目前没有统一生成 `Failure` 事件。扩展错误路径时必须阅读具体 emit site，
不能假设存在中央拦截器。（页面等待的超时不在这张表上了：它现在是 `ctx.luau` 里的
`native.raise("timeout", ...)`，落在 `task.native_call` 那条流上。）

### 严格后台

engine 只规定“必须严格后台”的端口契约，具体机制由 Windows 适配器实现：

- `modules/controller/source/controller/input.hpp` 明列
  `SetForegroundWindow`、`SetFocus`、`SendInput`、`mouse_event`、`keybd_event`、
  `SetCursorPos` 为 forbidden background APIs。
- `modules/controller/source/controller/platform/windows-input.cpp` 只向已解析的单个
  HWND 调用 `PostMessageW`，并拒绝 null 与 `HWND_BROADCAST`。
- controller 在 post 前检查 lease、目标 window 存活、client bounds 和消息编码；
  `ControllerActionSink` 维护 held-input bookkeeping，并在 click 失败时补偿 release。

因此 engine 的 platform-free 并非减少安全性，而是把可移植的授权时序与不可移植的
投递证明分层。任何新 adapter 都必须重新兑现 `IActionSink` 的严格后台和 lease
pass-through 契约；仅仅“实现了虚函数”并不自动获得该保证。

## 端口与依赖

主要 inbound edges 如下：

- `entry/cli/run-windows.cpp` 提供已选择目标、live fingerprint、预算、超时、
  cancellation 与两个端口，交给 `task::TaskHost`；驱动 observe/resolve/find/act 的是
  受信任 Luau framework，CLI 自己不再调用任何 engine 动词。
- annotation 通过 runtime manifest 提供 catalog、模板、page signatures、
  action-target 定义与 allowed-page policy。
- domain 提供 `Frame`、`CoordinateTransform`、`Detection`、`ObservationLease`、
  `CaptureSessionId`、`TargetGeneration`、`FrameId` 和 `AutomationErrorKind`。
- core 提供 `Result`/`Status`、monotonic time、整数类型和 contracts。

主要 outbound edges 如下：

- 对 annotation：调用 `parseRuntimeManifest`、`RecognitionRuntime::create`、
  `evaluatePage`、`evaluateActionTarget`、`resolveClickPixel`、
  `ActionDetection::create`、`authorizeCoordinateAction`。
- 对 capture adapter：observe 前请求目标复核与 capture，act 的最后边缘再次复核。
- 对 action adapter：只跨越已经转换好的 `Point<ClientSpace>` 与未丢字段的 lease。
- 对 trace adapter：同步发送结构化 `TraceEvent`；文件路径、append/truncate 与 flush
  都不穿过 engine 边界。
- 对调用方：返回结构化 page outcome、optional action、act receipt 或保留
  `AutomationErrorKind` 的 `Error`，不把平台异常或 Luau 类型带入 API。

依赖方向必须保持单向：entry 可以同时看见 engine 与 controller，engine 不能反向
include controller。否则 fake 端口无法在平台无关测试中替代桌面能力，也会把 Windows
类型泄漏进未来 Luau binding 的稳定领域表面。

## 测试

`tests/engine/test-runtime-loader.cpp` 固定读取边界：

- 编译一个 authoring fixture、写出发布目录、再由 `loadRuntimeProject` 读回并实际识别
  页面，验证 compile → publish → load → recognize 闭环。
- corrupt manifest 返回 `InvalidResource`。
- missing template 返回 `IoFailure`，错误文本包含 `assets/templates`。
- 篡改模板 bytes 导致 hash mismatch 并返回 `InvalidResource`。
- 超过 16 MiB 的 manifest 在读取前由 stat fast path 拒绝。

`tests/trace/test-trace.cpp` 固定 wire contract：

- 全字段事件的 schema-first 固定顺序和精确 golden JSON。
- 最小事件只输出 `schema` 与 `kind`。
- quote、backslash 与 control bytes 的 JSON escaping。

同一文件验证 `trace::FileTraceSink` 的 JSONL 写入：

- 每次 emit 产生一条由 `serializeTraceEvent` 定义的行。
- 不可打开的路径返回 error `Status`，而不是静默丢 trace。

`tests/engine/test-session.cpp` 使用 `FakeFrameSource`、`CountingActionSink`、
`CollectingTraceSink`，固定运行时状态机：

- happy path 只投递一次，lease 的 `FrameId` 原样到达 sink，事件顺序精确为
  start/observe/page/action/authorize/click/invalidate。
- fingerprint mismatch、错误 allowed page、expired lease 均为零投递并留下相应
  failure/rejection trace。
- action 后复用、moved-from 复用返回 `StaleObservation`；foreign session handle
  返回 `InternalInvariant`。
- `ClickDelivered` trace 故障发生在真实 click 之后时，句柄已经失效，重试不会双投递。
- unknown page 保持 `UnknownPage`，不产生可供 `act` 使用的 `ResolvedPage`。
- comparison budget stop 和 recognition cancellation 保留不同 error kind，并记录
  `RecognitionStopped`。
- observe 前 cancellation、observe 后 act cancellation 都是零投递。
- target instance 在观察后失效时，delivery-edge guard 阻止 sink call。
- `observe` 交给 `IFrameSource::capture` 的 `CaptureBudget` 携带真期限与可用的 stop
  token：`DeadlineHonouringFrameSource` 真的阻塞到那个时刻才返回，是套件里唯一一个
  不是凭空满足 budget 的帧源。

`pressKey` 在 `EngineSession` 这一层没有自己的用例。`CountingActionSink` 实现了这个动词并记录
它被围栏在哪个 `TargetGeneration` 上——那个 generation 正是被测的授权差别——但行为钉在上一层的
`tests/task/test-operator-front-end.cpp`：按键不需要已解析的 page（这正是它与点击的分界）、
投出去的按键消费掉它的周期且只到达 sink 一次、无法解析的键名在周期被花掉之前就被拒绝，
以及 task 与 operator 走到它的方式完全一致。改 `pressKey` 时读那几个用例，不要去 engine 里找。

这些测试有意把 engine 的顺序与 Windows 分离：engine fake 固定 lease pass-through，CLI
sink 测试固定 JSONL durability，controller 的 lease、窗口 identity、message encoding
与 strict-background 约束由 `tests/controller/` 固定。`ControllerActionSink` 的补偿与
真实组合目前没有单独的 CLI unit test，修改该 adapter 时不能误把下游测试当成直接覆盖。

## 后续扩展

### B2 Luau 绑定

`docs/plans/2026-07-23-engine-architecture.md` 指定当前 API 镜像已锁定的 D1 Model B，
目的就是 B2 不重构领域表面即可绑定：

- Luau capture/observe handle 对应 move-only `Observation`。
- 同帧 page query/find 对应 `resolvePage` 与 `findAction`。
- Tier A miss 对应成功的空 optional；Tier B/C 的 Luau 表达仍需按 D4 与 hardening
  ledger 实现，不能把所有 C++ `Error` 简单变成可被 `pcall` 吞掉的普通 error。
- click 对应 `act`，成功后整个 handle invalidated。
- 等待**不对应任何 engine 动词**：它是 framework Luau 里 observe/resolve 加一次
  `wait` 原语的循环。这是 2026-07-29 的更正——原文写「wait 对应 `waitForPage`」，而那个
  动词已经不存在了。

当前 `modules/script` 与 `modules/engine` 之间没有 manifest edge，B2 尚未实现。绑定层
应依赖稳定的 engine 操作面，不应把 `lua_State`、userdata 或 scheduler 概念反向放入
engine。D4 Tier C 的不可吞取消、VM interrupt、instruction/runtime budget 和 sandbox
仍以 `docs/plans/2026-07-21-p0b-luau-hardening-ledger.md` 为实现约束。

### 平台与 fake

P3 第二平台和测试替身都通过 `IFrameSource`、`IActionSink`、`ITraceSink` 接入。
新增平台时，目标发现与 adapter 仍放 entry/platform；engine 不增加 `#ifdef Windows`。
新 `IActionSink` 必须证明目标实例、lease fencing 与 strict-background，而不是只做
坐标传输。

### 等待与 D6（已迁出 engine）

弹窗处理**不再是 engine 的扩展点**。`sweepKnownPopups` 那个 no-op 接缝随
`waitForPage` 一起在 2026-07-29(`8b16f2d`)删除，D6 的能力落在受信任 Luau framework 的
interrupt 注册表上：`task.interrupt{ id, when, max_hits, handle }` 声明，
`ctx:wait_for_page` 的每一轮解析出页面后交给注册表匹配。

这次搬迁本身就是那个能力缺口的答案。旧接缝在 engine 的轮询循环里，而弹窗策略在 Luau 里，
两边够不着——真接上去也只会在一次等待的开头响一次，中途冒出来的弹窗仍然处理不了。
现在 grill D6 要的三条都成立且可读：周期边界（handler 拿到的是**当前**这个观察周期，
点击消费它，循环随即重新观察）、声明顺序 first-match（`task.define` 的列表序）、
有界命中（`max_hits`，缺省 3）。详见
[`docs/plans/2026-07-29-three-layer-task-system.md`](../../plans/2026-07-29-three-layer-task-system.md)
§6。

### 生命周期与 D10

当前 `EngineSession` 是一次 run 的拥有者，没有 task id、队列或并发。D10 在
`docs/plans/2026-07-21-lua-task-model-grill-decisions.md` 预留
`load_project/start_task/pause/resume/cancel/query_task/subscribe_events`，并要求从
P0 一次一 run 升级到 P2 常驻 Engine 时保持 API 语义。扩展点应包在 session 外部管理
生命周期，不应削弱 observation 的单帧与单动作不变量。

### runtime 与 trace 演进

`docs/plans/2026-07-23-engine-architecture.md` 明确把 `project.toml` 的项目级
fingerprint 读取推后；当前 authority 是 runtime manifest 内嵌 fingerprint。未来增加
读取时，应明确两个 authority 的一致性检查，不能静默选择其一。

trace 演进应新增 schema version，并同步 `TraceEvent`、显式 wire-name switch、
serializer golden tests 与所有 sinks/consumers。不能借 C++ enum rename 偷改 v1；
也不能把新增字段的 unordered iteration 引入稳定输出。

新增 swipe 或更丰富 action 时，应扩展明确的 action port/receipt 与对应 lease
规则，而不是绕过 `act` 直接暴露 controller。无论动作种类如何，delivery-edge
revalidation、invalidate-before-fallible-post-delivery-work、零焦点窃取和可诊断 trace
这些约束在扩展后仍须保留。

**按键动作就是这条规则的样板**（2026-07-30，`ed38124`）：它新增了 `IActionSink::pressKey`、
`EngineSession::pressKey`、自己的 `KeyReceipt` 和自己的 `engine.key_delivered`，而不是复用
click 的回执或它的 lease。它说明的是：接缝上的围栏不是一条照抄的规则，而是每种动作都要各自
回答一次的问题——点击围栏的是坐标，需要 lease；按键围栏的是实例，需要 generation——而正确的
「不同」写法是换一个参数类型，不是在原来那个上加一个标志位。将来的拖拽两端各有一个坐标、
中间还有一段保持，它得用第三种方式回答同一个问题。
