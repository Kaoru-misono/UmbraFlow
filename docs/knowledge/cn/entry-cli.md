# entry/cli 架构知识

`entry/cli` 是 `umbra-flow` 的命令行入口，也是 Windows 上的组合入口。
它把平台无关的 `engine` 端口接到 `controller` 的真实捕获与后台输入能力上，
并负责参数、资源名称和进程退出码。

## 入口职责

该目录拥有四类产品边界。

第一类是进程边界。`entry/cli/main.cpp` 把 `argv` 复制成拥有所有权的
`std::vector<std::string>`，识别 `run` 子命令，输出 usage、成功报告或错误，
并把所有结果收敛为整数退出码。未带子命令时只打印项目名和 usage；当前没有
隐式执行任务的默认行为。

第二类是命令边界。`entry/cli/args.hpp` 的 `RunArgs` 是一次 `run` 的完整值对象，
`entry/cli/args.cpp` 的 `parseRunArguments` 负责把十个成对的 value flags 转成
路径、名称、预算和单调时钟 duration。解析成功后没有“未设置”的可选字段：
四个必填项必须非空，其余字段已采用安全默认值。

第三类是资源名称边界。发布后的 runtime manifest 使用稳定 ID，而人输入
`--page` 和 `--action` 时使用名称。`entry/cli/name-resolution.hpp` 提供
`resolvePageName` 和 `resolveActionName`，在已经验证的
`annotation::RecognitionCatalog` 上完成 name-to-ID 翻译。

第四类是组合边界。Windows 实现 `entry/cli/run-windows.cpp` 的 `runProduct`，
选择一个真实窗口，创建捕获、输入、trace 三个 adapter，再把它们连到
`engine::EngineSession`。非 Windows 构建使用
`entry/cli/run-unsupported.cpp`，保留同一个产品二进制和可测试命令层，但
`runProduct` 明确返回 `UnsupportedCapability`。

该目录不负责以下工作：

- 不拥有识别、页面判定、动作授权或 observation 生命周期；这些属于
  `modules/annotation/` 与 `modules/engine/`。
- 不拥有 Win32 窗口枚举、WGC、目标 generation、lease 校验或消息投递；
  这些属于 `modules/controller/`，CLI 只做薄适配与装配。
- 不定义任务语言或通用工作流。当前 `runProduct` 是一个固定 smoke flow：
  等待一个页面、寻找一个 action target、存在时点击一次。
- 不读取 authoring 文档，也不生成 manifest 或模板。它只消费
  `engine::loadRuntimeProject` 能加载的已发布项目。
- 不提供前台或全局输入降级。后台投递失败就是产品失败。
- 不承担守护进程、托盘、调度或多任务生命周期；当前设计是一进程一次
  `run`。

这条边界解释了为什么目标发现留在 entry，而没有进入 `engine`：
`engine::FrameSource` 接收的是“已经绑定的一个目标”，从而保持平台无关；
窗口标题选择、DPI 声明和 Win32 几何则是产品在 Windows 上如何取得该端口的
策略。

## 命令执行流程

### 命令行入口

`entry/cli/main.cpp` 的 `dispatch` 只有两个公开产品路径：

- 空参数：打印 `g_projectName` 与 `runUsageText()`，返回 `0`。
- 首参数为 `run`：把剩余参数交给 `dispatchRun`。

其他首参数被视为 unknown subcommand，打印错误与 usage，返回 `1`。
`main` 还检查 `argumentCount` 能否安全转换且不为零，并在最外层捕获
`std::exception` 和未知异常；异常不会穿过进程边界。

`entry/cli/args.hpp` 的 `RunArgs` 对应以下真实 CLI 表面：

- `--project DIR`、`--selector TITLE-SUBSTRING`、`--page NAME`、
  `--action NAME` 必填。
- `--timeout SEC` 默认 30 秒，是 `waitForPage` 的总期限。
- `--poll MS` 默认 250 ms，且只接受 1 到 60000 ms。
- `--budget N` 默认 `1 << 28`，限制一次识别的像素比较数。
- `--recognition-timeout MS` 默认 2000 ms，是每次识别的 deadline。
- `--max-frame-age MS` 默认 750 ms，决定 observation lease 可用于动作的时长。
- `--trace PATH` 默认 `umbra-flow-trace.jsonl`。

整数使用 `std::from_chars` 完整消费输入，duration 在转换前检查目标表示范围。
flag 以“flag 后紧跟 value”的二元组读取；未知 flag、缺值、非整数和越界值都
返回 `InvalidResource`。因此后续组合只处理类型化值，不重复解析字符串。

