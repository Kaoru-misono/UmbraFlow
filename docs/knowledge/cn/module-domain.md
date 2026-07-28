# `modules/domain` 架构知识

`domain` 是 UmbraFlow 的平台无关语义层：它把“哪一帧、哪个目标实例、哪个坐标系、这次动作还是否可信”编码成可跨模块传递的值。它不做识别、窗口管理或输入投递；它提供这些流程共同依赖且不能各自解释的词汇和拒绝规则。

## 模块范围

`modules/domain/manifest.txt` 只公开依赖 `core`。因此 `domain` 可以被 `vision`、`image`、`annotation`、`engine`、`script` 和 Windows-only `controller` 共同使用，而不会把平台 API 或更高层产品策略反向带入底层。

它拥有五组契约：

- `modules/domain/source/domain/space.hpp` 与 `space.cpp` 定义坐标空间、浮点几何、整数像素几何及二者之间可证明的桥。
- `modules/domain/source/domain/ids.hpp` 与 `ids.cpp` 定义强类型标识、`Label` 和不可回绕的 `TargetGeneration`。
- `modules/domain/source/domain/frame.hpp` 与 `frame.cpp` 定义带身份、捕获时间、像素所有权和坐标变换的 `Frame`。
- `modules/domain/source/domain/detection.hpp` 与 `detection.cpp` 定义同帧 `Detection` 以及动作时效凭证 `ObservationLease`。
- `modules/domain/source/domain/error.hpp`、`error.cpp`、`time.hpp` 和 `time.cpp` 定义自动化错误分类、恢复范围以及单调时间上的安全运算。

`domain` 不负责以下工作：

- 不产生 `CaptureSessionId` 或 `FrameId`。`CaptureSessionId` 由组合根提供，当前 CLI 在 `entry/cli/run-windows.cpp` 构造；`FrameId` 的逐捕获分配由 `modules/controller/source/controller/detail/capture-wgc.hpp` 的 `FrameIdCounter` 完成。
- 不判断何时目标窗口已换代。`modules/controller/source/controller/target.cpp` 的 `ResolvedTarget` 根据进程实例、窗口句柄、client size 和连续性推进 `TargetGeneration`。
- 不解释检测标签是否能触发动作。`Detection` 只是带同帧身份的几何证据；`modules/annotation/source/annotation/authorization.cpp` 的 `ActionDetection` 和 `authorizeCoordinateAction` 才绑定 catalog、recognizer、page 与 live fingerprint。
- 不执行模板匹配、裁图、PNG 编解码、trace 或重试。相应策略分别属于 `vision`、`image`、`engine` 或调用者。
- 不实现 strict-background。后台投递由 `modules/controller/source/controller/platform/windows-input.cpp` 的 `PostMessageW` 边界实现；禁止前台化和全局注入的 API 名单位于 `modules/controller/source/controller/input.hpp`。
- 不给任意 `Point`、`Rect` 或 `Detection` 构造器附加隐式校验。需要安全保证的边界必须显式调用 `create`、`ensure...` 或授权函数，不能把“类型存在”误当成“值已验证”。

这些约束使 `domain` 可以被离线测试和跨平台代码复用。平台事实由 controller
判断，产品授权由 annotation 和 engine 完成。

## 共享数据模型

### 坐标空间与几何值

`modules/domain/source/domain/space.hpp` 声明四个空 tag：

- `DesktopSpace`：桌面坐标，client origin 也以此表示。
- `ClientSpace`：目标窗口 client area 坐标，是 Controller 接受指针动作的坐标。
- `FrameSpace`：捕获帧上的浮点坐标，是检测矩形与识别结果进入动作链时的公共空间。
- `NormalizedSpace`：以 frame width/height 归一化的浮点坐标。

`CoordinateSpace` concept 限定 `Point<Space>` 与 `Rect<Space>` 只能实例化在这四种空间。模板参数使 `Point<DesktopSpace>` 不能误传给要求 `Point<ClientSpace>` 的 API；这是编译期的量纲隔离，不是运行期范围检查。

