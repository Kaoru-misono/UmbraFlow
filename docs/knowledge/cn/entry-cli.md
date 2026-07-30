# entry/cli 架构知识

`entry/cli` 是 `umbra-flow` 的命令行入口，也是 Windows 上为一次会话绑定桌面目标的
组合根。它把平台无关的 `engine` 端口接到 `controller` 的真实捕获与后台输入能力上，
并负责参数和进程退出码。

**`umbra-flow` 有两个子命令 `run` 与 `drive`，它们是同一张能力面上的两个前端。**
`run` 加载项目自带的 Luau 任务，由受信任 framework 驱动；`drive` 接收进程外操作者送来的
JSON 行命令，驱动同一批原语。两者都够不到对方够不到的东西，而**一个 generation 只接受其中
一个**：`TaskHost::Generation::claimFrontEnd` 在 `startTask` 与 `startOperatorSession` 中
先到的那个上闩住前端，此后该 generation 终身拒绝另一个。它报的是
`UnsupportedCapability` 而不是不变量失败——问是合法的，只是拿不到，这时二进制里没有任何
东西坏掉（2026-07-30，`ed38124`）。

运行生命周期已经不在这里。自 2026-07-29 起它位于 `task::TaskHost`
（`modules/task/source/task/task-host.hpp`），依据
`docs/plans/2026-07-29-three-layer-task-system.md` 第十三节。本目录余下的工作是：
解析参数、构造 adapter 与它们实现的端口、调用 host、打印它返回的报告；2026-07-30 起还多一件：
操作者协议本身，它是同一张 host 表面的文本编码，不含任何 policy。

## 入口职责

该目录拥有三类产品边界。

第一类是进程边界。`entry/cli/main.cpp` 把 `argv` 复制成拥有所有权的
`std::vector<std::string>`，识别 `run` 与 `drive` 两个子命令，输出 usage、已启动会话的
单行报告或错误，并把所有结果收敛为整数退出码。已启动的 run 无论完成、被取消还是失败，都会
报出 task 名、source hash、seed 和 trace 路径；已启动的 drive 会话报的是 queue、results 与
trace 路径——它没有跑任何 task，也就没有 task 名和 source hash 可报。失败再追加一行渲染后的
错误。未带子命令时打印项目名和**两份** usage：两种模式是平等公民，没有哪一个是读者默认
看到的那个。

第二类是命令边界。`entry/cli/args.hpp` 的 `RunArgs` 与 `DriveArgs` 分别是一次 `run` 和一次
`drive` 的完整值对象，由 `entry/cli/args.cpp` 的 `parseRunArguments` / `parseDriveArguments`
把成对的 value flags 转成路径、名称、预算和单调时钟 duration。解析成功后没有“未设置”的
可选字段：必填项必须非空，其余字段已采用安全默认值。

**两个参数形状先于其他任何东西说明了互斥。** `DriveArgs` 上没有 `--task`，`RunArgs` 上也
没有 `--queue`，因此“既有 task 又有命令队列”的会话根本拼不出来；`main.cpp` 又按一个子命令
token 选定模式，两个 handler 互不可达。但这两条都不是结构性的拒绝——真正兜底的是
`TaskHost` 的 per-generation 前端闩，无论从哪条路来的第二个前端都会被它拒绝。

第三类是组合边界。Windows 实现 `entry/cli/run-windows.cpp` 的 `runProduct` 与
`entry/cli/drive-windows.cpp` 的 `driveProduct` 选择一个真实窗口，创建捕获与输入两个
adapter，并把它们装进 `task::TaskRunConfig` 交给 `task::TaskHost`。CLI 不再提及
`engine::EngineSession`，由 host 构造并拥有它。两个命令都需要的桌面绑定——DPI 感知、
窗口枚举、候选选择、捕获与 controller 装配——只写一份，住在
`entry/cli/platform/target-binding.{hpp,cpp}`，返回一个 `BoundTarget`，于是两个命令不会对
“你说的是哪个窗口”给出两种答案。非 Windows 构建使用 `entry/cli/run-unsupported.cpp` 与
`entry/cli/drive-unsupported.cpp`，保留同一个产品二进制和可测试命令层，但两个产品入口都
明确返回 `UnsupportedCapability`。

该目录不负责以下工作：

- 不拥有识别、页面判定、动作授权或 observation 生命周期；这些属于
  `modules/annotation/` 与 `modules/engine/`。
- 不拥有 Win32 窗口枚举、WGC、目标 generation、lease 校验或消息投递；
  这些属于 `modules/controller/`，CLI 只做薄适配与装配。
- 不定义任务语言或通用工作流，也不再运行固定流程。`runSmokeFlow` 这条单步路径
  （等待一个页面、寻找一个 action target、存在时点击一次）已于 2026-07-29 删除
  （`docs/plans/2026-07-29-three-layer-task-system.md` 第十六节）。`runProduct`
  绑定目标后调用 `TaskHost::startTask`，run 内部发生什么由项目的 task 脚本决定。
