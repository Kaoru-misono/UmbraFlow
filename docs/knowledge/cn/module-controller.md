# `modules/controller` 架构知识

本文描述 `modules/controller` 的当前 Windows 实现。上位方向来自
`docs/plans/2026-07-21-product-form-and-roadmap.md`，未完成的 Win32 加固以
`docs/plans/2026-07-20-post-port-win32-robustness.md` 为准；计划与代码不一致时，
以下以代码为准，尚未实现的要求放在“后续扩展”。

## 模块职责

`modules/controller` 是 Windows 桌面能力适配层。它把窗口、进程、WGC、D3D11
和窗口消息转换成 `core`/`domain` 可以表达的值、`Result<T>` 与 `Status`。
`modules/controller/manifest.txt` 将整个模块限制为 Windows，公开依赖只有
`core`、`domain`，Win32、D3D11、DXGI、DWM、NTDLL 与 Windows Runtime 库均为
Windows private dependency。

模块拥有五组职责：

- `modules/controller/source/controller/discovery.hpp`：枚举顶层窗口，把易失的
  Win32 查询结果收敛为 `TargetCandidate`。
- `modules/controller/source/controller/target.hpp`：按 `TargetSelector` 唯一解析
  候选，并用 `ResolvedTarget` 管理目标连续性与 `TargetGeneration`。
- `modules/controller/source/controller/capture.hpp`：把一个已绑定窗口包装成
  `WgcCaptureSession`，返回裁到 client area 的 BGRA8 `Frame`。
- `modules/controller/source/controller/input.hpp`：只通过目标窗口消息投递鼠标、
  键盘和文本，维护按下状态与逐消息审计。
- `modules/controller/source/controller/dpi.hpp`：在使用物理窗口几何前建立并验证
  `PER_MONITOR_AWARE_V2` 进程前提。

公开类型位于 `uf`，纯逻辑实现细节位于 `uf::controller_detail`，Win32 桥位于
`uf::controller_platform`。仓库中没有 `uf::controller` 命名空间；新增代码若按
目录名猜命名空间，会绕开现有组织方式。

controller 不负责以下策略：

- 不识别页面、不解释 `Detection`，也不授权动作；这些属于 `annotation` 和
  `engine`。
- 不决定产品如何按标题模糊匹配、是否忽略隐藏窗口，或如何向用户展示歧义。
  controller 自身只提供精确 selector 和“必须唯一”的解析；CLI、workbench 可以在
  候选值之上实现各自策略。
- 不分配产品级 `CaptureSessionId`，不管理任务取消、重试、trace 文件或 Luau 生命周期。
- 不验证项目的 `ProjectFingerprint`。`TargetIdentity` 不含 DPI，
  `DeliveryTarget` 也不携带 compatibility proof。
- 不提供前台激活、全局输入或移动真实光标的降级路径。目标不接受
  `PostMessageW` 时，调用失败而不是切换后端。
- 不抽象第二个平台。跨平台调用面是 `engine` 的端口；本模块本身有意保持
  Windows-only。

这种边界把可离线复现的授权语义留在平台无关模块，同时把所有 HWND、COM、
回调和驱动生命周期集中到一个可审计区域。

## 目标、捕获与输入

### 发现、解析与目标代际

发现入口是 `enumerateCandidates() -> Result<std::vector<TargetCandidate>>`。
`modules/controller/source/controller/platform/windows-discovery.cpp` 的真实路径为：

1. `EnumWindows` 同步收集 `WindowHandle`；回调只保存不拥有的 opaque token。
2. 每个 handle 依次查询 PID、可选进程启动时间、可选 executable path、window
   class、title、client size、DPI、visible 与 iconic 状态。
3. `OpenProcess` 遇到 access denied 或进程在查询中退出时，只把启动时间和路径
   降级为空；PID、class、title、client size、DPI 等必需事实查询失败则返回
   `TargetUnavailable`。
4. `GetWindowThreadProcessId` 的失败和 `GetDpiForWindow == 0` 都被显式拒绝；
   每次中间查询失败后再次检查窗口是否已经消失，以区分正常竞态与真实 API
   失败。