### 离线加载与名称解析

`runProduct` 首先调用
`modules/engine/source/engine/runtime-loader.hpp` 的
`engine::loadRuntimeProject`。该加载器读取
`generated/annotations.runtime.toml`，在读取前应用 16 MiB 大小上限，再按
manifest 引用读取 `assets/templates/<hash>.png`，由
`annotation::RecognitionRuntime::create` 校验 hash closure 并解码模板。

加载成功后，CLI 才通过 `resolvePageName` 和 `resolveActionName` 查询
`loaded.m_runtime.manifest().catalog()`。页面名称只在 `catalog.pages()` 中
匹配；动作名称只在 `catalog.recognizers()` 中匹配
`annotation::AnnotationType::ActionTarget`，同名的 page anchor 不能被当成动作。
匹配是精确、区分大小写的字符串比较。

`modules/annotation/source/annotation/catalog.cpp` 在
`RecognitionCatalog::create` 中已经拒绝重复 page name、recognizer name，
以及跨资源冲突的 ID 和名称，所以线性扫描不会面对“同名取第一个”的歧义。
失败信息按 catalog 顺序列出所有可用页面或 action target，给操作者直接可改的
诊断。

这段离线工作在访问桌面之前完成：损坏的 manifest、缺失模板或未知名称不会
先声明 DPI、注册 console handler、枚举窗口或创建捕获资源。

### Windows 组合序列

离线前置成功后，`entry/cli/run-windows.cpp` 严格按以下顺序装配：

1. `modules/controller/source/controller/dpi.hpp` 的
   `ensurePerMonitorAwareV2` 声明 per-monitor-aware V2。后续 client size、
   client origin 与 DPI fingerprint 才处在一致的坐标解释下。
2. `entry/cli/platform/windows-console-cancellation.hpp` 的
   `platform::ConsoleCancellation::install` 注册 Ctrl-C/Ctrl-Break handler，
   并取得供 engine 使用的 `std::stop_token`。
3. `enumerateCandidates` 枚举窗口；内部 `selectCandidate` 要求标题包含
   `RunArgs::m_selector` 的候选恰好一个。零个或多个都返回
   `TargetUnavailable`，不会依赖枚举顺序猜测。
4. 用所选 handle 构造 `TargetSelector`，再由 `resolveTarget` 得到
   `ResolvedTarget`，读取 `ClientSize`、`WindowHandle` 与当前
   `TargetGeneration`。当前一次性进程使用固定 `SessionId{1}`。
5. 从 resolved client width/height 与候选窗口的 DPI 创建实时
   `annotation::ProjectFingerprint`。它不是替换 manifest fingerprint；
   两者在 recognition 和 action authorization 中必须相等。
6. `platform::clientOriginDesktop` 通过 `ClientToScreen` 求 client `(0, 0)`
   的 desktop-space 原点；与 client extent 一起创建 `ClientGeometry`，再创建
   `WgcCaptureSession`。
7. 用同一 handle、session、generation 和 client extent 创建
   `DeliveryTarget`，保证捕获身份与投递身份来自同一次 target resolution。
8. 创建 `FileTraceSink`、`platform::WgcFrameSource` 和
   `platform::ControllerActionSink`，把 CLI 参数填入
   `engine::EngineSessionConfig`，最后调用 `engine::EngineSession::create`。

`EngineSessionConfig` 携带 live fingerprint、像素预算、单次识别期限、
最大帧年龄和 cancellation token。三个 adapter 以
`std::unique_ptr<engine::FrameSource>`、`std::unique_ptr<engine::ActionSink>`、
`std::unique_ptr<engine::TraceSink>` 移交给 session；从这一点起 session 拥有
端口生命周期。

执行阶段调用
`modules/engine/source/engine/session.hpp` 的
`EngineSession::waitForPage(pageId, timeout, pollInterval)`。返回的 `PageWait`
把命中的 `ResolvedPage` 与产生它的同一 `Observation` 配对，CLI 随后在该
observation 上调用 `findAction(actionId)`，不会为动作另抓一帧。

动作缺席是正常的 Tier-A 结果：
`findAction` 返回成功的空 `std::optional<ActionFound>`，CLI 生成
`RunReport{m_actionDelivered = false}`。动作存在时，
`EngineSession::act` 消费 observation，执行授权、frame-to-client 变换和投递，
返回 `ActReceipt`；CLI 把实际 client click point 写入成功报告。