`Point<Space>` 只保存两个 `float`。`Rect<Space>` 保存 `x`、`y`、`width`、`height`；`contains` 使用半开区间 `[x, x + width) × [y, y + height)`，`center` 做浮点中心计算，`isEmpty` 将任一非正 extent 视为空。构造器允许负数、无穷和 NaN，安全边界必须另行验证。

`PixelPoint` 与 `PixelRect` 使用 `uint32`，表达离散像素而不是某个桌面空间。`PixelRect::create`：

1. 拒绝零宽或零高；
2. 用 `checkedAdd` 证明 `x + width` 与 `y + height` 不溢出；
3. 缓存 `right` 和 `bottom`，使后续 extent 检查不必重新进行不安全加法。

`PixelRect::ensureWithinExtent` 在越界时返回 `ActionRejected`，不会裁剪。`PixelRectHash` 对四个值字段做稳定的值哈希，供 unordered 容器使用；它不是持久化 hash 或内容标识。

### 精确的整数—浮点桥

`k_maxExactFrameDimension` 等于 `1 << 24`，即 `16,777,216`。IEEE-754 binary32 能精确表示闭区间 `[0, 2^24]` 中的每个整数，而 `2^24 + 1` 是第一个不能精确表示的整数。这里的 inclusive 边界是接口契约，不应改写成 `< 2^24`。

`pixelPointToFramePoint` 逐坐标检查 `x`、`y` 不大于该上界，随后才 `static_cast<float>`。越界返回 `InvalidResource`，因为输入像素资源已经无法无损进入 `FrameSpace`。

`pixelRectToFrameRect` 检查的是远端边 `x + width` 和 `y + height`，并先提升到 `uint64` 计算，避免近 `uint32` 上界时回绕。远端边等于 `2^24` 合法，超过才返回 `InvalidResource`。通过检查后，origin、extent 和远端边都处在可精确表示区间，四次整数到 `float` 的转换不丢信息。

反方向 `CoordinateTransform::frameRectToPixelRect` 有意是覆盖型量化，不是逐值逆变换：

- 先用 `ensureFrameRectInBounds` 证明矩形有限、非空且在 frame 内；
- 起点取 `floor`，远端边取 `ceil`，因此任何有面积的 subpixel rect 至少覆盖一个像素；
- 对最多 `1e-3F` 的边界浮点误差，先 clamp 到真实 frame extent；
- 用 `checkedIntegralCast<uint32>` 和 `checkedSubtract` 构造最终 `PixelRect`；
- 若量化后没有像素覆盖，返回 `ActionRejected`，不猜测结果。

所以精确承诺是：合法整数 `PixelRect` 经 `pixelRectToFrameRect`，再在同尺寸 identity transform 上经 `frameRectToPixelRect`，可以 round-trip；任意浮点矩形转像素本来就会按 coverage 规则量化。

### `CoordinateTransform`

`CoordinateTransform::create` 是唯一公开构造入口。它要求 desktop client origin、client width/height 都有限，client extent 为正，frame width/height 非零且各自不超过 `2^24`。失败归类为 `InternalInvariant`，因为调用者正试图建立一个系统内部无法安全使用的变换。

对象保存：

- `m_clientOriginX/Y`：client 左上角在 `DesktopSpace` 中的位置；
- `m_clientWidth/Height`：client 的浮点 extent；
- `m_frameWidth/Height`：捕获帧的整数 extent。

点变换由平移和按轴缩放组成：`desktopToClient`/`clientToDesktop` 只处理 origin；`clientToFrame`/`frameToClient` 按 client 与 frame 的尺寸比缩放；`frameToNormalized`/`normalizedToFrame` 再除以或乘以 frame extent。`desktopToFrame` 和 `frameToDesktop` 只是上述步骤的组合。

矩形变换 `clientRectToFrame` 与 `frameRectToClient` 对 origin 和 extent 使用相同比例，不做 clamp。所有这些纯变换都是 `noexcept`，并假设 transform 本身已由 `create` 验证；它们不会验证传入点是否有限或处于可见区域，也不会强制 `NormalizedSpace` 落在 `[0, 1]`。

动作边界使用两个不同规则：