`WindowHandle`、`ProcessId`、`ProcessStartTime` 与 `Dpi` 是强类型值，
`ClientSize` 保存无符号宽高。`TargetCandidate` 是一次枚举快照，不是持续有效的
窗口借用；其 optional metadata 不能被误读成 identity 已确认。

`TargetSelector` 的 `withProcess`、`withWindowHandle`、`withWindowClass` 和
`withTitle` 返回新值，字符串匹配精确且区分大小写。`matchingCandidates` 只做
筛选；`resolveTarget` 还要求恰好一个匹配。零匹配和多匹配都返回
`TargetUnavailable`，同时指定 PID 与 HWND 而二者冲突时会单独报错，因此解析
从不“挑第一个看起来合适的窗口”。

`ResolvedTarget` 保存 `TargetIdentity { WindowHandle, ProcessId,
ProcessStartTime?, ClientSize }`、当前 `TargetGeneration` 和内部 continuity。
初始 generation 为零。`revalidate()` 重新读取 live identity，再交给
`applyRevalidation`：

- PID 与启动时间均相同，且 HWND、client size 也相同：返回
  `RevalidateOutcome::Unchanged`。
- 进程实例不同，或同一进程实例的 HWND/client size 改变：generation 前进一次，
  保存新 identity，返回 `GenerationBumped`。
- 窗口消失：generation 前进一次并锁存 `Lost`。
- 任一侧缺少进程启动时间：无法排除 PID 复用，generation 前进一次并锁存
  `InstanceUnconfirmed`。

`Lost` 和 `InstanceUnconfirmed` 后续不会反复推进 generation；必须由
`reResolve` 用新候选恢复。对仍为 confirmed 的目标显式 `reResolve` 会先推进
generation；对已经锁存失效的目标不会重复推进。这使每次连续性断裂只产生一个
代际边界。`TargetGeneration::next()` 与 `FrameIdCounter::nextId()` 都拒绝回绕，
旧凭证不会因整数溢出重新变得有效。

`errorOnLost` 只把 `Lost` 转为 `ControllerDisconnected`；其他 outcome 仍由调用者
按上下文处理。`ResolvedTarget` 的多次 Win32 读取不是原子快照，等值 HWND
回收竞态仍是已登记的开放项。

### WGC session 与帧

调用者先用 `ClientGeometry::create` 提供物理 desktop-space client origin 和正的
client extent，再调用 `WgcCaptureSession::create(WindowHandle, CaptureSessionId,
TargetGeneration, ClientGeometry, WgcCaptureOptions)`。session 固定保存创建时的
session/generation；目标变化后应销毁并以新 generation 重建，而不是原地改写。

创建过程位于
`modules/controller/source/controller/platform/windows-capture.cpp`：

1. `WindowInstanceMarker` 在目标 HWND 上设置进程内唯一名称的 window property，
   property 值是 session 独占的 event-handle token。相同数值的 HWND 若被系统
   回收，新窗口不会继承该 property，`matches()` 因而能拒绝错误实例。

   **这一步 `SetPropW` 上的 Win32 错误 5 意味着提权，2026-07-30（`2429578`）起消息直接这么说**，
   而不是只报一个数字。目标的完整性级别高于调用方时，UIPI 会拒绝在它上面盖属性。而最顺手的
   探针指向的方向恰恰是反的——不提权地对同一个窗口 `PostMessage` 是成功的——所以「输入投递能用」
   **推不出**「capture 能绑定」，这一点真的绕过一次冤枉路。这里的错误 5 只有一个原因，所以消息
   把它点名；其他 Win32 错误保持数字形式。见
   `docs/pitfalls/capture-and-target-selection.md`。
2. 检查 WGC 可用性和 OS build。build 低于 19041 时无法关闭 cursor capture，
   session 直接失败。`WgcCaptureOptions::requireBorderless()` 当前总是失败，因为
   还没有调用者拥有的 borderless access grant 路径。
3. 创建硬件 D3D11 device；失败后尝试 WARP。immediate context 启用
   `ID3D11Multithread` 保护，因为 free-threaded WGC callback 与消费端共享 device。