- 不做资源名称翻译。脚本直接以 `uf.pages.NAME` 与 `uf.recognizers.NAME`
  命名资源（根已于 2026-07-29 由 `umbra` 改名为 `uf`，`2f4af93`），
  `modules/task/source/task/script-validator.hpp` 的
  `task::validateScriptResources` 在 VM 存在之前就把每处引用对能力面闭合。
  `entry/cli/name-resolution.{hpp,cpp}` 只为翻译那两个已删除 flag 而存在，
  已随它们一并删除。
- 不读取 authoring 文档，也不生成 manifest 或模板，甚至不负责加载：它只给出已发布
  项目目录，由 `TaskHost::loadProject` 经 `engine::loadRuntimeProject` 读取。
- 不提供前台或全局输入降级。后台投递失败就是产品失败。
- 不承担守护进程、托盘、调度或多任务生命周期；当前设计是一进程一次会话——要么一次 `run`，
  要么一次 `drive`，绝不同时。
- `drive` 不新增能力。操作者协议是私有能力面的文本编码，外加两个用它拼出来的循环；
  操作者遇到的每一条拒绝就是 task 遇到的那一条，因为两者调的是同一个 `TaskContext`。

这条边界解释了为什么目标发现留在 entry，而没有进入 `engine`：
`engine::IFrameSource` 接收的是“已经绑定的一个目标”，从而保持平台无关；
窗口标题选择、DPI 声明和 Win32 几何则是产品在 Windows 上如何取得该端口的
策略。

## 命令执行流程

### 命令行入口

`entry/cli/main.cpp` 的 `dispatch` 有三个公开产品路径：

- 空参数：打印生成的 `application::k_name` 与 `usageText()`（两个子命令的 usage），返回 `0`。
- 首参数为 `run`：把剩余参数交给 `dispatchRun`。
- 首参数为 `drive`：把剩余参数交给 `dispatchDrive`。

其他首参数被视为 unknown subcommand，打印错误与两份 usage，返回 `1`。
`main` 还检查 `argumentCount` 能否安全转换且不为零，并在最外层捕获
`std::exception` 和未知异常；异常不会穿过进程边界。

`entry/cli/args.hpp` 的 `RunArgs` 对应以下真实 CLI 表面：

- `--project DIR`、`--selector TITLE-SUBSTRING`、`--task NAME` 是 **`run` 的**必填项；
  `--task` 指名已发布项目里的 `tasks/NAME.luau`。
- `--budget N` 默认 `k_defaultPixelComparisonBudget == 1 << 31`，限制一次识别的像素比较数。
  2026-07-30（`2429578`）之前它是 `1 << 28`，而那个数跑不完一个普通页面：
  `evaluateGrayPage` 把一份预算摊给**一页上的每个 anchor**，所以要覆盖的是它们的和而不是
  单次搜索。常量处写下的包络是 480x90 搜索区里的 200x50 模板——11,521 个位置乘 10,000 像素
  ——十六个这样的 anchor 合计 1,843,360,000。对小模板做全帧搜索仍被有意留在默认之外
  （约为上限的 1.9 倍），这样的项目必须自己标搜索区，或者明知故犯地抬高 `--budget`。
  这个常量与 `umbra-authoring` 共用，于是验证与运行由同一个数管着。
- `--recognition-timeout MS` 默认 2000 ms，是每次识别的 deadline。
- `--max-frame-age MS` 默认 750 ms，决定 observation lease 可用于动作的时长。
- `--trace PATH` 默认 `umbra-flow-trace.jsonl`。

`DriveArgs` 绑定目标与项目的方式和 `RunArgs` 完全一致，`--budget`、`--recognition-timeout`、
`--max-frame-age`、`--trace` 是同名同默认值的同一批字段——两个前端不能在不同的保证下运行。
取代 `--task` 的是一对 IPC 文件：

- `--project DIR`、`--selector TITLE-SUBSTRING`、`--queue PATH`、`--results PATH` 是
  **`drive` 的**必填项。
- `--idle-timeout S` 默认 120 秒，命令队列安静下来就结束会话，免得操作者走开、或驱动进程
  已经死了，会话还一直占着捕获设备和一个绑定目标。这个数与 m0-demo input agent 自己的
  idle timeout 一致——它遵循的就是那套协议。

`DriveArgs` 上**有意没有任何 timeout、轮询间隔或重试默认值**。那些是 policy；每个便利命令
都把它们要求为必填字段（见下），而一个这样的 CLI flag 会成为 task 侧 policy 的第二个住处。

