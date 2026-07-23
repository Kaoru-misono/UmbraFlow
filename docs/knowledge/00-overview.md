# UmbraFlow 全系统架构总览

本文描述 2026-07-24 仓库中的可执行架构，并用当前产品计划解释这些边界为什么存在。模块图以 `docs/ARCHITECTURE.md` 为准；S0 标注契约以 `docs/plans/2026-07-22-annotation-design.md` 为准；A/B 交付节奏以 `docs/plans/2026-07-21-product-form-and-roadmap.md` 为准。计划与代码不完全重合时，本文把代码现状写在正文，把尚未落地的能力放在“扩展接缝”。

## 职责与边界

UmbraFlow 的目标不是一般桌面宏，而是“严格后台、可追踪、由视觉证据授权”的个人游戏自动化。这个目标把系统自然切成两半：平台无关部分回答“看到了什么、是否允许动作”，Windows 部分只回答“如何抓取这个目标、如何把已经授权的动作投递给它”。二者只在 `entry/` 组合，避免识别策略依赖 HWND，也避免 controller 知道 page 或 recognizer。

`docs/ARCHITECTURE.md` 中依赖箭头表示“左侧 consumer 依赖右侧 provider”。主干是：

```text
core
  ↑
domain
  ↑
vision      image
  \          /
   annotation
       ↑
     engine

controller (Windows) -> core, domain
script               -> core, domain
entry/cli             -> engine + controller
entry/workbench       -> annotation + engine + controller + image
```

`vision` 与 `image` 是同层兄弟，不互相依赖；图中的向上排列表示产品语义逐层增加：

- `modules/core/manifest.txt` 没有 link dependency，只提供 `Result`、checked arithmetic、strong type、monotonic time、UTF-8 与 contract 等机制；它刻意不含游戏、页面、图像或 Windows policy。
- `modules/domain/manifest.txt` 只依赖 core，拥有跨模块传递的 `Frame`、坐标空间、`Detection`、`ObservationLease`、`FrameId`、`TargetGeneration` 和 `AutomationErrorKind`；这些值描述观察/动作因果链，但不解释画面语义。
- `modules/vision/manifest.txt` 只依赖 core/domain，拥有确定性的 Gray8 转换和 SAD matcher；它不知道 PNG、page、阈值业务规则或输入投递。
- `modules/image/manifest.txt` 只依赖 core/domain，并把 stb 放在 private FFI 后面；它负责 PNG、channel layout 和矩形 crop，不决定一张 crop 是什么 recognizer。
- `modules/annotation/manifest.txt` 依赖 core/domain/vision，private 依赖 image；它把像素算法提升成项目 fingerprint、recognizer、page signature、证据、授权、双文档和 deterministic compiler。
- `modules/engine/manifest.txt` 依赖 core/domain/annotation；它拥有 runtime loader、`Observation` session、capture/input/trace ports 和动作时序，但不含 Win32 类型，也不复制 annotation 规则。
- `modules/controller/manifest.txt` 是唯一带 `platforms = windows` 的 reusable module；它拥有窗口发现、目标连续性、WGC、DPI 和 `PostMessageW` 输入，只依赖 core/domain，不能绕过上层自行决定点击。
- `modules/script/manifest.txt` 是独立的 Luau 0.730 基础模块；它没有依赖 engine，`uf::script::Engine::runNumber` 仍是未 sandbox、不可取消的最小执行器，不是当前产品 runtime 入口。

“除 controller 外平台无关”准确地说是 reusable 主干平台无关。`umbra-workbench` 本身是 Win32 + D3D11 GUI，`umbra-flow run` 的真实 adapter 也是 Windows-only；但它们都在 `entry/`，没有把平台类型反向泄漏到 domain、vision、image、annotation 或 engine。这个形状允许 Linux/macOS 构建纯模块，也允许 CI 用 fake port 回放帧。

系统刻意不做以下跨层捷径：

