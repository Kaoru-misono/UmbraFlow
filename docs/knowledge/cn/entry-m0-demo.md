# entry/m0-demo：冻结的 M0 真机底座

`entry/m0-demo` 是已经冻结的 Windows 验收程序。它保存了一条在真实机器和高完整性
目标窗口上验证过的最短链路：WGC 后台捕获、
灰度 SAD 模板匹配、基于观测租约的客户端坐标点击、严格后台 `PostMessageW` 投递、
前后台与光标 guard、投递审计，以及失败后的输入补偿和有序关闭。

> **更正 2026-07-31。** 本文此前把 `input-agent` 子命令写成冻结 demo 的一部分。它并不冻结
> ——它已经成了标注前端，并且带着自己的 `umbra-input-agent` 可执行文件搬去了
> [`entry/input-agent`](entry-input-agent.md)。留在这里的才是真冻结的：固定的
> home -> result -> reset 循环，以及 `capture` 诊断工具。`m0-demo input-agent` 现在只打印
> 程序去了哪里，然后以失败退出。
>
> 但 demo 并不自给自足。它链接 `input_agent_support`，用的是两者仍然共用的东西——帧 PNG
> 写出、路径围栏、目标选择与捕获会话搭建、JSON 字符串转义、错误文本，以及命令行解析原语。
> 依赖方向是 demo -> 前端，反过来绝不发生，所以将来退役 demo 仍然只是一次删除。

本文中的“冻结”以
`docs/plans/2026-07-20-m0-demo-port-deviations.md` 和
`docs/plans/2026-07-23-engine-architecture.md` 为准：不要继续把产品能力加进这个目录，
也不要让新的产品入口链接它的实现。需要复用时，应复制已经验证的安全语义到
`engine`/runner adapter，并由产品契约重新承载。

## 它验证什么

M0 demo 拥有两种进程入口，分派点在 `entry/m0-demo/main.cpp`，外加第三种只做转向的拼写。

- 默认入口解析 `Args`，选择一个真实窗口，创建 `WgcCaptureSession` 和
  `DeliveryTarget`，加载 `home`、`result`、`reset` 三张模板，然后调用
  `runPipeline`。
- `capture` 子命令解析 `CaptureArgs`，只做目标发现、WGC 捕获和 PNG 输出；
  实现在 `entry/m0-demo/capture-mode.cpp` 的 `runCapture`。
- `input-agent` 这个参数会得到一条点名 `umbra-input-agent` 的消息和失败退出。这个拼写
  留着，是因为记录下来的流程和会话脚本还会到这里找它；没有这个分支，demo 自己的解析器
  会答“unknown argument”，读起来像构建坏了。

默认 pipeline 的业务形状是固定的：

```text
click home -> wait result -> click reset -> wait home
```

它只负责证明这条有限循环能在后台安全执行。`LoopConfig::m_loops` 决定重复次数，
`RunSummary` 汇总 attempted、succeeded、guard violation、stop 和 audit clean。
`RunSummary::passed()` 仅在未停止、审计干净、无 guard violation、且每次尝试都成功时
返回 true。

它不承担以下产品职责：

- 不读取 `project.toml`、`annotations.toml` 或
  `generated/annotations.runtime.toml`。
- 不创建或调用 annotation 的 `RecognitionRuntime`，也不理解元素 ID、能力集合、
  page signature、`ResolvedPage` 或 page reference。（更正 2026-07-31：这里原本列的是
  `annotation_type` 与 `allowed_page_ids`，两者都被
  [能力模型计划](../../plans/2026-07-31-annotation-model-capabilities.md)删除。这一条的意思
  没变，而且更强了——不管标注模型长成什么样，m0-demo 一概不理解。）
- 不执行“页面证据授权动作”。一个模板只要通过 SAD 阈值，`clickWhenPresent`
  就点击该匹配矩形中心。
- 不提供 Luau task、长期 Engine 生命周期、通用 popup sweep、跨平台 port 或 GUI。
- 不把 `capture` 子命令当成输入授权；它只是捕获诊断工具。
- 不为标注会话服务。那是 [`entry/input-agent`](entry-input-agent.md)，另一个程序、
  另一个二进制。