整数使用 `std::from_chars` 完整消费输入，duration 在转换前检查目标表示范围。
flag 以“flag 后紧跟 value”的二元组读取；未知 flag、缺值、非整数和越界值都
返回 `InvalidResource`。因此后续组合只处理类型化值，不重复解析字符串。

**页面等待的两个 flag 已删。** `--timeout SEC` 与 `--poll MS` 曾经是脚本页面等待的宿主
默认值，经 `task::TaskRunConfig` 转发进 `TaskContextConfig`。2026-07-29(`d1a0685`)
等待循环整个搬进受信任 Luau framework 之后，宿主没有任何代码再读这两个值，它们连同两个
config 字段一起删除；今天它们会被上面那条未知 flag 规则**当作未知参数拒绝**。等多久、
多久重看一次现在是 `modules/task/runtime/ctx.luau` 里的 `k_defaultTimeoutMillis`
（600000 ms）与 `k_defaultPollMillis`（500 ms），脚本按 `{ timeout_ms, poll_ms }` 覆盖。
保留的 `--recognition-timeout` 是每次识别的 deadline，与页面等待无关。

### 操作者协议（`drive`）

`entry/cli/drive-protocol.{hpp,cpp}` 定义线协议，`entry/cli/drive.{hpp,cpp}` 跑循环。命令以
**JSON 行追加进 `--queue`**，**每条命令有且只有一行 JSON 结果追加进 `--results` 并立即 flush**，
因此读文件的操作者在下一条命令执行之前就能看到上一条的回答。`k_maxDriveCommandBytes` 把单行
封顶在 64 KiB，与 m0-demo input agent 的上限一致：一条命令是几个标量，接近这个数的只可能是
畸形行而不是大行。

**IPC 路径上的三条拒绝，由 `validateDriveIpcPaths` 在触碰桌面之前完成。** 队列必须已经存在，
因为由会话创建它会与正在追加的操作者相竞争；两个路径必须不同，因为读自己 results 的会话会把
它们重新执行一遍；而**results 路径必须不存在**——操作者的输出永远是一个新文件，于是上一次会话
留下的旧 results 绝不会被当成这一次的，也不会被静默追加或覆盖。最后这条是 m0-demo input agent
的守卫，有意照搬过来：它抓到过两次真实的操作者失误。

**队列按字节偏移读。** `QueueReader` 持有会话自己的偏移量，因此无论轮询多少次，一行只执行一次；
而且**尾部不完整的一行会被扣住，直到它的终止符到达**——操作者分两次写入追加一条命令，绝不会被
执行掉一半。队列变小意味着它在会话底下被替换或截断了，偏移量此时指的是另一个文件的字节，
按 `InvalidResource` 拒绝。

**两层命令，一份 policy。** 第一层是私有能力面的逐字复制：一个原语一条命令，名字、参数与失败
模式都与 Luau 表面相同，只把不透明句柄换成整数 id——操作者手里只有文本。它们是
`cycle_open`、`cycle_close`、`cycle_page`、`cycle_find`、`cycle_click`、`key`、`settle`、
`deadline`、`wait`，外加 `quit`。每一条恰好映射到一个 `task::OperatorSession` 动词，再恰好
映射到一次 `TaskContext` 调用——与 `ctx.luau` 发出的是同一次调用。第二层是便利命令
`wait_page` 与 `find_click`，各自替掉一段手写循环。

让第二层安全的那条约束是：**便利命令不带任何自己的 policy 默认值**。每个 timeout 与轮询间隔
（`timeout_ms`、`poll_ms`）都是**必填**字段，缺了就拒绝而不是补上，于是
`modules/task/runtime/ctx.luau` 保留它的默认值，并且仍是 task 侧 policy 的唯一住处，C++ 这边
不存在第二份会与它走偏的拷贝。命令不接受的字段同样是被拒绝而不是被忽略。第二层只由第一层的
动词组成：它决定什么时候停止循环，而这个决定用到的每一个数都由调用方给出。

`DriveResult` 只有一种行形状而不是十二种——`ok` 加上各动词可能产出的那些可选值——所以操作者用
一个 parser 读结果。`ok` 是每个调用方都依赖的那条区分，因此被拒绝的命令绝不会把它报成 true。
失败行的 `errorKind` 用的是 domain 自己的 wire 拼写，和 trace 行、task 的 Tier B 错误读到的
是同一个串。被拒绝的命令通常是正常结局——操作者读一行然后换一条命令试——只有一个例外：
`Cancelled` 会结束会话，因为 generation 一旦花掉，之后每个原语都会在终局闩上被拒，继续下去
只会把队列空转到 idle timeout。