- annotation 不发现窗口、不发送输入，也不读 authoring UI state。
- engine 不选择目标、不创建 WGC、不直接写文件、不执行 Luau，也不接受未授权坐标。
- controller 不加载 manifest、不识别模板、不解释 `ResolvedPage`。
- Workbench 只 author/capture/preview/publish，不拥有 runtime input capability；runtime 只读 generated manifest 与 cropped templates，不读取完整 authoring screenshots。
- `m0-demo` 不被 engine 或 CLI 链接；它是冻结的验收参考，不是共享实现仓库。

当前有三条容易混淆的可执行路径：

- `umbra-workbench` 是 A1 authoring 路径，创建/重开项目、框选、Preview，并发布资产。
- `umbra-flow run` 是 B1 产品路径，目前执行“等待一个 page、寻找一个 action、点击一次”的 C++ smoke flow；它尚不是 Luau 驱动的完整每日任务。
- `m0-demo` 是 Rust→C++ 移植期 substrate demo，已真机验证 WGC + 后台输入，但已冻结。

## 关键类型与数据流

### Authoring 与 runtime 的双文档模型

S0 不让 runtime 直接消费 GUI 的工作文件。`modules/annotation/source/annotation/authoring-document.hpp`
中的 `AuthoringDocument` 是可完整 reopen 的 authoring source of truth；同目录下的
`RuntimeManifest` 则是最小、只读、可部署的 runtime closure。

| Artifact | 所有者与用途 | Runtime 是否需要 |
|---|---|---|
| `annotations.toml` | Workbench 拥有；保存 source、稳定 ID、geometry、page link、regression | 否 |
| `assets/sources/<hash>.png` | 完整原图，用于 reopen、重新裁剪和回归 | 否 |
| `assets/templates/<hash>.png` | 从 `template_rect` 生成的 lossless crop | 是 |
| `generated/annotations.runtime.toml` | recognizer、page、fingerprint 与 asset closure | 是 |

两个 schema 常量分别是 `g_authoringDocumentSchema` 的
`umbraflow-authoring/v1` 和 `g_runtimeManifestSchema` 的
`umbraflow-annotations/v1`。`serializeAuthoringDocument`、
`parseAuthoringDocument`、`serializeRuntimeManifest` 与
`parseRuntimeManifest` 都要求 canonical TOML；reader 会重新 serialize 并逐字比较，
因此 field order、UUID order、UTF-8、LF 和最后一个 newline 都是格式契约。

`compileAuthoringDocument` 位于
`modules/annotation/source/annotation/authoring-compiler.cpp`。它先闭合 source 与
recognizer 引用、验证 source hash/fingerprint，逐 source decode，再调用
`generateTemplateAsset` 做 BGRA crop、canonical PNG encode 和 SHA-256。结果
`CompiledAuthoringProject` 同时拥有 runtime manifest 文本与去重后的 `TemplateAsset`。

`ContentHash` 和 `sha256` 定义在
`modules/annotation/source/annotation/content-hash.hpp`。source path 与 template path
都由 bytes 的 lowercase SHA-256 决定；`RecognitionRuntime::create` 还会重新 hash
每份 encoded template，并验证 template dimensions 与 recognizer geometry。内容寻址
因此同时提供 dedup、发布历史保留和“manifest 指向的 bytes 没被替换”的闭包证明。

Workbench 的 `saveAndGenerateAuthoringProject` 位于
`entry/workbench/project-persistence.cpp`，发布顺序固定为 source assets、template
assets、`annotations.toml`、最后 runtime manifest。最后一步是 runtime commit point：
新 manifest 可见时，它引用的 immutable assets 已全部存在。当前只有每个文件的原子
replace，没有跨 artifact transaction；若最后一步失败，新 authoring document 可与
旧 runtime closure 暂时并存，代码不会假装回滚成功。

### 从 WGC 到 JSONL 的真实运行路径

真实 composition root 是 `entry/cli/run-windows.cpp`。完整数据流可以压缩为：

```text
WgcCaptureSession::capture
  -> Frame(Bgra8) + ObservationLease
  -> bgra8ToGray8
  -> bounded matchTemplateSad
  -> AnchorEvidence
  -> PageResolver::resolve
  -> ResolvedPage + ActionDetection(Detection) + ObservationLease
  -> authorizeCoordinateAction
  -> ActionSink::click
  -> controller lease fencing + PostMessageW
  -> TraceEvent -> JSONL
```