它没有接入 annotation 授权栈，因为 M0 只验证捕获和输入链路。S0 之后，页面证据、
动作权限、运行时清单闭包和失败关闭授权才成为产品契约。
`docs/plans/2026-07-22-annotation-design.md` 明确要求
`Detection + ResolvedPage + ObservationLease + target fingerprint` 共同授权动作，
而 M0 只有模板命中、租约和目标复验，没有 `ResolvedPage` 这一层能力。

真机验收记录位于 `docs/TODO.md` §0、
`docs/plans/2026-07-20-post-port-win32-robustness.md` 和已归档的
`docs/archive/plans/2026-07-20-ui-verification-runbook.md`。2026-07-21 的结果是：

- 卡厄斯梦境客户区为 1600×900，WGC frame 与 client 的 K2 crop
  `delta=(0,0)`；
- 头像切换三次、标签切换三次成功；
- 首次“潜力”介绍 overlay 被识别并安全关闭；
- 所有已投递点击走严格后台 `PostMessage` 链，没有抢 foreground；
- 真实机器上确实触发过 `StaleObservation: lease expired`，并保持
  `delivered:false`，证明过期观测会 fail closed。

这里的 `delta` 是 `frame.width/height - client.width/height`。
`capture-mode.cpp` 和 input agent 的 capture result 都按这个定义记录尺寸差。
`delta=(0,0)` 是该目标和该真机配置的验收事实，不是所有 WGC 目标的普遍保证。

## 执行流程

### 入口参数与组合

`entry/m0-demo/args.hpp` 定义两组值类型。

- `Args` 保存三组 template/ROI、`m_threshold`、`Mode`、循环数、最大 frame age、
  capture stall timeout、可选 click delay、seed 和 log path。
- `CaptureArgs` 保存 selector、输出路径、frame 数、间隔和 log path。

`SelectorArgs`——可选的 PID、HWND、window class 和 title——归 input agent 所有，
声明在 `entry/input-agent/target-setup.hpp` 里消费它的 `buildSelector` 旁边，
因为两个程序拼的是同样四个 flag。两者共用的底层解析原语（`parseInteger`、
`parseWindowHandle`、`require`、`invalid`）出于同样的理由住在
`entry/input-agent/arg-parsing.hpp`。

默认 pipeline 的 `--threshold` 必填，范围是 0..255；默认
`--max-action-frame-age` 为 750 ms，默认 `--stall-timeout` 为 1000 ms。
`Mode` 默认为 `Guard`，另一个值是 `Coexist`。这些约束由
`entry/m0-demo/args.cpp` 的 `parseArguments` 实施，而公开数据形状在
`entry/m0-demo/args.hpp`。

`main.cpp` 中的 `runWithLog` 是默认路径的组合入口：

1. `ensurePerMonitorAwareV2()` 声明 DPI awareness。
2. `installConsoleControlHandler()` 安装 Ctrl-C/Ctrl-Break stop flag。
3. `enumerateCandidates()`、`buildSelector()`、`resolveTarget()` 得到
   `ResolvedTarget`。
4. 用固定的 `CaptureSessionId{1}` 创建 `WgcCaptureSession`。
5. `loadTemplate()` 把三张 PNG 转成灰度 `Template`。
6. 从 `Args` 组装 `Templates` 与 `LoopConfig`。
7. 以 HWND、session、generation 和 client size 创建 `DeliveryTarget`。
8. 调用 `runPipeline()`，最后关闭 console control registration。

`Template` 在 `entry/m0-demo/pipeline.hpp` 中拥有 label、灰度像素
`std::vector<std::byte>`、宽高和一个 `Rect<FrameSpace>` 搜索 ROI。
`Templates` 只是固定的 home/result/reset 三元组；它不是通用 recognizer collection。

### 捕获、SAD 匹配与结果验收

主循环的真实识别链在 `entry/m0-demo/pipeline.cpp`。