另一侧的消费者是 `task::OperatorSession`（`modules/task/source/task/operator-session.hpp`），
它是受信任 Luau framework 的**同级兄弟**，而不是通往 Luau 的一条路。保证层在 Luau 之下——
观察周期账本、四要件点击授权、指纹检查和 trace 全在 `TaskContext` 背后的 C++ 里——所以操作者
拿到的原语与拒绝，和 task 拿到的一模一样。这里没有 chunk、没有源码、没有任何字符串会变成代码，
沙箱关掉的 eval 路径也一条都没重开。操作者能命名的只有 task 能命名的：会话持有一份与
`uf.recognizers` / `uf.pages` 同源的 `CapabilitySurface`。`raise`、`emit` 与 `random` 被有意
排除——操作者没有自己的 framework 结构要记录，而放进 `emit` 会给 `framework.*` 事件添第二个
作者，而 trace 校验状态机现在在 operator 流上对这类事件直接拒绝。

### 项目加载

`runProduct` 以项目目录和携带 console stop token 的 `task::TaskHostConfig` 调用
`task::TaskHost::loadProject`。host 再调用
`modules/engine/source/engine/runtime-loader.hpp` 的
`engine::loadRuntimeProject`：读取 `generated/annotations.runtime.toml`，在读取前
应用 16 MiB 大小上限，再按 manifest 引用读取 `assets/templates/<hash>.png`，由
`annotation::RecognitionRuntime::create` 校验 hash closure 并解码模板。

随后 host 用 `task::CapabilitySurface::create` 从
`loaded.runtime.manifest().catalog()` 构建脚本可见的能力面，把已加载 runtime 与该
能力面一起注册为一个 generation，并返回它的 `GenerationId`。generation 比针对它的
每一次 run 活得更久：runtime 与能力面按项目存在，而 trace recorder、engine session、
task context 和 VM 按 run 存在。

这段工作在访问桌面之前完成：错误的项目路径、损坏的 manifest 或缺失模板不会先声明
DPI、枚举窗口或创建捕获资源。该顺序是承重的，并于 2026-07-29 真机复验——先绑定目标
会让错误的 `--project` 报成窗口错误，而不是 manifest 错误。

### Windows 组合序列

`entry/cli/run-windows.cpp` 严格按以下顺序装配。其中第 3 到第 8 步已经不写在那里了：
2026-07-30（`ed38124`）起它们住在 `entry/cli/platform/target-binding.cpp` 的
`platform::bindTarget` 里，与 `drive-windows.cpp` 逐字共用；后者按同样顺序做第 1、2 步和
第 3–8 步，唯一的差别是 `run` 调 `startTask` 的地方它调 `TaskHost::startOperatorSession`。
`drive` 还在第 1 步之前先校验 IPC 路径，因此一个已经存在的 results 文件会在 console handler
都还没安装时就失败。

1. `entry/cli/platform/windows-console-cancellation.hpp` 的
   `platform::ConsoleCancellation::install` 注册 Ctrl-C/Ctrl-Break handler 并取得
   `std::stop_token`。它在任何可能阻塞的动作之前最先安装，其 registration 声明在
   之后所有局部量之前，因而最后才注销。该 token 成为
   `TaskHostConfig::externalCancellation`，generation 把它与 `TaskHost::cancel`
   驱动的 stop source 合并，使外部停止与显式取消汇入同一个源。
2. `TaskHost::loadProject` 如上一节所述加载项目，发生在任何桌面接触之前。
3. `modules/controller/source/controller/dpi.hpp` 的
   `ensurePerMonitorAwareV2` 声明 per-monitor-aware V2。后续 client size、
   client origin 与 DPI fingerprint 才处在一致的坐标解释下。
4. `enumerateCandidates` 枚举窗口；`entry/cli/candidate-selection.hpp` 的
   `selectCandidate` 要求标题包含 `RunArgs::selector` 且可捕获（可见、未最小化）的
   候选恰好一个。零个或多个都返回 `TargetUnavailable`，不会依赖枚举顺序猜测。
5. 用所选 handle 构造 `TargetSelector`，再由 `resolveTarget` 得到
   `ResolvedTarget`，读取 `ClientSize`、`WindowHandle` 与当前
   `TargetGeneration`。当前一次性进程使用固定 `CaptureSessionId{1}`。
6. 从 resolved client width/height 与候选窗口的 DPI 创建实时
   `annotation::ProjectFingerprint`。它不是替换 manifest fingerprint；
   两者在 recognition 和 action authorization 中必须相等。
7. `platform::clientOriginDesktop` 通过 `ClientToScreen` 求 client `(0, 0)`
   的 desktop-space 原点；与 client extent 一起创建 `ClientGeometry`，再创建
   `WgcCaptureSession`。用同一 handle、session、generation 和 client extent 创建
   `DeliveryTarget`，保证捕获身份与投递身份来自同一次 target resolution。
8. `platform::WgcFrameSource` 与 `platform::ControllerActionSink` 包住上面两个资源，
   连同 live fingerprint 和 CLI 的调节值填成一个 `task::TaskRunConfig`，移动交给
   `TaskHost::startTask`。