逐步跟读时，入口与关键类型如下：

1. `engine::loadRuntimeProject` 从
   `modules/engine/source/engine/runtime-loader.cpp` 读取
   `generated/annotations.runtime.toml`，按 manifest 只加载唯一、被引用的 template
   hash，再构造 `annotation::RecognitionRuntime`。额外的旧 asset 被忽略。
2. `runProduct` 先解析 page/action name，之后才声明 DPI awareness、发现目标并创建
   `WgcCaptureSession` 与 `DeliveryTarget`。坏项目在接触 desktop 前失败。
3. `EngineSession::observe` 先调用 `FrameSource::validateTargetInstance`，再 capture。
   Windows adapter `WgcFrameSource` 只是对 `WgcCaptureSession` 的薄转发。
4. WGC 的 `Frame` 携带 immutable `FrameBuffer` owner、`SessionId`、
   `TargetGeneration`、单调递增的 `FrameId`、capture instant、BGRA geometry 和
   `CoordinateTransform`。`ObservationLease::forFrame` 把相同 identity 与最多 750 ms
   的动作时效绑定起来。
5. `Observation::resolvePage` 调用 `RecognitionRuntime::evaluatePage`。它先要求 live
   `ProjectFingerprint` 与 manifest 完全相等，并要求 frame extent 等于项目
   `base_resolution`；P0 没有隐式 scaling。
6. `withGrayFrame` 对 BGRA frame 只转换一次；`bgra8ToGray8` 使用整数
   `77*R + 150*G + 29*B` 后右移 8 bit。template 在 runtime 创建时也经过同一函数，
   Preview 与 runtime 没有两套 grayscale kernel。
7. page anchors 按 catalog 的稳定 UUID order 调用 bounded `matchTemplateSad`。
   `RecognitionPolicy` 同时给出总 pixel-comparison budget、deadline 和
   `std::stop_token`；一个 page evaluation 的 budget 在 anchors 间累计。
8. `AnchorEvaluation::fromSadOutcome` 把 matcher 结果变为 `AnchorEvidence`，记录
   recognizer ID、hit、integer `sadScore`、integer `maximumSad`、matched rect 和
   display-only confidence。
9. `PageResolver::resolve` 评估全部 page：所有 required hit 且所有 forbidden miss
   才是 candidate。零个 candidate 产生 `UnknownPage`，一个产生 `ResolvedPage`，
   多个产生 `AmbiguousPages`；没有 priority 或 heuristic tie-break。
10. `Observation::findAction` 在同一 frame 上单独评估一个 `ActionTarget`。miss 是成功
    的空 `optional<ActionFound>`；hit 生成 `Detection`，再由
    `ActionDetection::create` 把它绑定到 project 与 `RecognizerId`。`ActionFound`
    还保存由 template-local offset 或 integer rectangle center 得出的 `PixelPoint`。
11. `EngineSession::act` 要求调用者交出同一 `Observation`、`ResolvedPage` 和
    `ActionFound`。授权成功后，click pixel 经 `pixelPointToFramePoint` 和 frame 自带
    transform 转为 `Point<ClientSpace>`，delivery edge 再复核目标实例。
12. `ControllerActionSink` 把 client point 和原始 lease 交给 controller 的
    `uf::click`。controller 检查 lease、client bounds、window 存活，再用
    `PostMessageW` 发送 move/down/up；失败时 adapter 尝试 `releaseHeld` 清理残留状态。
13. engine 在 observe、page outcome、action outcome、authorize、delivery、
    invalidation 和部分 failure site 同步 emit `TraceEvent`。
    `serializeTraceEvent` 固定输出 `engine-trace/v1` 与稳定 field order；
    `FileTraceSink` 每次写一行并立即 flush，形成 JSONL。

### 整数 basis-point 阈值

`SimilarityThreshold` 位于
`modules/annotation/source/annotation/catalog.hpp`，持久化范围是 `[0, 10000]`；
`9000` 表示 90.00%。决策边界完全使用 checked integer arithmetic：

