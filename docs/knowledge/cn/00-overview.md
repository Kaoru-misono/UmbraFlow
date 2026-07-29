# UmbraFlow 全系统架构总览

本文说明 2026-07-24 仓库中已经可以构建和运行的架构。模块依赖以
[`docs/ARCHITECTURE.md`](../../ARCHITECTURE.md) 为准；尚未完成的产品能力见
[`docs/plans/`](../../plans/README.md)。

## 系统做什么

UmbraFlow 根据视觉证据授权后台操作。平台无关模块负责识别和授权；Windows 代码
负责目标捕获与输入投递。二者只在 `entry/` 组合，因此识别策略不依赖 HWND，
controller 也不需要了解 page 或 recognizer。

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
entry/cli            -> engine + controller
entry/workbench      -> annotation + engine + controller + image
```

图中箭头表示左侧模块依赖右侧模块。`vision` 和 `image` 位于同一层，互不依赖。

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

`controller` 是唯一限定为 Windows 的可复用模块。`umbra-workbench` 和
`umbra-flow run` 的实际适配器也只支持 Windows，但平台代码都留在 `entry/`，
不会反向进入 domain、vision、image、annotation 或 engine。Linux/macOS 因此仍可
构建平台无关模块，CI 也能用替身端口测试运行时流程。

## 三个可执行入口

| 入口 | 用途 | 当前状态 |
| --- | --- | --- |
| `umbra-workbench` | 编辑标注项目、采集源图、预览、编译和发布 | A1 标注工具 |
| `umbra-flow run` | 加载已发布项目，运行 `--task NAME` 指名的 Luau 任务 | P0 单任务 runner |
| `m0-demo` | 验证 WGC 捕获和严格后台输入 | 已冻结，不再承载产品功能 |

三条路径不能混用：

- `Workbench` 可以生成识别资产，但没有输入能力。
- `umbra-flow run` 只读取生成后的运行时清单和模板，不读取完整的编辑截图。
- `m0-demo` 没有接入 annotation 授权栈，也不能作为 engine 或 CLI 的共享实现。

## 从编辑项目到运行时

Workbench 维护两类文档：

- `AuthoringDocument` 保存完整编辑信息，可以重新打开继续修改。
- `RuntimeManifest` 只保留运行时识别和授权需要的数据。

典型目录如下：

```text
project.toml
assets/sources/<content-hash>.png
annotations.toml
generated/annotations.runtime.toml
assets/templates/<content-hash>.png
```

`compileAuthoringDocument` 从源图裁出模板，规范编码 PNG，以编码后的字节计算
`ContentHash`，再生成运行时清单。Workbench 发布时先写内容寻址资产，最后替换
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
   `EngineSession::findAction(observation, id)` 始终使用该 observation 持有的同一帧，
   不会在中间隐式重新截图。
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
的 `PostMessageW`；`SetForegroundWindow`、`SetFocus`、`SendInput`、`mouse_event`、
`keybd_event` 和 `SetCursorPos` 都在禁止名单中。

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
| 产品命令行和 Windows 组合流程 | [`entry-cli.md`](entry-cli.md) |
| 冻结的真机验收链路 | [`entry-m0-demo.md`](entry-m0-demo.md) |

## 验证范围

平台无关测试覆盖坐标和身份、SAD、PNG、标注文档、页面解析、动作授权、运行时加载、
Observation 生命周期、预算、取消和追踪序列化。controller 测试覆盖目标代际、租约、
消息序列、坐标范围和禁止 API。

真实 WGC、窗口遮挡、最小化、DPI、UIPI 和焦点不变仍需要 Windows 真机验证。合成测试
通过不能代替这些证据。当前未完成项见 [`docs/TODO.md`](../../TODO.md)，后续产品阶段
见 [`docs/plans/2026-07-21-product-form-and-roadmap.md`](../../plans/2026-07-21-product-form-and-roadmap.md)。