4. `CreateForWindow` 建 `GraphicsCaptureItem`，读取初始 item size，按
   `ClientToScreen(0, 0)` 和 `DWMWA_EXTENDED_FRAME_BOUNDS` 算出
   `ClientCropRect`。这里不用 `GetWindowRect`，避免把 WGC 不包含的 invisible
   resize border 算进 crop。
5. 创建两个 buffer 的 free-threaded frame pool，关闭 cursor capture，注册
   item-closed 与 frame-arrived callback，然后 `StartCapture`。

frame-arrived callback 不捕获 `Impl` 或裸 `this`，只按值持有
`std::shared_ptr<FrameSlot>`。`FrameSlot` 用 mutex 保护一个 `m_latest`：
生产者覆盖为最新 `CapturedArrival`，消费者取走后清空，因此慢消费者不会累积
无界帧队列。callback 以 `MonotonicInstant::now()` 记录 host arrival time，
不是 WGC 的 GPU produce time。item closed 和 callback HRESULT failure 也发布到
同一 wait predicate。

`WgcCaptureSession::capture()` 在 operation mutex 下串行执行：

1. `waitForFrame` 等到 latest frame、item closed、callback failure 或 stall
   timeout。`StallTracker` 以 arrival time 而不是像素变化或消费时间判鲜度；一帧
   即使已在 slot 中，消费时超过 timeout 仍返回 `CaptureStalled`。
   `StallTracker::check` 除时刻外还必须收一个 `TargetWindowState`：窗口被最小化
   或已销毁时根本不合成任何帧，那就是 stall 的成因而不是无关事实。该状态由
   `windows-capture.cpp` 的 `observeTargetWindow`（先 `IsWindow` 再 `IsIconic`）
   给出，`stalledFrameFailure` 把它写成同时点名窗口状态与解决动作的消息。遮挡与
   移出屏幕外故意不探测：DWM 对这两种情况仍在合成，因此都不可能是成因。
2. `CaptureGeometryState::observeContentSize` 要求每帧 `ContentSize` 与创建时
   完全相同；D3D surface size 还要与已确认 size 相同。任何无效值或不匹配都会
   永久锁存 invalidated，此后即使尺寸恢复也必须重建 session。
3. `readbackSurface` 只复制 `ClientCropRect` 到可复用 staging texture，
   `readbackBgra8` 去掉 D3D row-pitch padding，得到紧密 BGRA8 client pixels。
4. 再次验证 `WindowInstanceMarker`，重新读取当前 client origin。窗口只移动而
   extent 不变时，像素尺寸保持稳定，但新 `CoordinateTransform` 会反映新的
   desktop origin。
5. 分配单调 `FrameId`，以 callback arrival time 作为 `capturedAt`，把 immutable
   `std::shared_ptr<FrameBuffer const>`、session/generation 和 transform 一起放入
   `Frame`。

`CaptureHygiene` 暴露创建时 OS build、cursor 是否已禁用、borderless 支持与
border-required 状态，供上层记录能力事实。`validateTargetInstance()` 单独检查
item-closed 与 marker，供 engine 在 observe 和 delivery 边缘调用。

`close()` 与 `capture()` 共用 operation mutex。teardown 先把
`FrameSlot::m_acceptingFrames` 置为 false，再关闭 session、撤销 callback、关闭
frame pool、清理 latest frame 和 window property。已经在途的 callback 看到
accepting=false 后会关闭其 frame，不会重新填充 slot。

### 严格后台输入

`DeliveryTarget::create` 把 `WindowHandle`、`CaptureSessionId`、
`TargetGeneration` 和非空 `ClientSize` 固定为一个投递 capability。它不拥有
窗口，也不自动跟随 `ResolvedTarget`；generation 或 session 改变时应创建新值。

坐标动作的公开入口是 `movePointer`、`click`、`pointerDown`、`pointerUp`、
`longPress` 和 `scroll`。它们接受 `Point<ClientSpace>` 与 `ObservationLease`。
`checkPointerPreconditions` 的实际拒绝顺序为：