- `ensureFrameRectInBounds` 允许 `1e-3F` 的浮点边界误差，适合矩形缩放后再量化。
- `ensureFramePointInBounds` 要求严格的半开范围 `0 <= x < width`、`0 <= y < height`，没有 epsilon，适合最终点击点。

### Frame 身份与像素所有权

`modules/domain/source/domain/ids.hpp` 用 `StrongId<Tag>` 定义 `EngineRunId`、`TaskRunId`、`CaptureSessionId`、`FrameId`、`StateId`、`RecognitionId` 和 `ActionId`。相同的 `uint64` 数值不能在这些类型之间隐式转换，因此日志里“17”相同不等于语义身份相同。

`TargetGeneration` 包装 `core::Generation`。默认值与 `initial()` 相同；`fromValue` 主要用于恢复或测试；`next()` 在 `uint64` 顶点返回 `InternalInvariant`，绝不回绕后重新接受陈旧证据。

D0 的双计数器含义由 `docs/plans/2026-07-21-lua-task-model-grill-decisions.md` 锁定：

- `FrameId` 是高频 liveness 维度。当前 WGC `FrameIdCounter::nextId` 在每次成功组装 frame 前分配递增 ID，计数器溢出即失败。
- `TargetGeneration` 是低频 safety 维度。`ResolvedTarget` 在进程实例变化、handle/client size 变化、丢失连续性或显式 re-resolve 时推进它；同一目标的无变化 revalidation 不推进。
- `CaptureSessionId` 再隔离捕获会话，避免新 session 从低 `FrameId` 重新计数时与旧证据碰撞。

frame identity 由 `(CaptureSessionId, TargetGeneration, FrameId)` 三元组组成。`Frame`、
`Detection`、`ObservationLease`，以及 annotation 的 `FrameIdentity` 都携带或导出
这一组值。

`FrameBuffer` 独占一个 `std::vector<std::byte>`，只暴露 `span<const byte>`。它不能 copy/move assign，外部也没有可变 byte API。`Frame` 持有 `shared_ptr<FrameBuffer const>`，所以复制 `Frame` 或把像素交给异步识别不会复制整帧，也不会产生悬空 view。

`Frame::create` 同时证明：

- pixel owner 非空；
- width/height 非零；
- `stride >= width * bytesPerPixel(pixelFormat)`；
- `bufferLength >= stride * height`；
- 上述乘法与整数转换不溢出；
- `CoordinateTransform::frameSize()` 与 frame width/height 完全一致。

`PixelFormat` 当前只有 `Bgra8` 和 `Gray8`，`bytesPerPixel` 分别为 4 和 1。新增格式时必须同步扩展无 default 的 switch，否则不应让未知格式静默通过 geometry 检查。

Windows capture 在 `modules/controller/source/controller/platform/windows-capture.cpp` 的 FrameArrived callback 用 host `MonotonicInstant::now()` 记录 `capturedAt`，随后把 arrival time、身份、像素和当前 transform 一起交给 `Frame::create`。它不是 GPU produce time，也不是可序列化墙钟。

### `Detection` 与 `ObservationLease`

`Detection` 是不可变值载体：保存 `CaptureSessionId`、`TargetGeneration`、`FrameId`、`Label`、`Rect<FrameSpace>` 和 `confidence`。构造器不校验 rect 或 confidence；可信动作还必须经过 annotation 的 recognizer/page 授权，不能只凭 label。

`Label::create` 保证字符串是合法 UTF-8，但允许空字符串；`value()` 返回受 owner
生命周期约束的 const reference。根据
[2026-07-28 review follow-up](../../plans/2026-07-28-full-project-review-fixes.md)，
`Detection::label()` 同样返回 lifetime-bound `Label const&`，不会按值复制 label。

`ObservationLease` 的构造器私有，只能用 `forFrame` 从真实 `Frame` 派生。它复制 frame 三元身份，并计算 `expiresAt = capturedAt + effectiveAge`。