`captureFresh()` 先调用 `Machine::ensureTargetUnchanged()`，再
`WgcCaptureSession::capture()`，捕获成功后再次复验 target。只有
`AutomationErrorKind::CaptureStalled` 被解释为“本轮没有 frame”并允许继续等待；
其他 capture error 原样终止。这样不会把 `StaleObservation` 误标为普通 stall。

`recognizeRaw()` 对每个 frame 执行：

```text
Frame
  -> CoordinateTransform::frameRectToPixelRect(template.m_roi)
  -> image::cropBgra8
  -> bgra8ToGray8
  -> GrayImage::create
  -> matchTemplateSad
  -> optional<SadMatch>
```

SAD 搜索的比较预算固定为 64 Mi 次 pixel comparison。
`SadSearchPoll` 在搜索中检查全局 stop flag 和 transition timeout。
`SadSearchStopReason::Cancelled`、`TimedOut`、`ComparisonBudgetExhausted`
分别成为 stopped、timed out、failed；它们不会被降格成“没有匹配”。

`matchTemplateSad` 返回的坐标相对 ROI，`recognizeRaw()` 用 checked addition
加回 ROI 的 x/y，得到 frame-space `SadMatch`。随后
`acceptMatch(found, width, height, maximumAverageSad)` 计算：

```text
area   = width * height
budget = maximumAverageSad * area
accept = found exists && area != 0 && found.score <= budget
```

乘法溢出时 budget 饱和为 `uint64` 最大值；正常 CLI 路径已把每像素平均阈值限制在
0..255，模板尺寸也经过加载与 ROI 检查。等号属于接受边界。

这套阈值不是产品的 basis-point 模型。S0 在
`docs/plans/2026-07-22-annotation-design.md` §1.4 定义：

```text
maxSad = floor((10000 - minSimilarityBp) * 255 * templatePixels / 10000)
hit    = sadScore <= maxSad
```

产品保存的是 `min_similarity_bp` 0..10000，数值越高要求越相似；M0 保存的是
`maximumAverageSad` 0..255，数值越高越宽松。两者不仅单位和方向相反，S0 还规定了
整数 floor、schema validation 和 manifest persistence。因此 engine 只认 basis points，
`docs/plans/2026-07-23-engine-architecture.md` 明确规定不迁移 M0 的
`--threshold` 记录值。

### 从命中位置到后台点击

匹配被接受后，`hitCenterFrame()` 以 `SadMatch` 左上角和 template 宽高构造
`Rect<FrameSpace>`，取几何中心。`CoordinateTransform::frameToClient()` 再把中心转换为
`Point<ClientSpace>`。

`ObservationLease::forFrame(*captured, config.m_maxActionFrameAge)` 把 frame 的
session、target generation 和过期时刻绑定成投递凭证。点击前
`Machine::ensureTargetUnchanged()` 再做一次 target instance 和
`ResolvedTarget::revalidate()` 复验，然后调用 controller 的：

```text
click(DeliveryTarget, ObservationLease, Point<ClientSpace>, HeldInputs, AuditLog)
```

controller 在投递边缘重新检查 lease session、expiry、generation、坐标有限性、
client bounds 和 Win32 signed-16-bit 可编码范围。任何不一致都在
`modules/controller/source/controller/input-revalidation.cpp` 的
`checkPointerPreconditions()` 中返回 `StaleObservation` 或 `ActionRejected`，
不会调用平台投递。

`click()` 的 pointer 序列是 `WM_MOUSEMOVE`、`WM_LBUTTONDOWN`、
`WM_LBUTTONUP`；最终由
`modules/controller/source/controller/platform/windows-input.cpp` 的
`PostMessageW` 排队。M0 不调用 `SetForegroundWindow`、`SetFocus`、
`SendInput`、`mouse_event`、`keybd_event` 或 `SetCursorPos`。

点击成功后，返回的 `deliveredAt` 成为下一次页面观察的因果屏障。
`waitUntilPresent()` 用 `frameIsCausal(capturedAt, deliveredAt)` 丢弃早于点击完成的
frame，防止旧画面被当成状态转换证据。