1. lease session 必须等于 `DeliveryTarget::sessionId()`；
2. 当前 monotonic time 不得晚于 lease expiry；
3. lease generation 必须等于 `DeliveryTarget::generation()`；
4. 坐标必须 finite、位于半开 client bounds 内，并能编码为非负 signed-16-bit
   mouse coordinate；
5. 浮点坐标用 `floor` 变成 `ClientPixel`。

随后 `HeldInputs` 必须未绑定到其他 delivery identity，`IsWindow` 必须仍为真，
才会进入消息边界。`click` 依次投递 move、left-down、left-up；若 down 成功而
up 失败，held 状态有意保留给调用者补偿。`longPress` 在 down 后等待，再通过
调用者提供的 refresh callback 要求 HWND、session、generation 全部未变，才投递
up；client geometry 变化本身不影响该 identity 比较。

`scroll` 在该点投递一条 `WM_MOUSEWHEEL`，也是这里唯一 `lParam` 用**屏幕**坐标的入口：
Win32 对滚轮消息的位置就是这么规定的，而 `WM_MOUSEMOVE` 和按键消息用的是 client 坐标。
所以它先用 `ClientToScreen(hwnd, {0, 0})` 把 `ClientPixel` 平移成
`controller_detail::ScreenPixel` 再构造消息。分成两个类型是刻意的——窗口落在主显示器
左侧或上方时屏幕坐标会是负数，而 `ClientPixel` 在构造时就拒绝负值，两个空间因此不可能
被混着传进消息构造函数。它同样跑完整的 `checkPointerPreconditions` 围栏，因为滚轮的位置
正是目标用来判定「滚哪个控件」的依据。`WheelDelta` 以格（`WHEEL_DELTA` = 120）计数，
正值表示远离操作者，拒绝 0，并且做了上界约束，保证原始 delta 仍放得进 `wParam` 的
有符号 16 位高字。只做垂直方向：`WM_MOUSEHWHEEL` 是有意不做的，它会在从这里一直到
线上协议的每一层都多出一个轴，而本项目没有需要横向滚动的地方。

键盘入口 `keyPress`、`keyDown`、`keyUp`、`inputText` 与 `inputUnichar` 不接受
`ObservationLease`，只比较 action generation。`KeyInput` 记录 virtual key 与
extended-key 位；`inputText` 先严格解码 UTF-8，再按 UTF-16 code unit 发送
`WM_CHAR`，`inputUnichar` 只接受 Unicode scalar 并发送 `WM_UNICHAR`。

**`keyPress` 在 2026-07-30（`ed38124`）才有了第一个生产调用方。** 在那之前整条键盘路径只被
测试跑过，上面那句「收 generation 而不是 lease」是一项没人用的能力；现在它是
`engine::IActionSink::pressKey` 与 CLI 的 `ControllerActionSink` 所依据的契约，而且事后看
恰好是对的——按键不指名坐标，没有任何矩形可供 lease 围栏。当时缺的只是「名字到虚拟键」的
映射：`KeyInput::fromName` 与 `KeyInput::fromKeyName` 负责解析，而**有哪些名字存在是
`modules/domain` 里 `KeyName` 的唯一定义**，`fromName` 走的是它而不是重写一遍判断，两边因此
不会产生分歧。另外四个入口至今没有生产调用方。

`fromKeyName` 解析三个族，每一族都是一条规则而不是一串比较：具名键走 `NamedKeyCode` 表，
`"F1".."F12"` 走 `VK_F1 + n - 1`（这些码是连续的），单个字母或数字走它自己的 ASCII 码
（`VK_A..VK_Z` 与 `VK_0..VK_9` 的定义正是如此）。具名表由一条 `static_assert` 与
`domain::k_namedKeys` 配对：`domain` 收了却在这里没有虚拟键的名字会编译失败，而不是掉进
单字符分支、在一个作者本来有权写的按键上触发 `UF_CHECK`。这条保证正是 `fromKeyName` 能保持
`noexcept` 且全函数的原因。`"ENTER"` 是 `VK_RETURN` 且**不带** extended 位；带 extended 的
小键盘 Enter 仍然是它自己的具名工厂 `KeyInput::numpadEnter()`。

