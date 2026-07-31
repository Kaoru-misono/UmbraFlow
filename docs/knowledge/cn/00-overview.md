# UmbraFlow 全系统架构总览

本文说明 2026-07-24 仓库中已经可以构建和运行的架构。模块依赖以
[`docs/ARCHITECTURE.md`](../../ARCHITECTURE.md) 为准；尚未完成的产品能力见
[`docs/plans/`](../../plans/README.md)。

## 系统做什么

UmbraFlow 根据视觉证据授权后台操作。平台无关模块负责识别和授权；Windows 代码
负责目标捕获与输入投递。二者只在 `entry/` 组合，因此识别策略不依赖 HWND，
controller 也不需要了解 page 或 element。

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
entry/cli            -> task（外加 engine、controller）
entry/authoring      -> entry/workbench + entry/cli（外加 image）
entry/workbench      -> annotation（外加 image）
```

> 更正（2026-07-31）：`entry/workbench` 还是 GUI 的时候另外读 `engine` 与 `controller`。
> `b57b67b` 归档了那层外壳，留下的是标注后端库，它 public 链接 `annotation`、private 链接
> `image`。engine 仍然会进到标注流程，但走的是 `entry/cli`——`umbra-authoring match`
> 跑的是真的 `RecognitionRuntime`。

图中箭头表示左侧模块依赖右侧模块。`vision` 和 `image` 位于同一层，互不依赖。
`modules/task` 与 `modules/trace` 没有画在这里，也还没有自己的页；这两页还欠着的范围见
`README.md`。

| 模块 | 负责什么 | 不负责什么 |
| --- | --- | --- |
| `core` | `Result`、受检运算、强类型、单调时间、UTF-8、契约检查 | 游戏、图像、页面或平台策略 |
| `domain` | 帧身份、坐标空间、目标代际、检测、观察租约、错误分类 | 识别算法和输入投递 |
| `vision` | Gray8 转换和有资源上限的 SAD 匹配 | PNG、页面规则和业务阈值 |
| `image` | PNG 编解码、像素布局转换和矩形裁剪 | 识别器和动作授权 |
| `annotation` | 标注模型、页面识别、证据、授权和确定性编译 | 捕获窗口和投递输入 |
| `engine` | 加载发布物、保持同帧决策、编排端口和记录追踪 | Win32、目标选择和 Luau 宿主 |
| `controller` | 窗口发现、目标连续性、WGC、DPI 和严格后台输入 | 页面识别和动作选择 |
| `script` | Luau 底座：VM、沙箱、配额、指令与时间预算、interrupt 取消，以及双环境拆分 | task policy——等待、重试、step 和 interrupt，它们住在 `modules/task/runtime/` 下的 Luau framework 里 |

`controller` 是唯一限定为 Windows 的可复用模块。`umbra-flow run` 的实际适配器
也只支持 Windows，但平台代码都留在 `entry/`，
不会反向进入 domain、vision、image、annotation 或 engine。Linux/macOS 因此仍可
构建平台无关模块，CI 也能用替身端口测试运行时流程。

## 三个二进制、四个入口

`umbra-flow` 是一个二进制、两个子命令；另外两个各只有一个入口。

| 入口 | 用途 | 当前状态 |
| --- | --- | --- |
| `umbra-authoring` | 在命令行上标注项目，外加测量帧 | 目前唯一的标注工具（2026-07-30） |
| `umbra-flow run` | 加载已发布项目，运行 `--task NAME` 指名的 Luau 任务 | P0 单任务 runner |
| `umbra-flow drive` | 加载同样的项目，执行 `--queue` 里操作者送来的 JSON 行命令 | P0 操作者前端（2026-07-30） |
| `umbra-input-agent` | 对一个裸窗口服务标注会话的命令队列 | 标注前端；2026-07-31 离开 `m0-demo` |
| `m0-demo` | 验证 WGC 捕获和严格后台输入 | 已冻结；固定循环加 `capture` 诊断，仅此而已 |

> 更正（2026-07-31）：这张表原来第一行是第五个入口 `umbra-workbench`，写着「A1 标注工具」。
> `b57b67b` 把它归档了，外壳留在 git 历史里。它的后端仍然被链接，只是改由
> `umbra-authoring` 链接，所以下面描述的标注能力没有跟着一起消失——但有三样只存在于
> GUI 里的东西确实消失了，它们记在
> [能力模型计划](../../plans/2026-07-31-annotation-model-capabilities.md) §四之二.1，
> 其中两样此后以 `umbra-authoring page reference` 的形式落地。

这些路径不能混用：

- `umbra-authoring` 可以生成识别资产，但它没有输入能力。
- `umbra-flow` 只读取生成后的运行时清单和模板，不读取完整的编辑截图。
- **`run` 与 `drive` 是同一张能力面上的两个前端，一个 generation 只接受其中一个。**
  `TaskHost` 在先到的那个前端上上闩，此后终身拒绝另一个。两者都够不到对方够不到的东西：
  操作者前端绑定的是受信任 Luau framework 绑定的那批私有原语，继承同样的拒绝。它是同级的
  第二个消费者，不是通往 Luau 的口子——没有 chunk、没有源码、没有任何字符串会变成代码。
- **`trace::FrontEnd` 有第三个值，而它不是那张能力面的第三个消费者**（2026-07-31）。
  `annotation` 就是 `umbra-input-agent`：标注会话为了量一个裸窗口而驱动它，没有项目、
  没有 generation、也没有能力面。把它写进同一个枚举，是因为「是谁驱动了这个目标」是一个问题、
  一套答案，而在此之前标注会话的点击与抓帧事后根本归不了属。它不写 `umbraflow-trace/v2` 的行
  ——该 schema 每一行都带 `runId` 与 `generationId`，而它两者皆无——所以它用同一个值、
  同一套拼写盖在自己的 results 文件上。
- `m0-demo` 没有接入 annotation 授权栈，也不能作为 engine 或 CLI 的共享实现。

`umbra-authoring` 是**开发工具，并且它自己不直接写任何东西**：每一次改动都经过
`annotation::AuthoringDocument`，因为标注产物就是点击授权的证据，那道校验不能被绕过。
它的子命令是 `project init|show|save`、`page create|add|reference`、
`match ROOT ELEMENT --frame PNG [--page PAGE]`（拿一张留出来的截图验证某个元素），
以及 `frames stability|probe|census`
（vision 的那三个测量原语）。每次调用不论成败都往 stdout 写一份 JSON 文档。`match` 才是重点：
标注、验证、迭代，整个回路里没有人。

> 更正（2026-07-31）：动词表原来写的是 `page create|add-anchor|add-target`。三个画像素的
> 动词收敛成一个——`page add ROOT PAGE NAME --capability C... <draw>`，`C` 是
> `identify[:required|:forbidden]`、`interact` 或 `read`，每个能力给一次——因为能力现在是
> 集合而不是三选一，于是「既认页又可点」的元素是一个元素、一个周期只匹配一次。`--shared`
> 随 `bool shared` 字段一起退掉。`page reference ROOT PAGE ELEMENT [--capability C...]
> [--search-roi x,y,w,h]` 是新增的：把项目已经持有的元素放到第二个页面，这个动词此前在
> CLI 上根本不存在。它的 `--capability` 用同一套 `C` 词汇，说的是**这一页**行使被借元素的
> 哪几种能力，于是第二个页面可以用 `identify:required` 或 `identify:forbidden` 把已有的
> 标记收进自己的签名；不给这个标志则继承 interact 与 read，identify 永远不继承。裁决出处：
> [能力模型计划](../../plans/2026-07-31-annotation-model-capabilities.md)。

失败文档用 `kind` 与 `response` 作答，两者都采用其他每一个 JSON 表面所用的**wire 拼写**
（`automationErrorWireName` 与 `failureResponseWireName`），所以是 `recognition_incomplete`
而不是 C++ 枚举名。`response` 的存在是为了让调用方不必解析消息就能分清「没跑完」与「硬失败」。
2026-07-30（`81ba61b`）之前 `kind` 用的是枚举名，而它旁边的 `response` 早就用 wire 名，
于是同一个 JSON 对象用两套约定作答，读了两个表面的 agent 会带着同一个 kind 的两种拼法。

## 从编辑项目到运行时

标注工具维护两类文档：

- `AuthoringDocument` 保存完整编辑信息，可以重新打开继续修改。schema 是
  `umbraflow-authoring/v4`。
- `RuntimeManifest` 只保留运行时识别和授权需要的数据。schema 是
  `umbraflow-annotations/v3`。

> 更正（2026-07-31）：三值 annotation type 变成能力集合时，两个 schema 在同一次原子改动里
> 一起升版，并且旧 id 都没有读路径——旧 schema 串会按普通的 unsupported-schema 报错。见
> [能力模型计划](../../plans/2026-07-31-annotation-model-capabilities.md) §三。

典型目录如下：

```text
project.toml
assets/sources/<content-hash>.png
annotations.toml
generated/annotations.runtime.toml
assets/templates/<content-hash>.png
```

`compileAuthoringDocument` 从源图裁出模板，规范编码 PNG，以编码后的字节计算
`ContentHash`，再生成运行时清单。标注工具发布时先写内容寻址资产，最后替换
`generated/annotations.runtime.toml`。运行时只信任该清单引用的资产，不扫描目录猜测
应该加载哪些文件。

当前发布过程不是跨文件事务。如果最后替换清单失败，磁盘上可能同时存在新的编辑文档
和旧的运行时闭包，但 loader 不会把两者拼成一个半新半旧的项目。

## 一次运行如何发生

Windows 产品路径的组合入口是 `entry/cli/run-windows.cpp`：

1. CLI 解析参数，加载运行时项目，并把页面名和动作名解析成稳定 ID。
2. 以上离线检查在访问桌面之前完成。清单损坏、模板缺失或名称错误不会先创建平台资源。
3. controller 选择唯一目标窗口，建立 `TargetGeneration` 和 WGC 捕获会话。
4. `WgcCaptureSession::capture` 返回带像素、捕获时间、坐标变换和身份的 `Frame`。
5. `EngineSession::observe` 创建 `Observation`。
   `EngineSession::resolvePage(observation)` 与
   `EngineSession::findAction(observation, pageId, elementId)` 始终使用该 observation
   持有的同一帧，不会在中间隐式重新截图。

   > 更正（2026-07-31）：`findAction` 原本收 `(observation, id)`。现在它要指名页面，因为
   > 每页的事实搬到了引用行上——细化的搜索区域和钉死的形态都属于「某一页怎么用这个元素」，
   > 而不行使 `interact` 的页面在那里根本没有动作可定位。它仍然不授权任何东西：页面参数
   > 挑的是一行引用，不是发一张许可。id 的类型是 `annotation::ElementId`，同一次改动之前
   > 叫 `RecognizerId`。
6. annotation 将页面解析为 `ResolvedPage`、`UnknownPage` 或 `AmbiguousPages`。
   只有唯一页面和完整识别结果可以继续。
7. `authorizeCoordinateAction` 同时检查页面权限、动作检测、观察租约、项目指纹和帧身份。
8. engine 把动作坐标转换到 client space，投递前再次确认目标实例，并把原始 lease
   交给 `IActionSink`。
9. controller 再检查 session、generation、租约年龄、坐标范围和 Win32 编码范围，
   最后通过 `PostMessageW` 投递；失败时不会降级为前台或全局输入。
10. engine 生成带版本的 `TraceEvent`，CLI 以 JSONL 逐条写入并刷新。

## 必须保持的约束

### 失败时不执行动作

以下情况都必须停止在授权或投递之前：

- 资源、schema、哈希、引用闭包或几何无效；
- 页面无法唯一确定；
- 识别被取消、超时或耗尽比较预算；
- 项目指纹、目标代际或帧身份不一致；
- 观察已经过期或使用过；
- 目标实例无法在投递前重新确认；
- 投递前要求的追踪记录无法按契约写入。

错误不能被转换成“尽量点击一次”，也不能通过前台化或全局输入兜底。

点击成功后的 `ClickDelivered` 和 `ObservationInvalidated` 追踪也可能写入失败。
这类失败不能撤销已经发生的点击；engine 会先使 observation 失效，再传播追踪错误，
因此调用方不能把返回失败理解为“零投递”并重试同一个动作。

### 同帧和身份隔离

帧身份由 `(CaptureSessionId, TargetGeneration, FrameId)` 组成：

- `FrameId` 在一次捕获会话中单调增加；
- `TargetGeneration` 在目标实例、窗口句柄、客户区尺寸或连续性变化时增加；
- `CaptureSessionId` 隔离不同捕获会话。

`Observation` 持有原始帧。页面证据、动作证据和租约都来自这同一帧。成功投递后，
observation 立即失效，防止重复点击。

### 确定性和资源上限

- 灰度转换、SAD、阈值和候选排序使用整数规则。
- 相似度阈值以 `[0, 10000]` 的基点保存，命中边界使用包含等号的整数比较。
- 匹配顺序、页面顺序、TOML 字段顺序和 JSON 字段顺序固定。
- 文件大小、图像尺寸、模板数量、搜索比较次数、等待时长和重试次数都有上限。
- 取消、超时和预算耗尽会保留明确的停止原因，不能伪装成普通 miss。

### 所有权和平台边界

`Frame` 共享只读像素所有权，`GrayImage` 等 view 只在 backing buffer 有效时使用。
`EngineSession` 独占三个端口，`Observation` 也不能跨 session 使用。observation
不 borrow session；私有共享 identity token 会随 session move，在没有 raw
back-pointer 的情况下维持该边界。平台 handle、D3D 对象和 Win32 输入实现留在
controller 或 `entry/` 的平台目录。

严格后台不是一个可选开关，而是可达 API 的限制。当前允许的输入路径最终落到目标窗口
的 `PostMessageW`——鼠标消息如此，2026-07-30 起按键消息也如此；
`SetForegroundWindow`、`SetFocus`、`SendInput`、`mouse_event`、
`keybd_event` 和 `SetCursorPos` 都在禁止名单中。按键和别的东西一样走 `PostMessageW`，
带同一份审计记录，down 与 up 之间不做任何保持。

按键的授权与点击有意不同：`IActionSink::click` 收 `ObservationLease`，
`IActionSink::pressKey` 收 `TargetGeneration`——lease 围栏的是坐标，而按键不指名坐标。
它仍然要求一个打开的观察周期并花掉它，因为投出去的按键与点击一样会改变屏幕。

## 去哪里找

| 想了解的问题 | 文档 |
| --- | --- |
| 基础错误、数值、所有权和时间能力 | [`module-core.md`](module-core.md) |
| 帧身份、坐标、检测和租约 | [`module-domain.md`](module-domain.md) |
| Gray8/SAD、PNG 和像素布局 | [`module-vision-image.md`](module-vision-image.md) |
| 标注文档、编译、页面识别和授权 | [`module-annotation.md`](module-annotation.md) |
| 运行时端口、Observation、动作和追踪 | [`module-engine.md`](module-engine.md) |
| Luau 底座：沙箱、预算、取消和两个环境 | [`module-script.md`](module-script.md) |
| WGC、目标连续性、DPI 和输入 | [`module-controller.md`](module-controller.md) |
| 标注工具的编辑、预览和发布流程 | [`entry-workbench.md`](entry-workbench.md) |
| 色键、模板 mask 与带 mask 的匹配器 | [`module-annotation.md`](module-annotation.md)、[`module-vision-image.md`](module-vision-image.md) |
| 产品命令行、操作者 `drive` 协议和 Windows 组合流程 | [`entry-cli.md`](entry-cli.md) |
| 对裸窗口服务标注会话的队列 | [`entry-input-agent.md`](entry-input-agent.md) |
| 冻结的真机验收链路 | [`entry-m0-demo.md`](entry-m0-demo.md) |

## 验证范围

平台无关测试覆盖坐标和身份、SAD、PNG、标注文档、页面解析、动作授权、运行时加载、
Observation 生命周期、预算、取消和追踪序列化。controller 测试覆盖目标代际、租约、
消息序列、坐标范围和禁止 API。

真实 WGC、窗口遮挡、最小化、DPI、UIPI 和焦点不变仍需要 Windows 真机验证。合成测试
通过不能代替这些证据。当前未完成项见 [`docs/TODO.md`](../../TODO.md)，后续产品阶段
见 [`docs/plans/2026-07-21-product-form-and-roadmap.md`](../../plans/2026-07-21-product-form-and-roadmap.md)。