`TaskRunConfig` 不是普通 config，而是一条移入式所有权边界：它携带
`std::unique_ptr<engine::IFrameSource>` 与 `std::unique_ptr<engine::IActionSink>`，
调用方那一份因此被置空，端口生命周期归这次 run。除端口外它还携带 live fingerprint、
像素预算、单次识别期限、最大帧年龄和 trace 路径。**它不带页面等待预算**：等多久、多久
重看一次由 task 在 Luau 里决定，宿主留一份读不到的回退值只会是一个没人读的字段。

真正的运行发生在 `startTask`：它加载并校验 task、打开 trace，再依次构造
`trace::TraceRecorder`、`engine::EngineSession`、`task::TaskContext` 和 VM，只在这次
run 的时长内存活；recorder 声明在所有借用者之前并由 `unique_ptr` 持有，地址固定，
因此三者在包括提前返回在内的每条路径上都先于它销毁。CLI 看不到这一切：它拿到一个
`task::TaskRunReport` 并打印一行。

### 两个端口适配器

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

它的第二个动词 `pressKey(key, actionGeneration)` 以 `KeyInput::fromKeyName(key)` 转发到
`uf::keyPress`，传的是 `TargetGeneration` 而不是 lease。这不是漏传：按键不指名坐标，
没有任何矩形的位置会因此过时，也就没有东西要靠帧年龄去保护；仍然必须成立的是这次按键落到
observation 来自的那个目标实例上，而 generation 携带的正是这件事。两种授权的完整对照见
`module-engine.md`。

如果 pointer 或按键的 down/up 链中任一步失败，adapter 保留原始错误，并调用
`releaseHeld` 排空可能残留的 held input——按下投出去而释放没投出去，会把键卡在目标里。
补偿 release 的失败只追加 context，不会遮蔽最初的失败。`AuditLog` 和 held state 都是
adapter 的拥有型成员，不会借用 `runProduct` 栈上的临时对象。

第三个端口已经不是 CLI 的 adapter。`trace::FileTraceSink` 位于
`modules/trace/source/trace/file-sink.hpp`，由 `TaskHost::startTask` 在 task 加载并
校验之后才打开，因此拼错的 task 名不会留下证据文件。`create` 以 binary + trunc 模式
打开路径并返回 `std::unique_ptr<trace::ITraceSink>`；打开失败是 `IoFailure`。每次
`emit` 写一条序列化 JSONL、追加换行并立即 `flush`，写或 flush 失败同样向调用方返回
`IoFailure`。这避免事件跨 emit 留在 C++ stream buffer 中，但代码没有声明文件系统级
durable sync 保证。

### 退出码契约

`entry/cli/run.hpp` 的强类型 `ExitCode` 统一定义退出码，`run.cpp` 负责把结构化错误
映射到该枚举；只有 `main.cpp` 在进程边界使用 `std::to_underlying` 转为 `int`：

| 退出码 | 含义 |
|---:|---|
| `0` | 空命令的帮助路径，或 task / drive 会话运行至完成 |
| `1` | unknown subcommand、参数/资源错误、unsupported host、绝大多数运行失败或未捕获异常 |
| `2` | `TargetCompatibilityUnverified` |
| `4` | `Timeout` |
| `5` | `Cancelled`，或运行失败返回时 console stop 已被请求 |

`3` 是刻意缺席，并且会一直缺席。它曾是 `ActionAbsent`，唯一产出者是 2026-07-29 删除的
smoke 路径；目标缺席意味着什么，现在由 task 脚本自己决定。该值被留空而不是改派，
因为读到旧日志里 `3` 的操作者或脚本，绝不能被告知它原来是别的意思。

所有 CLI 路径都返回 `ExitCode`，避免 `EXIT_FAILURE` 与裸数字分散表达同一契约。
`exitCodeForError(error, stopRequested)` 先检查 `stopRequested`，再检查
`AutomationErrorKind`。因此 Ctrl-C 发生在阻塞 capture 等步骤时，即使底层最终
冒出 `CaptureStalled`、`IoFailure` 或 `Timeout`，操作者的取消意图仍优先报告
为 `5`。参数解析早于 handler 安装，解析错误明确以 `stopRequested=false`
映射；非 Windows 实现也始终报告未收到取消。

已经启动的会话由 `exitCodeForReport(report, stopRequested)` 映射，这是该映射的唯一
定义，`run` 与 `drive` 共用它——两者都以一个 `task::TaskRunReport` 收尾：不带 failure
的报告是 `Success`，其余一律把结束它的 failure 交给 `exitCodeForError`。因此被取消的 run 报 `5` 是因为它的 failure kind 是 `Cancelled`，
而不是这个函数另外知道取消这回事；从未启动的 run 也按同一条路径映射同一种 kind。