收下 `"SHIFT"` 有一个后果值得写明：`wheelSpec` 在滚轮的 `wParam` 里仍然只报左键，从不报
`MK_SHIFT`。按住修饰键这件事现在可以表达了，但要到那个状态需要 `keyDown`，而它没有生产
调用方，所以本项目投出的滚轮消息不可能缺一个本该带上的修饰位。等 `keyDown` 有了第一个
调用方那天，再在那里推导它。

`modules/controller/source/controller/detail/input-message.hpp` 将动作确定性编码为
`PostSpec`：鼠标只使用 `WM_MOUSEMOVE`、`WM_LBUTTONDOWN`、`WM_LBUTTONUP`，
键盘只使用 `WM_KEYDOWN`、`WM_KEYUP`、`WM_CHAR`、`WM_UNICHAR`。最终唯一系统
调用位于 `modules/controller/source/controller/platform/windows-input.cpp` 的
`postInputMessage`。它先拒绝 null HWND 和 `HWND_BROADCAST`，然后把尝试写入
`AuditLog`，最后调用 `PostMessageW`。因此 audit record 表示“尝试投递”，包含
随后失败的普通 HWND；null/broadcast 因未尝试而不留 record。

`HeldInputs` 用 ordered set/map 保存 held keys 与 pointer，并绑定
`{ HWND, CaptureSessionId, TargetGeneration }`。`releaseHeld` 总是先清空内存状态，再按
key 后 pointer 的稳定顺序 best-effort 投递 Up，并为每项返回 `ReleaseOutcome`。
identity 不匹配时完全不投递。清空优先保证本进程不会把旧 held capability 带到
新目标，但也意味着失败的 Up 没有内建重试状态。

`AuditLog::records()` 返回借用内部 vector 的 span；下一次 audited delivery 可能
扩容并使它失效。`HeldInputs` 与 `AuditLog` 都由调用者拥有，controller 不隐藏
全局输入状态。

### DPI

`ensurePerMonitorAwareV2()` 调用 `SetProcessDpiAwarenessContext`，再用当前 thread
context 验证实际状态。成功设置返回 `DpiDeclaration::Declared`；只有 Win32
access denied 且实际 context 已是 V2 时才返回 `AlreadyDeclared`。setter 看似
成功但实际 context 错误、access denied 且 context 错误、或其他 Win32 error
都以 `InternalInvariant` fail closed。

这项调用应发生在任何依赖物理像素的窗口查询之前。它只建立进程坐标前提；
每窗口 DPI 由 discovery 的 `GetDpiForWindow` 取得，项目级 DPI fingerprint 的
持续校验不在此 API 内。

## 必须保持的约束

### Fail-closed 与代际隔离

- 目标选择必须唯一；identity 无法确认、窗口丢失、尺寸改变、WGC item closed、
  callback failure、stale arrival、无效 crop 或 surface mismatch 都返回结构化
  failure，不猜测、不裁剪、不复用旧 geometry。
- `Lost`、`InstanceUnconfirmed` 和 capture geometry invalidation 都会锁存。瞬时
  恢复不能让旧 lease 或旧 transform 自动复活。
- pointer delivery 在 `PostMessageW` 前重验 lease session、age、generation、
  client bounds、held binding 与窗口存活。当前该层没有“当前 FrameId”输入，
  因而不比较 lease frame ID；完整 frame-ID 比较发生在
  `modules/annotation/source/annotation/authorization.cpp` 的
  `authorizeCoordinateAction`。不能把这两层合并描述成 controller 已独立完成
  全四字段 fencing。
- 单独调用 input API 时，`IsWindow` 只能证明该数值当前是窗口，不能证明仍是原
  实例。产品路径依靠 `EngineSession::act` 紧邻 sink 调用前执行
  `WgcCaptureSession::validateTargetInstance`，再由 input 层做上述 lease 检查。

### 确定性

