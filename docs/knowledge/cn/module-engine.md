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
- 不定义 trace 文件格式之外的存储策略。engine 的 serializer 不做 I/O，也不追加换行；JSONL 的打开、逐条写入和 flush 位于 `entry/cli/file-trace-sink.cpp`。

严格后台输入只能由平台层实现，识别和授权则必须能在没有桌面、没有 HWND 的 CI
中复现。为此，engine 只依赖几个窄端口；测试可以提供替身实现，平台代码也能保持
简短、便于审查。

## 运行时流程

### 端口

公开端口集中在 `modules/engine/source/engine/ports.hpp`。

- `IFrameSource::capture() -> Result<Frame>` 从一个已经绑定的目标取得帧。
- `IFrameSource::validateTargetInstance() -> Status` 复核绑定的仍是同一个目标实例。engine 在 capture 前调用一次，在 delivery 前再调用一次，后者用于关闭“观察后 HWND 被复用或目标被替换”的窗口。
- `IActionSink::click(Point<ClientSpace>, ObservationLease const&) -> Status` 接收 client 坐标和原始 lease。lease pass-through 是接口的一部分，适配器不能只传坐标，否则 controller 层无法执行第二层 session/generation/age fencing。
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
- `EngineSession::observe() -> Result<Observation>` 先检查 cancellation，再复核目标实例、capture、由 `ObservationLease::forFrame` 建 lease、提取 `annotation::FrameIdentity`，emit `Observed` 后才把句柄交给调用者。
- `EngineSession::resolvePage(Observation const&)` 对传入 observation 持有的 frame
  调用自身 `RecognitionRuntime::evaluatePage`，返回由 `ResolvedPage`、
  `UnknownPage` 或 `AmbiguousPages` 组成的 `PageOutcome`。
- `EngineSession::findAction(Observation const&, RecognizerId)` 对同一 frame 调用
  `evaluateActionTarget`。未命中是 `Result<std::optional<ActionFound>>` 的成功空值，
  对应 D4 Tier A，不是错误。
- `ActionFound` 保存原始 `AnchorEvidence`、绑定 recognizer identity 的 `ActionDetection` 和确定性的 `PixelPoint`。点击点由 annotation 的 `resolveClickPixel` 决定；match rect 经 `pixelRectToFrameRect` 变成 authorization-ready `Detection`。
- `EngineSession::act(Observation&&, ResolvedPage const&, ActionFound const&)` 消费 observation，成功返回记录被授权 `FrameId` 与 client-space 坐标的 `ActReceipt`。
- `EngineSession::waitForPage(PageId, timeout, pollInterval)` 重复 observe/resolve，命中后返回成对的 `Observation` 与同帧 `ResolvedPage`，调用方无需、也不应重新识别。

一次产品 CLI 数据流可在 `entry/cli/run-windows.cpp` 直接跟读：