错误文本由 `formatRunError` 组合 automation kind、message、全部 context，
以及存在时的 native error category/value。这是 CLI 的单行诊断格式，不改变
底层 `Error` 的分类。

## 必须保持的约束

**Fail-closed。** 项目加载在任何桌面副作用之前完成，task 的脚本资源校验在 VM 存在
之前、trace 文件打开之前完成，因此拼错的 task 名或无法解析的资源名既不会留下 VM，
也不会留下证据文件；窗口 substring 必须
唯一；DPI 声明、几何创建、WGC、delivery 和 trace 任一步失败都通过
`UF_TRY` 立即终止。live fingerprint 不匹配 manifest 时，
`RecognitionRuntime` 拒绝识别，`authorizeCoordinateAction` 也再次拒绝动作。
动作 authorization 之后，`EngineSession::act` 还会在 sink 调用前通过
`IFrameSource::validateTargetInstance` 复验绑定实例，失败时零投递。

**两层 stale-observation fence。** engine 层用 observation 携带的
`ObservationLease`、`ResolvedPage` 和 `ActionDetection` 调用
`authorizeCoordinateAction`；CLI adapter 又把同一 lease 转交 controller，
使其在 post 时复验 session、generation 与 age。任何一层失败都不发送点击。
成功点击后 observation 在可能失败的 post-click trace 之前即被标记 invalid，
避免 trace 失败诱发重复投递。

**确定性。** 参数转换不依赖 locale；脚本资源校验把每个名称对能力面按 catalog 顺序的
稳定句柄表解析；窗口选择拒绝多匹配而不取“第一个”；同一 `Observation` 同时承载
页面与动作证据；默认点击点和坐标变换由 annotation/engine 计算。trace 使用
固定 schema serializer。`TaskHost` 每次 run 从 `std::random_device` 抽一枚新种子并
盖进 `run.started`，因此一次 run 的随机序列可以从它自己的记录重放；悄悄恒定的种子
在 trace 里看着完全正确，却会毁掉这条性质。真实窗口内容与到达时序本身不是确定输入，
但入口不再额外引入隐式选择。

**所有权与生命周期可见。** `RunArgs` 与经 `TaskRunConfig` 交给 `TaskHost` 的两个
adapter 都按值或 `unique_ptr` 移动；`LoadedRuntime` 归 generation 所有，每次 run 拷贝
一份（不是每帧一份），使 generation 下一次 run 仍有 runtime 可用。
`ConsoleCancellation` 以 RAII 管理 handler
registration；实际 `stop_source` 是 module-static process-lifetime 对象，所以
handler 注销后退出码边界仍能读取 stop 状态。该 source 一旦停止不会复位，这与
“每进程恰好一次 run”契约一致。

`engine::Observation` 不 borrow 它的 `EngineSession`，只与产生它的 session
共享一个私有、不可变的 identity token；移动 session 不会留下悬空
back-reference，其他 session 仍能拒绝该句柄。observation move 后源对象失效，
`act` 又按 rvalue 消费它，类型和运行时 flag 一起限制复用。

**严格后台。** CLI 不调用 focus、activation 或全局输入 API。
`ControllerActionSink` 最终进入
`modules/controller/source/controller/platform/windows-input.cpp` 的
`PostMessageW` 路径，只向解析出的单一 HWND 投递整数 mouse message——2026-07-30 起还有
key message——并拒绝 null 与 `HWND_BROADCAST`。`pressKey` 走的是同一条路、同一份审计记录、
同一条 `HWND_BROADCAST` 拒绝，且 down 与 up 之间不做任何保持。目标失活、消息投递失败或兼容性无法确认时都失败，
不存在“为了成功”切到前台输入的 fallback。

**Trace 是正确性路径的一部分。** `trace::ITraceSink::emit` 返回 `Status`，run 括号
（`run.started`、`run.resources_validated`、`run.finished`）、识别失败、授权与投递
相关事件都可令操作失败。`trace::FileTraceSink` 也不吞写入错误。这使“无法留下所要求
的证据”成为显式产品失败，而不是不可见的 best-effort 丢日志。收尾一端同样成立：run
本身成功但 `run.finished` 写不出去时，`startTask` 把那个 `IoFailure` 放进报告，于是
这次 run 报为 Failed——残缺的 trace 不是一次完成的 run。run 自身的失败始终优先，
因此只有在没有其他失败时才会出现这种情况。

## 依赖关系

入站边是 shell/operator → CLI。跨边界的是字符串参数、项目路径、目标标题
substring、名称和退出码；CLI 在这一层负责可读诊断与默认值。

出站边现在指向 `task`，通过 `entry/CMakeLists.txt` 中的
`${PROJECT_NAME}_cli_support` 建立：它在所有 host 上都 PUBLIC 链接
`${PROJECT_NAME}_task`——退出码边界要映射 `task::TaskRunReport`，所以即使 Windows
组合不参与构建，task 模块也属于这个库的接口。`${PROJECT_NAME}_engine` 同样保持
PUBLIC，因为两个 adapter 实现的是 engine 端口。跨边界的是：