```text
pixels  = templateWidth * templateHeight
maxSad  = floor((10000 - minSimilarityBp) * 255 * pixels / 10000)
hit     = sadScore <= maxSad
```

等号是命中。float confidence 只用于显示，既不决定 hit，也不参与 candidate ordering。
这样相同 pixels、geometry 与 manifest 在不同 build 中不会因 float rounding 改变动作。

## 设计不变量

### Fail-closed

系统把“不知道”与“拒绝”分开表达，但二者都不能产生输入：

- malformed/corrupt/non-canonical document、断裂引用、hash mismatch、越界 geometry、
  重复 page signature 和不兼容 fingerprint 都在 load/compile 阶段返回
  `InvalidResource` 或更具体的结构化错误。
- `Cancelled`、`TimedOut`、`ComparisonBudgetExhausted` 是 matcher stop，不会折叠成
  `hit=false`；任一 anchor stop 终止整个 page attempt。
- `UnknownPage` 与 `AmbiguousPages` 是完整评估结果，不是 exception，但只有
  `ResolvedPage` 这个 host-created type 能进入 `EngineSession::act`。
- action recognizer 必须是 active catalog 的 `ActionTarget`，其
  `allowedPageIds()` 必须包含 resolved page；名称相同不能替代 stable identity。
- recognition 前与 authorization 时都检查项目 fingerprint；delivery 前还复核绑定
  target instance。任一失败都发生在 sink call 之前。
- trace emit 不是 best effort。多数 engine evidence emit 失败会中止操作；点击已经
  落地后，observation 会先失效再 emit，防止 trace failure 引发重试双击。

### 两层动作安全

`docs/plans/2026-07-21-lua-task-model-grill-decisions.md` 的 D0/D1 要求观察身份与
投递身份不可脱钩。当前代码把它实现成两层，而不是只相信一个上层 boolean：

1. Layer 1 是 `authorizeCoordinateAction`，位于
   `modules/annotation/source/annotation/authorization.cpp`。它比较 active project、
   fingerprint、resolved page、allowed page、`ActionDetection`，并要求 page evidence、
   `Detection` 与 delivery 的 `SessionId`、`TargetGeneration`、`FrameId` 全相等；
   最后调用 `ObservationLease::validate` 检查完整 identity 与 expiry。
2. Layer 2 是 controller delivery fencing。`ActionSink` 的签名强制传递
   `ObservationLease`；`ControllerActionSink` 不能把它简化成裸坐标。
   `controller_detail::checkPointerPreconditions` 在 post 前再次检查 session、
   `TargetGeneration`、lease age 和 client bounds，随后才允许单目标
   `PostMessageW`。

这里必须保持代码级精度：lease 在第二层仍携带 `FrameId`，但当前 `DeliveryTarget`
没有“当前 frame”字段，因此 controller 不做第二次独立的 `FrameId` equality check。
完整同帧比较由 Layer 1 执行，lease 原样到达 Layer 2 由 engine test 固定。若未来让
controller 独立比较 frame freshness，需要扩充 delivery state，不能在文档中假定已经存在。

动作成功后 `Observation` 立即失效。它不可复制、move 后 source 也被标记失效；
任何 surviving alias 再调用 `resolvePage`、`findAction` 或 `act` 都返回
`StaleObservation`。这把“一次观察、同帧多查询、一次坐标动作、重新观察”从习惯变成
runtime contract。

### Determinism 与有界性

确定性不是为了离线算法竞赛，而是为了回答“无人值守时为什么点了这里”：

- grayscale 与 SAD 都是 integer；matcher 以 row-major 顺序扫描，等分只保留最早位置。
- basis-point threshold 使用 inclusive integer boundary，checked overflow 会拒绝资源。
- recognizer/page/member arrays 按 stable UUID 排序；所有 page 都评估后才判唯一性。
- authoring/runtime TOML 有唯一 canonical bytes；PNG encoder 配置和 golden bytes 被固定。
- source/template 以 encoded bytes 的 SHA-256 寻址；相同 crop 可 deduplicate。
- `RecognitionPolicy` 限制 comparison、deadline 与 cancellation；stop 保留已完成计数和原因。
- trace schema、wire names 与 field order 显式固定，不跟随 C++ enum rename 漂移。