- selector 的字符串比较、候选数量规则、坐标 `floor`、Win32 `lParam` bit layout
  和 release ordering 都是显式规则；不存在 fuzzy match、随机选窗或隐式 rounding。
- `TargetGeneration` 与 session 内 `FrameId` 单调且不可回绕。相同输入状态只会
  产生 `Unchanged` 或恰好一次 generation bump。
- `FrameSlot` 明确定义 latest-frame coalescing；`StallTracker` 使用 monotonic
  arrival timestamp。真实桌面帧和调度时机本身不是可复现输入，但给定到达序列
  后，接受/拒绝条件是确定的。
- `AuditLog` 保持尝试顺序和 monotonic timestamp。它是诊断证据，不是成功确认；
  `PostMessageW` 成功只表示消息已入队。

### 所有权、生命周期与并发

- `WgcCaptureSession` 不可复制，以 `std::unique_ptr<Impl>` 独占 COM session、
  frame pool、D3D state 和 marker。owned process HANDLE 使用
  `UniqueProcessHandle`，COM interface 使用 `winrt::com_ptr`/projection，
  window marker token 使用 `winrt::handle`。
- asynchronous callback 只持有 shared `FrameSlot`，不持有 session borrow 或裸
  `this`。slot mutex 保护 latest/accepting，condition variable 负责发布；
  operation mutex 则串行 consumer-side geometry、stall、D3D 与 teardown。
- D3D Map 的裸 span 在 paired Unmap 前完成复制，不逃出 platform 函数。
  返回的 `Frame` 共享 immutable pixel buffer，可以安全越过 session 下一次
  capture 的生命周期。
- `close()` 当前不能中断已持有 operation mutex 的 `capture()` wait；并发 close
  会等到 capture timeout。这是已知边界，不应把 RAII teardown 误解为可取消等待。
- `HeldInputs`、`AuditLog` 和 `DeliveryTarget` 都是显式 caller-owned values；
  controller 没有隐藏 singleton。`AuditLog` 返回的 span 只在下一次 append 前有效。

### 严格后台与 platform/FFI 边界

严格后台不是运行时开关，而是可达 API 集合。`k_forbiddenBackgroundApis` 记录
六个原始 guard 名；`scripts/check_safety.py` 还静态禁止其他前台化 API 的直接
使用。controller 唯一注入 primitive 是 `PostMessageW`，不会调用 focus、
activation、global input 或 cursor-position API，也没有失败后降级分支。

所有 `Windows.h`、D3D、DWM、WinRT ABI、native out-parameter、opaque-handle
bit conversion 与 mapped pointer 都位于
`modules/controller/source/controller/platform/`。每处危险操作都有邻近
`// SAFETY:`，并在边界内转成 value、RAII owner 或 `Result<T>`。公开 header
不暴露 `HWND`、`HANDLE`、COM pointer 或 Windows SDK struct；`WindowHandle`
只是 pointer-width、非 owning 的 opaque value。

本模块没有单独 `ffi/` 目录：Win32 和 COM 的 FFI 责任由 `platform/` 完整承担。
`modules/controller/source/controller/detail/` 保存可移植的纯算法和窄 access
helper，主要目的是让边界规则可离线测试，而不是提供第二套 public API。

## 依赖关系

向下，controller 只跨两条模块边：

- `core` 提供 `Result`/`Status`、contracts、checked arithmetic/cast、strong
  values、non-wrapping generation、monotonic time 和 scope-exit。
- `domain` 提供 `Frame`、`FrameBuffer`、`CaptureSessionId`、`TargetGeneration`、
  `ObservationLease`、坐标空间、`CoordinateTransform` 与
  `AutomationErrorKind`。controller 产生或消费这些值，但不重新定义其语义。

向上，当前产品组合发生在 `entry/cli/run-windows.cpp`：

1. `ensurePerMonitorAwareV2` 建 DPI 前提；
2. `enumerateCandidates` 后由 CLI 做 title-substring 选择，再以 exact HWND
   `resolveTarget`；
3. 候选的 client size/DPI 构造一次 live fingerprint，entry 的
   `clientOriginDesktop` 构造初始 `ClientGeometry`；