- 两个拥有型 port implementation 与实时 `annotation::ProjectFingerprint`，以
  `task::TaskRunConfig` 移入，另带像素预算、识别期限和最大帧年龄；
- 项目路径、task 名称与 trace 路径；
- `GenerationId`、`task::TaskRunReport` 和结构化 `Error`；
- `drive` 还多一样：`startOperatorSession` 交回来的
  `std::unique_ptr<task::OperatorSession>`，由 CLI 逐动词驱动，最后用
  `OperatorSession::finish` 关掉。`startTask` 会阻塞整个 run，`startOperatorSession` 不阻塞
  ——这是两个前端唯一的生命周期差别。

`task` 与 `engine` 都不看到 HWND、console handler、文件选择语法、标题 substring，也不看到
queue 与 results 文件。反方向，CLI 不解释 recognizer evidence、page outcome 或授权规则，
也不再驱动 `EngineSession` 的任何动词：它只调用 `loadProject`，然后调 `startTask` 或
`startOperatorSession`，再读报告。

向 `controller` 的出站边只存在于 Windows build。跨边界的是 resolved
`WindowHandle`、`ClientSize`、`Dpi`、`TargetGeneration`、`ClientGeometry`、
`WgcCaptureSession`、`DeliveryTarget` 和 `ObservationLease`。WGC 产生的
`Frame` 带 session/generation/frame identity 与 coordinate transform，
最终 click 又携同一 identity lineage 回到 controller。

向 `annotation` 的协作现在完全间接。CLI 只直接创建 `ProjectFingerprint` 以表达真实
目标的尺寸/DPI，其余一概不碰：它不读取 `RecognitionCatalog`，不修改 catalog，也不
创建 authoring resource。

`${PROJECT_NAME}_cli_support` 的拆分是测试架构的一部分，而非单纯构建便利。
`entry/cli/main.cpp` 只保留薄进程壳；参数解析、错误格式化和退出码映射进入 static
library，供 executable 与 `test-cli` 共同链接。Windows 时 library 再加入真实 adapter 并链接 controller；其他平台加入
`run-unsupported.cpp`。因此平台无关契约能在没有 Windows desktop 的 CI 中测试，
而产品 executable 不需要导出内部函数。

根据 2026-07-28 的开发者决定，仓库根 `manifest.txt` 是应用名和版本的权威来源。
顶层 CMake 从中得到 `PROJECT_NAME`/`PROJECT_VERSION`，`entry/CMakeLists.txt`
再把 `application-info.hpp` 生成到 build tree，供 CLI executable 私有使用。因此
`main.cpp` 读取的是类型化生成元数据，不需要在 `core` 放产品常量，也不需要全局
compile-definition macro。

## 测试

`tests/cli/test-args.cpp` 钉住完整 flag happy path、所有可选默认值、三个必填项、
unknown/missing/non-integer 输入、poll 的 1/60000 ms 边界，以及两条退出码映射——
按 failure kind 的 `exitCodeForError` 与按 run 结局的 `exitCodeForReport`，其中包括
“收尾时才收到停止的已完成 run 仍报成功”。它还钉住 `--page` 与 `--action` 是被拒绝
而不是被忽略，使过时调用大声失败而不是悄悄跑成另一个 task；以及 usage 文本含
`--task` 且不含这两个已删 flag。其中 cancellation-priority 用 `CaptureStalled`、
`Timeout`、`IoFailure` 配合 `stopRequested=true`，直接防止底层错误覆盖 Ctrl-C 意图。

`tests/cli/test-drive-protocol.cpp` 在所有 host 上钉住操作者协议——它不提任何 Win32 类型：
第一层每条命令的解析、让便利命令不带 policy 默认值的必填字段拒绝、命令不接受的字段是被拒绝
而不是被忽略、结果行形状，以及三条 IPC 路径拒绝。

`tests/task/test-operator-front-end.cpp` 钉住它下面那层会话，做法是把同一个场景沿两条路各跑
一遍：无同帧 page 的点击、过期帧上的点击，在 task 路径与 operator 路径上被同样地拒绝；
操作者能命名的恰好是 task 能命名的、多一个都没有；每条 operator trace 行都归属 operator 前端；
operator 流对 `framework.*` 事件直接拒绝。它还钉住一次投出去的按键会消费掉它的周期，
以及此后下一个动词收到的那句拒绝**不提 click**——花掉这个周期的是一次按键
（2026-07-30，`2075404`）。互斥本身在 `tests/task/test-task-host.cpp`：一个 generation 只驱动
一个前端，先到的那个说了算。