`k_defaultMaxActionFrameAge` 是 750 ms。`clampMaxActionFrameAge` 只允许调用者缩短该 fuse，不能通过配置放宽：`effectiveAge = min(requested, 750ms)`。负 duration 在 clamp 前被拒绝；`checkedAddMonotonic` 还拒绝 deadline 溢出。

到期判断是严格的 `now > expiresAt`。恰好在 deadline 仍有效；零时长 lease 在 `capturedAt` 当刻有效，之后立即失效。

`ObservationLease::validate` 按顺序比较 session、target generation、observed frame，最后检查年龄；任一失败都是 `StaleObservation`。它不重试，因为“重新观察”是上层控制流选择，不应隐藏在安全凭证里。

### 错误公共面

`AutomationErrorKind` 枚举记录原因；`FailureResponse` 记录失败应向外展开多远。`failureResponse(AutomationErrorKind)` 的穷尽 switch 当前映射为：

- `Cancelled` → `Cancelled`；
- `CaptureStalled`、`StaleObservation` → `Retry`；
- `RecognitionFailed`、`ActionRejected` → `StepFailed`；
- 其余 kind → `Abort`。

新增 `AutomationErrorKind` 时，没有 default 的 switch 会迫使开发者选择恢复范围。`fail(kind, message, nativeCode)` 把 kind 编码进私有的 `uf.automation` error category；编码使用 underlying value 加一，避免合法 kind 变成值为零的“无错误”code。`automationErrorKind` 只接受该 category，普通 `std::error_code` 不会冒充自动化错误；无法分类的 `Error` 保守地映射为 `Abort`。

`systemErrorCode(uint32)` 专为 OS 原生状态码保真。它有意把 unsigned bit pattern 转为 `int` 后放入 `std::system_category()`；高位为 1 的 HRESULT-shaped/GetLastError 值不能用 checked narrowing 丢弃。该函数只包装原生原因，业务 kind 仍由外层 `fail(AutomationErrorKind, ..., nativeCode)` 决定。

`checkedAddMonotonic` 拒绝负 duration 和时间点溢出。`elapsedNanosecondsSince` 对反向时间饱和为零，对不能装入 `uint64` 的结果饱和为最大值；它适合 trace duration，不承担 lease 判定。

## 必须保持的约束

**Fail-closed。** 非有限几何、空/越界区域、整数溢出、transform/frame 尺寸不一致、陈旧身份、过期 lease 和未知错误分类都返回结构化失败；转换函数不会自动裁出一个“看起来能用”的动作目标。矩形边界的 epsilon 只吸收浮点噪声，最终仍 clamp 到真实 extent。

**身份一致性。** 同一次识别到动作授权必须维持 `(CaptureSessionId, TargetGeneration, FrameId)`。`modules/annotation/source/annotation/authorization.cpp` 比较 resolved page、action detection 和 delivery state 的 `FrameIdentity`，然后调用 `ObservationLease::validate`。这让“来自同标签但不同帧”的 detection 无法授权。

**D0/D1 的当前实现层次。** `modules/engine/source/engine/session.cpp` 的 `Observation` 是 move-only；move source 与成功 click 后的 handle 都被标记 invalidated，再使用返回 `StaleObservation`。成功投递后先置 invalidated，再发送可能失败的 trace，避免 trace 失败诱发重复 click。

**D0 权威与现状必须区分。** `docs/plans/2026-07-21-lua-task-model-grill-decisions.md` 要求注入层同时重检 `FrameId` 与 `TargetGeneration`。当前 Controller 的 `checkPointerPreconditions` 实际重检 `CaptureSessionId`、lease age、`TargetGeneration` 和 client point，却没有“当前 `FrameId`”参数；`FrameId` 现由 annotation/engine 的同帧比较与 `Observation` 单次消费约束。维护者不应把注释中的“delivery layer re-runs frameId fence”当成已经由 Controller 独立实现。

**确定性优先，时间只作保险丝。** 身份相等是主要判据，纯坐标变换、整数边界和恢复映射对相同输入给出相同输出。单调时间只用于 `max_action_frame_age`，处理游戏自身改变界面但 generation 未变化的漏网情况；超时只向拒绝方向影响结果。