monotonic wall clock 只参与 deadline、wait 和 lease 陈旧保险丝。它的变化只能更早停止或
拒绝动作，不参与正常 hit/miss 或 page ordering，因此时间不确定性沿安全方向收敛。

### Ownership、lifetime 与 strict-background

`Frame` 通过 `std::shared_ptr<FrameBuffer const>` 共享 immutable pixels；
`GrayImage` 只是 read-only span view，`withGrayFrame` 保证 backing gray buffer 覆盖同步
matcher call。`EngineSession` 用 `std::unique_ptr` 独占三个 port；`Observation` 内的
`EngineSession*` 是有明确“session 必须更长寿”契约的 borrow，并在跨 session action 时
被检查。`ResolvedPage`、`ObservationLease` 与 `ActionDetection` 的有效构造路径受限，
调用者不能随意拼一个看似授权的 aggregate。

严格后台由 controller 的可审计窄边界兑现：

- `modules/controller/source/controller/platform/windows-input.cpp` 只对已绑定的单一 HWND
  调用 `PostMessageW`，显式拒绝 null 与 `HWND_BROADCAST`。
- `modules/controller/source/controller/input.hpp` 列出禁止的 foreground/global input API；
  `scripts/check_safety.py` 还会扫描 `modules/`、`entry/`、`tests/`，拒绝激活窗口、
  全局注入或移动真实 cursor 的调用。
- 没有 `SendInput` fallback，没有“后台失败就激活窗口”的降级路径。

任何新平台 adapter 都必须重新证明 `ActionSink` 的 lease pass-through、单目标投递与
不抢焦点；实现了接口本身不等于自动满足 strict-background。

## 与其他部分的协作

跨层数据有意保持为窄 value 或 port：

- Workbench 入站接收 imported PNG 或 controller 提供的完整 BGRA `Frame`，转换成
  `AuthoringSource` 与 owned `AuthoringSourceAsset`；它向 annotation 交付 document +
  source bytes，取回 compiled templates、manifest 和 Preview evidence。
- annotation 向下只把 `GrayImage`、`PixelRect` 和 bounded policy 交给 vision；
  image 只在 annotation private implementation 中处理 PNG/crop，PNG codec 类型不会
  出现在 engine public API。
- CLI 向 engine 交付 `LoadedRuntime`、`EngineSessionConfig` 与三个 owned port。
  engine 不接收 HWND、DPI API handle 或 filesystem writer。
- capture port 向 engine 只跨越 `Frame`；action port 向 controller 只跨越
  `Point<ClientSpace>` + `ObservationLease`。page/recognizer evidence 不下沉到 controller。
- trace port 向 entry 只跨越 `TraceEvent`；换行、文件打开、flush 与 I/O error mapping
  留在 `entry/cli/file-trace-sink.cpp`。
- Fake `FrameSource`、`ActionSink` 和 `TraceSink` 使用完全相同的 engine 表面，因此
  offline CI 能断言“零投递”，不需要伪造 Win32。

`CoordinateTransform` 是 capture 产生的 live Client↔Frame 关系；annotation geometry
则是项目 `base_resolution` 的 integer `PixelRect`。P0 要求两者 identity-compatible，
没有把 base→live scaling 偷塞进现有 transform。这个分离让未来分辨率适配可以新增明确
阶段，而不会悄悄改变 controller 的 client coordinate 语义。

错误跨模块统一走 `Result<T>`/`Status`，并保留 `AutomationErrorKind`。controller 的
native failure、annotation 的 resource failure、vision 的 stop 和 engine 的 stale
observation 因而能到达同一个 CLI exit/trace 边界，而不是靠解析 error string 决策。

更细的模块导航可继续阅读现有知识文档：
`docs/knowledge/module-core.md`、`docs/knowledge/module-domain.md`、
`docs/knowledge/module-annotation.md`、`docs/knowledge/module-engine.md`、
`docs/knowledge/module-script.md`、`docs/knowledge/entry-cli.md` 和
`docs/knowledge/entry-workbench.md`。