`tests/task/test-task-host.cpp` 覆盖原先住在这里的运行生命周期。它把一个真实
annotation 项目发布到临时目录，用 fake 端口把 `TaskHost` 端到端跑通，再把 trace 文件
读回来：验收用例断言整条有序的 `umbraflow-trace/v1` 括号——从 `run.started` 经 engine
事件与 `task.native_call` 直到 `run.finished`，且处在同一 seq、同一 run 与 generation
身份下。它还钉住同一 task 的两次 run 抽到不同种子、报告里的种子就是 `run.started`
记录的那枚、失败的 run 是被报告而不是让调用失败、缺失的 task 在打开任何 trace 文件
之前就失败，以及 P2 动词报 `UnsupportedCapability`。

CLI adapter 的安全语义由下游测试分层固定：

- `tests/engine/test-session.cpp` 覆盖完整 observe→resolve→find→act、
  fingerprint mismatch、未授权 action、过期 lease、失效/跨 session
  observation、投递边缘 target revalidation、取消，以及 `observe` 交出去的
  `CaptureBudget` 是不是一个真期限。
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

`tests/CMakeLists.txt` 的 `test-cli` 在所有 host 上编入 `cli/test-args.cpp` 与
`cli/test-drive-protocol.cpp`，仅在 Windows 上追加 `cli/test-candidate-selection.cpp`
（`selectCandidate` 也只在那里参与编译），并链接 `${PROJECT_NAME}_cli_support`。当前没有
直接单元测试覆盖 `main`/`dispatch`、console registration、client-origin adapter、
`platform::bindTarget` 或整条真实 Windows 组合；这些属于真机 smoke/E2E 验证面，不能用现有
离线测试的绿色替代。运行生命周期已经
不在这份清单上：它搬进 `TaskHost` 正是为了变得可测。

## 后续扩展

`docs/plans/2026-07-29-three-layer-task-system.md` 是当前 CLI/task 组合的直接权威。
其第十三节把 `TaskHost` 的动词集从 P0 一直锁到 P2，使常驻 host 取代 CLI 时 API 面
不变；第十六节记录了 smoke flow 连同 `--page`、`--action` 与 `ExitCode::ActionAbsent`
的删除。因此新增编排属于受信任的 Luau framework 或 `TaskHost`，不得回流到
`runProduct`。

`docs/plans/2026-07-23-engine-architecture.md` 仍是 engine/端口组合本身的权威，
并列出以下扩展点：

- P3 第二平台通过实现相同 `IFrameSource`、`IActionSink`、`ITraceSink` 接入；
  CLI host implementation 可以替换 `run-unsupported.cpp`，engine 无需感知平台。
- B2 Luau 对 `Observation`、observe/find/act/wait 做 1:1 binding；现有 C++
  API 形状就是为避免届时重构而保留。
- D6/P1 弹窗处理**已不在 engine**：`sweepKnownPopups` 那个 no-op hook 随
  `waitForPage` 于 2026-07-29(`8b16f2d`)删除，弹窗现在由受信任 Luau framework 的
  interrupt 注册表处理（`task.interrupt` 声明，`ctx:wait_for_page` 每轮匹配）。这一条
  对 CLI 的结论不变：它仍然不该塞进窗口发现或 argument parsing。
- P0-C 若 UIPI 真机验证要求分进程提权，计划要求把 m0-demo input-agent 的协议
  语义复制到 runner adapter 层，而不是链接已经冻结的 m0-demo。

`drive` 前端不改变这条权威：它是同一张 `TaskHost` 表面的第二个消费者，所以除
`startOperatorSession` 之外没有新增任何 host 动词。操作者想要而 task 做不到的东西，属于
能力面——放在那里两者都能拿到——而不属于 `drive-protocol.cpp`。

`docs/plans/2026-07-21-product-form-and-roadmap.md` 把当前形态定义为“P0 CLI
单任务、严格后台、可靠取消”，并把 P2 定义为托盘常驻 App。届时可复用的边界是
`TaskHost` 加上 host 为它绑定的端口，但进程生命周期、任务列表、计划任务和
UI 状态属于新的 app shell，不应反向成为 CLI、task 或 engine policy。

该 roadmap 的 B3 还要求硬取消与长程稳定性；
`docs/plans/2026-07-20-post-port-win32-robustness.md` 记录了 capture wait 取消、
stall timeout 与 lease age 配对等仍开放的 controller 问题。相关改进应落在
WGC/controller 能力及其 adapter 连接处，并保持当前原则：取消或 capture
不确定性只能终止动作，不能放宽 lease 或改用前台输入。

分辨率自适应、OCR、pause/resume 和常驻调度也被上述计划明确放在当前 Phase 3
之外。扩展它们时应沿既有边界加入新的 recognition/engine/app 能力；尤其不能
通过在 CLI 中伪造 live fingerprint 来绕过当前尺寸/DPI fail-closed 契约。