### 三个端口适配器

`entry/cli/platform/wgc-frame-source.hpp` 的 `platform::WgcFrameSource` 按值拥有
move-only `WgcCaptureSession`。`capture()` 原样转发到 session，
`validateTargetInstance()` 也原样转发。这个薄层没有复制 capture 规则：
frame ID、target generation、content-size 变化失效和 capture stall 都继续由
`modules/controller/source/controller/capture.hpp` 的实现决定。

`entry/cli/platform/controller-action-sink.hpp` 的
`platform::ControllerActionSink` 按值拥有 `DeliveryTarget`，并为该目标持有
`HeldInputs` 与 `AuditLog`。它的 `click(point, lease)` 把
`ObservationLease` 不变地传给
`modules/controller/source/controller/input.hpp` 的 `uf::click`。
controller 在投递时再次检查 session、generation、到期时间、坐标有限性、
client bounds 与 Win32 signed-16-bit 编码范围。

如果 pointer down/up 链中任一步失败，adapter 保留原始错误，并调用
`releaseHeld` 排空可能残留的 held input。补偿 release 的失败只追加 context，
不会遮蔽最初的 click 失败。`AuditLog` 和 held state 都是 adapter 的拥有型成员，
不会借用 `runProduct` 栈上的临时对象。

`entry/cli/file-trace-sink.hpp` 的 `FileTraceSink` 拥有 `std::ofstream`。
`create` 以 binary + trunc 模式打开路径并返回
`std::unique_ptr<engine::TraceSink>`；打开失败是 `IoFailure`。每次 `emit` 使用
`engine::serializeTraceEvent` 写一条 JSONL、追加换行并立即 `flush`，写或 flush
失败同样向 engine 返回 `IoFailure`。这避免事件跨 emit 留在 C++ stream buffer
中，但代码没有声明文件系统级 durable sync 保证。

### 退出码契约

`entry/cli/run.hpp` 的强类型 `ExitCode` 统一定义退出码，`run.cpp` 负责把结构化错误
映射到该枚举；只有 `main.cpp` 在进程边界使用 `std::to_underlying` 转为 `int`：

| 退出码 | 含义 |
|---:|---|
| `0` | 空命令的帮助路径，或动作成功投递 |
| `1` | unknown subcommand、参数/资源错误、unsupported host、绝大多数运行失败或未捕获异常 |
| `2` | `TargetCompatibilityUnverified` |
| `3` | 页面已解析，但指定 action target 在该 observation 中缺席 |
| `4` | `Timeout` |
| `5` | `Cancelled`，或运行失败返回时 console stop 已被请求 |

所有 CLI 路径都返回 `ExitCode`，避免 `EXIT_FAILURE` 与裸数字分散表达同一契约。
`exitCodeForError(error, stopRequested)` 先检查 `stopRequested`，再检查
`AutomationErrorKind`。因此 Ctrl-C 发生在阻塞 capture 等步骤时，即使底层最终
冒出 `CaptureStalled`、`IoFailure` 或 `Timeout`，操作者的取消意图仍优先报告
为 `5`。参数解析早于 handler 安装，解析错误明确以 `stopRequested=false`
映射；非 Windows 实现也始终报告未收到取消。

错误文本由 `formatRunError` 组合 automation kind、message、全部 context，
以及存在时的 native error category/value。这是 CLI 的单行诊断格式，不改变
底层 `Error` 的分类。

## 必须保持的约束

**Fail-closed。** 资源加载和名称解析在桌面副作用前完成；窗口 substring 必须
唯一；DPI 声明、几何创建、WGC、delivery 和 trace 任一步失败都通过
`UF_TRY` 立即终止。live fingerprint 不匹配 manifest 时，
`RecognitionRuntime` 拒绝识别，`authorizeCoordinateAction` 也再次拒绝动作。
动作 authorization 之后，`EngineSession::act` 还会在 sink 调用前通过
`FrameSource::validateTargetInstance` 复验绑定实例，失败时零投递。

**两层 stale-observation fence。** engine 层用 observation 携带的
`ObservationLease`、`ResolvedPage` 和 `ActionDetection` 调用
`authorizeCoordinateAction`；CLI adapter 又把同一 lease 转交 controller，
使其在 post 时复验 session、generation 与 age。任何一层失败都不发送点击。
成功点击后 observation 在可能失败的 post-click trace 之前即被标记 invalid，
避免 trace 失败诱发重复投递。