**所有权与生命周期可见。** `Frame` 用 `shared_ptr<const FrameBuffer>` 共享大像素数据，`bytes()` 的 span 明确依赖 `FrameBuffer` owner。`Detection`、lease、transform 和 ID 都按值携带。`domain` 不保存 raw pointer、callback 或异步 borrow。

**Strict-background 是跨模块不变量。** `domain` 只提供 `Point<ClientSpace>`、lease 和错误语义；`engine::IActionSink` 要求适配器把 lease 传到 delivery layer，Controller 最终使用 `PostMessageW`。`SetForegroundWindow`、`SetFocus`、`SendInput`、`mouse_event`、`keybd_event`、`SetCursorPos` 被列为 forbidden。任何 domain 扩展都不能以“坐标不够表达”为理由降级到前台或全局输入。

## 被哪些模块使用

典型数据流如下：

1. `controller` 解析窗口与 client geometry，维护 `CaptureSessionId`/`TargetGeneration`，捕获像素并为每帧分配 `FrameId`。
2. `CoordinateTransform::create` 把 desktop client origin、client extent 与 frame extent 固化在同一个 `Frame` 中。
3. `Frame::create` 验证 buffer geometry、transform 尺寸和 immutable pixel owner。
4. `engine::EngineSession::observe` 先 revalidate target，再 capture，并从 frame 创建 `ObservationLease`。
5. `annotation::RecognitionRuntime`/`vision` 在 frame 像素上产生整数 `PixelRect` 证据；engine 通过 `pixelRectToFrameRect` 创建同帧 `Detection`。
6. annotation 把 detection 绑定到 `action_target` recognizer，并证明 page、project、fingerprint、三元身份和 lease 一致。
7. engine 把整数 click pixel 经 `pixelPointToFramePoint` 和 frame 的 `frameToClient` 转成 `Point<ClientSpace>`。
8. `engine::IActionSink::click` 把 client point 与原 lease 一并交给 Controller；Controller 重检目标 generation、session、年龄、窗口存活与 client bounds，再由 `PostMessageW` 后台投递。
9. 成功后 engine 使 `Observation` 失效，下一动作必须重新 observe。

入站边主要是 `core`：`Result`/`Status`、checked arithmetic/cast、`StrongId`/`Generation`、enum reflection、`MonotonicInstant` 和 release-safe contracts。`domain` 把这些通用机制组合成自动化语义，但不把 UmbraFlow 策略下沉到 `core`。

出站消费者各取一部分而不是依赖 aggregate header：

- `vision` 消费 `Frame`、`PixelRect` 和错误类型，输出确定性匹配证据。
- `image` 消费 pixel geometry 与 frame layout，执行裁图和格式转换。
- `annotation` 消费 frame identity、detection、lease 和 geometry，建立 page/action capability。
- `engine` 组织 observe/resolve/find/act 生命周期，并让 trace 携带 domain identity 和 error kind。
- `controller` 生产 frame/transform，维护目标 generation，并消费 client point 与 lease。
- `script` 最终通过宿主绑定看到这些语义，而不直接获得像素 owner 或平台 handle。

## 测试

`tests/domain/test-space.cpp` 是坐标契约主测试：已知映射、round-trip tolerance、半开 containment、finite/bounds 检查、`floor`/`ceil` coverage、subpixel 至少一像素、`PixelRect` overflow、hash，以及 `[0, 2^24]` inclusive 边界和整数 round-trip 都在这里固定。

`tests/domain/test-frame.cpp` 固定 immutable shared pixels、`PixelFormat` 字节数、stride/length 的最小合法值、padding、零尺寸、乘法溢出、空 owner 和 transform/frame 尺寸一致性。

`tests/domain/test-ids.cpp` 固定各 ID 类型不可混用、`TargetGeneration` 单调且不回绕，以及 `Label` 的 UTF-8 接受/拒绝行为。

`tests/domain/test-detection.cpp` 固定 750 ms 只能缩短、deadline 的严格边界、三元身份逐项拒绝、负年龄和 deadline overflow。修改 lease 比较顺序或到期运算时必须先理解这些精确断言。