4. 同一 HWND/session/generation 创建 `WgcCaptureSession` 和
   `DeliveryTarget`；
5. `entry/cli/platform/wgc-frame-source.hpp` 的 `WgcFrameSource` 把 capture 和
   marker validation 映射到 `engine::IFrameSource`；
6. `entry/cli/platform/controller-action-sink.cpp` 的 `ControllerActionSink` 拥有
   `DeliveryTarget`、`HeldInputs`、`AuditLog`，把 lease 原样传给 `uf::click`，
   click 失败后调用 `releaseHeld` 并保留原始错误。

`modules/engine/source/engine/session.cpp` 在 observe 前验证 frame source，在动作
授权后、sink 调用前再次验证目标实例。随后 annotation 检查 fingerprint、page、
detection、完整 frame identity 与 lease，controller 最后检查可在投递点观察到的
session/generation/age/bounds。跨边界的数据只有 domain values 和 structured
errors，没有 HWND 或 D3D resource 进入 engine。

`entry/workbench/platform/windows-capture-source.cpp` 复用 discovery 与
`WgcCaptureSession` 做 one-shot authoring source capture，再把 `Frame` 交给
source ingestion。其 visible/non-iconic 和“第一个 title substring match”策略
属于 workbench，不是 controller 的解析契约。

`entry/input-agent/` 与 `entry/m0-demo/` 直接使用 target、capture 和 input 接口。前者是标注前端
（见 [`entry-input-agent.md`](entry-input-agent.md)）；后者已冻结为真机验收参考，
其产品路径由 engine/CLI 组合取代。低层 `AuditLog` 记录 Win32 message
attempt，engine `ITraceSink` 记录 observe/authorize/deliver 等产品事件，二者目的
不同，不能互相替代。

## 测试

`tests/CMakeLists.txt` 只在 `${PROJECT_NAME}_controller` target 存在时注册
`test-controller`，所以这些测试是 Windows-only，但尽量把 OS-independent
规则提取后离线执行。

- `tests/controller/test-discovery.cpp` 钉住 pointer-width handle、失败的 live
  Win32 查询、best-effort process metadata、FILETIME 拼接和 loss-tolerant
  UTF-16 转换。
- `tests/controller/test-target.cpp` 钉住 exact/case-sensitive selector、零/多
  匹配拒绝、PID/HWND 冲突诊断、identity change 的单次 generation bump、
  `Lost`/`InstanceUnconfirmed` 锁存、PID reuse 与 explicit re-resolution。
- `tests/controller/test-dpi.cpp` 穷举 DPI setter 分类：success、access denied、
  actual-context mismatch、HRESULT low bits 和其他 error。
- `tests/controller/test-capture-wgc.cpp` 覆盖 `FrameIdCounter`、`ClientGeometry`、
  whole-pixel client extent、`CaptureGeometryState` 的 ContentSize latch 及
  `WgcCaptureOptions` timeout。
- `tests/controller/test-capture-stall.cpp` 钉住 timeout 边界、arrival-time
  freshness 和新 arrival 重置；`tests/controller/test-capture-os-build.cpp`
  钉住 19041/20348 capability threshold。
- `tests/controller/test-capture-readback.cpp` 覆盖 DWM/WGC crop geometry、far-edge
  bounds、overflow/zero rejection 和 padded-row BGRA8 packing。
- `tests/controller/test-input-message.cpp` 钉住 keyboard/mouse message bits、
  extended keys、UTF-8→UTF-16、signed-16-bit coordinates、失败投递先审计以及
  null/broadcast 零投递。
- `tests/controller/test-input-revalidation.cpp` 钉住 pointer fence 的检查顺序、
  lease session/expiry/generation、half-open bounds、finite/floor 规则、
  signed-16-bit 极限、keyboard generation 与 dead HWND。
- `tests/controller/test-input-held.cpp` 钉住 held binding、稳定 Up 列表、
  clear-before-attempt 和 mismatch 时零 post；`tests/controller/test-input.cpp`
  覆盖 empty delivery geometry、dead-target compensation、refresh identity 与
  long-press 前置拒绝。