## 测试策略

测试按“纯算法 → 产品语义 → port 编排 → Windows 边界”分层，避免真机不稳定性污染
deterministic contract，同时明确 synthetic test 不能替代实机 strict-background 证据。

| 测试文件 | 固定的契约 |
|---|---|
| `tests/vision/test-sad.cpp` | exact SAD、budget 边界、三种 stop、poll interval、row-major tie、BT.601 integer gray |
| `tests/image/test-pixels.cpp` | channel conversion、stride-aware crop、template 与 frame 共用 gray kernel |
| `tests/image/test-png.cpp` | PNG round trip、相同输入相同 bytes、pinned golden bytes、quota 与 malformed input |
| `tests/annotation/test-catalog.cpp` | basis-point inclusive boundary、geometry/page closure、重复 signature 拒绝 |
| `tests/annotation/test-recognition.cpp` | Resolved/Unknown/Ambiguous 无 priority、任一 stop 中止完整 resolution |
| `tests/annotation/test-recognition-runtime.cpp` | BGRA/Gray evidence 相同、global budget、fingerprint、asset closure、action hit/miss |
| `tests/annotation/test-authorization.cpp` | project/page/recognizer/identity/lease/fingerprint 全部进入 layer-1 gate |
| `tests/annotation/test-authoring-document.cpp` | authoring canonical byte round trip 与 schema drift 拒绝 |
| `tests/annotation/test-runtime-manifest.cpp` | runtime-only manifest、canonical escapes 与非规范输入拒绝 |
| `tests/annotation/test-authoring-compiler.cpp` | pure compile、source closure、dedup、geometry 与 work quota |
| `tests/annotation/test-template-asset.cpp` | crop→encode→hash→content-addressed path |
| `tests/annotation/test-regression-runner.cpp` | expected PageOutcome、suite cancellation/deadline、per-case budget |
| `tests/engine/test-runtime-loader.cpp` | published project load、missing/tampered template、manifest size cap |
| `tests/engine/test-session.cpp` | observe→resolve→find→act、零投递谱、lease pass-through、失效句柄、target-edge revalidation |
| `tests/engine/test-trace.cpp` | `engine-trace/v1` schema-first golden JSON、minimal record 与 escaping |
| `tests/cli/test-file-trace-sink.cpp` | 一次 emit 一行 JSONL、write/open failure 不静默 |
| `tests/controller/test-capture-wgc.cpp` | monotonic `FrameId`、geometry invalidation、capture option boundary |
| `tests/controller/test-capture-stall.cpp` | stale frame 与 stall timeout 的精确边界 |
| `tests/controller/test-input-revalidation.cpp` | session/generation/age/bounds delivery fence 及检查顺序 |
| `tests/controller/test-input-guard.cpp` | forbidden API vocabulary 与 per-delivery audit |
| `tests/workbench/test-project-persistence.cpp` | assets-first/manifest-last publication、immutable collision、完整 reopen |
| `tests/workbench/test-preview.cpp` | Preview 复用 runtime page/action evaluation，并保留 stop reason |
| `tests/workbench/test-source-ingestion.cpp` | imported/WGC source canonical encode、hash、provenance 与 stride |

`tests/CMakeLists.txt` 只在 controller target 存在时注册 Windows controller/workbench
suite。`tests/workbench/test-real-regression.cpp` 只有本地
`tests/assets/real-regression` 存在时才注册为 `REAL`，真实游戏截图不会进入 CI bundle。

当前合成帧测试已经覆盖 B1 fail-closed 主链，但
`docs/TODO.md` 仍把 Workbench 真机人工验证、`umbra-flow run` 真机 smoke、A1→B1
端到端、遮挡/最小化/CaptureStalled 和 10–20 分钟长程列为待办。不能用 CI 绿灯宣称
这些实机性质已经验收。

`tests/m0-demo/` 继续固定冻结 demo 的参数、guard、input-agent、pipeline、JSONL 和
shutdown 行为。它们保护验收参考不退化，但不证明 engine 路径采用了 S0 schema。

## 扩展接缝

