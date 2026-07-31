# entry/m0-demo：冻结的 M0 真机底座

`entry/m0-demo` 是已经冻结的 Windows 验收程序。它保存了一条在真实机器和高完整性
目标窗口上验证过的最短链路：WGC 后台捕获、
灰度 SAD 模板匹配、基于观测租约的客户端坐标点击、严格后台 `PostMessageW` 投递、
前后台与光标 guard、投递审计，以及失败后的输入补偿和有序关闭。

本文中的“冻结”以
`docs/plans/2026-07-20-m0-demo-port-deviations.md` 和
`docs/plans/2026-07-23-engine-architecture.md` 为准：不要继续把产品能力加进这个目录，
也不要让新的产品入口链接它的实现。需要复用时，应复制已经验证的安全语义到
`engine`/runner adapter，并由产品契约重新承载。

## 它验证什么

M0 demo 拥有三种进程入口，分派点在 `entry/m0-demo/main.cpp`。

- 默认入口解析 `Args`，选择一个真实窗口，创建 `WgcCaptureSession` 和
  `DeliveryTarget`，加载 `home`、`result`、`reset` 三张模板，然后调用
  `runPipeline`。
- `capture` 子命令解析 `CaptureArgs`，只做目标发现、WGC 捕获和 PNG 输出；
  实现在 `entry/m0-demo/capture-mode.cpp` 的 `runCapture`。
- `input-agent` 子命令解析 `InputAgentArgs`，常驻轮询 append-only JSONL 命令文件，
  执行 `capture`、`click`、`quit`；实现在 `entry/m0-demo/input-agent.cpp` 的
  `runInputAgent`。

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
- 不把 `input-agent` 文件协议当成产品 IPC。它是一次 UAC 后由非提权驱动端持续操纵
  提权代理的验收夹具。

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
`capture-mode.cpp` 和 `input-agent.cpp` 的 capture result 都按这个定义记录尺寸差。
`delta=(0,0)` 是该目标和该真机配置的验收事实，不是所有 WGC 目标的普遍保证。

## 执行流程

### 入口参数与组合

`entry/m0-demo/args.hpp` 定义三组值类型。

- `SelectorArgs` 保存可选的 PID、HWND、window class 和 title。
- `Args` 保存三组 template/ROI、`m_threshold`、`Mode`、循环数、最大 frame age、
  capture stall timeout、可选 click delay、seed 和 log path。
- `CaptureArgs` 保存 selector、输出路径、frame 数、间隔和 log path。
- `InputAgentArgs` 保存 HWND、queue/results 文件、output directory 和 idle timeout。

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

### 提权 input-agent 分进程协议

真机目标完整性高于普通驱动进程时，验收采用 split-process：
开发者用 `Start-Process -Verb RunAs` 手工启动一次
`m0-demo input-agent`；之后非提权一侧只向 queue 追加命令并读取 results。
提权边界因此集中在一个长期存活的 agent，而不是每次点击都触发 UAC。

> **agent 已拆成 drive 层与 annotation 层**（2026-07-31，接在 `65f43d8` 给服务循环开出接缝
> 之后）。三个文件、三种关注点，每一层都能脱离下面两层单测：
>
> - `input-agent-loop.{hpp,cpp}` —— `runInputAgentQueueLoop`，服务循环，也是唯一决定「一次 run
>   如何结束」的地方，架在 `IInputAgentSession` 端口之上。`InputAgentResultWriter` 也在这里，
>   前端盖章就盖在它上面。
> - `input-agent-annotation.{hpp,cpp}` —— `AnnotationSession`，annotation 层：输出路径围栏、
>   before/after 取景、PNG 编码、results 行的形状。将来标注会话长出的动词——读一块区域、
>   由读出来的东西提议一个元素——归在这里。
> - `input-agent-drive.{hpp,cpp}` —— `IInputAgentDrive` 与 `WindowInputAgentDrive`：把一次输入
>   按一次观察投给窗口，再拿回一帧。它不认识文件、不编码图像、不解析命令，所以标注会话新造的
>   东西一样都到不了它这里。
>
> `IInputAgentDrive` 刻意不是 `engine::IActionSink`，理由与 `IInputAgentSession` 不是它相同：
> 那个端口说的是 engine 的词汇——针对**已标注元素**的一次已授权动作——而且住在这个可执行文件
> 不链接的模块里。标注会话进行时什么都还没标注（量屏幕正是产出标注的过程），所以根本没有元素
> 供那种端口点名。
>
> **每一条 results 行都盖着产出它的前端**：行首是 `"front_end":"annotation"`，值来自
> `trace::FrontEnd::Annotation`，拼写来自 `trace::frontEndWireName`。`m0_demo_support` 为这一个
> 类型链接了 `modules/trace`。agent 不写 `umbraflow-trace/v1` 的行——该 schema 每一行都带
> `runId` 与 `generationId`，而够不到项目的标注会话两者皆无；results 文件就是它的全部证据流，
> 盖章由 writer 而不是各个 serializer 完成，理由与「`trace::TraceRecorder` 而不是各个发射方
> 拥有那枚章」是同一条。