`tests/domain/test-error.cpp` 枚举全部 `AutomationErrorKind`，固定 detail name 与 `FailureResponse`；还固定 generic error fail-closed 以及 `0x8007'0005` 这类高位 OS code 的 bit-pattern 保真。

`tests/domain/test-time.cpp` 固定 monotonic add、反向 elapsed 饱和为零、负 duration 与 overflow 拒绝。

跨模块测试验证 domain 值在实际调用链中的用法：

- `tests/controller/test-capture-wgc.cpp` 固定 session 内 `FrameId` 单调和 overflow 拒绝，并验证 capture geometry 创建 transform。
- `tests/controller/test-target.cpp` 固定何种窗口身份变化恰好推进一次 `TargetGeneration`。
- `tests/controller/test-input-revalidation.cpp` 固定 session/generation/age fencing、client bounds 和 signed-16-bit message 坐标限制；它也反映当前没有 current `FrameId` 入参。
- `tests/controller/test-audit-log.cpp` 固定 strict-background forbidden API 集合与投递 audit。
- `tests/annotation/test-authorization.cpp` 固定 same-frame page/detection/lease/fingerprint、recognizer identity 和 allowed-page 授权。
- `tests/engine/test-session.cpp` 固定 observe-to-click 数据流、lease 原样传递、动作后失效、moved-from/foreign observation 拒绝、delivery-edge target revalidation，以及 trace 失败不能使动作可重放。

这些测试大多使用合成 frame 和显式 `MonotonicInstant`，避免真实窗口、墙钟抖动或 GPU 时序进入 domain 的确定性回归面。

## 后续扩展

**P1 分辨率自适应。** `docs/plans/2026-07-21-product-form-and-roadmap.md` 指定 P0 使用 project `base_resolution`/DPI fingerprint 与 identity gate，P1 才增加显式 Base→Live viewport transform。它应作为新的、命名清楚的空间或变换层接到 `CoordinateTransform` 前后，不能偷偷改变现有 `FrameSpace`、`ClientSpace` 或 `[0, 2^24]` 桥的含义。

**Luau 表面。** `docs/plans/2026-07-23-engine-architecture.md` 要求未来 Luau 1:1 镜像 `observe`/`resolvePage`/`findAction`/`act`。domain 值应继续作为只读 host capability 的载荷；不要为脚本便利暴露可伪造 lease、可变 frame identity 或裸像素生命周期。

**常驻 Engine 与第二平台。** 同一计划把 engine ports 留给 P2 常驻生命周期和未来非 Windows adapter。新 capture adapter 必须产生相同的 `Frame`、host monotonic arrival time、递增 `FrameId`、目标 generation 和 transform 证明；新 action adapter 必须保留 lease forwarding 与 strict-background，不得把平台差异泄漏进 domain。

**D0 投递层补强。** 根据
`docs/plans/2026-07-21-lua-task-model-grill-decisions.md`，Controller delivery
还需要获得可比较的 current `FrameId`，才能在最终投递点独立验证 lease
的双计数器，而不是只依赖 engine 的单次消费。实现时需要同时调整
`IActionSink`、Controller delivery contract 和相应测试，不能只改注释。

**新错误 kind。** 在 `AutomationErrorKind` 增项时，应同步 enum reflection、`failureResponse` 穷尽映射、trace/脚本边界和 `tests/domain/test-error.cpp` 的完整 case 表。是否 retry 是控制流政策，不能从错误名字临时猜测。

**新像素格式或坐标空间。** 新 `PixelFormat` 必须定义 bytes-per-pixel、frame geometry、vision/image 支持与测试；新 coordinate space 必须加入 `CoordinateSpace` 并提供显式转换。不要使用同为 `float` 或同为 `uint32` 作为跳过空间类型的理由。

**动作种类扩展。** D1 权威把任一坐标动作视为“观察后单次动作”。未来 swipe、long press 或其他 pointer action 应复用同一三元身份、lease fuse、client conversion 和动作后失效语义；键盘动作当前只携带 `TargetGeneration`，若被纳入同一 observe/act 模型，需要先在权威契约与 delivery API 中明确其 `FrameId` 关系。