点击失败按 `clickFailureDisposition()` 分类：

- `StaleObservation`：记录 `click_retry`，重新捕获；
- `ControllerDisconnected`：无法排队，abort 整个 run；
- 其他错误：立即 fail 当前 step，保留真实 error kind，避免反复重试后伪装成 timeout。

### 状态保护、审计与关闭

`entry/m0-demo/guard.hpp` 的 `GuardPolicy::forMode(Mode::Guard)` 要求比较
foreground 和 cursor。每轮开始时 `runOne()` 取得 `GuardBaseline`：
baseline foreground 必须非空且不是 target；每轮结束再观察，foreground 和 cursor
必须与 baseline 相同。`Mode::Coexist` 则关闭这两项比较。

guard 的意义是验证“后台自动化没有改变用户的全局交互状态”，不是动作授权。
即使业务 step 失败，`combineLoopStatus()` 仍能保留同时发生的 guard violation，
不会让一个失败遮住另一个失败。

每次 controller delivery 都进入 `AuditLog`。shutdown 时
`summarizeAudit()` 要求所有记录的 HWND 等于 target，且 message 属于
`entry/m0-demo/platform/windows-background-messages.cpp` 的 allowlist：
mouse move/down/up、key down/up、`WM_CHAR`、`WM_UNICHAR`。

`runPipeline()` 把可变运行状态收拢进私有 `Machine`：
`ResolvedTarget`、move-only `WgcCaptureSession`、`DeliveryTarget`、
`HeldInputs` 和 `AuditLog`。`shutdownMachine()` 无论主流程成功与否都按顺序：

```text
releaseHeld -> session.close -> audit/summary -> log.flush
```

前一阶段失败不会阻止后续阶段执行，最终保留第一个错误。这保证 partial Down
不会因为另一个错误而跳过 best-effort Up，审计和日志也尽可能落盘。

### input agent 已经不在这里

本节原本记录的提权分进程协议，随实现它的程序一起搬走了。命令语法、queue cursor、
路径围栏规则、observe -> act 热路径与前端盖章，见
[`entry-input-agent.md`](entry-input-agent.md)。

有一件事仍留在这一侧：`requireUnchangedTarget` 是共用的。它现在住在
`entry/input-agent/target-setup.cpp`，demo 的 pipeline 到那里调用它。

## 必须保持的约束

**Fail closed。** 没有 frame、搜索被控制信号中断、comparison budget 耗尽、
ROI/模板不合法、target generation 改变、instance 无法确认、lease 过期、坐标越界、
后台消息越界或审计不干净，都不能被解释为成功投递。具体机制分布在
`captureFresh()`、`searchStopStatus()`、`requireUnchangedTarget()`（现属 input agent）、
`ObservationLease`、controller `checkPointerPreconditions()`、`checkGuard()` 和
`summarizeAudit()`。尤其 `CaptureStalled` 是唯一可重试的 capture absence，
未知 error 不进入 permissive fallback。

**严格后台。** 输入只通过 controller 的 window-targeted message delivery；
guard 检查 foreground/cursor 未变，audit 再检查 target HWND 和 message allowlist。
这形成“投递机制 + 外部状态观察 + 事后记录”三层证据。`Coexist` 会关闭 guard 比较，
因此需要证明严格后台性质的验收必须使用默认 `Guard`，不能只看 click 返回成功。

**观测与动作因果一致。** click 必须携带由同一 captured frame 产生的
`ObservationLease`；session、generation、age 在 delivery edge 重验。动作后的识别还必须
满足 `capturedAt >= deliveredAt`。因此旧 frame 既不能授权当前 click，也不能证明 click
已经引起页面转换。

**目标身份连续。** capture 前后与 click 前都复验 target。
`GenerationBumped` 和 `InstanceUnconfirmed` 成为 `StaleObservation`；
`Lost` 成为 `ControllerDisconnected`。shutdown 若不能确认原 target，会构造 poisoned
session identity，使补偿投递在 controller precondition 处被拒绝，而不是冒险向可能复用的
HWND 发送 Up。