**确定性。** 参数转换不依赖 locale；name resolution 使用 catalog 的稳定顺序
和唯一名称；窗口选择拒绝多匹配而不取“第一个”；同一 `Observation` 同时承载
页面与动作证据；默认点击点和坐标变换由 annotation/engine 计算。trace 使用
固定 schema serializer。真实窗口内容与到达时序本身不是确定输入，但入口不再
额外引入隐式选择。

**所有权与生命周期可见。** `RunArgs`、`LoadedRuntime`、三个 adapter 及其系统
资源都按值或 `unique_ptr` 移动。`ConsoleCancellation` 以 RAII 管理 handler
registration；实际 `stop_source` 是 module-static process-lifetime 对象，所以
handler 注销后退出码边界仍能读取 stop 状态。该 source 一旦停止不会复位，这与
“每进程恰好一次 run”契约一致。

`engine::Observation` 内部有指向其 `EngineSession` 的非 owning back-reference，
因此必须短于 session；`runProduct` 的局部作用域满足这一点。observation
move 后源对象失效，`act` 又按 rvalue 消费它，类型和运行时 flag 一起限制复用。

**严格后台。** CLI 不调用 focus、activation 或全局输入 API。
`ControllerActionSink` 最终进入
`modules/controller/source/controller/platform/windows-input.cpp` 的
`PostMessageW` 路径，只向解析出的单一 HWND 投递整数 mouse message，并拒绝
null 与 `HWND_BROADCAST`。目标失活、消息投递失败或兼容性无法确认时都失败，
不存在“为了成功”切到前台输入的 fallback。

**Trace 是正确性路径的一部分。** `engine::TraceSink::emit` 返回 `Status`，
session creation 的 `SessionStarted`、识别失败、授权与投递相关事件都可令操作
失败。`FileTraceSink` 也不吞写入错误。这使“无法留下所要求的证据”成为显式产品
失败，而不是不可见的 best-effort 丢日志。

## 依赖关系

入站边是 shell/operator → CLI。跨边界的是字符串参数、项目路径、目标标题
substring、名称和退出码；CLI 在这一层负责可读诊断与默认值。

向 `engine` 的出站边通过 `entry/CMakeLists.txt` 中的
`${PROJECT_NAME}_cli_support` 建立。跨边界的是：

- `LoadedRuntime` 与 `EngineSessionConfig`；
- 三个拥有型 port implementation；
- `PageId`、`RecognizerId`、`RunReport` 和结构化 `Error`。

`engine` 不看到 HWND、console handler、文件选择语法或标题 substring。
反方向，CLI 不解释 recognizer evidence、page outcome 或授权规则，只驱动
`waitForPage`、`findAction`、`act` 的公开表面。

向 `controller` 的出站边只存在于 Windows build。跨边界的是 resolved
`WindowHandle`、`ClientSize`、`Dpi`、`TargetGeneration`、`ClientGeometry`、
`WgcCaptureSession`、`DeliveryTarget` 和 `ObservationLease`。WGC 产生的
`Frame` 带 session/generation/frame identity 与 coordinate transform，
最终 click 又携同一 identity lineage 回到 controller。

向 `annotation` 的协作主要经 `engine` 间接发生。CLI 直接接触
`RecognitionCatalog` 只为名称解析，直接创建 `ProjectFingerprint` 只为表达
真实目标的尺寸/DPI。它不修改 catalog，也不创建 authoring resource。

`${PROJECT_NAME}_cli_support` 的拆分是测试架构的一部分，而非单纯构建便利。
`entry/cli/main.cpp` 只保留薄进程壳；参数解析、name resolution、file trace、
错误格式化和退出码映射进入 static library，供 executable 与 `test-cli` 共同
链接。Windows 时 library 再加入真实 adapter 并链接 controller；其他平台加入
`run-unsupported.cpp`。因此平台无关契约能在没有 Windows desktop 的 CI 中测试，
而产品 executable 不需要导出内部函数。

## 测试

`tests/cli/test-args.cpp` 钉住完整 flag happy path、所有可选默认值、四个必填项、
unknown/missing/non-integer 输入、poll 的 1/60000 ms 边界，以及退出码映射。
其中 cancellation-priority 用 `CaptureStalled`、`Timeout`、`IoFailure`
配合 `stopRequested=true`，直接防止底层错误覆盖 Ctrl-C 意图。