`entry/m0-demo/input-agent-protocol.hpp` 定义五个 variant：

- `InputAgentCaptureCommand{output}`；
- `InputAgentClickCommand{x, y, outputBefore, outputAfter, settle}`；
- `InputAgentKeyCommand{key, outputBefore, outputAfter, settle}`；
- `InputAgentScrollCommand{x, y, delta, outputBefore, outputAfter, settle}`；
- `InputAgentQuitCommand`。

对应的 JSON object 只接受 `op=capture|click|key|scroll|quit` 及各自的精确字段集。
`parseInputAgentCommand()` 拒绝超过 64 KiB、非法 UTF-8、重复/未知字段、错误 JSON number、
空路径、NUL、非有限坐标和超过 5000 ms 的 settle。动作默认 settle 为 400 ms。

`delta` 的单位是整数格（notch），不是 `WHEEL_DELTA` 原始单位；解析时经 `WheelDelta::create`
解析，所以 0、小数、以及原始值放不进 `wParam` 有符号 16 位字的格数，都会在命令入队前被拒绝，
与 `key` 经 `KeyInput::fromName` 解析是同一种做法。

`InputAgentQueueReader` 按 offset 增量读取 append-only queue，接受 LF/CRLF，
保留未完成行；queue 被截断或 pending command 超过 1 MiB 时 fail closed。
`ResultWriter` 每写一条 JSONL result 就 `flushDurably()`。

agent 的文件权限面同样是协议的一部分：queue 与 results 必须不同、且都在
output directory 之外；截图路径必须被 confinement 检查限制在 output directory 内，
不得 alias IPC 文件。`platform::FileWriter::createExclusive()` 通过已经验证并保持打开的
目录 handle 链做相对 `NtCreateFile(FILE_CREATE)`，拒绝 overwrite、reparse escape、
alternate data stream 和目录重命名竞态。

`executeClick()` 的 observe -> act 热路径如下，`executeScroll()` 走同一条，
只是把 `click` 换成 `scroll`：

```text
reserve fresh before/after outputs
-> capture immutable before Frame
-> ObservationLease::forFrame
-> validateInputAgentPointerAction
-> ResolvedTarget::revalidate
-> requireUnchangedTarget
-> WgcCaptureSession::validateTargetInstance
-> click
-> encode/write before PNG
-> settle
-> capture/encode/write after PNG
```

before PNG 的编码和持久化刷新放在点击之后。真机曾发现，把 1600×900
BGRA 编码、写盘、`FlushFileBuffers` 放在 capture 与 click 之间，会消耗 750 ms
lease budget 并制造非预期的过期。移动后仍保存同一个 immutable pre-click `Frame`，
但不再让取证 I/O 延长 observe -> act。

命令解析错误会写一条失败 result 后继续；target revalidation/instance 失败则写 result
并停止 agent。每条命令完成后 `clearInputAgentCommandAudit()` 清空 audit，
防止长驻代理的记录无限增长。

## 必须保持的约束

**Fail closed。** 没有 frame、搜索被控制信号中断、comparison budget 耗尽、
ROI/模板不合法、target generation 改变、instance 无法确认、lease 过期、坐标越界、
后台消息越界或审计不干净，都不能被解释为成功投递。具体机制分布在
`captureFresh()`、`searchStopStatus()`、`requireUnchangedTarget()`、
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

**目标身份连续。** capture 前后、click 前、input-agent click 前都复验 target。
`GenerationBumped` 和 `InstanceUnconfirmed` 成为 `StaleObservation`；
`Lost` 成为 `ControllerDisconnected`。shutdown 若不能确认原 target，会构造 poisoned
session identity，使补偿投递在 controller precondition 处被拒绝，而不是冒险向可能复用的
HWND 发送 Up。

**确定性决策。** 给定同一灰度 frame、template、ROI 和 threshold，SAD 接受边界完全由整数
score 和整数 budget 决定。可选 click pacing 使用 `SplitMix64` 和显式 seed，
固定 seed 产生固定 delay 序列。真实 WGC 到达时间和 OS scheduling 本身不确定；
设计保证的是这些输入一旦给定，分支规则不依赖浮点 confidence 或隐藏随机源。