```text
loadRuntimeProject
  -> resolve page/action names
  -> bind WgcCaptureSession + DeliveryTarget
  -> create IFrameSource/IActionSink/ITraceSink adapters
  -> EngineSession::create
  -> waitForPage
  -> EngineSession::findAction(PageWait::m_observation, actionId)
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

`engine-trace/v1` 的 `PageResolved` / `PageUnknown` / `PageAmbiguous` 与
`ActionFound` / `ActionAbsent` 折叠成上面两个 kind 的 outcome；原先与阶段无关的
`RecognitionStopped` 与 `Failure` 也成为对应阶段的 outcome，因此现在还能读出停止或
失败发生在哪一步。在脚本路径上 `SessionStarted` 没有后继：组合根的 `run.started`
记录同一时刻，并带上 project、task、source hash、framework 版本与 bundle hash、
Luau 编译器版本、seed 与 run 身份。`entry/cli` 的 smoke 路径不写任何 run 级事件，
它的 trace 从第一条 `engine.observed` 开始；该路径随 stage 1d 的 TaskHost 一起删除。

`trace::TraceRecorder` 在每条事件上盖 `seq`、`runId`、`generationId`，以及 `meta`
里的 `wallClock`。`meta` 是文档化的非 golden 字段集，golden 比较前用
`trace::stripNonGoldenFields` 剥掉。engine 不拥有 sink：它借用 run 的 recorder，
文件的打开、逐条写入与 flush 由 `modules/trace` 的 `FileTraceSink` 负责。

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
  `RecognitionFailed`、`Timeout` 或 `Cancelled`。
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
- session 独占 runtime 与三个端口；`ActionFound`、`PageWait`、`ActReceipt` 都是
  明确拥有其结果的值，不返回悬空的临时 view。

### 确定性与有界执行

同一 observation 上的所有查询都读取同一 frame，避免两个隐式 capture 之间游戏状态
变化。识别 policy 每次由固定 comparison budget、monotonic deadline 和 stop token
构造；超预算/超时是可区分的停止，不把部分搜索结果当完整结果。

manifest 顺序驱动资产读取，重复 hash 用线性 `loadedHashes` 去重，不依赖 unordered
container 的迭代顺序。点击 pixel 由 annotation 规则确定，坐标变换来自 captured
frame 自带的 `CoordinateTransform`。trace 字段顺序和 wire names 也固定，便于 golden
比较与下游拒绝未知 schema。

墙钟只参与 recognition deadline、lease 陈旧保险丝和 wait deadline。这些路径的
偏差只会缩短可操作窗口或返回停止/超时，不会把未知状态转成允许动作。

`waitForPage` 的 sleep 被 `k_maxPollSleepSlice = 100ms` 切片。即使调用者给出 10 秒
poll interval，每片之间也检查 cancellation 和 deadline，因此等待不会把取消响应
绑死在完整 poll interval 上。`sweepKnownPopups` 每轮先调用一次，但当前是明确的 no-op。

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
cancellation、`waitForPage` 自身 timeout/cancellation，以及 `IActionSink::click`
直接失败，目前没有统一生成 `Failure` 事件。扩展错误路径时必须阅读具体 emit site，
不能假设存在中央拦截器。

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
  cancellation 与三个端口，然后驱动 `waitForPage -> findAction -> act`。
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

`tests/engine/test-trace.cpp` 固定 wire contract：

- 全字段事件的 schema-first 固定顺序和精确 golden JSON。
- 最小事件只输出 `schema` 与 `kind`。
- quote、backslash 与 control bytes 的 JSON escaping。

`tests/cli/test-file-trace-sink.cpp` 验证 JSONL 写入：

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
- 10 秒 poll sleep 中途取消能因 100 ms slicing 在测试上限内返回；另有立即 timeout
  与第二帧成功命中的测试。

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
- wait 对应 `waitForPage`，并直接携带命中它的 observation。

当前 `modules/script` 与 `modules/engine` 之间没有 manifest edge，B2 尚未实现。绑定层
应依赖稳定的 engine 操作面，不应把 `lua_State`、userdata 或 scheduler 概念反向放入
engine。D4 Tier C 的不可吞取消、VM interrupt、instruction/runtime budget 和 sandbox
仍以 `docs/plans/2026-07-21-p0b-luau-hardening-ledger.md` 为实现约束。

### 平台与 fake

P3 第二平台和测试替身都通过 `IFrameSource`、`IActionSink`、`ITraceSink` 接入。
新增平台时，目标发现与 adapter 仍放 entry/platform；engine 不增加 `#ifdef Windows`。
新 `IActionSink` 必须证明目标实例、lease fencing 与 strict-background，而不是只做
坐标传输。

### 等待与 D6

`EngineSession::sweepKnownPopups` 已位于每个 `waitForPage` cycle 的开头。
`docs/plans/2026-07-23-engine-architecture.md` 把 P0-C 的最小 known-popup sweep 和
P1 的 `bot:on` registry 放在这里。当前 no-op 只锁住调用时机，不代表 popup policy
已经存在；未来实现还要服从 grill D6 的周期边界、声明顺序 first-match 和有界命中。

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

新增 swipe、key 或更丰富 action 时，应扩展明确的 action port/receipt 与对应 lease
规则，而不是绕过 `act` 直接暴露 controller。无论动作种类如何，delivery-edge
revalidation、invalidate-before-fallible-post-delivery-work、零焦点窃取和可诊断 trace
这些约束在扩展后仍须保留。