`tests/cli/test-name-resolution.cpp` 用真实 `RecognitionCatalog` 钉住 page 和
action name-to-ID，并确认 page anchor 不能解析成 action；未知名称的 available
列表也是测试契约的一部分。

`tests/cli/test-file-trace-sink.cpp` 钉住“一次 emit 一条序列化 JSONL”及顺序，
并确认无法打开的路径返回 `IoFailure`。固定 JSON schema 本身由
`tests/engine/test-trace.cpp` 覆盖。

CLI adapter 的安全语义由下游测试分层固定：

- `tests/engine/test-session.cpp` 覆盖完整 observe→resolve→find→act、
  fingerprint mismatch、未授权 action、过期 lease、失效/跨 session
  observation、投递边缘 target revalidation、取消和 `waitForPage` 超时。
- `tests/engine/test-runtime-loader.cpp` 覆盖发布项目加载、坏 manifest、缺模板、
  hash mismatch 与 manifest 大小上限。
- `tests/controller/test-input-revalidation.cpp` 覆盖 lease session/generation/age、
  坐标范围及 dead HWND；`tests/controller/test-input-held.cpp` 覆盖 held state
  排空及 release 失败；`tests/controller/test-input.cpp` 覆盖 delivery target
  与 held-action identity。
- `tests/controller/test-capture-wgc.cpp` 与
  `tests/controller/test-capture-stall.cpp` 覆盖 WGC adapter 所转发的 frame ID、
  geometry invalidation、options 和 stale arrival 行为。
- `tests/controller/test-target.cpp`、`tests/controller/test-discovery.cpp` 与
  `tests/controller/test-dpi.cpp` 固定 target resolution、generation、Win32
  discovery 错误和 DPI fail-closed 分类。

`tests/CMakeLists.txt` 的 `test-cli` 只直接编入三个平台无关 CLI 测试文件并链接
`${PROJECT_NAME}_cli_support`。当前没有直接单元测试覆盖 `main`/`dispatch`、
`selectCandidate`、console registration、client-origin adapter 或整条真实 Windows
组合；这些属于真机 smoke/E2E 验证面，不能用现有离线测试的绿色替代。

## 后续扩展

`docs/plans/2026-07-23-engine-architecture.md` 是当前 engine/CLI 组合的直接权威。
它明确把 CLI 定位为 Phase 3 的 Windows 组合根，把固定 C++ 流程定位为 smoke
flow，并把任务语言留给 Luau。因此新增任务编排应接到
`EngineSession` 的 observe/act/wait 表面或 script binding，不应继续在
`runProduct` 中堆出一套声明式语言。

同一计划还列出以下扩展点：

- P3 第二平台通过实现相同 `FrameSource`、`ActionSink`、`TraceSink` 接入；
  CLI host implementation 可以替换 `run-unsupported.cpp`，engine 无需感知平台。
- B2 Luau 对 `Observation`、observe/find/act/wait 做 1:1 binding；现有 C++
  API 形状就是为避免届时重构而保留。
- D6/P1 弹窗处理接在 `EngineSession::sweepKnownPopups` 的现有 no-op hook，
  而不是塞进窗口发现或 CLI argument parsing。
- P0-C 若 UIPI 真机验证要求分进程提权，计划要求把 m0-demo input-agent 的协议
  语义复制到 runner adapter 层，而不是链接已经冻结的 m0-demo。

`docs/plans/2026-07-21-product-form-and-roadmap.md` 把当前形态定义为“P0 CLI
单任务、严格后台、可靠取消”，并把 P2 定义为托盘常驻 App。届时可复用
`EngineSession` 和三个端口的组合边界，但进程生命周期、任务列表、计划任务和
UI 状态属于新的 app shell，不应反向成为 CLI 或 engine policy。

该 roadmap 的 B3 还要求硬取消与长程稳定性；
`docs/plans/2026-07-20-post-port-win32-robustness.md` 记录了 capture wait 取消、
stall timeout 与 lease age 配对等仍开放的 controller 问题。相关改进应落在
WGC/controller 能力及其 adapter 连接处，并保持当前原则：取消或 capture
不确定性只能终止动作，不能放宽 lease 或改用前台输入。

分辨率自适应、OCR、pause/resume 和常驻调度也被上述计划明确放在当前 Phase 3
之外。扩展它们时应沿既有边界加入新的 recognition/engine/app 能力；尤其不能
通过在 CLI 中伪造 live fingerprint 来绕过当前尺寸/DPI fail-closed 契约。