Roadmap 使用 S0/A1/B1/A2/B2/A3/B3 薄片，而不是先做完整 GUI 再做完整 runtime。
当前落点如下：

| Slice | 当前实现 | 尚未完成 |
|---|---|---|
| S0 | domain identity/geometry、vision bounded SAD、image deterministic PNG、annotation 双 schema/page/action contract | 设计已锁定；后续 schema 变化需新权威 |
| A1 | `umbra-workbench`、source ingestion、draft/history、canvas、全部属性、Preview、save/reopen | 真机 GUI 验收；更成熟的 multi-page/sample UX |
| B1 | `modules/engine` ports/loader/session/trace、`umbra-flow run`、synthetic fail-closed CI | 真机 smoke 与 A1 资产端到端 |
| A2/A3 | annotation backend 已能表达多 page、regression 与 Unknown/Ambiguous | 多 page 编辑体验、样本管理、批量与回归 UX |
| B2/B3 | `EngineSession` 已留下 observe/find/act/wait 表面与 popup sweep hook | Luau binding、sandbox/cancel/quota、最小 popup 清扫、完整每日与长程 |

扩展时应从相应 authority 与现有 seam 进入：

- S0 schema、recognizer、page、asset 或授权变化先修订
  `docs/plans/2026-07-22-annotation-design.md` 的后继权威，再改
  `RecognitionCatalog`、compiler、runtime、Preview 与 regression；不能只在 GUI 加字段。
- A2/A3 接在 `AuthoringDocument`、Workbench draft/history、`runPreview` 和
  `runAuthoringRegressions` 上，不应建立第二份 editor-only manifest 或 matcher。
- B2 把 Luau capability 绑定到现有 `EngineSession` 表面。
  `docs/plans/2026-07-21-luau-integration-plan.md` 明确当前只完成基础集成前两步；
  sandbox、不可吞取消、allocator quota 与 host bindings 仍开放，
  `docs/plans/2026-07-21-p0b-luau-hardening-ledger.md` 是实现检查表。
- D6 的 `EngineSession::sweepKnownPopups` 当前是 no-op。P0-C 在每个 wait cycle 填最小
  known-popup sweep；P1 才按
  `docs/plans/2026-07-21-lua-task-model-grill-decisions.md` 加注册顺序、
  first-match、禁重入与 `max_hits`。
- P1 分辨率适配应在 annotation base geometry 与 live FrameSpace 之间新增独立、
  可 trace 的 Base→Live transform；不要篡改现有 `CoordinateTransform` 的
  Client↔Frame 职责，也不要把 float scale 散落进 recognizer。
- P2 tray app、任务生命周期、overlay 与 HTML trace report 建在 engine port/session/
  event seam 上。当前 `TraceEvent` 是 versioned JSONL vocabulary，不是完整 replay
  package；新增 resource snapshot 或订阅语义需要显式 schema/version 设计。
- P3 第二平台只需实现 `FrameSource` 与 `ActionSink` 的语义表面，但仍必须提供目标实例
  continuity、lease fencing 和 strict-background 的平台级证明。
- Workbench publication 的 transaction 加固入口是
  `saveAndGenerateAuthoringProject` 与 platform file-publication seam。在加 generation
  directory、journal 或等价协议前，必须维持 assets-first、runtime-manifest-last。

`m0-demo` 的冻结依据是
`docs/plans/2026-07-20-m0-demo-port-deviations.md` 与
`docs/plans/2026-07-23-engine-architecture.md`。它保留了已经真机验证的 WGC、
后台 PostMessage、guard、提权 input-agent 与 shutdown 参考，但绕过 S0
authoring/runtime schema、page/action capability 和 basis-point threshold；其
`--threshold` 语义也明确不迁移。继续向 demo 加产品功能会形成第二套授权与资产路径。

因此，新产品能力进入 `annotation -> engine -> entry` 主干；若真机结果暴露 demo 中已解决
而主干缺失的 Windows 语义，只复制经过验证的语义到 controller/adapter，并新增主干测试，
不让 engine 链接 frozen demo。等主干达到真机能力对齐后，demo 才按计划另行退役。