**确定性决策。** 给定同一灰度 frame、template、ROI 和 threshold，SAD 接受边界完全由整数
score 和整数 budget 决定。可选 click pacing 使用 `SplitMix64` 和显式 seed，
固定 seed 产生固定 delay 序列。真实 WGC 到达时间和 OS scheduling 本身不确定；
设计保证的是这些输入一旦给定，分支规则不依赖浮点 confidence 或隐藏随机源。

**所有权与生命周期可见。** `Template` 自有灰度 bytes；`Machine` 自有 capture session、
held state 和 audit；`ObservationLease` 是值凭证，不保存调用者裸引用。
RAII handle wrapper 负责关闭 Win32
process/token handles。

**有界等待与可停止。** transition timeout、capture stall timeout 和 SAD comparison budget
都有显式边界。
console handler 只在 Ctrl-C/Ctrl-Break 时设置 lock-free atomic；pipeline 和 SAD poll
协作检查它。Windows close/logoff/shutdown 不在该 handler 的覆盖范围内，这是
`docs/plans/2026-07-20-m0-demo-port-deviations.md` F-19 记录的冻结差异。

## 与产品代码的关系

入站边是 CLI 和文件资产。调用者提供 window selector、三张 trusted PNG、
三个 frame-space ROI、平均 SAD threshold 以及运行策略。

向 `controller` 的出站边包括：

- discovery：`enumerateCandidates`、`resolveTarget`、`ResolvedTarget::revalidate`；
- DPI：`ensurePerMonitorAwareV2`；
- capture：`WgcCaptureSession`、`WgcCaptureOptions`、`Frame`；
- input：`DeliveryTarget`、`ObservationLease`、`click`、`releaseHeld`、`AuditLog`。

跨这些边传递的是有类型的 target identity、session/generation、client geometry、
frame timestamp/transform、lease 和 delivery audit，而不是裸 HWND 加无约束坐标。

向 `image`/`vision` 的边是 PNG 解码、BGRA crop、灰度转换、`GrayImage` 和
`matchTemplateSad`。M0 自己决定 ROI、comparison budget、timeout poll 和接受阈值；
vision 只返回 `SadSearchOutcome`/`SadMatch`，不决定是否授权点击。

向 `domain`/`core` 的边是 `FrameSpace`、`ClientSpace`、`DesktopSpace`、
`CoordinateTransform`、`MonotonicInstant`、typed IDs、checked arithmetic 和
`Result<...>`/`AutomationErrorKind`。这些类型让空间、时间和身份错误不能靠普通整数
悄悄穿过 pipeline。

日志边由 `entry/m0-demo/log-jsonl.cpp` 的 `JsonlLog` 和 `LogLine` 承担。
每行可带 elapsed time、loop index、frame ID、target generation、SAD score
（字段名仍叫 `confidence`）、lease outcome 和 detail。日志写/flush 失败本身是
`InvalidResource`，不会静默丢失。该 schema 是冻结 demo 的诊断格式，不是 engine 的
产品 trace schema。

annotation/engine 与 M0 没有链接边。当前产品路径是
`annotation -> engine -> entry/umbra-flow`。如果 runner 需要 M0 已证明的 target poison
或补 Up 语义，`docs/plans/2026-07-23-engine-architecture.md` 要求在 adapter 层复制语义，
不链接 `entry/m0-demo`。M0 现在唯一有的那条链接边指向另一个方向：它链接
`input_agent_support` 取共用的 entry 底座，而正是这个方向让产品不欠冻结 demo 任何东西。

## 测试

`tests/CMakeLists.txt` 把以下文件组成 `test-m0-demo`，链接
`${PROJECT_NAME}_m0_demo_support`。input agent 自己的用例搬进了 `test-input-agent`，
见 [`entry-input-agent.md`](entry-input-agent.md)。

- `tests/m0-demo/test-args.cpp` 固定完整参数、defaults、selector、duration、
  click delay/seed、threshold 0..255 和 capture 参数。