- `tests/controller/test-audit-log.cpp` 钉住 runtime forbidden-name list 和
  `AuditLog` append 顺序；静态源码禁令另由 `scripts/check_safety.py` 执行。
- `tests/engine/test-session.cpp` 从端口另一侧钉住 delivery-edge target
  revalidation、lease pass-through 和各种授权失败时 sink click count 为零。

这些单元测试不创建真实 `WgcCaptureSession`，也不直接驱动
`WindowInstanceMarker`、free-threaded callback、D3D driver 或成功的真实窗口输入。
resize、recreation、minimize/stall、DPI、遮挡与焦点不变仍需要真机验证；
`docs/plans/2026-07-20-m0-demo-port-deviations.md` 将冻结的 m0-demo 定位为该验收
参考，而不是 CI 替代品。

## 后续扩展

以下扩展已有计划依据，但当前尚未实现。

第一组是 discovery/identity 加固。
`docs/plans/2026-07-20-post-port-win32-robustness.md` 的 S-1/S-2 指出
`probeCandidate` 与 `ResolvedTarget::readLiveIdentity` 仍是多次、非原子的 HWND
查询。`WindowInstanceMarker` 只保护已建立的 capture session，不能修复枚举阶段
把两个窗口的字段拼成一个快照。后续原子化或 post-observation identity recheck
应接在 platform probe 和 live revalidation 处，并保留
`InstanceUnconfirmed` 的 fail-closed 语义。S-4 的动态 title/class buffer 也属于
同一 discovery 边界。

第二组是持续 compatibility gate。
`docs/plans/2026-07-22-annotation-design.md` §2 与
`docs/plans/2026-07-21-lua-task-model-grill-decisions.md` D8 要求 recognition 和
Controller delivery 前持续复核 live size、integer DPI、target identity 与
transform。当前 `ResolvedTarget` 不保存 DPI，CLI 的 live fingerprint 只在启动
时构造，`DeliveryTarget` 也没有 compatibility proof。新增 fresh observation
必须在 `IFrameSource::validateTargetInstance` 和 delivery 边缘接入；不匹配应推进
generation 或返回 `TargetCompatibilityUnverified`，不能只更新
`CoordinateTransform` 后继续使用旧 lease。P1 的 base-to-live 自适应变换应保持
为 annotation/runtime 的独立值，不应塞进 controller 现有 live
Client↔Frame `CoordinateTransform`。

第三组是 capture liveness。
Win32 robustness 计划仍把遮挡判定、capture-wait cancellation，以及
stall timeout 与 lease age 配对列为开放项。共享信号可以接入 `FrameSlot` 的 wait
predicate 与 notification；`WgcCaptureOptions::captureStallTimeout()` 仍是捕获端
旋钮，动作 age 则由 `ObservationLease`/engine 配置拥有，二者需要在组合层协调。
任何优化都必须保留 arrival-time freshness、item-closed/marker 拒绝和
ContentSize latch，不能把静态画面简单当成安全新帧。

第四组是 input compensation。
同一 Win32 计划还记录了三个扩展点：`releaseHeld` 可在每个 best-effort Up 前
复核窗口实例；clear-before-attempt 可演进为显式 retry/persisted-held policy；
`scanCodeFor` 返回零时可在构造 `PostSpec` 前以 `ActionRejected` 拒绝。当前
`ControllerActionSink` 已是产品补偿的集中点，但 identity proof 和 retry
ownership 仍必须显式，不能把失败 Up 静默算作成功。

第五组是 DPI 与 borderless capability。
process-DPI 的更强验证可替换
`modules/controller/source/controller/platform/windows-dpi.cpp` 中基于 calling
thread context 的确认方式；现有调用仍要求在线程 DPI override 之前执行。
borderless capture 已由 `WgcCaptureOptions::requireBorderless` 预留 fail-closed
开关。实现前必须先建立由调用者持有的访问许可，再改变 session 设置；不能
仅凭 OS build 支持就报告 borderless 已兑现。