**所有权与生命周期可见。** `Template` 自有灰度 bytes；`Machine` 自有 capture session、
held state 和 audit；`InputAgentQueueReader` 自有 path、offset 和 pending buffer；
Windows `FileWriter` 以 `unique_ptr<State>` 独占 native handles。
`Frame` 值在 input-agent 中跨 click 保留，用同一 immutable frame 延后编码；
`ObservationLease` 是值凭证，不保存调用者裸引用。RAII handle wrapper 负责关闭 Win32
process/token/file handles。

**有界等待与可停止。** transition timeout、capture stall timeout、SAD comparison budget、
input-agent settle 上限、idle timeout 和 queue/command byte limit 都有显式边界。
console handler 只在 Ctrl-C/Ctrl-Break 时设置 lock-free atomic；pipeline 和 SAD poll
协作检查它。Windows close/logoff/shutdown 不在该 handler 的覆盖范围内，这是
`docs/plans/2026-07-20-m0-demo-port-deviations.md` F-19 记录的冻结差异。

## 与产品代码的关系

入站边是 CLI 和文件资产。调用者提供 window selector、三张 trusted PNG、
三个 frame-space ROI、平均 SAD threshold 以及运行策略；input-agent 调用者提供一个
已存在的 append-only queue、results path 和受限 output directory。

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
`annotation -> engine -> entry/umbra-flow`。如果 runner 需要 M0 已证明的 target poison、
补 Up 或提权代理语义，`docs/plans/2026-07-23-engine-architecture.md` 要求在 adapter 层
复制语义，不链接 `entry/m0-demo`。

## 测试

`tests/CMakeLists.txt` 把以下文件组成 `test-m0-demo`，链接
`${PROJECT_NAME}_m0_demo_support`。

- `tests/m0-demo/test-args.cpp` 固定完整参数、defaults、selector、duration、
  click delay/seed、threshold 0..255、capture 参数和 input-agent 参数的拒绝边界。
- `tests/m0-demo/test-pipeline.cpp` 固定 click error triage、匹配中心、
  per-pixel-average threshold 的 inclusive 边界、target revalidation、动作后 frame
  因果屏障、guard/status 合并、ROI/template geometry、`RunSummary::passed()` 和
  target/message audit。
- `tests/m0-demo/test-guard.cpp` 固定 Windows integrity RID label、Guard 模式的
  foreground/cursor 比较、非 target baseline，以及 Coexist 的关闭语义。
- `tests/m0-demo/test-input-agent.cpp` 固定严格 JSON command grammar、UTF-8、
  settle 上限、path confinement、增量 line framing、queue truncation/size limit、
  handle-relative exclusive output、per-command audit 清理、client bounds 和 stale
  generation 拒绝。
- `tests/m0-demo/test-input-agent-loop.cpp` 固定一次 run 结束的各种方式——停机命令、
  解析不出的行、idle 超时、一次应答是否重启倒计时——以及每一条应答都带前端盖章，
  包括循环自己写的那两条。
- `tests/m0-demo/test-input-agent-annotation.cpp` 固定拆分买到的那道接缝：越界的输出在
  drive 被要求观察之前就被拒；before 帧只在投递之后才编码，于是 observe->act 窗口里不放慢活；
  窗口被换掉是唯一会结束整次 run 的失败；`delivered` 跟着 drive 的答案走，而不是会话自己
  设的一个旗标。
- `tests/m0-demo/test-capture-mode.cpp` 固定 log path 不得 alias 任一 output PNG。
- `tests/m0-demo/test-log-jsonl.cpp` 固定 error kind/context/native origin、
  JSONL 字段顺序/null 表示，以及 sink open/write/flush failure。
- `tests/m0-demo/test-pacing.cpp` 固定 delay range、inclusive sampling、
  fixed delay 和 `SplitMix64` seed determinism。
- `tests/m0-demo/test-shutdown.cpp` 固定 release -> close -> audit -> flush 顺序，
  并证明早期错误不跳过后续 cleanup。

这些是纯行为与边界测试，不等于真实 Windows UI 验收。K2 `delta=(0,0)`、不抢焦点、
高完整性目标的实际 `PostMessage` 效果、before/after 画面变化和 lease 在硬件延迟下的
表现，来自前述 2026-07-21 runbook/TODO 记录。反过来，真机验收也不能替代
input-agent parser、path race 和 shutdown ordering 的自动测试。

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
要求 split-process，再把 M0 input-agent 的协议安全语义复制到 runner adapter：
append-only framing、strict parser、output confinement、fresh-file create、durable result、
target/lease revalidation 和 agent stop policy。协议可以演进，但冻结 demo 不随之演进。

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