- `tests/m0-demo/test-pipeline.cpp` 固定 click error triage、匹配中心、
  per-pixel-average threshold 的 inclusive 边界、动作后 frame 因果屏障、
  guard/status 合并、ROI/template geometry、`RunSummary::passed()` 和
  target/message audit。
- `tests/m0-demo/test-guard.cpp` 固定 Windows integrity RID label、Guard 模式的
  foreground/cursor 比较、非目标基线，以及 Coexist 的关闭语义。
- `tests/m0-demo/test-capture-mode.cpp` 固定 log path 不得 alias 任一 output PNG。
- `tests/m0-demo/test-log-jsonl.cpp` 固定 JSONL 字段顺序/null 表示，
  以及 sink open/write/flush failure。
- `tests/m0-demo/test-pacing.cpp` 固定 delay range、inclusive sampling、
  fixed delay 和 `SplitMix64` seed determinism。
- `tests/m0-demo/test-shutdown.cpp` 固定 release -> close -> audit -> flush 顺序，
  并证明早期错误不跳过后续 cleanup。

这些是纯行为与边界测试，不等于真实 Windows UI 验收。K2 `delta=(0,0)`、不抢焦点、
高完整性目标的实际 `PostMessage` 效果、before/after 画面变化和 lease 在硬件延迟下的
表现，来自前述 2026-07-21 runbook/TODO 记录。反过来，真机验收也不能替代
path race 和 shutdown ordering 的自动测试。

`docs/TODO.md` 仍把遮挡、最小化/`CaptureStalled`、投递中 Ctrl-C 和 10–20 分钟长程
验证列为未完成。因此不能从 M0 已通过的短程场景外推这些性质。

## 退役与迁移

M0 不应继续扩成产品；后续能力从外部替代。

识别与授权已经迁到 `modules/annotation` 和 `modules/engine`。
新 recognizer、page evidence、action target、default click、basis-point threshold 和
runtime manifest 都应遵循 `docs/plans/2026-07-22-annotation-design.md`，不能增加第四张
M0 template 或给 `clickWhenPresent()` 塞 page 特例。

新平台通过产品 runner 的 `IFrameSource`/`IActionSink` 适配器接入。
`docs/plans/2026-07-23-engine-architecture.md` 要求 engine 保持平台无关，
Windows WGC 和 background input 在 entry adapter 组合；Fake port 则在 CI 回放合成
frame 并记录零/有投递。未来第二平台也应实现这些 port，而不是条件编译 M0 pipeline。

提权方案属于 P0-C。产品计划当前允许 `umbra-flow run` 先整体提权；若 UIPI 真机结果
要求 split-process，再把 input agent 的协议安全语义复制到 runner adapter：
append-only framing、strict parser、output confinement、fresh-file create、durable result、
target/lease revalidation 和 agent stop policy。协议在 `entry/input-agent` 里演进，
冻结 demo 不随之演进。

capture robustness 的后续工作以
`docs/plans/2026-07-20-post-port-win32-robustness.md` 为依据，包括遮挡、
stall-timeout 与 lease-age 配对、capture wait cancellation 等。不要在 M0 里局部修出一套
与产品 port 不同的生命周期。

退役条件不是“engine 已有代码”或“CI 已绿”，而是真机能力对齐。
`docs/plans/2026-07-23-engine-architecture.md` 给出的等价检查点是：

1. `umbra-flow run` 经产品 annotation/engine 授权后，在真机严格后台点击成功；
2. K2 capture 仍为 `delta=(0,0)`；
3. lease 过期仍 fail closed、零投递；
4. workbench 生成 manifest 到 runner 消费的 A1+B1 真机端到端闭环完成；
5. Fake IFrameSource 与静态 screenshot regression 固定相同 fail-closed 语义。

`docs/plans/2026-07-20-m0-demo-port-deviations.md` 的冻结声明进一步要求：
在上述真机能力对齐前保留 M0 作为验收参考，对齐后才退役。
截至 `docs/TODO.md` 当前状态，产品真机 smoke 和 workbench -> manifest -> runner
端到端仍待开发者执行，所以这个参考尚不能仅凭产品路径已经存在而删除。
